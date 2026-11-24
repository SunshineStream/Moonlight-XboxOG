/**
 * @file tests/unit/input/ui_input_test.cpp
 * @brief Verifies keyboard navigation holds, input-source arbitration and repeat-safe commands.
 */
// test header include
#include "src/input/ui_input.h"

// standard includes
#include <array>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>

// lib includes
#include <lizardbyte/common/testing.h>

/**
 * @brief Shared grid commands preserve direction, horizontal priority and neutral input.
 */
TEST(UiInputTest, MapsDirectionalCommandsToGridSteps) {
  const std::array<input::UiCommand, 4> commands {input::UiCommand::move_left, input::UiCommand::move_right, input::UiCommand::move_up, input::UiCommand::move_down};
  for (const auto command : commands) {
    const auto [x, y] = input::navigation_step(command);
    EXPECT_EQ(input::direction_command(x, y), command);
  }
  EXPECT_EQ(input::direction_command(2, -3), input::UiCommand::move_right);
  EXPECT_EQ(input::direction_command(-2, 3), input::UiCommand::move_left);
  EXPECT_EQ(input::direction_command(0, 0), input::UiCommand::none);
  EXPECT_EQ(input::navigation_step(input::UiCommand::none), (std::pair<int, int> {0, 0}));
  EXPECT_EQ(input::navigation_step(input::UiCommand::activate), (std::pair<int, int> {0, 0}));
}

/**
 * @brief Keyboard navigation uses the same initial delay and interval as controller navigation.
 */
TEST(KeyboardNavigationTest, RepeatsHeldArrowsWithoutOperatingSystemEvents) {
  input::KeyboardNavigation navigation;
  EXPECT_EQ(navigation.poll(100), input::UiCommand::none);
  EXPECT_EQ(navigation.press(1, input::UiCommand::move_down, 100), input::UiCommand::move_down);
  EXPECT_EQ(navigation.poll(249), input::UiCommand::none);
  EXPECT_EQ(navigation.poll(250), input::UiCommand::move_down);
  EXPECT_EQ(navigation.poll(294), input::UiCommand::none);
  EXPECT_EQ(navigation.poll(295), input::UiCommand::move_down);
  EXPECT_EQ(navigation.poll(10000), input::UiCommand::move_down);
  EXPECT_EQ(navigation.poll(10000), input::UiCommand::none);
  navigation.release(1);
  EXPECT_EQ(navigation.poll(11000), input::UiCommand::none);
}

/**
 * @brief Duplicate press events neither navigate twice nor postpone a held key's initial repeat.
 */
TEST(KeyboardNavigationTest, IgnoresDuplicatePressesAndUnknownKeys) {
  input::KeyboardNavigation navigation;
  EXPECT_EQ(navigation.press(1, input::UiCommand::move_left, 100), input::UiCommand::move_left);
  EXPECT_EQ(navigation.press(1, input::UiCommand::move_left, 200), input::UiCommand::none);
  EXPECT_EQ(navigation.press(2, input::UiCommand::none, 220), input::UiCommand::none);
  EXPECT_EQ(navigation.poll(250), input::UiCommand::move_left);
  navigation.release(400);
  EXPECT_EQ(navigation.poll(295), input::UiCommand::move_left);
}

/**
 * @brief The most recently pressed direction wins, with a still-held earlier direction restored on release.
 */
TEST(KeyboardNavigationTest, ResolvesOverlappingNavigationKeysInPressOrder) {
  input::KeyboardNavigation navigation;
  navigation.press(20, input::UiCommand::move_down, 100);
  EXPECT_EQ(navigation.press(10, input::UiCommand::move_up, 200), input::UiCommand::move_up);
  EXPECT_EQ(navigation.poll(349), input::UiCommand::none);
  EXPECT_EQ(navigation.poll(350), input::UiCommand::move_up);
  navigation.release(10);
  EXPECT_EQ(navigation.poll(351), input::UiCommand::move_down);
  navigation.reset();
  EXPECT_EQ(navigation.poll(2000), input::UiCommand::none);
}

/**
 * @brief Confirmations and toggles fire once and stop an old held arrow from carrying into a new modal.
 */
