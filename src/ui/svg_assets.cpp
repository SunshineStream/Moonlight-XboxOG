/**
 * @file src/ui/svg_assets.cpp
 * @brief Rasterizes shared shell and streaming SVG assets with SDL3_image.
 */
// class header include
#include "src/ui/svg_assets.h"

// lib includes
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

namespace ui {
  SDL_Surface *load_svg_surface(const std::string &assetPath, int maxWidth, int maxHeight) {
    if (maxWidth <= 0 || maxHeight <= 0) {
      return nullptr;
    }

    SDL_IOStream *rw = SDL_IOFromFile(assetPath.c_str(), "rb");
    if (rw == nullptr) {
      return nullptr;
    }

    size_t dataSize = 0U;
    SDL_IOStream *file = SDL_IOFromFile(assetPath.c_str(), "rb");
    if (file == nullptr) {
      return nullptr;
    }

    SDL_Surface *surface = IMG_LoadSizedSVG_IO(file, maxWidth, maxHeight);
    SDL_CloseIO(file);
    if (surface == nullptr) {
      return nullptr;
    }

    if (!SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND)) {
      SDL_DestroySurface(surface);
      return nullptr;
    }

    return surface;
  }

}  // namespace ui
