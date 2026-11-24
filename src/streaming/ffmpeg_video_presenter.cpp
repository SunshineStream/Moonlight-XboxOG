/**
 * @file src/streaming/ffmpeg_video_presenter.cpp
 * @brief Implements GPU and software presentation of the current decoded picture.
 */
// class header include
#include "src/streaming/ffmpeg_video_presenter.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

// lib includes
extern "C" {
#include <libswscale/swscale.h>
}

#ifdef NXDK
  #include <hal/video.h>
#endif

// local includes
#include "src/logging/logger.h"
#include "src/ui/sdl_render.h"

namespace {
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
}  // namespace

namespace streaming {
  FfmpegVideoPresenter::FfmpegVideoPresenter(VideoAccelerationReport &acceleration):
      acceleration_(acceleration) {}

  FfmpegVideoPresenter::~FfmpegVideoPresenter() {
    reset();
  }

  bool FfmpegVideoPresenter::gpu_presentation_enabled() const {
    return gpuPresentationEnabled_;
  }

  void FfmpegVideoPresenter::swap_queued_frame(FfmpegVideoFrame &queuedFrame) {
    renderFrame_.swap(queuedFrame);
  }

  const FfmpegVideoFrame &FfmpegVideoPresenter::frame() const {
    return renderFrame_;
  }

  bool FfmpegVideoPresenter::present(SDL_Renderer *renderer, int screenWidth, int screenHeight, bool newFrame) {
    const bool presented = render_video_frame(renderer, screenWidth, screenHeight, newFrame);
    if (presented) {
      overlayChanged_ = false;
    }
    return presented;
  }

  void FfmpegVideoPresenter::reset() {
    stop_video_presentation();
    if (statisticsTexture_ != nullptr) {
      SDL_DestroyTexture(statisticsTexture_);
      statisticsTexture_ = nullptr;
    }
    statisticsOverlay_.set_bitmap(0, 0, {});
    saveIndicatorOverlay_.set_bitmap(0, 0, {});
    saveIndicatorFrame_.reset();

    if (texture_ != nullptr) {
      SDL_DestroyTexture(texture_);
      texture_ = nullptr;
    }
    textureWidth_ = 0;
    textureHeight_ = 0;
    sws_freeContext(presentScaleContext_);
    presentScaleContext_ = nullptr;
    renderFrame_.clear();
    overlayFailed_ = false;
    softwareScanoutFailed_ = false;
  }

  void FfmpegVideoPresenter::configure_gpu_presentation(bool enabled, GpuBufferCount bufferCount) {
    gpuPresentationEnabled_ = enabled;
    presenter_.set_buffer_count(bufferCount);
  }

  std::string FfmpegVideoPresenter::presentation_path() const {
    if (presenter_.active()) {
      return "GPU";
    }
    return acceleration_.softwareFrames > 0 ? "Software" : "Waiting";
  }

  void FfmpegVideoPresenter::set_statistics_bitmap(int width, int height, std::vector<std::uint8_t> grayscale, VideoOverlayPlacement placement, std::vector<std::uint8_t> opacity) {
    statisticsOverlay_.set_bitmap(width, height, std::move(grayscale), placement, std::move(opacity));
    invalidate_overlay_texture();
  }

  void FfmpegVideoPresenter::set_color_overlay(int width, int height, std::vector<std::uint8_t> pixels, VideoOverlayPlacement placement) {
    statisticsOverlay_.set_argb_bitmap(width, height, std::move(pixels), placement);
    invalidate_overlay_texture();
  }

  void FfmpegVideoPresenter::set_color_overlay_surface(const std::shared_ptr<SDL_Surface> &surface, VideoOverlayPlacement placement, bool opaqueBackground, int scale) {
    if (!surface || surface->pitch != surface->w * 4) {
      return;
    }
    const auto pixels = std::shared_ptr<const std::uint8_t>(surface, static_cast<const std::uint8_t *>(surface->pixels));
    if (statisticsOverlay_.share_argb_surface(surface->w, surface->h, pixels, static_cast<std::size_t>(surface->pitch) * surface->h, placement, opaqueBackground, scale)) {
      invalidate_overlay_texture();
    }
  }

