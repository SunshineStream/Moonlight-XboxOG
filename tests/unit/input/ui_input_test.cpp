/**
 * @file tests/unit/input/ui_input_test.cpp
 * @brief Verifies keyboard navigation holds, input-source arbitration and repeat-safe commands.
 */
// test includes
#include <lizardbyte/common/testing.h>

// standard includes
#include <cstdint>
#include <limits>

// local includes
#include "src/input/ui_input.h"

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
  state.leftTriggerPressed = state.rightTriggerPressed = true;
  state.leftShoulderPressed = state.rightShoulderPressed = true;
  state.controllerStartPressed = state.controllerBackPressed = true;
  state.controllerExitComboArmed = state.controllerExitComboTriggered = true;
  state.controllerStartDownTick = state.controllerBackDownTick = 120;
  state.leftShoulderRepeatTick = state.rightShoulderRepeatTick = 120;
  state.leftTriggerRepeatTick = state.rightTriggerRepeatTick = 120;
  state.moveUpHoldState = state.moveDownHoldState = state.moveLeftHoldState = state.moveRightHoldState = {true, 100, 200};
  state.reset_after_stream();
  EXPECT_FALSE(state.leftTriggerPressed || state.rightTriggerPressed || state.leftShoulderPressed || state.rightShoulderPressed);
  EXPECT_FALSE(state.controllerStartPressed || state.controllerBackPressed || state.controllerExitComboArmed || state.controllerExitComboTriggered);
  EXPECT_EQ(state.controllerStartDownTick + state.controllerBackDownTick + state.leftShoulderRepeatTick + state.rightShoulderRepeatTick + state.leftTriggerRepeatTick + state.rightTriggerRepeatTick, 0);
  for (const auto &direction : {state.moveUpHoldState, state.moveDownHoldState, state.moveLeftHoldState, state.moveRightHoldState}) {
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
