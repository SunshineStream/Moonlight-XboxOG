/**
 * @file tests/unit/input/stream_menu_test.cpp
 * @brief Verifies dual-stick sectors, dead zones, keyboard modifiers and log navigation.
 */
// test includes
#include <lizardbyte/common/testing.h>

// standard includes
#include <array>
#include <set>

// local includes
#include "src/input/stream_menu.h"
#include "third-party/moonlight-common-c/src/Limelight.h"

/**
 * @brief Both sticks select all eight actions and never select an action in the dead zone.
 */
TEST(StreamMenuTest, SelectsEverySectorAndRetainsCenteredSelection) {
  input::StreamMenu menu;
  menu.open(42);
  EXPECT_EQ(menu.owner, 42);
  EXPECT_EQ(menu.selected, input::StreamAction::none);
  EXPECT_FALSE(menu.select_stick(false, input::StreamMenu::WHEEL_DEADZONE - 1, -input::StreamMenu::WHEEL_DEADZONE + 1));
  EXPECT_EQ(menu.selected, input::StreamAction::none);
  const std::array<short, 4> x {0, 32767, 0, -32768};
  const std::array<short, 4> y {32767, 0, -32768, 0};
  for (int wheel = 0; wheel < 2; ++wheel) {
    for (int sector = 0; sector < 4; ++sector) {
      EXPECT_TRUE(menu.select_stick(wheel != 0, x[sector], y[sector]));
      EXPECT_EQ(static_cast<int>(menu.selected), wheel * 4 + sector);
      EXPECT_FALSE(menu.select_stick(wheel != 0, x[sector], y[sector]));
      EXPECT_FALSE(menu.select_stick(wheel != 0, 0, 0));
      EXPECT_EQ(static_cast<int>(menu.selected), wheel * 4 + sector);
    }
  }
  menu.page = input::StreamMenuPage::keyboard;
  EXPECT_FALSE(menu.select_stick(false, 0, 32767));
  EXPECT_EQ(menu.selected, input::StreamAction::statistics);
  menu.close();
  EXPECT_EQ(menu.page, input::StreamMenuPage::closed);
  EXPECT_EQ(menu.selected, input::StreamAction::none);
}

/**
 * @brief Sample-only input keeps updating a selected sector and a stationary stick cannot steal focus.
 */
TEST(StreamMenuTest, UpdatesSampledSelectionAfterTheFirstSector) {
  input::StreamMenu menu;
  menu.open(1);
  EXPECT_TRUE(menu.sample_sticks(0, 9000, 0, 0));
  EXPECT_EQ(menu.selected, input::StreamAction::keyboard);
  EXPECT_TRUE(menu.sample_sticks(9000, 0, 0, 0));
  EXPECT_EQ(menu.selected, input::StreamAction::mouse);
  EXPECT_TRUE(menu.sample_sticks(9000, 0, 0, 9000));
  EXPECT_EQ(menu.selected, input::StreamAction::guide);
  EXPECT_FALSE(menu.sample_sticks(9000, 0, 0, 9000));
  EXPECT_EQ(menu.selected, input::StreamAction::guide);
  EXPECT_FALSE(menu.sample_sticks(9000, 0, 0, 0));
  EXPECT_EQ(menu.selected, input::StreamAction::guide);
  EXPECT_TRUE(menu.sample_sticks(-9000, 0, 0, 0));
  EXPECT_EQ(menu.selected, input::StreamAction::quit);
  menu.close();
  menu.open(1);
  EXPECT_TRUE(menu.sample_sticks(-9000, 0, 0, 0));
  EXPECT_EQ(menu.selected, input::StreamAction::quit);
}

/**
 * @brief Keyboard focus follows the QWERTY grid and preserves its row across tall controls.
 */
