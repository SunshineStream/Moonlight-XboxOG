/**
 * @file src/streaming/session.cpp
 * @brief Implements the streaming session runtime.
 */
// class header include
#include "src/streaming/session.h"

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <Limelight.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

// platform includes
#ifdef NXDK
  #include <windows.h>  // NOSONAR(cpp:S3806) nxdk requires lowercase header names
#else
  #include <openssl/rand.h>
#endif

#if defined(NXDK) && SUNSHINE_INPUT_DIAGNOSTICS
  #include <hal/debug.h>
#endif

// local includes
#include "src/input/shell_controller.h"
#include "src/input/stream_exit.h"
#include "src/input/stream_input.h"
#include "src/logging/logger.h"
#include "src/startup/memory_stats.h"
#include "src/startup/video_mode.h"
#include "src/streaming/ffmpeg_stream_backend.h"
#include "src/streaming/stats_overlay.h"
#include "src/streaming/stream_controls.h"
#include "src/streaming/video_codec_selection.h"
#include "src/ui/asset_path.h"
#include "src/ui/brand_colors.h"
#include "src/ui/dialog_surface.h"
#include "src/ui/sdl_render.h"
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

  constexpr Uint32 STREAM_FRAME_DELAY_MILLISECONDS = 16U;
  constexpr Uint32 STREAM_PRESENT_POLL_MILLISECONDS = 2U;
  constexpr Uint32 STREAM_VIDEO_IDLE_IDR_MILLISECONDS = 1500U;
  constexpr Uint32 STREAM_VIDEO_IDLE_IDR_COOLDOWN_MILLISECONDS = 2000U;
  constexpr int MIN_STREAM_FPS = 15;
#ifdef SUNSHINE_NATIVE_CLIENT
  constexpr int MAX_STREAM_FPS = 120;
  constexpr int MAX_STREAM_BITRATE_KBPS = 150000;
#else
  constexpr int MAX_STREAM_FPS = 60;
  constexpr int MAX_STREAM_BITRATE_KBPS = 50000;
#endif
  constexpr int MIN_STREAM_BITRATE_KBPS = 250;
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
    std::optional<Uint64> startupTick;  ///< Session entry tick, cleared when the first video frame is presented.
    std::optional<VIDEO_MODE> pendingVideoMode;  ///< Output to switch to immediately before presenting decoded video.
#ifdef SUNSHINE_DESKTOP
    bool relativeMouseRequested = false;  ///< Last requested SDL mouse mode, including unsupported requests.
#endif
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
#ifdef SUNSHINE_NATIVE_CLIENT
    std::array<std::atomic<std::uint64_t>, 4> rumble {};  ///< Latest motor levels per host slot, with a pending marker.
    std::array<std::atomic<std::uint64_t>, 4> triggerRumble {};  ///< Latest trigger motor levels per host slot.
    std::array<std::array<std::atomic<std::uint32_t>, 2>, 4> motion {};  ///< Pending sensor rates per host slot.
    std::array<std::atomic<std::uint32_t>, 4> leds {};  ///< Pending controller colors per host slot.
#endif
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
    VIDEO_MODE videoMode {};  ///< Stream mode requested from the host.
    int fps = app::DEFAULT_STREAM_FRAMERATE;
    int bitrateKbps = app::DEFAULT_STREAM_BITRATE_KBPS;
    app::VideoCodecSelection videoCodec = app::VideoCodecSelection::preferMpeg2;  ///< Decoder preference resolved against host capabilities after launch.
    int packetSize = DEFAULT_PACKET_SIZE;
    int streamingRemotely = STREAM_CFG_AUTO;
  };

  /** @brief Restore normal render-thread priority when leaving the scope. */
  class ScopedThreadPriority {
  public:
    /**
     * @brief Set the render thread's priority for the duration of this scope.
     * @param priority SDL priority to request.
     */
    explicit ScopedThreadPriority(SDL_ThreadPriority priority):
        active_(SDL_SetCurrentThreadPriority(priority)) {
      if (!active_) {
        logging::debug("stream", std::string("SDL_SetCurrentThreadPriority failed for stream render thread: ") + SDL_GetError());
      }
    }

    ~ScopedThreadPriority() {
      if (active_) {
        SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
      }
    }

    ScopedThreadPriority(const ScopedThreadPriority &) = delete;
    ScopedThreadPriority &operator=(const ScopedThreadPriority &) = delete;

  private:
    bool active_;  ///< True after SDL accepted the priority change.
  };

  /**
   * @brief Compare the dimensions, pixel depth and refresh rate of two display modes.
   * @param left First mode.
   * @param right Second mode.
   * @return True when both modes use the same settings.
   */
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

#ifdef NXDK
    /**
     * @brief Apply a streaming display mode to the Xbox and SDL window.
     * @param requestedVideoMode The desired output mode.
     */
    void switch_to(const VIDEO_MODE &requestedVideoMode) {
      if (requestedVideoMode.width <= 0 || requestedVideoMode.height <= 0 || video_modes_match(requestedVideoMode, activeVideoMode_)) {
        return;
      }

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
    }
