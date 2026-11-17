/**
 * @file src/app/client_state.h
 * @brief Declares client state models and transitions.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// nxdk includes
#include <hal/video.h>

// local includes
#include "src/app/host_records.h"
#include "src/app/pairing_flow.h"
#include "src/app/screenshot_library.h"
#include "src/input/navigation_input.h"
#include "src/input/stream_menu.h"
#include "src/input/ui_input.h"
#include "src/logging/logger.h"
#include "src/startup/saved_files.h"
#include "src/ui/menu_model.h"

namespace app {

  /**
   * @brief Top-level screens used by the Moonlight client shell.
   */
  enum class ScreenId {
    home,  ///< Placeholder home screen identifier retained for shared shell logic.
    hosts,  ///< Saved-host browser and primary landing screen.
    apps,  ///< Per-host application library screen.
    add_host,  ///< Manual host entry workflow.
    pair_host,  ///< Pairing workflow for an unpaired host.
    settings,  ///< Shell settings screen.
  };

  /**
   * @brief Focus areas on the hosts page.
   */
  enum class HostsFocusArea {
    toolbar,  ///< Focus is on the hosts toolbar buttons.
    grid,  ///< Focus is on the saved-host tile grid.
  };

  /**
   * @brief Active modal surfaced on top of the current page.
   */
  enum class ModalId {
    none,  ///< No modal is currently visible.
    support,  ///< Support and help modal.
    host_actions,  ///< Host action menu for the selected host.
    host_details,  ///< Host detail sheet for the selected host.
    app_actions,  ///< App action menu for the selected app.
    app_details,  ///< App detail sheet for the selected app.
    confirmation,  ///< Destructive-action confirmation dialog.
    log_viewer,  ///< Dedicated log viewer modal.
    connection_test,  ///< Progress and result of a manual connection test.
    directory_picker,  ///< Screenshot destination browser and local folder-name keyboard.
    screenshot_viewer,  ///< Local captures associated with the selected server and app.
    quit_progress,  ///< Authenticated host-app quit operation running in the background.
  };

  /**
   * @brief Layout options for the embedded log viewer.
   */
  enum class LogViewerPlacement {
    full,  ///< Use the full modal width for the log viewer.
    left,  ///< Dock the log viewer on the left side of the split layout.
    right,  ///< Dock the log viewer on the right side of the split layout.
  };

  /**
   * @brief Focus areas used by the two-pane settings screen.
   */
  enum class SettingsFocusArea {
    categories,  ///< Focus is on the settings category list.
    options,  ///< Focus is on the options list for the selected category.
  };

  /**
   * @brief Top-level categories shown on the left side of the settings screen.
   */
  enum class SettingsCategory {
    logging,  ///< Logging and diagnostics options.
    display,  ///< Display, stream video, audio and telemetry options.
    input,  ///< Input and controller options.
    storage,  ///< Screenshot destinations and other saved-file location options.
    reset,  ///< Reset and cleanup actions.
  };

  /**
   * @brief Destructive confirmation requests surfaced in a modal popup.
   */
  enum class ConfirmationAction {
    none,  ///< No destructive action is pending confirmation.
    delete_saved_file,  ///< Delete one saved file or directory.
    factory_reset,  ///< Remove all persisted Moonlight state.
    delete_screenshot,  ///< Remove the selected screenshot and its journal entry.
    quit_app,  ///< Terminate the selected in-progress app on its host.
  };

  /**
   * @brief Active field for keypad-based host entry.
   */
  enum class AddHostField {
    address,  ///< The host IPv4 address field.
    port,  ///< The optional host port override field.
  };

  /**
   * @brief Controller selection state for the add-host keypad modal.
   */
  struct AddHostKeypadState {
    bool visible;  ///< True when the keypad modal is currently shown.
    std::size_t selectedButtonIndex;  ///< Zero-based selection inside the keypad button grid.
    std::string stagedInput;  ///< Draft text currently assembled inside the keypad modal.
    std::array<std::string, 4> addressOctets {};  ///< Independently editable IPv4 sections.
    std::size_t activeOctet = 0U;  ///< IPv4 section receiving keypad digits.
    bool replaceOctet = true;  ///< Replace the selected section when its first digit is entered.
  };

  /**
   * @brief Progress and result retained by the manual connection-test dialog.
   */
  struct ConnectionTestState {
    bool inProgress = false;  ///< True while the background test is running.
    bool succeeded = false;  ///< Whether the completed test reached the host.
    std::string address;  ///< Endpoint address being tested.
    uint16_t port = 0;  ///< Endpoint port being tested.
    std::string message;  ///< Pending guidance or completed diagnostic result.
  };

  /**
   * @brief Controller-friendly draft state for manual host entry.
   */
  struct AddHostDraft {
    std::string addressInput;  ///< Raw host address text entered by the user.
    std::string portInput;  ///< Raw port override text entered by the user.
    AddHostField activeField;  ///< Field currently targeted by directional input.
    AddHostKeypadState keypad;  ///< Nested keypad-modal selection state.
    ScreenId returnScreen;  ///< Screen to return to when add-host flow completes or cancels.
    std::string validationMessage;  ///< Validation feedback for the current address or port text.
    std::string connectionMessage;  ///< Result message from the latest host connection test.
    bool lastConnectionSucceeded;  ///< True when the latest connection test succeeded.
  };

  /**
   * @brief Context modal state shared by the hosts and apps pages.
   */
  struct ModalState {
    ModalId id = ModalId::none;  ///< Currently active modal identifier.
    std::size_t selectedActionIndex = 0;  ///< Zero-based index of the highlighted modal action.

    /**
     * @brief Return whether a modal is currently active.
     *
     * @return true when the modal identifier is not ModalId::none.
     */
    bool active() const {
      return id != ModalId::none;
    }
  };

  /**
   * @brief Content shown by the destructive-action confirmation dialog.
   */
  struct ConfirmationDialogState {
    ConfirmationAction action = ConfirmationAction::none;  ///< Requested confirmation action.
    std::string targetPath;  ///< File or directory path targeted by the action, when applicable.
    std::string title;  ///< Modal title presented to the user.
    std::vector<std::string> lines;  ///< Body lines describing the consequence of the action.
    ModalId returnModal = ModalId::none;  ///< Dialog restored after confirmation or cancellation.
    std::size_t returnActionIndex = 0;  ///< Focus to restore in the parent dialog.
  };

  /**
   * @brief Visual intent of an explicitly requested user notification.
   */
  enum class NotificationKind {
    information,  ///< Non-blocking information about a completed or degraded operation.
    success,  ///< Confirmation that a user-requested operation completed.
    error,  ///< Actionable failure requiring attention.
  };

  /**
   * @brief One notification retained independently of diagnostic and workflow status.
   */
  struct NotificationState {
    NotificationKind kind = NotificationKind::information;  ///< Notification severity and icon.
    std::string title;  ///< Concise description of the event.
    std::string message;  ///< User-facing consequence or recovery guidance.
    bool requiresInteraction = true;  ///< Retain until explicitly dismissed when true.
    std::uint32_t remainingMilliseconds = 0;  ///< Visible lifetime left for an automatic notification.
  };

  /**
   * @brief Shell-wide state that is not owned by a specific workflow screen.
   */
  struct ShellState {
    ScreenId activeScreen = ScreenId::hosts;  ///< Screen currently shown by the shell.
    input::UiInputDevice inputDevice = input::UiInputDevice::gamepad;  ///< Last intentional UI input, used for contextual helper glyphs.
    bool overlayVisible = false;  ///< True when the diagnostics overlay is visible.
    bool shouldExit = false;  ///< True when the application should terminate.
    std::size_t overlayScrollOffset = 0U;  ///< Scroll offset used by long overlay content.
    std::string statusMessage;  ///< Diagnostic or workflow status; never implicitly raises a notification.
    std::vector<NotificationState> notifications;  ///< Outstanding notifications in arrival order.
  };

  /**
   * @brief State owned by the saved-host browser and retained host snapshot.
   */
  struct HostsState {
    bool dirty = false;  ///< True when the host list changed and should be saved.
    bool loaded = false;  ///< True when the hosts page list is currently loaded in memory.
    HostsFocusArea focusArea = HostsFocusArea::toolbar;  ///< Focused region on the hosts page.
    std::size_t selectedToolbarButtonIndex = 0U;  ///< Zero-based selection inside the hosts toolbar.
    std::size_t selectedHostIndex = 0U;  ///< Zero-based selection inside the saved host list.
    std::vector<HostRecord> items;  ///< Saved hosts currently tracked by the shell.
    HostRecord active;  ///< Host snapshot kept for host-specific non-host screens after unloading the hosts page.
    bool activeLoaded = false;  ///< True when active contains a valid host snapshot.
    std::string selectedAddress;  ///< Last selected host address used to restore hosts-page selection after reload.
    uint16_t selectedPort = 0;  ///< Last selected host port override used to restore hosts-page selection after reload.
    std::vector<std::string> pairingResetEndpoints;  ///< Endpoints whose pairing material should be cleared during reset.

    /**
     * @brief Return whether the saved-host collection is empty.
     *
     * @return True when no saved hosts are currently loaded.
     */
    bool empty() const {
      return items.empty();
    }

    /**
     * @brief Return the number of saved hosts currently tracked by the shell.
     *
     * @return Number of host records stored in the collection.
     */
    std::size_t size() const {
      return items.size();
    }

    /**
     * @brief Return an iterator to the first saved host.
     *
     * @return Mutable iterator to the first element.
     */
    auto begin() {
      return items.begin();
    }

    /**
     * @brief Return an iterator one past the last saved host.
     *
     * @return Mutable iterator to the end of the collection.
     */
    auto end() {
      return items.end();
    }

    /**
     * @brief Return a const iterator to the first saved host.
     *
     * @return Const iterator to the first element.
     */
    auto begin() const {
      return items.begin();
    }

    /**
     * @brief Return a const iterator one past the last saved host.
     *
     * @return Const iterator to the end of the collection.
     */
    auto end() const {
      return items.end();
    }

    /**
     * @brief Remove every saved host from the collection.
     */
    void clear() {
      items.clear();
    }

    /**
     * @brief Return the first saved host.
     *
     * @return Reference to the first host record.
     */
    HostRecord &front() {
      return items.front();
    }

    /**
     * @brief Return the first saved host.
     *
     * @return Const reference to the first host record.
     */
    const HostRecord &front() const {
      return items.front();
    }

    /**
     * @brief Return the last saved host.
     *
     * @return Reference to the last host record.
     */
    HostRecord &back() {
      return items.back();
    }

    /**
     * @brief Return the last saved host.
     *
     * @return Const reference to the last host record.
     */
    const HostRecord &back() const {
      return items.back();
    }

    /**
     * @brief Return the saved host at the requested index.
     *
     * @param index Zero-based host index.
     * @return Reference to the host record at @p index.
     */
    HostRecord &operator[](std::size_t index) {
      return items[index];
    }

    /**
     * @brief Return the saved host at the requested index.
     *
     * @param index Zero-based host index.
     * @return Const reference to the host record at @p index.
     */
    const HostRecord &operator[](std::size_t index) const {
      return items[index];
    }
  };

  /**
   * @brief State owned by the per-host apps browser.
   */
  struct AppsState {
    std::size_t selectedAppIndex = 0U;  ///< Zero-based selection inside the visible app list.
    std::size_t scrollPage = 0U;  ///< Horizontal page offset for paged app browsing.
    bool showHiddenApps = false;  ///< True when hidden apps should remain visible in the apps screen.
    bool refreshFailureNotified = false;  ///< Suppress repeated cached-library refresh warnings until recovery.
  };

  /**
   * @brief Staged directory selection; the saved setting changes only after successful selection.
   */
  struct DirectoryPickerState {
    std::string path;  ///< Current directory, or empty for mounted drives.
    std::vector<std::string> folders;  ///< Immediate child directory paths.
    std::string error;  ///< Listing, creation or destination validation feedback.
    bool available = false;  ///< The current directory was read successfully.
    bool creating = false;  ///< Show the local folder-name keyboard.
    std::string name;  ///< Draft child folder name.
    std::size_t caret = 0;  ///< Insertion point inside the folder name.
    input::StreamMenu keyboard;  ///< Reuse the stream keyboard's pages, labels and focus geometry locally.
  };

  /**
   * @brief Persisted storage and placement preferences for local streaming controls.
   */
  struct LocalStreamPreferences {
    std::string screenshotDirectory;  ///< Screenshot folder; empty selects the title-scoped screenshots directory.
    bool keyboardDockTop = false;  ///< Dock the stream keyboard to the top instead of the bottom.
  };

  /**
   * @brief State owned by the settings, log viewer, and saved-file workflows.
   */
  struct SettingsState {
    SettingsFocusArea focusArea = SettingsFocusArea::categories;  ///< Focused pane within the settings screen.
    SettingsCategory selectedCategory = SettingsCategory::logging;  ///< Settings category selected in the left pane.
    std::string logFilePath;  ///< Path currently loaded into the log viewer.
    std::vector<std::string> logViewerLines;  ///< Loaded log file lines shown in the log viewer.
    std::size_t logViewerScrollOffset = 0U;  ///< Zero-based vertical scroll offset inside the log viewer.
    LogViewerPlacement logViewerPlacement = LogViewerPlacement::full;  ///< Log viewer pane placement relative to the shell.
    logging::LogLevel loggingLevel = logging::LogLevel::none;  ///< Minimum runtime log level written to the persisted log file.
    logging::LogLevel xemuConsoleLoggingLevel = logging::LogLevel::none;  ///< Minimum runtime log level mirrored through DbgPrint() to xemu's serial console.
    std::vector<VIDEO_MODE> availableVideoModes;  ///< Detected Xbox video modes exposed as stream-resolution choices.
    VIDEO_MODE preferredVideoMode {};  ///< Preferred stream resolution requested from the host.
    bool preferredVideoModeSet = false;  ///< True when preferredVideoMode contains a user-selected or default mode.
    int streamFramerate = 30;  ///< Preferred stream frame rate in frames per second.
    int streamBitrateKbps = 1000;  ///< Preferred stream bitrate in kilobits per second.
    bool playAudioOnPc = false;  ///< True when the host PC should continue local audio playback during streaming.
    bool showPerformanceStats = false;  ///< Display live performance telemetry and include it in the stream summary.
    bool playAudioOnXbox = true;  ///< True when the Xbox should decode and play streamed audio locally.
    LocalStreamPreferences controls;  ///< Screenshot storage and keyboard placement preferences.
    bool dirty = false;  ///< True when persisted TOML-backed settings changed and should be saved.
    std::vector<startup::SavedFileEntry> savedFiles;  ///< Saved-file catalog shown on the reset settings page.
    bool savedFilesDirty = true;  ///< True when the saved-file catalog should be refreshed.
  };

  /**
   * @brief Transient captures and selection for one app's local screenshot viewer.
   */
  struct ScreenshotViewerState {
    ScreenshotSource source;  ///< Stable app and host identity captured when the viewer opens.
    std::vector<ScreenshotRecord> entries;  ///< Matching captures ordered newest first.
    std::size_t selectedIndex = 0;  ///< Image currently displayed; zero for an empty library.
    std::string error;  ///< Any recoverable journal warning or read error.
  };

  /**
   * @brief Retain the user-confirmed host and app until its quit operation completes.
   */
  struct AppQuitState {
    std::string address;  ///< Saved server endpoint captured before confirmation.
    uint16_t port = 0;  ///< Saved HTTP port override.
    int appId = 0;  ///< Expected running app; prevents quitting a different session.
    std::string appName;  ///< App label for progress and completion feedback.
    bool inProgress = false;  ///< True while the background quit request owns the progress dialog.
  };

  /**
   * @brief Complete local application state, including transient dialogs and screenshot browsing.
   */
  struct ClientState {
    ShellState shell;  ///< Shell-wide status and overlay state.
    HostsState hosts;  ///< Saved-host browsing state and retained host snapshot.
    AppsState apps;  ///< Apps-screen selection and paging state.
    ui::MenuModel menu;  ///< Primary vertical menu model for the active screen.
    ui::MenuModel detailMenu;  ///< Secondary detail or actions menu.
    AddHostDraft addHostDraft;  ///< Draft state for the add-host workflow.
    ConnectionTestState connectionTest;  ///< Manual test progress and result.
    PairingDraft pairingDraft;  ///< Draft state for the active pairing workflow.
    ModalState modal;  ///< Context modal currently stacked over the shell.
    SettingsState settings;  ///< Settings, log viewer, and saved-file workflow state.
    DirectoryPickerState directoryPicker;  ///< Pending screenshot-directory workflow.
    ScreenshotViewerState screenshotViewer;  ///< Local app screenshot library, released when browsing ends.
    AppQuitState appQuit;  ///< Retained target for a confirmed host-app quit operation.
    ConfirmationDialogState confirmation;  ///< Confirmation dialog content for destructive actions.
  };

  /**
   * @brief Navigation and modal effects emitted by one command update.
   */
  struct AppNavigationUpdate {
    bool screenChanged = false;  ///< True when the active screen changed.
    bool overlayChanged = false;  ///< True when overlay content changed.
    bool overlayVisibilityChanged = false;  ///< True when overlay visibility toggled.
    bool exitRequested = false;  ///< True when the shell requested application exit.
    bool modalOpened = false;  ///< True when a modal became active during the update.
    bool modalClosed = false;  ///< True when the active modal was dismissed during the update.
    std::string activatedItemId;  ///< Stable identifier for the activated menu item, when any.
  };

  /**
   * @brief Network and browsing requests emitted by one command update.
   */
  struct AppRequestUpdate {
    bool connectionTestRequested = false;  ///< True when a manual host connection test should run.
    std::string connectionTestAddress;  ///< Host address that should be tested.
    uint16_t connectionTestPort = 0;  ///< Host port that should be tested.
    bool pairingRequested = false;  ///< True when manual pairing should begin.
    bool pairingCancelledRequested = false;  ///< True when an in-progress pairing request should be cancelled.
    std::string pairingAddress;  ///< Host address targeted by pairing.
    uint16_t pairingPort = 0;  ///< Host port targeted by pairing.
    std::string pairingPin;  ///< Generated client PIN that should be shown to the user.
    bool appsBrowseRequested = false;  ///< True when app browsing for the selected host should begin.
    bool appsBrowseShowHidden = false;  ///< Hidden-app visibility requested for the app browse action.
    bool streamLaunchRequested = false;  ///< True when the selected host app should start or resume streaming.
    bool logViewRequested = false;  ///< True when the log viewer should be refreshed from disk.
    bool directoryListRequested = false;  ///< Read the picker's current directory or mounted drives.
    bool directoryCreateRequested = false;  ///< Create the named child of the current picker directory.
    bool directorySelectRequested = false;  ///< Check destination writability before accepting the picker directory.
    bool screenshotsRequested = false;  ///< Load captures for the viewer's retained server and app identity.
    bool screenshotDeleteRequested = false;  ///< Delete the selected capture after confirmation.
    bool quitAppRequested = false;  ///< Start the retained host-app quit request in a background task.
  };

  /**
   * @brief Persistence and cleanup side effects emitted by one command update.
   */
  struct AppPersistenceUpdate {
    bool hostsChanged = false;  ///< True when the host list changed and should be persisted.
    bool settingsChanged = false;  ///< True when persisted TOML-backed settings changed.
    bool savedFileDeleteRequested = false;  ///< True when one managed file should be deleted.
    std::string savedFileDeletePath;  ///< Managed file path requested for deletion.
    bool factoryResetRequested = false;  ///< True when a full saved-data reset should run.
    bool hostDeleteCleanupRequested = false;  ///< True when host deletion follow-up cleanup should run.
    bool deletedHostWasPaired = false;  ///< True when the deleted host previously had pairing credentials.
    std::string deletedHostAddress;  ///< Address of the host removed from storage.
    uint16_t deletedHostPort = 0;  ///< Port of the host removed from storage.
    std::vector<std::string> deletedHostCoverArtCacheKeys;  ///< Cover-art cache keys to remove for the deleted host.
  };

  /**
   * @brief Result of updating the client shell with a UI command.
   */
  struct AppUpdate {
    AppNavigationUpdate navigation;  ///< Navigation and modal changes emitted by the command.
    AppRequestUpdate requests;  ///< Network and browsing requests emitted by the command.
    AppPersistenceUpdate persistence;  ///< Persistence and cleanup work emitted by the command.
  };

  /**
   * @brief Create the initial app state shown after startup.
   *
   * @return The initial client state.
   */
  ClientState create_initial_state();

  /**
   * @brief Queue an intentional notification without duplicating an outstanding event.
   *
   * @param state Mutable client state.
   * @param kind Severity used to select the notification icon.
   * @param title Concise event description.
   * @param message Consequence or recovery guidance; empty messages are ignored.
   * @param autoDismissMilliseconds Visible lifetime, or zero to require explicit dismissal.
   */
  void show_notification(ClientState &state, NotificationKind kind, std::string title, std::string message, std::uint32_t autoDismissMilliseconds = 0U);

  /**
   * @brief Dismiss the newest outstanding notification targeted by the controller hint.
   *
   * @param state Mutable client state.
   */
  void dismiss_notification(ClientState &state);

  /**
   * @brief Expire automatic notifications, pausing while a modal, keypad or overlay covers the stack.
   *
   * @param state Mutable client state.
   * @param elapsedMilliseconds Shell time elapsed since the preceding update.
   */
  void advance_notifications(ClientState &state, std::uint32_t elapsedMilliseconds);

  /**
   * @brief Return a display label for a screen identifier.
   *
   * @param screen Screen identifier to stringify.
   * @return Stable lowercase screen name.
   */
  const char *to_string(ScreenId screen);

  /**
   * @brief Replace the in-memory host list from a persisted snapshot.
   *
   * @param state Mutable app state.
   * @param hosts Loaded host records.
   * @param statusMessage Optional status line shown in the shell.
   */
  void replace_hosts(ClientState &state, std::vector<HostRecord> hosts, std::string statusMessage = {});

  /**
   * @brief Add or refresh one auto-discovered host in the current host list.
   *
   * Discovery results are normalized to the same saved-host conventions used by
   * manual host entry. When a matching host already exists, transient runtime
   * fields such as reachability are refreshed without overwriting a custom saved
   * name. When no host matches, a new host record is appended and marked dirty so
   * it can be persisted.
   *
   * @param state Mutable app state.
   * @param displayName Discovered host name, or an empty string to use the default label.
   * @param address Discovered IPv4 address.
   * @param port Discovered host HTTP port.
   * @return true when persisted host metadata changed or a new host was added.
   */
  bool merge_discovered_host(ClientState &state, std::string displayName, const std::string &address, uint16_t port);

  /**
   * @brief Replace the in-memory saved-file inventory shown on the settings page.
   *
   * @param state Mutable app state.
   * @param savedFiles Saved files currently found on disk.
   */
  void replace_saved_files(ClientState &state, std::vector<startup::SavedFileEntry> savedFiles);

  /**
   * @brief Return the current host address shown in the add-host flow.
   *
   * @param state App state containing the add-host draft.
   * @return Current draft IPv4 address text.
   */
  std::string current_add_host_address(const ClientState &state);

  /**
   * @brief Return the effective TCP port for the current add-host draft.
   *
   * @param state App state containing the add-host draft.
   * @return Effective host port using the default when the field is empty.
   */
  uint16_t current_add_host_port(const ClientState &state);

  /**
   * @brief Return the current pairing PIN shown in the pairing flow.
   *
   * @param state App state containing the pairing draft.
   * @return Four-digit PIN string.
   */
  std::string current_pairing_pin(const ClientState &state);

  /**
   * @brief Complete the connection-test dialog and update reachability for the tested endpoint.
   *
   * @param state Mutable app state.
   * @param success Whether the test succeeded.
   * @param message User-visible status message.
   */
  void apply_connection_test_result(ClientState &state, bool success, std::string message);

  /**
   * @brief Apply a pairing result and confirm a successful pairing with a brief notification.
   *
   * @param state Mutable app state.
   * @param address Host address used for pairing.
   * @param port Host port used for pairing.
   * @param success Whether the pairing attempt succeeded.
   * @param message User-visible status message.
   * @return True when the host list changed and should be persisted.
   */
  bool apply_pairing_result(ClientState &state, const std::string &address, uint16_t port, bool success, std::string message);

  /**
   * @brief Apply a fetched app list to a saved host.
   *
   * @param state Mutable app state.
   * @param address Host address used for the fetch.
   * @param port Host port used for the fetch.
   * @param apps Fresh app records returned by the host.
   * @param appListContentHash Stable content hash for the returned app list.
   * @param success Whether the fetch succeeded.
   * @param message User-visible status message.
   */
  void apply_app_list_result(
    ClientState &state,
    const std::string &address,
    uint16_t port,
    std::vector<HostAppRecord> apps,
    uint64_t appListContentHash,
    bool success,
    std::string message
  );

  /**
   * @brief Mark one cached cover-art entry as available for a host app.
   *
   * @param state Mutable app state.
   * @param address Host address owning the app.
   * @param port Host port owning the app.
   * @param appId App identifier whose cached art is now available.
   */
  void mark_cover_art_cached(ClientState &state, const std::string &address, uint16_t port, int appId);

  /**
   * @brief Update the log file path tracked by the shell.
   *
   * @param state Mutable app state.
   * @param logFilePath Path to the log file that should be shown in the viewer.
   */
  void set_log_file_path(ClientState &state, std::string logFilePath);

  /**
   * @brief Replace the loaded log viewer contents.
   *
   * @param state Mutable app state.
   * @param lines Log file lines ready for display.
   * @param statusMessage User-visible status line for the log viewer state.
   */
  void apply_log_viewer_contents(ClientState &state, std::vector<std::string> lines, std::string statusMessage);

  /**
   * @brief Return whether a saved host still requires a manual pairing flow.
   *
   * @param state App state containing the saved host list.
   * @param address Host address to inspect.
   * @param port Host port to inspect.
   * @return true when the matching host exists and is not paired.
   */
  bool host_requires_manual_pairing(const ClientState &state, const std::string &address, uint16_t port);

  /**
   * @brief Enter the apps screen for the currently selected host after authorization has been refreshed.
   *
   * @param state Mutable app state.
   * @param showHiddenApps Whether hidden apps should still be shown.
   * @return true when the apps page was entered.
   */
  bool begin_selected_host_app_browse(ClientState &state, bool showHiddenApps);

  /**
   * @brief Return the currently selected loaded host for the active screen.
   *
   * On the hosts page this returns the selected saved host tile. On host-specific
   * pages such as pairing it may return the lightweight active host snapshot.
   *
   * @param state App state containing the loaded host selection.
   * @return Selected host record, or nullptr when no saved host is selected.
   */
  const HostRecord *selected_host(const ClientState &state);

  /**
   * @brief Return the currently selected app on the Apps screen.
   *
   * @param state App state containing the selected host and apps list.
   * @return Selected app record, or nullptr when no visible app is selected.
   */
  const HostAppRecord *selected_app(const ClientState &state);

  /**
   * @brief Return the host currently shown by the Apps screen.
   *
   * @param state App state containing the selected host.
   * @return Host record backing the apps page, or nullptr when unavailable.
   */
  const HostRecord *apps_host(const ClientState &state);

  /**
   * @brief Apply a UI command to the client shell.
   *
   * @param state Mutable app state.
   * @param command UI command from controller or keyboard input.
   * @return Summary of the resulting state transition.
   */
  AppUpdate handle_command(ClientState &state, input::UiCommand command);

  /**
   * @brief Apply a completed quit operation and clear only the confirmed app's in-progress state.
   * @param state Client state containing the retained quit target.
   * @param succeeded True after the host confirmed that no app is running.
   * @param message Completion or actionable failure detail.
   */
  void apply_app_quit_result(ClientState &state, bool succeeded, std::string message);
  /**
   * @brief Remove a deleted capture from the open viewer and keep its selection in range.
   * @param state Viewer that requested deletion.
   * @param succeeded True when the bitmap and index deletion both succeeded.
   * @param error Failure detail to keep visible without hiding the selected image.
   */
  void apply_screenshot_delete_result(ClientState &state, bool succeeded, std::string error);

  /**
   * @brief Insert printable text into the local folder-name draft, without forwarding it to a host.
   * @param state Current client state.
   * @param text ASCII text to append, limited to the Xbox component length.
   */
  void append_directory_name(ClientState &state, std::string_view text);

  /**
   * @brief Complete a validated directory selection and rebuild its settings label.
   * @param state Current client state.
   * @param update Receives persistence and modal closure flags.
   * @param path Selected folder; empty restores the title-scoped default.
   */
  void accept_screenshot_directory(ClientState &state, AppUpdate &update, std::string_view path);

}  // namespace app
