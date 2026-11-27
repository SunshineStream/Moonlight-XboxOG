/**
 * @file src/ui/sunshine_logo.cpp
 * @brief Rasterizes and composites the animated Sunshine logo atlas with SDL.
 */
// class header include
#include "src/ui/sunshine_logo.h"

// standard includes
#include <algorithm>
#include <utility>

// local includes
#include "src/ui/asset_path.h"
#include "src/ui/svg_assets.h"

namespace ui {

  bool SunshineLogo::load(int width, int height) {
    constexpr auto arcCount = static_cast<int>(splash::LOGO_ARC_COUNT);
    const int size = std::max(width, height);
    atlas_.reset(load_svg_surface(build_asset_path("sunshine-logo-arcs.svg"), size * arcCount, size));
    if (!atlas_) {
      return false;
    }
    if (atlas_->w != atlas_->h * arcCount) {
      return SDL_SetError("Invalid Sunshine logo atlas dimensions");
    }
    if (atlas_->w == width * arcCount && atlas_->h == height) {
      return true;
    }

    // Scale once before drawing on an Xbox mode with non-square pixels.
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> scaled(SDL_CreateSurface(width * arcCount, height, atlas_->format), SDL_DestroySurface);
    if (!scaled || !SDL_SetSurfaceBlendMode(atlas_.get(), SDL_BLENDMODE_NONE) || !SDL_BlitSurfaceScaled(atlas_.get(), nullptr, scaled.get(), nullptr, SDL_SCALEMODE_LINEAR) || !SDL_SetSurfaceBlendMode(scaled.get(), SDL_BLENDMODE_BLEND)) {
      return false;
    }
    atlas_ = std::move(scaled);
    return true;
  }

  bool SunshineLogo::draw(SDL_Surface *surface, const SDL_Rect &destination, const splash::LogoOpacity &opacity) {
    for (std::size_t arc = 0; arc < opacity.size(); ++arc) {
      const SDL_Rect tile {static_cast<int>(arc) * destination.w, 0, destination.w, destination.h};
      if (!SDL_SetSurfaceAlphaMod(atlas_.get(), opacity[arc]) || !SDL_BlitSurface(atlas_.get(), &tile, surface, &destination)) {
        return false;
      }
    }
    return true;
  }

}  // namespace ui