#else
    /**
     * @brief Retain the operating system's display mode on portable clients.
     * @param requestedVideoMode Unused Xbox scanout mode.
     */
    static void switch_to(const VIDEO_MODE &requestedVideoMode) {
      (void) requestedVideoMode;
    }
#endif

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

    const std::string version(appVersion);
    const char *cursor = version.c_str();
    std::array<int, 3> components {};
    for (auto &component : components) {
      char *end = nullptr;
      errno = 0;
      const long value = std::strtol(cursor, &end, 10);
      if (end == cursor || errno == ERANGE || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return "7.1.431.-1";
      }
      component = static_cast<int>(value);
      // Sunshine's fourth component is replaced, so only separators between the first three are required.
      if (&component != &components.back()) {
        if (*end != '.') {
          return "7.1.431.-1";
        }
        cursor = end + 1;
      }
    }

    constexpr std::array minimumVersion {7, 1, 431};
    const auto &[major, minor, patch] = std::max(components, minimumVersion);

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
    return ui::build_asset_path("fonts/vegur-regular.ttf");
  }

#ifdef SUNSHINE_DESKTOP
  /**
   * @brief Apply a relative mouse mode change on the SDL event thread.
   * @param window Stream window receiving mouse events.
   * @param enabled True to capture the pointer for relative movement.
   */
  void set_stream_relative_mouse_mode(SDL_Window *window, bool enabled) {
    if (!SDL_SetWindowRelativeMouseMode(window, enabled)) {
      logging::warn("input", std::string("Could not change relative mouse mode: ") + SDL_GetError());
    }
  }

  /**
   * @brief Release the pointer before transport shutdown or return to the shell.
   * @param resources Stream state and renderer associated with the window.
   */
  void release_stream_mouse_capture(StreamUiResources &resources) {
    resources.relativeMouseRequested = false;
    if (resources.renderer != nullptr) {
      if (SDL_Window *window = SDL_GetRenderWindow(resources.renderer); window != nullptr && SDL_GetWindowRelativeMouseMode(window)) {
        set_stream_relative_mouse_mode(window, false);
      }
    }
  }
