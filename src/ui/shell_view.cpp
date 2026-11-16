/**
 * @file src/ui/shell_view.cpp
 * @brief Implements the shell view renderer and layout helpers.
 */
// class header include
#include "src/ui/shell_view.h"

// standard includes
#include "src/platform/filesystem_utils.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace {

  std::string format_file_size(std::uint64_t sizeBytes) {
    if (sizeBytes < 1024ULL) {
      return std::to_string(sizeBytes) + " B";
    }
    if (sizeBytes < (1024ULL * 1024ULL)) {
      return std::to_string((sizeBytes + 1023ULL) / 1024ULL) + " KB";
    }
    return std::to_string((sizeBytes + (1024ULL * 1024ULL) - 1ULL) / (1024ULL * 1024ULL)) + " MB";
  }

  /**
   * @brief Name the primary collection or task displayed below the brand strip.
   *
   * @param state Current application state.
   * @return User-facing page heading.
   */
  std::string page_title(const app::ClientState &state) {
    switch (state.shell.activeScreen) {
      case app::ScreenId::home:
      case app::ScreenId::hosts:
        return "Your PCs";
      case app::ScreenId::apps:
        if (const app::HostRecord *host = app::apps_host(state); host != nullptr) {
          return host->displayName;
        }
        return "Apps";
      case app::ScreenId::add_host:
        return "Add Host";
      case app::ScreenId::pair_host:
        return "Pair Host";
      case app::ScreenId::settings:
        return "Settings";
    }

    return "Hosts";
  }

  /**
   * @brief Summarize the current page without repeating controller hints.
   *
   * @param state Current shell state.
   * @return Short supporting copy displayed beneath the page title.
   */
  std::string page_subtitle(const app::ClientState &state) {
    switch (state.shell.activeScreen) {
      case app::ScreenId::home:
      case app::ScreenId::hosts:
        return state.hosts.items.empty() ? "Your games. Your PC. On your Xbox." : "Choose a PC to start playing.";
      case app::ScreenId::apps:
        {
          const app::HostRecord *host = app::apps_host(state);
          if (host == nullptr || host->pairingState != app::PairingState::paired) {
            return "Pair your PC to browse its library.";
          }
          if (host->appListState == app::HostAppListState::loading) {
            return "Refreshing your library...";
          }
          const auto count = std::count_if(host->apps.begin(), host->apps.end(), [&state](const app::HostAppRecord &record) {
            return state.apps.showHiddenApps || !record.hidden;
          });
          return std::to_string(count) + (count == 1 ? " app" : " apps") + "  /  " + (state.apps.showHiddenApps ? "Including hidden apps" : "Ready to stream");
        }
      case app::ScreenId::add_host:
        return "Connect to a PC on your network.";
      case app::ScreenId::pair_host:
        return "A secure connection for your next session.";
      case app::ScreenId::settings:
        return "Make yourself at home.";
    }
    return {};
  }

  std::string host_tile_status(const app::HostRecord &host) {
    if (host.reachability == app::HostReachability::unknown) {
      return "Checking...";
    }
    if (host.reachability != app::HostReachability::online) {
      return "Offline";
    }
    if (host.pairingState != app::PairingState::paired) {
      return "Needs Pairing";
    }
    return "Online";
  }

  /**
   * @brief Select a grayscale moon phase for a PC's connection state.
   *
   * @param host Host whose reachability and pairing are displayed.
   * @return Packaged first-party moon SVG path.
   */
  std::string host_tile_icon(const app::HostRecord &host) {
    if (host.reachability == app::HostReachability::online && host.pairingState == app::PairingState::paired) {
      return "hosts\\moon-full.svg";
    }
    if (host.reachability == app::HostReachability::online) {
      return "hosts\\moon-half.svg";
    }
    return "hosts\\moon-crescent.svg";
  }

  /**
   * @brief Build compact navigation actions with packaged Lucide icons.
   *
   * @param state Current toolbar focus state.
   * @return Settings, support and manual connection buttons.
   */
  std::vector<ui::ShellToolbarButton> toolbar_buttons(const app::ClientState &state) {
    return {
      {"settings", "Settings", "G", "icons\\settings.svg", state.shell.activeScreen == app::ScreenId::hosts && state.hosts.focusArea == app::HostsFocusArea::toolbar && state.hosts.selectedToolbarButtonIndex % 3U == 0U},
      {"support", "Support", "?", "icons\\circle-question-mark.svg", state.shell.activeScreen == app::ScreenId::hosts && state.hosts.focusArea == app::HostsFocusArea::toolbar && state.hosts.selectedToolbarButtonIndex % 3U == 1U},
      {"add-host", "Add Host", "+", "icons\\circle-plus.svg", state.shell.activeScreen == app::ScreenId::hosts && state.hosts.focusArea == app::HostsFocusArea::toolbar && state.hosts.selectedToolbarButtonIndex % 3U == 2U},
    };
  }

  std::vector<ui::ShellHostTile> host_tiles(const app::ClientState &state) {
    std::vector<ui::ShellHostTile> tiles;
    tiles.reserve(state.hosts.items.size());
    for (std::size_t index = 0; index < state.hosts.items.size(); ++index) {
      const app::HostRecord &host = state.hosts.items[index];
      tiles.emplace_back(ui::ShellHostTile {
        host.address,
        host.displayName,
        host_tile_status(host),
        host_tile_icon(host),
        host.pairingState,
        host.reachability,
        state.shell.activeScreen == app::ScreenId::hosts && state.hosts.focusArea == app::HostsFocusArea::grid && index == state.hosts.selectedHostIndex,
      });
    }
    return tiles;
  }

  std::vector<ui::ShellAppTile> app_tiles(const app::ClientState &state) {
    std::vector<ui::ShellAppTile> tiles;
    const app::HostRecord *host = app::apps_host(state);
    if (host == nullptr || host->pairingState != app::PairingState::paired) {
      return tiles;
    }

    for (std::size_t index = 0, visibleIndex = 0; index < host->apps.size(); ++index) {
      const app::HostAppRecord &appRecord = host->apps[index];
      if (!state.apps.showHiddenApps && appRecord.hidden) {
        continue;
      }

      const std::string detail = appRecord.running ? "Running now" : std::string {};

      std::string badgeLabel;
      if (appRecord.favorite) {
        badgeLabel = "Favorite";
      } else if (appRecord.hdrSupported) {
        badgeLabel = "HDR";
      } else if (appRecord.hidden) {
        badgeLabel = "Hidden";
      }

      tiles.emplace_back(ui::ShellAppTile {
        std::to_string(appRecord.id),
        appRecord.name,
        detail,
        badgeLabel,
        appRecord.boxArtCacheKey,
        appRecord.hidden,
        appRecord.favorite,
        appRecord.boxArtCached,
        appRecord.running,
        state.shell.activeScreen == app::ScreenId::apps && visibleIndex == state.apps.selectedAppIndex,
      });
      ++visibleIndex;
    }

    return tiles;
  }

  std::string keypad_value(const app::ClientState &state) {
    if (state.addHostDraft.keypad.visible) {
      return state.addHostDraft.keypad.stagedInput.empty() && state.addHostDraft.activeField == app::AddHostField::port ? "default (47989)" : state.addHostDraft.keypad.stagedInput;
    }

    if (state.addHostDraft.activeField == app::AddHostField::address) {
      return state.addHostDraft.addressInput;
    }
    if (state.addHostDraft.portInput.empty()) {
      return "default (47989)";
    }
    return state.addHostDraft.portInput;
  }

  const char *settings_category_label(app::SettingsCategory category) {
    switch (category) {
      case app::SettingsCategory::logging:
        return "Logging";
      case app::SettingsCategory::display:
        return "Display";
      case app::SettingsCategory::input:
        return "Input";
      case app::SettingsCategory::reset:
        return "Reset";
    }

    return "Logging";
  }

  /**
   * @brief Build a numeric keypad shared by IPv4 sections and optional port entry.
   *
   * @param state Current keypad selection.
   * @return Digit keys with their focus state.
   */
  std::vector<ui::ShellModalButton> keypad_buttons(const app::ClientState &state) {
    const std::vector<std::string> labels {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};

    std::vector<ui::ShellModalButton> buttons;
    buttons.reserve(labels.size());
    for (std::size_t index = 0; index < labels.size(); ++index) {
      buttons.emplace_back(ui::ShellModalButton {
        labels[index],
        true,
        state.addHostDraft.keypad.visible && index == state.addHostDraft.keypad.selectedButtonIndex,
      });
    }

    return buttons;
  }

  /**
   * @brief Explain the active keypad field without repeating button hints or the input value.
   *
   * @param state Current add-host field.
   * @return Concise guidance displayed above the keypad input.
   */
  std::vector<std::string> keypad_modal_lines(const app::ClientState &state) {
    if (!state.addHostDraft.validationMessage.empty()) {
      return {state.addHostDraft.validationMessage};
    }
    return {state.addHostDraft.activeField == app::AddHostField::address ? "Enter your PC's IPv4 address. Three digits advance automatically." : "Enter a custom port, or leave empty for 47989."};
  }

  /**
   * @brief Provide first-run connection guidance for an empty PC collection.
   *
   * @param state Current application state.
   * @return Connection guidance or a short collection description.
   */
  std::vector<std::string> hosts_body_lines(const app::ClientState &state) {
    if (state.hosts.items.empty()) {
      return {
        "No PCs have been added yet.",
        "Moonlight-compatible PCs discovered over the local network will appear here automatically.",
        "Use Add Host to save a host manually when auto discovery is unavailable.",
      };
    }

    return {
      "Select a PC to pair or browse its apps.",
    };
  }

  /**
   * @brief Explain library loading, empty, unpaired and failed states inline.
   *
   * @param state Current application state.
   * @return Supporting text when no library tiles can be displayed.
   */
  std::vector<std::string> apps_body_lines(const app::ClientState &state) {
    const app::HostRecord *host = app::apps_host(state);
    if (host == nullptr) {
      return {"No host is selected."};
    }
    if (host->pairingState != app::PairingState::paired) {
      return {"This host is not paired yet. Return and select it to begin pairing."};
    }
    if (host->appListState == app::HostAppListState::loading) {
      return {"Loading your apps...", "Your library will appear here shortly."};
    }
    if (host->appListState == app::HostAppListState::failed) {
      return {
        host->appListStatusMessage.empty() ? "The app list could not be refreshed." : host->appListStatusMessage,
      };
    }
    if (host->apps.empty()) {
      return {"Host did not return any launchable apps for this host."};
    }

    return {};
  }

  /**
   * @brief Describe manual connection fields and their current validation state.
   *
   * @param state Current application state.
   * @return Field values and connection guidance for manual PC entry.
   */
  std::vector<std::string> add_host_body_lines(const app::ClientState &state) {
    std::vector<std::string> lines = {
      "Enter your PC's address to get connected.",
      std::string("Address: ") + app::current_add_host_address(state),
      std::string("Port: ") + (state.addHostDraft.portInput.empty() ? std::string("default (47989)") : state.addHostDraft.portInput),
      "Press A to edit either field with the keypad modal.",
    };

    if (!state.addHostDraft.validationMessage.empty()) {
      lines.emplace_back("Validation: " + state.addHostDraft.validationMessage);
    }
    if (!state.addHostDraft.connectionMessage.empty()) {
      lines.emplace_back("Connection: " + state.addHostDraft.connectionMessage);
    }
    return lines;
  }

  std::vector<std::string> pair_host_body_lines(const app::ClientState &state) {
    std::vector<std::string> lines = {
      std::string("Target host: ") + state.pairingDraft.targetAddress,
    };
    if (state.pairingDraft.stage == app::PairingStage::idle) {
      lines.emplace_back("Checking whether the host is reachable before showing a PIN.");
    } else {
      lines.emplace_back(std::string("Target port: ") + std::to_string(state.pairingDraft.targetPort));
      if (!state.pairingDraft.generatedPin.empty()) {
        lines.emplace_back(std::string("PIN: ") + app::current_pairing_pin(state));
        lines.emplace_back("Enter the PIN on the host.");
      }
    }
    if (!state.pairingDraft.statusMessage.empty()) {
      lines.emplace_back("Status: " + state.pairingDraft.statusMessage);
    }
    return lines;
  }

  std::vector<std::string> settings_body_lines(const app::ClientState &state) {
    std::vector<std::string> lines = {
      std::string("Category: ") + settings_category_label(state.settings.selectedCategory),
    };
    if (state.settings.selectedCategory == app::SettingsCategory::logging) {
      lines.emplace_back("Runtime log file: reset on every startup");
      lines.push_back(std::string("Log file path: ") + (state.settings.logFilePath.empty() ? "not configured" : state.settings.logFilePath));
      lines.push_back(std::string("File logging level: ") + logging::to_string(state.settings.loggingLevel));
      lines.push_back(std::string("xemu console logging level: ") + logging::to_string(state.settings.xemuConsoleLoggingLevel));
      lines.emplace_back("File logging writes to disk and should usually stay at NONE unless you are debugging an issue.");
      lines.emplace_back("xemu console logging writes through DbgPrint(). Start xemu with -device lpc47m157 -serial stdio.");
      lines.emplace_back("Startup console messages are always shown before the splash screen.");
      return lines;
    }
    if (state.settings.selectedCategory == app::SettingsCategory::reset) {
      if (state.settings.savedFiles.empty()) {
        lines.emplace_back("Saved files: none found.");
        return lines;
      }
      lines.emplace_back("Saved files on disk:");
      for (const startup::SavedFileEntry &savedFile : state.settings.savedFiles) {
        lines.push_back("- " + savedFile.displayName + " (" + format_file_size(savedFile.sizeBytes) + ")");
      }
      return lines;
    }
    if (state.settings.selectedCategory == app::SettingsCategory::display) {
      lines.emplace_back("Display options will be added here.");
      return lines;
    }

    lines.emplace_back("Input options will be added here.");
    return lines;
  }

  std::vector<std::string> body_lines(const app::ClientState &state) {
    switch (state.shell.activeScreen) {
      case app::ScreenId::home:
      case app::ScreenId::hosts:
        return hosts_body_lines(state);
      case app::ScreenId::apps:
        return apps_body_lines(state);
      case app::ScreenId::add_host:
        return add_host_body_lines(state);
      case app::ScreenId::pair_host:
        return pair_host_body_lines(state);
      case app::ScreenId::settings:
        return settings_body_lines(state);
    }

    return {};
  }

  /**
   * @brief Resolve a navigation icon from a stable settings or host action identifier.
   *
   * @param id Navigation action identifier.
   * @return Packaged Lucide SVG path, or an empty path for undecorated actions.
   */
  std::string menu_icon(const std::string &id) {
    if (id == "settings-category:logging") {
      return "icons\\scroll-text.svg";
    }
    if (id == "settings-category:display") {
      return "icons\\monitor.svg";
    }
    if (id == "settings-category:input") {
      return "icons\\gamepad-2.svg";
    }
    if (id == "settings-category:reset") {
      return "icons\\rotate-ccw.svg";
    }
    if (id == "edit-address") {
      return "icons\\network.svg";
    }
    if (id == "edit-port") {
      return "icons\\hash.svg";
    }
    if (id == "test-connection") {
      return "icons\\activity.svg";
    }
    if (id == "start-pairing") {
      return "icons\\key-round.svg";
    }
    if (id == "save-host") {
      return "icons\\save.svg";
    }
    return {};
  }

  /**
   * @brief Build navigation rows with contextual Lucide icons.
   *
   * @param state Current menu and selection state.
   * @return Render-ready navigation actions.
   */
  std::vector<ui::ShellActionRow> menu_rows(const app::ClientState &state) {
    std::vector<ui::ShellActionRow> rows;
    const ui::MenuItem *selectedItem = state.menu.selected_item();
    for (const ui::MenuItem &item : state.menu.items()) {
      rows.emplace_back(ui::ShellActionRow {
        item.id,
        item.label,
        item.enabled,
        selectedItem != nullptr && selectedItem->id == item.id,
        false,
        menu_icon(item.id),
      });
    }
    return rows;
  }

  std::vector<ui::ShellActionRow> detail_menu_rows(const app::ClientState &state) {
    std::vector<ui::ShellActionRow> rows;
    const ui::MenuItem *selectedItem = state.detailMenu.selected_item();
    for (const ui::MenuItem &item : state.detailMenu.items()) {
      rows.emplace_back(ui::ShellActionRow {
        item.id,
        item.label,
        item.enabled,
        state.settings.focusArea == app::SettingsFocusArea::options && selectedItem != nullptr && selectedItem->id == item.id,
        false,
      });
    }
    return rows;
  }

  /**
   * @brief Present only explicit notification events, with dismissal focused on the newest card.
   *
   * @param state Outstanding user notifications.
   * @return Cards in arrival order with severity icons and one controller dismissal target.
   */
  std::vector<ui::ShellNotification> notification_views(const app::ClientState &state) {
    std::vector<ui::ShellNotification> notifications;
    for (const auto &notification : state.shell.notifications) {
      const std::string icon = notification.kind == app::NotificationKind::success ? "icons\\circle-check.svg" : (notification.kind == app::NotificationKind::error ? "icons\\circle-x.svg" : "icons\\info.svg");
      notifications.push_back({notification.title, notification.message, {}, icon});
    }
    if (!notifications.empty()) {
      notifications.back().actions = {{"dismiss-notification", notifications.size() > 1U ? "Dismiss latest" : "Dismiss", "icons\\button-x.svg", {}, false}};
    }
    return notifications;
  }

  void fill_support_modal_view(ui::ShellViewModel *viewModel) {
    viewModel->modal.title = "Support";
    viewModel->modal.lines = {
      "Moonlight for the original Xbox",
      "Run Sunshine on your PC, then select it here to pair and browse your games.",
      "Press Y on a PC or app for more options.",
    };
    viewModel->modal.footerActions = {
      {"close", "Close", "icons\\button-a.svg", "icons\\button-start.svg", true},
      {"back", "Back", "icons\\button-b.svg", {}, false},
    };
  }

  void fill_host_actions_modal_view(const app::ClientState &state, ui::ShellViewModel *viewModel) {
    viewModel->modal.title = "Host Actions";
    viewModel->modal.actions = {
      {"view-all-apps", "View all apps", true, state.modal.selectedActionIndex == 0U, false},
      {"test-connection", "Test Network Connection", true, state.modal.selectedActionIndex == 1U, false},
      {"delete-host", "Delete PC", true, state.modal.selectedActionIndex == 2U, false},
      {"view-host-details", "View Details", true, state.modal.selectedActionIndex == 3U, false},
    };
  }

  void fill_host_details_modal_view(const app::ClientState &state, ui::ShellViewModel *viewModel) {
    viewModel->modal.title = "Host Details";
    const app::HostRecord *host = app::selected_host(state);
    if (host == nullptr) {
      return;
    }

    const char *reachabilityLabel = "UNKNOWN";
    if (host->reachability == app::HostReachability::online) {
      reachabilityLabel = "ONLINE";
    } else if (host->reachability == app::HostReachability::offline) {
      reachabilityLabel = "OFFLINE";
    }

    viewModel->modal.lines = {
      "Name: " + host->displayName,
      std::string("State: ") + reachabilityLabel,
      std::string("Active Address: ") + (host->activeAddress.empty() ? "NULL" : host->activeAddress),
      std::string("UUID: ") + (host->uuid.empty() ? "NULL" : host->uuid),
      std::string("Local Address: ") + (host->localAddress.empty() ? "NULL" : host->localAddress),
      std::string("Remote Address: ") + (host->remoteAddress.empty() ? "NULL" : host->remoteAddress),
      std::string("IPv6 Address: ") + (host->ipv6Address.empty() ? "NULL" : host->ipv6Address),
      std::string("Manual Address: ") + (host->manualAddress.empty() ? host->address : host->manualAddress),
      std::string("MAC Address: ") + (host->macAddress.empty() ? "NULL" : host->macAddress),
      std::string("Pair State: ") + (host->pairingState == app::PairingState::paired ? "PAIRED" : "NOT_PAIRED"),
      "Running Game ID: " + std::to_string(host->runningGameId),
      "HTTPS Port: " + std::to_string(app::effective_host_port(host->httpsPort)),
    };
  }

  /**
   * @brief Build app actions with explicit hidden and favorite checkbox states.
   *
   * @param state Current app and modal selection.
   * @param viewModel Receives the modal action rows.
   */
  void fill_app_actions_modal_view(const app::ClientState &state, ui::ShellViewModel *viewModel) {
    if (const app::HostAppRecord *appRecord = app::selected_app(state); appRecord != nullptr) {
      viewModel->modal.title = appRecord->name;
      viewModel->modal.actions = {
        {"toggle-hidden-app", "Hide app", true, state.modal.selectedActionIndex == 0U, appRecord->hidden, {}, true},
        {"view-app-details", "View details", true, state.modal.selectedActionIndex == 1U, false},
        {"toggle-favorite", "Set favorite", true, state.modal.selectedActionIndex == 2U, appRecord->favorite, {}, true},
      };
    }
  }

  void fill_app_details_modal_view(const app::ClientState &state, ui::ShellViewModel *viewModel) {
    if (const app::HostAppRecord *appRecord = app::selected_app(state); appRecord != nullptr) {
      viewModel->modal.title = "App Details";
      viewModel->modal.lines = {
        "Name: " + appRecord->name,
        std::string("HDR Supported: ") + (appRecord->hdrSupported ? "YES" : "NO"),
        "ID: " + std::to_string(appRecord->id),
      };
    }
  }

  void fill_log_viewer_modal_view(const app::ClientState &state, ui::ShellViewModel *viewModel) {
    viewModel->modal.title = "Log File";
    viewModel->logViewer.visible = true;
    viewModel->logViewer.path = state.settings.logFilePath.empty() ? "not configured" : state.settings.logFilePath;
    viewModel->logViewer.lines = state.settings.logViewerLines;
    viewModel->logViewer.scrollOffset = state.settings.logViewerScrollOffset;
    viewModel->logViewer.placement = state.settings.logViewerPlacement;
    viewModel->modal.footerActions = {
      {"older", "Older", "icons\\button-lb.svg", {}, false},
      {"newer", "Newer", "icons\\button-rb.svg", {}, false},
      {"fast-older", "Fast Older", "icons\\button-lt.svg", {}, false},
      {"fast-newer", "Fast Newer", "icons\\button-rt.svg", {}, false},
      {"move-pane", "Move Pane", "icons\\button-x.svg", {}, false},
      {"close", "Close", "icons\\button-b.svg", {}, true},
    };
  }

  void fill_confirmation_modal_view(const app::ClientState &state, ui::ShellViewModel *viewModel) {
    viewModel->modal.title = state.confirmation.title;
    viewModel->modal.lines = state.confirmation.lines;
    viewModel->modal.footerActions = {
      {"confirm", "OK", "icons\\button-a.svg", {}, state.modal.selectedActionIndex == 0U},
      {"cancel", "Cancel", "icons\\button-b.svg", {}, state.modal.selectedActionIndex != 0U},
    };
  }

  /**
   * @brief Present staged folder selection or the same keyboard geometry used by stream controls.
   * @param state Current directory picker and input source.
   * @param viewModel Receives a scrollable folder dialog or local name-entry keyboard.
   */
  void fill_directory_picker_view(const app::ClientState &state, ui::ShellViewModel *viewModel) {
    const auto &picker = state.directoryPicker;
    if (picker.creating) {
      viewModel->modal.visible = false;
      auto &view = viewModel->keypad;
      view.visible = true;
      view.title = "New Folder";
      view.value = picker.name;
      view.value.insert(std::min(picker.caret, view.value.size()), "|");
      if (!picker.error.empty()) {
        view.lines = {picker.error};
      }
      view.columnCount = input::StreamMenu::KEYBOARD_COLUMNS;
      view.rowCount = input::StreamMenu::KEYBOARD_ROWS;
      const auto &cells = picker.keyboard.keyboard_cells();
      for (std::size_t i = 0; i < cells.size(); ++i) {
        const auto &cell = cells[i];
        std::string label = picker.keyboard.key_label(static_cast<int>(i));
        if (cell.key == input::StreamMenu::KEYBOARD_PAGE_KEY && picker.keyboard.keyboardLayout == 1) {
          label = "ABC";
        } else if (cell.key == 46) {
          label = "OK";
        }
        view.buttons.push_back({label, true, i == static_cast<std::size_t>(picker.keyboard.keyIndex), cell.column, cell.row, cell.width, cell.height});
      }
      if (state.shell.inputDevice == input::UiInputDevice::keyboard) {
        view.footerActions = {{"create", "Create", "icons\\button-start.svg", {}, false}, {"cancel", "Cancel", "icons\\button-b.svg", {}, false}};
      } else {
        view.footerActions = {{"type", "Type", "icons\\button-a.svg", {}, true}, {"delete", "Delete", "icons\\button-x.svg", {}, false}, {"shift", picker.keyboard.shiftOnce ? "Shift: On" : "Shift", "icons\\button-y.svg", {}, false}, {"create", "Create", "icons\\button-start.svg", {}, false}, {"cancel", "Cancel", "icons\\button-b.svg", {}, false}};
      }
      return;
    }
    auto &view = viewModel->modal;
    view.title = "Screenshot Folder";
    const auto path = picker.path.empty() ? std::string("Choose a drive") : picker.path;
    view.lines = {path.size() > 72 ? "..." + path.substr(path.size() - 69) : path};
    if (!picker.error.empty()) {
      view.lines.push_back(picker.error);
    } else if (!picker.path.empty() && picker.folders.empty()) {
      view.lines.push_back("No subfolders here.");
    }
    const bool canUse = picker.available && !picker.path.empty();
    view.actions = {
      {"use-folder", "Use this folder", canUse, state.modal.selectedActionIndex == 0},
      {"new-folder", "New folder...", canUse, state.modal.selectedActionIndex == 1},
      {"parent-folder", "Up / Drives", !picker.path.empty(), state.modal.selectedActionIndex == 2},
      {"default-folder", "Use default location", true, state.modal.selectedActionIndex == 3},
    };
    for (std::size_t i = 0; i < picker.folders.size(); ++i) {
      const auto &folder = picker.folders[i];
      view.actions.push_back({folder, picker.path.empty() ? folder : platform::file_name_from_path(folder), true, state.modal.selectedActionIndex == i + 4});
    }
    view.footerActions = {{"select", "Select", "icons\\button-a.svg", {}, true}, {"use-folder", "Use folder", "icons\\button-y.svg", {}, false}, {"up", "Up", "icons\\button-lb.svg", {}, false}, {"cancel", "Cancel", "icons\\button-b.svg", {}, false}};
  }

  /**
   * @brief Build the active dialog, including connection-test status icons and diagnostics.
   *
   * @param state Current reducer-owned modal state.
   * @param viewModel Receives render-ready dialog content.
   */

  void fill_modal_view(const app::ClientState &state, ui::ShellViewModel *viewModel) {
    if (!state.modal.active()) {
      return;
    }

    viewModel->modal.visible = true;
    switch (state.modal.id) {
      case app::ModalId::support:
        fill_support_modal_view(viewModel);
        return;
      case app::ModalId::host_actions:
        fill_host_actions_modal_view(state, viewModel);
        return;
      case app::ModalId::host_details:
        fill_host_details_modal_view(state, viewModel);
        return;
      case app::ModalId::app_actions:
        fill_app_actions_modal_view(state, viewModel);
        return;
      case app::ModalId::app_details:
        fill_app_details_modal_view(state, viewModel);
        return;
      case app::ModalId::log_viewer:
        fill_log_viewer_modal_view(state, viewModel);
        return;
      case app::ModalId::confirmation:
        fill_confirmation_modal_view(state, viewModel);
        return;
      case app::ModalId::directory_picker:
        fill_directory_picker_view(state, viewModel);
        return;
      case app::ModalId::connection_test:
        viewModel->modal.busy = state.connectionTest.inProgress;
        viewModel->modal.title = state.connectionTest.inProgress ? "Testing connection" : (state.connectionTest.succeeded ? "Connection successful" : "Connection failed");
        viewModel->modal.iconAssetPath = state.connectionTest.inProgress ? "icons\\loader-circle.svg" : (state.connectionTest.succeeded ? "icons\\circle-check.svg" : "icons\\circle-x.svg");
        viewModel->modal.lines = {state.connectionTest.address + ":" + std::to_string(state.connectionTest.port), state.connectionTest.message};
        return;
      case app::ModalId::none:
        return;
    }
  }

  /**
   * @brief Build controller hints with section navigation immediately before keypad acceptance.
   *
   * @param state Current screen, dialog and input field.
   * @return Actions displayed within the page or active input dialog.
   */
  std::vector<ui::ShellFooterAction> footer_actions(const app::ClientState &state) {
    switch (state.shell.activeScreen) {
      case app::ScreenId::home:
      case app::ScreenId::hosts:
        {
          std::string openLabel = "Pair";
          if (state.hosts.focusArea == app::HostsFocusArea::toolbar) {
            openLabel = "Select";
          } else if (const app::HostRecord *selectedHost = app::selected_host(state); selectedHost != nullptr && selectedHost->pairingState == app::PairingState::paired) {
            openLabel = "Open";
          }
          std::vector<ui::ShellFooterAction> actions = {
            {"open", std::move(openLabel), "icons\\button-a.svg", {}, true},
          };
          if (state.hosts.focusArea == app::HostsFocusArea::grid && app::selected_host(state) != nullptr) {
            actions.emplace_back(ui::ShellFooterAction {"host-menu", "Host Menu", "icons\\button-y.svg", {}, false});
          }
          actions.emplace_back(ui::ShellFooterAction {"exit", "Exit", "icons\\button-select.svg", "icons\\button-start.svg", false});
          return actions;
        }
      case app::ScreenId::apps:
        {
          std::vector<ui::ShellFooterAction> actions;
          if (app::selected_app(state) != nullptr) {
            actions.emplace_back(ui::ShellFooterAction {"launch", "Launch", "icons\\button-a.svg", {}, true});
            actions.emplace_back(ui::ShellFooterAction {"app-menu", "App Menu", "icons\\button-y.svg", {}, false});
          }
          actions.emplace_back(ui::ShellFooterAction {"back", "Back", "icons\\button-b.svg", {}, false});
          return actions;
        }
      case app::ScreenId::add_host:
        if (state.addHostDraft.keypad.visible) {
          std::vector<ui::ShellFooterAction> actions {
            {"enter", "Enter Key", "icons\\button-a.svg", {}, true},
            {"delete", "Delete", "icons\\button-x.svg", {}, false},
            {"cancel", "Cancel", "icons\\button-b.svg", {}, false},
          };
          if (state.addHostDraft.activeField == app::AddHostField::address) {
            actions.push_back({"change-section", "Prev/Next", "icons\\button-lb.svg", "icons\\button-rb.svg", false});
          }
          actions.push_back({"accept", "Accept", "icons\\button-start.svg", {}, false});
          return actions;
        }
        return {
          {"select", "Select", "icons\\button-a.svg", {}, true},
          {"back", "Back", "icons\\button-b.svg", {}, false},
        };
      case app::ScreenId::pair_host:
        return {
          {"back", "Cancel", "icons\\button-b.svg", {}, false},
        };
      case app::ScreenId::settings:
        return {
          {"select", "Select", "icons\\button-a.svg", {}, true},
          {"back", state.settings.focusArea == app::SettingsFocusArea::options ? "Categories" : "Back", "icons\\button-b.svg", {}, false},
        };
    }

    return {};
  }

  /**
   * @brief Translate a bundled controller glyph to its working keyboard equivalent.
   * @param icon Controller icon path.
   * @return Keyboard keycap label, or an empty string for a non-controller icon.
   */
  std::string keyboard_hint_for_icon(const std::string &icon) {
    if (icon == "icons\\button-a.svg" || icon == "icons\\button-start.svg") {
      return "Enter";
    }
    if (icon == "icons\\button-b.svg" || icon == "icons\\button-select.svg") {
      return "Esc";
    }
    if (icon == "icons\\button-x.svg") {
      return "Bksp";
    }
    if (icon == "icons\\button-y.svg") {
      return "M";
    }
    if (icon == "icons\\button-lb.svg") {
      return "PgUp";
    }
    if (icon == "icons\\button-rb.svg") {
      return "PgDn";
    }
    if (icon == "icons\\button-lt.svg") {
      return "Shift+PgUp";
    }
    if (icon == "icons\\button-rt.svg") {
      return "Shift+PgDn";
    }
    return {};
  }

  /**
   * @brief Replace controller footer glyphs with scalable keyboard keycaps.
   * @param actions Page, modal or notification actions.
   */
  void use_keyboard_hints(std::vector<ui::ShellFooterAction> &actions) {
    for (auto &action : actions) {
      action.keyLabel = keyboard_hint_for_icon(action.iconAssetPath);
      action.secondaryKeyLabel = keyboard_hint_for_icon(action.secondaryIconAssetPath);
      if (action.id == "exit") {
        action.keyLabel = "Alt+F4";
        action.secondaryKeyLabel.clear();
      }
      if (action.id == "enter") {
        action.keyLabel = "Space";
      }
      if (action.keyLabel == action.secondaryKeyLabel) {
        action.secondaryKeyLabel.clear();
      }
      if (!action.keyLabel.empty()) {
        action.iconAssetPath.clear();
      }
      if (!action.secondaryKeyLabel.empty() || action.secondaryIconAssetPath == "icons\\button-start.svg") {
        action.secondaryIconAssetPath.clear();
      }
    }
  }

  /**
   * @brief Supply default modal actions and choose glyphs for the most recently used input device.
   * @param state Source input device.
   * @param viewModel Receives contextual helpers throughout the shell.
   */
  void fill_input_hints(const app::ClientState &state, ui::ShellViewModel &viewModel) {
    if (viewModel.modal.visible && !viewModel.modal.busy && viewModel.modal.footerActions.empty()) {
      if (viewModel.modal.actions.empty()) {
        viewModel.modal.footerActions = {{"close", "Close", "icons\\button-b.svg", {}, false}};
      } else {
        viewModel.modal.footerActions = {{"select", "Select", "icons\\button-a.svg", {}, true}, {"back", "Back", "icons\\button-b.svg", {}, false}};
      }
    }
    if (state.shell.inputDevice != input::UiInputDevice::keyboard) {
      return;
    }
    use_keyboard_hints(viewModel.frame.footerActions);
    use_keyboard_hints(viewModel.keypad.footerActions);
    use_keyboard_hints(viewModel.modal.footerActions);
    for (auto &notification : viewModel.notification.items) {
      use_keyboard_hints(notification.actions);
    }
  }

}  // namespace

