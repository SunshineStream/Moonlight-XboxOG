/**
 * @file src/ui/mobile_orientation.h
 * @brief Declares the mobile window's stream orientation control.
 */
#pragma once

// lib includes
#include <SDL3/SDL.h>

namespace ui {

#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS)
  /**
   * @brief Restrict a handheld window to landscape for a stream or restore menu rotation.
   * @param window SDL window shared by the menu and stream.
   * @param streaming True while starting or running a stream.
   */
  void set_mobile_streaming_orientation(SDL_Window *window, bool streaming);
#endif

}  // namespace ui
