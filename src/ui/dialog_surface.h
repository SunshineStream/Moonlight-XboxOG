/**
 * @file src/ui/dialog_surface.h
 * @brief Shared dialog surface for shell and in-stream modals on both SDL render paths.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cmath>

// lib includes
#include <SDL.h>

namespace ui {
  /**
   * @brief Paint rounded horizontal spans without renderer clipping or GPU-specific primitives.
   * @param renderer Active SDL renderer.
   * @param rect Surface bounds.
   * @param color Surface color.
   * @param radius Corner radius.
   */
  inline void draw_dialog_round_rect(SDL_Renderer *renderer, SDL_Rect rect, SDL_Color color, int radius) {
    radius = std::max(0, std::min(radius, std::min(rect.w, rect.h) / 2));
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_Rect middle {rect.x, rect.y + radius, rect.w, rect.h - radius * 2};
    SDL_RenderFillRect(renderer, &middle);
    for (int y = 0; y < radius; ++y) {
      const int dy = radius - y - 1;
      const int inset = radius - static_cast<int>(std::sqrt(radius * radius - dy * dy));
      SDL_Rect row {rect.x + inset, rect.y + y, rect.w - inset * 2, 1};
      SDL_RenderFillRect(renderer, &row);
      row.y = rect.y + rect.h - y - 1;
      SDL_RenderFillRect(renderer, &row);
    }
  }

  /**
   * @brief Draw the shared opaque navy dialog, shadow and blue accent.
   * @param renderer Active SDL renderer.
   * @param rect Dialog bounds.
   */
  inline void draw_dialog_surface(SDL_Renderer *renderer, const SDL_Rect &rect) {
    SDL_BlendMode previousBlend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &previousBlend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    draw_dialog_round_rect(renderer, {rect.x + 5, rect.y + 7, rect.w, rect.h}, {0, 0, 0, 0x70}, 12);
    draw_dialog_round_rect(renderer, rect, {0x15, 0x1E, 0x2E, 255}, 12);
    draw_dialog_round_rect(renderer, {rect.x + 20, rect.y + 16, 36, 3}, {0x82, 0xB4, 0xFF, 255}, 1);
    SDL_SetRenderDrawBlendMode(renderer, previousBlend);
  }

  /**
   * @brief Paint a progress bar with separate completed, active and pending segments.
   * @param renderer Active SDL renderer.
   * @param rect Full progress bar bounds.
   * @param step Zero-based index of the active segment.
   * @param count Total number of segments.
   */
  inline void draw_dialog_progress(SDL_Renderer *renderer, SDL_Rect rect, int step, int count) {
    constexpr int gap = 4;
    const int trackWidth = rect.w - gap * (count - 1);
    if (count <= 0 || trackWidth < count || rect.h <= 0) {
      return;
    }
    for (int segment = 0; segment < count; ++segment) {
      const int left = segment * trackWidth / count;
      const int right = (segment + 1) * trackWidth / count;
      const SDL_Color activeOrPending = segment == step ? SDL_Color {0x4D, 0x77, 0xB3, 255} : SDL_Color {0x30, 0x3D, 0x52, 255};
      const SDL_Color color = segment < step ? SDL_Color {0x82, 0xB4, 0xFF, 255} : activeOrPending;
      draw_dialog_round_rect(renderer, {rect.x + left + gap * segment, rect.y, right - left, rect.h}, color, 2);
    }
  }
}  // namespace ui
