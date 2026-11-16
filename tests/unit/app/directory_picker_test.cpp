/**
 * @file tests/unit/app/directory_picker_test.cpp
 * @brief Verifies staged screenshot destination selection and the local folder-name keyboard.
 */
// test includes
#include <lizardbyte/common/testing.h>

// standard includes
#include <string>

// local includes
#include "src/app/client_state.h"
#include "src/startup/storage_paths.h"
#include "src/ui/shell_view.h"

namespace {
  /**
   * @brief Open screenshot destination settings using normal shell navigation.
   * @param configured Existing screenshot destination.
   * @return State with the folder picker open and a pending directory read.
   */
  app::ClientState open_picker(const std::string &configured = {}) {
    auto state = app::create_initial_state();
    state.settings.screenshotDirectory = configured;
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(state.detailMenu.select_item_by_id("screenshot-directory"));
    const auto update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.requests.directoryListRequested);
    EXPECT_TRUE(update.navigation.modalOpened);
    EXPECT_EQ(state.modal.id, app::ModalId::directory_picker);
    return state;
  }

  /**
   * @brief Begin local name entry in an available directory.
   * @param state Open folder picker.
   */
  void start_folder_name(app::ClientState &state) {
    state.directoryPicker.available = true;
    state.modal.selectedActionIndex = 1;
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(state.directoryPicker.creating);
  }
}  // namespace

/**
 * @brief Opening or canceling the picker leaves the persisted screenshot folder unchanged.
 */
TEST(DirectoryPickerTest, StagesConfiguredAndDefaultLocationsUntilAccepted) {
  for (const std::string configured : {std::string {}, std::string("F:\\Shots")}) {
    auto state = open_picker(configured);
    EXPECT_EQ(state.directoryPicker.path, configured.empty() ? startup::default_storage_path("screenshots") : configured);
    auto view = ui::build_shell_view_model(state, {});
    EXPECT_EQ(view.modal.title, "Screenshot Folder");
    EXPECT_FALSE(view.modal.actions[0].enabled);
    const auto update = app::handle_command(state, input::UiCommand::back);
    EXPECT_TRUE(update.navigation.modalClosed);
    EXPECT_EQ(state.settings.screenshotDirectory, configured);
    EXPECT_TRUE(state.directoryPicker.folders.empty());
    EXPECT_FALSE(update.persistence.settingsChanged);
  }
}

/**
 * @brief Child, parent and drive navigation request new listings without changing the setting.
 */
TEST(DirectoryPickerTest, BrowsesChildrenParentsAndDrives) {
  auto state = open_picker("F:\\Shots");
  state.directoryPicker.available = true;
  state.directoryPicker.folders = {"F:\\Shots\\Games", "F:\\Shots\\Desktop"};
  state.modal.selectedActionIndex = 4;
  EXPECT_TRUE(app::handle_command(state, input::UiCommand::activate).requests.directoryListRequested);
  EXPECT_EQ(state.directoryPicker.path, "F:\\Shots\\Games");
  EXPECT_TRUE(app::handle_command(state, input::UiCommand::move_left).requests.directoryListRequested);
  EXPECT_EQ(state.directoryPicker.path, "F:\\Shots");
  app::handle_command(state, input::UiCommand::previous_page);
  EXPECT_EQ(state.directoryPicker.path, "F:\\");
  app::handle_command(state, input::UiCommand::previous_page);
  EXPECT_TRUE(state.directoryPicker.path.empty());
  EXPECT_EQ(state.settings.screenshotDirectory, "F:\\Shots");
  state.modal.selectedActionIndex = 0;
  EXPECT_FALSE(app::handle_command(state, input::UiCommand::activate).requests.directorySelectRequested);
  state.modal.selectedActionIndex = 1;
  app::handle_command(state, input::UiCommand::activate);
  EXPECT_FALSE(state.directoryPicker.creating);
}

/**
 * @brief Selection requests a write check; only its successful completion updates and persists the setting.
 */
