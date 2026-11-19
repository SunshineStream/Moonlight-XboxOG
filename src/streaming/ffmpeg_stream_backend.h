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
#include "third-party/moonlight-common-c/src/Limelight.h"

#include <SDL.h>

// local includes
#include "src/app/screenshot_library.h"
#include "src/streaming/aligned_media_buffer.h"
#include "src/streaming/audio_player.h"
#include "src/streaming/ffmpeg_video_frame.h"
#include "src/streaming/pvideo_presenter.h"
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
     * @brief Select the preferred presentation path before starting the session.
     * @param enabled True to try NV2A with automatic software fallback.
     */
    void set_gpu_presentation_enabled(bool enabled);

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
     * @brief Describe the active presentation path.
     *
     * @return GPU, Software or Waiting.
     */
    std::string presentation_path() const;
    /**
     * @brief Replace the shared GPU/software statistics bitmap on the render thread.
     * @param width Bitmap width.
     * @param height Bitmap height.
     * @param grayscale Packed, unpremultiplied grayscale pixels.
     * @param placement Panel anchor shared by GPU and software paths.
     * @param opacity Optional per-pixel coverage; empty for opaque statistics.
     */
    void set_statistics_bitmap(int width, int height, std::vector<std::uint8_t> grayscale, VideoOverlayPlacement placement = VideoOverlayPlacement::top_left, std::vector<std::uint8_t> opacity = {});
    /**
     * @brief Replace the same shared overlay with colored UI pixels on the render thread.
     * @param width Bitmap width.
     * @param height Bitmap height.
     * @param pixels Straight ARGB bytes preserving the bundled assets' colors and opacity.
     * @param placement Panel anchor shared by GPU and software paths.
     */
    void set_color_overlay(int width, int height, std::vector<std::uint8_t> pixels, VideoOverlayPlacement placement = VideoOverlayPlacement::top_left);
    /**
     * @brief Replace the shared panel directly from SDL storage, reusing its full-screen color allocation.
     * @param width Source width.
     * @param height Source height.
     * @param pixels Premultiplied B, G, R, A source bytes.
     * @param capacity Accessible source byte count.
     * @param pitch Source row stride.
     * @param placement Shared GPU/software panel anchor.
     * @param opaqueBackground Source was rendered onto an opaque panel.
     */
    void set_color_overlay_surface(int width, int height, const std::uint8_t *pixels, std::size_t capacity, int pitch, VideoOverlayPlacement placement, bool opaqueBackground = false);
    /**
     * @brief Submit a small focus repaint while retaining the rest of the panel and its conversion caches.
     * @param region Updated rectangle in panel pixels.
     * @param pixels Packed straight ARGB bytes for that rectangle.
     */
    void update_color_overlay(const VideoRectangle &region, const std::vector<std::uint8_t> &pixels);

    /**
     * @brief Check whether an overlay change requires presenting an otherwise idle frame.
     * @return True when the shared bitmap changed since the last presentation.
     */
    bool has_pending_overlay() const;

    /**
     * @brief Draw local controls before a stream has produced its first video frame.
     * @param renderer SDL renderer.
     * @param width Output width.
     * @param height Output height.
     */
    void render_local_overlay(SDL_Renderer *renderer, int width, int height);

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
     * @brief Resolve the video rectangle available to shared GPU/software controls.
     * @param width Output width, replaced with the fitted video width.
     * @param height Output height, replaced with the fitted video height.
     */
    void local_overlay_size(int &width, int &height) const;

    /**
     * @brief Render the latest decoded video frame to the supplied renderer.
     *
     * @param renderer SDL renderer used by the stream session.
     * @param screenWidth Current renderer output width.
     * @param screenHeight Current renderer output height.
     * @param allowDirectFramebuffer True when platform-specific direct presentation may bypass SDL's renderer.
     * @return True when a decoded frame was available and rendered.
     */
    bool render_latest_video_frame(SDL_Renderer *renderer, int screenWidth, int screenHeight, bool allowDirectFramebuffer = true);

    /**
     * @brief Restore CPU framebuffer output before drawing shutdown or summary UI on the render thread.
     */
    void stop_video_presentation();

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
     * @brief Present a frame while the public wrapper records submission time.
     * @param renderer SDL video renderer.
     * @param screenWidth Output width.
     * @param screenHeight Output height.
     * @param allowDirectFramebuffer Permit Xbox GPU or direct framebuffer presentation.
     * @return True after successful presentation.
     */
    bool render_video_frame(SDL_Renderer *renderer, int screenWidth, int screenHeight, bool allowDirectFramebuffer);
    /**
     * @brief Draw the cached panel when the SDL presentation fallback is used.
     * @param renderer SDL video renderer.
     * @param destination Output video rectangle.
     */
    void render_sdl_statistics(SDL_Renderer *renderer, const SDL_Rect &destination);
    /**
     * @brief Mark pixels changed, retaining the SDL texture when its dimensions still match.
     */
    void invalidate_overlay_texture();
    /**
     * @brief Record a retrieved frame before any client discard.
     *
     * @param unit Completed network frame.
     */
    void record_received_unit(PDECODE_UNIT unit);
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
     * @brief Present the latest decoded frame through a platform-specific direct framebuffer path.
     *
     * @param screenWidth Current framebuffer width.
     * @param screenHeight Current framebuffer height.
     * @return True when the frame was presented without SDL renderer involvement.
     */
    bool render_latest_video_frame_to_framebuffer(int screenWidth, int screenHeight);

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
      SwsContext *presentScaleContext = nullptr;
      AVFrame *decodedFrame = nullptr;
      AVFrame *convertedFrame = nullptr;
      AVPacket *packet = nullptr;
      const char *codecName = "video";  ///< Human-readable active video decoder name.
      SDL_Texture *texture = nullptr;
      SDL_Thread *decoderThread = nullptr;
      int textureWidth = 0;
      int textureHeight = 0;
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
      SDL_Rect directFramebufferDestination {0, 0, 0, 0};
      FfmpegVideoFrame latestFrame;  ///< Mailbox slot exchanged under frameMutex_.
      FfmpegVideoFrame decodeFrame;  ///< Decoder-owned slot reused to publish each new reference.
      FfmpegVideoFrame renderFrame;  ///< Render-thread reference retained for presentation and UI redraws.
      bool directFramebufferCleared = false;
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
    PvideoPresenter presenter_;  ///< Main-thread GPU presentation resources.
    VideoOverlay statisticsOverlay_;  ///< Shared cached panel for all presentation paths.
    VideoStatistics statistics_;  ///< Cumulative decode and presentation telemetry.
    SDL_Texture *statisticsTexture_ = nullptr;  ///< Cached panel texture for SDL's final fallback.
    bool statisticsBitmapChanged_ = false;  ///< A panel update requires refreshing a paused video frame.
    bool statisticsTextureChanged_ = false;  ///< SDL texture requires the current bitmap.
    VideoRectangle statisticsTextureDamage_ {};  ///< Pending SDL texture upload, accumulated until software presentation.
    VideoAccelerationReport acceleration_;  ///< Session diagnostics survive decoder cleanup.
    bool gpuPresentationEnabled_ = false;  ///< Session opt-in applied before decoder setup.
    bool overlayFailed_ = false;  ///< Avoid repeating a known failing hardware path this session.
  };

}  // namespace streaming
