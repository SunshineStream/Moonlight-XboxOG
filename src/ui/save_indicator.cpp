/**
 * @file src/ui/save_indicator.cpp
 * @brief Draws a small Moonlight dimming wave during file saves.
 */
// class header include
#include "src/ui/save_indicator.h"

// local includes
#include "src/app/file_save_activity.h"
#include "src/splash/splash_animation.h"
#include "src/ui/asset_path.h"
#include "src/ui/save_indicator_timing.h"
#include "src/ui/sdl_render.h"
#include "src/ui/svg_assets.h"

namespace ui {
  std::optional<std::uint64_t> save_indicator_frame(std::uint64_t nowMilliseconds) {
    const auto save = app::file_save_snapshot();
    static SaveIndicatorTiming timing(save.generation);
    return timing.sample(nowMilliseconds, save);
  }

  SDL_Surface *SaveIndicator::raster(std::uint64_t frameMilliseconds) {
    constexpr int tile = SIZE / 2;
    if (!loadAttempted_) {
      loadAttempted_ = true;
      wedges_.reset(load_svg_surface(build_asset_path("moonlight-logo-wedges.svg"), tile * static_cast<int>(splash::SPLASH_WEDGE_COUNT), tile));
      if (wedges_ && wedges_->h == tile && wedges_->w == tile * static_cast<int>(splash::SPLASH_WEDGE_COUNT)) {
        image_.reset(SDL_CreateSurface(SIZE, SIZE, SDL_PIXELFORMAT_ARGB8888));
      }
    }
    // Blending the wedges onto transparent pixels produces premultiplied ARGB.
    if (!image_ || !SDL_FillSurfaceRect(image_.get(), nullptr, 0)) {
      return nullptr;
    }

    const auto brightness = splash::logo_wave_brightness(frameMilliseconds);
    constexpr SDL_Rect quadrants[] {{0, 0, tile, tile}, {tile, 0, tile, tile}, {tile, tile, tile, tile}, {0, tile, tile, tile}};
    for (std::size_t i = 0; i < brightness.size(); ++i) {
      const SDL_Rect source {static_cast<int>(i) * tile, 0, tile, tile};
      const auto color = brightness[i];
      if (!SDL_SetSurfaceColorMod(wedges_.get(), color, color, color) || !SDL_BlitSurface(wedges_.get(), &source, image_.get(), &quadrants[i / 2])) {
        return nullptr;
      }
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
