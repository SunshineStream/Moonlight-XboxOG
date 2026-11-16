/**
 * @file tests/unit/app/menu_actions_test.cpp
 * @brief Verifies retained action menus, confirmed capture deletion and host-app termination.
 */
// test includes
#include <lizardbyte/common/testing.h>

// standard includes
#include <string>

// local includes
#include "src/app/client_state.h"
#include "src/ui/shell_view.h"

namespace {
  /**
   * @brief Open a cached app library with a running app and its local context menu.
   * @return State whose first app owns the host's retained session.
   */
  app::ClientState running_app_menu() {
    auto state = app::create_initial_state();
    app::HostRecord host {"Gaming PC", "192.0.2.1", 0, app::PairingState::paired};
    host.uuid = "server-one";
    host.runningGameId = 7;
    host.apps = {{"Game", 7}, {"Other", 8}};
    host.apps[0].running = true;
    app::replace_hosts(state, {host});
    EXPECT_TRUE(app::begin_selected_host_app_browse(state, false));
    state.hosts.active.appListState = app::HostAppListState::ready;
    app::handle_command(state, input::UiCommand::open_context_menu);
    return state;
  }

  /**
   * @brief Open quit confirmation through its real context-menu action.
   * @param state Library with its first app's actions open.
   */
  void confirm_quit_menu(app::ClientState &state) {
    state.modal.id = app::ModalId::app_actions;
    state.modal.selectedActionIndex = 4;
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.modal.id, app::ModalId::confirmation);
    EXPECT_EQ(state.confirmation.action, app::ConfirmationAction::quit_app);
  }

  /**
   * @brief Host and app dialogs share selection-only retention and invalidate when their content changes.
   */
  TEST(MenuActionsTest, RetainsHostAndAppActionsAcrossFocusChanges) {
    for (const auto modal : {app::ModalId::host_actions, app::ModalId::app_actions}) {
      auto state = running_app_menu();
      state.hosts.items = {state.hosts.active};
      state.hosts.loaded = true;
      state.modal.id = modal;
      const auto original = ui::build_shell_view_model(state, {}).modal;
      EXPECT_FALSE(original.folderBrowser);
      for (int i = 0; i < 8; ++i) {
        app::handle_command(state, input::UiCommand::move_down);
        EXPECT_TRUE(ui::same_action_dialog_content(original, ui::build_shell_view_model(state, {}).modal));
      }
      auto changed = original;
      changed.actions[0].label += " changed";
      EXPECT_FALSE(ui::same_action_dialog_content(original, changed));
      changed = original;
      changed.busy = true;
      EXPECT_FALSE(ui::same_action_dialog_content(original, changed));
      state.shell.inputDevice = input::UiInputDevice::keyboard;
      EXPECT_FALSE(ui::same_action_dialog_content(original, ui::build_shell_view_model(state, {}).modal));
    }
  }

  /**
   * @brief Static screens and local dialogs stop idle redraws while diagnostics and progress keep animating.
   */
  TEST(MenuActionsTest, AnimatesOnlyViewsThatNeedClockUpdates) {
    auto state = running_app_menu();
    for (const auto modal : {app::ModalId::host_actions, app::ModalId::app_actions, app::ModalId::confirmation, app::ModalId::directory_picker, app::ModalId::screenshot_viewer, app::ModalId::log_viewer, app::ModalId::support}) {
      state.modal.id = modal;
      EXPECT_FALSE(ui::shell_requires_animation(state));
    }
    state.modal.id = app::ModalId::none;
    EXPECT_FALSE(ui::shell_requires_animation(state));
    state.hosts.active.appListState = app::HostAppListState::loading;
    EXPECT_TRUE(ui::shell_requires_animation(state));
    state.modal.id = app::ModalId::app_actions;
    EXPECT_FALSE(ui::shell_requires_animation(state));
    state.modal.id = app::ModalId::quit_progress;
    EXPECT_TRUE(ui::shell_requires_animation(state));
    state.modal.id = app::ModalId::connection_test;
    EXPECT_FALSE(ui::shell_requires_animation(state));
    state.connectionTest.inProgress = true;
    EXPECT_TRUE(ui::shell_requires_animation(state));
    state.modal.id = app::ModalId::support;
    state.shell.overlayVisible = true;
    EXPECT_TRUE(ui::shell_requires_animation(state));
    state.shell.overlayVisible = false;
    state.modal.id = app::ModalId::none;
    for (auto screen : {app::ScreenId::hosts, app::ScreenId::settings, app::ScreenId::add_host}) {
      state.shell.activeScreen = screen;
      EXPECT_FALSE(ui::shell_requires_animation(state));
    }
    state.shell.activeScreen = app::ScreenId::pair_host;
    EXPECT_TRUE(ui::shell_requires_animation(state));
  }

  /**
   * @brief Deleting a capture requires confirmation, preserves the selected capture on failure and handles the last image.
   */
  TEST(MenuActionsTest, ConfirmsScreenshotDeletionAndRestoresViewerNavigation) {
    auto state = running_app_menu();
    state.modal.selectedActionIndex = 3;
    EXPECT_TRUE(app::handle_command(state, input::UiCommand::activate).requests.screenshotsRequested);
    const auto source = state.screenshotViewer.source;
    state.screenshotViewer.entries = {{source, "first.bmp", 20}, {source, "second.bmp", 10}};
    state.screenshotViewer.selectedIndex = 1;
    EXPECT_FALSE(app::handle_command(state, input::UiCommand::delete_character).requests.screenshotDeleteRequested);
    EXPECT_EQ(state.confirmation.targetPath, "second.bmp");
    EXPECT_EQ(state.modal.id, app::ModalId::confirmation);
    app::handle_command(state, input::UiCommand::back);
    EXPECT_EQ(state.modal.id, app::ModalId::screenshot_viewer);
    EXPECT_EQ(state.screenshotViewer.selectedIndex, 1U);
    EXPECT_EQ(state.screenshotViewer.entries.size(), 2U);
    app::handle_command(state, input::UiCommand::delete_character);
    const auto accepted = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(accepted.requests.screenshotDeleteRequested);
    EXPECT_EQ(state.modal.id, app::ModalId::screenshot_viewer);
    app::apply_screenshot_delete_result(state, false, "Read-only folder");
    EXPECT_EQ(state.screenshotViewer.entries.size(), 2U);
    EXPECT_EQ(state.screenshotViewer.error, "Read-only folder");
    app::apply_screenshot_delete_result(state, true, {});
    ASSERT_EQ(state.screenshotViewer.entries.size(), 1U);
    EXPECT_EQ(state.screenshotViewer.entries[0].path, "first.bmp");
    EXPECT_EQ(state.screenshotViewer.selectedIndex, 0U);
    EXPECT_TRUE(state.screenshotViewer.error.empty());
    app::handle_command(state, input::UiCommand::delete_character);
    app::handle_command(state, input::UiCommand::move_down);
    EXPECT_FALSE(app::handle_command(state, input::UiCommand::activate).requests.screenshotDeleteRequested);
    EXPECT_EQ(state.modal.id, app::ModalId::screenshot_viewer);
    EXPECT_EQ(state.screenshotViewer.entries.size(), 1U);
    app::apply_screenshot_delete_result(state, true, {});
    EXPECT_TRUE(state.screenshotViewer.entries.empty());
    EXPECT_EQ(state.screenshotViewer.selectedIndex, 0U);
    app::apply_screenshot_delete_result(state, true, {});
    EXPECT_FALSE(app::handle_command(state, input::UiCommand::delete_character).requests.screenshotDeleteRequested);
    EXPECT_EQ(state.modal.id, app::ModalId::screenshot_viewer);
    app::handle_command(state, input::UiCommand::back);
    app::apply_screenshot_delete_result(state, false, "stale result");
    EXPECT_TRUE(state.screenshotViewer.error.empty());
    EXPECT_EQ(state.modal.id, app::ModalId::app_actions);
  }

  /**
   * @brief Quit targets are retained through confirmation, errors keep the session resumable, and success clears its marker.
   */
  TEST(MenuActionsTest, ConfirmsQuitAndClearsOnlyTheCompletedApp) {
    auto state = running_app_menu();
    const auto view = ui::build_shell_view_model(state, {});
    ASSERT_EQ(view.modal.actions.size(), 5U);
    EXPECT_EQ(view.modal.actions[4].id, "quit-app");
    EXPECT_TRUE(view.content.appTiles[0].running);
    EXPECT_EQ(view.content.appTiles[0].detail, "In progress / Resume");
    confirm_quit_menu(state);
    EXPECT_EQ(state.appQuit.appId, 7);
    app::handle_command(state, input::UiCommand::back);
    EXPECT_EQ(state.modal.id, app::ModalId::app_actions);
    EXPECT_EQ(state.modal.selectedActionIndex, 4U);
    EXPECT_FALSE(state.appQuit.inProgress);
    confirm_quit_menu(state);
    const auto update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.requests.quitAppRequested);
    EXPECT_TRUE(state.appQuit.inProgress);
    EXPECT_EQ(state.modal.id, app::ModalId::quit_progress);
    EXPECT_TRUE(ui::build_shell_view_model(state, {}).modal.busy);
    app::handle_command(state, input::UiCommand::back);
    EXPECT_EQ(state.modal.id, app::ModalId::quit_progress);
    app::apply_app_quit_result(state, false, "Another client owns the app");
    EXPECT_TRUE(state.hosts.active.apps[0].running);
    EXPECT_EQ(state.hosts.active.runningGameId, 7U);
    EXPECT_FALSE(state.appQuit.inProgress);
    EXPECT_EQ(state.modal.id, app::ModalId::none);
    confirm_quit_menu(state);
    app::handle_command(state, input::UiCommand::activate);
    state.hosts.active.apps[1].running = true;
    app::HostRecord unrelated {"Other PC", "192.0.2.2"};
    unrelated.runningGameId = 7;
    unrelated.apps = {{"Other PC Game", 7}};
    unrelated.apps[0].running = true;
    state.hosts.items = {state.hosts.active, unrelated};
    app::apply_app_quit_result(state, true, {});
    EXPECT_FALSE(state.hosts.active.apps[0].running);
    EXPECT_TRUE(state.hosts.active.apps[1].running);
    EXPECT_EQ(state.hosts.active.runningGameId, 0U);
    EXPECT_FALSE(state.hosts.items[0].apps[0].running);
    EXPECT_TRUE(state.hosts.items[1].apps[0].running);
    EXPECT_EQ(state.hosts.items[1].runningGameId, 7U);
    EXPECT_TRUE(state.hosts.dirty);
    EXPECT_EQ(state.modal.id, app::ModalId::none);
    app::handle_command(state, input::UiCommand::move_right);
    EXPECT_EQ(state.apps.selectedAppIndex, 1U);
    app::apply_app_quit_result(state, true, "stale completion");
    EXPECT_TRUE(state.hosts.active.apps[1].running);
  }

  /**
   * @brief A removed quit action cannot wrap into Hide app when a background refresh marks the app stopped.
   */
  TEST(MenuActionsTest, StoppedAppCannotTurnAStaleQuitSelectionIntoHide) {
    auto state = running_app_menu();
    state.modal.selectedActionIndex = 4;
    state.hosts.active.apps[0].running = false;
    EXPECT_TRUE(ui::build_shell_view_model(state, {}).modal.actions[3].selected);
    const auto update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_FALSE(update.requests.quitAppRequested);
    EXPECT_FALSE(state.hosts.active.apps[0].hidden);
    EXPECT_EQ(state.modal.id, app::ModalId::app_actions);
    EXPECT_EQ(state.modal.selectedActionIndex, 3U);
    EXPECT_EQ(ui::build_shell_view_model(state, {}).modal.actions.size(), 4U);
    state.modal.selectedActionIndex = 4;
    app::handle_command(state, input::UiCommand::move_down);
    EXPECT_EQ(state.modal.selectedActionIndex, 0U);
  }
}  // namespace