#endif

  /**
   * @brief Release stream resources and restore the shell's pointer behavior.
   * @param resources Stream resources to close, or nullptr.
   */
  void close_stream_ui_resources(StreamUiResources *resources) {
    if (resources == nullptr) {
      return;
    }

#ifdef SUNSHINE_DESKTOP
    release_stream_mouse_capture(*resources);
#endif
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
  SDL_Gamepad *open_primary_controller(SDL_Gamepad *preferredController) {
    if (preferredController != nullptr && SDL_GamepadConnected(preferredController)) {
      if (SDL_Gamepad *controller = SDL_OpenGamepad(SDL_GetGamepadID(preferredController)); controller != nullptr) {
        logging::info("input", "Retained the menu controller for streaming; USB input remains open");
        return controller;
      }
    }

    SDL_Gamepad *controller = nullptr;
    input::recover_shell_controller(&controller);
    if (controller != nullptr) {
      const std::unique_ptr<char, decltype(&SDL_free)> mapping(SDL_GetGamepadMapping(controller), SDL_free);
      logging::info("input", "Opened stream controller " + std::to_string(SDL_GetGamepadID(controller)) + " | mapping=" + (mapping ? mapping.get() : "unavailable"));
      return controller;
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

  /**
   * @brief Load the session fonts before the shell hands off the display.
   * @param videoMode Output mode used to size fonts.
   * @param resources Receives fonts owned by the session.
   * @param errorMessage Receives an error when required fonts are unavailable.
   * @return True when the title and body fonts are ready.
   */
  bool initialize_stream_fonts(const VIDEO_MODE &videoMode, StreamUiResources &resources, std::string &errorMessage) {
    if (!TTF_Init()) {
      errorMessage = std::string("TTF_Init failed for the streaming session: ") + SDL_GetError();
      return false;
    }
    resources.ttfInitialized = true;

    const std::string fontPath = build_font_path();
    resources.titleFont = TTF_OpenFont(fontPath.c_str(), std::max(22, videoMode.height / 18));
    resources.bodyFont = TTF_OpenFont(fontPath.c_str(), ui::control_hint_font_size(videoMode.height / streaming::controls_raster_scale(videoMode.height)));
    if (resources.titleFont == nullptr || resources.bodyFont == nullptr) {
      errorMessage = "Failed to load the streaming session font from " + fontPath + ": " + SDL_GetError();
      return false;
    }

    resources.statisticsFont = TTF_OpenFont(fontPath.c_str(), std::max(12, videoMode.height / 45));
    if (resources.statisticsFont == nullptr) {
      resources.statisticsEnabled = false;
      logging::warn("stream", std::string("Unable to load the performance overlay font: ") + SDL_GetError());
    }
    return true;
  }

  /**
   * @brief Create the CPU renderer for the startup dialog and fallback presentation.
   * @param window Stream window after the shell renderer has been released.
   * @param errorMessage Receives the SDL failure detail if creation fails.
   * @return Owned renderer, or nullptr on error.
   */
  SDL_Renderer *create_stream_renderer(SDL_Window *window, std::string &errorMessage) {
#ifdef NXDK
    // PVIDEO owns pbkit while streaming, including when the CPU draws the startup dialog.
    auto *renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
#else
    auto *renderer = SDL_CreateRenderer(window, nullptr);
#endif
    if (renderer == nullptr) {
      errorMessage = std::string("SDL_CreateRenderer failed for the streaming session: ") + SDL_GetError();
    }
    return renderer;
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
    if (buffer != nullptr && size <= static_cast<std::size_t>(std::numeric_limits<int>::max()) && RAND_bytes(buffer, static_cast<int>(size)) == 1) {
      return true;
    }
    if (errorMessage != nullptr) {
      *errorMessage = "Failed to generate secure random bytes for the streaming input keys";
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
   * @brief Describe a codec preference for logs and waiting screens.
   *
   * @param selection Codec preference selected in settings.
   * @return Human-readable codec preference.
   */
  const char *describe_video_codec_selection(app::VideoCodecSelection selection) {
    switch (selection) {
      case app::VideoCodecSelection::preferMpeg2:
        return "Auto (Prefer MPEG-2)";
      case app::VideoCodecSelection::autoDetect:
        return "Auto";
      case app::VideoCodecSelection::h264:
        return "H.264";
      case app::VideoCodecSelection::mpeg2:
        return "MPEG-2/H.262";
      case app::VideoCodecSelection::h263p:
        return "H.263+";
    }

    return "Auto";
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
    resolved.videoCodec = settings.video.codec;

    return resolved;
  }

  /**
   * @brief Prepare connection settings and resolve the initial codec choice against the host's capabilities.
   * @param launchResult Host capabilities and endpoint returned by launch or resume.
   * @param remoteInputKey Encryption key for controller and keyboard traffic.
   * @param remoteInputIv Initialization vector for remote input encryption.
   * @param outputVideoMode Current Xbox scanout mode.
   * @param streamParameters Requested video mode, frame rate, bitrate, and codec.
   * @param context Stream startup context receiving the resolved settings.
   */
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
#ifdef SUNSHINE_NATIVE_CLIENT
    int decoderFormats = streaming::FfmpegStreamBackend::available_video_formats();
    if (streamParameters.videoCodec == app::VideoCodecSelection::h264) {
      decoderFormats &= VIDEO_FORMAT_H264;
    }
    context->streamConfiguration.supportedVideoFormats = streaming::portable_video_formats(decoderFormats, launchResult.serverCodecModeSupport);
#else
    context->streamConfiguration.supportedVideoFormats = streaming::supported_video_formats(streamParameters.videoCodec, launchResult.serverCodecModeSupport);
#endif
    context->streamConfiguration.clientRefreshRateX100 = select_client_refresh_rate_x100(outputVideoMode);
#ifdef SUNSHINE_NATIVE_CLIENT
    context->streamConfiguration.colorSpace = COLORSPACE_REC_709;
    context->streamConfiguration.encryptionFlags = ENCFLG_ALL;
#else
    context->streamConfiguration.colorSpace = COLORSPACE_REC_601;
    context->streamConfiguration.encryptionFlags = ENCFLG_NONE;
#endif
    context->streamConfiguration.colorRange = COLOR_RANGE_LIMITED;
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

#ifdef SUNSHINE_NATIVE_CLIENT
  /**
   * @brief Publish a host rumble request for the SDL event thread.
   * @param slot Host controller slot.
   * @param first First motor intensity.
   * @param second Second motor intensity.
   * @tparam Triggers Whether the request targets trigger motors.
   */
  template<bool Triggers>
  void on_controller_rumble(std::uint16_t slot, std::uint16_t first, std::uint16_t second) {
    if (StreamConnectionState *state = active_connection_state(); state != nullptr && slot < state->rumble.size()) {
      auto &pending = Triggers ? state->triggerRumble : state->rumble;
      pending[slot].store((1ULL << 32) | (static_cast<std::uint64_t>(first) << 16) | second);
    }
  }

  /**
   * @brief Queue a request for accelerometer or gyro input on the event thread.
   * @param slot Host controller slot.
   * @param type Moonlight motion type.
   * @param rate Reports per second requested by the host.
   */
  void on_controller_motion(std::uint16_t slot, std::uint8_t type, std::uint16_t rate) {
    if (StreamConnectionState *state = active_connection_state(); state != nullptr && slot < state->motion.size() && (type == LI_MOTION_TYPE_ACCEL || type == LI_MOTION_TYPE_GYRO)) {
      state->motion[slot][type == LI_MOTION_TYPE_ACCEL ? 0 : 1].store((1U << 16) | rate);
    }
  }

  /**
   * @brief Queue a controller light update on the event thread.
   * @param slot Host controller slot.
   * @param red Red intensity.
   * @param green Green intensity.
   * @param blue Blue intensity.
   */
  void on_controller_led(std::uint16_t slot, std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    if (StreamConnectionState *state = active_connection_state(); state != nullptr && slot < state->leds.size()) {
      state->leds[slot].store((1U << 24) | (static_cast<std::uint32_t>(red) << 16) | (static_cast<std::uint32_t>(green) << 8) | blue);
    }
  }
#endif

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
    logging::log(connection_log_level(message), "moonlight-common-c", message);
  }

  /**
   * @brief Start the streaming transport and publish its result to the render thread.
   * @param startContext Transport configuration and shared connection state.
   * @return Zero after publishing the result, or -1 if the context is incomplete.
   */
  int run_stream_start_thread(StreamStartContext *startContext) {
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
    const bool rendered = ui::render_texture(renderer, texture, &source, &destination);
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

    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(TTF_RenderText_Blended_Wrapped(font, text.c_str(), 0, color, static_cast<Uint32>(bounds.w)), &SDL_DestroySurface);
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
    SDL_GetCurrentRenderOutputSize(resources->renderer, &width, &height);
    const bool wasVisible = resources->controller.has_overlay();
    if (resources->controller.pump(resources->mediaBackend, resources->bodyFont, width, height, connectionState->connectionStarted.load(), resources->statisticsEnabled)) {
      request_stream_stop(connectionState);
    }
#ifdef SUNSHINE_DESKTOP
    if (SDL_Window *window = SDL_GetRenderWindow(resources->renderer); window != nullptr) {
      const bool playing = connectionState->connectionStarted.load() && !connectionState->stopRequested.load() && !connectionState->connectionTerminated.load();
      const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
      const bool capture = input::should_capture_stream_mouse(playing, resources->controller.has_overlay(), focused);
      if (capture != resources->relativeMouseRequested) {
        resources->relativeMouseRequested = capture;
        set_stream_relative_mouse_mode(window, capture);
      }
    }
#endif
#ifdef SUNSHINE_NATIVE_CLIENT
    for (unsigned int slot = 0; slot < connectionState->rumble.size(); ++slot) {
      if (const auto effect = connectionState->rumble[slot].exchange(0); effect != 0) {
        resources->controller.rumble(slot, static_cast<std::uint16_t>(effect >> 16), static_cast<std::uint16_t>(effect), false);
      }
      if (const auto effect = connectionState->triggerRumble[slot].exchange(0); effect != 0) {
        resources->controller.rumble(slot, static_cast<std::uint16_t>(effect >> 16), static_cast<std::uint16_t>(effect), true);
      }
      if (const auto color = connectionState->leds[slot].exchange(0); color != 0) {
        resources->controller.set_controller_led(slot, color);
      }
      for (unsigned int sensor = 0; sensor < connectionState->motion[slot].size(); ++sensor) {
        if (const auto rate = connectionState->motion[slot][sensor].exchange(0); rate != 0) {
          resources->controller.set_motion_reporting(slot, sensor == 0 ? LI_MOTION_TYPE_ACCEL : LI_MOTION_TYPE_GYRO, static_cast<std::uint16_t>(rate));
        }
      }
    }
#endif
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
    snapshot.presentationPath = mediaBackend.presentation().presentation_path();
    snapshot.decoderFeatures = mediaBackend.acceleration_report().cpuFeatures;
    return snapshot;
  }

  /**
   * @brief Sample telemetry at most once per second after the first frame has been presented.
   * @param context Active stream configuration.
   * @param connectionState Current connection quality.
   * @param resources Renderer, cached font, telemetry sampler and video backend.
   * @return True when the video must be refreshed to display a new panel.
   */
  bool update_stream_statistics_overlay(const StreamStartContext &context, const StreamConnectionState &connectionState, StreamUiResources *resources) {
    if (resources == nullptr || resources->startupTick || resources->pendingVideoMode || resources->controller.has_overlay() || !resources->statisticsEnabled || resources->statisticsFont == nullptr || !resources->mediaBackend.has_decoded_video()) {
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
    SDL_GetCurrentRenderOutputSize(resources->renderer, &screenWidth, &screenHeight);
    constexpr int padding = 6;
    const int lineHeight = TTF_GetFontLineSkip(resources->statisticsFont) + 2;
    int width = 0;
    for (const auto &line : lines) {
      int lineWidth = 0;
      TTF_GetStringSize(resources->statisticsFont, line.c_str(), 0, &lineWidth, nullptr);
      width = std::max(width, lineWidth);
    }
    width = std::min(width + padding * 2, screenWidth - streaming::VideoOverlay::MARGIN * 2);
    const int height = std::min(static_cast<int>(lines.size()) * lineHeight + padding * 2, screenHeight - streaming::VideoOverlay::MARGIN * 2);
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> panel(SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888), SDL_DestroySurface);
    if (!panel) {
      logging::warn("stream", std::string("Unable to allocate the performance overlay: ") + SDL_GetError());
      resources->statisticsEnabled = false;
      return false;
    }
    SDL_FillSurfaceRect(panel.get(), nullptr, SDL_MapSurfaceRGB(panel.get(), 16, 16, 16));
    int top = padding;
    for (const auto &line : lines) {
      std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> text(TTF_RenderText_Blended(resources->statisticsFont, line.c_str(), 0, {255, 255, 255, 255}), SDL_DestroySurface);
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
    resources->mediaBackend.presentation().set_statistics_bitmap(width, height, std::move(grayscale));
    return true;
  }

  void request_idle_video_refresh_if_needed(const streaming::FfmpegStreamBackend *mediaBackend, Uint64 *lastRequestTicks) {
    if (mediaBackend == nullptr || lastRequestTicks == nullptr || !mediaBackend->has_decoded_video()) {
      return;
    }

    const std::uint64_t idleMilliseconds = mediaBackend->milliseconds_since_last_decoded_video_frame(PltGetMicroseconds());
    if (idleMilliseconds < STREAM_VIDEO_IDLE_IDR_MILLISECONDS) {
      return;
    }
    const Uint64 nowTicks = SDL_GetTicks();
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
#if defined(NXDK) && SUNSHINE_INPUT_DIAGNOSTICS
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
   * @brief Paint a centered startup card whose layout stays fixed as progress changes.
   * @param renderer Target for the current stage of the renderer handoff.
   * @param resources Session fonts.
   * @param dialog Modal content with an optional startup progress step.
   * @param screenWidth Output width in pixels.
   * @param screenHeight Output height in pixels.
   * @param footer Optional stream-menu hint.
   * @return True when the dialog was drawn.
   */
  bool draw_stream_start_dialog(SDL_Renderer *renderer, const StreamUiResources &resources, const ui::ShellModalView &dialog, int screenWidth, int screenHeight, const std::string &footer = {}) {
    const auto [card, title, body, progress, hint] = ui::calculate_stream_startup_layout(screenWidth, screenHeight, TTF_GetFontLineSkip(resources.titleFont), TTF_GetFontLineSkip(resources.bodyFont));
    if (card.w == 0) {
      return false;
    }

    std::string bodyText;
    for (const auto &line : dialog.lines) {
      if (!bodyText.empty()) {
        bodyText += '\n';
      }
      bodyText += line;
    }
    constexpr SDL_Color text = ui::brand::TEXT;
    const auto rasterizeBody = [&bodyText, text, wrapWidth = static_cast<Uint32>(body.w)](TTF_Font *font) {
      return std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
        TTF_RenderText_Blended_Wrapped(font, bodyText.c_str(), 0, text, wrapWidth),
        SDL_DestroySurface
      );
    };
    auto raster = rasterizeBody(resources.bodyFont);
    if (raster && raster->h > body.h && resources.statisticsFont != nullptr) {
      if (auto compact = rasterizeBody(resources.statisticsFont); compact && compact->h < raster->h) {
        raster = std::move(compact);
      }
    }
    if (!raster) {
      return false;
    }

    SDL_SetRenderDrawColor(renderer, ui::brand::BACKGROUND.r, ui::brand::BACKGROUND.g, ui::brand::BACKGROUND.b, ui::brand::BACKGROUND.a);
    SDL_RenderClear(renderer);
    ui::draw_dialog_surface(renderer, {card.x, card.y, card.w, card.h});
    if (!render_text_line(renderer, resources.titleFont, dialog.title, {title.x, title.y, title.w, title.h}, text) || !render_text_surface(renderer, raster.get(), {body.x, body.y, body.w, body.h})) {
      return false;
    }
    if (dialog.streamStartupStep.has_value()) {
      ui::draw_dialog_progress(renderer, {progress.x, progress.y, progress.w, progress.h}, static_cast<int>(*dialog.streamStartupStep), ui::STREAM_STARTUP_STEP_COUNT);
    }
    return footer.empty() || render_text_line(renderer, resources.bodyFont, footer, {hint.x, hint.y, hint.w, hint.h}, ui::brand::ACCENT);
  }

#ifdef NXDK
  /**
   * @brief Paint the startup card onto the CPU framebuffer before the shell releases pbkit.
   * @param resources Session fonts shared by subsequent startup frames.
   * @param dialog Current startup progress.
   */
  void stage_stream_start_dialog(const StreamUiResources &resources, const ui::ShellModalView &dialog) {
    const VIDEO_MODE mode = XVideoGetMode();
    void *framebuffer = XVideoGetFB();
    if (framebuffer == nullptr || (mode.bpp != 15 && mode.bpp != 16 && mode.bpp != 32)) {
      return;
    }
    const auto format = mode.bpp == 32 ? SDL_PIXELFORMAT_XRGB8888 : (mode.bpp == 16 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_XRGB1555);
    const int pitch = mode.width * SDL_BYTESPERPIXEL(format);
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(SDL_CreateSurfaceFrom(mode.width, mode.height, format, framebuffer, pitch), SDL_DestroySurface);
    if (!surface) {
      return;
    }
    const std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer);
    if (renderer && draw_stream_start_dialog(renderer.get(), resources, dialog, mode.width, mode.height)) {
      // The standalone surface renderer needs no window resize or pbkit ownership.
      SDL_RenderPresent(renderer.get());
      XVideoFlushFB();
    }
  }
#endif

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
    SDL_GetCurrentRenderOutputSize(resources->renderer, &screenWidth, &screenHeight);

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
    draw_stream_start_dialog(resources->renderer, *resources, dialog, screenWidth, screenHeight, input::STREAM_EXIT_HINT);

    if (resources->controller.has_overlay() && mediaBackend != nullptr) {
      mediaBackend->presentation().render_local_overlay(resources->renderer, screenWidth, screenHeight);
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
    const SDL_ThreadFunction startEntry = [](auto context) {
      return run_stream_start_thread(static_cast<StreamStartContext *>(context));
    };
    SDL_Thread *startThread = SDL_CreateThread(startEntry, "start-stream", &attempt.startContext);
    if (startThread != nullptr) {
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

  /**
   * @brief Check whether negotiation ended in RTSP before playback and without a local cancellation.
   * @param connectionState Completed connection attempt.
   * @return True when a retry is permissible.
   */
  bool can_retry_rtsp_handshake(const StreamConnectionState &connectionState) {
    return !connectionState.stopRequested.load() && !connectionState.connectionStarted.load() && connectionState.failedStage.load() == STAGE_RTSP_HANDSHAKE;
  }

  /**
   * @brief Start transport with one retry per applicable RTSP fallback.
   *
   * A failed preferred MPEG-2 handshake can retry with standard codec negotiation.
   *
   * @param attempt Stream startup state and UI resources.
   * @param videoCodec User's requested codec selection.
   * @param rtspFallbackAttempted Receives whether default RTSP discovery was attempted.
   * @return True when the transport thread ran, including connection failures reported in the state.
   */
  bool run_stream_start_with_rtsp_fallback(const StreamStartAttemptContext &attempt, app::VideoCodecSelection videoCodec, bool *rtspFallbackAttempted) {
    if (rtspFallbackAttempted != nullptr) {
      *rtspFallbackAttempted = false;
    }
    if (!run_stream_start_attempt(attempt)) {
      return false;
    }
    if (auto &videoFormats = attempt.startContext.streamConfiguration.supportedVideoFormats; videoCodec == app::VideoCodecSelection::preferMpeg2 && videoFormats == VIDEO_FORMAT_MPEG2 && can_retry_rtsp_handshake(attempt.connectionState)) {
      logging::warn("stream", "MPEG-2 handshake failed; retrying with standard codec negotiation");
      attempt.resources.mediaBackend.shutdown();
      videoFormats = streaming::supported_video_formats(app::VideoCodecSelection::autoDetect, attempt.startContext.serverInformation.serverCodecModeSupport);
      reset_connection_state(&attempt.connectionState);
      if (!run_stream_start_attempt(attempt)) {
        return false;
      }
    }
    if (!can_retry_rtsp_handshake(attempt.connectionState) || attempt.startContext.rtspSessionUrl.empty()) {
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
   * @brief Switch directly from the progressive startup card to interlaced video.
   * @param loopContext Active stream resources and display mode.
   * @return New output height when the mode changed, or zero when no resize is needed.
   */
  int activate_pending_stream_video_mode(const ActiveStreamLoopContext &loopContext) {
    auto &resources = loopContext.resources;
    const int previousHeight = loopContext.videoMode.active_video_mode().height;
#ifdef NXDK
    loopContext.videoMode.switch_to(*resources.pendingVideoMode);
#else
    ScopedStreamVideoMode::switch_to(*resources.pendingVideoMode);
#endif
    resources.pendingVideoMode.reset();
    const int outputHeight = loopContext.videoMode.active_video_mode().height;
    if (outputHeight == previousHeight) {
      return 0;
    }

    // xemu's VGA scanout can show only one field of a 1080i CPU framebuffer
    // before PVIDEO takes over. Keep the startup card in the progressive mode.
    resources.controller.invalidate_panel(resources.mediaBackend);
    return outputHeight;
  }

  /**
   * @brief Resize the overlay fonts in place after the display handoff.
   * @param resources Cached fonts owned by the stream.
   * @param outputHeight Height of the new scanout in pixels.
   */
  void resize_stream_fonts(StreamUiResources &resources, int outputHeight) {
    const auto resizeFont = [](TTF_Font *font, int size) {
      if (font != nullptr && !TTF_SetFontSize(font, static_cast<float>(size))) {
        logging::warn("stream", std::string("Unable to resize a streaming font: ") + SDL_GetError());
      }
    };
    resizeFont(resources.titleFont, std::max(22, outputHeight / 18));
    resizeFont(resources.bodyFont, ui::control_hint_font_size(outputHeight / streaming::controls_raster_scale(outputHeight)));
    resizeFont(resources.statisticsFont, std::max(12, outputHeight / 45));
  }

  /**
   * @brief Present a pending frame and finish any UI work deferred across the mode switch.
   * @param loopContext Active stream resources and display mode.
   * @param lastIdleVideoIdrRequestTick Last idle refresh request, updated when a new one is sent.
   */
  void render_active_stream_frame_if_needed(const ActiveStreamLoopContext &loopContext, Uint64 *lastIdleVideoIdrRequestTick) {
    const bool hasDecodedVideo = loopContext.resources.mediaBackend.has_decoded_video();
    int newOutputHeight = 0;
    if (hasDecodedVideo && loopContext.resources.pendingVideoMode) {
      newOutputHeight = activate_pending_stream_video_mode(loopContext);
    }
    const bool statisticsChanged = update_stream_statistics_overlay(loopContext.startContext, loopContext.connectionState, &loopContext.resources);
    loopContext.resources.mediaBackend.presentation().update_save_indicator(SDL_GetTicks());
    if (const bool shouldRenderFrame = !hasDecodedVideo || statisticsChanged || loopContext.resources.mediaBackend.presentation().has_pending_overlay() || loopContext.resources.mediaBackend.has_unrendered_video_frame(); shouldRenderFrame) {
      render_stream_frame(
        loopContext.host,
        loopContext.app,
        loopContext.startContext,
        loopContext.connectionState,
        &loopContext.resources.mediaBackend,
        &loopContext.resources
      );
    }
    if (newOutputHeight > 0) {
      resize_stream_fonts(loopContext.resources, newOutputHeight);
    }
    request_idle_video_refresh_if_needed(&loopContext.resources.mediaBackend, lastIdleVideoIdrRequestTick);
    SDL_Delay(hasDecodedVideo ? STREAM_PRESENT_POLL_MILLISECONDS : STREAM_FRAME_DELAY_MILLISECONDS);
  }

  /**
   * @brief Pump input and render until transport termination or confirmed local quit.
   * @param loopContext Active stream resources.
   */
  void run_active_stream_loop(const ActiveStreamLoopContext &loopContext) {
    Uint64 lastIdleVideoIdrRequestTick = 0U;
    while (!loopContext.connectionState.connectionTerminated.load() && !loopContext.connectionState.stopRequested.load()) {
      pump_stream_events(&loopContext.resources, &loopContext.connectionState);
      if (loopContext.connectionState.stopRequested.load()) {
        break;
      }
      render_active_stream_frame_if_needed(loopContext, &lastIdleVideoIdrRequestTick);
    }
#ifdef SUNSHINE_DESKTOP
    release_stream_mouse_capture(loopContext.resources);
#endif
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

  /**
   * @brief Stop transport, collect the final report and return the controller to the shell.
   * @param target Active host, application and credentials.
   * @param settings Receives changes to keyboard placement.
   * @param startContext Negotiated connection parameters.
   * @param connectionState Recorded termination state.
   * @param resources Active stream resources, released before returning.
   * @param report Receives the final status, metrics and retained controller.
   */
  void finish_stream_session(const streaming::StreamSessionTarget &target, app::SettingsState &settings, const StreamStartContext &startContext, const StreamConnectionState &connectionState, StreamUiResources &resources, streaming::StreamSessionReport &report) {
    const auto &[host, app, clientIdentity] = target;
    streaming::StreamStatisticsSnapshot finalStatistics = sample_stream_statistics(startContext, connectionState, resources.mediaBackend);
    resources.statisticsSampler.apply(&finalStatistics);
    if (finalStatistics.interval.decodeTimeMs >= 0) {
      finalStatistics.decoderLatencyMs = static_cast<int>(finalStatistics.interval.decodeTimeMs + 0.5);
    }
    if (finalStatistics.interval.hostProcessingMs >= 0) {
      finalStatistics.hostLatencyMs = static_cast<int>(finalStatistics.interval.hostProcessingMs + 0.5);
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
#ifdef NXDK
    startup::log_memory_statistics();
#endif
    if (settings.controls.keyboardDockTop != resources.controller.keyboard_docked_top()) {
      settings.controls.keyboardDockTop = resources.controller.keyboard_docked_top();
      settings.dirty = true;
    }
    report.summaryLines = build_stream_summary_lines(host, app, startContext, finalStatistics, finalMessage);
    report.controller = resources.controller.take_controller_for_shell();
    close_stream_ui_resources(&resources);
    report.message = std::move(finalMessage);
  }

}  // namespace

namespace streaming {

  bool run_stream_session(
    SDL_Window *window,
    const VIDEO_MODE &videoMode,
    app::SettingsState &settings,
    const StreamSessionTarget &target,
    SDL_Gamepad *preferredController,
    const std::function<void()> &releaseShell,
    StreamSessionReport &report
  ) {
    const auto &[host, app, clientIdentity] = target;
    const Uint64 sessionStarted = SDL_GetTicks();
    report = {};
    const ResolvedStreamParameters streamParameters = resolve_stream_parameters(videoMode, settings);
    const VIDEO_MODE streamPresentationVideoMode = startup::choose_stream_output_mode(streamParameters.videoMode, videoMode);
    const VIDEO_MODE startupVideoMode = startup::choose_stream_startup_mode(streamPresentationVideoMode, videoMode);

    StreamUiResources resources {};
    if (!video_modes_match(startupVideoMode, streamPresentationVideoMode)) {
      resources.pendingVideoMode = streamPresentationVideoMode;
    }
    resources.startupTick = sessionStarted;
    resources.statisticsEnabled = settings.video.showPerformanceStats;
    std::string initializationError;
    const bool fontsReady = initialize_stream_fonts(startupVideoMode, resources, initializationError);
#ifdef NXDK
    if (fontsReady) {
      stage_stream_start_dialog(resources, ui::build_stream_launch_dialog(host, app));
    }
#endif
    releaseShell();

    ScopedStreamVideoMode scopedStreamVideoMode(window, videoMode, startupVideoMode);
    const auto &activeStreamVideoMode = scopedStreamVideoMode.active_video_mode();
    resources.renderer = fontsReady ? create_stream_renderer(window, initializationError) : nullptr;
    if (resources.renderer == nullptr) {
      close_stream_ui_resources(&resources);
      report.message = initializationError;
      logging::error("stream", initializationError);
      return false;
    }

    enable_stream_controller_events();
    resources.controller.reset(open_primary_controller(preferredController));
    resources.controller.configure(settings.controls.screenshotDirectory, settings.controls.keyboardDockTop, app::screenshot_source(host, app));
    ScopedThreadPriority streamThreadPriority(SDL_THREAD_PRIORITY_HIGH);

#ifdef NXDK
    startup::log_memory_statistics();
#endif

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
    SDL_GetCurrentRenderOutputSize(resources.renderer, &screenWidth, &screenHeight);
    draw_stream_start_dialog(resources.renderer, resources, ui::build_stream_launch_dialog(host, app, ui::StreamStartupStep::starting_on_host), screenWidth, screenHeight);
    SDL_RenderPresent(resources.renderer);

    std::string launchError;
    const Uint64 hostRequestStarted = SDL_GetTicks();
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
      std::string("Requested stream configuration | resolution=") + std::to_string(startContext.streamConfiguration.width) + "x" + std::to_string(startContext.streamConfiguration.height) + " | fps=" + std::to_string(startContext.streamConfiguration.fps) + " | bitrate=" + std::to_string(startContext.streamConfiguration.bitrate) + " kbps | decoder=" + describe_video_codec_selection(settings.video.codec) + " | packetSize=" + std::to_string(startContext.streamConfiguration.packetSize) + " | networkProfile=" + describe_streaming_remotely_mode(startContext.streamConfiguration.streamingRemotely) + " | clientRefreshX100=" + std::to_string(startContext.streamConfiguration.clientRefreshRateX100)
    );
    resources.mediaBackend.audio_player().configure(settings.audio.mode, settings.audio.playOnXbox);
    resources.mediaBackend.presentation().configure_gpu_presentation(settings.video.gpuPresentation, settings.video.gpuBufferCount);
    logging::info("stream", std::string("Local audio mode: ") + (settings.audio.playOnXbox ? audio_mode_label(settings.audio.mode) : "Disabled"));
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
#ifdef SUNSHINE_NATIVE_CLIENT
    startContext.connectionCallbacks.rumble = on_controller_rumble<false>;
    startContext.connectionCallbacks.rumbleTriggers = on_controller_rumble<true>;
    startContext.connectionCallbacks.setMotionEventState = on_controller_motion;
    startContext.connectionCallbacks.setControllerLED = on_controller_led;
#endif
    startContext.connectionCallbacks.stageComplete = on_stage_complete;
    startContext.connectionCallbacks.stageFailed = on_stage_failed;
    startContext.connectionCallbacks.connectionStarted = on_connection_started;
    startContext.connectionCallbacks.connectionTerminated = on_connection_terminated;
    startContext.connectionCallbacks.connectionStatusUpdate = on_connection_status_update;
    startContext.connectionCallbacks.logMessage = on_log_message;

    bool rtspFallbackAttempted = false;
    if (const StreamStartAttemptContext startAttempt {host, app, startContext, connectionState, resources, report.message}; !run_stream_start_with_rtsp_fallback(startAttempt, settings.video.codec, &rtspFallbackAttempted)) {
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

    finish_stream_session(target, settings, startContext, connectionState, resources, report);
    return true;
  }

}  // namespace streaming
