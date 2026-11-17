/**
 * @file src/input/shell_controller.cpp
 * @brief Implements menu controller recovery across host virtual-gamepad removal and hotplug.
 */
// class header include
#include "src/input/shell_controller.h"

namespace input {
  bool recover_shell_controller(SDL_GameController **controller) {
    if (controller == nullptr || (*controller != nullptr && SDL_GameControllerGetAttached(*controller) == SDL_TRUE)) {
      return false;
    }
    const bool changed = *controller != nullptr;
    if (*controller != nullptr) {
      SDL_GameControllerClose(*controller);
      *controller = nullptr;
    }
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
      if (SDL_IsGameController(index) == SDL_FALSE) {
        continue;
      }
      SDL_GameController *candidate = SDL_GameControllerOpen(index);
      if (candidate == nullptr) {
        continue;
      }
      // Removal and enumeration can overlap. Never retain another detached
      // reference just because SDL still exposed its device index this frame.
      if (SDL_GameControllerGetAttached(candidate) == SDL_FALSE) {
        SDL_GameControllerClose(candidate);
        continue;
      }
      *controller = candidate;
      return true;
    }
    return changed;
  }
}  // namespace input
