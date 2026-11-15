/**
 * @file tests/unit/app/client_state_test.cpp
 * @brief Verifies client state models and transitions.
 */
// test includes
#include "tests/support/network_test_constants.h"

#include <lizardbyte/common/testing.h>

// standard includes
#include <array>
#include <cstdint>
#include <limits>
#include <string>

// local includes
#include "src/app/client_state.h"

namespace {

  /**
   * @brief Queue distinct events together without duplicating an outstanding notification.
   */
  TEST(ClientStateTest, QueuesDistinctNotificationsAndCoalescesDuplicates) {
    auto state = app::create_initial_state();
    app::show_notification(state, app::NotificationKind::error, "Ignored", "");
    EXPECT_TRUE(state.shell.notifications.empty());
    app::show_notification(state, app::NotificationKind::error, "Storage", "Changes were not saved.");
    app::show_notification(state, app::NotificationKind::error, "Storage", "Changes were not saved.");
    ASSERT_EQ(state.shell.notifications.size(), 1U);
    app::show_notification(state, app::NotificationKind::error, "Storage", "A file was not deleted.");
    app::show_notification(state, app::NotificationKind::error, "Settings", "Changes were not saved.");
    app::show_notification(state, app::NotificationKind::success, "Storage", "Changes were not saved.", 5000U);
    ASSERT_EQ(state.shell.notifications.size(), 4U);
    EXPECT_TRUE(state.shell.notifications.front().requiresInteraction);
    EXPECT_FALSE(state.shell.notifications.back().requiresInteraction);
    EXPECT_EQ(state.shell.notifications.back().remainingMilliseconds, 5000U);
  }

  /**
   * @brief Expire automatic cards independently while keeping manual errors until acknowledged.
   */
  TEST(ClientStateTest, ExpiresAutomaticNotificationsAndRetainsManualNotifications) {
    auto state = app::create_initial_state();
    app::show_notification(state, app::NotificationKind::error, "Storage", "Free some space and try again.");
    app::show_notification(state, app::NotificationKind::success, "Saved", "The PC was saved.", 100U);
    app::show_notification(state, app::NotificationKind::information, "Library", "Showing saved apps.", 300U);
    app::advance_notifications(state, 0U);
    EXPECT_EQ(state.shell.notifications.size(), 3U);
    app::advance_notifications(state, 99U);
    EXPECT_EQ(state.shell.notifications[1].remainingMilliseconds, 1U);
    app::advance_notifications(state, 1U);
    ASSERT_EQ(state.shell.notifications.size(), 2U);
    EXPECT_EQ(state.shell.notifications.back().title, "Library");
    EXPECT_EQ(state.shell.notifications.back().remainingMilliseconds, 200U);
    app::advance_notifications(state, std::numeric_limits<std::uint32_t>::max());
    ASSERT_EQ(state.shell.notifications.size(), 1U);
    EXPECT_EQ(state.shell.notifications.front().title, "Storage");
    app::dismiss_notification(state);
    app::dismiss_notification(state);
    app::advance_notifications(state, 0U);
    EXPECT_TRUE(state.shell.notifications.empty());
  }

  /**
   * @brief X acknowledges only the newest card and leaves older events available.
   */
  TEST(ClientStateTest, DismissesTheNewestNotificationWithTheController) {
    auto state = app::create_initial_state();
    app::show_notification(state, app::NotificationKind::error, "Older", "Needs attention.");
    app::show_notification(state, app::NotificationKind::success, "Newer", "Completed.", 5000U);
    state.shell.statusMessage = "Diagnostic status";
    app::handle_command(state, input::UiCommand::delete_character);
    ASSERT_EQ(state.shell.notifications.size(), 1U);
    EXPECT_EQ(state.shell.notifications.front().title, "Older");
    EXPECT_TRUE(state.shell.statusMessage.empty());
    app::handle_command(state, input::UiCommand::delete_character);
    EXPECT_TRUE(state.shell.notifications.empty());
  }

  /**
   * @brief Preserve automatic-card lifetimes while dialogs, diagnostics or numeric input cover them.
   */
  TEST(ClientStateTest, PausesNotificationTimersWhileTheStackIsCovered) {
    auto state = app::create_initial_state();
    app::show_notification(state, app::NotificationKind::success, "Saved", "The PC was saved.", 5000U);
    state.modal.id = app::ModalId::support;
    app::advance_notifications(state, 4000U);
    state.modal.id = app::ModalId::none;
    state.shell.overlayVisible = true;
    app::advance_notifications(state, 4000U);
    state.shell.overlayVisible = false;
    state.shell.activeScreen = app::ScreenId::add_host;
    state.addHostDraft.keypad.visible = true;
    app::advance_notifications(state, 4000U);
    ASSERT_EQ(state.shell.notifications.size(), 1U);
    EXPECT_EQ(state.shell.notifications.front().remainingMilliseconds, 5000U);
    state.addHostDraft.keypad.visible = false;
    app::advance_notifications(state, 4000U);
    EXPECT_EQ(state.shell.notifications.front().remainingMilliseconds, 1000U);
    state.shell.activeScreen = app::ScreenId::hosts;
    state.addHostDraft.keypad.visible = true;
    app::advance_notifications(state, 1000U);
    EXPECT_TRUE(state.shell.notifications.empty());
  }

  /**
   * @brief Repeated background refresh failures produce one notice until the library recovers.
   */
  TEST(ClientStateTest, ReportsCachedLibraryRefreshFailureOnceUntilRecovery) {
    auto state = app::create_initial_state();
    const std::string address = test_support::kTestIpv4Addresses[test_support::kIpOffice];
    app::replace_hosts(state, {{"Office PC", address, 0, app::PairingState::paired}});
    ASSERT_TRUE(app::begin_selected_host_app_browse(state, false));
    state.hosts.active.apps = {{"Desktop", 1}};
    app::apply_app_list_result(state, address, app::DEFAULT_HOST_PORT, {}, 0U, false, "Timed out");
    ASSERT_EQ(state.shell.notifications.size(), 1U);
    EXPECT_EQ(state.shell.notifications.front().kind, app::NotificationKind::information);
    EXPECT_FALSE(state.shell.notifications.front().requiresInteraction);
    app::dismiss_notification(state);
    app::apply_app_list_result(state, address, app::DEFAULT_HOST_PORT, {}, 0U, false, "Connection refused");
    EXPECT_TRUE(state.shell.notifications.empty());
    app::apply_app_list_result(state, address, app::DEFAULT_HOST_PORT, {{"Desktop", 1}}, 1U, true, "Loaded 1 app(s)");
    EXPECT_FALSE(state.apps.refreshFailureNotified);
    app::apply_app_list_result(state, address, app::DEFAULT_HOST_PORT, {}, 0U, false, "Timed out again");
    EXPECT_EQ(state.shell.notifications.size(), 1U);
  }

