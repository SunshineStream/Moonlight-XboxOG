/**
 * @file src/streaming/ffmpeg_stream_backend.cpp
 * @brief Implements the FFmpeg-backed streaming decode backend.
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
#ifdef SUNSHINE_NATIVE_CLIENT
  #include <libavutil/hwcontext.h>
#endif
#include <libavutil/cpu.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

// local includes
#include "src/app/file_save_activity.h"
#include "src/logging/logger.h"
#include "src/platform/filesystem_utils.h"

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
  using SunshineCallbackContext = void;

  constexpr std::uint64_t STREAM_VIDEO_SUBMISSION_LOG_INTERVAL = 120;
  constexpr int STREAM_VIDEO_DROPPED_FRAME_STATUS = -2;
  constexpr std::uint64_t STREAM_VIDEO_DROP_LOG_INTERVAL = 30;
  constexpr Uint32 STREAM_VIDEO_DECODE_YIELD_MILLISECONDS = 1U;

  /**
   * @brief FFmpeg codec metadata selected from one negotiated Sunshine format.
   */
  struct VideoCodecSelection {
    AVCodecID codecId = AV_CODEC_ID_NONE;  ///< FFmpeg codec identifier.
    const char *displayName = "video";  ///< Human-readable codec name for logs.
  };

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
   * @brief Distinguish a receive failure from the normal end of a decoder drain.
   * @param result FFmpeg frame receive result.
   * @return True when receiving frames failed and the caller needs a new keyframe.
   */
  bool video_receive_failed(int result) {
    return result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF;
  }

#ifdef SUNSHINE_NATIVE_CLIENT
  /**
   * @brief Recognize decoder errors that may require switching away from hardware.
   * @param errorCode FFmpeg packet submission or frame receive result.
   * @return True for a hardware, I/O or unsupported-operation failure.
   */
  bool is_hardware_decoder_failure(int errorCode) {
    return errorCode == AVERROR_EXTERNAL || errorCode == AVERROR(EIO) || errorCode == AVERROR(ENOSYS);
  }
