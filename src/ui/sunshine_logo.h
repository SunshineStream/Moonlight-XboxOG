/**
 * @file src/ui/sunshine_logo.h
 * @brief Loads and composites the four animated Sunshine logo arcs.
 */
#pragma once

// standard includes
#include <memory>

// lib includes
#include <SDL3/SDL.h>

// local includes
#include "src/splash/splash_animation.h"

namespace ui {

  /**
   * @brief Cache a raster atlas of Sunshine logo arcs for software composition.
   */
  class SunshineLogo {
  public:
    /**
     * @brief Load the atlas at the output size, including framebuffer aspect correction.
     * @param width Width of one arc's tile in framebuffer pixels.
     * @param height Height of one arc's tile in framebuffer pixels.
     * @return True when the atlas is ready to draw.
     */
    bool load(int width, int height);

    /**
     * @brief Composite each arc over the destination at its assigned opacity.
     * @param surface Destination framebuffer or transparent overlay.
     * @param destination Position and size of the loaded logo in the destination.
     * @param opacity One alpha multiplier per arc in SVG drawing order.
     * @return True when all arcs were drawn.
     */
    bool draw(SDL_Surface *surface, const SDL_Rect &destination, const splash::LogoOpacity &opacity);

  private:
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> atlas_ {nullptr, SDL_DestroySurface};  ///< Four full-size tiles in SVG drawing order.
  };

}  // namespace ui