namespace ui {

  /**
   * @brief Return whether a screen uses the split menu-and-detail layout.
   *
   * @param screen Screen identifier to inspect.
   * @return true when the screen renders a split menu layout.
   */
  bool screen_uses_split_menu_layout(app::ScreenId screen) {
    return screen == app::ScreenId::settings || screen == app::ScreenId::add_host || screen == app::ScreenId::pair_host;
  }

  /**
   * @brief Populate which shell-view panels are currently active.
   *
   * @param state Current reducer-owned client state.
   * @param viewModel View model being assembled for rendering.
   */
  void fill_view_model_panel_state(const app::ClientState &state, ShellViewModel *viewModel) {
    if (!screen_uses_split_menu_layout(state.shell.activeScreen)) {
      return;
    }

    viewModel->content.leftPanelActive = state.shell.activeScreen != app::ScreenId::settings || state.settings.focusArea == app::SettingsFocusArea::categories;
    viewModel->content.rightPanelActive = state.shell.activeScreen == app::ScreenId::settings && state.settings.focusArea == app::SettingsFocusArea::options;
  }

  /**
   * @brief Copy the currently selected menu row label and description into the view model.
   *
   * @param state Current reducer-owned client state.
   * @param viewModel View model being assembled for rendering.
   */
  void fill_view_model_selected_menu_details(const app::ClientState &state, ShellViewModel *viewModel) {
    if (!screen_uses_split_menu_layout(state.shell.activeScreen)) {
      return;
    }

    if (state.shell.activeScreen == app::ScreenId::settings && state.settings.focusArea == app::SettingsFocusArea::options && state.detailMenu.selected_item() != nullptr) {
      viewModel->content.selectedMenuRowLabel = state.detailMenu.selected_item()->label;
      viewModel->content.selectedMenuRowDescription = state.detailMenu.selected_item()->description;
      return;
    }
    if (state.menu.selected_item() != nullptr) {
      viewModel->content.selectedMenuRowLabel = state.menu.selected_item()->label;
      viewModel->content.selectedMenuRowDescription = state.menu.selected_item()->description;
    }
  }