TEST(DirectoryPickerTest, AcceptsValidatedSelectionAndRestoresTheDefault) {
  auto state = open_picker("F:\\Old");
  state.directoryPicker.path = "E:\\Screenshots";
  state.directoryPicker.available = true;
  state.modal.selectedActionIndex = 9;
  EXPECT_TRUE(app::handle_command(state, input::UiCommand::open_context_menu).requests.directorySelectRequested);
  auto update = app::handle_command(state, input::UiCommand::activate);
  EXPECT_TRUE(update.requests.directorySelectRequested);
  EXPECT_EQ(state.settings.screenshotDirectory, "F:\\Old");
  app::accept_screenshot_directory(state, update, state.directoryPicker.path);
  EXPECT_EQ(state.settings.screenshotDirectory, "E:\\Screenshots");
  EXPECT_TRUE(state.settings.dirty);
  EXPECT_TRUE(update.persistence.settingsChanged);
  EXPECT_TRUE(update.navigation.modalClosed);
  EXPECT_TRUE(state.directoryPicker.path.empty());
  EXPECT_EQ(state.detailMenu.selected_item()->id, "screenshot-directory");
  app::handle_command(state, input::UiCommand::activate);
  state.modal.selectedActionIndex = 3;
  update = app::handle_command(state, input::UiCommand::confirm);
  EXPECT_TRUE(state.settings.screenshotDirectory.empty());
  EXPECT_TRUE(update.persistence.settingsChanged);
  app::handle_command(state, input::UiCommand::activate);
  state.modal.selectedActionIndex = 3;
  EXPECT_FALSE(app::handle_command(state, input::UiCommand::activate).persistence.settingsChanged);
  app::accept_screenshot_directory(state, update, "ignored");
  EXPECT_TRUE(state.settings.screenshotDirectory.empty());
}

/**
 * @brief Folder name entry reuses the OSK's caps, one-shot shift, symbols and tall-key navigation.
 */
TEST(DirectoryPickerTest, TypesLocallyWithTheSharedKeyboardAndCaret) {
  auto state = open_picker("F:\\Shots");
  start_folder_name(state);
  auto &picker = state.directoryPicker;
  app::handle_command(state, input::UiCommand::activate);
  EXPECT_EQ(picker.name, "q");
  picker.keyboard.keyIndex = 43;
  app::handle_command(state, input::UiCommand::activate);
  picker.keyboard.keyIndex = 10;
  app::handle_command(state, input::UiCommand::activate);
  app::handle_command(state, input::UiCommand::open_context_menu);
  app::handle_command(state, input::UiCommand::activate);
  EXPECT_EQ(picker.name, "qQq");
  EXPECT_FALSE(picker.keyboard.shiftOnce);
  picker.keyboard.keyIndex = 42;
  app::handle_command(state, input::UiCommand::activate);
  app::append_directory_name(state, "Z");
  EXPECT_EQ(picker.name, "qQZq");
  picker.keyboard.keyIndex = 45;
  app::handle_command(state, input::UiCommand::activate);
  app::handle_command(state, input::UiCommand::delete_character);
  EXPECT_EQ(picker.name, "qQZ");
  app::handle_command(state, input::UiCommand::next_page);
  picker.keyboard.keyIndex = 12;
  app::handle_command(state, input::UiCommand::activate);
  EXPECT_EQ(picker.name, "qQZ#");
  picker.keyboard.keyIndex = 40;
  app::handle_command(state, input::UiCommand::activate);
  EXPECT_EQ(picker.name.back(), ' ');
  picker.keyboard.keyIndex = 44;
  app::handle_command(state, input::UiCommand::activate);
  EXPECT_EQ(picker.name, "qQZ#");
  picker.keyboard.keyIndex = 41;
  app::handle_command(state, input::UiCommand::activate);
  EXPECT_EQ(picker.keyboard.keyboardLayout, 0);
  app::handle_command(state, input::UiCommand::move_up);
  app::handle_command(state, input::UiCommand::move_right);
  app::handle_command(state, input::UiCommand::move_down);
  app::handle_command(state, input::UiCommand::move_left);
  picker.keyboard.keyIndex = 46;
  EXPECT_TRUE(app::handle_command(state, input::UiCommand::activate).requests.directoryCreateRequested);
  EXPECT_EQ(state.settings.screenshotDirectory, "F:\\Shots");
  EXPECT_FALSE(state.settings.dirty);
}

