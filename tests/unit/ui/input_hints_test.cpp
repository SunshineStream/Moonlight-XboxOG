/**
 * @file tests/unit/ui/input_hints_test.cpp
 * @brief Verifies keyboard keycaps throughout the UI and restoration of controller glyphs.
 */
// test header include
#include "src/ui/shell_view.h"

// standard includes
#include <array>
#include <string>

// lib includes
#include <lizardbyte/common/testing.h>

/**
 * @brief Main-page helpers follow keyboard input and restore the original glyphs after gamepad input.
 */
TEST(InputHintsTest, SwitchesMainPageHelpersBetweenKeyboardAndGamepad) {
  auto state = app::create_initial_state();
  state.shell.inputDevice = input::UiInputDevice::keyboard;
  auto view = ui::build_shell_view_model(state, {});
  ASSERT_EQ(view.frame.footerActions.size(), 2);
  EXPECT_EQ(view.frame.footerActions[0].keyLabel, "Enter");
  EXPECT_EQ(view.frame.footerActions[1].keyLabel, "Alt+F4");
  for (const auto &action : view.frame.footerActions) {
    EXPECT_TRUE(action.iconAssetPath.empty());
    EXPECT_TRUE(action.secondaryIconAssetPath.empty());
  }
  state.shell.inputDevice = input::UiInputDevice::gamepad;
  view = ui::build_shell_view_model(state, {});
  EXPECT_EQ(view.frame.footerActions[0].iconAssetPath, "icons\\button-a.svg");
  EXPECT_EQ(view.frame.footerActions[1].iconAssetPath, "icons\\button-select.svg");
  EXPECT_EQ(view.frame.footerActions[1].secondaryIconAssetPath, "icons\\button-start.svg");
  EXPECT_TRUE(view.frame.footerActions[0].keyLabel.empty());
}

/**
 * @brief The numeric keypad distinguishes Space for typing a selected key from Enter for accepting the field.
 */
TEST(InputHintsTest, UsesWorkingKeyboardBindingsForTheAddressKeypad) {
  auto state = app::create_initial_state();
  state.shell.inputDevice = input::UiInputDevice::keyboard;
  state.shell.activeScreen = app::ScreenId::add_host;
  state.addHostDraft.keypad.visible = true;
  state.addHostDraft.activeField = app::AddHostField::address;
  const auto view = ui::build_shell_view_model(state, {});
  ASSERT_EQ(view.keypad.footerActions.size(), 5);
  EXPECT_EQ(view.keypad.footerActions[0].keyLabel, "Space");
  EXPECT_EQ(view.keypad.footerActions[1].keyLabel, "Bksp");
  EXPECT_EQ(view.keypad.footerActions[2].keyLabel, "Esc");
  EXPECT_EQ(view.keypad.footerActions[3].keyLabel, "PgUp");
  EXPECT_EQ(view.keypad.footerActions[3].secondaryKeyLabel, "PgDn");
  EXPECT_EQ(view.keypad.footerActions[4].keyLabel, "Enter");
  EXPECT_TRUE(view.frame.footerActions.empty());
  for (const auto &action : view.keypad.footerActions) {
    EXPECT_TRUE(action.iconAssetPath.empty());
    EXPECT_TRUE(action.secondaryIconAssetPath.empty());
  }
}

/**
 * @brief Alternate controller buttons for the same keyboard action collapse to one keycap.
 */
TEST(InputHintsTest, UsesKeyboardHintsInSupportAndDefaultDialogs) {
  auto state = app::create_initial_state();
  state.shell.inputDevice = input::UiInputDevice::keyboard;
  state.modal.id = app::ModalId::support;
  auto view = ui::build_shell_view_model(state, {});
  ASSERT_EQ(view.modal.footerActions.size(), 2);
  EXPECT_EQ(view.modal.footerActions[0].keyLabel, "Enter");
  EXPECT_TRUE(view.modal.footerActions[0].secondaryKeyLabel.empty());
  EXPECT_TRUE(view.modal.footerActions[0].secondaryIconAssetPath.empty());
  EXPECT_EQ(view.modal.footerActions[1].keyLabel, "Esc");
  state.modal.id = app::ModalId::connection_test;
  state.connectionTest.inProgress = false;
  view = ui::build_shell_view_model(state, {});
  ASSERT_EQ(view.modal.footerActions.size(), 1);
  EXPECT_EQ(view.modal.footerActions[0].keyLabel, "Esc");
  state.connectionTest.inProgress = true;
  EXPECT_TRUE(ui::build_shell_view_model(state, {}).modal.footerActions.empty());
}

/**
 * @brief Log viewer paging, fast paging, pane movement and close hints all use available keyboard bindings.
 */
TEST(InputHintsTest, LabelsLogViewerShortcutsAndNotificationDismissal) {
  auto state = app::create_initial_state();
  state.shell.inputDevice = input::UiInputDevice::keyboard;
  state.modal.id = app::ModalId::log_viewer;
  const auto view = ui::build_shell_view_model(state, {});
  const std::array<std::string, 6> expected {"PgUp", "PgDn", "Shift+PgUp", "Shift+PgDn", "Bksp", "Esc"};
  ASSERT_EQ(view.modal.footerActions.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(view.modal.footerActions[i].keyLabel, expected[i]);
    EXPECT_TRUE(view.modal.footerActions[i].iconAssetPath.empty());
  }
  state.modal.id = app::ModalId::none;
  app::show_notification(state, app::NotificationKind::error, "Storage", "Free some space.");
  const auto notificationView = ui::build_shell_view_model(state, {});
  ASSERT_FALSE(notificationView.notification.items.empty());
  ASSERT_FALSE(notificationView.notification.items.back().actions.empty());
  EXPECT_EQ(notificationView.notification.items.back().actions[0].keyLabel, "Bksp");
}
