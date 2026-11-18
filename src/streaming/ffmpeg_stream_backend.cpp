/**
 * @file src/streaming/ffmpeg_stream_backend.cpp
 * @brief Implements the FFmpeg-backed streaming decode backend for Xbox sessions.
 */
// class header include
#include "src/streaming/ffmpeg_stream_backend.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// lib includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/cpu.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

// local includes
#include "src/logging/logger.h"
#include "src/platform/filesystem_utils.h"

// nxdk includes
#ifdef NXDK
  #include <hal/video.h>
#endif

/**
 * @brief Return the platform monotonic clock value in microseconds.
 *
 * @return Microseconds from the platform timer used by moonlight-common-c.
 */
extern "C" std::uint64_t PltGetMicroseconds(void);
static_assert(streaming::AlignedMediaBuffer::PADDING >= AV_INPUT_BUFFER_PADDING_SIZE, "Media buffers must retain FFmpeg's SIMD read padding");

namespace {

  /**
   * @brief Opaque callback context passed by moonlight-common-c and FFmpeg.
   */
  using MoonlightCallbackContext = void;

  constexpr std::uint64_t STREAM_VIDEO_SUBMISSION_LOG_INTERVAL = 120;
  constexpr int STREAM_VIDEO_DROPPED_FRAME_STATUS = -2;
  constexpr std::uint64_t STREAM_VIDEO_DROP_LOG_INTERVAL = 30;
  constexpr Uint32 STREAM_VIDEO_DECODE_YIELD_MILLISECONDS = 1U;

  /**
   * @brief Return the backend currently receiving video callbacks.
   *
   * @return Mutable callback backend slot.
   */
  streaming::FfmpegStreamBackend *&active_video_backend() {
    static streaming::FfmpegStreamBackend *backend = nullptr;
    return backend;
  }

  /**
   * @brief Return the backend currently receiving audio callbacks.
   *
   * @return Mutable callback backend slot.
   */
  streaming::FfmpegStreamBackend *&active_audio_backend() {
    static streaming::FfmpegStreamBackend *backend = nullptr;
    return backend;
  }

  /**
   * @brief Return the one-time FFmpeg logging setup guard.
   *
   * @return Shared initialization flag.
   */
  std::once_flag &ffmpeg_logging_once() {
    static std::once_flag once;
    return once;
  }

  /**
   * @brief Convert an FFmpeg error code into readable text.
   *
   * @param errorCode Negative FFmpeg return code.
   * @return User-readable FFmpeg error text.
   */
  std::string describe_ffmpeg_error(int errorCode) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer {};
    av_strerror(errorCode, buffer.data(), buffer.size());
    return std::string(buffer.data());
  }

  /**
   * @brief Remove trailing CR and LF characters from an FFmpeg log line.
   *
   * @param message Candidate log line.
   * @return Trimmed log line.
   */
  std::string trim_ffmpeg_log_line(std::string message) {
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
      message.pop_back();
    }
    return message;
  }

  /**
   * @brief Map one FFmpeg log level to the project logger severity.
   *
   * @param ffmpegLevel FFmpeg log level constant.
   * @return Corresponding project log level.
   */
  logging::LogLevel map_ffmpeg_log_level(int ffmpegLevel) {
    if (ffmpegLevel <= AV_LOG_ERROR) {
      return logging::LogLevel::error;
    }
    if (ffmpegLevel <= AV_LOG_WARNING) {
      return logging::LogLevel::warning;
    }
    if (ffmpegLevel <= AV_LOG_INFO) {
      return logging::LogLevel::info;
    }
    if (ffmpegLevel <= AV_LOG_VERBOSE) {
      return logging::LogLevel::debug;
    }
    return logging::LogLevel::trace;
  }

  /**
   * @brief Forward FFmpeg's internal logs into the project logger.
   *
   * @param avClassContext FFmpeg component instance emitting the line.
   * @param level FFmpeg log level.
   * @param format `printf`-style format string.
   * @param arguments Variadic arguments matching `format`.
   */
  void ffmpeg_log_callback(MoonlightCallbackContext *avClassContext, int level, const char *format, va_list arguments) {
    std::array<char, 1024> buffer {};
    int printPrefix = 1;
    va_list argumentsCopy;
    va_copy(argumentsCopy, arguments);
    const int formattedLength = av_log_format_line2(avClassContext, level, format, argumentsCopy, buffer.data(), static_cast<int>(buffer.size()), &printPrefix);
    va_end(argumentsCopy);
    if (formattedLength < 0) {
      return;
    }

    std::string message;
    if (static_cast<std::size_t>(formattedLength) < buffer.size()) {
      message.assign(buffer.data(), static_cast<std::size_t>(formattedLength));
    } else {
      message.assign(buffer.data());
    }
    message = trim_ffmpeg_log_line(std::move(message));
    if (message.empty()) {
      return;
    }
    if (message.find("No accelerated colorspace conversion found") != std::string::npos) {
      return;
    }

    logging::log(map_ffmpeg_log_level(level), "ffmpeg", std::move(message));
  }

  /**
   * @brief Install the shared FFmpeg log callback once for the process.
   */
  void ensure_ffmpeg_logging_installed() {
    std::call_once(ffmpeg_logging_once(), []() {
      av_log_set_level(AV_LOG_ERROR);
      av_log_set_callback(ffmpeg_log_callback);
    });
  }

  /**
   * @brief Compute a centered destination rectangle that preserves aspect ratio.
   *
   * @param screenWidth Renderer output width.
   * @param screenHeight Renderer output height.
   * @param frameWidth Decoded video width.
   * @param frameHeight Decoded video height.
   * @return Letterboxed destination rectangle.
   */
  SDL_Rect build_letterboxed_destination(int screenWidth, int screenHeight, int frameWidth, int frameHeight) {
    if (screenWidth <= 0 || screenHeight <= 0 || frameWidth <= 0 || frameHeight <= 0) {
      return SDL_Rect {0, 0, 0, 0};
    }

    const double screenAspect = static_cast<double>(screenWidth) / static_cast<double>(screenHeight);
    const double frameAspect = static_cast<double>(frameWidth) / static_cast<double>(frameHeight);

    int destinationWidth = screenWidth;
    int destinationHeight = screenHeight;
    if (frameAspect > screenAspect) {
      destinationHeight = std::max(1, static_cast<int>(static_cast<double>(screenWidth) / frameAspect));
    } else {
      destinationWidth = std::max(1, static_cast<int>(static_cast<double>(screenHeight) * frameAspect));
    }

    return SDL_Rect {
      (screenWidth - destinationWidth) / 2,
      (screenHeight - destinationHeight) / 2,
      destinationWidth,
      destinationHeight,
    };
  }

  int on_video_setup(int videoFormat, int width, int height, int redrawRate, MoonlightCallbackContext *context, int drFlags) {  // NOSONAR(cpp:S5008) moonlight-common-c requires a void* render context callback.
    auto *backend = static_cast<streaming::FfmpegStreamBackend *>(context);
    if (backend == nullptr) {
      return -1;
    }

    active_video_backend() = backend;
    return backend->setup_video_decoder(videoFormat, width, height, redrawRate, drFlags);
  }

  void on_video_start() {
    if (streaming::FfmpegStreamBackend *backend = active_video_backend(); backend != nullptr) {
      backend->start_video_decoder();
    }
  }

  void on_video_stop() {
    if (streaming::FfmpegStreamBackend *backend = active_video_backend(); backend != nullptr) {
      backend->stop_video_decoder();
    }
  }

  void on_video_cleanup() {
    if (streaming::FfmpegStreamBackend *backend = active_video_backend(); backend != nullptr) {
      backend->cleanup_video_decoder();
      active_video_backend() = nullptr;
    }
  }

  /**
   * @brief Prepare the active Opus decoder from the negotiated multistream configuration.
   * @param audioConfiguration Negotiated Moonlight layout.
   * @param opusConfig Negotiated Opus channels and mappings.
   * @param context Stream backend supplied by Moonlight.
   * @return Zero when playback can start or has been disabled.
   */
  int on_audio_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, MoonlightCallbackContext *context, int) {  // NOSONAR(cpp:S5008,cpp:S995) moonlight-common-c requires this callback signature.
    auto *backend = static_cast<streaming::FfmpegStreamBackend *>(context);
    if (backend == nullptr) {
      return -1;
    }

    const int result = backend->audio_player().initialize(audioConfiguration, opusConfig);
    if (result == 0) {
      active_audio_backend() = backend;
    }
    return result;
  }

  /** @brief Start the active SDL audio queue. */
  void on_audio_start() {
    if (streaming::FfmpegStreamBackend *backend = active_audio_backend(); backend != nullptr) {
      backend->audio_player().start();
    }
  }

  /** @brief Pause the active SDL audio queue. */
  void on_audio_stop() {
    if (streaming::FfmpegStreamBackend *backend = active_audio_backend(); backend != nullptr) {
      backend->audio_player().stop();
    }
  }

  /** @brief Release the active audio player after the transport audio thread exits. */
  void on_audio_cleanup() {
    if (streaming::FfmpegStreamBackend *backend = active_audio_backend(); backend != nullptr) {
      backend->audio_player().cleanup();
      active_audio_backend() = nullptr;
    }
  }

  /**
   * @brief Decode and queue the next Moonlight Opus payload.
   * @param sampleData Encoded payload, or null for a lost packet.
   * @param sampleLength Payload length in bytes.
   */
  void on_audio_decode_and_play_sample(char *sampleData, int sampleLength) {  // NOSONAR(cpp:S995) moonlight-common-c supplies mutable sample data by callback signature.
    if (streaming::FfmpegStreamBackend *backend = active_audio_backend(); backend != nullptr) {
      backend->audio_player().decode_and_queue(sampleData, sampleLength);
    }
  }

  /**
   * @brief Track actual completed BMP writes while retaining SDL's platform file implementation.
   */
  struct ScreenshotFileProgress {
    SDL_RWops *file;  ///< Worker-owned file, closed after SDL finalizes the BMP header.
    std::atomic<int> &progress;  ///< UI-readable percentage independent of file and surface lifetimes.
    Sint64 expected;  ///< Final 24-bit BMP length, including scanline padding and header.
    Sint64 position = 0;  ///< Current file position, including SDL's header fixup seeks.
    Sint64 written = 0;  ///< Highest written position, preventing header fixups from reducing progress.
  };

}  // namespace

