/**
 * @file src/streaming/session.cpp
 * @brief Implements the Xbox streaming session runtime.
 */
// class header include
#include "src/streaming/session.h"

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include "third-party/moonlight-common-c/src/Limelight.h"

#include <SDL.h>
#include <SDL_ttf.h>

// platform includes
#include <windows.h>  // NOSONAR(cpp:S3806) nxdk requires lowercase header names

#if defined(NXDK) && MOONLIGHT_INPUT_DIAGNOSTICS
  #include <hal/debug.h>
#endif

// local includes
#include "src/input/stream_exit.h"
#include "src/logging/logger.h"
#include "src/os.h"
#include "src/startup/memory_stats.h"
#include "src/streaming/ffmpeg_stream_backend.h"
#include "src/streaming/stats_overlay.h"
#include "src/streaming/stream_controls.h"

#ifdef NXDK
/**
 * @brief Fill an integer with cryptographically strong random bytes on nxdk.
 *
 * @param randomValue Output integer populated with secure random bits.
 * @return Zero on success, or a non-zero platform error code on failure.
 */
extern "C" int rand_s(unsigned int *randomValue);
#endif

/**
 * @brief Return the platform monotonic clock value in microseconds.
 *
 * @return Microseconds from the platform timer used by moonlight-common-c.
 */
extern "C" std::uint64_t PltGetMicroseconds(void);

namespace {

  constexpr Uint8 BACKGROUND_RED = 0x08;
  constexpr Uint8 BACKGROUND_GREEN = 0x0A;
  constexpr Uint8 BACKGROUND_BLUE = 0x10;
  constexpr Uint8 TEXT_RED = 0xF2;
  constexpr Uint8 TEXT_GREEN = 0xF5;
  constexpr Uint8 TEXT_BLUE = 0xF8;
  constexpr Uint8 ACCENT_RED = 0x00;
  constexpr Uint8 ACCENT_GREEN = 0xF3;
  constexpr Uint8 ACCENT_BLUE = 0xD4;
  constexpr Uint32 STREAM_FRAME_DELAY_MILLISECONDS = 16U;
  constexpr Uint32 STREAM_PRESENT_POLL_MILLISECONDS = 2U;
  constexpr Uint32 STREAM_END_STATS_POLL_MILLISECONDS = 50U;
  constexpr Uint32 STREAM_VIDEO_IDLE_IDR_MILLISECONDS = 1500U;
  constexpr Uint32 STREAM_VIDEO_IDLE_IDR_COOLDOWN_MILLISECONDS = 2000U;
  constexpr int MIN_STREAM_FPS = 15;
  constexpr int MAX_STREAM_FPS = 60;
  constexpr int MIN_STREAM_BITRATE_KBPS = 250;
  constexpr int MAX_STREAM_BITRATE_KBPS = 50000;
  constexpr int DEFAULT_PACKET_SIZE = 1024;
  constexpr std::size_t MAX_CONNECTION_PROTOCOL_MESSAGES = 24U;

  struct StreamUiResources {
    SDL_Renderer *renderer = nullptr;
    TTF_Font *titleFont = nullptr;
    TTF_Font *bodyFont = nullptr;
    TTF_Font *statisticsFont = nullptr;  ///< Font used only when rebuilding the live panel.
    bool statisticsEnabled = false;  ///< Performance Stats controls the live panel and end metrics.
    streaming::VideoStatisticsSampler statisticsSampler;  ///< One-second telemetry intervals.
    streaming::StreamControls controller;
    streaming::FfmpegStreamBackend mediaBackend {};
    bool ttfInitialized = false;
  };

  /**
   * @brief Retained connection-protocol messages protected by their own lock.
   */
  class StreamProtocolLogBuffer {
  public:
    /**
     * @brief Append one trimmed protocol message to the rolling buffer.
     *
     * @param message Protocol log message to retain.
     */
    void append(std::string message) {
      std::scoped_lock lock(mutex_);
      if (messages_.size() >= MAX_CONNECTION_PROTOCOL_MESSAGES) {
        messages_.pop_front();
      }
      messages_.push_back(std::move(message));
    }

    /**
     * @brief Return the most recent retained protocol message.
     *
     * @return Latest message, or an empty string when the buffer is empty.
     */
    [[nodiscard]] std::string latest() const {
      std::scoped_lock lock(mutex_);
      return messages_.empty() ? std::string {} : messages_.back();
    }

    /**
     * @brief Remove every retained protocol message.
     */
    void clear() {
      std::scoped_lock lock(mutex_);
      messages_.clear();
    }

  private:
    mutable std::mutex mutex_;  ///< Guards retained protocol messages.
    std::deque<std::string> messages_;  ///< Rolling protocol message buffer.
  };

  struct StreamConnectionState {
    std::atomic<int> currentStage = STAGE_NONE;
    std::atomic<int> failedStage = STAGE_NONE;
    std::atomic<int> failedCode = 0;
    std::atomic<int> terminationError = 0;
    std::atomic<int> startResult = -1;
    std::atomic<bool> startCompleted = false;
    std::atomic<bool> connectionStarted = false;
    std::atomic<bool> connectionTerminated = false;
    std::atomic<bool> poorConnection = false;
    std::atomic<bool> stopRequested = false;
    StreamProtocolLogBuffer protocolLog;  ///< Recent protocol messages reported by Moonlight callbacks.
  };

  struct StreamStartContext {
    StreamConnectionState *connectionState = nullptr;
    STREAM_CONFIGURATION streamConfiguration {};
    SERVER_INFORMATION serverInformation {};
    CONNECTION_LISTENER_CALLBACKS connectionCallbacks {};
    DECODER_RENDERER_CALLBACKS videoCallbacks {};
    AUDIO_RENDERER_CALLBACKS audioCallbacks {};
    streaming::FfmpegStreamBackend *mediaBackend = nullptr;
    std::string address;
    std::string reportedAppVersion;
    std::string appVersion;
    std::string gfeVersion;
    std::string rtspSessionUrl;
  };

  struct StreamStartAttemptContext {
    const app::HostRecord &host;
    const app::HostAppRecord &app;
    StreamStartContext &startContext;
    StreamConnectionState &connectionState;
    StreamUiResources &resources;
    std::string *statusMessage = nullptr;
  };

  struct ActiveStreamLoopContext {
    const app::SettingsState &settings;
    const app::HostRecord &host;
    const app::HostAppRecord &app;
    StreamStartContext &startContext;
    StreamConnectionState &connectionState;
    StreamUiResources &resources;
  };

  struct TextLineLayout {
    SDL_Color color {};
    int x = 0;
    int y = 0;
    int maxWidth = 0;
    int *drawnHeight = nullptr;
  };

  struct ResolvedStreamParameters {
    VIDEO_MODE videoMode {};
    int fps = app::DEFAULT_STREAM_FRAMERATE;
    int bitrateKbps = app::DEFAULT_STREAM_BITRATE_KBPS;
    int packetSize = DEFAULT_PACKET_SIZE;
    int streamingRemotely = STREAM_CFG_AUTO;
  };

  void assign_status_message(std::string *statusMessage, std::string_view message) {
    if (statusMessage != nullptr) {
      statusMessage->assign(message.data(), message.size());
    }
  }

  class ScopedThreadPriority {
  public:
    explicit ScopedThreadPriority(SDL_ThreadPriority priority) {
      active_ = SDL_SetThreadPriority(priority) == 0;
      if (!active_) {
        logging::debug("stream", std::string("SDL_SetThreadPriority failed for stream render thread: ") + SDL_GetError());
      }
    }

    ~ScopedThreadPriority() {
      if (active_) {
        SDL_SetThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
      }
    }

    ScopedThreadPriority(const ScopedThreadPriority &) = delete;
    ScopedThreadPriority &operator=(const ScopedThreadPriority &) = delete;

  private:
    bool active_ = false;
  };

  bool video_modes_match(const VIDEO_MODE &left, const VIDEO_MODE &right) {
    return left.width == right.width && left.height == right.height && left.bpp == right.bpp && left.refresh == right.refresh;
  }

  VIDEO_MODE select_stream_presentation_video_mode(const VIDEO_MODE &fallbackVideoMode, const VIDEO_MODE &streamVideoMode) {
    if (streamVideoMode.width > 0 && streamVideoMode.height > 0) {
      return streamVideoMode;
    }

    return fallbackVideoMode;
  }

