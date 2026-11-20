/**
 * @file src/ui/sdl_render.h
 * @brief Draw pixel-aligned UI layouts with SDL3's floating-point renderer.
 */
#pragma once

#include <SDL3/SDL.h>

namespace ui {
  /**
   * @brief Convert a pixel-aligned rectangle to renderer coordinates.
   * @param rect Integer layout bounds.
   * @return Equivalent floating-point bounds.
   */
  inline SDL_FRect render_bounds(const SDL_Rect &rect) {
    return {static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.w), static_cast<float>(rect.h)};
  }

  /**
   * @brief Render a texture between pixel-aligned source and destination bounds.
   * @param renderer Active renderer.
   * @param texture Texture to draw.
   * @param source Source bounds, or null to use the full texture.
   * @param destination Destination bounds, or null to use the full target.
   * @return True if SDL accepted the draw.
   */
  inline bool render_texture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *source, const SDL_Rect *destination) {
    const SDL_FRect src = source ? render_bounds(*source) : SDL_FRect {};
    const SDL_FRect dst = destination ? render_bounds(*destination) : SDL_FRect {};
    return SDL_RenderTexture(renderer, texture, source ? &src : nullptr, destination ? &dst : nullptr);
  }

  /**
   * @brief Fill pixel-aligned bounds with the renderer's current draw color.
   * @param renderer Active renderer.
   * @param rect Rectangle to fill.
   */
  inline void fill_rect(SDL_Renderer *renderer, const SDL_Rect &rect) {
    const auto bounds = render_bounds(rect);
    SDL_RenderFillRect(renderer, &bounds);
  }

  /**
   * @brief Outline pixel-aligned bounds with the renderer's current draw color.
   * @param renderer Active renderer.
   * @param rect Rectangle to outline.
   */
  inline void draw_rect(SDL_Renderer *renderer, const SDL_Rect &rect) {
    const auto bounds = render_bounds(rect);
    SDL_RenderRect(renderer, &bounds);
  }
}  // namespace ui