  /**
   * @brief Fill the diagnostics overlay with streaming stats and recent log lines.
   *
   * @param state Current reducer-owned client state.
   * @param logEntries Retained log entries available for overlay display.
   * @param statsLines Preformatted streaming statistics lines.
   * @param viewModel View model being assembled for rendering.
   */
  void fill_view_model_overlay(const app::ClientState &state, const std::vector<logging::LogEntry> &logEntries, const std::vector<std::string> &statsLines, ShellViewModel *viewModel) {
    if (!viewModel->overlay.visible) {
      return;
    }

    if (!statsLines.empty()) {
      viewModel->overlay.lines.insert(viewModel->overlay.lines.end(), statsLines.begin(), statsLines.end());
    } else {
      viewModel->overlay.lines.emplace_back("No active stream");
    }

    const std::size_t logLineLimit = 10;
    const std::size_t availableLogCount = logEntries.size();
    const std::size_t maxOffset = availableLogCount > logLineLimit ? availableLogCount - logLineLimit : 0;
    const std::size_t clampedOffset = std::min(state.shell.overlayScrollOffset, maxOffset);
    const std::size_t startIndex = availableLogCount > logLineLimit ? availableLogCount - logLineLimit - clampedOffset : 0;
    const std::size_t endIndex = std::min(availableLogCount, startIndex + logLineLimit);

    for (std::size_t index = startIndex; index < endIndex; ++index) {
      viewModel->overlay.lines.push_back(logging::format_entry(logEntries[index]));
    }

    if (clampedOffset > 0U) {
      viewModel->overlay.lines.emplace(viewModel->overlay.lines.begin(), "Showing earlier log entries");
    }
  }