  void FfmpegVideoPresenter::update_color_overlay(const VideoRectangle &region) {
    if (!statisticsOverlay_.update_shared_argb_surface(region)) {
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
    overlayChanged_ = true;
    statisticsTextureChanged_ = true;
  }

  void FfmpegVideoPresenter::update_save_indicator(Uint64 nowMilliseconds) {
    const auto frame = ui::save_indicator_frame(nowMilliseconds);
    if (frame == saveIndicatorFrame_) {
      return;
    }
    saveIndicatorFrame_ = frame;
    if (!frame.has_value()) {
      if (saveIndicatorOverlay_.width() != 0) {
        saveIndicatorOverlay_.set_bitmap(0, 0, {});
        overlayChanged_ = true;
      }
    } else if (const auto *raster = saveIndicator_.raster(*frame); raster != nullptr && saveIndicatorOverlay_.set_argb_surface(raster->w, raster->h, static_cast<const std::uint8_t *>(raster->pixels), static_cast<std::size_t>(raster->pitch) * raster->h, raster->pitch, VideoOverlayPlacement::bottom_right)) {
      overlayChanged_ = true;
    }
  }

  bool FfmpegVideoPresenter::has_pending_overlay() const {
    return overlayChanged_;
  }

  void FfmpegVideoPresenter::render_local_overlay(SDL_Renderer *renderer, int width, int height) {
    render_sdl_statistics(renderer, {0, 0, width, height});
  }

  void FfmpegVideoPresenter::local_overlay_size(int &width, int &height) const {
    const auto &frame = renderFrame_.view();
    if (frame.width > 0 && frame.height > 0) {
      const auto destination = build_letterboxed_destination(width, height, frame.width, frame.height);
      width = destination.w;
      height = destination.h;
    }
  }

  void FfmpegVideoPresenter::stop_video_presentation() {
    if (!presenter_.reset()) {
      acceleration_.record_fallback("GPU overlay teardown timed out; acceleration disabled");
      logging::warn("stream", "GPU overlay teardown timed out; buffers retained to avoid DMA into freed storage");
    }
    if (!softwareScanout_.reset()) {
      logging::warn("stream", "Software scanout retirement timed out; extra framebuffer retained");
    }
  }

  bool FfmpegVideoPresenter::render_video_frame(SDL_Renderer *renderer, int screenWidth, int screenHeight, bool newFrame) {
    const auto &frame = renderFrame_.view();
    if (frame.width <= 0 || frame.height <= 0) {
      return false;
    }

    const SDL_Rect destination = build_letterboxed_destination(screenWidth, screenHeight, frame.width, frame.height);
#ifdef NXDK
    // The startup dialog may have uploaded a full-size SDL controls texture.
    // Retire it before native presentation allocates its scanout buffers.
    if (!softwareScanoutFailed_ && statisticsTexture_ != nullptr) {
      SDL_DestroyTexture(statisticsTexture_);
      statisticsTexture_ = nullptr;
    }
    if (render_gpu_video_frame(destination, newFrame)) {
      return true;
    }
    if (!softwareScanoutFailed_) {
      if (render_latest_video_frame_to_framebuffer(screenWidth, screenHeight)) {
        if (newFrame) {
          ++acceleration_.softwareFrames;
        }
        return true;
      }
      softwareScanoutFailed_ = true;
      logging::warn("stream", "Double-buffered scanout unavailable; using SDL software presentation");
      stop_video_presentation();
    }
#endif
    return render_sdl_video_frame(renderer, destination, newFrame);
  }

#ifdef NXDK
  bool FfmpegVideoPresenter::render_gpu_video_frame(const SDL_Rect &destination, bool newFrame) {
    if (!gpuPresentationEnabled_ || overlayFailed_) {
      return false;
    }
    const bool alreadyPresenting = presenter_.active();
    if (!newFrame && !overlayChanged_ && alreadyPresenting) {
      return true;
    }
    if (std::string reason; !presenter_.present(renderFrame_.view(), {destination.x, destination.y, destination.w, destination.h}, &reason, {}, &statisticsOverlay_, &saveIndicatorOverlay_)) {
      overlayFailed_ = true;
      acceleration_.record_fallback(reason);
      logging::warn("stream", "GPU presentation fallback: " + reason);
      return false;
    }
    if (!alreadyPresenting) {
      logging::info("stream", "GPU presentation using " + std::to_string(presenter_.active_buffer_count()) + " buffers");
      // The GPU now owns scanout. Discard the startup card on the hidden CPU
      // page so a later reset cannot put it back on screen.
      const VIDEO_MODE mode = XVideoGetMode();
      auto *framebuffer = XVideoGetFB();
      if (framebuffer != nullptr && mode.width > 0 && mode.height > 0 && (mode.bpp == 16 || mode.bpp == 32)) {
        std::memset(framebuffer, 0, static_cast<std::size_t>(mode.width) * static_cast<std::size_t>(mode.height) * static_cast<std::size_t>(mode.bpp / 8));
        XVideoFlushFB();
      }
    }
    if (newFrame) {
      ++acceleration_.gpuFrames;
    }
    return true;
  }
#endif

  bool FfmpegVideoPresenter::render_sdl_video_frame(SDL_Renderer *renderer, const SDL_Rect &destination, bool textureNeedsUpload) {
    const auto &frame = renderFrame_.view();
    if (texture_ == nullptr || textureWidth_ != frame.width || textureHeight_ != frame.height) {
      if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
      }

      texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
      if (texture_ == nullptr) {
        logging::error("stream", std::string("SDL_CreateTexture failed for video presentation: ") + SDL_GetError());
        return false;
      }

      textureWidth_ = frame.width;
      textureHeight_ = frame.height;
      textureNeedsUpload = true;
    }

    if (textureNeedsUpload && !SDL_UpdateYUVTexture(texture_, nullptr, frame.planes[0], frame.pitches[0], frame.planes[1], frame.pitches[1], frame.planes[2], frame.pitches[2])) {
      logging::error("stream", std::string("SDL_UpdateYUVTexture failed during video presentation: ") + SDL_GetError());
      return false;
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    const bool presented = ui::render_texture(renderer, texture_, nullptr, &destination);
    if (presented) {
      render_sdl_statistics(renderer, destination);
      if (saveIndicatorFrame_.has_value() && saveIndicatorOverlay_.width() != 0) {
        const auto bounds = saveIndicatorOverlay_.rectangle({destination.x, destination.y, destination.w, destination.h});
        if (bounds.width > 0 && bounds.height > 0) {
          saveIndicator_.draw(renderer, {bounds.x, bounds.y, bounds.width, bounds.height}, *saveIndicatorFrame_);
        }
      }
      SDL_RenderPresent(renderer);
    }
    if (presented && textureNeedsUpload) {
      ++acceleration_.softwareFrames;
    }
    return presented;
  }

  void FfmpegVideoPresenter::render_sdl_statistics(SDL_Renderer *renderer, const SDL_Rect &destination) {
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
      const auto &region = statisticsTextureDamage_;
      const SDL_Rect damage {region.x, region.y, region.width, region.height};
      void *pixels = nullptr;
      int pitch = 0;
      if (!SDL_LockTexture(statisticsTexture_, &damage, &pixels, &pitch)) {
        return;
      }
      const bool uploaded = statisticsOverlay_.copy_argb_region(region, static_cast<std::uint8_t *>(pixels), pitch);
      SDL_UnlockTexture(statisticsTexture_);
      if (!uploaded) {
        return;
      }
      statisticsTextureChanged_ = false;
    }
    const auto bounds = statisticsOverlay_.rectangle({destination.x, destination.y, destination.w, destination.h});
    const auto bitmap = statisticsOverlay_.bitmap_rectangle(bounds);
    const SDL_Rect panel {bounds.x, bounds.y, bounds.width, bounds.height};
    const SDL_Rect source {bitmap.x, bitmap.y, bitmap.width, bitmap.height};
    ui::render_texture(renderer, statisticsTexture_, &source, &panel);
  }

