/**
 * @file src/input/shell_controller.h
 * @brief Recovers the shell's active gamepad without depending on rendering or device-added events.
 */
#pragma once

// lib includes
#include <SDL.h>

namespace input {
  /**
   * @brief Preserve a working controller, or acquire an already attached replacement after removal.
   *
   * The caller owns the returned SDL reference. A missing device is retried on
   * later input iterations, including when the shell retains an idle frame.
   *
   * @param controller Owned menu controller reference to preserve or replace; null is ignored.
   * @return True when the owned menu reference changed, so the caller can reset stale gamepad holds.
   */
  bool recover_shell_controller(SDL_GameController **controller);
}  // namespace input