  class ScopedStreamVideoMode {
  public:
    ScopedStreamVideoMode(SDL_Window *window, const VIDEO_MODE &originalVideoMode, const VIDEO_MODE &requestedVideoMode):
        window_(window),
        originalVideoMode_(originalVideoMode),
        activeVideoMode_(originalVideoMode) {
      if (requestedVideoMode.width <= 0 || requestedVideoMode.height <= 0 || video_modes_match(requestedVideoMode, originalVideoMode)) {
        return;
      }

#ifdef NXDK
      if (!XVideoSetMode(requestedVideoMode.width, requestedVideoMode.height, requestedVideoMode.bpp, requestedVideoMode.refresh)) {
        logging::warn(
          "stream",
          "Failed to switch Xbox video mode for stream presentation to " + std::to_string(requestedVideoMode.width) + "x" + std::to_string(requestedVideoMode.height) + " @ " +
            std::to_string(requestedVideoMode.refresh) + " Hz; using current output mode"
        );
        return;
      }

      activeVideoMode_ = requestedVideoMode;
      changed_ = true;
      if (window_ != nullptr) {
        SDL_SetWindowSize(window_, activeVideoMode_.width, activeVideoMode_.height);
      }
      logging::info("stream", "Switched Xbox video mode for stream presentation to " + std::to_string(activeVideoMode_.width) + "x" + std::to_string(activeVideoMode_.height));
#else
      (void) requestedVideoMode;
#endif
    }

    ~ScopedStreamVideoMode() {
#ifdef NXDK
      if (changed_) {
        XVideoSetMode(originalVideoMode_.width, originalVideoMode_.height, originalVideoMode_.bpp, originalVideoMode_.refresh);
        if (window_ != nullptr) {
          SDL_SetWindowSize(window_, originalVideoMode_.width, originalVideoMode_.height);
        }
      }
#endif
    }

    ScopedStreamVideoMode(const ScopedStreamVideoMode &) = delete;
    ScopedStreamVideoMode &operator=(const ScopedStreamVideoMode &) = delete;

    const VIDEO_MODE &active_video_mode() const {
      return activeVideoMode_;
    }

  private:
    SDL_Window *window_ = nullptr;
    VIDEO_MODE originalVideoMode_ {};
    VIDEO_MODE activeVideoMode_ {};
    bool changed_ = false;
  };

  /**
   * @brief Describe the active Moonlight network profile for logging.
   *
   * @param streamingRemotely Moonlight stream locality mode.
   * @return Human-readable profile label.
   */
  const char *describe_streaming_remotely_mode(int streamingRemotely) {
    switch (streamingRemotely) {
      case STREAM_CFG_LOCAL:
        return "local";
      case STREAM_CFG_REMOTE:
        return "remote";
      default:
        return "auto";
    }
  }

  /**
   * @brief Return the connection state currently receiving moonlight-common-c callbacks.
   *
   * @return Mutable callback connection-state slot.
   */
  StreamConnectionState *&active_connection_state() {
    static StreamConnectionState *connectionState = nullptr;
    return connectionState;
  }