  ShellViewModel build_shell_view_model(
    const app::ClientState &state,
    const std::vector<logging::LogEntry> &logEntries,
    const std::vector<std::string> &statsLines
  ) {
    ShellViewModel viewModel {};
    viewModel.frame.screen = state.shell.activeScreen;
    viewModel.frame.title = "Moonlight";
    viewModel.frame.pageTitle = page_title(state);
    viewModel.frame.subtitle = page_subtitle(state);
    viewModel.frame.statusMessage = state.shell.statusMessage;
    viewModel.notification.visible = !state.shell.notifications.empty() && !state.modal.active() && !state.shell.overlayVisible && !(state.shell.activeScreen == app::ScreenId::add_host && state.addHostDraft.keypad.visible);
    if (viewModel.notification.visible) {
      viewModel.notification.items = notification_views(state);
    }
    viewModel.content.hostColumnCount = 3U;
    viewModel.content.appColumnCount = 4U;
    viewModel.content.toolbarButtons = toolbar_buttons(state);
    viewModel.content.hostTiles = host_tiles(state);
    viewModel.content.appTiles = app_tiles(state);
    viewModel.content.bodyLines = body_lines(state);
    viewModel.content.menuRows = menu_rows(state);
    viewModel.content.detailMenuRows = detail_menu_rows(state);
    fill_view_model_panel_state(state, &viewModel);
    fill_view_model_selected_menu_details(state, &viewModel);
    viewModel.frame.footerActions = footer_actions(state);
    viewModel.overlay.visible = state.shell.overlayVisible;
    viewModel.overlay.title = "Diagnostics";
    viewModel.keypad.visible = state.shell.activeScreen == app::ScreenId::add_host && state.addHostDraft.keypad.visible;
    viewModel.keypad.title = state.addHostDraft.activeField == app::AddHostField::address ? "Address Keypad" : "Port Keypad";
    viewModel.keypad.columnCount = 3U;

    if (viewModel.keypad.visible) {
      viewModel.keypad.lines = keypad_modal_lines(state);
      viewModel.keypad.value = keypad_value(state);
      viewModel.keypad.segmentedAddress = state.addHostDraft.activeField == app::AddHostField::address;
      viewModel.keypad.addressOctets = state.addHostDraft.keypad.addressOctets;
      viewModel.keypad.activeOctet = state.addHostDraft.keypad.activeOctet;
      viewModel.keypad.footerActions = viewModel.frame.footerActions;
      viewModel.frame.footerActions.clear();
      viewModel.keypad.buttons = keypad_buttons(state);
    }

    fill_modal_view(state, &viewModel);
    fill_input_hints(state, viewModel);

    fill_view_model_overlay(state, logEntries, statsLines, &viewModel);

    return viewModel;
  }

}  // namespace ui