TEST(StreamMenuTest, NavigatesWideAndTallKeyboardCells) {
  input::StreamMenu menu;
  menu.open(-1);
  menu.page = input::StreamMenuPage::keyboard;
  EXPECT_EQ(menu.keyboard_key(), 'Q');
  menu.navigate(-1, 0);
  EXPECT_EQ(menu.keyboard_key(), 0x125);
  menu.navigate(1, 0);
  EXPECT_EQ(menu.keyboard_key(), 'Q');
  menu.navigate(0, 1);
  EXPECT_EQ(menu.keyboard_key(), 'A');
  menu.navigate(0, -1);
  EXPECT_EQ(menu.keyboard_key(), 'Q');
  menu.navigate(0, -1);
  EXPECT_EQ(menu.keyboard_key(), '1');
  menu.navigate(0, -1);
  EXPECT_EQ(menu.keyboard_key(), 0x20);
  menu.navigate(0, 1);
  EXPECT_EQ(menu.keyboard_key(), '1');
  menu.navigate(0, -1);
  EXPECT_EQ(menu.keyboard_key(), 0x20);
  menu.navigate(1, 0);
  EXPECT_EQ(menu.keyboard_key(), 0x0D);
  menu.navigate(-1, 0);
  EXPECT_EQ(menu.keyboard_key(), 0x20);
  menu.navigate(0, 1);
  EXPECT_EQ(menu.keyboard_key(), '0');
}

/**
 * @brief Every host key remains accessible, with complete geometry on all three layouts.
 */
TEST(StreamMenuTest, PreservesHostKeysAcrossThreeLayouts) {
  input::StreamMenu menu;
  menu.open(-1);
  menu.page = input::StreamMenuPage::keyboard;
  std::set<int> codes;
  for (int page = 0; page < 3; ++page) {
    EXPECT_EQ(menu.keyboardLayout, page);
    std::array<int, input::StreamMenu::KEYBOARD_COLUMNS * input::StreamMenu::KEYBOARD_ROWS> coverage {};
    for (const auto &cell : menu.keyboard_cells()) {
      if (cell.key < static_cast<int>(input::StreamMenu::KEYS.size())) {
        codes.insert(input::StreamMenu::KEYS[cell.key].code);
      }
      for (int y = cell.row; y < cell.row + cell.height; ++y) {
        for (int x = cell.column; x < cell.column + cell.width; ++x) {
          ++coverage.at(y * input::StreamMenu::KEYBOARD_COLUMNS + x);
        }
      }
    }
    for (int count : coverage) {
      EXPECT_EQ(count, 1);
    }
    menu.keyIndex = 41;
    EXPECT_FALSE(menu.key_label(41).empty());
    EXPECT_EQ(menu.keyboard_key(), 0);
  }
  EXPECT_EQ(menu.keyboardLayout, 0);
  for (const auto &key : input::StreamMenu::KEYS) {
    EXPECT_EQ(codes.count(key.code), 1) << key.label;
  }
}

/**
 * @brief One-shot Shift survives local toggles, affects one key and reverses persistent Caps for letters.
 */
TEST(StreamMenuTest, ConsumesShiftOnceAndKeepsCapsSeparate) {
  input::StreamMenu menu;
  menu.open(1);
  menu.page = input::StreamMenuPage::keyboard;
  menu.toggle_shift();
  EXPECT_EQ(menu.key_label(10), "Q");
  EXPECT_EQ(menu.key_modifiers('Q'), MODIFIER_SHIFT);
  EXPECT_EQ(menu.key_label(0), "!");
  menu.key_sent();
  EXPECT_FALSE(menu.shiftOnce);
  EXPECT_EQ(menu.key_modifiers('Q'), 0);
  menu.keyIndex = 43;
  EXPECT_EQ(menu.keyboard_key(), 0);
  EXPECT_TRUE(menu.caps);
  EXPECT_EQ(menu.key_label(10), "Q");
  EXPECT_EQ(menu.key_label(0), "1");
  menu.toggle_shift();
  EXPECT_EQ(menu.key_label(10), "q");
  menu.key_sent();
  EXPECT_EQ(menu.key_label(10), "Q");
  menu.keyboard_key();
  EXPECT_FALSE(menu.caps);
  menu.toggle_shift();
  menu.toggle_shift();
  EXPECT_FALSE(menu.shiftOnce);
  menu.toggle_shift();
  menu.close();
  EXPECT_FALSE(menu.shiftOnce);
  EXPECT_FALSE(menu.caps);
}

