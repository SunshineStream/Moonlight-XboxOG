/**
 * @file src/splash/splash_screen.h
 * @brief Declares the splash screen workflow.
 */
#pragma once

// standard includes
#include <functional>

// nxdk includes
#include "src/platform/video_mode.h"

#include <SDL3/SDL.h>

namespace splash {

  /**
   * @brief Play the splash animation and show the complete logo within a fixed duration.
   *
   * @param window SDL window used for splash rendering.
   * @param videoMode Active video mode used to size the splash layout.
   * @param durationMilliseconds Visible splash duration in milliseconds.
   */
  void show_splash_screen(SDL_Window *window, const VIDEO_MODE &videoMode, unsigned int durationMilliseconds = 1500U);

  /**
   * @brief Reveal the logo, loop its glow until startup completes and then briefly show it in full color.
   *
   * @param window SDL window used for splash rendering.
   * @param videoMode Active video mode used to size the splash layout.
   * @param keepShowing Callback that returns true while startup is still in progress.
   */
  void show_splash_screen(SDL_Window *window, const VIDEO_MODE &videoMode, const std::function<bool()> &keepShowing);

}  // namespace splash