  /**
   * @brief Remove trailing CR and LF characters from a log line.
   *
   * @param message Candidate log line.
   * @return Trimmed log line.
   */
  std::string trim_trailing_line_breaks(std::string message) {
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
      message.pop_back();
    }
    return message;
  }

  /**
   * @brief Return a printable fallback for optional log fields.
   *
   * @param value Candidate string value.
   * @return The original value, or `<none>` when empty.
   */
  std::string printable_log_value(std::string_view value) {
    return value.empty() ? std::string("<none>") : std::string(value);
  }

  /**
   * @brief Return whether a string begins with the requested ASCII prefix.
   *
   * @param text Candidate text.
   * @param prefix Expected prefix.
   * @return True when the prefix matches case-insensitively.
   */
  bool starts_with_ascii_case_insensitive(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
      return false;
    }

    for (std::size_t index = 0; index < prefix.size(); ++index) {
      const auto textCharacter = static_cast<unsigned char>(text[index]);
      const auto prefixCharacter = static_cast<unsigned char>(prefix[index]);
      if (std::tolower(textCharacter) != std::tolower(prefixCharacter)) {
        return false;
      }
    }

    return true;
  }

  bool contains_ascii_case_insensitive(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
      return true;
    }
    if (text.size() < needle.size()) {
      return false;
    }

    for (std::size_t offset = 0; offset <= text.size() - needle.size(); ++offset) {
      if (starts_with_ascii_case_insensitive(text.substr(offset), needle)) {
        return true;
      }
    }

    return false;
  }

  bool is_high_volume_connection_log(std::string_view text) {
    static constexpr std::array<std::string_view, 22> HIGH_VOLUME_PREFIXES {
      "Audio packet queue overflow",
      "Control message took over 10 ms",
      "Depacketizer detected corrupt frame",
      "Failed to decrypt audio packet",
      "Failed to decrypt video packet",
      "IDR frame request sent",
      "Input queue reached maximum size limit",
      "Invalidate reference frame request sent",
      "Invalid last payload length",
      "Leaving speculative RFI mode",
      "Network dropped ",
      "Next post-invalidation frame is:",
      "Received OOS audio data",
      "Recovered ",
      "Requesting IDR frame on behalf of DR",
      "Sending RFI request for unrecoverable frame",
      "Sending speculative RFI request",
      "Unable to recover audio data",
      "Unrecoverable frame ",
      "Video decode unit queue overflow",
      "Waiting for IDR frame",
      "Waiting for RFI frame",
    };

    return std::any_of(HIGH_VOLUME_PREFIXES.begin(), HIGH_VOLUME_PREFIXES.end(), [text](std::string_view prefix) {
      return starts_with_ascii_case_insensitive(text, prefix);
    });
  }

  logging::LogLevel connection_log_level(std::string_view message) {
    if (
      starts_with_ascii_case_insensitive(message, "WARNING:") ||
      starts_with_ascii_case_insensitive(message, "Failed") ||
      starts_with_ascii_case_insensitive(message, "Invalid") ||
      starts_with_ascii_case_insensitive(message, "No ") ||
      contains_ascii_case_insensitive(message, " failed")
    ) {
      return logging::LogLevel::warning;
    }

    return logging::LogLevel::debug;
  }

  /**
   * @brief Return whether the host metadata indicates Sunshine.
   *
   * @param gfeVersion Reported GFE or Sunshine version string.
   * @return True when the host looks like Sunshine.
   */
  bool is_sunshine_host_version(std::string_view gfeVersion) {
    return starts_with_ascii_case_insensitive(gfeVersion, "Sunshine");
  }

  /**
   * @brief Normalize the host appversion string for Sunshine protocol selection.
   *
   * moonlight-common-c uses the appversion quad to pick RTSP behavior and marks
   * Sunshine hosts by making the fourth component negative. Sunshine also tracks
   * the newer Gen 7 TCP RTSP flow, so older emulated appversion values are
   * normalized to the 7.1.431 generation before the connection starts.
   *
   * @param appVersion Reported appversion from the host launch response.
   * @param gfeVersion Reported GFE or Sunshine version string.
   * @return Appversion string to pass into moonlight-common-c.
   */
  std::string normalize_streaming_app_version(std::string_view appVersion, std::string_view gfeVersion) {
    if (!is_sunshine_host_version(gfeVersion)) {
      return std::string(appVersion);
    }

    int major = 7;
    int minor = 1;
    int patch = 431;
    int build = 0;
    if (const int parsedFields = std::sscanf(std::string(appVersion).c_str(), "%d.%d.%d.%d", &major, &minor, &patch, &build); parsedFields < 3) {
      return "7.1.431.-1";
    }

    if (major < 7 || (major == 7 && minor < 1) || (major == 7 && minor == 1 && patch < 431)) {
      major = 7;
      minor = 1;
      patch = 431;
    }

    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch) + ".-1";
  }

  /**
   * @brief Format one `moonlight-common-c` log callback line.
   *
   * @param format `printf`-style format string supplied by the library.
   * @param arguments Variadic argument list matching `format`.
   * @return Formatted log line.
   */
  std::string format_connection_log_message(const char *format, va_list arguments) {
    if (format == nullptr) {
      return {};
    }

    std::array<char, 1024> stackBuffer {};
    va_list argumentsCopy;
    va_copy(argumentsCopy, arguments);
    const int requiredLength = std::vsnprintf(stackBuffer.data(), stackBuffer.size(), format, argumentsCopy);  // NOSONAR(cpp:S5281) format is supplied by moonlight-common-c.
    va_end(argumentsCopy);
    if (requiredLength < 0) {
      return {};
    }
    if (static_cast<std::size_t>(requiredLength) < stackBuffer.size()) {
      return trim_trailing_line_breaks(std::string(stackBuffer.data(), static_cast<std::size_t>(requiredLength)));
    }

    std::string dynamicBuffer(static_cast<std::size_t>(requiredLength) + 1U, '\0');
    va_copy(argumentsCopy, arguments);
    std::vsnprintf(dynamicBuffer.data(), dynamicBuffer.size(), format, argumentsCopy);  // NOSONAR(cpp:S5281) format is supplied by moonlight-common-c.
    va_end(argumentsCopy);
    dynamicBuffer.resize(static_cast<std::size_t>(requiredLength));
    return trim_trailing_line_breaks(std::move(dynamicBuffer));
  }

  /**
   * @brief Append one connection-protocol log line to the retained rolling buffer.
   *
   * @param connectionState Connection state owning the retained protocol log buffer.
   * @param message Log line to retain.
   */
  void append_connection_protocol_message(StreamConnectionState *connectionState, std::string message) {
    if (connectionState == nullptr) {
      return;
    }

    message = trim_trailing_line_breaks(std::move(message));
    if (message.empty()) {
      return;
    }

    connectionState->protocolLog.append(std::move(message));
  }

  /**
   * @brief Return the most recent retained connection-protocol line.
   *
   * @param connectionState Connection state holding retained protocol logs.
   * @return Latest protocol message, or an empty string when none was recorded.
   */
  std::string latest_connection_protocol_message(const StreamConnectionState &connectionState) {
    return connectionState.protocolLog.latest();
  }

  /**
   * @brief Reset the mutable connection state before retrying stream startup.
   *
   * @param connectionState State object to reset.
   */
  void reset_connection_state(StreamConnectionState *connectionState) {
    if (connectionState == nullptr) {
      return;
    }

    connectionState->currentStage.store(STAGE_NONE);
    connectionState->failedStage.store(STAGE_NONE);
    connectionState->failedCode.store(0);
    connectionState->terminationError.store(0);
    connectionState->startResult.store(-1);
    connectionState->startCompleted.store(false);
    connectionState->connectionStarted.store(false);
    connectionState->connectionTerminated.store(false);
    connectionState->poorConnection.store(false);
    connectionState->stopRequested.store(false);
    connectionState->protocolLog.clear();
  }

  std::string build_font_path() {
    return std::string(DATA_PATH) + "assets" + PATH_SEP + "fonts" + PATH_SEP + "vegur-regular.ttf";
  }

  void close_controller(SDL_GameController *controller) {
    if (controller != nullptr) {
      SDL_GameControllerClose(controller);
    }
  }

  void close_stream_ui_resources(StreamUiResources *resources) {
    if (resources == nullptr) {
      return;
    }

    resources->mediaBackend.shutdown();
    {
      resources->controller.close();
    }
    if (resources->statisticsFont != nullptr) {
      TTF_CloseFont(resources->statisticsFont);
      resources->statisticsFont = nullptr;
    }
    if (resources->bodyFont != nullptr) {
      TTF_CloseFont(resources->bodyFont);
      resources->bodyFont = nullptr;
    }
    if (resources->titleFont != nullptr) {
      TTF_CloseFont(resources->titleFont);
      resources->titleFont = nullptr;
    }
    if (resources->renderer != nullptr) {
      SDL_DestroyRenderer(resources->renderer);
      resources->renderer = nullptr;
    }
    if (resources->ttfInitialized) {
      TTF_Quit();
      resources->ttfInitialized = false;
    }
  }

  /**
   * @brief Acquire the menu's controller without closing its USB input pipe, or select an available controller.
   *
   * @param preferredController Controller still retained by the shell, or nullptr.
   * @return Separately owned controller reference, or nullptr when no device could be opened.
   */
  SDL_GameController *open_primary_controller(SDL_GameController *preferredController) {
    if (preferredController != nullptr && SDL_GameControllerGetAttached(preferredController) != SDL_FALSE) {
      const SDL_JoystickID preferredId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(preferredController));
      for (int joystickIndex = 0; joystickIndex < SDL_NumJoysticks(); ++joystickIndex) {
        if (SDL_JoystickGetDeviceInstanceID(joystickIndex) == preferredId) {
          if (SDL_GameController *controller = SDL_GameControllerOpen(joystickIndex); controller != nullptr) {
            logging::info("input", "Retained the menu controller for streaming; USB input remains open");
            return controller;
          }
        }
      }
    }
    for (int joystickIndex = 0; joystickIndex < SDL_NumJoysticks(); ++joystickIndex) {
      if (!SDL_IsGameController(joystickIndex)) {
        continue;
      }

      SDL_GameController *controller = SDL_GameControllerOpen(joystickIndex);
      if (controller != nullptr) {
        char *mapping = SDL_GameControllerMapping(controller);
        logging::info("input", "Opened stream controller at device " + std::to_string(joystickIndex) + " of " + std::to_string(SDL_NumJoysticks()) + " | mapping=" + (mapping != nullptr ? std::string(mapping) : std::string("unavailable")));
        SDL_free(mapping);
        return controller;
      }
    }

    return nullptr;
  }

  /**
   * @brief Keep Xbox controller input active when a video-mode transition leaves SDL without keyboard focus.
   *
   * Xbox has one display window and no keyboard-focus event source to recover
   * focus. Local quit and gameplay input must not depend on SDL's desktop filter.
   */
  void enable_stream_controller_events() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  }

  bool initialize_stream_ui_resources(SDL_Window *window, const VIDEO_MODE &videoMode, SDL_GameController *preferredController, StreamUiResources *resources, std::string *errorMessage) {
    if (window == nullptr || resources == nullptr) {
      if (errorMessage != nullptr) {
        *errorMessage = "Streaming requires a valid SDL window";
      }
      return false;
    }

    enable_stream_controller_events();

    if (TTF_Init() != 0) {
      if (errorMessage != nullptr) {
        *errorMessage = std::string("TTF_Init failed for the streaming session: ") + TTF_GetError();
      }
      return false;
    }
    resources->ttfInitialized = true;

    resources->renderer = SDL_CreateRenderer(window, -1, 0);
    if (resources->renderer == nullptr) {
      if (errorMessage != nullptr) {
        *errorMessage = std::string("SDL_CreateRenderer failed for the streaming session: ") + SDL_GetError();
      }
      close_stream_ui_resources(resources);
      return false;
    }

    const std::string fontPath = build_font_path();
    resources->titleFont = TTF_OpenFont(fontPath.c_str(), std::max(22, videoMode.height / 18));
    resources->bodyFont = TTF_OpenFont(fontPath.c_str(), std::max(16, videoMode.height / 28));
    if (resources->titleFont == nullptr || resources->bodyFont == nullptr) {
      if (errorMessage != nullptr) {
        *errorMessage = std::string("Failed to load the streaming session font from ") + fontPath + ": " + TTF_GetError();
      }
      close_stream_ui_resources(resources);
      return false;
    }

    resources->controller.reset(open_primary_controller(preferredController));
    {
      resources->statisticsFont = TTF_OpenFont(fontPath.c_str(), std::max(12, videoMode.height / 45));
      if (resources->statisticsFont == nullptr) {
        resources->statisticsEnabled = false;
        logging::warn("stream", std::string("Unable to load the performance overlay font: ") + TTF_GetError());
      }
    }
    return true;
  }

  bool fill_random_bytes(unsigned char *buffer, std::size_t size, std::string *errorMessage) {
#ifdef NXDK
    if (buffer == nullptr) {
      if (errorMessage != nullptr) {
        *errorMessage = "Streaming could not allocate the random-input key buffer";
      }
      return false;
    }

    std::size_t offset = 0;
    while (offset < size) {
      unsigned int randomValue = 0;
      if (::rand_s(&randomValue) != 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "Failed to generate secure random bytes for the streaming input keys";
        }
        return false;
      }

      const std::size_t chunkSize = std::min(sizeof(randomValue), size - offset);
      std::memcpy(buffer + offset, &randomValue, chunkSize);
      offset += chunkSize;
    }
    return true;
#else
    (void) buffer;
    (void) size;
    if (errorMessage != nullptr) {
      *errorMessage = "The streaming runtime is only supported on the Xbox build";
    }
    return false;
