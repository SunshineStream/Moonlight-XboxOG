/**
 * @file src/ui/save_indicator.h
 * @brief Shared Moonlight save indicator artwork for the shell and streaming overlays.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <optional>

// lib includes
#include <SDL3/SDL.h>

namespace ui {
  /**
   * @brief Sample the indicator on the shared shell and stream render thread.
   * @param nowMilliseconds Monotonic UI clock.
   * @return Sample time during a tracked save or its minimum display time, or no frame when hidden.
   */
  std::optional<std::uint64_t> save_indicator_frame(std::uint64_t nowMilliseconds);

  /**
   * @brief Cache a small raster of the splash logo's dimming wave.
   */
  class SaveIndicator {
  public:
    static constexpr int SIZE = 40;  ///< Square raster size in pixels.

    /**
     * @brief Render the next indicator image, reusing its allocated surfaces.
     * @param frameMilliseconds Time sampled at the indicator frame rate.
     * @return Borrowed premultiplied ARGB8888 raster, or null if the artwork is unavailable.
     */
    SDL_Surface *raster(std::uint64_t frameMilliseconds);

    /**
     * @brief Draw the indicator into an SDL frame.
     * @param renderer Active SDL renderer.
     * @param bounds Destination rectangle in the caller's coordinates.
     * @param frameMilliseconds Time sampled at the indicator frame rate.
     */
    void draw(SDL_Renderer *renderer, const SDL_Rect &bounds, std::uint64_t frameMilliseconds);

  private:
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> wedges_ {nullptr, SDL_DestroySurface};  ///< Small clockwise wedge atlas.
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image_ {nullptr, SDL_DestroySurface};  ///< Composited indicator frame.
    bool loadAttempted_ = false;  ///< Avoid reading the asset repeatedly after a failure.
  };
}  // namespace ui