// SDL file callbacks use C linkage; project-prefixed names avoid global symbol collisions.
/**
 * @brief Delegate a BMP file seek while keeping progress accounting in sync.
 * @param stream Progress-decorated SDL file.
 * @param offset Byte offset relative to whence.
 * @param whence SDL seek origin.
 * @return New byte position or a negative value on error.
 */
extern "C" Sint64 moonlight_screenshot_file_seek(SDL_RWops *stream, Sint64 offset, int whence) {
  auto &writer = *static_cast<ScreenshotFileProgress *>(stream->hidden.unknown.data1);
  writer.position = SDL_RWseek(writer.file, offset, whence);
  return writer.position;
}

/**
 * @brief Forward one BMP write and publish progress only for bytes actually written.
 * @param stream Progress-decorated SDL file.
 * @param pixels Header or pixel bytes supplied by the SDL C writer callback.
 * @param size Bytes per object.
 * @param count Requested object count.
 * @return Number of complete objects written.
 */
extern "C" std::size_t moonlight_screenshot_file_write(SDL_RWops *stream, const void *pixels, std::size_t size, std::size_t count) {
  auto &writer = *static_cast<ScreenshotFileProgress *>(stream->hidden.unknown.data1);
  const auto written = SDL_RWwrite(writer.file, pixels, size, count);
  writer.position += static_cast<Sint64>(written * size);
  writer.written = std::max(writer.written, writer.position);
  writer.progress.store(10 + static_cast<int>(85 * std::min(writer.written, writer.expected) / writer.expected));
  return written;
}

namespace streaming {

  VideoAccelerationReport FfmpegStreamBackend::acceleration_report() const {
    return acceleration_;
  }

  VideoStatisticsCounters FfmpegStreamBackend::performance_counters() const {
    return statistics_.counters();
  }

  std::string FfmpegStreamBackend::presentation_path() const {
    return presenter_.active() ? "GPU" : (acceleration_.softwareFrames > 0 ? "Software" : "Waiting");
  }

  void FfmpegStreamBackend::set_statistics_bitmap(int width, int height, std::vector<std::uint8_t> grayscale, VideoOverlayPlacement placement, std::vector<std::uint8_t> opacity) {
    statisticsOverlay_.set_bitmap(width, height, std::move(grayscale), placement, std::move(opacity));
    invalidate_overlay_texture();
  }

  void FfmpegStreamBackend::set_color_overlay(int width, int height, std::vector<std::uint8_t> pixels, VideoOverlayPlacement placement) {
    statisticsOverlay_.set_argb_bitmap(width, height, std::move(pixels), placement);
    invalidate_overlay_texture();
  }

  void FfmpegStreamBackend::set_color_overlay_surface(int width, int height, const std::uint8_t *pixels, std::size_t capacity, int pitch, VideoOverlayPlacement placement, bool opaqueBackground) {
    if (statisticsOverlay_.set_argb_surface(width, height, pixels, capacity, pitch, placement, opaqueBackground)) {
      invalidate_overlay_texture();
    }
  }

