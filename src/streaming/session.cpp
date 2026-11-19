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
#include <optional>
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
#include "src/startup/video_mode.h"
#include "src/streaming/ffmpeg_stream_backend.h"
#include "src/streaming/stats_overlay.h"
#include "src/streaming/stream_controls.h"
#include "src/ui/dialog_surface.h"
#include "src/ui/shell_layout.h"
#include "src/ui/shell_view.h"

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
  constexpr Uint32 STREAM_FRAME_DELAY_MILLISECONDS = 16U;
  constexpr Uint32 STREAM_PRESENT_POLL_MILLISECONDS = 2U;
  constexpr Uint32 STREAM_VIDEO_IDLE_IDR_MILLISECONDS = 1500U;
  constexpr Uint32 STREAM_VIDEO_IDLE_IDR_COOLDOWN_MILLISECONDS = 2000U;
  constexpr int MIN_STREAM_FPS = 15;
  constexpr int MAX_STREAM_FPS = 60;
  constexpr int MIN_STREAM_BITRATE_KBPS = 250;
  constexpr int MAX_STREAM_BITRATE_KBPS = 50000;
  constexpr int DEFAULT_PACKET_SIZE = 1024;
  constexpr std::size_t MAX_CONNECTION_PROTOCOL_MESSAGES = 24U;

  /**
   * @brief Holds the state owned by the stream's event and render thread.
   */
  struct StreamUiResources {
    SDL_Renderer *renderer = nullptr;
    TTF_Font *titleFont = nullptr;
    TTF_Font *bodyFont = nullptr;
    TTF_Font *statisticsFont = nullptr;  ///< Font used only when rebuilding the live panel.
    bool statisticsEnabled = false;  ///< Performance Stats controls the live panel and end metrics.
    streaming::VideoStatisticsSampler statisticsSampler;  ///< One-second telemetry intervals.
    streaming::StreamControls controller;
    streaming::FfmpegStreamBackend mediaBackend {};
    std::optional<Uint32> startupTick;  ///< Session entry tick, cleared when the first video frame is presented.
    std::optional<VIDEO_MODE> pendingVideoMode;  ///< Output to switch to immediately before presenting decoded video.
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

  /**
   * @brief References needed to run a transport startup attempt and report a failure.
   */
  struct StreamStartAttemptContext {
    const app::HostRecord &host;
    const app::HostAppRecord &app;
    StreamStartContext &startContext;
    StreamConnectionState &connectionState;
    StreamUiResources &resources;
    std::string &statusMessage;  ///< Result text to update if setup fails.
  };

  class ScopedStreamVideoMode;

  struct ActiveStreamLoopContext {
    const app::HostRecord &host;
    const app::HostAppRecord &app;
    StreamStartContext &startContext;
    StreamConnectionState &connectionState;
    StreamUiResources &resources;
    ScopedStreamVideoMode &videoMode;  ///< Owns the temporary mode switch and restores the shell mode on exit.
  };

  struct ResolvedStreamParameters {
    VIDEO_MODE videoMode {};
    int fps = app::DEFAULT_STREAM_FRAMERATE;
    int bitrateKbps = app::DEFAULT_STREAM_BITRATE_KBPS;
    int packetSize = DEFAULT_PACKET_SIZE;
    int streamingRemotely = STREAM_CFG_AUTO;
  };

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

  class ScopedStreamVideoMode {
  public:
    ScopedStreamVideoMode(SDL_Window *window, const VIDEO_MODE &originalVideoMode, const VIDEO_MODE &requestedVideoMode):
        window_(window),
        originalVideoMode_(originalVideoMode),
        activeVideoMode_(originalVideoMode) {
      switch_to(requestedVideoMode);
    }

    /**
     * @brief Apply a streaming display mode to the Xbox and SDL window.
     * @param requestedVideoMode The desired output mode.
     */
    void switch_to(const VIDEO_MODE &requestedVideoMode) {
      if (requestedVideoMode.width <= 0 || requestedVideoMode.height <= 0 || video_modes_match(requestedVideoMode, activeVideoMode_)) {
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
      changed_ = !video_modes_match(activeVideoMode_, originalVideoMode_);
      if (window_ != nullptr) {
        SDL_SetWindowSize(window_, activeVideoMode_.width, activeVideoMode_.height);
      }
      logging::info("stream", "Switched Xbox video mode for stream presentation to " + std::to_string(activeVideoMode_.width) + "x" + std::to_string(activeVideoMode_.height) + " at " + std::to_string(activeVideoMode_.bpp) + " bpp");
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
    resources->bodyFont = TTF_OpenFont(fontPath.c_str(), std::max(16, videoMode.height / streaming::controls_raster_scale(videoMode.height) / 28));
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

  /**
   * @brief Render text into a bounded area without changing SDL's renderer clip.
   * @param renderer Active SDL renderer.
   * @param surface Rasterized text.
   * @param bounds Available space for the text.
   * @return True when the text was copied to the renderer.
   */
  bool render_text_surface(SDL_Renderer *renderer, SDL_Surface *surface, const SDL_Rect &bounds) {
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture == nullptr) {
      return false;
    }
    const SDL_Rect source {0, 0, std::min(surface->w, bounds.w), std::min(surface->h, bounds.h)};
    const SDL_Rect destination {bounds.x, bounds.y, source.w, source.h};
    const bool rendered = SDL_RenderCopy(renderer, texture, &source, &destination) == 0;
    SDL_DestroyTexture(texture);
    return rendered;
  }

  /**
   * @brief Rasterize and draw a wrapped run of text at the requested position.
   * @param renderer Active SDL renderer.
   * @param font Font used for the run.
   * @param text Text to draw.
   * @param bounds Available space for the text.
   * @param color Text color.
   * @return True when the text was rendered.
   */
  bool render_text_line(SDL_Renderer *renderer, TTF_Font *font, const std::string &text, const SDL_Rect &bounds, SDL_Color color) {
    if (renderer == nullptr || font == nullptr || bounds.w <= 0 || bounds.h <= 0) {
      return false;
    }

    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface(TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), color, static_cast<Uint32>(bounds.w)), &SDL_FreeSurface);
    return surface && render_text_surface(renderer, surface.get(), bounds);
  }

  /**
   * @brief Describe the current transport stage or an interruption when more detail is available.
   * @param connectionState Current transport stage and outcome.
   * @return Text overriding the startup step label, or an empty string to use that label.
   */
  std::string build_stage_status_line(const StreamConnectionState &connectionState) {
    if (const int failedStage = connectionState.failedStage.load(); failedStage != STAGE_NONE) {
      return std::string("Connection failed during ") + LiGetStageName(failedStage) + " (error " + std::to_string(connectionState.failedCode.load()) + ")";
    }
    if (connectionState.connectionTerminated.load()) {
      const int terminationError = connectionState.terminationError.load();
      return terminationError == ML_ERROR_GRACEFUL_TERMINATION ? "The host ended the stream gracefully." : "The host ended the stream (error " + std::to_string(terminationError) + ")";
    }
    if (const int stage = connectionState.currentStage.load(); stage != STAGE_NONE && !connectionState.connectionStarted.load()) {
      return std::string("Establishing the stream connection (") + LiGetStageName(stage) + ")...";
    }
    return {};
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
    if (resources == nullptr || resources->pendingVideoMode || resources->controller.has_overlay() || !resources->statisticsEnabled || resources->statisticsFont == nullptr || !resources->mediaBackend.has_decoded_video()) {
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

  /**
   * @brief Paint startup progress with a footer that stays inside television-safe margins.
   * @param resources Event-thread renderer and fonts.
   * @param dialog Modal content with an optional startup progress step.
   * @param screenWidth Output width in pixels.
   * @param screenHeight Output height in pixels.
   * @param footer Optional stream-menu hint.
   * @return True when the dialog was drawn.
   */
  bool draw_stream_start_dialog(const StreamUiResources &resources, const ui::ShellModalView &dialog, int screenWidth, int screenHeight, const std::string &footer = {}) {
    const int padding = std::max(20, screenHeight / 40);
    const int dialogWidth = std::min(760, screenWidth * 3 / 4);
    const int innerWidth = dialogWidth - 2 * padding;
    const int titleHeight = TTF_FontLineSkip(resources.titleFont);
    const int bodyTop = 28 + titleHeight + 12;
    constexpr int progressHeight = 8;
    int footerTextHeight = 0;
    if (!footer.empty()) {
      int footerWidth = 0;
      const bool wraps = TTF_SizeUTF8(resources.bodyFont, footer.c_str(), &footerWidth, nullptr) == 0 && footerWidth > innerWidth;
      footerTextHeight = TTF_FontLineSkip(resources.bodyFont) * (wraps ? 2 : 1);
    }
    const int footerHeight = progressHeight + 12 + footerTextHeight;
    const int maxBodyHeight = screenHeight - screenHeight / 10 - bodyTop - footerHeight - 2 * padding;
    if (innerWidth <= 0 || maxBodyHeight <= 0) {
      return false;
    }

    std::string bodyText;
    for (const auto &line : dialog.lines) {
      if (!bodyText.empty()) {
        bodyText += '\n';
      }
      bodyText += line;
    }
    const auto rasterizeBody = [&](TTF_Font *font) {
      return std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>(
        TTF_RenderUTF8_Blended_Wrapped(font, bodyText.c_str(), {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, static_cast<Uint32>(innerWidth)),
        SDL_FreeSurface
      );
    };
    auto body = rasterizeBody(resources.bodyFont);
    if (body && body->h > maxBodyHeight && resources.statisticsFont != nullptr) {
      if (auto compact = rasterizeBody(resources.statisticsFont); compact && compact->h < body->h) {
        body = std::move(compact);
      }
    }
    if (!body) {
      return false;
    }

    const int bodyHeight = std::min(body->h, maxBodyHeight);
    const auto [x, y, w, h] = ui::calculate_shell_dialog(screenWidth, screenHeight, dialogWidth, bodyTop + bodyHeight + footerHeight + 2 * padding);
    SDL_SetRenderDrawColor(resources.renderer, BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0xFF);
    SDL_RenderClear(resources.renderer);
    ui::draw_dialog_surface(resources.renderer, {x, y, w, h});

    const int left = x + padding;
    const SDL_Color text {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF};
    if (!render_text_line(resources.renderer, resources.titleFont, dialog.title, {left, y + 28, innerWidth, titleHeight}, text) || !render_text_surface(resources.renderer, body.get(), {left, y + bodyTop, innerWidth, bodyHeight})) {
      return false;
    }
    if (dialog.streamStartupStep.has_value()) {
      ui::draw_dialog_progress(resources.renderer, {left, y + h - padding - footerHeight, innerWidth, progressHeight}, static_cast<int>(*dialog.streamStartupStep), ui::STREAM_STARTUP_STEP_COUNT);
    }
    if (!footer.empty()) {
      const int footerY = y + h - padding - footerTextHeight;
      return render_text_line(resources.renderer, resources.bodyFont, footer, {left, footerY, innerWidth, footerTextHeight}, {0x82, 0xB4, 0xFF, 0xFF});
    }
    return true;
  }

  /**
   * @brief Present the latest decoded frame or continue showing startup progress.
   * @param host Selected PC.
   * @param app Selected app on the PC.
   * @param context Stream configuration used for diagnostics.
   * @param connectionState Current transport stage and outcome.
   * @param mediaBackend Backend supplying video and local controls.
   * @param resources Event-thread renderer, fonts and first-frame timing.
   * @return True when there are resources to render the frame.
   */
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

    const bool hasDecodedVideo = !resources->pendingVideoMode && mediaBackend != nullptr && mediaBackend->has_decoded_video();
    if (hasDecodedVideo && mediaBackend->render_latest_video_frame(resources->renderer, screenWidth, screenHeight)) {
      if (resources->startupTick.has_value()) {
        logging::info("stream", "Stream startup: first video frame presented after " + std::to_string(SDL_GetTicks() - *resources->startupTick) + " ms in the session");
        resources->startupTick.reset();
      }
      render_stream_input_diagnostics(resources);
      return true;
    }
    const auto step = connectionState.connectionStarted.load() ? ui::StreamStartupStep::first_frame : ui::StreamStartupStep::connecting;
    auto dialog = ui::build_stream_launch_dialog(host, app, step);
    if (auto detail = build_stage_status_line(connectionState); !detail.empty()) {
      dialog.lines.front() = std::move(detail);
    }
    if (resources->statisticsEnabled && mediaBackend != nullptr) {
      dialog.lines.push_back(mediaBackend->build_overlay_status_line());
    }
    draw_stream_start_dialog(*resources, dialog, screenWidth, screenHeight, input::STREAM_EXIT_HINT);

    if (resources->controller.has_overlay() && mediaBackend != nullptr) {
      mediaBackend->render_local_overlay(resources->renderer, screenWidth, screenHeight);
    }
    SDL_RenderPresent(resources->renderer);
    render_stream_input_diagnostics(resources);
    return true;
  }

  /**
   * @brief Build the summary displayed by the shell after restoring the app picker.
   * @param host PC that supplied the stream.
   * @param app Selected host app.
   * @param context Stream configuration and address.
   * @param statisticsSnapshot Final acceleration and performance data.
   * @param finalMessage Human-readable session outcome.
   * @return Summary rows with optional performance details.
   */
  std::vector<std::string> build_stream_summary_lines(
    const app::HostRecord &host,
    const app::HostAppRecord &app,
    const StreamStartContext &context,
    const streaming::StreamStatisticsSnapshot &statisticsSnapshot,
    const std::string &finalMessage
  ) {
    std::vector<std::string> lines {finalMessage, "Host: " + host.displayName + " (" + context.address + ")", "App: " + app.name};
    lines.insert(lines.end(), statisticsSnapshot.accelerationSummary.begin(), statisticsSnapshot.accelerationSummary.end());
    if (statisticsSnapshot.showPerformanceMetrics) {
      const auto performance = streaming::build_stats_overlay_lines(statisticsSnapshot);
      lines.insert(lines.end(), performance.begin(), performance.end());
    }
    return lines;
  }

  /**
   * @brief Run transport startup while the event thread handles input and presents progress.
   * @param attempt Resources and status for one transport startup attempt.
   * @return True when the worker ran to completion or cancellation.
   */
  bool run_stream_start_attempt(const StreamStartAttemptContext &attempt) {
    if (SDL_Thread *startThread = SDL_CreateThread(run_stream_start_thread, "start-stream", &attempt.startContext); startThread != nullptr) {
      while (!attempt.connectionState.startCompleted.load() && !attempt.connectionState.stopRequested.load()) {
        pump_stream_events(&attempt.resources, &attempt.connectionState);
        if (attempt.connectionState.stopRequested.load()) {
          LiInterruptConnection();
          break;
        }
        render_stream_frame(attempt.host, attempt.app, attempt.startContext, attempt.connectionState, &attempt.resources.mediaBackend, &attempt.resources);
        SDL_Delay(STREAM_FRAME_DELAY_MILLISECONDS);
      }

      int threadResult = 0;
      SDL_WaitThread(startThread, &threadResult);
      (void) threadResult;
      return true;
    }

    const std::string createThreadError = std::string("Failed to start the streaming transport thread: ") + SDL_GetError();
    close_stream_ui_resources(&attempt.resources);
    attempt.statusMessage = createThreadError;
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

  /**
   * @brief Switch to the pending 1080i mode once the first frame is ready.
   * @param loopContext Active stream resources and display mode.
   */
  void activate_pending_stream_video_mode(const ActiveStreamLoopContext &loopContext) {
    auto &resources = loopContext.resources;
    const int previousHeight = loopContext.videoMode.active_video_mode().height;
    loopContext.videoMode.switch_to(*resources.pendingVideoMode);
    resources.pendingVideoMode.reset();
    const int outputHeight = loopContext.videoMode.active_video_mode().height;
    if (outputHeight == previousHeight) {
      return;
    }

    resources.controller.invalidate_panel(resources.mediaBackend);
    const std::string fontPath = build_font_path();
    const auto resizeFont = [&fontPath](TTF_Font *&font, int size) {
      if (font == nullptr) {
        return;
      }
      if (auto *replacement = TTF_OpenFont(fontPath.c_str(), size); replacement != nullptr) {
        TTF_CloseFont(font);
        font = replacement;
      } else {
        logging::warn("stream", std::string("Unable to resize a streaming font: ") + TTF_GetError());
      }
    };
    resizeFont(resources.titleFont, std::max(22, outputHeight / 18));
    resizeFont(resources.bodyFont, std::max(16, outputHeight / streaming::controls_raster_scale(outputHeight) / 28));
    resizeFont(resources.statisticsFont, std::max(12, outputHeight / 45));
  }

  void render_active_stream_frame_if_needed(const ActiveStreamLoopContext &loopContext, Uint32 *lastIdleVideoIdrRequestTick) {
    const bool hasDecodedVideo = loopContext.resources.mediaBackend.has_decoded_video();
    if (hasDecodedVideo && loopContext.resources.pendingVideoMode) {
      activate_pending_stream_video_mode(loopContext);
    }
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
    StreamSessionReport &report
  ) {
    const Uint32 sessionStarted = SDL_GetTicks();
    report = {};
    const ResolvedStreamParameters streamParameters = resolve_stream_parameters(videoMode, settings);
    const VIDEO_MODE streamPresentationVideoMode = startup::choose_stream_output_mode(streamParameters.videoMode, videoMode);
    const VIDEO_MODE startupVideoMode = startup::choose_stream_startup_mode(streamPresentationVideoMode, videoMode);
    ScopedStreamVideoMode scopedStreamVideoMode(window, videoMode, startupVideoMode);

    StreamUiResources resources {};
    if (!video_modes_match(startupVideoMode, streamPresentationVideoMode)) {
      resources.pendingVideoMode = streamPresentationVideoMode;
    }
    resources.startupTick = sessionStarted;
    resources.statisticsEnabled = settings.presentation.showPerformanceStats;
    const auto &activeStreamVideoMode = scopedStreamVideoMode.active_video_mode();
    if (std::string initializationError; !initialize_stream_ui_resources(window, activeStreamVideoMode, preferredController, &resources, &initializationError)) {
      report.message = initializationError;
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
      report.message = randomError;
      logging::error("stream", randomError);
      return false;
    }

    const std::string hostAddress = host.activeAddress.empty() ? host.address : host.activeAddress;
    const uint16_t httpPort = host.resolvedHttpPort == 0U ? app::effective_host_port(host.port) : host.resolvedHttpPort;

    network::StreamLaunchResult launchResult {};
    const VIDEO_MODE negotiatedOutputMode = resources.pendingVideoMode.value_or(activeStreamVideoMode);
    network::StreamLaunchConfiguration launchConfiguration {};
    launchConfiguration.appId = app.id;
    launchConfiguration.width = select_stream_width(streamParameters.videoMode);
    launchConfiguration.height = select_stream_height(streamParameters.videoMode);
    launchConfiguration.fps = streamParameters.fps;
    launchConfiguration.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    launchConfiguration.playAudioOnPc = settings.audio.playOnPc;
    launchConfiguration.clientRefreshRateX100 = select_client_refresh_rate_x100(negotiatedOutputMode);
    launchConfiguration.remoteInputAesKeyHex = hex_encode(remoteInputKey.data(), remoteInputKey.size());
    launchConfiguration.remoteInputAesIvHex = hex_encode(remoteInputIv.data(), remoteInputIv.size());

    int screenWidth = 0;
    int screenHeight = 0;
    SDL_GetRendererOutputSize(resources.renderer, &screenWidth, &screenHeight);
    draw_stream_start_dialog(resources, ui::build_stream_launch_dialog(host, app, ui::StreamStartupStep::starting_on_host), screenWidth, screenHeight);
    SDL_RenderPresent(resources.renderer);

    std::string launchError;
    const Uint32 hostRequestStarted = SDL_GetTicks();
    const bool accepted = network::launch_or_resume_stream(hostAddress, httpPort, clientIdentity, launchConfiguration, &launchResult, &launchError);
    logging::info("stream", "Stream startup: host request took " + std::to_string(SDL_GetTicks() - hostRequestStarted) + " ms");
    if (!accepted) {
      close_stream_ui_resources(&resources);
      report.message = launchError;
      logging::error("stream", launchError);
      return false;
    }

    StreamConnectionState connectionState {};
    StreamStartContext startContext {};
    startContext.connectionState = &connectionState;
    startContext.mediaBackend = &resources.mediaBackend;
    configure_stream_start_context(launchResult, remoteInputKey, remoteInputIv, negotiatedOutputMode, streamParameters, &startContext);
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
    if (const StreamStartAttemptContext startAttempt {host, app, startContext, connectionState, resources, report.message}; !run_stream_start_with_rtsp_fallback(startAttempt, &rtspFallbackAttempted)) {
      return false;
    }

    if (connectionState.startResult.load() != 0 || !connectionState.connectionStarted.load()) {
      std::string failureMessage = connectionState.stopRequested.load() ? std::string("Cancelled the stream start for ") + app.name : describe_start_failure(connectionState);
      if (rtspFallbackAttempted && !connectionState.stopRequested.load()) {
        failureMessage += " after retrying default RTSP discovery";
      }
      active_connection_state() = nullptr;
      close_stream_ui_resources(&resources);
      report.message = failureMessage;
      logging::warn("stream", failureMessage);
      return false;
    }

    logging::info("stream", std::string(launchResult.resumedSession ? "Resumed stream for " : "Launched stream for ") + app.name + " on " + host.displayName);
    run_active_stream_loop({host, app, startContext, connectionState, resources, scopedStreamVideoMode});

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
    report.summaryLines = build_stream_summary_lines(host, app, startContext, finalStatistics, finalMessage);
    report.controller = resources.controller.take_controller_for_shell();
    close_stream_ui_resources(&resources);
    report.message = std::move(finalMessage);
    return true;
  }

}  // namespace streaming