#endif

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
  void ffmpeg_log_callback(SunshineCallbackContext *avClassContext, int level, const char *format, va_list arguments) {
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
   * @brief Resolve the FFmpeg decoder to use for one negotiated video format.
   *
   * @param videoFormat Sunshine negotiated video format mask.
   * @param selection Output codec selection populated on success.
   * @return True when the video format is supported by this backend.
   */
  bool select_video_codec(int videoFormat, VideoCodecSelection *selection) {
    if (selection == nullptr) {
      return false;
    }

    if ((videoFormat & VIDEO_FORMAT_MASK_MPEG2) != 0) {
      selection->codecId = AV_CODEC_ID_MPEG2VIDEO;
      selection->displayName = "MPEG-2/H.262";
      return true;
    }
    if ((videoFormat & VIDEO_FORMAT_H263P) != 0) {
      selection->codecId = AV_CODEC_ID_H263P;
      selection->displayName = "H.263+";
      return true;
    }
    if ((videoFormat & VIDEO_FORMAT_MASK_H264) != 0) {
      selection->codecId = AV_CODEC_ID_H264;
      selection->displayName = "H.264";
      return true;
    }
#ifdef SUNSHINE_NATIVE_CLIENT
    if ((videoFormat & VIDEO_FORMAT_H265) != 0) {
      *selection = {AV_CODEC_ID_HEVC, "HEVC"};
      return true;
    }
    if ((videoFormat & VIDEO_FORMAT_AV1_MAIN8) != 0) {
      *selection = {AV_CODEC_ID_AV1, "AV1"};
      return true;
    }
#endif

    return false;
  }

#ifdef SUNSHINE_NATIVE_CLIENT
  /**
   * @brief Find an FFmpeg software decoder for a portable format.
   * @param id FFmpeg codec ID.
   * @return Software decoder, or nullptr when unavailable.
   */
  const AVCodec *find_software_decoder(AVCodecID id) {
    if (id == AV_CODEC_ID_AV1) {
      if (const AVCodec *decoder = avcodec_find_decoder_by_name("libdav1d"); decoder != nullptr) {
        return decoder;
      }
      return avcodec_find_decoder_by_name("libaom-av1");
    }
    return avcodec_find_decoder(id);
  }

  /**
   * @brief Find Android's byte-buffer MediaCodec decoder, if built.
   * @param id FFmpeg codec ID.
   * @return Matching MediaCodec wrapper on Android, or nullptr.
   */
  const AVCodec *find_mediacodec_decoder(AVCodecID id) {
  #ifdef __ANDROID__
    switch (id) {
      case AV_CODEC_ID_H264:
        return avcodec_find_decoder_by_name("h264_mediacodec");
      case AV_CODEC_ID_HEVC:
        return avcodec_find_decoder_by_name("hevc_mediacodec");
      case AV_CODEC_ID_AV1:
        return avcodec_find_decoder_by_name("av1_mediacodec");
      default:
        return nullptr;
    }
  #else
    (void) id;
    return nullptr;
  #endif
  }

  /**
   * @brief Prefer the hardware format if the decoder offers it for this stream.
   * @param context Decoder context with the selected hardware format.
   * @param formats Candidate formats for the negotiated stream.
   * @return An available hardware or software pixel format.
   */
  AVPixelFormat select_hardware_pixel_format(AVCodecContext *context, const AVPixelFormat *formats) {
    const auto requested = *static_cast<const int *>(context->opaque);
    for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
      if (static_cast<int>(*format) == requested) {
        return *format;
      }
    }
    return avcodec_default_get_format(context, formats);
  }

  /**
   * @brief Attach the first available platform device supported by this decoder.
   * @param context Decoder context to prepare.
   * @param codec Codec advertised by FFmpeg.
   * @param hardwareFormat Receives the device's hardware pixel format.
   * @return Device name when initialized, or nullptr for software decoding.
   */
  const char *attach_hardware_decoder(AVCodecContext &context, const AVCodec &codec, AVPixelFormat &hardwareFormat) {
  #if defined(_WIN32)
    constexpr std::array devices {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2};
  #elif defined(__APPLE__)
    constexpr std::array devices {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
  #elif defined(__linux__) && !defined(__ANDROID__)
    constexpr std::array devices {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA};
  #else
    constexpr std::array<AVHWDeviceType, 0> devices {};
  #endif
    for (const auto device : devices) {
      for (int index = 0;; ++index) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(&codec, index);
        if (config == nullptr) {
          break;
        }
        if (config->device_type == device && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 && av_hwdevice_ctx_create(&context.hw_device_ctx, device, nullptr, nullptr, 0) == 0) {
          hardwareFormat = config->pix_fmt;
          context.get_format = select_hardware_pixel_format;
          return av_hwdevice_get_type_name(device);
        }
      }
    }
    return nullptr;
  }

  /**
   * @brief Open a portable FFmpeg decoder with the negotiated picture dimensions.
   * @param context Allocated decoder context, which may be null after an allocation failure.
   * @param decoder FFmpeg implementation to open.
   * @param width Negotiated width in pixels.
   * @param height Negotiated height in pixels.
   * @return FFmpeg status code.
   */
  int open_portable_decoder(AVCodecContext *context, const AVCodec *decoder, int width, int height) {
    if (context == nullptr) {
      return AVERROR(ENOMEM);
    }
    context->width = width;
    context->height = height;
    context->thread_count = 0;
    context->thread_type = FF_THREAD_SLICE;
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    return avcodec_open2(context, decoder, nullptr);
  }

  /**
   * @brief Replace a hardware frame with its software copy for conversion and presentation.
   * @param frame Decoded frame to read back, or an existing software frame to leave in place.
   * @param staging Temporary frame used for the transfer.
   * @return Zero on success or a negative FFmpeg error.
   */
  int read_hardware_frame(AVFrame &frame, AVFrame &staging) {
    if (frame.hw_frames_ctx == nullptr) {
      return 0;
    }
    av_frame_unref(&staging);
    int transfer = av_hwframe_transfer_data(&staging, &frame, 0);
    if (transfer >= 0) {
      transfer = av_frame_copy_props(&staging, &frame);
    }
    av_frame_unref(&frame);
    if (transfer < 0) {
      logging::warn("stream", std::string("Could not read a hardware video frame: ") + describe_ffmpeg_error(transfer));
      av_frame_unref(&staging);
      return transfer;
    }
    av_frame_move_ref(&frame, &staging);
    return 0;
  }