  void FfmpegStreamBackend::invalidate_overlay_texture() {
    statisticsBitmapChanged_ = true;
    statisticsTextureChanged_ = true;
    statisticsTextureDamage_ = {0, 0, statisticsOverlay_.width(), statisticsOverlay_.height()};
    if (statisticsTexture_ != nullptr) {
      int width = 0;
      int height = 0;
      if (SDL_QueryTexture(statisticsTexture_, nullptr, nullptr, &width, &height) != 0 || width != statisticsOverlay_.width() || height != statisticsOverlay_.height()) {
        SDL_DestroyTexture(statisticsTexture_);
        statisticsTexture_ = nullptr;
      }
    }
  }

  void FfmpegStreamBackend::update_color_overlay(const VideoRectangle &region, const std::vector<std::uint8_t> &pixels) {
    if (!statisticsOverlay_.update_argb_bitmap(region, pixels)) {
      return;
    }
    if (statisticsTextureChanged_) {
      const int right = std::max(statisticsTextureDamage_.x + statisticsTextureDamage_.width, region.x + region.width);
      const int bottom = std::max(statisticsTextureDamage_.y + statisticsTextureDamage_.height, region.y + region.height);
      statisticsTextureDamage_.x = std::min(statisticsTextureDamage_.x, region.x);
      statisticsTextureDamage_.y = std::min(statisticsTextureDamage_.y, region.y);
      statisticsTextureDamage_.width = right - statisticsTextureDamage_.x;
      statisticsTextureDamage_.height = bottom - statisticsTextureDamage_.y;
    } else {
      statisticsTextureDamage_ = region;
    }
    statisticsBitmapChanged_ = true;
    statisticsTextureChanged_ = true;
  }

  bool FfmpegStreamBackend::has_pending_overlay() const {
    return statisticsBitmapChanged_;
  }

  void FfmpegStreamBackend::render_local_overlay(SDL_Renderer *renderer, int width, int height) {
    render_sdl_statistics(renderer, {0, 0, width, height});
  }

  void FfmpegStreamBackend::local_overlay_size(int &width, int &height) const {
    const auto &frame = video_.renderFrame.view();
    if (frame.width > 0 && frame.height > 0) {
      const auto destination = build_letterboxed_destination(width, height, frame.width, frame.height);
      width = destination.w;
      height = destination.h;
    }
  }

  bool FfmpegStreamBackend::request_screenshot(std::string_view directory, const app::ScreenshotSource &source) {
    if (const auto result = take_screenshot_result(); !result.empty()) {
      logging::info("screenshots", result);
    }
    if (screenshot_.thread != nullptr || video_.renderFrame.view().width <= 0) {
      logging::warn("screenshots", "Capture not queued: another screenshot is pending or no video frame is available");
      return false;
    }
    if (!screenshot_.frame.retain(video_.renderFrame)) {
      logging::warn("screenshots", "Capture not queued: could not retain the decoded frame");
      return false;
    }
    screenshot_.source = source;
    screenshot_.directory = directory;
    screenshot_.capturedAt = static_cast<std::int64_t>(std::time(nullptr));
    screenshot_.result.clear();
    screenshot_.progress.store(0);
    screenshot_.complete.store(false);
    screenshot_.thread = SDL_CreateThread([](auto *context) {
      return save_screenshot_worker(static_cast<ScreenshotTask *>(context));
    },
                                          "save-screenshot",
                                          &screenshot_);
    if (screenshot_.thread == nullptr) {
      screenshot_.frame.clear();
      logging::warn("screenshots", std::string("Cannot start screenshot worker: ") + SDL_GetError());
    }
    return screenshot_.thread != nullptr;
  }

  std::string FfmpegStreamBackend::take_screenshot_result() {
    if (screenshot_.thread == nullptr || !screenshot_.complete.load()) {
      return {};
    }
    SDL_WaitThread(screenshot_.thread, nullptr);
    screenshot_.thread = nullptr;
    return std::exchange(screenshot_.result, {});
  }

  int FfmpegStreamBackend::screenshot_progress() const {
    return screenshot_.thread != nullptr ? screenshot_.progress.load() : -1;
  }

  /**
   * @brief Encode a BMP with byte-based progress, closing the file on every outcome.
   * @param surface Converted BGR screenshot.
   * @param path Destination filename.
   * @param progress UI-readable completed percentage.
   * @return True after both encoding and file finalization succeed.
   */
  bool save_screenshot_bitmap(SDL_Surface &surface, const std::string &path, std::atomic<int> &progress) {
    SDL_RWops *file = SDL_RWFromFile(path.c_str(), "wb");
    if (file == nullptr) {
      return false;
    }
    ScreenshotFileProgress writer {file, progress, 54 + static_cast<Sint64>((surface.w * 3 + 3) & ~3) * surface.h};
    SDL_RWops progressFile {};
    progressFile.hidden.unknown.data1 = &writer;
    progressFile.seek = moonlight_screenshot_file_seek;
    progressFile.write = moonlight_screenshot_file_write;
    const bool encoded = SDL_SaveBMP_RW(&surface, &progressFile, 0) == 0;
    const bool closed = SDL_RWclose(file) == 0;
    const bool saved = encoded && closed;
    if (!saved) {
      std::remove(path.c_str());
    }
    return saved;
  }

  SDL_Surface *FfmpegStreamBackend::convert_screenshot_frame(const Yuv420FrameView &frame) {
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, frame.width, frame.height, 24, SDL_PIXELFORMAT_BGR24);
    if (surface == nullptr) {
      return nullptr;
    }
    SwsContext *conversion = sws_getContext(frame.width, frame.height, AV_PIX_FMT_YUV420P, frame.width, frame.height, AV_PIX_FMT_BGR24, SWS_POINT, nullptr, nullptr, nullptr);
    if (conversion == nullptr) {
      SDL_FreeSurface(surface);
      return nullptr;
    }
    const int *coefficients = sws_getCoefficients(frame.matrix == VideoColorMatrix::bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
    sws_setColorspaceDetails(conversion, coefficients, frame.fullRange, coefficients, 1, 0, 1 << 16, 1 << 16);
    const std::array<const std::uint8_t *, 4> pixels {frame.planes[0], frame.planes[1], frame.planes[2], nullptr};
    const std::array<int, 4> pitch {frame.pitches[0], frame.pitches[1], frame.pitches[2], 0};
    std::array<std::uint8_t *, 4> output {static_cast<std::uint8_t *>(surface->pixels), nullptr, nullptr, nullptr};
    const std::array<int, 4> outputPitch {surface->pitch, 0, 0, 0};
    const bool converted = sws_scale(conversion, pixels.data(), pitch.data(), 0, frame.height, output.data(), outputPitch.data()) == frame.height;
    sws_freeContext(conversion);
    if (!converted) {
      SDL_FreeSurface(surface);
      return nullptr;
    }
    return surface;
  }