TEST(KeyboardNavigationTest, NeverRepeatsActivationOrToggleCommands) {
  for (const auto command : {input::UiCommand::activate, input::UiCommand::confirm, input::UiCommand::back, input::UiCommand::open_context_menu, input::UiCommand::toggle_overlay, input::UiCommand::delete_character}) {
    input::KeyboardNavigation navigation;
    navigation.press(1, input::UiCommand::move_down, 0);
    EXPECT_EQ(navigation.press(2, command, 10), command);
    EXPECT_EQ(navigation.press(2, command, 20), input::UiCommand::none);
    EXPECT_EQ(navigation.poll(1000), input::UiCommand::none);
    navigation.release(2);
    EXPECT_EQ(navigation.poll(2000), input::UiCommand::none);
  }
}

/**
 * @brief Paging keys repeat and the repeat clock remains correct across the SDL tick counter's wraparound.
 */
TEST(KeyboardNavigationTest, RepeatsPagingAcrossTickWraparound) {
  for (const auto command : {input::UiCommand::move_right, input::UiCommand::previous_page, input::UiCommand::next_page, input::UiCommand::fast_previous_page, input::UiCommand::fast_next_page}) {
    input::KeyboardNavigation navigation;
    const auto start = std::numeric_limits<std::uint32_t>::max() - 80;
    EXPECT_EQ(navigation.press(1, command, start), command);
    EXPECT_EQ(navigation.poll(start + 149), input::UiCommand::none);
    EXPECT_EQ(navigation.poll(start + 150), command);
    EXPECT_EQ(navigation.poll(start + 195), command);
  }
}

/**
 * @brief Centered sticks, small drift and released triggers do not replace keyboard helper icons.
 */
TEST(UiInputActivityTest, RequiresDeliberateAnalogInput) {
  for (short value : {0, 1, -1, 15999, -15999}) {
    EXPECT_FALSE(input::is_gamepad_ui_activity(value, false));
  }
  for (short value : {16000, -16000, 32767, -32768}) {
    EXPECT_TRUE(input::is_gamepad_ui_activity(value, false));
  }
  EXPECT_FALSE(input::is_gamepad_ui_activity(-32768, true));
  EXPECT_FALSE(input::is_gamepad_ui_activity(15999, true));
  EXPECT_TRUE(input::is_gamepad_ui_activity(16000, true));
  EXPECT_TRUE(input::is_gamepad_ui_activity(32767, true));
}

/**
 * @brief Returning from a stream clears swallowed key releases, controller holds, repeat clocks and exit latches.
 */
TEST(ShellInputStateTest, ResumesNavigationAfterStreamingConsumesReleaseEvents) {
  input::ShellInputState state;
  state.keyboardNavigation.press(1, input::UiCommand::move_down, 100);
  state.keyboardNavigation.press(2, input::UiCommand::confirm, 110);
  state.leftTriggerPressed = true;
  state.rightTriggerPressed = true;
  state.leftShoulderPressed = true;
  state.rightShoulderPressed = true;
  state.controllerStartPressed = true;
  state.controllerBackPressed = true;
  state.controllerExitComboArmed = true;
  state.controllerExitComboTriggered = true;
  state.controllerStartDownTick = 120;
  state.controllerBackDownTick = 120;
  state.leftShoulderRepeatTick = 120;
  state.rightShoulderRepeatTick = 120;
  state.leftTriggerRepeatTick = 120;
  state.rightTriggerRepeatTick = 120;
  state.controllerNavigation.up = {true, 100, 200};
  state.controllerNavigation.down = {true, 100, 200};
  state.controllerNavigation.left = {true, 100, 200};
  state.controllerNavigation.right = {true, 100, 200};
  state.reset_after_stream();
  EXPECT_FALSE(state.leftTriggerPressed || state.rightTriggerPressed || state.leftShoulderPressed || state.rightShoulderPressed);
  EXPECT_FALSE(state.controllerStartPressed || state.controllerBackPressed || state.controllerExitComboArmed || state.controllerExitComboTriggered);
  EXPECT_EQ(state.controllerStartDownTick + state.controllerBackDownTick + state.leftShoulderRepeatTick + state.rightShoulderRepeatTick + state.leftTriggerRepeatTick + state.rightTriggerRepeatTick, 0);
  for (const auto &direction : {state.controllerNavigation.up, state.controllerNavigation.down, state.controllerNavigation.left, state.controllerNavigation.right}) {
    EXPECT_FALSE(direction.active);
    EXPECT_EQ(direction.activatedTick, 0);
    EXPECT_EQ(direction.lastRepeatTick, 0);
  }
  EXPECT_TRUE(state.controllerNavigationNeutralRequired);
  EXPECT_EQ(state.keyboardNavigation.poll(10000), input::UiCommand::none);
  EXPECT_EQ(state.keyboardNavigation.press(1, input::UiCommand::move_down, 10000), input::UiCommand::move_down);
  EXPECT_EQ(state.keyboardNavigation.poll(10150), input::UiCommand::move_down);
  EXPECT_EQ(state.keyboardNavigation.press(2, input::UiCommand::confirm, 10200), input::UiCommand::confirm);
}