#endif

  /**
   * @brief Convert a software frame to the presenter's I420 format.
   * @param frame Decoded software frame.
   * @param converted Receives the I420 pixels and presentation metadata.
   * @param scaleContext Decoder-owned conversion context, replaced when the format changes.
   * @return Zero on success or a negative FFmpeg error.
   */
  int convert_video_frame(const AVFrame &frame, AVFrame &converted, SwsContext *&scaleContext) {
    scaleContext = sws_getCachedContext(
      scaleContext,
      frame.width,
      frame.height,
      static_cast<AVPixelFormat>(frame.format),
      frame.width,
      frame.height,
      AV_PIX_FMT_YUV420P,
      SWS_FAST_BILINEAR,
      nullptr,
      nullptr,
      nullptr
    );
    if (scaleContext == nullptr) {
      logging::warn("stream", "sws_getCachedContext failed for video conversion");
      return AVERROR(EINVAL);
    }

    av_frame_unref(&converted);
    converted.format = AV_PIX_FMT_YUV420P;
    converted.width = frame.width;
    converted.height = frame.height;
    converted.color_range = frame.color_range;
    converted.colorspace = frame.colorspace;
    converted.flags = frame.flags;
    if (const int allocation = av_frame_get_buffer(&converted, 32); allocation < 0) {
      logging::warn("stream", "av_frame_get_buffer failed for converted video frame");
      return allocation;
    }

    if (const int scaled = sws_scale(scaleContext, frame.data, frame.linesize, 0, frame.height, converted.data, converted.linesize); scaled != converted.height) {
      logging::warn("stream", "sws_scale failed to convert the full video frame");
      av_frame_unref(&converted);
      return scaled < 0 ? scaled : AVERROR(EINVAL);
    }
    return 0;
  }

  int on_video_setup(int videoFormat, int width, int height, int redrawRate, SunshineCallbackContext *context, int drFlags) {  // NOSONAR(cpp:S5008) moonlight-common-c requires a void* render context callback.
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
   * @param audioConfiguration Negotiated Sunshine layout.
   * @param opusConfig Negotiated Opus channels and mappings.
   * @param context Stream backend supplied by Sunshine.
   * @return Zero when playback can start or has been disabled.
   */
  int on_audio_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, SunshineCallbackContext *context, int) {  // NOSONAR(cpp:S5008,cpp:S995) moonlight-common-c requires this callback signature.
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
   * @brief Decode and queue the next Sunshine Opus payload.
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
    SDL_IOStream *file;  ///< Worker-owned file, closed after SDL finalizes the BMP header.
    std::atomic<int> &progress;  ///< UI-readable percentage independent of file and surface lifetimes.
    Sint64 expected;  ///< Final 24-bit BMP length, including scanline padding and header.
    Sint64 position = 0;  ///< Current file position, including SDL's header fixup seeks.
    Sint64 written = 0;  ///< Highest written position, preventing header fixups from reducing progress.
  };

}  // namespace

// SDL file callbacks use C linkage; project-prefixed names avoid global symbol collisions.
/**
 * @brief Delegate a BMP file seek while keeping progress accounting in sync.
 * @param context BMP progress state.
 * @param offset Byte offset relative to whence.
 * @param whence SDL seek origin.
 * @return New byte position or a negative value on error.
 */
