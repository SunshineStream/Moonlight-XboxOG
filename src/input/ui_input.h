/**
 * @file src/input/ui_input.h
 * @brief Shared UI navigation timing and keyboard hold tracking.
 */
#pragma once

// standard includes
#include <cstdint>
#include <map>
#include <utility>

// local includes
#include "src/input/navigation_input.h"

namespace input {
  constexpr std::uint32_t NAVIGATION_INITIAL_REPEAT_MILLISECONDS = 150;  ///< Delay shared by keyboard and controller navigation.
  constexpr std::uint32_t NAVIGATION_REPEAT_MILLISECONDS = 45;  ///< Interval shared by keyboard and controller navigation.
  constexpr short STICK_NAVIGATION_THRESHOLD = 16000;  ///< Deflection required to begin directional navigation.
  constexpr short STICK_NAVIGATION_RELEASE_THRESHOLD = 12000;  ///< Lower release threshold prevents duplicate steps from stick jitter.

  /**
   * @brief Device whose intentional input most recently controlled the UI.
   */
  enum class UiInputDevice {
    gamepad,  ///< Show controller button glyphs.
    keyboard,  ///< Show keyboard keycaps.
  };

  /**
   * @brief Decide whether analog input is deliberate enough to select controller hints.
   * @param value SDL axis value.
   * @param trigger True for a unipolar trigger; false for a signed stick axis.
   * @return True outside the UI dead zone; neutral and small drift are ignored.
   */
  bool is_gamepad_ui_activity(short value, bool trigger);

  /**
   * @brief Classify commands that can repeat safely while a keyboard key remains held.
   * @param command Mapped UI command.
   * @return True for directional navigation and paging, never confirmation or toggles.
   */
  bool is_repeatable_ui_command(UiCommand command);

  /**
   * @brief Map signed directional steps to a command, preferring horizontal movement.
   * @param x Horizontal step, positive rightward.
   * @param y Vertical step, positive downward.
   * @return Directional command, or none for a centered input.
   */
  UiCommand direction_command(int x, int y);

  /**
   * @brief Convert a directional command to a single keyboard or grid step.
   * @param command Navigation command.
   * @return Horizontal and vertical steps; zero for other commands.
   */
  std::pair<int, int> navigation_step(UiCommand command);

  /**
   * @brief Generates UI repeats from transitions, including USB keyboards without OS autorepeat.
   */
  class KeyboardNavigation {
  public:
    /**
     * @brief Start with no held navigation keys.
     */
    KeyboardNavigation() = default;
    /**
     * @brief Copy the current hold and repeat state.
     * @param other Navigation state to copy.
     */
    KeyboardNavigation(const KeyboardNavigation &other) = default;
    /**
     * @brief Replace held navigation keys and repeat state with a copy.
     * @param other Navigation state to copy.
     * @return This navigation state.
     */
    KeyboardNavigation &operator=(const KeyboardNavigation &other) = default;
    /**
     * @brief Release retained navigation keys.
     */
    ~KeyboardNavigation() = default;
    /**
     * @brief Transfer held navigation keys without a potentially throwing container move.
     * @param other Navigation state being transferred.
     */
    KeyboardNavigation(KeyboardNavigation &&other) noexcept = default;
    /**
     * @brief Transfer navigation state when replacing the active input owner.
     * @param other Navigation state being transferred.
     * @return This navigation state.
     */
    KeyboardNavigation &operator=(KeyboardNavigation &&other) noexcept = default;
    /**
     * @brief Register an initial key press and issue its command exactly once.
     * @param key Physical key identity, such as an SDL scancode.
     * @param command Mapped command, or none for an unmapped key.
     * @param now Monotonic millisecond tick count.
     * @return Initial command, or none for duplicate presses and unmapped keys.
     */
    UiCommand press(int key, UiCommand command, std::uint32_t now);
    /**
     * @brief Stop repeating a released or disconnected key.
     * @param key Physical key identity.
     */
    void release(int key);
    /**
     * @brief Repeat the most recently pressed navigation key when its timer is due.
     * @param now Monotonic millisecond tick count, allowed to wrap.
     * @return At most one repeat command; long stalls do not generate catch-up bursts.
     */
    UiCommand poll(std::uint32_t now);
    /**
     * @brief Clear held keys on focus loss, device changes or modal activation.
     */
    void reset();

  private:
    /**
     * @brief One keyboard navigation hold.
     */
    struct HeldKey {
      UiCommand command = UiCommand::none;  ///< Command associated with this physical key.
      std::uint32_t pressedAt = 0;  ///< Initial press tick.
      std::uint32_t repeatedAt = 0;  ///< Last emitted repeat tick.
      std::uint64_t order = 0;  ///< Press order, independent of scancode ordering.
    };

    std::map<int, HeldKey> held_;  ///< Current mapped keys, including action keys that must not repeat.
    std::uint64_t order_ = 0;  ///< Monotonic ordering within the current input lifetime.
  };