  /**
   * @brief Host operations appear after entry and disappear after the address is cleared.
   */
  TEST(ClientStateTest, HidesHostOperationsUntilAnAddressIsEntered) {
    app::ClientState state = app::create_initial_state();
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.menu.items().size(), 3U);
    EXPECT_FALSE(state.menu.select_item_by_id("test-connection"));
    EXPECT_FALSE(state.menu.select_item_by_id("start-pairing"));
    EXPECT_FALSE(state.menu.select_item_by_id("save-host"));
    state.addHostDraft.addressInput = " \t\n";
    app::handle_command(state, input::UiCommand::none);
    EXPECT_EQ(state.menu.items().size(), 3U);
    state.addHostDraft.addressInput = test_support::kTestIpv4Addresses[test_support::kIpOffice];
    app::handle_command(state, input::UiCommand::none);
    EXPECT_EQ(state.menu.items().size(), 6U);
    EXPECT_TRUE(state.menu.select_item_by_id("test-connection"));
    EXPECT_TRUE(state.menu.select_item_by_id("start-pairing"));
    EXPECT_TRUE(state.menu.select_item_by_id("save-host"));
    state.addHostDraft.addressInput.clear();
    const auto update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.menu.items().size(), 3U);
    EXPECT_FALSE(update.persistence.hostsChanged);
    EXPECT_FALSE(update.requests.connectionTestRequested);
    EXPECT_FALSE(update.requests.pairingRequested);
    EXPECT_TRUE(state.addHostDraft.keypad.visible);
  }

  /**
   * @brief Navigates from the initial hosts toolbar selection to the display settings options.
   *
   * @param state Client state to drive through the display settings commands.
   */
  void open_display_settings_options(app::ClientState &state) {
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::settings);

    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.settings.selectedCategory, app::SettingsCategory::display);
    ASSERT_EQ(state.settings.focusArea, app::SettingsFocusArea::options);
  }

  TEST(ClientStateTest, StartsOnTheHostsScreenWithTheToolbarSelected) {
    const app::ClientState state = app::create_initial_state();

    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
    EXPECT_EQ(state.hosts.focusArea, app::HostsFocusArea::toolbar);
    EXPECT_EQ(state.hosts.selectedToolbarButtonIndex, 2U);
    EXPECT_FALSE(state.shell.overlayVisible);
    EXPECT_FALSE(state.shell.shouldExit);
    EXPECT_FALSE(state.hosts.dirty);
    EXPECT_EQ(state.settings.loggingLevel, logging::LogLevel::none);
    EXPECT_EQ(state.settings.streamFramerate, 30);
  }

  TEST(ClientStateTest, ReplacesHostsFromPersistenceWithoutMarkingThemDirty) {
    app::ClientState state = app::create_initial_state();

    app::replace_hosts(state, {
                                {"Living Room PC", test_support::kTestIpv4Addresses[test_support::kIpLivingRoom], 0, app::PairingState::not_paired},
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired},
                              },
                       "Loaded 2 saved host(s)");

    ASSERT_EQ(state.hosts.size(), 2U);
    EXPECT_FALSE(state.hosts.dirty);
    EXPECT_EQ(state.shell.statusMessage, "Loaded 2 saved host(s)");
    EXPECT_EQ(state.hosts.focusArea, app::HostsFocusArea::grid);
    EXPECT_EQ(state.hosts.selectedHostIndex, 0U);
  }

  TEST(ClientStateTest, MergeDiscoveredHostAddsANewReachableHostToTheGrid) {
    app::ClientState state = app::create_initial_state();

    EXPECT_TRUE(app::merge_discovered_host(state, "Living Room PC", test_support::kTestIpv4Addresses[test_support::kIpLivingRoom], 0));

    ASSERT_EQ(state.hosts.size(), 1U);
    EXPECT_TRUE(state.hosts.dirty);
    EXPECT_EQ(state.hosts.front().displayName, "Living Room PC");
    EXPECT_EQ(state.hosts.front().address, test_support::kTestIpv4Addresses[test_support::kIpLivingRoom]);
    EXPECT_EQ(state.hosts.front().reachability, app::HostReachability::online);
    EXPECT_EQ(state.hosts.focusArea, app::HostsFocusArea::grid);
    EXPECT_EQ(state.hosts.selectedHostIndex, 0U);
  }

  TEST(ClientStateTest, MergeDiscoveredHostPrefersDiscoveredNamesOnlyForDefaultNamedHosts) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Host 10.0.0.25", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::not_paired, app::HostReachability::unknown},
                              });

    EXPECT_TRUE(app::merge_discovered_host(state, "Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost]));
    ASSERT_EQ(state.hosts.size(), 1U);
    EXPECT_EQ(state.hosts.front().displayName, "Office PC");
    EXPECT_EQ(state.hosts.front().reachability, app::HostReachability::online);

    state.hosts.dirty = false;
    state.hosts.front().displayName = "Custom Office";
    EXPECT_FALSE(app::merge_discovered_host(state, "Discovered Office", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost]));
    EXPECT_EQ(state.hosts.front().displayName, "Custom Office");
    EXPECT_FALSE(state.hosts.dirty);
  }

  TEST(ClientStateTest, HostsToolbarCanOpenSettingsAndAddHost) {
    app::ClientState state = app::create_initial_state();

    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::move_left);

    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.navigation.screenChanged);
    EXPECT_EQ(update.navigation.activatedItemId, "settings-button");
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::settings);

    update = app::handle_command(state, input::UiCommand::back);
    EXPECT_TRUE(update.navigation.screenChanged);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);

    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.navigation.screenChanged);
    EXPECT_EQ(update.navigation.activatedItemId, "add-host-button");
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::add_host);
  }

  TEST(ClientStateTest, SettingsCanRequestLogViewingAndCycleBothLoggingLevelsFromNone) {
    app::ClientState state = app::create_initial_state();

    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::move_left);

    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::settings);

    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.settings.selectedCategory, app::SettingsCategory::logging);
    EXPECT_EQ(state.settings.focusArea, app::SettingsFocusArea::options);

    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.requests.logViewRequested);
    EXPECT_EQ(update.navigation.activatedItemId, "view-log-file");

    app::handle_command(state, input::UiCommand::move_down);
    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_FALSE(update.requests.logViewRequested);
    EXPECT_TRUE(update.persistence.settingsChanged);
    EXPECT_EQ(state.settings.loggingLevel, logging::LogLevel::error);
    EXPECT_EQ(state.shell.statusMessage, "Logging level set to ERROR");

    app::handle_command(state, input::UiCommand::move_down);
    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.persistence.settingsChanged);
    EXPECT_EQ(state.settings.xemuConsoleLoggingLevel, logging::LogLevel::error);
    EXPECT_EQ(state.shell.statusMessage, "xemu console logging level set to ERROR");
  }

  TEST(ClientStateTest, TogglingAndScrollingTheOverlayUpdatesTheVisibleState) {
    app::ClientState state = app::create_initial_state();

    app::AppUpdate update = app::handle_command(state, input::UiCommand::toggle_overlay);
    EXPECT_TRUE(update.navigation.overlayChanged);
    EXPECT_TRUE(update.navigation.overlayVisibilityChanged);
    EXPECT_TRUE(state.shell.overlayVisible);
    EXPECT_EQ(state.shell.overlayScrollOffset, 0U);

    update = app::handle_command(state, input::UiCommand::previous_page);
    EXPECT_TRUE(update.navigation.overlayChanged);
    EXPECT_GT(state.shell.overlayScrollOffset, 0U);

    update = app::handle_command(state, input::UiCommand::next_page);
    EXPECT_TRUE(update.navigation.overlayChanged);
    EXPECT_EQ(state.shell.overlayScrollOffset, 0U);
  }

  TEST(ClientStateTest, BackFromHostsDoesNotRequestShutdown) {
    app::ClientState state = app::create_initial_state();

    const app::AppUpdate update = app::handle_command(state, input::UiCommand::back);

    EXPECT_FALSE(update.navigation.exitRequested);
    EXPECT_FALSE(state.shell.shouldExit);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
  }

  /**
   * @brief Refresh host actions after an address is entered and save a configured endpoint.
   */
  TEST(ClientStateTest, CanSaveAManualHostEntryWithACustomPort) {
    app::ClientState state = app::create_initial_state();

    state.hosts.focusArea = app::HostsFocusArea::toolbar;
    state.hosts.selectedToolbarButtonIndex = 2U;
    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);

    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::add_host);
    state.addHostDraft.addressInput = test_support::kTestIpv4Addresses[test_support::kIpManualCustomPort];
    state.addHostDraft.portInput = "48000";
    app::handle_command(state, input::UiCommand::none);
    state.menu.select_item_by_id("save-host");
    update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_TRUE(update.navigation.screenChanged);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
    ASSERT_EQ(state.hosts.size(), 1U);
    EXPECT_EQ(state.hosts.front().address, test_support::kTestIpv4Addresses[test_support::kIpManualCustomPort]);
    EXPECT_EQ(state.hosts.front().port, test_support::kTestPorts[test_support::kPortDefaultHost]);
    EXPECT_EQ(state.hosts.front().displayName, "Host " + std::string(test_support::kTestIpv4Addresses[test_support::kIpManualCustomPort]));
    EXPECT_EQ(state.hosts.selectedHostIndex, 0U);
  }

  /**
   * @brief Keep duplicate validation and cancellation available after form actions appear.
   */
  TEST(ClientStateTest, RejectsDuplicateHostEntriesAndAllowsCancellationBackToHosts) {
    app::ClientState state = app::create_initial_state();

    state.hosts.focusArea = app::HostsFocusArea::toolbar;
    state.hosts.selectedToolbarButtonIndex = 2U;
    app::handle_command(state, input::UiCommand::activate);
    state.addHostDraft.addressInput = test_support::kTestIpv4Addresses[test_support::kIpHostGridA];
    app::handle_command(state, input::UiCommand::none);
    state.menu.select_item_by_id("save-host");
    app::handle_command(state, input::UiCommand::activate);

    ASSERT_EQ(state.hosts.size(), 1U);
    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::hosts);

    state.hosts.focusArea = app::HostsFocusArea::toolbar;
    state.hosts.selectedToolbarButtonIndex = 2U;
    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.navigation.screenChanged);
    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::add_host);

    state.addHostDraft.addressInput = test_support::kTestIpv4Addresses[test_support::kIpHostGridA];
    app::handle_command(state, input::UiCommand::none);
    state.menu.select_item_by_id("save-host");
    update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_FALSE(update.navigation.screenChanged);
    EXPECT_EQ(state.addHostDraft.validationMessage, "That host is already saved");

    state.menu.select_item_by_id("cancel-add-host");
    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.navigation.screenChanged);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
  }

  TEST(ClientStateTest, SelectingAnUnpairedHostStartsPairing) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Living Room PC", test_support::kTestIpv4Addresses[test_support::kIpLivingRoom], 0, app::PairingState::not_paired},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    const app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_TRUE(update.navigation.screenChanged);
    EXPECT_TRUE(update.requests.pairingRequested);
    EXPECT_EQ(update.requests.pairingAddress, test_support::kTestIpv4Addresses[test_support::kIpLivingRoom]);
    EXPECT_EQ(update.requests.pairingPort, app::DEFAULT_HOST_PORT);
    EXPECT_TRUE(app::is_valid_pairing_pin(update.requests.pairingPin));
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::pair_host);
    EXPECT_EQ(state.pairingDraft.stage, app::PairingStage::pin_ready);
    EXPECT_EQ(state.pairingDraft.generatedPin, update.requests.pairingPin);
    EXPECT_EQ(state.pairingDraft.statusMessage, "Enter the PIN on the host. Pairing will continue automatically.");
    EXPECT_FALSE(state.hosts.loaded);
    EXPECT_TRUE(state.hosts.empty());
    EXPECT_TRUE(state.hosts.activeLoaded);
  }

  TEST(ClientStateTest, SelectingAnUnpairedHostPrefersTheResolvedHttpPortForPairing) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Living Room PC", test_support::kTestIpv4Addresses[test_support::kIpLivingRoom], 0, app::PairingState::not_paired},
                              });
    state.hosts.front().resolvedHttpPort = test_support::kTestPorts[test_support::kPortResolvedHttp];

    app::handle_command(state, input::UiCommand::move_down);
    const app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_TRUE(update.requests.pairingRequested);
    EXPECT_EQ(update.requests.pairingPort, test_support::kTestPorts[test_support::kPortResolvedHttp]);
    EXPECT_EQ(state.pairingDraft.targetPort, test_support::kTestPorts[test_support::kPortResolvedHttp]);
  }

  TEST(ClientStateTest, SelectingAnOfflineUnpairedHostDoesNotOpenThePairingScreen) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Living Room PC", test_support::kTestIpv4Addresses[test_support::kIpLivingRoom], 0, app::PairingState::not_paired, app::HostReachability::offline},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    const app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_FALSE(update.navigation.screenChanged);
    EXPECT_FALSE(update.requests.pairingRequested);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
    EXPECT_EQ(state.shell.statusMessage, "Host is offline. Bring it online before pairing.");
  }

  TEST(ClientStateTest, BackingOutOfThePairingScreenRequestsPairingCancellation) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Living Room PC", test_support::kTestIpv4Addresses[test_support::kIpLivingRoom], 0, app::PairingState::not_paired, app::HostReachability::online},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::pair_host);
    ASSERT_TRUE(update.requests.pairingRequested);

    update = app::handle_command(state, input::UiCommand::back);

    EXPECT_TRUE(update.navigation.screenChanged);
    EXPECT_TRUE(update.requests.pairingCancelledRequested);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
  }

  TEST(ClientStateTest, HostGridNavigationMatchesTheRenderedThreeColumnLayout) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Host A", test_support::kTestIpv4Addresses[test_support::kIpHostGridA], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Host B", test_support::kTestIpv4Addresses[test_support::kIpHostGridB], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Host C", test_support::kTestIpv4Addresses[test_support::kIpHostGridC], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Host D", test_support::kTestIpv4Addresses[test_support::kIpHostGridD], 0, app::PairingState::paired, app::HostReachability::online},
                              });

    EXPECT_EQ(state.hosts.selectedHostIndex, 0U);
    app::handle_command(state, input::UiCommand::move_right);
    EXPECT_EQ(state.hosts.selectedHostIndex, 1U);
    app::handle_command(state, input::UiCommand::move_right);
    EXPECT_EQ(state.hosts.selectedHostIndex, 2U);
    app::handle_command(state, input::UiCommand::move_down);
    EXPECT_EQ(state.hosts.selectedHostIndex, 3U);
  }

  TEST(ClientStateTest, HostGridCanMoveDownIntoAPartialNextRowFromAnyColumn) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Host A", test_support::kTestIpv4Addresses[test_support::kIpHostGridA], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Host B", test_support::kTestIpv4Addresses[test_support::kIpHostGridB], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Host C", test_support::kTestIpv4Addresses[test_support::kIpHostGridC], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Host D", test_support::kTestIpv4Addresses[test_support::kIpHostGridD], 0, app::PairingState::paired, app::HostReachability::online},
                              });

    app::handle_command(state, input::UiCommand::move_right);
    EXPECT_EQ(state.hosts.selectedHostIndex, 1U);
    app::handle_command(state, input::UiCommand::move_down);
    EXPECT_EQ(state.hosts.selectedHostIndex, 3U);

    state.hosts.selectedHostIndex = 2U;
    app::handle_command(state, input::UiCommand::move_down);
    EXPECT_EQ(state.hosts.selectedHostIndex, 3U);
  }

  TEST(ClientStateTest, HostGridWrapsRightToTheNextRowAndLeftToThePreviousRow) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Host A", test_support::kTestIpv4Addresses[test_support::kIpHostGridA], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Host B", test_support::kTestIpv4Addresses[test_support::kIpHostGridB], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Host C", test_support::kTestIpv4Addresses[test_support::kIpHostGridC], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Host D", test_support::kTestIpv4Addresses[test_support::kIpHostGridD], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Host E", test_support::kTestIpv4Addresses[test_support::kIpHostGridE], 0, app::PairingState::paired, app::HostReachability::online},
                              });

    state.hosts.selectedHostIndex = 2U;
    app::handle_command(state, input::UiCommand::move_right);
    EXPECT_EQ(state.hosts.selectedHostIndex, 3U);

    app::handle_command(state, input::UiCommand::move_left);
    EXPECT_EQ(state.hosts.selectedHostIndex, 2U);
  }

  TEST(ClientStateTest, SelectingAPairedHostOpensTheAppsScreen) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    const app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_FALSE(update.navigation.screenChanged);
    EXPECT_TRUE(update.requests.appsBrowseRequested);
    EXPECT_FALSE(update.requests.appsBrowseShowHidden);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
  }

  TEST(ClientStateTest, EnteringTheAppsScreenUnloadsTheHostsPageData) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    ASSERT_TRUE(app::begin_selected_host_app_browse(state, false));

    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::apps);
    EXPECT_FALSE(state.hosts.loaded);
    EXPECT_TRUE(state.hosts.empty());
    EXPECT_TRUE(state.hosts.activeLoaded);
    EXPECT_EQ(state.hosts.active.address, test_support::kTestIpv4Addresses[test_support::kIpOffice]);
  }

  TEST(ClientStateTest, SelectingAnOfflinePairedHostDoesNotOpenTheAppsScreen) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::offline},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    const app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_FALSE(update.navigation.screenChanged);
    EXPECT_TRUE(update.requests.appsBrowseRequested);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
    EXPECT_TRUE(state.shell.statusMessage.empty());
  }

  TEST(ClientStateTest, AppliesFetchedAppListsAndPreservesPerAppFlags) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    ASSERT_TRUE(app::begin_selected_host_app_browse(state, false));
    state.hosts.active.runningGameId = 101;
    state.hosts.active.apps = {
      {"Steam", 101, false, true, true, "cached-steam", true, false},
    };

    app::apply_app_list_result(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], {
                                                                                                                                                             {"Steam", 101, true, false, false, "cached-steam", false, false},
                                                                                                                                                             {"Desktop", 102, false, false, false, "cached-desktop", true, false},
                                                                                                                                                           },
                               0x55AAU,
                               true,
                               "Loaded 2 app(s)");

    ASSERT_EQ(state.hosts.active.apps.size(), 2U);
    EXPECT_EQ(state.hosts.active.appListState, app::HostAppListState::ready);
    EXPECT_TRUE(state.hosts.active.apps[0].hidden);
    EXPECT_TRUE(state.hosts.active.apps[0].favorite);
    EXPECT_TRUE(state.hosts.active.apps[0].boxArtCached);
    EXPECT_TRUE(state.hosts.active.apps[0].running);
    EXPECT_TRUE(state.hosts.active.apps[1].boxArtCached);
    EXPECT_TRUE(state.shell.statusMessage.empty());
  }

  TEST(ClientStateTest, AppliesFetchedAppListsWhenBackgroundTasksReportTheResolvedHttpPort) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    state.hosts.front().resolvedHttpPort = test_support::kTestPorts[test_support::kPortResolvedHttp];
    ASSERT_TRUE(app::begin_selected_host_app_browse(state, false));

    app::apply_app_list_result(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortResolvedHttp], {
                                                                                                                                                              {"Steam", 101, true, false, false, "steam-cover", true, false},
                                                                                                                                                            },
                               0xBEEFU,
                               true,
                               "Loaded 1 app(s)");

    ASSERT_EQ(state.hosts.active.apps.size(), 1U);
    EXPECT_EQ(state.hosts.active.appListState, app::HostAppListState::ready);
    EXPECT_EQ(state.hosts.active.apps.front().name, "Steam");
    EXPECT_TRUE(state.shell.statusMessage.empty());
  }

  TEST(ClientStateTest, MarksCoverArtCachedWhenBackgroundTasksReportTheResolvedHttpsPort) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    ASSERT_TRUE(app::begin_selected_host_app_browse(state, false));
    state.hosts.active.httpsPort = test_support::kTestPorts[test_support::kPortResolvedHttps];
    state.hosts.active.apps = {
      {"Steam", 101, true, false, false, "steam-cover", false, false},
    };

    app::mark_cover_art_cached(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortResolvedHttps], 101);

    ASSERT_EQ(state.hosts.active.apps.size(), 1U);
    EXPECT_TRUE(state.hosts.active.apps.front().boxArtCached);
  }

  TEST(ClientStateTest, SuccessfulAppListRefreshMarksHostsDirtyForPersistence) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    ASSERT_FALSE(state.hosts.dirty);
    ASSERT_TRUE(app::begin_selected_host_app_browse(state, false));

    app::apply_app_list_result(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], {
                                                                                                                                                             {"Steam", 101, true, false, false, "steam-cover", true, false},
                                                                                                                                                           },
                               0xACEDU,
                               true,
                               "Loaded 1 app(s)");

    EXPECT_TRUE(state.hosts.dirty);
    ASSERT_EQ(state.hosts.active.apps.size(), 1U);
    EXPECT_EQ(state.hosts.active.appListContentHash, 0xACEDU);
  }

  TEST(ClientStateTest, FailedRefreshKeepsCachedAppsAvailable) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    ASSERT_TRUE(app::begin_selected_host_app_browse(state, false));
    state.hosts.active.apps = {
      {"Steam", 101, false, false, false, "cached-steam", true, false},
    };
    state.hosts.active.appListContentHash = 0x1234U;

    app::apply_app_list_result(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], {}, 0, false, "Timed out while refreshing apps");

    EXPECT_EQ(state.hosts.active.appListState, app::HostAppListState::ready);
    ASSERT_EQ(state.hosts.active.apps.size(), 1U);
    EXPECT_EQ(state.hosts.active.apps.front().name, "Steam");
    EXPECT_EQ(state.shell.statusMessage, "Timed out while refreshing apps");
  }

  TEST(ClientStateTest, LeavingTheAppsScreenUnloadsTheInMemoryAppList) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    ASSERT_TRUE(app::begin_selected_host_app_browse(state, false));
    state.hosts.active.apps = {
      {"Steam", 101, false, false, false, "cached-steam", true, false},
    };
    state.hosts.active.appListState = app::HostAppListState::ready;

    const app::AppUpdate update = app::handle_command(state, input::UiCommand::back);

    EXPECT_TRUE(update.navigation.screenChanged);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
    EXPECT_TRUE(state.hosts.active.apps.empty());
    EXPECT_EQ(state.hosts.active.appListState, app::HostAppListState::idle);
  }

  TEST(ClientStateTest, ExplicitUnpairedAppListFailureMarksTheHostAsNotPaired) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::activate);

    app::apply_app_list_result(
      state,
      test_support::kTestIpv4Addresses[test_support::kIpOffice],
      test_support::kTestPorts[test_support::kPortDefaultHost],
      {},
      0,
      false,
      "The host reports that this client is no longer paired. Pair the host again."
    );

    EXPECT_EQ(state.hosts.front().pairingState, app::PairingState::not_paired);
    EXPECT_EQ(state.hosts.front().appListState, app::HostAppListState::failed);
  }

  TEST(ClientStateTest, ExplicitUnpairedAppListFailureClearsCachedApps) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::activate);
    state.hosts.front().apps = {
      {"Steam", 101, false, false, false, "cached-steam", true, false},
    };
    state.hosts.front().appListContentHash = 0x1234U;

    app::apply_app_list_result(
      state,
      test_support::kTestIpv4Addresses[test_support::kIpOffice],
      test_support::kTestPorts[test_support::kPortDefaultHost],
      {},
      0,
      false,
      "The client is not authorized. Certificate verification failed."
    );

    EXPECT_TRUE(state.hosts.front().apps.empty());
    EXPECT_EQ(state.hosts.front().pairingState, app::PairingState::not_paired);
    EXPECT_EQ(state.hosts.front().appListState, app::HostAppListState::failed);
  }

  TEST(ClientStateTest, TransientAppListFailuresDoNotMarkTheHostAsNotPaired) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::activate);

    app::apply_app_list_result(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], {}, 0, false, "Timed out while refreshing apps");

    EXPECT_EQ(state.hosts.front().pairingState, app::PairingState::paired);
  }

  TEST(ClientStateTest, AppGridWrapsHorizontallyAndFindsTheClosestItemInPartialRows) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    ASSERT_TRUE(app::begin_selected_host_app_browse(state, false));
    app::apply_app_list_result(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], {
                                                                                                                                                             {"App 1", 1, false, false, false, "app-1", false, false},
                                                                                                                                                             {"App 2", 2, false, false, false, "app-2", false, false},
                                                                                                                                                             {"App 3", 3, false, false, false, "app-3", false, false},
                                                                                                                                                             {"App 4", 4, false, false, false, "app-4", false, false},
                                                                                                                                                             {"App 5", 5, false, false, false, "app-5", false, false},
                                                                                                                                                           },
                               0x99U,
                               true,
                               "Loaded 5 app(s)");

    state.apps.selectedAppIndex = 3U;
    app::handle_command(state, input::UiCommand::move_right);
    EXPECT_EQ(state.apps.selectedAppIndex, 4U);

    app::handle_command(state, input::UiCommand::move_left);
    EXPECT_EQ(state.apps.selectedAppIndex, 3U);

    state.apps.selectedAppIndex = 2U;
    app::handle_command(state, input::UiCommand::move_down);
    EXPECT_EQ(state.apps.selectedAppIndex, 4U);
  }

  TEST(ClientStateTest, LogViewerCanScrollAndCyclePlacement) {
    app::ClientState state = app::create_initial_state();
    app::apply_log_viewer_contents(state, {"line-1", "line-2", "line-3", "line-4"}, "Loaded log file preview");

    ASSERT_EQ(state.modal.id, app::ModalId::log_viewer);
    EXPECT_EQ(state.shell.statusMessage, "Loaded log file preview");
    EXPECT_EQ(state.settings.logViewerPlacement, app::LogViewerPlacement::full);
    EXPECT_EQ(state.settings.logViewerScrollOffset, 0U);

    app::handle_command(state, input::UiCommand::previous_page);
    EXPECT_EQ(state.settings.logViewerScrollOffset, 1U);

    app::handle_command(state, input::UiCommand::fast_previous_page);
    EXPECT_EQ(state.settings.logViewerScrollOffset, 3U);

    app::handle_command(state, input::UiCommand::next_page);
    EXPECT_EQ(state.settings.logViewerScrollOffset, 2U);

    app::handle_command(state, input::UiCommand::delete_character);
    EXPECT_EQ(state.settings.logViewerPlacement, app::LogViewerPlacement::left);

    app::handle_command(state, input::UiCommand::open_context_menu);
    EXPECT_EQ(state.settings.logViewerPlacement, app::LogViewerPlacement::right);

    const app::AppUpdate update = app::handle_command(state, input::UiCommand::back);
    EXPECT_TRUE(update.navigation.modalClosed);
    EXPECT_EQ(state.modal.id, app::ModalId::none);
  }

  TEST(ClientStateTest, LeavingTheAppsScreenClearsTransientAppStatusAndIgnoresLaterRefreshText) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    ASSERT_TRUE(app::begin_selected_host_app_browse(state, false));
    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::apps);
    EXPECT_TRUE(state.shell.statusMessage.empty());

    app::AppUpdate update = app::handle_command(state, input::UiCommand::back);
    EXPECT_TRUE(update.navigation.screenChanged);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
    EXPECT_TRUE(state.shell.statusMessage.empty());

    app::apply_app_list_result(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], {
                                                                                                                                                             {"Steam", 101, false, false, false, "steam-cover", false, false},
                                                                                                                                                           },
                               0,
                               false,
                               "The host applist response did not contain any app entries");

    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
    EXPECT_TRUE(state.shell.statusMessage.empty());
    EXPECT_EQ(state.hosts.active.appListState, app::HostAppListState::idle);
    EXPECT_TRUE(state.hosts.active.appListStatusMessage.empty());
  }

  TEST(ClientStateTest, SettingsCanRequestDeletionOfSavedFilesFromTheResetCategory) {
    app::ClientState state = app::create_initial_state();
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::settings);

    app::replace_saved_files(state, {
                                      {R"(E:\UDATA\12345678\moonlight.log)", "moonlight.log", 128U},
                                    });
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.settings.selectedCategory, app::SettingsCategory::reset);
    ASSERT_EQ(state.settings.focusArea, app::SettingsFocusArea::options);
    ASSERT_TRUE(state.detailMenu.select_item_by_id("delete-saved-file:E:\\UDATA\\12345678\\moonlight.log"));

    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_TRUE(update.navigation.modalOpened);
    EXPECT_EQ(state.modal.id, app::ModalId::confirmation);

    update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_TRUE(update.persistence.savedFileDeleteRequested);
    EXPECT_EQ(update.persistence.savedFileDeletePath, "E:\\UDATA\\12345678\\moonlight.log");
  }

  TEST(ClientStateTest, SettingsFactoryResetUsesAConfirmationDialog) {
    app::ClientState state = app::create_initial_state();
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::settings);

    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.settings.selectedCategory, app::SettingsCategory::reset);
    ASSERT_EQ(state.settings.focusArea, app::SettingsFocusArea::options);

    const app::AppUpdate openUpdate = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(openUpdate.navigation.modalOpened);
    ASSERT_EQ(state.modal.id, app::ModalId::confirmation);

    const app::AppUpdate confirmUpdate = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(confirmUpdate.persistence.factoryResetRequested);
  }

  TEST(ClientStateTest, HostContextMenuCanDeleteTheSelectedHost) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Living Room PC", test_support::kTestIpv4Addresses[test_support::kIpLivingRoom], 0, app::PairingState::not_paired},
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::open_context_menu);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    const app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_TRUE(update.persistence.hostsChanged);
    ASSERT_EQ(state.hosts.size(), 1U);
    EXPECT_EQ(state.hosts.front().displayName, "Office PC");
  }

  TEST(ClientStateTest, DeletingAPairedHostRequestsPersistentCleanupAndMarksItForManualRePairing) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });
    state.hosts.front().apps = {
      {"Steam", 101, false, false, false, "steam-cover", true, false},
      {"Desktop", 102, false, false, false, "desktop-cover", true, false},
      {"Duplicate", 103, false, false, false, "steam-cover", true, false},
    };
    state.hosts.front().appListState = app::HostAppListState::ready;

    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::open_context_menu);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    const app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_TRUE(update.persistence.hostsChanged);
    EXPECT_TRUE(update.persistence.hostDeleteCleanupRequested);
    EXPECT_TRUE(update.persistence.deletedHostWasPaired);
    EXPECT_EQ(update.persistence.deletedHostAddress, test_support::kTestIpv4Addresses[test_support::kIpOffice]);
    EXPECT_EQ(update.persistence.deletedHostPort, test_support::kTestPorts[test_support::kPortDefaultHost]);
    ASSERT_EQ(update.persistence.deletedHostCoverArtCacheKeys.size(), 2U);
    EXPECT_EQ(update.persistence.deletedHostCoverArtCacheKeys[0], "steam-cover");
    EXPECT_EQ(update.persistence.deletedHostCoverArtCacheKeys[1], "desktop-cover");
    EXPECT_TRUE(state.hosts.empty());
    EXPECT_TRUE(app::host_requires_manual_pairing(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost]));
  }

  TEST(ClientStateTest, SuccessfulRePairingClearsTheManualRePairRequirementAfterHostDeletion) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::open_context_menu);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    const app::AppUpdate deleteUpdate = app::handle_command(state, input::UiCommand::activate);

    ASSERT_TRUE(deleteUpdate.persistence.hostDeleteCleanupRequested);
    ASSERT_TRUE(app::host_requires_manual_pairing(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost]));

    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::not_paired, app::HostReachability::online},
                              });

    EXPECT_TRUE(app::apply_pairing_result(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], true, "Paired successfully"));
    EXPECT_FALSE(app::host_requires_manual_pairing(state, test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost]));
    EXPECT_EQ(state.hosts.front().pairingState, app::PairingState::paired);
  }

  /**
   * @brief Reveal and dispatch connection testing after entering a host address.
   */
  TEST(ClientStateTest, RequestsAConnectionTestFromTheAddHostScreen) {
    app::ClientState state = app::create_initial_state();

    app::handle_command(state, input::UiCommand::activate);
    state.addHostDraft.addressInput = test_support::kTestIpv4Addresses[test_support::kIpHostGridA];
    state.addHostDraft.portInput = "48000";
    app::handle_command(state, input::UiCommand::none);
    state.menu.select_item_by_id("test-connection");

    const app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_TRUE(update.requests.connectionTestRequested);
    EXPECT_EQ(update.requests.connectionTestAddress, test_support::kTestIpv4Addresses[test_support::kIpHostGridA]);
    EXPECT_EQ(update.requests.connectionTestPort, test_support::kTestPorts[test_support::kPortDefaultHost]);
    EXPECT_EQ(state.shell.statusMessage, "Testing connection to " + std::string(test_support::kTestIpv4Addresses[test_support::kIpHostGridA]) + ":48000...");
    EXPECT_TRUE(update.navigation.modalOpened);
    EXPECT_EQ(state.modal.id, app::ModalId::connection_test);
    EXPECT_TRUE(state.connectionTest.inProgress);
  }

  /**
   * @brief Keep address edits isolated and replace the selected octet on new input.
   */
  TEST(ClientStateTest, StagesCancelsDeletesAndAcceptsAddHostKeypadEdits) {
    app::ClientState state = app::create_initial_state();

    app::handle_command(state, input::UiCommand::activate);
    state.addHostDraft.addressInput = test_support::kTestIpv4Addresses[test_support::kIpHostGridA];

    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_FALSE(update.navigation.screenChanged);
    EXPECT_TRUE(state.addHostDraft.keypad.visible);
    EXPECT_EQ(state.addHostDraft.keypad.selectedButtonIndex, 0U);
    EXPECT_EQ(state.addHostDraft.keypad.stagedInput, test_support::kTestIpv4Addresses[test_support::kIpHostGridA]);

    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.addHostDraft.keypad.addressOctets[0], "1");
    EXPECT_EQ(state.addHostDraft.keypad.addressOctets[3], "10");

    app::handle_command(state, input::UiCommand::delete_character);
    EXPECT_TRUE(state.addHostDraft.keypad.addressOctets[0].empty());

    app::handle_command(state, input::UiCommand::back);
    EXPECT_FALSE(state.addHostDraft.keypad.visible);
    EXPECT_EQ(state.addHostDraft.addressInput, test_support::kTestIpv4Addresses[test_support::kIpHostGridA]);

    app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(state.addHostDraft.keypad.visible);
    app::handle_command(state, input::UiCommand::activate);
    const std::string editedAddress = "1.168.0.10";  // NOSONAR(cpp:S1313) deliberate edited IPv4 fixture.
    EXPECT_EQ(state.addHostDraft.keypad.stagedInput, editedAddress);
    app::handle_command(state, input::UiCommand::confirm);
    EXPECT_FALSE(state.addHostDraft.keypad.visible);
    EXPECT_EQ(state.addHostDraft.addressInput, editedAddress);
  }

  /**
   * @brief Advance full IPv4 sections, edit earlier sections and enforce byte bounds.
   */
  TEST(ClientStateTest, AddressKeypadAdvancesAndSwitchesOctetsWithoutChangingKeyFocus) {
    auto state = app::create_initial_state();
    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::activate);
    auto &keypad = state.addHostDraft.keypad;
    app::handle_command(state, input::UiCommand::previous_page);
    EXPECT_EQ(keypad.activeOctet, 0U);
    for (int digit = 0; digit < 3; ++digit) {
      app::handle_command(state, input::UiCommand::activate);
    }
    EXPECT_EQ(keypad.addressOctets[0], "111");
    EXPECT_EQ(keypad.activeOctet, 1U);
    app::handle_command(state, input::UiCommand::delete_character);
    EXPECT_EQ(keypad.activeOctet, 0U);
    EXPECT_EQ(keypad.addressOctets[0], "11");
    app::handle_command(state, input::UiCommand::next_page);
    keypad.selectedButtonIndex = 1U;
    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::activate);
    keypad.selectedButtonIndex = 7U;
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(keypad.addressOctets[1], "228");
    EXPECT_EQ(keypad.activeOctet, 2U);
    app::handle_command(state, input::UiCommand::previous_page);
    EXPECT_EQ(keypad.selectedButtonIndex, 7U);
    keypad.selectedButtonIndex = 1U;
    app::handle_command(state, input::UiCommand::activate);
    keypad.selectedButtonIndex = 4U;
    app::handle_command(state, input::UiCommand::activate);
    keypad.selectedButtonIndex = 5U;
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(keypad.addressOctets[1], "25");
    EXPECT_EQ(keypad.activeOctet, 1U);
    EXPECT_FALSE(state.addHostDraft.validationMessage.empty());
    keypad.selectedButtonIndex = 4U;
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(keypad.addressOctets[1], "255");
    EXPECT_TRUE(state.addHostDraft.validationMessage.empty());
    app::handle_command(state, input::UiCommand::next_page);
    EXPECT_EQ(keypad.activeOctet, 3U);
    app::handle_command(state, input::UiCommand::next_page);
    EXPECT_EQ(keypad.activeOctet, 3U);
    keypad.selectedButtonIndex = 0U;
    for (int digit = 0; digit < 4; ++digit) {
      app::handle_command(state, input::UiCommand::activate);
    }
    EXPECT_EQ(keypad.addressOctets[3], "111");
    EXPECT_EQ(keypad.activeOctet, 3U);
    app::handle_command(state, input::UiCommand::next_page);
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(keypad.addressOctets[3], "111");
  }

  /**
   * @brief Keep an erased address empty and preserve the optional port editing workflow.
   */
  TEST(ClientStateTest, EmptyAddressAndPortKeypadEditsRemainIndependent) {
    auto state = app::create_initial_state();
    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::delete_character);
    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::delete_character);
    EXPECT_TRUE(state.addHostDraft.keypad.stagedInput.empty());
    app::handle_command(state, input::UiCommand::confirm);
    EXPECT_TRUE(state.addHostDraft.addressInput.empty());
    EXPECT_FALSE(state.menu.select_item_by_id("test-connection"));
    ASSERT_TRUE(state.menu.select_item_by_id("edit-port"));
    app::handle_command(state, input::UiCommand::activate);
    for (const auto command : {input::UiCommand::previous_page, input::UiCommand::next_page, input::UiCommand::delete_character}) {
      app::handle_command(state, command);
    }
    EXPECT_EQ(state.addHostDraft.keypad.activeOctet, 0U);
    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::delete_character);
    EXPECT_EQ(state.addHostDraft.keypad.stagedInput, "1");
    app::handle_command(state, input::UiCommand::confirm);
    EXPECT_EQ(state.addHostDraft.portInput, "1");
  }

  /**
   * @brief Wrap numeric keypad focus within rows, including the single zero key.
   */
  TEST(ClientStateTest, AddHostKeypadWrapsHorizontallyAcrossRowEdges) {
    app::ClientState state = app::create_initial_state();

    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_TRUE(state.addHostDraft.keypad.visible);

    state.addHostDraft.keypad.selectedButtonIndex = 2U;
    app::handle_command(state, input::UiCommand::move_right);
    EXPECT_EQ(state.addHostDraft.keypad.selectedButtonIndex, 0U);

    app::handle_command(state, input::UiCommand::move_left);
    EXPECT_EQ(state.addHostDraft.keypad.selectedButtonIndex, 2U);

    state.addHostDraft.keypad.selectedButtonIndex = 9U;
    app::handle_command(state, input::UiCommand::move_right);
    EXPECT_EQ(state.addHostDraft.keypad.selectedButtonIndex, 9U);

    app::handle_command(state, input::UiCommand::move_left);
    EXPECT_EQ(state.addHostDraft.keypad.selectedButtonIndex, 9U);
  }

  /**
   * @brief Wrap numeric keypad focus through its short final row.
   */
  TEST(ClientStateTest, AddHostKeypadWrapsVerticallyAcrossTopAndBottomRows) {
    app::ClientState state = app::create_initial_state();

    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_TRUE(state.addHostDraft.keypad.visible);

    state.addHostDraft.keypad.selectedButtonIndex = 1U;
    app::handle_command(state, input::UiCommand::move_up);
    EXPECT_EQ(state.addHostDraft.keypad.selectedButtonIndex, 9U);

    app::handle_command(state, input::UiCommand::move_down);
    EXPECT_EQ(state.addHostDraft.keypad.selectedButtonIndex, 0U);

    state.addHostDraft.keypad.selectedButtonIndex = 8U;
    app::handle_command(state, input::UiCommand::move_down);
    EXPECT_EQ(state.addHostDraft.keypad.selectedButtonIndex, 9U);

    app::handle_command(state, input::UiCommand::move_down);
    EXPECT_EQ(state.addHostDraft.keypad.selectedButtonIndex, 0U);
  }

  /**
   * @brief Complete the visible test and update only the endpoint that was tested.
   */
  TEST(ClientStateTest, ApplyingConnectionTestResultsUpdatesTheVisibleDraftAndActiveHost) {
    app::ClientState state = app::create_initial_state();
    app::handle_command(state, input::UiCommand::activate);
    state.addHostDraft.addressInput = test_support::kTestIpv4Addresses[test_support::kIpHostGridA];
    state.hosts.activeLoaded = true;
    state.hosts.active = {
      "Host A",
      test_support::kTestIpv4Addresses[test_support::kIpHostGridA],
      test_support::kTestPorts[test_support::kPortDefaultHost],
      app::PairingState::not_paired,
      app::HostReachability::unknown,
    };
    state.connectionTest = {true, false, state.hosts.active.address, app::effective_host_port(state.hosts.active.port), {}};

    app::apply_connection_test_result(state, true, "Connected successfully");

    EXPECT_TRUE(state.addHostDraft.lastConnectionSucceeded);
    EXPECT_EQ(state.addHostDraft.connectionMessage, "Connected successfully");
    EXPECT_EQ(state.shell.statusMessage, "Connected successfully");
    EXPECT_EQ(state.hosts.active.reachability, app::HostReachability::online);
    EXPECT_FALSE(state.connectionTest.inProgress);
    EXPECT_TRUE(state.connectionTest.succeeded);
    EXPECT_EQ(state.connectionTest.message, "Connected successfully");
  }

  /**
   * @brief A manual add-host test must not change an unrelated saved host's status.
   */
  TEST(ClientStateTest, ConnectionTestResultsOnlyChangeMatchingEndpoints) {
    auto state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Other PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], 0, app::PairingState::paired, app::HostReachability::online},
                                {"Test PC", test_support::kTestIpv4Addresses[test_support::kIpHostGridA], 0, app::PairingState::paired, app::HostReachability::unknown},
                              });
    state.connectionTest = {true, false, state.hosts.items[1].address, app::DEFAULT_HOST_PORT, {}};
    app::apply_connection_test_result(state, false, "Timed out");
    EXPECT_EQ(state.hosts.items[0].reachability, app::HostReachability::online);
    EXPECT_EQ(state.hosts.items[1].reachability, app::HostReachability::offline);
    EXPECT_FALSE(state.connectionTest.inProgress);
    EXPECT_EQ(state.connectionTest.message, "Timed out");
  }

  /**
   * @brief Keep pairing hidden for blank input and validate entered addresses before starting.
   */
  TEST(ClientStateTest, AddHostStartPairingValidatesInputAndAddsNewHostsWhenNeeded) {
    app::ClientState state = app::create_initial_state();
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::add_host);
    EXPECT_FALSE(state.menu.select_item_by_id("start-pairing"));
    state.addHostDraft.addressInput = "invalid";
    app::handle_command(state, input::UiCommand::none);
    ASSERT_TRUE(state.menu.select_item_by_id("start-pairing"));

    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_FALSE(update.requests.pairingRequested);
    EXPECT_EQ(state.addHostDraft.validationMessage, "Enter a valid IPv4 host address");

    state.addHostDraft.addressInput = test_support::kTestIpv4Addresses[test_support::kIpLivingRoom];
    state.addHostDraft.portInput = "48000";
    update = app::handle_command(state, input::UiCommand::activate);

    EXPECT_TRUE(update.persistence.hostsChanged);
    EXPECT_TRUE(update.requests.pairingRequested);
    EXPECT_EQ(update.requests.pairingAddress, test_support::kTestIpv4Addresses[test_support::kIpLivingRoom]);
    EXPECT_EQ(update.requests.pairingPort, test_support::kTestPorts[test_support::kPortDefaultHost]);
    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::pair_host);
    EXPECT_TRUE(state.hosts.activeLoaded);
  }

  /**
   * @brief Show connection progress from a host context menu and retain the result until dismissed.
   */
  TEST(ClientStateTest, HostActionsModalCanRequestConnectionTestsAndOpenDetails) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::open_context_menu);
    app::handle_command(state, input::UiCommand::move_down);
    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.requests.connectionTestRequested);
    EXPECT_EQ(update.requests.connectionTestAddress, test_support::kTestIpv4Addresses[test_support::kIpOffice]);
    EXPECT_EQ(update.requests.connectionTestPort, test_support::kTestPorts[test_support::kPortDefaultHost]);
    ASSERT_EQ(state.modal.id, app::ModalId::connection_test);
    for (const auto command : {input::UiCommand::back, input::UiCommand::activate, input::UiCommand::confirm, input::UiCommand::move_down}) {
      EXPECT_FALSE(app::handle_command(state, command).navigation.modalClosed);
      EXPECT_EQ(state.modal.id, app::ModalId::connection_test);
    }
    app::apply_connection_test_result(state, false, "Connection timed out");
    EXPECT_EQ(state.hosts.front().reachability, app::HostReachability::offline);
    EXPECT_FALSE(state.connectionTest.succeeded);
    EXPECT_TRUE(app::handle_command(state, input::UiCommand::back).navigation.modalClosed);

    app::handle_command(state, input::UiCommand::open_context_menu);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.modal.id, app::ModalId::host_details);
    EXPECT_FALSE(update.navigation.modalClosed);
  }

  TEST(ClientStateTest, AppActionsModalCanToggleFlagsAndShowDetails) {
    app::ClientState state = app::create_initial_state();
    state.shell.activeScreen = app::ScreenId::apps;
    state.hosts.activeLoaded = true;
    state.apps.showHiddenApps = true;
    state.hosts.active = {
      "Office PC",
      test_support::kTestIpv4Addresses[test_support::kIpOffice],
      test_support::kTestPorts[test_support::kPortDefaultHost],
      app::PairingState::paired,
      app::HostReachability::online,
    };
    state.hosts.active.apps = {
      {"Steam", 101, false, false, false, "steam-cover", true, false},
    };

    app::handle_command(state, input::UiCommand::open_context_menu);
    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.persistence.hostsChanged);
    EXPECT_TRUE(state.hosts.active.apps.front().hidden);

    app::handle_command(state, input::UiCommand::open_context_menu);
    app::handle_command(state, input::UiCommand::move_down);
    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.modal.id, app::ModalId::app_details);
    EXPECT_FALSE(update.navigation.modalClosed);

    app::handle_command(state, input::UiCommand::back);
    app::handle_command(state, input::UiCommand::open_context_menu);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.persistence.hostsChanged);
    EXPECT_TRUE(state.hosts.active.apps.front().favorite);
  }

  TEST(ClientStateTest, DisplaySettingsCanBeActivatedAndBackNavigationReturnsFocusToCategories) {
    app::ClientState state = app::create_initial_state();
    state.settings.availableVideoModes = {
      VIDEO_MODE {640, 480, 32, 60},
      VIDEO_MODE {1280, 720, 32, 60},
    };
    state.settings.preferredVideoMode = state.settings.availableVideoModes.front();
    state.settings.preferredVideoModeSet = true;
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.shell.activeScreen, app::ScreenId::settings);

    app::handle_command(state, input::UiCommand::move_down);
    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(update.navigation.activatedItemId, "settings-category:display");
    EXPECT_EQ(state.settings.focusArea, app::SettingsFocusArea::options);

    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(update.navigation.activatedItemId, "cycle-stream-video-mode");
    EXPECT_TRUE(update.persistence.settingsChanged);
    EXPECT_EQ(state.settings.preferredVideoMode.width, 1280);
    EXPECT_EQ(state.settings.preferredVideoMode.height, 720);

    update = app::handle_command(state, input::UiCommand::back);
    EXPECT_EQ(state.settings.focusArea, app::SettingsFocusArea::categories);
    EXPECT_FALSE(update.navigation.screenChanged);
  }

  /**
   * @brief Changing stream resolution resets custom bitrate, persists both values and refreshes the menu.
   */
  TEST(ClientStateTest, DisplaySettingsCanCycleXboxVideoModeStreamChoices) {
    app::ClientState state = app::create_initial_state();
    state.settings.availableVideoModes = {
      VIDEO_MODE {640, 480, 32, 60},
      VIDEO_MODE {720, 480, 32, 60},
      VIDEO_MODE {1280, 720, 32, 60},
    };
    state.settings.preferredVideoMode = state.settings.availableVideoModes.front();
    state.settings.preferredVideoModeSet = true;
    state.settings.streamBitrateKbps = 750;

    ASSERT_NO_FATAL_FAILURE(open_display_settings_options(state));

    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(update.navigation.activatedItemId, "cycle-stream-video-mode");
    EXPECT_TRUE(update.persistence.settingsChanged);
    EXPECT_EQ(state.settings.preferredVideoMode.width, 720);
    EXPECT_EQ(state.settings.preferredVideoMode.height, 480);
    EXPECT_EQ(state.settings.streamBitrateKbps, 2000);
    EXPECT_TRUE(state.settings.dirty);
    EXPECT_EQ(state.shell.statusMessage, "Stream resolution set to 720x480");
    ASSERT_TRUE(state.detailMenu.select_item_by_id("cycle-stream-bitrate"));
    EXPECT_EQ(state.detailMenu.selected_item()->label, "Stream Bitrate: 2 Mbps");

    ASSERT_TRUE(state.detailMenu.select_item_by_id("cycle-stream-video-mode"));
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.settings.streamBitrateKbps, 5000);
    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.persistence.settingsChanged);
    EXPECT_EQ(state.settings.preferredVideoMode.width, 640);
    EXPECT_EQ(state.settings.streamBitrateKbps, 1000);
  }

  /**
   * @brief Cycling an unchanged or unavailable resolution preserves a manually selected bitrate.
   */
  TEST(ClientStateTest, DisplaySettingsPreserveCustomBitrateWithoutAResolutionChange) {
    app::ClientState state = app::create_initial_state();
    state.settings.availableVideoModes = {VIDEO_MODE {1280, 720, 32, 60}};
    state.settings.preferredVideoMode = state.settings.availableVideoModes.front();
    state.settings.preferredVideoModeSet = true;
    state.settings.streamBitrateKbps = 750;
    ASSERT_NO_FATAL_FAILURE(open_display_settings_options(state));

    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.settings.streamBitrateKbps, 750);

    state.settings.availableVideoModes.clear();
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_FALSE(state.settings.preferredVideoModeSet);
    EXPECT_EQ(state.settings.streamBitrateKbps, 750);

    state.settings.availableVideoModes = {VIDEO_MODE {1280, 720, 32, 60}};
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(state.settings.preferredVideoModeSet);
    EXPECT_EQ(state.settings.streamBitrateKbps, 5000);
  }

  /**
   * @brief FPS changes reset bitrate using the selected stream dimensions rather than display refresh.
   */
  TEST(ClientStateTest, DisplaySettingsCanCycleStreamFramerateThroughSixtyFps) {
    app::ClientState state = app::create_initial_state();
    state.settings.preferredVideoMode = VIDEO_MODE {1280, 720, 32, 50};
    state.settings.preferredVideoModeSet = true;
    state.settings.streamBitrateKbps = 750;

    ASSERT_NO_FATAL_FAILURE(open_display_settings_options(state));

    ASSERT_TRUE(state.detailMenu.select_item_by_id("cycle-stream-framerate"));
    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(update.navigation.activatedItemId, "cycle-stream-framerate");
    EXPECT_TRUE(update.persistence.settingsChanged);
    EXPECT_EQ(state.settings.streamFramerate, 60);
    EXPECT_EQ(state.settings.streamBitrateKbps, 10000);
    EXPECT_TRUE(state.settings.dirty);
    EXPECT_EQ(state.shell.statusMessage, "Stream frame rate set to 60 FPS");
    ASSERT_NE(state.detailMenu.selected_item(), nullptr);
    EXPECT_EQ(state.detailMenu.selected_item()->label, "Stream Frame Rate: 60 FPS");

    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(update.navigation.activatedItemId, "cycle-stream-framerate");
    EXPECT_EQ(state.settings.streamFramerate, 15);
    EXPECT_EQ(state.settings.streamBitrateKbps, 3000);
    EXPECT_EQ(state.shell.statusMessage, "Stream frame rate set to 15 FPS");

    state.settings.streamFramerate = 999;
    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(update.navigation.activatedItemId, "cycle-stream-framerate");
    EXPECT_EQ(state.settings.streamFramerate, 30);
    EXPECT_EQ(state.settings.streamBitrateKbps, 5000);
    EXPECT_EQ(state.shell.statusMessage, "Stream frame rate set to 30 FPS");
  }

  /**
   * @brief FPS changes before mode detection use the normal 640x480 default dimensions.
   */
  TEST(ClientStateTest, DisplaySettingsUseDefaultResolutionForFramerateChangesBeforeDetection) {
    app::ClientState state = app::create_initial_state();
    ASSERT_NO_FATAL_FAILURE(open_display_settings_options(state));
    ASSERT_TRUE(state.detailMenu.select_item_by_id("cycle-stream-framerate"));

    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.settings.streamFramerate, 60);
    EXPECT_EQ(state.settings.streamBitrateKbps, 3000);
  }

  /**
   * @brief Manual bitrate adjustment covers all Xbox defaults, wraps and formats fractional Mbps.
   */
  TEST(ClientStateTest, DisplaySettingsCycleManualBitrateInMbpsThroughTheHighestDefault) {
    app::ClientState state = app::create_initial_state();
    state.settings.streamBitrateKbps = 7000;
    ASSERT_NO_FATAL_FAILURE(open_display_settings_options(state));
    ASSERT_TRUE(state.detailMenu.select_item_by_id("cycle-stream-bitrate"));
    EXPECT_EQ(state.detailMenu.selected_item()->label, "Stream Bitrate: 7 Mbps");

    for (int bitrateKbps = 8000; bitrateKbps <= 20000; bitrateKbps += 1000) {
      state.settings.dirty = false;
      const app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
      EXPECT_TRUE(update.persistence.settingsChanged);
      EXPECT_TRUE(state.settings.dirty);
      EXPECT_EQ(state.settings.streamBitrateKbps, bitrateKbps);
    }

    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.settings.streamBitrateKbps, 500);
    EXPECT_EQ(state.shell.statusMessage, "Stream bitrate set to 0.5 Mbps");
    EXPECT_EQ(state.detailMenu.selected_item()->label, "Stream Bitrate: 0.5 Mbps");

    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.settings.streamBitrateKbps, 750);
    EXPECT_EQ(state.detailMenu.selected_item()->label, "Stream Bitrate: 0.75 Mbps");

    state.settings.streamBitrateKbps = 3500;
    app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(state.settings.streamBitrateKbps, 4000);
  }

  /**
   * @brief Other display controls leave a manually selected bitrate unchanged.
   */
  TEST(ClientStateTest, DisplaySettingsPreserveManualBitrateWhenOtherOptionsChange) {
    app::ClientState state = app::create_initial_state();
    state.settings.streamBitrateKbps = 1500;
    ASSERT_NO_FATAL_FAILURE(open_display_settings_options(state));
    constexpr std::array<const char *, 3> controls {"toggle-play-audio-on-pc", "toggle-play-audio-on-xbox", "toggle-show-performance-stats"};
    for (const char *control : controls) {
      ASSERT_TRUE(state.detailMenu.select_item_by_id(control));
      app::handle_command(state, input::UiCommand::activate);
      EXPECT_EQ(state.settings.streamBitrateKbps, 1500);
    }
  }

  TEST(ClientStateTest, DisplaySettingsCanToggleXboxAudioAndPerformanceStats) {
    app::ClientState state = app::create_initial_state();
    ASSERT_TRUE(state.settings.playAudioOnXbox);
    ASSERT_FALSE(state.settings.showPerformanceStats);

    ASSERT_NO_FATAL_FAILURE(open_display_settings_options(state));

    ASSERT_TRUE(state.detailMenu.select_item_by_id("toggle-play-audio-on-xbox"));
    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(update.navigation.activatedItemId, "toggle-play-audio-on-xbox");
    EXPECT_TRUE(update.persistence.settingsChanged);
    EXPECT_FALSE(state.settings.playAudioOnXbox);
    EXPECT_EQ(state.shell.statusMessage, "Play audio on Xbox disabled");
    ASSERT_NE(state.detailMenu.selected_item(), nullptr);
    EXPECT_EQ(state.detailMenu.selected_item()->label, "Play Audio on Xbox: Off");

    ASSERT_TRUE(state.detailMenu.select_item_by_id("toggle-show-performance-stats"));
    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(update.navigation.activatedItemId, "toggle-show-performance-stats");
    EXPECT_TRUE(update.persistence.settingsChanged);
    EXPECT_TRUE(state.settings.showPerformanceStats);
    EXPECT_EQ(state.shell.statusMessage, "Performance stats enabled");
    ASSERT_NE(state.detailMenu.selected_item(), nullptr);
    EXPECT_EQ(state.detailMenu.selected_item()->label, "Performance Stats: On");
  }

  TEST(ClientStateTest, ConfirmationModalCanBeCancelledWithoutRequestingPersistenceChanges) {
    app::ClientState state = app::create_initial_state();
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::move_left);
    app::handle_command(state, input::UiCommand::activate);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::move_down);
    app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.settings.selectedCategory, app::SettingsCategory::reset);

    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    ASSERT_EQ(state.modal.id, app::ModalId::confirmation);
    EXPECT_TRUE(update.navigation.modalOpened);

    app::handle_command(state, input::UiCommand::move_right);
    update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_TRUE(update.navigation.modalClosed);
    EXPECT_FALSE(update.persistence.factoryResetRequested);
    EXPECT_EQ(state.shell.statusMessage, "Cancelled the pending reset action");
  }

  TEST(ClientStateTest, AppsScreenCanLaunchSelectionsAndClearStatusWithDeleteCharacter) {
    app::ClientState state = app::create_initial_state();
    state.shell.activeScreen = app::ScreenId::apps;
    state.hosts.activeLoaded = true;
    state.hosts.active = {
      "Office PC",
      test_support::kTestIpv4Addresses[test_support::kIpOffice],
      test_support::kTestPorts[test_support::kPortDefaultHost],
      app::PairingState::paired,
      app::HostReachability::online,
    };
    state.hosts.active.apps = {
      {"Steam", 101, false, false, false, "steam-cover", true, false},
    };

    app::AppUpdate update = app::handle_command(state, input::UiCommand::activate);
    EXPECT_EQ(update.navigation.activatedItemId, "launch-app");
    EXPECT_TRUE(update.requests.streamLaunchRequested);
    EXPECT_EQ(state.shell.statusMessage, "Starting stream for Steam...");

    update = app::handle_command(state, input::UiCommand::delete_character);
    EXPECT_TRUE(state.shell.statusMessage.empty());
    EXPECT_FALSE(update.navigation.screenChanged);
  }

  TEST(ClientStateTest, SuccessfulPairingReturnsToHostsAndKeepsTheHostSelected) {
    app::ClientState state = app::create_initial_state();
    app::replace_hosts(state, {
                                {"Living Room PC", test_support::kTestIpv4Addresses[test_support::kIpLivingRoom], 0, app::PairingState::not_paired, app::HostReachability::online},
                                {"Office PC", test_support::kTestIpv4Addresses[test_support::kIpOffice], test_support::kTestPorts[test_support::kPortDefaultHost], app::PairingState::paired, app::HostReachability::online},
                              });

    state.hosts.selectedHostIndex = 1U;
    EXPECT_TRUE(app::apply_pairing_result(state, test_support::kTestIpv4Addresses[test_support::kIpLivingRoom], app::DEFAULT_HOST_PORT, true, "Paired successfully"));

    EXPECT_EQ(state.shell.activeScreen, app::ScreenId::hosts);
    EXPECT_EQ(state.hosts.selectedHostIndex, 0U);
    EXPECT_EQ(state.hosts.front().pairingState, app::PairingState::paired);
  }

}  // namespace
