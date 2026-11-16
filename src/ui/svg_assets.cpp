/**
 * @file src/ui/svg_assets.cpp
 * @brief Rasterizes shared shell and streaming SVG assets with NanoSVG.
 */
#include "src/ui/svg_assets.h"

#include <algorithm>
#include <nanosvg.h>
#include <nanosvgrast.h>
#include <SDL.h>

namespace ui {
  SDL_Surface *load_svg_surface(const std::string &assetPath, int maxWidth, int maxHeight) {
    if (maxWidth <= 0 || maxHeight <= 0) {
      return nullptr;
    }

    SDL_RWops *rw = SDL_RWFromFile(assetPath.c_str(), "rb");
    if (rw == nullptr) {
      return nullptr;
    }

    size_t dataSize = 0U;
    auto *data = static_cast<char *>(SDL_LoadFile_RW(rw, &dataSize, 1));
    if (data == nullptr) {
      return nullptr;
    }

    NSVGimage *image = nsvgParse(data, "px", 96.0f);
    SDL_free(data);
    if (image == nullptr) {
      return nullptr;
    }

    int targetWidth = 0;
    int targetHeight = 0;
    if (image->width > 0 && image->height > 0) {
      targetWidth = maxWidth;
      targetHeight = maxHeight;
      if (image->width * static_cast<double>(maxHeight) > image->height * static_cast<double>(maxWidth)) {
        targetHeight = std::max(1, static_cast<int>(image->height * static_cast<double>(maxWidth) / image->width));
      } else {
        targetWidth = std::max(1, static_cast<int>(image->width * static_cast<double>(maxHeight) / image->height));
      }
    }
    if (targetWidth <= 0 || targetHeight <= 0) {
      nsvgDelete(image);
      return nullptr;
    }

    NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
    if (rasterizer == nullptr) {
      nsvgDelete(image);
      return nullptr;
    }

    SDL_Surface *surface = SDL_CreateRGBSurface(
      SDL_SWSURFACE,
      targetWidth,
      targetHeight,
      32,
      0x000000FF,
      0x0000FF00,
      0x00FF0000,
      0xFF000000
    );
    if (surface == nullptr) {
      nsvgDeleteRasterizer(rasterizer);
      nsvgDelete(image);
      return nullptr;
    }

    const bool mustLockSurface = SDL_MUSTLOCK(surface) != 0;
    if (mustLockSurface && SDL_LockSurface(surface) != 0) {
      SDL_FreeSurface(surface);
      nsvgDeleteRasterizer(rasterizer);
      nsvgDelete(image);
      return nullptr;
    }

    SDL_memset(surface->pixels, 0, static_cast<size_t>(surface->pitch) * static_cast<size_t>(surface->h));
    const float rasterScale = std::min(static_cast<float>(targetWidth) / image->width, static_cast<float>(targetHeight) / image->height);
    nsvgRasterize(rasterizer, image, 0.0f, 0.0f, rasterScale, static_cast<unsigned char *>(surface->pixels), surface->w, surface->h, surface->pitch);

    if (mustLockSurface) {
      SDL_UnlockSurface(surface);
    }

    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);

    if (SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND) != 0) {
      SDL_FreeSurface(surface);
      return nullptr;
    }

    return surface;
  }

}  // namespace ui
