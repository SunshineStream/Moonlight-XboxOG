/**
 * @file src/ui/save_indicator.cpp
 * @brief Draws a small Sunshine glow during file saves.
 */
// class header include
#include "src/ui/save_indicator.h"

// local includes
#include "src/app/file_save_activity.h"
#include "src/splash/splash_animation.h"
#include "src/ui/save_indicator_timing.h"
#include "src/ui/sdl_render.h"

namespace ui {
  std::optional<std::uint64_t> save_indicator_frame(std::uint64_t nowMilliseconds) {
    const auto save = app::file_save_snapshot();
    static SaveIndicatorTiming timing(save.generation);
    return timing.sample(nowMilliseconds, save);
  }

  SDL_Surface *SaveIndicator::raster(std::uint64_t frameMilliseconds) {
    if (!loadAttempted_) {
      loadAttempted_ = true;
      if (logo_.load(SIZE, SIZE)) {
        image_.reset(SDL_CreateSurface(SIZE, SIZE, SDL_PIXELFORMAT_ARGB8888));
      }
    }

    // Blending the arcs onto transparent pixels produces premultiplied ARGB.
    if (!image_ || !SDL_FillSurfaceRect(image_.get(), nullptr, 0) || !logo_.draw(image_.get(), {0, 0, SIZE, SIZE}, splash::logo_glow_opacity(frameMilliseconds))) {
      return nullptr;
    }
    return image_.get();
  }

  void SaveIndicator::draw(SDL_Renderer *renderer, const SDL_Rect &bounds, std::uint64_t frameMilliseconds) {
    if (auto *surface = raster(frameMilliseconds); surface != nullptr) {
      const std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(SDL_CreateTextureFromSurface(renderer, surface), SDL_DestroyTexture);
      if (texture && SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED)) {
        render_texture(renderer, texture.get(), nullptr, &bounds);
      }
    }
  }
}  // namespace ui