  int FfmpegStreamBackend::save_screenshot_worker(ScreenshotTask *context) {
    auto &job = *context;
    // Stay below the streaming event loop without starving behind USB/audio work.
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
    job.progress.store(1);
    std::string error;
    const std::string path = app::next_screenshot_path(job.directory, job.capturedAt, error);
    SDL_Surface *surface = path.empty() ? nullptr : convert_screenshot_frame(job.frame.view());
    // Release the YUV snapshot before disk I/O so full-screen menus still fit in 64 MiB.
    job.frame.clear();
    bool saved = false;
    if (surface != nullptr) {
      job.progress.store(10);
      saved = save_screenshot_bitmap(*surface, path, job.progress);
    }
    SDL_FreeSurface(surface);
    if (saved) {
      job.progress.store(99);
      job.result = app::append_screenshot({job.source, path, job.capturedAt}, app::screenshot_library_path(), error) ? "Saved " + path : "Saved " + path + "; could not update the screenshot library: " + error;
    } else {
      job.result = "Screenshot failed: " + (error.empty() ? std::string(SDL_GetError()) : error);
    }
    job.complete.store(true);
    return 0;
  }

  FfmpegStreamBackend::~FfmpegStreamBackend() {
    shutdown();
  }

  void FfmpegStreamBackend::initialize_callbacks(DECODER_RENDERER_CALLBACKS *videoCallbacks, AUDIO_RENDERER_CALLBACKS *audioCallbacks) const {
    ensure_ffmpeg_logging_installed();

    if (videoCallbacks != nullptr) {
      LiInitializeVideoCallbacks(videoCallbacks);
      videoCallbacks->setup = on_video_setup;
      videoCallbacks->start = on_video_start;
      videoCallbacks->stop = on_video_stop;
      videoCallbacks->cleanup = on_video_cleanup;
      videoCallbacks->submitDecodeUnit = nullptr;
      videoCallbacks->capabilities = CAPABILITY_PULL_RENDERER;
    }

    if (audioCallbacks != nullptr) {
      LiInitializeAudioCallbacks(audioCallbacks);
      audioCallbacks->init = on_audio_init;
      audioCallbacks->start = on_audio_start;
      audioCallbacks->stop = on_audio_stop;
      audioCallbacks->cleanup = on_audio_cleanup;
      audioCallbacks->decodeAndPlaySample = on_audio_decode_and_play_sample;
      audioCallbacks->capabilities = CAPABILITY_SLOW_OPUS_DECODER;
      if (!audio_.enabled()) {
        audioCallbacks->capabilities |= CAPABILITY_DIRECT_SUBMIT;
      }
    }
  }

  AudioPlayer &FfmpegStreamBackend::audio_player() {
    return audio_;
  }

  void FfmpegStreamBackend::set_gpu_presentation_enabled(bool enabled) {
    gpuPresentationEnabled_ = enabled;
  }

  void FfmpegStreamBackend::shutdown() {
    audio_.cleanup();
    cleanup_video_decoder();
    if (screenshot_.thread != nullptr) {
      SDL_WaitThread(screenshot_.thread, nullptr);
      screenshot_.thread = nullptr;
      logging::info("screenshots", screenshot_.result);
      screenshot_.frame.clear();
    }
  }

  bool FfmpegStreamBackend::has_decoded_video() const {
    return video_.hasFrame.load();
  }

  std::string FfmpegStreamBackend::active_video_decoder_name() const {
    return video_.codecName;
  }

  /**
   * @brief Return whether two SDL rectangles describe the same area.
   *
   * @param left First rectangle.
   * @param right Second rectangle.
   * @return True when every rectangle field matches.
   */
  bool rectangles_match(const SDL_Rect &left, const SDL_Rect &right) {
    return left.x == right.x && left.y == right.y && left.w == right.w && left.h == right.h;
  }

#ifdef NXDK
  /**
   * @brief Convert the active Xbox framebuffer bpp to FFmpeg's packed RGB format.
   *
   * @param bitsPerPixel Active framebuffer bits per pixel.
   * @return Matching FFmpeg pixel format, or none when unsupported.
   */
  AVPixelFormat xbox_framebuffer_pixel_format(int bitsPerPixel) {
    switch (bitsPerPixel) {
      case 15:
        return AV_PIX_FMT_RGB555LE;
      case 16:
        return AV_PIX_FMT_RGB565LE;
      case 32:
        return AV_PIX_FMT_BGR0;
      default:
        return AV_PIX_FMT_NONE;
    }
  }

  /**
   * @brief Return bytes per pixel for one Xbox framebuffer mode.
   *
   * @param bitsPerPixel Active framebuffer bits per pixel.
   * @return Bytes per pixel, or zero when unsupported.
   */
  int xbox_framebuffer_bytes_per_pixel(int bitsPerPixel) {
    switch (bitsPerPixel) {
      case 15:
      case 16:
        return 2;
      case 32:
        return 4;
      default:
        return 0;
    }
  }
#endif

  bool FfmpegStreamBackend::has_unrendered_video_frame() const {
    return video_.publishedFrameVersion.load() != video_.renderedFrameVersion;
  }

  std::uint64_t FfmpegStreamBackend::milliseconds_since_last_decoded_video_frame(std::uint64_t nowMicroseconds) const {
    const std::uint64_t lastDecodedFrameUs = video_.lastDecodedFrameUs.load();
    if (lastDecodedFrameUs == 0U || nowMicroseconds < lastDecodedFrameUs) {
      return 0U;
    }
    return (nowMicroseconds - lastDecodedFrameUs) / 1000U;
  }

  std::string FfmpegStreamBackend::build_overlay_status_line() const {
    return std::string("Video units: ") + std::to_string(video_.submittedDecodeUnitCount.load()) + " submitted / " + std::to_string(video_.decodedFrameCount.load()) + " decoded / " +
           std::to_string(video_.droppedDecodeUnitCount.load()) + " dropped | frame " + std::to_string(video_.lastDecodeFrameNumber.load()) + " queue " + std::to_string(video_.lastDecodeQueueUs.load() / 1000U) +
           " ms / age " + std::to_string(video_.lastReceiveAgeUs.load() / 1000U) + " ms | " + audio_.describe_playback();
  }

  bool FfmpegStreamBackend::render_latest_video_frame(SDL_Renderer *renderer, int screenWidth, int screenHeight, bool allowDirectFramebuffer) {
    const auto before = acceleration_.gpuFrames + acceleration_.softwareFrames;
    const auto started = PltGetMicroseconds();
    const bool result = render_video_frame(renderer, screenWidth, screenHeight, allowDirectFramebuffer);
    if (result) {
      statisticsBitmapChanged_ = false;
      if (acceleration_.gpuFrames + acceleration_.softwareFrames > before) {
        statistics_.presented(PltGetMicroseconds() - started);
      }
    }
    return result;
  }