/**
 * @brief Invalid, oversized and non-printable names never issue a filesystem request; cancellation keeps the browser.
 */
TEST(DirectoryPickerTest, ValidatesCreationAndPreservesTheBrowserOnCancel) {
  auto state = open_picker("F:\\Shots");
  start_folder_name(state);
  auto &picker = state.directoryPicker;
  EXPECT_FALSE(app::handle_command(state, input::UiCommand::confirm).requests.directoryCreateRequested);
  EXPECT_FALSE(picker.error.empty());
  app::append_directory_name(state, "../invalid");
  EXPECT_TRUE(picker.error.empty());
  EXPECT_FALSE(app::handle_command(state, input::UiCommand::confirm).requests.directoryCreateRequested);
  picker.name.clear();
  picker.caret = 100;
  app::append_directory_name(state, std::string(60, 'a') + "\n");
  EXPECT_EQ(picker.name.size(), 42);
  EXPECT_EQ(picker.caret, 42);
  EXPECT_TRUE(app::handle_command(state, input::UiCommand::confirm).requests.directoryCreateRequested);
  app::handle_command(state, input::UiCommand::back);
  EXPECT_FALSE(picker.creating);
  EXPECT_EQ(state.modal.id, app::ModalId::directory_picker);
  EXPECT_EQ(picker.path, "F:\\Shots");
  app::append_directory_name(state, "ignored");
  EXPECT_EQ(picker.name.size(), 42);
}

/**
 * @brief The browser supports large lists, error feedback and the shared keyboard's exact cell geometry and device hints.
 */
TEST(DirectoryPickerTest, BuildsScrollableFoldersAndControllerOrKeyboardNameEntry) {
  auto state = open_picker("F:\\Shots");
  state.directoryPicker.available = true;
  for (int i = 0; i < 100; ++i) {
    state.directoryPicker.folders.push_back("F:\\Shots\\Folder" + std::to_string(i));
  }
  app::handle_command(state, input::UiCommand::move_up);
  auto view = ui::build_shell_view_model(state, {});
  ASSERT_EQ(view.modal.actions.size(), 104);
  EXPECT_TRUE(view.modal.actions.back().selected);
  EXPECT_EQ(view.modal.actions.back().label, "Folder99");
  app::handle_command(state, input::UiCommand::move_down);
  EXPECT_EQ(state.modal.selectedActionIndex, 0);
  state.directoryPicker.error = "Cannot write here";
  view = ui::build_shell_view_model(state, {});
  EXPECT_EQ(view.modal.lines.back(), "Cannot write here");
  start_folder_name(state);
  view = ui::build_shell_view_model(state, {});
  EXPECT_FALSE(view.modal.visible);
  EXPECT_TRUE(view.keypad.visible);
  EXPECT_EQ(view.keypad.buttons.size(), 47);
  EXPECT_EQ(view.keypad.rowCount, 5);
  EXPECT_EQ(view.keypad.columnCount, 26);
  EXPECT_EQ(view.keypad.buttons[43].height, 2);
  EXPECT_EQ(view.keypad.buttons[46].label, "OK");
  EXPECT_EQ(view.keypad.footerActions.size(), 5);
  state.shell.inputDevice = input::UiInputDevice::keyboard;
  view = ui::build_shell_view_model(state, {});
  ASSERT_EQ(view.keypad.footerActions.size(), 2);
  EXPECT_EQ(view.keypad.footerActions[0].keyLabel, "Enter");
  EXPECT_EQ(view.keypad.footerActions[1].keyLabel, "Esc");
}