extern "C" Sint64 sunshine_screenshot_file_seek(void *context, Sint64 offset, SDL_IOWhence whence) {
  auto &writer = *static_cast<ScreenshotFileProgress *>(context);
  writer.position = SDL_SeekIO(writer.file, offset, whence);
  return writer.position;
}

/**
 * @brief Forward one BMP write and publish progress only for bytes actually written.
 * @param context BMP progress state.
 * @param pixels Header or pixel bytes supplied by the SDL C writer callback.
 * @param size Requested byte count.
 * @param status Receives the underlying stream's status after a partial write.
 * @return Bytes written.
 */
extern "C" std::size_t sunshine_screenshot_file_write(void *context, const void *pixels, std::size_t size, SDL_IOStatus *status) {
  auto &writer = *static_cast<ScreenshotFileProgress *>(context);
  const auto written = SDL_WriteIO(writer.file, pixels, size);
  if (written < size) {
    *status = SDL_GetIOStatus(writer.file);
  }
  writer.position += static_cast<Sint64>(written);
  writer.written = std::max(writer.written, writer.position);
  writer.progress.store(10 + static_cast<int>(85 * std::min(writer.written, writer.expected) / writer.expected));
  return written;
}

namespace streaming {
#ifdef SUNSHINE_NATIVE_CLIENT
  int FfmpegStreamBackend::available_video_formats() {
    int formats = 0;
    for (const int format : {VIDEO_FORMAT_H264, VIDEO_FORMAT_H265, VIDEO_FORMAT_AV1_MAIN8}) {
      VideoCodecSelection selection;
      if (select_video_codec(format, &selection) && find_software_decoder(selection.codecId) != nullptr) {
        formats |= format;
      }
    }
    return formats;
  }
#endif

  VideoAccelerationReport FfmpegStreamBackend::acceleration_report() const {
    return acceleration_;
  }

  VideoStatisticsCounters FfmpegStreamBackend::performance_counters() const {
    return statistics_.counters();
  }

