/**
 * @file src/ui/svg_assets.h
 * @brief Shared SVG rasterization for the shell and streaming controls.
 */
#pragma once

#include <string>

struct SDL_Surface;

namespace ui {
  /**
   * @brief Load a transparent RGBA SVG raster fitted to the requested bounds.
   * @param assetPath Full path to the SVG file.
   * @param maxWidth Maximum raster width in pixels.
   * @param maxHeight Maximum raster height in pixels.
   * @return Owned SDL surface to free with SDL_FreeSurface, or nullptr on failure.
   */
  SDL_Surface *load_svg_surface(const std::string &assetPath, int maxWidth, int maxHeight);
}  // namespace ui
