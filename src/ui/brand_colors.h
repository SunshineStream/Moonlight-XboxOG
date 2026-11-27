/**
 * @file src/ui/brand_colors.h
 * @brief Shared Sunshine colors for the shell, splash and stream overlays.
 */
#pragma once

// lib includes
#include <SDL3/SDL.h>

namespace ui::brand {
  inline constexpr SDL_Color BACKGROUND {0x17, 0x13, 0x18, 0xFF};  ///< Base shell and splash background.
  inline constexpr SDL_Color PANEL {0x2A, 0x22, 0x28, 0xFF};  ///< Standard panel background.
  inline constexpr SDL_Color PANEL_ALT {0x22, 0x1B, 0x21, 0xFF};  ///< Secondary panel background.
  inline constexpr SDL_Color ACCENT {0xFF, 0xC1, 0x5C, 0xFF};  ///< Focus, selection and progress.
  inline constexpr SDL_Color TEXT {0xFF, 0xF5, 0xEA, 0xFF};  ///< Primary text and icons.
  inline constexpr SDL_Color MUTED {0xC5, 0xB7, 0xAD, 0xFF};  ///< Secondary and inactive text.
}  // namespace ui::brand