  bool FfmpegStreamBackend::request_screenshot(std::string_view directory, const app::ScreenshotSource &source) {
    if (const auto result = take_screenshot_result(); !result.empty()) {
      logging::info("screenshots", result);
    }
    if (screenshot_.thread != nullptr || presentation_.frame().view().width <= 0) {
      logging::warn("screenshots", "Capture not queued: another screenshot is pending or no video frame is available");
      return false;
    }
    if (!screenshot_.frame.retain(presentation_.frame())) {
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
      return ScreenshotTask::save(static_cast<ScreenshotTask *>(context));
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
    SDL_IOStream *file = SDL_IOFromFile(path.c_str(), "wb");
    if (file == nullptr) {
      return false;
    }
    ScreenshotFileProgress writer {file, progress, 54 + static_cast<Sint64>((surface.w * 3 + 3) & ~3) * surface.h};
    SDL_IOStreamInterface callbacks;
    SDL_INIT_INTERFACE(&callbacks);
    callbacks.seek = sunshine_screenshot_file_seek;
    callbacks.write = sunshine_screenshot_file_write;
    SDL_IOStream *progressFile = SDL_OpenIO(&callbacks, &writer);
    const bool encoded = progressFile != nullptr && SDL_SaveBMP_IO(&surface, progressFile, true);
    const bool closed = SDL_CloseIO(file);
    const bool saved = encoded && closed;
    if (!saved) {
      std::remove(path.c_str());
    }
    return saved;
  }

  /**
   * @brief Convert the owned YUV snapshot into a BGR screenshot without retaining conversion buffers.
   * @param frame Stable screenshot snapshot.
   * @return Owned BGR surface, or nullptr when conversion fails.
   */
  static SDL_Surface *convert_screenshot_frame(const Yuv420FrameView &frame) {
    SDL_Surface *surface = SDL_CreateSurface(frame.width, frame.height, SDL_PIXELFORMAT_BGR24);
    if (surface == nullptr) {
      return nullptr;
    }
    SwsContext *conversion = sws_getContext(frame.width, frame.height, AV_PIX_FMT_YUV420P, frame.width, frame.height, AV_PIX_FMT_BGR24, SWS_POINT, nullptr, nullptr, nullptr);
    if (conversion == nullptr) {
      SDL_DestroySurface(surface);
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
      SDL_DestroySurface(surface);
      return nullptr;
    }
    return surface;
  }

  int FfmpegStreamBackend::ScreenshotTask::save(ScreenshotTask *context) {
    auto &job = *context;
    // Stay below the streaming event loop without starving behind USB/audio work.
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
    job.progress.store(1);
    std::string error;
    const std::string path = app::next_screenshot_path(job.directory, job.capturedAt, error);
    SDL_Surface *surface = path.empty() ? nullptr : convert_screenshot_frame(job.frame.view());
    // Release the YUV snapshot before disk I/O so full-screen menus still fit in 64 MiB.
    job.frame.clear();
    bool saved = false;
    if (surface != nullptr) {
      const app::FileSaveActivity saving;
      job.progress.store(10);
      saved = save_screenshot_bitmap(*surface, path, job.progress);
      SDL_DestroySurface(surface);
      if (saved) {
        job.progress.store(99);
        job.result = app::append_screenshot({job.source, path, job.capturedAt}, app::screenshot_library_path(), error) ? "Saved " + path : "Saved " + path + "; could not update the screenshot library: " + error;
      }
    }
    if (!saved) {
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

  FfmpegVideoPresenter &FfmpegStreamBackend::presentation() {
    return presentation_;
  }

  const FfmpegVideoPresenter &FfmpegStreamBackend::presentation() const {
    return presentation_;
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

  bool FfmpegStreamBackend::render_latest_video_frame(SDL_Renderer *renderer, int screenWidth, int screenHeight) {
    const auto before = acceleration_.gpuFrames + acceleration_.softwareFrames;
    const auto started = PltGetMicroseconds();
    if (renderer == nullptr || !video_.hasFrame.load()) {
      return false;
    }

    bool newFrame = false;
    if (const std::uint64_t publishedFrameVersion = video_.publishedFrameVersion.load(); publishedFrameVersion != video_.renderedFrameVersion) {
      std::scoped_lock lock(video_.frame_mutex());
      if (video_.latestFrameVersion != video_.renderedFrameVersion && video_.latestFrame.view().width > 0) {
        if (video_.latestFrameVersion > video_.renderedFrameVersion + 1) {
          statistics_.dropped(video_.latestFrameVersion - video_.renderedFrameVersion - 1);
        }
        presentation_.swap_queued_frame(video_.latestFrame);
        video_.renderedFrameVersion = video_.latestFrameVersion;
        newFrame = true;
      }
    }

    const bool presented = presentation_.present(renderer, screenWidth, screenHeight, newFrame);
    if (presented && acceleration_.gpuFrames + acceleration_.softwareFrames > before) {
      statistics_.presented(PltGetMicroseconds() - started);
    }
    return presented;
  }

  int FfmpegStreamBackend::setup_video_decoder(int videoFormat, int width, int height, int redrawRate, int drFlags) {
    (void) width;
    (void) height;
    (void) redrawRate;
    (void) drFlags;

    cleanup_video_decoder();
    acceleration_ = {};
    acceleration_.gpuPresentationRequested = presentation_.gpu_presentation_enabled();
    statistics_.reset();
    const int cpuFlags = av_get_cpu_flags();
    if ((cpuFlags & AV_CPU_FLAG_SSE) != 0) {
      acceleration_.cpuFeatures = "MMX/SSE";
    } else {
      acceleration_.cpuFeatures = (cpuFlags & AV_CPU_FLAG_MMX) != 0 ? "MMX" : "Scalar";
    }
    acceleration_.threaded = std::string(avcodec_configuration()).find("--enable-w32threads") != std::string::npos;

    VideoCodecSelection codecSelection;
    if (!select_video_codec(videoFormat, &codecSelection)) {
      logging::error("stream", "The negotiated video format is unsupported by the FFmpeg backend");
      return -1;
    }

#ifdef SUNSHINE_NATIVE_CLIENT
    const AVCodec *softwareCodec = find_software_decoder(codecSelection.codecId);
    const AVCodec *mediaCodec = find_mediacodec_decoder(codecSelection.codecId);
    const AVCodec *nativeCodec = codecSelection.codecId == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("av1") : softwareCodec;
    const AVCodec *platformCodec = nativeCodec != nullptr ? nativeCodec : softwareCodec;
    const AVCodec *codec = mediaCodec != nullptr ? mediaCodec : platformCodec;
#else
    const AVCodec *codec = avcodec_find_decoder(codecSelection.codecId);
#endif
    if (codec == nullptr) {
      logging::error("stream", std::string("FFmpeg did not provide a ") + codecSelection.displayName + " decoder");
      return -1;
    }

    video_.codecName = codecSelection.displayName;
    video_.codecContext = avcodec_alloc_context3(codec);
    video_.decodedFrame = av_frame_alloc();
    video_.convertedFrame = av_frame_alloc();
    video_.packet = av_packet_alloc();
    if (video_.codecContext == nullptr || video_.decodedFrame == nullptr || video_.convertedFrame == nullptr || video_.packet == nullptr) {
      logging::error("stream", "Failed to allocate FFmpeg video decoder resources");
      cleanup_video_decoder();
      return -1;
    }

#ifdef NXDK
    // One decoder worker avoids frame-threading latency on the single-core CPU.
    // FFmpeg's w32threads support still provides its internal synchronization.
    video_.codecContext->thread_count = 1;
    video_.codecContext->thread_type = 0;
    video_.codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_OUTPUT_CORRUPT;
    video_.codecContext->flags2 |= AV_CODEC_FLAG2_FAST | AV_CODEC_FLAG2_SHOW_ALL;
    video_.codecContext->err_recognition = AV_EF_EXPLODE;
    video_.codecContext->skip_loop_filter = AVDISCARD_ALL;
    video_.codecContext->skip_idct = AVDISCARD_NONREF;
#endif
#ifdef SUNSHINE_NATIVE_CLIENT
    AVPixelFormat hardwareFormat = AV_PIX_FMT_NONE;
    const char *hardware = mediaCodec != nullptr ? "MediaCodec" : attach_hardware_decoder(*video_.codecContext, *codec, hardwareFormat);
    if (hardwareFormat != AV_PIX_FMT_NONE) {
      video_.codecContext->opaque = &video_.hardwareFormat;
      video_.hardwareFormat = static_cast<int>(hardwareFormat);
    }
    if (hardware == nullptr && softwareCodec != nullptr && codec != softwareCodec) {
      avcodec_free_context(&video_.codecContext);
      codec = softwareCodec;
      video_.codecContext = avcodec_alloc_context3(codec);
    }
    int openResult = open_portable_decoder(video_.codecContext, codec, width, height);
    if (openResult < 0 && hardware != nullptr && softwareCodec != nullptr) {
      avcodec_free_context(&video_.codecContext);
      video_.codecContext = avcodec_alloc_context3(softwareCodec);
      hardware = nullptr;
      openResult = open_portable_decoder(video_.codecContext, softwareCodec, width, height);
    }
#else
    const int openResult = avcodec_open2(video_.codecContext, codec, nullptr);
#endif
    if (openResult < 0) {
      logging::error("stream", std::string("avcodec_open2 failed for ") + video_.codecName + ": " + describe_ffmpeg_error(openResult));
      cleanup_video_decoder();
      return openResult;
    }

#ifdef SUNSHINE_NATIVE_CLIENT
    if (hardware != nullptr) {
      video_.softwareFallback = softwareCodec;
      logging::info("stream", std::string("Opened FFmpeg ") + video_.codecName + " with " + hardware + " acceleration");
      return 0;
    }
#endif
    logging::info("stream", std::string("Using FFmpeg ") + video_.codecName + " software decoding");
    return 0;
  }

#ifdef SUNSHINE_NATIVE_CLIENT
  void FfmpegStreamBackend::fallback_to_software_decoder() {
    const auto *codec = std::exchange(video_.softwareFallback, nullptr);
    if (codec == nullptr) {
      return;
    }
    AVCodecContext *replacement = avcodec_alloc_context3(codec);
    if (open_portable_decoder(replacement, codec, video_.codecContext->width, video_.codecContext->height) < 0) {
      avcodec_free_context(&replacement);
      return;
    }
    avcodec_free_context(&video_.codecContext);
    video_.codecContext = replacement;
    logging::warn("stream", std::string("Switched ") + video_.codecName + " decoding to software");
  }
#endif

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

  void FfmpegStreamBackend::cleanup_video_decoder() {
    stop_video_decoder();
    presentation_.reset();

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

    video_.packetBuffer.clear();
    video_.decodeFrame.clear();
    {
      std::scoped_lock lock(video_.frame_mutex());
      video_.latestFrame.clear();
      video_.latestFrameVersion = 0;
    }
    video_.renderedFrameVersion = 0;
    video_.publishedFrameVersion.store(0);
    video_.codecName = "video";
#ifdef SUNSHINE_NATIVE_CLIENT
    video_.softwareFallback = nullptr;
#endif
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
    if (!SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH)) {
      logging::debug("stream", std::string("SDL_SetCurrentThreadPriority failed for video decoder: ") + SDL_GetError());
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

  void FfmpegStreamBackend::record_received_unit(const DECODE_UNIT *unit) {
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

  int FfmpegStreamBackend::receive_available_video_frames() {
    while (true) {
      const int receiveResult = avcodec_receive_frame(video_.codecContext, video_.decodedFrame);
      if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
        return receiveResult;
      }
      if (receiveResult < 0) {
        logging::warn("stream", std::string("avcodec_receive_frame failed for ") + video_.codecName + ": " + describe_ffmpeg_error(receiveResult));
#ifdef SUNSHINE_NATIVE_CLIENT
        if (is_hardware_decoder_failure(receiveResult)) {
          fallback_to_software_decoder();
        }
#endif
        return receiveResult;
      }

#ifdef SUNSHINE_NATIVE_CLIENT
      if (const int transfer = read_hardware_frame(*video_.decodedFrame, *video_.convertedFrame); transfer < 0) {
        fallback_to_software_decoder();
        return transfer;
      }
#endif
      AVFrame *frameToPresent = video_.decodedFrame;
      if (video_.decodedFrame->format != AV_PIX_FMT_YUV420P) {
        if (const int converted = convert_video_frame(*video_.decodedFrame, *video_.convertedFrame, video_.scaleContext); converted < 0) {
          av_frame_unref(video_.decodedFrame);
          return converted;
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
        logging::warn("stream", "Sunshine supplied a video decode unit larger than its declared length");
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
      if (video_receive_failed(receive_available_video_frames())) {
        av_packet_unref(video_.packet);
        return DR_NEED_IDR;
      }
      sendResult = avcodec_send_packet(video_.codecContext, video_.packet);
    }
    av_packet_unref(video_.packet);
    if (sendResult < 0) {
      logging::warn("stream", std::string("avcodec_send_packet failed for ") + video_.codecName + ": " + describe_ffmpeg_error(sendResult));
#ifdef SUNSHINE_NATIVE_CLIENT
      if (is_hardware_decoder_failure(sendResult)) {
        fallback_to_software_decoder();
      }
#endif
      return DR_NEED_IDR;
    }

    if (video_receive_failed(receive_available_video_frames())) {
      return DR_NEED_IDR;
    }

    return DR_OK;
  }

}  // namespace streaming