  /**
   * @brief Repeat one sampled controller direction with the shared keyboard/menu timing.
   */
  class DirectionNavigation {
  public:
    /**
     * @brief Emit a changed direction immediately, then repeat after the shared hold delay.
     * @param command Sampled direction, or none at neutral.
     * @param now Monotonic millisecond tick, allowed to wrap.
     * @return Direction to apply, or none until the next repeat.
     */
    UiCommand update(UiCommand command, std::uint32_t now);
    /**
     * @brief Navigate along the dominant stick axis using the main menus' hysteresis and repeat timing.
     * @param x Signed horizontal deflection, positive rightward.
     * @param y Signed vertical deflection, positive upward.
     * @param now Monotonic millisecond tick, allowed to wrap.
     * @return At most one directional step, or none at neutral or before a repeat is due.
     */
    UiCommand update_stick(short x, short y, std::uint32_t now);
    /**
     * @brief Clear the held direction on a page or input-device transition.
     */
    void reset();

  private:
    UiCommand command_ = UiCommand::none;  ///< Currently held direction.
    std::uint32_t pressedAt_ = 0;  ///< First sample of the held direction.
    std::uint32_t repeatedAt_ = 0;  ///< Most recent emitted repeat.
    bool repeated_ = false;  ///< The initial repeat delay has elapsed.
  };

  /**
   * @brief Repeat timing for one held controller direction.
   */
  struct ControllerNavigationHoldState {
    bool active = false;  ///< Direction is held.
    std::uint32_t activatedTick = 0;  ///< Initial activation tick.
    std::uint32_t lastRepeatTick = 0;  ///< Last repeat tick.
  };

  /**
   * @brief Directional repeat state for the shell's primary controller.
   */
  struct ControllerNavigationHolds {
    ControllerNavigationHoldState up {};  ///< Up repeat state.
    ControllerNavigationHoldState down {};  ///< Down repeat state.
    ControllerNavigationHoldState left {};  ///< Left repeat state.
    ControllerNavigationHoldState right {};  ///< Right repeat state.
  };

#ifdef MOONLIGHT_PORTABLE
  /**
   * @brief Origin and timing of the active shell touch gesture.
   */
  struct ShellTouchGesture {
    std::uint64_t finger = 0;  ///< Active touch ID, or zero when idle.
    std::uint64_t startedAt = 0;  ///< Touch timestamp used to distinguish a tap from a hold.
    float x = 0;  ///< Normalized horizontal origin.
    float y = 0;  ///< Normalized vertical origin.
  };
#endif

  /**
   * @brief Shell input lifetime, separate from the stream's input ownership.
   */
  struct ShellInputState {
    KeyboardNavigation keyboardNavigation;  ///< Keyboard repeat state.
    bool leftTriggerPressed = false;  ///< Held-input or navigation latch.
    bool rightTriggerPressed = false;  ///< Held-input or navigation latch.
    bool leftShoulderPressed = false;  ///< Held-input or navigation latch.
    bool rightShoulderPressed = false;  ///< Held-input or navigation latch.
    bool controllerStartPressed = false;  ///< Held-input or navigation latch.
    bool controllerBackPressed = false;  ///< Held-input or navigation latch.
    bool controllerExitComboArmed = false;  ///< Held-input or navigation latch.
    bool controllerExitComboTriggered = false;  ///< Held-input or navigation latch.
    bool controllerNavigationNeutralRequired = false;  ///< Held-input or navigation latch.
    std::uint32_t controllerStartDownTick = 0;  ///< Press or repeat tick for this input.
    std::uint32_t controllerBackDownTick = 0;  ///< Press or repeat tick for this input.
    std::uint32_t leftShoulderRepeatTick = 0;  ///< Press or repeat tick for this input.
    std::uint32_t rightShoulderRepeatTick = 0;  ///< Press or repeat tick for this input.
    std::uint32_t leftTriggerRepeatTick = 0;  ///< Press or repeat tick for this input.
    std::uint32_t rightTriggerRepeatTick = 0;  ///< Press or repeat tick for this input.
    ControllerNavigationHolds controllerNavigation;  ///< Directional repeat state.
#ifdef MOONLIGHT_PORTABLE
    ShellTouchGesture touch;  ///< Active gesture used for shell navigation.
#endif

    /**
     * @brief Clear stale holds after streaming consumed their release events, then wait for neutral.
     */
    void reset_after_stream() {
      *this = {};
      controllerNavigationNeutralRequired = true;
    }

    /**
     * @brief Clear a removed gamepad's holds while preserving any independent keyboard repeat.
     */
    void reset_after_controller_change() {
      auto keyboard = std::move(keyboardNavigation);
      *this = {};
      keyboardNavigation = std::move(keyboard);
      controllerNavigationNeutralRequired = true;
    }
  };

}  // namespace input
