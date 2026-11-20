/**
 * @file src/streaming/ffmpeg_stream_backend.h
 * @brief Declares the FFmpeg-backed streaming decode backend for Xbox sessions.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <Limelight.h>
#include <SDL3/SDL.h>

// local includes
#include "src/app/screenshot_library.h"
#include "src/streaming/aligned_media_buffer.h"
#include "src/streaming/audio_player.h"
#include "src/streaming/ffmpeg_video_frame.h"
#include "src/streaming/ffmpeg_video_presenter.h"
#include "src/streaming/stats_overlay.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace streaming {

  /**
   * @brief Opaque SDL thread context payload.
   */
  using SdlThreadContext = void;

  /**
   * @brief Owns the FFmpeg decode and SDL presentation state for one stream.
   *
   * The backend exposes Moonlight-compatible callback tables for video and audio,
   * presents the latest decoded video frame and delegates Opus decode and playback
   * to the audio player.
   */
  class FfmpegStreamBackend {
  public:
    /**
     * @brief Construct an empty backend.
     */
    FfmpegStreamBackend() = default;

    /**
     * @brief Destroy the backend and release all resources.
     */
    ~FfmpegStreamBackend();

    FfmpegStreamBackend(const FfmpegStreamBackend &) = delete;
    FfmpegStreamBackend &operator=(const FfmpegStreamBackend &) = delete;

    /**
     * @brief Populate Moonlight callback tables for this backend.
     *
     * @param videoCallbacks Output video callback table.
     * @param audioCallbacks Output audio callback table.
     */
    void initialize_callbacks(DECODER_RENDERER_CALLBACKS *videoCallbacks, AUDIO_RENDERER_CALLBACKS *audioCallbacks) const;

    /**
     * @brief Get the stream's Opus decoder and playback device.
     * @return Audio player owned by this backend.
     */
    AudioPlayer &audio_player();

    /**
     * @brief Access the render-thread presentation state.
     * @return Presenter for frames, overlays, and display settings.
     */
    FfmpegVideoPresenter &presentation();

    /**
     * @brief Read the current presentation state.
     * @return Presenter owned by the backend.
     */
    const FfmpegVideoPresenter &presentation() const;

    /**
     * @brief Drain any pending screenshot and release FFmpeg, SDL, and cached frame resources.
     */
    void shutdown();

    /**
     * @brief Return retained presentation and decoder acceleration diagnostics.
     * @return Snapshot to capture on the render thread before session cleanup.
     */
    VideoAccelerationReport acceleration_report() const;

    /**
     * @brief Read cumulative performance telemetry.
     *
     * @return Concurrent video counter snapshot.
     */
    VideoStatisticsCounters performance_counters() const;

    /**
     * @brief Snapshot the displayed host frame and save it on a bounded background worker.
     * @param directory Configured screenshot destination, or empty for the default.
     * @param source Server and app identity captured at request time.
     * @return True when queued; false before the first frame or while another capture is pending.
     */
    bool request_screenshot(std::string_view directory, const app::ScreenshotSource &source);
    /**
     * @brief Reap a completed screenshot worker without blocking the streaming event loop.
     * @return Completion or error detail, or empty when no result is ready.
     */
    std::string take_screenshot_result();

    /**
     * @brief Read background capture progress without waiting for conversion or disk I/O.
     * @return Zero through 99 while a capture is pending, or -1 when there is no worker.
     */
    int screenshot_progress() const;

    /**
     * @brief Render the latest decoded video frame to the supplied renderer.
     *
     * @param renderer SDL renderer used by the stream session.
     * @param screenWidth Current renderer output width.
     * @param screenHeight Current renderer output height.
     * @return True when decoded video is displayed on any presentation path.
     */
    bool render_latest_video_frame(SDL_Renderer *renderer, int screenWidth, int screenHeight);

    /**
     * @brief Report whether at least one decoded video frame is available.
     *
     * @return True when a decoded frame is ready for presentation.
     */
    bool has_decoded_video() const;

    /**
     * @brief Return the human-readable active video decoder name.
     *
     * @return Selected video decoder name, or a generic label before setup.
     */
    std::string active_video_decoder_name() const;

    /**
     * @brief Report whether a newly decoded video frame has not been presented yet.
     *
     * @return True when rendering would present a newer decoded frame.
     */
    bool has_unrendered_video_frame() const;

    /**
     * @brief Return how long it has been since FFmpeg published a decoded video frame.
     *
     * @param nowMicroseconds Current platform monotonic time.
     * @return Milliseconds since the last decoded frame, or zero before a frame is published.
     */
    std::uint64_t milliseconds_since_last_decoded_video_frame(std::uint64_t nowMicroseconds) const;

    /**
     * @brief Build a short user-visible media status line.
     *
     * @return Summary of decoded video and queued audio state.
     */
    std::string build_overlay_status_line() const;

    /**
     * @brief Initialize the FFmpeg decoder for Moonlight video callbacks.
     *
     * @param videoFormat Negotiated Moonlight video format.
     * @param width Negotiated stream width.
     * @param height Negotiated stream height.
     * @param redrawRate Negotiated redraw rate.
     * @param drFlags Moonlight decoder flags.
     * @return Zero on success.
     */
    int setup_video_decoder(int videoFormat, int width, int height, int redrawRate, int drFlags);

    /**
     * @brief Start the video decode path.
     */
    void start_video_decoder();

    /**
     * @brief Stop the video decode path.
     */
    void stop_video_decoder();

    /**
     * @brief Clean up all FFmpeg video decode resources.
     */
    void cleanup_video_decoder();

  private:
    /**
     * @brief Record a retrieved frame before any client discard.
     *
     * @param unit Completed network frame.
     */
    void record_received_unit(const DECODE_UNIT *unit);
    /**
     * @brief Entry point used by the SDL video decode worker thread.
     *
     * @param context Backend instance.
     * @return Zero when the worker exits normally.
     */
    static int run_video_decode_thread_trampoline(SdlThreadContext *context);

    /**
     * @brief Pull Moonlight decode units and feed them into FFmpeg.
     *
     * @return Zero when the worker exits normally.
     */
    int run_video_decode_thread();

    /**
     * @brief Drop queued decode units so the decoder works on the newest frame.
     *
     * @param frameHandle Current frame handle, replaced with the newest queued handle.
     * @param decodeUnit Current decode unit, replaced with the newest queued unit.
     * @return Number of decode units discarded.
     */
    int drop_queued_video_decode_units(VIDEO_FRAME_HANDLE *frameHandle, PDECODE_UNIT *decodeUnit);

    /**
     * @brief Drop stale queued video decode units and prepare the newest unit.
     *
     * @param frameHandle Current frame handle, replaced with the newest queued handle.
     * @param decodeUnit Current decode unit, replaced with the newest queued unit.
     * @return Moonlight decoder status after stale-frame handling.
     */
    int prepare_video_decode_unit(VIDEO_FRAME_HANDLE *frameHandle, PDECODE_UNIT *decodeUnit);

    /**
     * @brief Decode one Moonlight video decode unit on the video worker thread.
     *
     * @param decodeUnit Moonlight Annex B frame payload.
     * @return Moonlight decoder status code.
     */
    int decode_video_decode_unit(PDECODE_UNIT decodeUnit);

    /**
     * @brief Drain decoded frames currently available from FFmpeg.
     *
     * @return FFmpeg receive result.
     */
    int receive_available_video_frames();

    /**
     * @brief Publish one decoded or converted frame for presentation.
     *
     * @param frameToPresent FFmpeg frame in I420 layout, emptied on success.
     * @return True when the frame reference was transferred to the presenter queue.
     */
    bool publish_video_frame(AVFrame &frameToPresent);

    /**
     * @brief Own one bounded screenshot job independently of live rendering and decoding.
     */
    struct ScreenshotTask {
      SDL_Thread *thread = nullptr;  ///< Owned worker, joined after completion or at shutdown.
      std::atomic<bool> complete = false;  ///< Publishes the completed result before the UI can read it.
      std::atomic<int> progress = 0;  ///< Conversion, BMP write and index progress; completion is published separately.
      FfmpegVideoFrame frame;  ///< Separate reference to the uncomposited YUV frame.
      app::ScreenshotSource source;  ///< Original host and app identity.
      std::string directory;  ///< Destination at request time.
      std::int64_t capturedAt = 0;  ///< UTC capture time.
      std::string result;  ///< Detail read only after the worker completes.
    };

    /**
     * @brief Convert and save an owned YUV snapshot with lower priority than streaming.
     * @param context ScreenshotTask owned by this backend until the worker is joined.
     * @return Zero after publishing a completion result.
     */
    static int save_screenshot_worker(ScreenshotTask *context);
    /**
     * @brief Convert the owned YUV snapshot into a BGR screenshot without retaining conversion buffers.
     * @param frame Stable screenshot snapshot.
     * @return Owned BGR surface, or nullptr when conversion fails.
     */
    static SDL_Surface *convert_screenshot_frame(const Yuv420FrameView &frame);

    /**
     * @brief Hold FFmpeg and SDL state used by the video path.
     */
    struct VideoState {  // NOSONAR(cpp:S1820) Owned video pipeline state intentionally groups FFmpeg, SDL, queue, and metric lifetimes.
      AVCodecContext *codecContext = nullptr;
      SwsContext *scaleContext = nullptr;
      AVFrame *decodedFrame = nullptr;
      AVFrame *convertedFrame = nullptr;
      AVPacket *packet = nullptr;
      const char *codecName = "video";  ///< Human-readable active video decoder name.
      SDL_Thread *decoderThread = nullptr;
      std::uint64_t latestFrameVersion = 0;
      std::uint64_t renderedFrameVersion = 0;
      std::vector<std::uint8_t> packetBuffer;

      /**
       * @brief Return the mutex guarding decoded frame publication.
       *
       * @return Mutex used to exchange frames between decode and render threads.
       */
      [[nodiscard]] std::mutex &frame_mutex() const {
        return frameMutex_;
      }

    private:
      mutable std::mutex frameMutex_;  ///< Guards latest decoded frame publication between worker and render threads.

    public:
      FfmpegVideoFrame latestFrame;  ///< Mailbox slot exchanged under frameMutex_.
      FfmpegVideoFrame decodeFrame;  ///< Decoder-owned slot reused to publish each new reference.
      std::atomic<bool> decoderStopRequested = false;
      std::atomic<bool> hasFrame = false;
      std::atomic<std::uint64_t> publishedFrameVersion = 0;
      std::atomic<std::uint64_t> submittedDecodeUnitCount = 0;
      std::atomic<std::uint64_t> decodedFrameCount = 0;
      std::atomic<std::uint64_t> droppedDecodeUnitCount = 0;
      std::atomic<std::uint64_t> lastDecodeQueueUs = 0;
      std::atomic<std::uint64_t> lastReceiveAgeUs = 0;
      std::atomic<std::uint64_t> lastDecodedFrameUs = 0;
      std::atomic<int> lastDecodeFrameNumber = 0;
    };

    VideoState video_ {};
    AudioPlayer audio_;  ///< Session Opus decoder and SDL playback device.
    ScreenshotTask screenshot_ {};  ///< Single capture slot, bounded to one additional YUV frame.
    VideoStatistics statistics_;  ///< Cumulative decode and presentation telemetry.
    VideoAccelerationReport acceleration_;  ///< Session diagnostics survive decoder cleanup.
    FfmpegVideoPresenter presentation_ {acceleration_};  ///< Render-thread state and platform presentation paths.
  };

}  // namespace streaming