  void FfmpegVideoPresenter::invalidate_overlay_texture() {
    overlayChanged_ = true;
    statisticsTextureChanged_ = true;
    statisticsTextureDamage_ = {0, 0, statisticsOverlay_.width(), statisticsOverlay_.height()};
    if (statisticsTexture_ != nullptr) {
      float width = 0;
      float height = 0;
      if (!SDL_GetTextureSize(statisticsTexture_, &width, &height) || width != statisticsOverlay_.width() || height != statisticsOverlay_.height()) {
        SDL_DestroyTexture(statisticsTexture_);
        statisticsTexture_ = nullptr;
      }
    }
  }

  bool FfmpegVideoPresenter::render_latest_video_frame_to_framebuffer(int screenWidth, int screenHeight) {
#ifdef NXDK
    const VIDEO_MODE videoMode = XVideoGetMode();
    const int bytesPerPixel = xbox_framebuffer_bytes_per_pixel(videoMode.bpp);
    const AVPixelFormat destinationFormat = xbox_framebuffer_pixel_format(videoMode.bpp);
    if (videoMode.width <= 0 || videoMode.height <= 0 || bytesPerPixel <= 0 || destinationFormat == AV_PIX_FMT_NONE) {
      return false;
    }

    const int framebufferWidth = screenWidth > 0 ? std::min(screenWidth, videoMode.width) : videoMode.width;
    const int framebufferHeight = screenHeight > 0 ? std::min(screenHeight, videoMode.height) : videoMode.height;
    const auto &frame = renderFrame_.view();
    const SDL_Rect destination = build_letterboxed_destination(framebufferWidth, framebufferHeight, frame.width, frame.height);
    if (destination.w <= 0 || destination.h <= 0) {
      return false;
    }

    const int framebufferPitch = videoMode.width * bytesPerPixel;
    const auto framebufferSize = static_cast<std::size_t>(framebufferPitch) * videoMode.height;
    const VideoRectangle videoArea {destination.x, destination.y, destination.w, destination.h};
    auto *framebuffer = softwareScanout_.back_buffer(framebufferSize, videoArea);
    if (framebuffer == nullptr) {
      return false;
    }

    presentScaleContext_ = sws_getCachedContext(
      presentScaleContext_,
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
    if (presentScaleContext_ == nullptr) {
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

    if (const int scaledRows = sws_scale(presentScaleContext_, sourceData.data(), sourceLinesize.data(), 0, frame.height, destinationData.data(), destinationLinesize.data()); scaledRows <= 0) {
      logging::warn("stream", "sws_scale failed for direct framebuffer presentation");
      return false;
    }

    statisticsOverlay_.draw_rgb(framebuffer, framebufferSize, framebufferPitch, videoMode.width, videoMode.height, videoMode.bpp, videoArea);
    saveIndicatorOverlay_.draw_rgb(framebuffer, framebufferSize, framebufferPitch, videoMode.width, videoMode.height, videoMode.bpp, videoArea);
    return softwareScanout_.present();
#else
    (void) screenWidth;
    (void) screenHeight;
    return false;
#endif
  }

}  // namespace streaming