#endif
  }

  std::string hex_encode(const unsigned char *data, std::size_t size) {
    static constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

    std::string output;
    output.resize(size * 2U);
    for (std::size_t index = 0; index < size; ++index) {
      output[index * 2U] = HEX_DIGITS[(data[index] >> 4U) & 0x0F];  // NOSONAR(cpp:S6022) hex encoding is byte-oriented by design.
      output[(index * 2U) + 1U] = HEX_DIGITS[data[index] & 0x0F];  // NOSONAR(cpp:S6022) hex encoding is byte-oriented by design.
    }
    return output;
  }

  int select_stream_width(const VIDEO_MODE &videoMode) {
    return videoMode.width > 0 ? std::max(320, videoMode.width) : 640;
  }

  int select_stream_height(const VIDEO_MODE &videoMode) {
    return videoMode.height > 0 ? std::max(240, videoMode.height) : 480;
  }

  int select_client_refresh_rate_x100(const VIDEO_MODE &videoMode) {
    return videoMode.refresh > 0 ? videoMode.refresh * 100 : 6000;
  }

  /**
   * @brief Resolve the effective stream resolution to request for the current stream.
   *
   * @param fallbackVideoMode Active shell output mode.
   * @param settings Current shell settings.
   * @return Preferred stream resolution, or fallbackVideoMode when no override exists.
   */
  VIDEO_MODE select_effective_stream_video_mode(const VIDEO_MODE &fallbackVideoMode, const app::SettingsState &settings) {
    return settings.preferredVideoModeSet ? settings.preferredVideoMode : fallbackVideoMode;
  }

  /**
   * @brief Clamp the preferred frame rate into the supported stream range.
   *
   * @param settings Current shell settings.
   * @return Effective frame rate for the next stream.
   */
  int select_stream_fps(const app::SettingsState &settings) {
    return std::clamp(settings.streamFramerate, MIN_STREAM_FPS, MAX_STREAM_FPS);
  }

  /**
   * @brief Clamp the preferred bitrate into a reasonable stream range.
   *
   * @param settings Current shell settings.
   * @return Effective bitrate for the next stream in kilobits per second.
   */
  int select_stream_bitrate_kbps(const app::SettingsState &settings) {
    return std::clamp(settings.streamBitrateKbps, MIN_STREAM_BITRATE_KBPS, MAX_STREAM_BITRATE_KBPS);
  }

  /**
   * @brief Resolve the effective stream parameters for the next session.
   *
   * @param fallbackVideoMode Active shell output mode.
   * @param settings Current shell settings.
   * @return Stream parameters requested through the settings UI.
   */
  ResolvedStreamParameters resolve_stream_parameters(const VIDEO_MODE &fallbackVideoMode, const app::SettingsState &settings) {
    ResolvedStreamParameters resolved {};
    resolved.videoMode = select_effective_stream_video_mode(fallbackVideoMode, settings);
    resolved.fps = select_stream_fps(settings);
    resolved.bitrateKbps = select_stream_bitrate_kbps(settings);

    return resolved;
  }

  void configure_stream_start_context(
    const network::StreamLaunchResult &launchResult,
    const std::array<unsigned char, 16> &remoteInputKey,
    const std::array<unsigned char, 16> &remoteInputIv,
    const VIDEO_MODE &outputVideoMode,
    const ResolvedStreamParameters &streamParameters,
    StreamStartContext *context
  ) {
    if (context == nullptr) {
      return;
    }

    context->address = launchResult.serverInfo.activeAddress;
    if (context->address.empty()) {
      context->address = launchResult.serverInfo.localAddress;
    }
    if (context->address.empty()) {
      context->address = launchResult.serverInfo.remoteAddress;
    }
    context->reportedAppVersion = launchResult.appVersion;
    context->appVersion = normalize_streaming_app_version(launchResult.appVersion, launchResult.gfeVersion);
    context->gfeVersion = launchResult.gfeVersion;
    context->rtspSessionUrl = launchResult.rtspSessionUrl;

    LiInitializeServerInformation(&context->serverInformation);
    context->serverInformation.address = context->address.c_str();
    context->serverInformation.serverInfoAppVersion = context->appVersion.c_str();
    context->serverInformation.serverInfoGfeVersion = context->gfeVersion.empty() ? nullptr : context->gfeVersion.c_str();
    context->serverInformation.rtspSessionUrl = context->rtspSessionUrl.empty() ? nullptr : context->rtspSessionUrl.c_str();
    context->serverInformation.serverCodecModeSupport = launchResult.serverCodecModeSupport;

    LiInitializeStreamConfiguration(&context->streamConfiguration);
    context->streamConfiguration.width = select_stream_width(streamParameters.videoMode);
    context->streamConfiguration.height = select_stream_height(streamParameters.videoMode);
    context->streamConfiguration.fps = streamParameters.fps;
    context->streamConfiguration.bitrate = streamParameters.bitrateKbps;
    context->streamConfiguration.packetSize = streamParameters.packetSize;
    context->streamConfiguration.streamingRemotely = streamParameters.streamingRemotely;
    context->streamConfiguration.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    context->streamConfiguration.supportedVideoFormats = VIDEO_FORMAT_H264;
    context->streamConfiguration.clientRefreshRateX100 = select_client_refresh_rate_x100(outputVideoMode);
    context->streamConfiguration.colorSpace = COLORSPACE_REC_601;
    context->streamConfiguration.colorRange = COLOR_RANGE_LIMITED;
    context->streamConfiguration.encryptionFlags = ENCFLG_NONE;
    std::memcpy(context->streamConfiguration.remoteInputAesKey, remoteInputKey.data(), remoteInputKey.size());
    std::memcpy(context->streamConfiguration.remoteInputAesIv, remoteInputIv.data(), remoteInputIv.size());

    LiInitializeConnectionCallbacks(&context->connectionCallbacks);
  }

  void on_stage_starting(int stage) {
    if (StreamConnectionState *connectionState = active_connection_state(); connectionState != nullptr) {
      connectionState->currentStage.store(stage);
    }
    logging::debug("stream", std::string("Starting connection stage: ") + LiGetStageName(stage));
  }

  void on_stage_complete(int stage) {
    if (StreamConnectionState *connectionState = active_connection_state(); connectionState != nullptr) {
      connectionState->currentStage.store(stage);
    }
    logging::debug("stream", std::string("Completed connection stage: ") + LiGetStageName(stage));
  }

  void on_stage_failed(int stage, int errorCode) {
    if (StreamConnectionState *connectionState = active_connection_state(); connectionState != nullptr) {
      connectionState->failedStage.store(stage);
      connectionState->failedCode.store(errorCode);
    }
    logging::warn("stream", std::string("Connection stage failed: ") + LiGetStageName(stage) + " (error " + std::to_string(errorCode) + ")");
  }

  void on_connection_started() {
    if (StreamConnectionState *connectionState = active_connection_state(); connectionState != nullptr) {
      connectionState->connectionStarted.store(true);
    }
    logging::info("stream", "Streaming transport started");
  }

  void on_connection_terminated(int errorCode) {
    if (StreamConnectionState *connectionState = active_connection_state(); connectionState != nullptr) {
      connectionState->terminationError.store(errorCode);
      connectionState->connectionTerminated.store(true);
    }
    logging::warn("stream", std::string("Streaming transport terminated with error ") + std::to_string(errorCode));
  }

  void on_connection_status_update(int connectionStatus) {
    if (StreamConnectionState *connectionState = active_connection_state(); connectionState != nullptr) {
      const bool poorConnection = connectionStatus != CONN_STATUS_OKAY;
      if (connectionState->poorConnection.exchange(poorConnection) != poorConnection) {
        logging::warn("stream", poorConnection ? "Streaming transport reported poor network conditions" : "Streaming transport recovered to okay network conditions");
      }
    }
  }

  void on_log_message(const char *format, ...) {  // NOSONAR(cpp:S923) moonlight-common-c log callback requires printf-style ellipsis.
    if (format == nullptr || is_high_volume_connection_log(format)) {
      return;
    }

    va_list arguments;
    va_start(arguments, format);
    const std::string message = format_connection_log_message(format, arguments);
    va_end(arguments);
    if (message.empty() || is_high_volume_connection_log(message)) {
      return;
    }

    append_connection_protocol_message(active_connection_state(), message);
    logging::log(connection_log_level(message), "moonlight", message);
  }

  int run_stream_start_thread(void *context) {
    auto *startContext = static_cast<StreamStartContext *>(context);
    if (startContext == nullptr || startContext->connectionState == nullptr) {
      return -1;
    }

    active_connection_state() = startContext->connectionState;
    startContext->connectionState->startResult.store(
      LiStartConnection(
        &startContext->serverInformation,
        &startContext->streamConfiguration,
        &startContext->connectionCallbacks,
        &startContext->videoCallbacks,
        &startContext->audioCallbacks,
        startContext->mediaBackend,
        0,
        startContext->mediaBackend,
        0
      )
    );
    startContext->connectionState->startCompleted.store(true);
    return 0;
  }

  bool render_text_line(SDL_Renderer *renderer, TTF_Font *font, const std::string &text, const TextLineLayout &layout) {
    if (renderer == nullptr || font == nullptr || layout.maxWidth <= 0) {
      if (layout.drawnHeight != nullptr) {
        *layout.drawnHeight = 0;
      }
      return false;
    }

    SDL_Surface *surface = TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), layout.color, static_cast<Uint32>(layout.maxWidth));
    if (surface == nullptr) {
      if (layout.drawnHeight != nullptr) {
        *layout.drawnHeight = 0;
      }
      return false;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture == nullptr) {
      SDL_FreeSurface(surface);
      if (layout.drawnHeight != nullptr) {
        *layout.drawnHeight = 0;
      }
      return false;
    }

    const SDL_Rect destination {layout.x, layout.y, surface->w, surface->h};
    const int surfaceHeight = surface->h;
    SDL_FreeSurface(surface);
    const bool rendered = SDL_RenderCopy(renderer, texture, nullptr, &destination) == 0;
    SDL_DestroyTexture(texture);
    if (layout.drawnHeight != nullptr) {
      *layout.drawnHeight = surfaceHeight;
    }
    return rendered;
  }

  void render_text_lines(SDL_Renderer *renderer, TTF_Font *font, const std::vector<std::string> &lines, int screenWidth, int *cursorY) {
    if (cursorY == nullptr) {
      return;
    }

    for (const std::string &line : lines) {
      int drawnHeight = 0;
      render_text_line(renderer, font, line, {{TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, 28, *cursorY, std::max(1, screenWidth - 56), &drawnHeight});
      *cursorY += drawnHeight + 6;
    }
  }

  std::string build_stage_status_line(const StreamConnectionState &connectionState) {
    if (const int failedStage = connectionState.failedStage.load(); failedStage != STAGE_NONE) {
      return std::string("Connection failed during ") + LiGetStageName(failedStage) + " (error " + std::to_string(connectionState.failedCode.load()) + ")";
    }
    if (connectionState.connectionTerminated.load()) {
      const int terminationError = connectionState.terminationError.load();
      return terminationError == ML_ERROR_GRACEFUL_TERMINATION ? "The host ended the stream gracefully." : "The host ended the stream (error " + std::to_string(terminationError) + ")";
    }
    if (connectionState.connectionStarted.load()) {
      return std::string("Streaming transport is active. ") + input::STREAM_EXIT_HINT;
    }
    return std::string("Connecting: ") + LiGetStageName(connectionState.currentStage.load());
  }

  void open_controller_if_needed(StreamUiResources *resources, int deviceIndex) {
    if (resources == nullptr) {
      return;
    }

    resources->controller.open_if_needed(deviceIndex);
  }

  void close_controller_if_removed(StreamUiResources *resources, int joystickInstanceId) {
    if (resources == nullptr) {
      return;
    }

    resources->controller.close_if_removed(joystickInstanceId);
  }

  SDL_ControllerDeviceEvent copy_controller_device_event(const SDL_Event &event) {
    SDL_ControllerDeviceEvent controllerEvent {};
    std::memcpy(&controllerEvent, &event, sizeof(controllerEvent));
    return controllerEvent;
  }

  /**
   * @brief Request a local stop once, independently of the input sender and its network state.
   *
   * @param connectionState Session state receiving the local quit request.
   */
  void request_stream_stop(StreamConnectionState *connectionState) {
    if (connectionState != nullptr && !connectionState->stopRequested.exchange(true)) {
      logging::info("input", "Local stream stop confirmed");
    }
  }

  /**
   * @brief Route stream input and local controls on the SDL event thread.
   * @param resources Input owner, fonts and video backend.
   * @param connectionState Transport readiness and stop request.
   */
  void pump_stream_events(StreamUiResources *resources, StreamConnectionState *connectionState) {
    int width = 0;
    int height = 0;
    SDL_GetRendererOutputSize(resources->renderer, &width, &height);
    const bool wasVisible = resources->controller.has_overlay();
    if (resources->controller.pump(resources->mediaBackend, resources->bodyFont, width, height, connectionState->connectionStarted.load(), resources->statisticsEnabled)) {
      request_stream_stop(connectionState);
    }
    if (wasVisible && !resources->controller.has_overlay()) {
      resources->statisticsSampler = {};
    }
  }

  /**
   * @brief Capture transport and decoder statistics using caller-owned RTP video counters.
   *
   * @param context Active stream configuration and host connection details.
   * @param connectionState Mutable connection state tracked by the session loop.
   * @param mediaBackend FFmpeg backend that owns the active decoder.
   * @return Snapshot ready for rendering in diagnostics or end-of-stream summary.
   */
  streaming::StreamStatisticsSnapshot sample_stream_statistics(
    const StreamStartContext &context,
    const StreamConnectionState &connectionState,
    const streaming::FfmpegStreamBackend &mediaBackend
  ) {
    streaming::StreamStatisticsSnapshot snapshot {
      context.streamConfiguration.width,
      context.streamConfiguration.height,
      context.streamConfiguration.fps,
      -1,
      -1,
      -1,
      -1,
      -1,
      -1,
      -1,
      -1,
      connectionState.poorConnection.load(),
      mediaBackend.active_video_decoder_name(),
    };

    uint32_t estimatedRtt = 0;
    if (uint32_t estimatedRttVariance = 0; LiGetEstimatedRttInfo(&estimatedRtt, &estimatedRttVariance)) {
      snapshot.roundTripTimeMs = static_cast<int>(estimatedRtt);
      snapshot.roundTripVarianceMs = static_cast<int>(estimatedRttVariance);
    }

    snapshot.videoQueueDepth = LiGetPendingVideoFrames();
    snapshot.audioQueueDurationMs = LiGetPendingAudioDuration();
    RTP_VIDEO_STATS videoStats {};
    LiGetRTPVideoStats(&videoStats);
    snapshot.videoPacketsReceived = static_cast<int>(videoStats.packetCountVideo);
    snapshot.videoPacketsRecovered = static_cast<int>(videoStats.packetCountFecRecovered);
    snapshot.videoPacketsLost = static_cast<int>(videoStats.packetCountFecFailed);
    snapshot.presentationPath = mediaBackend.presentation_path();
    snapshot.decoderFeatures = mediaBackend.acceleration_report().cpuFeatures;
    return snapshot;
  }

  /**
   * @brief Sample telemetry and rasterize one shared panel at most once per second.
   * @param context Active stream configuration.
   * @param connectionState Current connection quality.
   * @param resources Renderer, cached font, telemetry sampler and video backend.
   * @return True when the video must be refreshed to display a new panel.
   */
  bool update_stream_statistics_overlay(const StreamStartContext &context, const StreamConnectionState &connectionState, StreamUiResources *resources) {
    if (resources == nullptr || resources->controller.has_overlay() || !resources->statisticsEnabled || resources->statisticsFont == nullptr || !resources->mediaBackend.has_decoded_video()) {
      return false;
    }
    const auto now = PltGetMicroseconds();
    if (!resources->statisticsSampler.due(now)) {
      return false;
    }
    auto snapshot = sample_stream_statistics(context, connectionState, resources->mediaBackend);
    resources->statisticsSampler.sample(now, resources->mediaBackend.performance_counters());
    resources->statisticsSampler.apply(&snapshot);
    const auto lines = streaming::build_live_stats_overlay_lines(snapshot);
    int screenWidth = 0;
    int screenHeight = 0;
    SDL_GetRendererOutputSize(resources->renderer, &screenWidth, &screenHeight);
    constexpr int padding = 6;
    const int lineHeight = TTF_FontLineSkip(resources->statisticsFont) + 2;
    int width = 0;
    for (const auto &line : lines) {
      int lineWidth = 0;
      TTF_SizeUTF8(resources->statisticsFont, line.c_str(), &lineWidth, nullptr);
      width = std::max(width, lineWidth);
    }
    width = std::min(width + padding * 2, screenWidth - streaming::VideoOverlay::MARGIN * 2);
    const int height = std::min(static_cast<int>(lines.size()) * lineHeight + padding * 2, screenHeight - streaming::VideoOverlay::MARGIN * 2);
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> panel(SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888), SDL_FreeSurface);
    if (!panel) {
      logging::warn("stream", std::string("Unable to allocate the performance overlay: ") + SDL_GetError());
      resources->statisticsEnabled = false;
      return false;
    }
    SDL_FillRect(panel.get(), nullptr, SDL_MapRGB(panel->format, 16, 16, 16));
    int top = padding;
    for (const auto &line : lines) {
      std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> text(TTF_RenderUTF8_Blended(resources->statisticsFont, line.c_str(), {255, 255, 255, 255}), SDL_FreeSurface);
      if (text) {
        SDL_Rect position {padding, top, text->w, text->h};
        SDL_BlitSurface(text.get(), nullptr, panel.get(), &position);
      }
      top += lineHeight;
    }
    std::vector<std::uint8_t> grayscale(static_cast<std::size_t>(width) * height);
    for (int row = 0; row < height; ++row) {
      for (int column = 0; column < width; ++column) {
        grayscale[static_cast<std::size_t>(row) * width + column] = static_cast<const std::uint8_t *>(panel->pixels)[static_cast<std::size_t>(row) * panel->pitch + column * 4];
      }
    }
    resources->mediaBackend.set_statistics_bitmap(width, height, std::move(grayscale));
    return true;
  }

  void request_idle_video_refresh_if_needed(const streaming::FfmpegStreamBackend *mediaBackend, Uint32 *lastRequestTicks) {
    if (mediaBackend == nullptr || lastRequestTicks == nullptr || !mediaBackend->has_decoded_video()) {
      return;
    }

    const std::uint64_t idleMilliseconds = mediaBackend->milliseconds_since_last_decoded_video_frame(PltGetMicroseconds());
    if (idleMilliseconds < STREAM_VIDEO_IDLE_IDR_MILLISECONDS) {
      return;
    }
    const Uint32 nowTicks = SDL_GetTicks();
    if (*lastRequestTicks != 0U && nowTicks - *lastRequestTicks < STREAM_VIDEO_IDLE_IDR_COOLDOWN_MILLISECONDS) {
      return;
    }

    LiRequestIdrFrame();
    *lastRequestTicks = nowTicks;
    logging::debug("stream", std::string("Requested IDR frame after ") + std::to_string(idleMilliseconds) + " ms without decoded video");
  }

  /**
   * @brief Draw opt-in input diagnostics on the final Xbox framebuffer after video presentation.
   *
   * @param resources Event-thread controller owner supplying the diagnostic state.
   */
  void render_stream_input_diagnostics(const StreamUiResources *resources) {
#if defined(NXDK) && MOONLIGHT_INPUT_DIAGNOSTICS
    if (resources != nullptr) {
      debugResetCursor();
      for (const auto &line : resources->controller.diagnostic_lines()) {
        debugPrint("%s\n", line.c_str());
      }
    }
#else
    (void) resources;
#endif
  }

  bool render_stream_frame(
    const app::HostRecord &host,
    const app::HostAppRecord &app,
    const StreamStartContext &context,
    const StreamConnectionState &connectionState,
    streaming::FfmpegStreamBackend *mediaBackend,
    StreamUiResources *resources
  ) {
    if (resources == nullptr || resources->renderer == nullptr || resources->titleFont == nullptr || resources->bodyFont == nullptr) {
      return false;
    }
    update_stream_statistics_overlay(context, connectionState, resources);

    int screenWidth = 0;
    int screenHeight = 0;
    SDL_GetRendererOutputSize(resources->renderer, &screenWidth, &screenHeight);

    const bool hasDecodedVideo = mediaBackend != nullptr && mediaBackend->has_decoded_video();
#ifdef NXDK
    if (hasDecodedVideo && mediaBackend->render_latest_video_frame(resources->renderer, screenWidth, screenHeight, true)) {
      render_stream_input_diagnostics(resources);
      return true;
    }
#endif
    if (hasDecodedVideo) {
      SDL_SetRenderDrawColor(resources->renderer, 0x00, 0x00, 0x00, 0xFF);
    } else {
      SDL_SetRenderDrawColor(resources->renderer, BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0xFF);
    }
    SDL_RenderClear(resources->renderer);

    const bool renderedVideo = hasDecodedVideo && mediaBackend->render_latest_video_frame(resources->renderer, screenWidth, screenHeight, false);
    if (renderedVideo) {
      SDL_RenderPresent(resources->renderer);
      render_stream_input_diagnostics(resources);
      return true;
    }

    int cursorY = 28;
    int titleHeight = 0;
    render_text_line(resources->renderer, resources->titleFont, "Moonlight Streaming", {{ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF}, 28, cursorY, std::max(1, screenWidth - 56), &titleHeight});
    cursorY += titleHeight + 8;

    std::vector<std::string> lines = {
      std::string("Host: ") + host.displayName + " (" + context.address + ")",
      std::string("App: ") + app.name + (context.rtspSessionUrl.empty() ? std::string {} : std::string(" | Session URL received")),
      build_stage_status_line(connectionState),
      mediaBackend != nullptr ? mediaBackend->build_overlay_status_line() : std::string("FFmpeg decode backend unavailable"),
      input::STREAM_EXIT_HINT,
    };
    if (!renderedVideo) {
      lines.emplace(lines.begin() + 2, std::string("Mode: ") + std::to_string(context.streamConfiguration.width) + "x" + std::to_string(context.streamConfiguration.height) + " @ " + std::to_string(context.streamConfiguration.fps) + " FPS | " + (mediaBackend != nullptr ? mediaBackend->active_video_decoder_name() : std::string("video")) + " | Stereo");
      lines.emplace(lines.begin() + 4, std::string("Launch mode: ") + (context.serverInformation.rtspSessionUrl != nullptr ? "Session URL supplied by host" : "Default RTSP discovery"));
      lines.emplace(lines.begin() + 5, "Waiting for the first decoded video frame and audio output.");
    }

    render_text_lines(resources->renderer, resources->bodyFont, lines, screenWidth, &cursorY);

    if (resources->controller.has_overlay() && mediaBackend != nullptr) {
      mediaBackend->render_local_overlay(resources->renderer, screenWidth, screenHeight);
    }
    SDL_RenderPresent(resources->renderer);
    render_stream_input_diagnostics(resources);
    return true;
  }

  bool render_stream_end_statistics_frame(
    const app::HostRecord &host,
    const app::HostAppRecord &app,
    const StreamStartContext &context,
    const streaming::StreamStatisticsSnapshot &statisticsSnapshot,
    const std::string &finalMessage,
    StreamUiResources *resources
  ) {
    if (resources == nullptr || resources->renderer == nullptr || resources->titleFont == nullptr || resources->bodyFont == nullptr) {
      return false;
    }

    int screenWidth = 0;
    SDL_GetRendererOutputSize(resources->renderer, &screenWidth, nullptr);

    SDL_SetRenderDrawColor(resources->renderer, BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0xFF);
    SDL_RenderClear(resources->renderer);

    int cursorY = 28;
    int titleHeight = 0;
    const bool fallbackUsed = !statisticsSnapshot.accelerationSummary.empty() && statisticsSnapshot.accelerationSummary.front() == "SOFTWARE FALLBACK USED";
    const SDL_Color summaryColor = fallbackUsed ? SDL_Color {0xFF, 0xB4, 0x46, 0xFF} : SDL_Color {ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF};
    render_text_line(resources->renderer, resources->titleFont, fallbackUsed ? "SOFTWARE FALLBACK USED" : "Stream Summary", {summaryColor, 28, cursorY, std::max(1, screenWidth - 56), &titleHeight});
    cursorY += titleHeight + 8;

    std::vector<std::string> lines = statisticsSnapshot.accelerationSummary;
    lines.push_back(finalMessage);
    lines.push_back(std::string("Host: ") + host.displayName + " (" + context.address + ")");
    lines.push_back(std::string("App: ") + app.name);
    if (statisticsSnapshot.showPerformanceMetrics) {
      for (const std::string &line : streaming::build_stats_overlay_lines(statisticsSnapshot)) {
        lines.push_back(line);
      }
    }
    lines.emplace_back("Press any button to return.");

    render_text_lines(resources->renderer, resources->bodyFont, lines, screenWidth, &cursorY);

    SDL_RenderPresent(resources->renderer);
    return true;
  }

  bool should_close_stream_end_statistics(const SDL_Event &event, StreamUiResources *resources) {
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
      const SDL_ControllerDeviceEvent controllerEvent = copy_controller_device_event(event);
      open_controller_if_needed(resources, controllerEvent.which);
      return false;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
      const SDL_ControllerDeviceEvent controllerEvent = copy_controller_device_event(event);
      close_controller_if_removed(resources, controllerEvent.which);
      return false;
    }
    return event.type == SDL_QUIT || event.type == SDL_KEYDOWN || event.type == SDL_MOUSEBUTTONDOWN;
  }

  void show_stream_end_statistics(
    const app::HostRecord &host,
    const app::HostAppRecord &app,
    const StreamStartContext &context,
    const streaming::StreamStatisticsSnapshot &statisticsSnapshot,
    const std::string &finalMessage,
    StreamUiResources *resources
  ) {
    if (!render_stream_end_statistics_frame(host, app, context, statisticsSnapshot, finalMessage, resources)) {
      return;
    }

    resources->controller.poll();
    bool controllerReadyForPress = !resources->controller.any_button_pressed();

    for (;;) {
      SDL_Event event {};
      while (SDL_PollEvent(&event) != 0) {
        if (should_close_stream_end_statistics(event, resources)) {
          return;
        }
      }
      resources->controller.poll();
      if (!resources->controller.any_button_pressed()) {
        controllerReadyForPress = true;
      } else if (controllerReadyForPress) {
        return;
      }
      SDL_Delay(STREAM_END_STATS_POLL_MILLISECONDS);
    }
  }

  /**
   * @brief Acknowledge a local quit before waiting for the input sender and transport teardown.
   *
   * @param resources Stream renderer and font resources owned by the event thread.
   */
  void render_stream_stopping_frame(StreamUiResources *resources) {
    if (resources == nullptr || resources->renderer == nullptr || resources->titleFont == nullptr) {
      return;
    }
    resources->mediaBackend.stop_video_presentation();
    int screenWidth = 0;
    SDL_GetRendererOutputSize(resources->renderer, &screenWidth, nullptr);
    SDL_SetRenderDrawColor(resources->renderer, BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0xFF);
    SDL_RenderClear(resources->renderer);
    render_text_line(resources->renderer, resources->titleFont, "Stopping stream...", {{ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF}, 28, 28, std::max(1, screenWidth - 56), nullptr});
    SDL_RenderPresent(resources->renderer);
  }

  bool run_stream_start_attempt(const StreamStartAttemptContext &attempt) {
    if (SDL_Thread *startThread = SDL_CreateThread(run_stream_start_thread, "start-stream", &attempt.startContext); startThread != nullptr) {
      while (!attempt.connectionState.startCompleted.load() && !attempt.connectionState.stopRequested.load()) {
        pump_stream_events(&attempt.resources, &attempt.connectionState);
        if (attempt.connectionState.stopRequested.load()) {
          render_stream_stopping_frame(&attempt.resources);
          LiInterruptConnection();
        } else {
          render_stream_frame(attempt.host, attempt.app, attempt.startContext, attempt.connectionState, &attempt.resources.mediaBackend, &attempt.resources);
        }
        SDL_Delay(STREAM_FRAME_DELAY_MILLISECONDS);
      }

      int threadResult = 0;
      SDL_WaitThread(startThread, &threadResult);
      (void) threadResult;
      return true;
    }

    const std::string createThreadError = std::string("Failed to start the streaming transport thread: ") + SDL_GetError();
    close_stream_ui_resources(&attempt.resources);
    assign_status_message(attempt.statusMessage, createThreadError);
    logging::error("stream", createThreadError);
    return false;
  }

  bool should_retry_stream_without_rtsp_session_url(const StreamStartContext &startContext, const StreamConnectionState &connectionState) {
    return !connectionState.stopRequested.load() && !connectionState.connectionStarted.load() && connectionState.failedStage.load() == STAGE_RTSP_HANDSHAKE && !startContext.rtspSessionUrl.empty();
  }

  bool run_stream_start_with_rtsp_fallback(const StreamStartAttemptContext &attempt, bool *rtspFallbackAttempted) {
    if (rtspFallbackAttempted != nullptr) {
      *rtspFallbackAttempted = false;
    }
    if (!run_stream_start_attempt(attempt)) {
      return false;
    }
    if (!should_retry_stream_without_rtsp_session_url(attempt.startContext, attempt.connectionState)) {
      return true;
    }

    if (rtspFallbackAttempted != nullptr) {
      *rtspFallbackAttempted = true;
    }
    logging::warn("stream", "RTSP handshake failed with the host-supplied session URL; retrying with default RTSP discovery");
    attempt.resources.mediaBackend.shutdown();
    attempt.startContext.rtspSessionUrl.clear();
    attempt.startContext.serverInformation.rtspSessionUrl = nullptr;
    reset_connection_state(&attempt.connectionState);
    return run_stream_start_attempt(attempt);
  }

  void render_active_stream_frame_if_needed(const ActiveStreamLoopContext &loopContext, Uint32 *lastIdleVideoIdrRequestTick) {
    const bool hasDecodedVideo = loopContext.resources.mediaBackend.has_decoded_video();
    const bool statisticsChanged = update_stream_statistics_overlay(loopContext.startContext, loopContext.connectionState, &loopContext.resources);
    if (const bool shouldRenderFrame = !hasDecodedVideo || statisticsChanged || loopContext.resources.mediaBackend.has_pending_overlay() || loopContext.resources.mediaBackend.has_unrendered_video_frame(); shouldRenderFrame) {
      render_stream_frame(
        loopContext.host,
        loopContext.app,
        loopContext.startContext,
        loopContext.connectionState,
        &loopContext.resources.mediaBackend,
        &loopContext.resources
      );
    }
    request_idle_video_refresh_if_needed(&loopContext.resources.mediaBackend, lastIdleVideoIdrRequestTick);
    SDL_Delay(hasDecodedVideo ? STREAM_PRESENT_POLL_MILLISECONDS : STREAM_FRAME_DELAY_MILLISECONDS);
  }

  /**
   * @brief Pump input and render until transport termination or confirmed local quit.
   * @param loopContext Active stream resources.
   */
  void run_active_stream_loop(const ActiveStreamLoopContext &loopContext) {
    Uint32 lastIdleVideoIdrRequestTick = 0U;
    while (!loopContext.connectionState.connectionTerminated.load() && !loopContext.connectionState.stopRequested.load()) {
      pump_stream_events(&loopContext.resources, &loopContext.connectionState);
      if (loopContext.connectionState.stopRequested.load()) {
        render_stream_stopping_frame(&loopContext.resources);
        break;
      }
      render_active_stream_frame_if_needed(loopContext, &lastIdleVideoIdrRequestTick);
    }
    loopContext.resources.controller.release_host_input();
  }

  std::string describe_start_failure(const StreamConnectionState &connectionState) {
    if (const int failedStage = connectionState.failedStage.load(); failedStage != STAGE_NONE) {
      std::string message = std::string("Failed to start streaming during ") + LiGetStageName(failedStage) + " (error " + std::to_string(connectionState.failedCode.load()) + ")";
      if (const std::string protocolMessage = latest_connection_protocol_message(connectionState); !protocolMessage.empty()) {
        message += ": ";
        message += protocolMessage;
      }
      return message;
    }
    if (const int startResult = connectionState.startResult.load(); startResult != 0) {
      std::string message = std::string("Failed to start streaming transport (library error ") + std::to_string(startResult) + ")";
      if (const std::string protocolMessage = latest_connection_protocol_message(connectionState); !protocolMessage.empty()) {
        message += ": ";
        message += protocolMessage;
      }
      return message;
    }
    return "Failed to start streaming transport";
  }

  std::string describe_session_end(const StreamConnectionState &connectionState, std::string_view appName) {
    if (connectionState.stopRequested.load()) {
      return "Stopped streaming " + std::string(appName);
    }
    if (const int terminationError = connectionState.terminationError.load(); terminationError == ML_ERROR_GRACEFUL_TERMINATION) {
      return std::string("The host closed ") + std::string(appName) + " cleanly";
    }
    if (connectionState.connectionTerminated.load()) {
      return std::string("The stream ended unexpectedly for ") + std::string(appName) + " (error " + std::to_string(connectionState.terminationError.load()) + ")";
    }
    return std::string("Streaming session ended for ") + std::string(appName);
  }

}  // namespace

