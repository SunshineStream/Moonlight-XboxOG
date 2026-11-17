/**
 * @file tests/support/shell_controller/SDL.h
 * @brief Minimal SDL controller API for native ownership and hotplug regression tests.
 */
#pragma once

/**
 * @brief SDL's boolean values used by the production recovery adapter.
 */
enum class SDL_bool {
  SDL_FALSE = 0,  ///< No or detached.
  SDL_TRUE = 1,  ///< Yes or attached.
};

constexpr auto SDL_FALSE = SDL_bool::SDL_FALSE;  ///< SDL-compatible false value.
constexpr auto SDL_TRUE = SDL_bool::SDL_TRUE;  ///< SDL-compatible true value.

/**
 * @brief Opaque SDL controller handle implemented by the test device registry.
 */
struct _SDL_GameController;
/**
 * @brief SDL-compatible public controller handle name.
 */
using SDL_GameController = _SDL_GameController;

extern "C" {
  /**
   * @brief Count currently enumerable joystick devices.
   * @return Device count.
   */
  int SDL_NumJoysticks();
  /**
   * @brief Check whether an enumerable device has a gamepad mapping.
   * @param index Current device index.
   * @return True for mapped gamepads.
   */
  SDL_bool SDL_IsGameController(int index);
  /**
   * @brief Acquire one SDL reference to a mapped gamepad.
   * @param index Current device index.
   * @return Owned handle, or null on failure.
   */
  SDL_GameController *SDL_GameControllerOpen(int index);
  /**
   * @brief Release one owned gamepad reference.
   * @param controller Owned handle being closed.
   */
  void SDL_GameControllerClose(const SDL_GameController *controller);
  /**
   * @brief Check whether an open gamepad still has a connected device.
   * @param controller Open controller reference.
   * @return True while the device is attached.
   */
  SDL_bool SDL_GameControllerGetAttached(const SDL_GameController *controller);
}
