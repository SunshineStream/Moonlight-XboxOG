/**
 * @file src/input/shell_controller.cpp
 * @brief Implements menu controller recovery across host virtual-gamepad removal and hotplug.
 */
// class header include
#include "src/input/shell_controller.h"

// standard includes
#include <memory>

namespace input {
  bool recover_shell_controller(SDL_Gamepad **controller) {
    if (controller == nullptr || (*controller != nullptr && SDL_GamepadConnected(*controller))) {
      return false;
    }
    const bool changed = *controller != nullptr;
    if (*controller != nullptr) {
      SDL_CloseGamepad(*controller);
      *controller = nullptr;
    }
    int count = 0;
    const std::unique_ptr<SDL_JoystickID, decltype(&SDL_free)> ids(SDL_GetGamepads(&count), SDL_free);
    for (int index = 0; ids && index < count; ++index) {
      SDL_Gamepad *candidate = SDL_OpenGamepad(ids.get()[index]);
      if (candidate == nullptr) {
        continue;
      }
      // Removal and enumeration can overlap. Never retain another detached
      // reference just because SDL still exposed its instance ID this frame.
      if (!SDL_GamepadConnected(candidate)) {
        SDL_CloseGamepad(candidate);
        continue;
      }
      *controller = candidate;
      return true;
    }
    return changed;
  }
}  // namespace input
