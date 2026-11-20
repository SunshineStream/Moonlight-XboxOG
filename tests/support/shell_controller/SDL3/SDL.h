/**
 * @file tests/support/shell_controller/SDL3/SDL.h
 * @brief Minimal SDL3 gamepad API for native ownership and hotplug tests.
 */
#pragma once

// standard includes
#include <cstdint>

/** @brief Opaque SDL gamepad handle implemented by the test device registry. */
struct SDL_Gamepad;
using SDL_JoystickID = std::uint32_t;  ///< SDL3 gamepad instance ID.

extern "C" {
  /**
   * @brief Snapshot currently mapped gamepads.
   * @param count Receives the number of IDs.
   * @return SDL-owned allocation terminated by zero, or null on error.
   */
  SDL_JoystickID *SDL_GetGamepads(int *count);
  /**
   * @brief Acquire one SDL reference to a gamepad.
   * @param instance Stable SDL instance ID.
   * @return Owned handle, or null on failure.
   */
  SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance);
  /**
   * @brief Release one owned gamepad reference in the test registry.
   * @param gamepad Stable handle identifying the registry entry.
   */
  void SDL_CloseGamepad(const SDL_Gamepad *gamepad);
  /**
   * @brief Check whether an open gamepad still has a connected device.
   * @param gamepad Open gamepad reference.
   * @return True while the device is attached.
   */
  bool SDL_GamepadConnected(const SDL_Gamepad *gamepad);
  /**
   * @brief Free SDL-owned enumeration data.
   * @param memory Allocation returned by SDL, or null.
   */
  void SDL_free(void *memory);
}