/**
 * @brief A gamepad replacement clears stale controller input while an independently held keyboard key keeps repeating.
 */
TEST(ShellInputStateTest, ControllerHandoffPreservesKeyboardAndResetsGamepadHolds) {
  input::ShellInputState state;
  state.leftTriggerPressed = true;
  state.rightTriggerPressed = true;
  state.leftShoulderPressed = true;
  state.rightShoulderPressed = true;
  state.controllerStartPressed = true;
  state.controllerBackPressed = true;
  state.controllerExitComboArmed = true;
  state.controllerExitComboTriggered = true;
  state.controllerStartDownTick = 100;
  state.controllerBackDownTick = 100;
  state.leftShoulderRepeatTick = 100;
  state.rightShoulderRepeatTick = 100;
  state.leftTriggerRepeatTick = 100;
  state.rightTriggerRepeatTick = 100;
  state.controllerNavigation.up = {true, 100, 200};
  state.controllerNavigation.down = {true, 100, 200};
  state.controllerNavigation.left = {true, 100, 200};
  state.controllerNavigation.right = {true, 100, 200};
  EXPECT_EQ(state.keyboardNavigation.press(1, input::UiCommand::move_right, 100), input::UiCommand::move_right);
  state.reset_after_controller_change();
  EXPECT_FALSE(state.leftTriggerPressed || state.rightTriggerPressed || state.leftShoulderPressed || state.rightShoulderPressed);
  EXPECT_FALSE(state.controllerStartPressed || state.controllerBackPressed || state.controllerExitComboArmed || state.controllerExitComboTriggered);
  EXPECT_EQ(state.controllerStartDownTick + state.controllerBackDownTick + state.leftShoulderRepeatTick + state.rightShoulderRepeatTick + state.leftTriggerRepeatTick + state.rightTriggerRepeatTick, 0U);
  for (const auto &hold : {state.controllerNavigation.up, state.controllerNavigation.down, state.controllerNavigation.left, state.controllerNavigation.right}) {
    EXPECT_FALSE(hold.active);
    EXPECT_EQ(hold.activatedTick, 0U);
    EXPECT_EQ(hold.lastRepeatTick, 0U);
  }
  EXPECT_TRUE(state.controllerNavigationNeutralRequired);
  EXPECT_EQ(state.keyboardNavigation.poll(249), input::UiCommand::none);
  EXPECT_EQ(state.keyboardNavigation.poll(250), input::UiCommand::move_right);
  state.keyboardNavigation.release(1);
  EXPECT_EQ(state.keyboardNavigation.poll(500), input::UiCommand::none);
  EXPECT_EQ(state.keyboardNavigation.press(2, input::UiCommand::confirm, 500), input::UiCommand::confirm);
  state.reset_after_controller_change();
  EXPECT_EQ(state.keyboardNavigation.press(2, input::UiCommand::confirm, 600), input::UiCommand::none);
  state.keyboardNavigation.release(2);
  EXPECT_EQ(state.keyboardNavigation.press(2, input::UiCommand::confirm, 700), input::UiCommand::confirm);
}

/**
 * @brief Controller direction changes respond immediately and held movement matches keyboard repeats.
 */