/**
 * @brief Explicit symbols send their required Shift and named combinations send their full modifier chord.
 */
TEST(StreamMenuTest, SendsSymbolsAndDesktopCombinations) {
  input::StreamMenu menu;
  menu.open(-1);
  menu.page = input::StreamMenuPage::keyboard;
  menu.toggle_keyboard_page();
  menu.keyIndex = 10;
  EXPECT_EQ(menu.key_label(10), "!");
  EXPECT_EQ(menu.keyboard_key(), '1');
  EXPECT_EQ(menu.key_modifiers('1'), MODIFIER_SHIFT);
  menu.toggle_keyboard_page();
  menu.caps = true;
  menu.shiftOnce = true;
  menu.modifiers = MODIFIER_META;
  for (int i = 20; i < 30; ++i) {
    menu.keyIndex = i;
    const auto &binding = input::StreamMenu::KEYS[menu.keyboard_cells()[i].key];
    EXPECT_EQ(menu.keyboard_key(), binding.code);
    EXPECT_EQ(menu.key_modifiers(binding.code), binding.modifiers);
  }
  menu.caps = false;
  menu.shiftOnce = false;
  menu.modifiers = 0;
  for (int i = 30; i < 34; ++i) {
    menu.keyIndex = i;
    EXPECT_EQ(menu.keyboard_key(), 0);
  }
  EXPECT_EQ(menu.modifiers, MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_META);
  EXPECT_TRUE(menu.shiftOnce);
  EXPECT_EQ(menu.key_label(30), "[Ctrl]");
  EXPECT_EQ(menu.key_label(33), "[Shift]");
  menu.close();
  EXPECT_EQ(menu.modifiers, 0);
}

/**
 * @brief Disconnect is accepted directly and arrows cannot change A/B semantics.
 */
TEST(StreamMenuTest, RequiresExplicitDisconnectAndCancelsDirectly) {
  input::StreamMenu menu;
  EXPECT_FALSE(menu.confirm_disconnect(input::StreamDisconnectAction::disconnect));
  menu.open(1);
  EXPECT_FALSE(menu.confirm_disconnect(input::StreamDisconnectAction::disconnect));
  menu.page = input::StreamMenuPage::quit_confirmation;
  menu.navigate(1, 0);
  menu.navigate(0, -1);
  EXPECT_EQ(menu.page, input::StreamMenuPage::quit_confirmation);
  EXPECT_FALSE(menu.confirm_disconnect(input::StreamDisconnectAction::cancel));
  EXPECT_EQ(menu.page, input::StreamMenuPage::radial);
  menu.page = input::StreamMenuPage::quit_confirmation;
  EXPECT_TRUE(menu.confirm_disconnect(input::StreamDisconnectAction::disconnect));
  EXPECT_EQ(menu.disconnectAction, input::StreamDisconnectAction::disconnect);
  menu.open(1);
  EXPECT_EQ(menu.disconnectAction, input::StreamDisconnectAction::cancel);
  menu.page = input::StreamMenuPage::quit_confirmation;
  EXPECT_TRUE(menu.confirm_disconnect(input::StreamDisconnectAction::quit_app));
  EXPECT_EQ(menu.disconnectAction, input::StreamDisconnectAction::quit_app);
  EXPECT_STREQ(input::stream_action_label(input::StreamAction::quit, false, false), "Exit");
}

/**
 * @brief A symbol sharing a digit's virtual key never changes that digit's label or sent modifiers.
 */