namespace streaming {

  bool run_stream_session(
    SDL_Window *window,
    const VIDEO_MODE &videoMode,
    app::SettingsState &settings,
    const app::HostRecord &host,
    const app::HostAppRecord &app,
    const network::PairingIdentity &clientIdentity,
    SDL_GameController *preferredController,
    SDL_GameController **returnController,
    std::string *statusMessage
  ) {
    if (returnController != nullptr) {
      *returnController = nullptr;
    }
    const ResolvedStreamParameters streamParameters = resolve_stream_parameters(videoMode, settings);
    const VIDEO_MODE streamPresentationVideoMode = select_stream_presentation_video_mode(videoMode, streamParameters.videoMode);
    ScopedStreamVideoMode scopedStreamVideoMode(window, videoMode, streamPresentationVideoMode);
    const VIDEO_MODE &activeStreamVideoMode = scopedStreamVideoMode.active_video_mode();

    StreamUiResources resources {};
    resources.statisticsEnabled = settings.presentation.showPerformanceStats;
    if (std::string initializationError; !initialize_stream_ui_resources(window, activeStreamVideoMode, preferredController, &resources, &initializationError)) {
      assign_status_message(statusMessage, initializationError);
      logging::error("stream", initializationError);
      return false;
    }
    resources.controller.configure(settings.controls.screenshotDirectory, settings.controls.keyboardDockTop, app::screenshot_source(host, app));
    ScopedThreadPriority streamThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    startup::log_memory_statistics();

    std::array<unsigned char, 16> remoteInputKey {};
    std::array<unsigned char, 16> remoteInputIv {};
    if (std::string randomError; !fill_random_bytes(remoteInputKey.data(), remoteInputKey.size(), &randomError) || !fill_random_bytes(remoteInputIv.data(), remoteInputIv.size(), &randomError)) {
      close_stream_ui_resources(&resources);
      assign_status_message(statusMessage, randomError);
      logging::error("stream", randomError);
      return false;
    }

    const std::string hostAddress = host.activeAddress.empty() ? host.address : host.activeAddress;
    const uint16_t httpPort = host.resolvedHttpPort == 0U ? app::effective_host_port(host.port) : host.resolvedHttpPort;

    network::StreamLaunchResult launchResult {};
    network::StreamLaunchConfiguration launchConfiguration {};
    launchConfiguration.appId = app.id;
    launchConfiguration.width = select_stream_width(streamParameters.videoMode);
    launchConfiguration.height = select_stream_height(streamParameters.videoMode);
    launchConfiguration.fps = streamParameters.fps;
    launchConfiguration.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    launchConfiguration.playAudioOnPc = settings.audio.playOnPc;
    launchConfiguration.clientRefreshRateX100 = select_client_refresh_rate_x100(activeStreamVideoMode);
    launchConfiguration.remoteInputAesKeyHex = hex_encode(remoteInputKey.data(), remoteInputKey.size());
    launchConfiguration.remoteInputAesIvHex = hex_encode(remoteInputIv.data(), remoteInputIv.size());

    if (std::string launchError; !network::launch_or_resume_stream(hostAddress, httpPort, clientIdentity, launchConfiguration, &launchResult, &launchError)) {
      close_stream_ui_resources(&resources);
      assign_status_message(statusMessage, launchError);
      logging::error("stream", launchError);
      return false;
    }

    StreamConnectionState connectionState {};
    StreamStartContext startContext {};
    startContext.connectionState = &connectionState;
    startContext.mediaBackend = &resources.mediaBackend;
    configure_stream_start_context(launchResult, remoteInputKey, remoteInputIv, activeStreamVideoMode, streamParameters, &startContext);
    if (startContext.appVersion != startContext.reportedAppVersion) {
      logging::info(
        "stream",
        std::string("Normalized host appversion from ") + startContext.reportedAppVersion + " to " + startContext.appVersion + " for " + (startContext.gfeVersion.empty() ? std::string("the current host") : startContext.gfeVersion)
      );
    }
    logging::info(
      "stream",
      std::string("Starting stream setup for ") + app.name + " on " + host.displayName + " at " + startContext.address + " | appversion " + startContext.appVersion + (startContext.rtspSessionUrl.empty() ? std::string(" | default RTSP discovery") : std::string(" | host session URL supplied"))
    );
    logging::info(
      "stream",
      std::string("Requested stream configuration | resolution=") + std::to_string(startContext.streamConfiguration.width) + "x" + std::to_string(startContext.streamConfiguration.height) + " | fps=" + std::to_string(startContext.streamConfiguration.fps) + " | bitrate=" + std::to_string(startContext.streamConfiguration.bitrate) + " kbps" + " | packetSize=" + std::to_string(startContext.streamConfiguration.packetSize) + " | networkProfile=" + describe_streaming_remotely_mode(startContext.streamConfiguration.streamingRemotely) + " | clientRefreshX100=" + std::to_string(startContext.streamConfiguration.clientRefreshRateX100)
    );
    resources.mediaBackend.audio_player().configure(settings.audio.mode, settings.audio.playOnXbox);
    resources.mediaBackend.set_gpu_presentation_enabled(settings.presentation.gpuPresentation);
    logging::info("stream", std::string("Xbox audio mode: ") + (settings.audio.playOnXbox ? audio_mode_label(settings.audio.mode) : "Disabled"));
    logging::debug(
      "stream",
      std::string("Stream connection metadata | active=") + printable_log_value(startContext.serverInformation.address == nullptr ? std::string_view {} : std::string_view {startContext.serverInformation.address}) +
        " | local=" + printable_log_value(launchResult.serverInfo.localAddress) +
        " | remote=" + printable_log_value(launchResult.serverInfo.remoteAddress) +
        " | gfeVersion=" + printable_log_value(startContext.gfeVersion) +
        " | sessionUrl=" + printable_log_value(startContext.rtspSessionUrl)
    );
    resources.mediaBackend.initialize_callbacks(&startContext.videoCallbacks, &startContext.audioCallbacks);
    startContext.connectionCallbacks.stageStarting = on_stage_starting;
    startContext.connectionCallbacks.stageComplete = on_stage_complete;
    startContext.connectionCallbacks.stageFailed = on_stage_failed;
    startContext.connectionCallbacks.connectionStarted = on_connection_started;
    startContext.connectionCallbacks.connectionTerminated = on_connection_terminated;
    startContext.connectionCallbacks.connectionStatusUpdate = on_connection_status_update;
    startContext.connectionCallbacks.logMessage = on_log_message;

    bool rtspFallbackAttempted = false;
    if (const StreamStartAttemptContext startAttempt {host, app, startContext, connectionState, resources, statusMessage}; !run_stream_start_with_rtsp_fallback(startAttempt, &rtspFallbackAttempted)) {
      return false;
    }

    if (connectionState.startResult.load() != 0 || !connectionState.connectionStarted.load()) {
      std::string failureMessage = connectionState.stopRequested.load() ? std::string("Cancelled the stream start for ") + app.name : describe_start_failure(connectionState);
      if (rtspFallbackAttempted && !connectionState.stopRequested.load()) {
        failureMessage += " after retrying default RTSP discovery";
      }
      active_connection_state() = nullptr;
      close_stream_ui_resources(&resources);
      assign_status_message(statusMessage, failureMessage);
      logging::warn("stream", failureMessage);
      return false;
    }

    logging::info("stream", std::string(launchResult.resumedSession ? "Resumed stream for " : "Launched stream for ") + app.name + " on " + host.displayName);
    run_active_stream_loop({settings, host, app, startContext, connectionState, resources});

    streaming::StreamStatisticsSnapshot finalStatistics = sample_stream_statistics(startContext, connectionState, resources.mediaBackend);
    resources.statisticsSampler.apply(&finalStatistics);
    if (finalStatistics.decodeTimeMs >= 0) {
      finalStatistics.decoderLatencyMs = static_cast<int>(finalStatistics.decodeTimeMs + 0.5);
    }
    if (finalStatistics.hostProcessingMs >= 0) {
      finalStatistics.hostLatencyMs = static_cast<int>(finalStatistics.hostProcessingMs + 0.5);
    }
    logging::info("stream", "Stopping stream transport");
    LiStopConnection();
    logging::info("stream", "Stopped stream transport");
    active_connection_state() = nullptr;
    const streaming::VideoAccelerationReport acceleration = resources.mediaBackend.acceleration_report();
    finalStatistics.accelerationSummary = acceleration.summary_lines();
    finalStatistics.showPerformanceMetrics = resources.statisticsEnabled;

    std::string finalMessage = describe_session_end(connectionState, app.name);
    if (resources.controller.quit_app_requested()) {
      std::string quitError;
      if (network::quit_host_app(host.address, host.port, clientIdentity, app.id, &quitError)) {
        finalMessage = "Quit " + app.name + " on " + host.displayName;
      } else {
        finalMessage += " | Couldn't quit app: " + quitError;
      }
    }
    if (!acceleration.fallbackReason.empty()) {
      finalMessage += " | SOFTWARE FALLBACK USED";
    }
    logging::info("stream", finalMessage);
    for (const auto &line : finalStatistics.accelerationSummary) {
      logging::info("stream", line);
    }
    startup::log_memory_statistics();
    if (settings.controls.keyboardDockTop != resources.controller.keyboard_docked_top()) {
      settings.controls.keyboardDockTop = resources.controller.keyboard_docked_top();
      settings.dirty = true;
    }
    show_stream_end_statistics(host, app, startContext, finalStatistics, finalMessage, &resources);
    if (returnController != nullptr) {
      *returnController = resources.controller.take_controller_for_shell();
    }
    close_stream_ui_resources(&resources);
    assign_status_message(statusMessage, finalMessage);
    return true;
  }

}  // namespace streaming