  bool FfmpegStreamBackend::render_video_frame(SDL_Renderer *renderer, int screenWidth, int screenHeight, bool allowDirectFramebuffer) {
    if (renderer == nullptr || !video_.hasFrame.load()) {
      return false;
    }

    bool textureNeedsUpload = false;
    if (const std::uint64_t publishedFrameVersion = video_.publishedFrameVersion.load(); publishedFrameVersion != video_.renderedFrameVersion) {
      std::scoped_lock lock(video_.frame_mutex());
      if (video_.latestFrameVersion != video_.renderedFrameVersion && video_.latestFrame.view().width > 0) {
        if (video_.latestFrameVersion > video_.renderedFrameVersion + 1) {
          statistics_.dropped(video_.latestFrameVersion - video_.renderedFrameVersion - 1);
        }
        video_.renderFrame.swap(video_.latestFrame);
        video_.renderedFrameVersion = video_.latestFrameVersion;
        textureNeedsUpload = true;
      }
    }

    const auto &frame = video_.renderFrame.view();
    if (frame.width <= 0 || frame.height <= 0) {
      return false;
    }

#ifdef NXDK
    if (allowDirectFramebuffer && gpuPresentationEnabled_ && !overlayFailed_) {
      if (!textureNeedsUpload && !statisticsBitmapChanged_ && presenter_.active()) {
        return true;
      }
      const SDL_Rect destination = build_letterboxed_destination(screenWidth, screenHeight, frame.width, frame.height);
      if (!presenter_.active()) {
        const VIDEO_MODE mode = XVideoGetMode();
        if (auto *framebuffer = XVideoGetFB(); framebuffer != nullptr && mode.width > 0 && mode.height > 0 && (mode.bpp == 16 || mode.bpp == 32)) {
          std::memset(framebuffer, 0, static_cast<std::size_t>(mode.width) * static_cast<std::size_t>(mode.height) * static_cast<std::size_t>(mode.bpp / 8));
          XVideoFlushFB();
        }
      }
      std::string reason;
      if (presenter_.present(frame, {destination.x, destination.y, destination.w, destination.h}, &reason, {}, &statisticsOverlay_)) {
        if (textureNeedsUpload) {
          ++acceleration_.gpuFrames;
        }
        return true;
      }
      overlayFailed_ = true;
      acceleration_.record_fallback(reason);
      logging::warn("stream", "GPU presentation fallback: " + reason);
    }
    if (allowDirectFramebuffer && render_latest_video_frame_to_framebuffer(screenWidth, screenHeight)) {
      if (textureNeedsUpload) {
        ++acceleration_.softwareFrames;
      }
      return true;
    }
#else
    (void) allowDirectFramebuffer;
#endif

    if (video_.texture == nullptr || video_.textureWidth != frame.width || video_.textureHeight != frame.height) {
      if (video_.texture != nullptr) {
        SDL_DestroyTexture(video_.texture);
        video_.texture = nullptr;
      }

      video_.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
      if (video_.texture == nullptr) {
        logging::error("stream", std::string("SDL_CreateTexture failed for video presentation: ") + SDL_GetError());
        return false;
      }

      video_.textureWidth = frame.width;
      video_.textureHeight = frame.height;
      textureNeedsUpload = true;
    }

    if (textureNeedsUpload && SDL_UpdateYUVTexture(video_.texture, nullptr, frame.planes[0], frame.pitches[0], frame.planes[1], frame.pitches[1], frame.planes[2], frame.pitches[2]) != 0) {
      logging::error("stream", std::string("SDL_UpdateYUVTexture failed during video presentation: ") + SDL_GetError());
      return false;
    }

    const SDL_Rect destination = build_letterboxed_destination(screenWidth, screenHeight, frame.width, frame.height);
    const bool presented = SDL_RenderCopy(renderer, video_.texture, nullptr, &destination) == 0;
    if (presented) {
      render_sdl_statistics(renderer, destination);
    }
    if (presented && textureNeedsUpload) {
      ++acceleration_.softwareFrames;
    }
    return presented;
  }

  void FfmpegStreamBackend::render_sdl_statistics(SDL_Renderer *renderer, const SDL_Rect &destination) {
    const int width = statisticsOverlay_.width();
    const int height = statisticsOverlay_.height();
    if (width == 0 || height == 0) {
      return;
    }
    if (statisticsTexture_ == nullptr) {
      statisticsTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
      if (statisticsTexture_ != nullptr) {
        SDL_SetTextureBlendMode(statisticsTexture_, SDL_BLENDMODE_BLEND);
      }
      statisticsTextureChanged_ = true;
      statisticsTextureDamage_ = {0, 0, width, height};
    }
    if (statisticsTexture_ == nullptr) {
      return;
    }
    if (statisticsTextureChanged_) {
      const auto &pixels = statisticsOverlay_.argb_pixels();
      const SDL_Rect damage {statisticsTextureDamage_.x, statisticsTextureDamage_.y, statisticsTextureDamage_.width, statisticsTextureDamage_.height};
      if (const auto offset = (static_cast<std::size_t>(damage.y) * width + damage.x) * 4; SDL_UpdateTexture(statisticsTexture_, &damage, pixels.data() + offset, width * 4) != 0) {
        return;
      }
      statisticsTextureChanged_ = false;
    }
    const auto bounds = statisticsOverlay_.rectangle({destination.x, destination.y, destination.w, destination.h});
    const SDL_Rect panel {bounds.x, bounds.y, bounds.width, bounds.height};
    const SDL_Rect source {0, 0, bounds.width, bounds.height};
    SDL_RenderCopy(renderer, statisticsTexture_, &source, &panel);
  }

  int FfmpegStreamBackend::setup_video_decoder(int videoFormat, int width, int height, int redrawRate, int drFlags) {
    (void) width;
    (void) height;
    (void) redrawRate;
    (void) drFlags;

    cleanup_video_decoder();
    acceleration_ = {};
    acceleration_.gpuPresentationRequested = gpuPresentationEnabled_;
    statistics_.reset();
    overlayFailed_ = false;
    const int cpuFlags = av_get_cpu_flags();
    acceleration_.cpuFeatures = (cpuFlags & AV_CPU_FLAG_SSE) != 0 ? "MMX/SSE" : ((cpuFlags & AV_CPU_FLAG_MMX) != 0 ? "MMX" : "Scalar");
    acceleration_.threaded = std::string(avcodec_configuration()).find("--enable-w32threads") != std::string::npos;

    if ((videoFormat & VIDEO_FORMAT_MASK_H264) == 0) {
      logging::error("stream", "The FFmpeg backend currently supports only H.264 video streams");
      return -1;
    }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
      logging::error("stream", "FFmpeg did not provide an H.264 decoder");
      return -1;
    }

    video_.codecName = "H.264";
    video_.codecContext = avcodec_alloc_context3(codec);
    video_.decodedFrame = av_frame_alloc();
    video_.convertedFrame = av_frame_alloc();
    video_.packet = av_packet_alloc();
    if (video_.codecContext == nullptr || video_.decodedFrame == nullptr || video_.convertedFrame == nullptr || video_.packet == nullptr) {
      logging::error("stream", "Failed to allocate FFmpeg video decoder resources");
      cleanup_video_decoder();
      return -1;
    }