TEST(StreamMenuTest, LabelsEveryCellIndependentlyOfTheFocusedBinding) {
  input::StreamMenu menu;
  menu.open(-1);
  menu.page = input::StreamMenuPage::keyboard;
  menu.toggle_keyboard_page();
  const std::array<const char *, 10> digits {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
  for (int focus = 0; focus < 47; ++focus) {
    menu.keyIndex = focus;
    for (int digit = 0; digit < 10; ++digit) {
      EXPECT_EQ(menu.key_label(digit), digits[digit]) << "focus=" << focus;
    }
  }
  menu.keyIndex = 12;
  EXPECT_EQ(menu.key_label(2), "3");
  EXPECT_EQ(menu.key_label(12), "#");
  EXPECT_EQ(menu.keyboard_key(), '3');
  EXPECT_EQ(menu.key_modifiers('3'), MODIFIER_SHIFT);
  menu.keyIndex = 2;
  EXPECT_EQ(menu.keyboard_key(), '3');
  EXPECT_EQ(menu.key_modifiers('3'), 0);
  menu.caps = true;
  EXPECT_EQ(menu.key_label(43), "Caps");
  EXPECT_EQ(menu.key_label(2), "3");
}

/**
 * @brief The symbols page retains its explicit digits and punctuation when one-shot Shift or Caps is active.
 */
TEST(StreamMenuTest, ShiftDoesNotReplaceExplicitSymbolsPageKeys) {
  input::StreamMenu menu;
  menu.open(-1);
  menu.page = input::StreamMenuPage::keyboard;
  menu.toggle_keyboard_page();
  std::array<std::string, 40> labels;
  for (int i = 0; i < 40; ++i) {
    labels[i] = menu.key_label(i);
  }
  for (bool caps : {false, true}) {
    menu.caps = caps;
    menu.shiftOnce = true;
    menu.modifiers = MODIFIER_CTRL;
    for (int i = 0; i < 40; ++i) {
      menu.keyIndex = i;
      const auto &binding = input::StreamMenu::KEYS[menu.keyboard_cells()[i].key];
      EXPECT_EQ(menu.key_label(i), labels[i]);
      EXPECT_EQ(menu.key_modifiers(menu.keyboard_key()), MODIFIER_CTRL | binding.modifiers);
    }
  }
  menu.key_sent();
  EXPECT_FALSE(menu.shiftOnce);
}

/**
 * @brief D-pad navigation selects a wheel sector and logs clamp to their scroll range.
 */
TEST(StreamMenuTest, SupportsDigitalNavigationAndLogScrolling) {
  input::StreamMenu menu;
  menu.open(0);
  menu.navigate(1, 0);
  EXPECT_EQ(menu.selected, input::StreamAction::mouse);
  menu.navigate(0, -1);
  EXPECT_EQ(menu.selected, input::StreamAction::keyboard);
  menu.page = input::StreamMenuPage::logs;
  menu.navigate(0, -1);
  EXPECT_EQ(menu.logOffset, 5);
  menu.navigate(0, -100);
  EXPECT_EQ(menu.logOffset, 255);
  menu.navigate(0, 100);
  EXPECT_EQ(menu.logOffset, 0);
  menu.close();
  menu.navigate(1, 1);
  EXPECT_EQ(menu.selected, input::StreamAction::none);
}

/**
 * @brief All wheel choices have visible labels and toggles describe their current state.
 */
TEST(StreamMenuTest, LabelsAllActionsAndToggleStates) {
  for (int i = 0; i <= 8; ++i) {
    EXPECT_STRNE(input::stream_action_label(static_cast<input::StreamAction>(i), false, false), "");
  }
  EXPECT_STREQ(input::stream_action_label(input::StreamAction::mouse, true, false), "Mouse: On");
  EXPECT_STREQ(input::stream_action_label(input::StreamAction::statistics, false, true), "Stats: On");
}