TEST(DirectionNavigationTest, RespondsImmediatelyAndUsesSharedRepeatTiming) {
  input::DirectionNavigation navigation;
  EXPECT_EQ(navigation.update(input::UiCommand::move_down, 100), input::UiCommand::move_down);
  EXPECT_EQ(navigation.update(input::UiCommand::move_down, 249), input::UiCommand::none);
  EXPECT_EQ(navigation.update(input::UiCommand::move_down, 250), input::UiCommand::move_down);
  EXPECT_EQ(navigation.update(input::UiCommand::move_down, 294), input::UiCommand::none);
  EXPECT_EQ(navigation.update(input::UiCommand::move_down, 295), input::UiCommand::move_down);
  EXPECT_EQ(navigation.update(input::UiCommand::move_left, 296), input::UiCommand::move_left);
  EXPECT_EQ(navigation.update(input::UiCommand::move_right, 297), input::UiCommand::move_right);
  EXPECT_EQ(navigation.update(input::UiCommand::move_right, 10000), input::UiCommand::move_right);
  EXPECT_EQ(navigation.update(input::UiCommand::move_right, 10000), input::UiCommand::none);
}

/**
 * @brief Neutral, actions and page transitions cancel a hold, including when the clock wraps.
 */
TEST(DirectionNavigationTest, ClearsOnNeutralOrResetAndHandlesClockWrap) {
  input::DirectionNavigation navigation;
  constexpr auto start = std::numeric_limits<std::uint32_t>::max() - 100;
  EXPECT_EQ(navigation.update(input::UiCommand::move_up, start), input::UiCommand::move_up);
  EXPECT_EQ(navigation.update(input::UiCommand::move_up, 49), input::UiCommand::move_up);
  EXPECT_EQ(navigation.update(input::UiCommand::none, 50), input::UiCommand::none);
  EXPECT_EQ(navigation.update(input::UiCommand::move_up, 51), input::UiCommand::move_up);
  EXPECT_EQ(navigation.update(input::UiCommand::confirm, 52), input::UiCommand::none);
  EXPECT_EQ(navigation.update(input::UiCommand::move_up, 53), input::UiCommand::move_up);
  navigation.reset();
  EXPECT_EQ(navigation.update(input::UiCommand::move_up, 54), input::UiCommand::move_up);
}

/**
 * @brief Both stick axes use the main menu dead zone, retain a noisy hold and release without a second tap.
 */
TEST(DirectionNavigationTest, StickFlicksStepOnceWithHysteresisOnBothAxes) {
  const std::array<std::tuple<short, short, input::UiCommand>, 4> axes {{{20000, 0, input::UiCommand::move_right}, {-20000, 0, input::UiCommand::move_left}, {0, 20000, input::UiCommand::move_up}, {0, -20000, input::UiCommand::move_down}}};
  for (const auto &[x, y, direction] : axes) {
    input::DirectionNavigation navigation;
    EXPECT_EQ(navigation.update_stick(x / 2, y / 2, 0), input::UiCommand::none);
    EXPECT_EQ(navigation.update_stick(x, y, 1), direction);
    EXPECT_EQ(navigation.update_stick(x * 3 / 5, y * 3 / 5, 40), input::UiCommand::none);
    EXPECT_EQ(navigation.update_stick(x * 7 / 10, y * 7 / 10, 80), input::UiCommand::none);
    EXPECT_EQ(navigation.update_stick(x, y, 149), input::UiCommand::none);
    EXPECT_EQ(navigation.update_stick(0, 0, 150), input::UiCommand::none);
    EXPECT_EQ(navigation.update_stick(x / 2, y / 2, 151), input::UiCommand::none);
    EXPECT_EQ(navigation.update_stick(x, y, 152), direction);
    EXPECT_EQ(navigation.update_stick(x, y, 302), direction);
  }
}

/**
 * @brief Dominant axis changes respond immediately, including ties and the signed axis endpoints.
 */
TEST(DirectionNavigationTest, StickChangesDirectionWithoutWaitingForRepeat) {
  input::DirectionNavigation navigation;
  EXPECT_EQ(navigation.update_stick(32767, -32000, 0), input::UiCommand::move_right);
  EXPECT_EQ(navigation.update_stick(32767, -32768, 1), input::UiCommand::move_down);
  EXPECT_EQ(navigation.update_stick(-32768, 0, 2), input::UiCommand::move_left);
  EXPECT_EQ(navigation.update_stick(16000, 16000, 3), input::UiCommand::move_up);
  EXPECT_EQ(navigation.update_stick(0, 11999, 4), input::UiCommand::none);
  EXPECT_EQ(navigation.update_stick(0, 15999, 5), input::UiCommand::none);
  EXPECT_EQ(navigation.update_stick(0, 16000, 6), input::UiCommand::move_up);
}