    // One decoder worker avoids frame-threading latency on the single-core CPU.
    // FFmpeg's w32threads support still provides its internal synchronization.
    video_.codecContext->thread_count = 1;
    video_.codecContext->thread_type = 0;
    video_.codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_OUTPUT_CORRUPT;
    video_.codecContext->flags2 |= AV_CODEC_FLAG2_FAST | AV_CODEC_FLAG2_SHOW_ALL;
    video_.codecContext->err_recognition = AV_EF_EXPLODE;
    video_.codecContext->skip_loop_filter = AVDISCARD_ALL;
    video_.codecContext->skip_idct = AVDISCARD_NONREF;
    if (const int openResult = avcodec_open2(video_.codecContext, codec, nullptr); openResult < 0) {
      logging::error("stream", std::string("avcodec_open2 failed for ") + video_.codecName + ": " + describe_ffmpeg_error(openResult));
      cleanup_video_decoder();
      return openResult;
    }

    logging::info("stream", std::string("Using FFmpeg ") + video_.codecName + " software decoding");
    return 0;
  }

  void FfmpegStreamBackend::start_video_decoder() {
    if (video_.decoderThread != nullptr) {
      return;
    }

    video_.decoderStopRequested.store(false);
    video_.decoderThread = SDL_CreateThread(run_video_decode_thread_trampoline, "stream-video", this);
    if (video_.decoderThread == nullptr) {
      logging::error("stream", std::string("Failed to start the prioritized video decoder thread: ") + SDL_GetError());
      video_.decoderStopRequested.store(true);
    }
  }

  void FfmpegStreamBackend::stop_video_decoder() {
    if (video_.decoderThread == nullptr) {
      return;
    }

    video_.decoderStopRequested.store(true);
    LiWakeWaitForVideoFrame();

    int threadResult = 0;
    SDL_WaitThread(video_.decoderThread, &threadResult);
    (void) threadResult;
    video_.decoderThread = nullptr;
  }

  void FfmpegStreamBackend::stop_video_presentation() {
    if (!presenter_.reset()) {
      acceleration_.record_fallback("GPU overlay teardown timed out; acceleration disabled");
      logging::warn("stream", "GPU overlay teardown timed out; buffers retained to avoid DMA into freed storage");
    }
  }

  void FfmpegStreamBackend::cleanup_video_decoder() {
    stop_video_decoder();
    stop_video_presentation();
    if (statisticsTexture_ != nullptr) {
      SDL_DestroyTexture(statisticsTexture_);
      statisticsTexture_ = nullptr;
    }
    statisticsOverlay_.set_bitmap(0, 0, {});

    if (video_.texture != nullptr) {
      SDL_DestroyTexture(video_.texture);
      video_.texture = nullptr;
    }
    video_.textureWidth = 0;
    video_.textureHeight = 0;

    if (video_.packet != nullptr) {
      av_packet_free(&video_.packet);
    }
    if (video_.decodedFrame != nullptr) {
      av_frame_free(&video_.decodedFrame);
    }
    if (video_.convertedFrame != nullptr) {
      av_frame_free(&video_.convertedFrame);
    }
    if (video_.codecContext != nullptr) {
      avcodec_free_context(&video_.codecContext);
    }
    if (video_.scaleContext != nullptr) {
      sws_freeContext(video_.scaleContext);
      video_.scaleContext = nullptr;
    }
    if (video_.presentScaleContext != nullptr) {
      sws_freeContext(video_.presentScaleContext);
      video_.presentScaleContext = nullptr;
    }

    video_.packetBuffer.clear();
    video_.renderFrame.clear();
    video_.decodeFrame.clear();
    {
      std::scoped_lock lock(video_.frame_mutex());
      video_.latestFrame.clear();
      video_.latestFrameVersion = 0;
    }
    video_.renderedFrameVersion = 0;
    video_.publishedFrameVersion.store(0);
    video_.codecName = "video";
    video_.directFramebufferDestination = SDL_Rect {0, 0, 0, 0};
    video_.directFramebufferCleared = false;
    video_.decoderStopRequested.store(false);
    video_.hasFrame.store(false);
    video_.submittedDecodeUnitCount.store(0);
    video_.decodedFrameCount.store(0);
    video_.droppedDecodeUnitCount.store(0);
    video_.lastDecodeQueueUs.store(0);
    video_.lastReceiveAgeUs.store(0);
    video_.lastDecodedFrameUs.store(0);
    video_.lastDecodeFrameNumber.store(0);
  }

  int FfmpegStreamBackend::run_video_decode_thread_trampoline(SdlThreadContext *context) {  // NOSONAR(cpp:S5008) SDL_CreateThread requires a void* trampoline context.
    auto *backend = static_cast<FfmpegStreamBackend *>(context);
    return backend != nullptr ? backend->run_video_decode_thread() : -1;
  }

  int FfmpegStreamBackend::run_video_decode_thread() {
    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) != 0) {
      logging::debug("stream", std::string("SDL_SetThreadPriority failed for video decoder: ") + SDL_GetError());
    }

    while (!video_.decoderStopRequested.load()) {
      VIDEO_FRAME_HANDLE frameHandle = nullptr;
      PDECODE_UNIT decodeUnit = nullptr;
      if (!LiWaitForNextVideoFrame(&frameHandle, &decodeUnit)) {
        break;
      }
      record_received_unit(decodeUnit);

      int decodeStatus = DR_OK;
      if (!video_.decoderStopRequested.load() && decodeUnit != nullptr) {
        decodeStatus = prepare_video_decode_unit(&frameHandle, &decodeUnit);
        if (decodeStatus == DR_OK) {
          const auto started = PltGetMicroseconds();
          decodeStatus = decode_video_decode_unit(decodeUnit);
          statistics_.decode_time(PltGetMicroseconds() - started, video_.lastDecodeQueueUs.load());
        }
        if (decodeStatus != DR_OK) {
          statistics_.dropped(1);
        }
      }
      LiCompleteVideoFrame(frameHandle, decodeStatus);
      if (!video_.decoderStopRequested.load() && decodeStatus == DR_OK) {
        SDL_Delay(STREAM_VIDEO_DECODE_YIELD_MILLISECONDS);
      }
    }

    return 0;
  }

  void FfmpegStreamBackend::record_received_unit(PDECODE_UNIT unit) {
    if (unit != nullptr && unit->fullLength > 0) {
      statistics_.received(static_cast<std::uint32_t>(unit->frameNumber), static_cast<std::uint64_t>(unit->fullLength), unit->frameHostProcessingLatency);
    }
  }

  int FfmpegStreamBackend::drop_queued_video_decode_units(VIDEO_FRAME_HANDLE *frameHandle, PDECODE_UNIT *decodeUnit) {
    if (frameHandle == nullptr || decodeUnit == nullptr || *frameHandle == nullptr || *decodeUnit == nullptr) {
      return 0;
    }

    int droppedFrames = 0;
    VIDEO_FRAME_HANDLE newestHandle = *frameHandle;
    PDECODE_UNIT newestDecodeUnit = *decodeUnit;

    while (true) {
      VIDEO_FRAME_HANDLE nextHandle = nullptr;
      PDECODE_UNIT nextDecodeUnit = nullptr;
      if (!LiPollNextVideoFrame(&nextHandle, &nextDecodeUnit)) {
        break;
      }
      record_received_unit(nextDecodeUnit);

      LiCompleteVideoFrame(newestHandle, STREAM_VIDEO_DROPPED_FRAME_STATUS);
      newestHandle = nextHandle;
      newestDecodeUnit = nextDecodeUnit;
      ++droppedFrames;
    }

    if (droppedFrames == 0) {
      return 0;
    }

    *frameHandle = newestHandle;
    *decodeUnit = newestDecodeUnit;
    statistics_.dropped(static_cast<std::uint64_t>(droppedFrames));

    if (const std::uint64_t droppedDecodeUnitCount = video_.droppedDecodeUnitCount.fetch_add(static_cast<std::uint64_t>(droppedFrames)) + static_cast<std::uint64_t>(droppedFrames); droppedDecodeUnitCount <= static_cast<std::uint64_t>(droppedFrames) || (droppedDecodeUnitCount % STREAM_VIDEO_DROP_LOG_INTERVAL) < static_cast<std::uint64_t>(droppedFrames)) {
      logging::warn(
        "stream",
        std::string("Dropped queued video decode units before decode | dropped_total=") + std::to_string(droppedDecodeUnitCount) + " | dropped_now=" + std::to_string(droppedFrames) +
          " | newest_frame=" + std::to_string(newestDecodeUnit != nullptr ? newestDecodeUnit->frameNumber : -1) +
          (newestDecodeUnit != nullptr && newestDecodeUnit->frameType != FRAME_TYPE_IDR ? " | waiting_for_idr" : "")
      );
    }

    return droppedFrames;
  }

  int FfmpegStreamBackend::prepare_video_decode_unit(VIDEO_FRAME_HANDLE *frameHandle, PDECODE_UNIT *decodeUnit) {
    if (const int droppedFrames = drop_queued_video_decode_units(frameHandle, decodeUnit); droppedFrames <= 0 || *decodeUnit == nullptr) {
      return DR_OK;
    }

    if ((*decodeUnit)->frameType == FRAME_TYPE_IDR && video_.codecContext != nullptr) {
      avcodec_flush_buffers(video_.codecContext);
      return DR_OK;
    }

    return DR_NEED_IDR;
  }

  bool FfmpegStreamBackend::publish_video_frame(AVFrame &frameToPresent) {
    if (!video_.decodeFrame.take(frameToPresent)) {
      return false;
    }
    {
      std::scoped_lock lock(video_.frame_mutex());
      video_.latestFrame.swap(video_.decodeFrame);
      ++video_.latestFrameVersion;
      video_.publishedFrameVersion.store(video_.latestFrameVersion);
    }
    video_.decodeFrame.clear();

    video_.lastDecodedFrameUs.store(PltGetMicroseconds());
    video_.hasFrame.store(true);
    video_.decodedFrameCount.fetch_add(1);
    statistics_.decoded();
    return true;
  }

  bool FfmpegStreamBackend::render_latest_video_frame_to_framebuffer(int screenWidth, int screenHeight) {
#ifdef NXDK
    const VIDEO_MODE videoMode = XVideoGetMode();
    std::uint8_t *framebuffer = XVideoGetFB();
    const int bytesPerPixel = xbox_framebuffer_bytes_per_pixel(videoMode.bpp);
    const AVPixelFormat destinationFormat = xbox_framebuffer_pixel_format(videoMode.bpp);
    if (framebuffer == nullptr || videoMode.width <= 0 || videoMode.height <= 0 || bytesPerPixel <= 0 || destinationFormat == AV_PIX_FMT_NONE) {
      return false;
    }

    const int framebufferWidth = screenWidth > 0 ? std::min(screenWidth, videoMode.width) : videoMode.width;
    const int framebufferHeight = screenHeight > 0 ? std::min(screenHeight, videoMode.height) : videoMode.height;
    const auto &frame = video_.renderFrame.view();
    const SDL_Rect destination = build_letterboxed_destination(framebufferWidth, framebufferHeight, frame.width, frame.height);
    if (destination.w <= 0 || destination.h <= 0) {
      return false;
    }

    const int framebufferPitch = videoMode.width * bytesPerPixel;
    if (!video_.directFramebufferCleared || !rectangles_match(video_.directFramebufferDestination, destination)) {
      std::memset(framebuffer, 0, static_cast<std::size_t>(framebufferPitch) * static_cast<std::size_t>(videoMode.height));
      video_.directFramebufferDestination = destination;
      video_.directFramebufferCleared = true;
    }

    video_.presentScaleContext = sws_getCachedContext(
      video_.presentScaleContext,
      frame.width,
      frame.height,
      AV_PIX_FMT_YUV420P,
      destination.w,
      destination.h,
      destinationFormat,
      SWS_FAST_BILINEAR,
      nullptr,
      nullptr,
      nullptr
    );
    if (video_.presentScaleContext == nullptr) {
      logging::warn("stream", "sws_getCachedContext failed for direct framebuffer presentation");
      return false;
    }

    const std::array<const std::uint8_t *, 4> sourceData {
      frame.planes[0],
      frame.planes[1],
      frame.planes[2],
      nullptr,
    };
    const std::array<int, 4> sourceLinesize {
      frame.pitches[0],
      frame.pitches[1],
      frame.pitches[2],
      0,
    };
    std::array<std::uint8_t *, 4> destinationData {
      framebuffer + (static_cast<std::size_t>(destination.y) * static_cast<std::size_t>(framebufferPitch)) + (static_cast<std::size_t>(destination.x) * static_cast<std::size_t>(bytesPerPixel)),
      nullptr,
      nullptr,
      nullptr,
    };
    const std::array<int, 4> destinationLinesize {
      framebufferPitch,
      0,
      0,
      0,
    };

    if (const int scaledRows = sws_scale(video_.presentScaleContext, sourceData.data(), sourceLinesize.data(), 0, frame.height, destinationData.data(), destinationLinesize.data()); scaledRows <= 0) {
      logging::warn("stream", "sws_scale failed for direct framebuffer presentation");
      return false;
    }

    statisticsOverlay_.draw_rgb(framebuffer, static_cast<std::size_t>(framebufferPitch) * videoMode.height, framebufferPitch, videoMode.width, videoMode.height, videoMode.bpp, {destination.x, destination.y, destination.w, destination.h});
    XVideoFlushFB();
    return true;
#else
    (void) screenWidth;
    (void) screenHeight;
    return false;
#endif
  }

  int FfmpegStreamBackend::receive_available_video_frames() {
    while (true) {
      const int receiveResult = avcodec_receive_frame(video_.codecContext, video_.decodedFrame);
      if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
        return receiveResult;
      }
      if (receiveResult < 0) {
        logging::warn("stream", std::string("avcodec_receive_frame failed for ") + video_.codecName + ": " + describe_ffmpeg_error(receiveResult));
        return receiveResult;
      }

      AVFrame *frameToPresent = video_.decodedFrame;
      if (video_.decodedFrame->format != AV_PIX_FMT_YUV420P) {
        video_.scaleContext = sws_getCachedContext(
          video_.scaleContext,
          video_.decodedFrame->width,
          video_.decodedFrame->height,
          static_cast<AVPixelFormat>(video_.decodedFrame->format),
          video_.decodedFrame->width,
          video_.decodedFrame->height,
          AV_PIX_FMT_YUV420P,
          SWS_FAST_BILINEAR,
          nullptr,
          nullptr,
          nullptr
        );
        if (video_.scaleContext == nullptr) {
          logging::warn("stream", "sws_getCachedContext failed for video conversion");
          av_frame_unref(video_.decodedFrame);
          return AVERROR(EINVAL);
        }

        av_frame_unref(video_.convertedFrame);
        video_.convertedFrame->format = AV_PIX_FMT_YUV420P;
        video_.convertedFrame->width = video_.decodedFrame->width;
        video_.convertedFrame->height = video_.decodedFrame->height;
        video_.convertedFrame->color_range = video_.decodedFrame->color_range;
        video_.convertedFrame->colorspace = video_.decodedFrame->colorspace;
        video_.convertedFrame->flags = video_.decodedFrame->flags;
        if (const int allocation = av_frame_get_buffer(video_.convertedFrame, 32); allocation < 0) {
          logging::warn("stream", "av_frame_get_buffer failed for converted video frame");
          av_frame_unref(video_.decodedFrame);
          return allocation;
        }

        if (const int scaled = sws_scale(video_.scaleContext, video_.decodedFrame->data, video_.decodedFrame->linesize, 0, video_.decodedFrame->height, video_.convertedFrame->data, video_.convertedFrame->linesize); scaled != video_.convertedFrame->height) {
          logging::warn("stream", "sws_scale failed to convert the full video frame");
          av_frame_unref(video_.decodedFrame);
          av_frame_unref(video_.convertedFrame);
          return scaled < 0 ? scaled : AVERROR(EINVAL);
        }
        frameToPresent = video_.convertedFrame;
      }

      const bool published = publish_video_frame(*frameToPresent);
      if (frameToPresent != video_.decodedFrame) {
        av_frame_unref(video_.decodedFrame);
      }
      if (!published) {
        av_frame_unref(frameToPresent);
        return AVERROR(EINVAL);
      }
    }
  }

  int FfmpegStreamBackend::decode_video_decode_unit(PDECODE_UNIT decodeUnit) {
    if (decodeUnit == nullptr || video_.codecContext == nullptr || video_.packet == nullptr || video_.decodedFrame == nullptr || video_.convertedFrame == nullptr) {
      return DR_NEED_IDR;
    }

    if (decodeUnit->fullLength <= 0) {
      return DR_NEED_IDR;
    }

    video_.packetBuffer.resize(static_cast<std::size_t>(decodeUnit->fullLength) + AV_INPUT_BUFFER_PADDING_SIZE);
    int offset = 0;
    for (PLENTRY buffer = decodeUnit->bufferList; buffer != nullptr; buffer = buffer->next) {
      if (buffer->length <= 0) {
        continue;
      }

      if (offset + buffer->length > decodeUnit->fullLength) {
        logging::warn("stream", "Moonlight supplied a video decode unit larger than its declared length");
        return DR_NEED_IDR;
      }

      std::memcpy(video_.packetBuffer.data() + offset, buffer->data, static_cast<std::size_t>(buffer->length));
      offset += buffer->length;
    }
    if (offset <= 0) {
      return DR_NEED_IDR;
    }
    std::memset(video_.packetBuffer.data() + offset, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    av_packet_unref(video_.packet);
    video_.packet->data = video_.packetBuffer.data();
    video_.packet->size = offset;

    const std::uint64_t submittedDecodeUnitCount = video_.submittedDecodeUnitCount.fetch_add(1) + 1;
    const std::uint64_t decodeStartUs = PltGetMicroseconds();
    video_.lastDecodeFrameNumber.store(decodeUnit->frameNumber);
    if (decodeUnit->enqueueTimeUs > 0 && decodeStartUs >= decodeUnit->enqueueTimeUs) {
      video_.lastDecodeQueueUs.store(decodeStartUs - decodeUnit->enqueueTimeUs);
    }
    if (decodeUnit->receiveTimeUs > 0 && decodeStartUs >= decodeUnit->receiveTimeUs) {
      video_.lastReceiveAgeUs.store(decodeStartUs - decodeUnit->receiveTimeUs);
    }
    if (submittedDecodeUnitCount == 1 || (submittedDecodeUnitCount % STREAM_VIDEO_SUBMISSION_LOG_INTERVAL) == 0) {
      const std::uint64_t receiveWindowUs = decodeUnit->enqueueTimeUs >= decodeUnit->receiveTimeUs ? decodeUnit->enqueueTimeUs - decodeUnit->receiveTimeUs : 0;
      logging::debug(
        "stream",
        std::string("Submitted video decode unit ") + std::to_string(submittedDecodeUnitCount) + " frame=" + std::to_string(decodeUnit->frameNumber) + " bytes=" + std::to_string(decodeUnit->fullLength) + " queue_us=" + std::to_string(receiveWindowUs)
      );
    }

    int sendResult = avcodec_send_packet(video_.codecContext, video_.packet);
    if (sendResult == AVERROR(EAGAIN)) {
      if (const int drainResult = receive_available_video_frames(); drainResult < 0 && drainResult != AVERROR(EAGAIN) && drainResult != AVERROR_EOF) {
        av_packet_unref(video_.packet);
        return DR_NEED_IDR;
      }
      sendResult = avcodec_send_packet(video_.codecContext, video_.packet);
    }
    av_packet_unref(video_.packet);
    if (sendResult < 0) {
      logging::warn("stream", std::string("avcodec_send_packet failed for ") + video_.codecName + ": " + describe_ffmpeg_error(sendResult));
      return DR_NEED_IDR;
    }

    if (const int receiveResult = receive_available_video_frames(); receiveResult < 0 && receiveResult != AVERROR(EAGAIN) && receiveResult != AVERROR_EOF) {
      return DR_NEED_IDR;
    }

    return DR_OK;
  }

}  // namespace streaming
