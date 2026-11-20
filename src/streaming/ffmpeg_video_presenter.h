/**
 * @file src/streaming/ffmpeg_video_presenter.h
 * @brief Presents a decoded FFmpeg frame and its overlay on the active display.
 */
#pragma once

// standard includes
#include <memory>
#include <string>
#include <vector>

// lib includes
#include <SDL3/SDL.h>

// local includes
#include "src/streaming/ffmpeg_video_frame.h"
#include "src/streaming/pvideo_presenter.h"
#include "src/streaming/software_scanout.h"

struct SwsContext;

namespace streaming {
  /**
   * @brief Owns render-thread state for GPU, direct framebuffer, and SDL presentation.
   */
  class FfmpegVideoPresenter {
  public:
    /**
     * @brief Start with an empty presentation state.
     * @param acceleration Session diagnostics shared with the decoder.
     */
    explicit FfmpegVideoPresenter(VideoAccelerationReport &acceleration);

    /** @brief Retire scanout and release textures, scaling state, and frame buffers. */
    ~FfmpegVideoPresenter();

    /** @brief Prevent copying the presentation resources. */
    FfmpegVideoPresenter(const FfmpegVideoPresenter &) = delete;

    /** @brief Prevent copying the presentation resources. */
    FfmpegVideoPresenter &operator=(const FfmpegVideoPresenter &) = delete;

    /**
     * @brief Select the preferred presentation path before starting the session.
     * @param enabled True to try NV2A with automatic software fallback.
     */
    void set_gpu_presentation_enabled(bool enabled);

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
     * @brief Retain the SDL panel for compositing without duplicating its color storage.
     * @param surface Premultiplied ARGB8888 surface owned by the controls.
     * @param placement Shared GPU/software panel anchor.
     * @param opaqueBackground Source was rendered onto an opaque panel.
     * @param scale Output pixels per raster sample.
     */
    void set_color_overlay_surface(const std::shared_ptr<SDL_Surface> &surface, VideoOverlayPlacement placement, bool opaqueBackground = false, int scale = 1);

    /**
     * @brief Submit a small focus repaint while retaining the rest of the panel and its conversion caches.
     * @param region Updated rectangle in panel pixels.
     */
    void update_color_overlay(const VideoRectangle &region);

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
     * @brief Resolve the video rectangle available to shared GPU/software controls.
     * @param width Output width, replaced with the fitted video width.
     * @param height Output height, replaced with the fitted video height.
     */
    void local_overlay_size(int &width, int &height) const;

    /**
     * @brief Read the configured GPU preference.
     * @return True if the session prefers GPU presentation.
     */
    bool gpu_presentation_enabled() const;

    /**
     * @brief Acquire a new display frame, returning the previous reference to the mailbox.
     * @param queuedFrame Latest decoded frame, locked by the caller.
     */
    void swap_queued_frame(FfmpegVideoFrame &queuedFrame);

    /**
     * @brief Access the held display frame so a screenshot can retain its own reference.
     * @return Read-only frame, with zero dimensions before the first picture.
     */
    const FfmpegVideoFrame &frame() const;

    /**
     * @brief Present the current frame and clear the pending overlay update on success.
     * @param renderer SDL renderer used for fallback.
     * @param screenWidth Current output width.
     * @param screenHeight Current output height.
     * @param newFrame True when the caller acquired a newly decoded frame.
     * @return True if the output is up to date.
     */
    bool present(SDL_Renderer *renderer, int screenWidth, int screenHeight, bool newFrame);

    /** @brief Restore CPU framebuffer output and clear session-owned presentation resources. */
    void reset();

  private:
    /**
     * @brief Restore CPU framebuffer output before drawing shutdown or summary UI on the render thread.
     */
    void stop_video_presentation();

    /**
     * @brief Present a frame on the first available output path.
     * @param renderer SDL video renderer.
     * @param screenWidth Output width.
     * @param screenHeight Output height.
     * @param newFrame True when the caller acquired a newly decoded frame.
     * @return True after successful presentation.
     */
    bool render_video_frame(SDL_Renderer *renderer, int screenWidth, int screenHeight, bool newFrame);

    /**
     * @brief Present the current video and UI through the GPU overlay when available.
     *
     * Clear the old startup page once the first GPU frame has taken over the display.
     *
     * @param destination Output video rectangle.
     * @param newFrame True when a new decoded frame was acquired.
     * @return True when GPU presentation is active and the output is up to date.
     */
    bool render_gpu_video_frame(const SDL_Rect &destination, bool newFrame);

    /**
     * @brief Upload and present the current video and UI through SDL.
     * @param renderer SDL video renderer.
     * @param destination Output video rectangle.
     * @param textureNeedsUpload True when a new decoded frame was acquired.
     * @return True after successful SDL presentation.
     */
    bool render_sdl_video_frame(SDL_Renderer *renderer, const SDL_Rect &destination, bool textureNeedsUpload);

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
     * @brief Present the latest decoded frame through a platform-specific direct framebuffer path.
     *
     * @param screenWidth Current framebuffer width.
     * @param screenHeight Current framebuffer height.
     * @return True when the frame was presented without SDL renderer involvement.
     */
    bool render_latest_video_frame_to_framebuffer(int screenWidth, int screenHeight);

    VideoAccelerationReport &acceleration_;  ///< Session diagnostics, owned by the backend.
    FfmpegVideoFrame renderFrame_;  ///< Frame retained for redraws and captures.
    SwsContext *presentScaleContext_ = nullptr;  ///< Software framebuffer conversion context.
    SDL_Texture *texture_ = nullptr;  ///< SDL fallback video texture.
    int textureWidth_ = 0;  ///< Width of the uploaded SDL video texture.
    int textureHeight_ = 0;  ///< Height of the uploaded SDL video texture.
    PvideoPresenter presenter_;  ///< Native GPU scanout.
    SoftwareScanout softwareScanout_;  ///< Native direct framebuffer scanout.
    VideoOverlay statisticsOverlay_;  ///< Shared cached panel for all presentation paths.
    SDL_Texture *statisticsTexture_ = nullptr;  ///< Cached panel texture for SDL's final fallback.
    bool statisticsBitmapChanged_ = false;  ///< A panel update requires refreshing a paused video frame.
    bool statisticsTextureChanged_ = false;  ///< SDL texture requires the current bitmap.
    VideoRectangle statisticsTextureDamage_ {};  ///< Pending SDL texture upload.
    bool gpuPresentationEnabled_ = false;  ///< GPU preference for the session.
    bool overlayFailed_ = false;  ///< Avoid retrying a failed GPU path in the same session.
    bool softwareScanoutFailed_ = false;  ///< Retain the SDL fallback after native software scanout fails.
  };
}  // namespace streaming
