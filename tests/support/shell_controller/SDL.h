/**
 * @file tests/support/shell_controller/SDL.h
 * @brief Minimal SDL controller API for native ownership and hotplug regression tests.
 */
#pragma once

/**
 * @brief SDL's boolean values used by the production recovery adapter.
 */
enum SDL_bool {
  SDL_FALSE = 0,  ///< No or detached.
  SDL_TRUE = 1,  ///< Yes or attached.
};

/**
 * @brief Opaque SDL controller handle implemented by the test device registry.
 */
typedef struct _SDL_GameController SDL_GameController;

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
  void SDL_GameControllerClose(SDL_GameController *controller);
  /**
   * @brief Check whether an open gamepad still has a connected device.
   * @param controller Open controller reference.
   * @return True while the device is attached.
   */
  SDL_bool SDL_GameControllerGetAttached(SDL_GameController *controller);
}
