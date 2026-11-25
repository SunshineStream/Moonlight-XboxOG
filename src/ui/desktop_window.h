/**
 * @file src/ui/desktop_window.h
 * @brief Window actions shared by the desktop shell and stream controls.
 */
#pragma once

// standard includes
#include <string>

// lib includes
#include <SDL3/SDL.h>

// local includes
#include "src/logging/logger.h"

namespace ui {
  /**
   * @brief Switch between borderless desktop fullscreen and the previous windowed state.
   * @param windowId Window that received the fullscreen shortcut.
   */
  inline void toggle_desktop_fullscreen(SDL_WindowID windowId) {
    if (SDL_Window *window = SDL_GetWindowFromID(windowId); window != nullptr) {
      const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
      if (!SDL_SetWindowFullscreen(window, !fullscreen)) {
        logging::warn("sdl", std::string("Could not change fullscreen mode: ") + SDL_GetError());
      }
    }
  }
}  // namespace ui
