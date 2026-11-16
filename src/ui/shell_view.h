/**
 * @file src/ui/shell_view.h
 * @brief Declares the shell view renderer and layout helpers.
 */
#pragma once

// standard includes
#include <cstddef>
#include <string>
#include <vector>

// local includes
#include "src/app/client_state.h"
#include "src/logging/logger.h"

namespace ui {

  /**
   * @brief Render-ready button shown in the hosts toolbar.
   */
  struct ShellToolbarButton {
    std::string id;  ///< Stable button identifier.
    std::string label;  ///< User-facing toolbar label.
    std::string glyph;  ///< Optional text glyph used when no icon asset is available.
    std::string iconAssetPath;  ///< Optional asset path for the button icon.
    bool selected = false;  ///< True when the button currently has focus.
  };

  /**
   * @brief Render-ready host tile for the Moonlight-style hosts page.
   */
  struct ShellHostTile {
    std::string id;  ///< Stable host tile identifier.
    std::string displayName;  ///< Primary host label.
    std::string statusLabel;  ///< Secondary host status text.
    std::string iconAssetPath;  ///< Asset path for the tile icon.
    app::PairingState pairingState = app::PairingState::not_paired;  ///< Pairing state displayed on the tile.
    app::HostReachability reachability = app::HostReachability::unknown;  ///< Reachability state displayed on the tile.
    bool selected = false;  ///< True when the tile currently has focus.
  };

  /**
   * @brief Render-ready app tile for the per-host apps page.
   */
  struct ShellAppTile {
    std::string id;  ///< Stable app tile identifier.
    std::string name;  ///< Primary app label.
    std::string detail;  ///< Secondary app detail text.
    std::string badgeLabel;  ///< Optional badge label such as HDR or Running.
    std::string boxArtCacheKey;  ///< Cache key used to resolve box art.
    bool hidden = false;  ///< True when the app is hidden by default.
    bool favorite = false;  ///< True when the app is flagged as a favorite.
    bool boxArtCached = false;  ///< True when box art is already available locally.
    bool running = false;  ///< True when the app is currently running on the host.
    bool selected = false;  ///< True when the tile currently has focus.
  };

  /**
   * @brief Render-ready vertical action row used by menus and modals.
   */
  struct ShellActionRow {
    std::string id;  ///< Stable row identifier.
    std::string label;  ///< User-facing row label.
    bool enabled = true;  ///< True when the row can be activated.
    bool selected = false;  ///< True when the row currently has focus.
    bool checked = false;  ///< True when the row represents an enabled option.
    std::string iconAssetPath;  ///< Optional icon displayed to the left of a menu label.
    bool checkable = false;  ///< Draw a checkbox for both checked and unchecked toggle values.
  };

  /**
   * @brief Render-ready footer chip pairing a button icon with an action label.
   */
  struct ShellFooterAction {
    std::string id;  ///< Stable footer action identifier.
    std::string label;  ///< User-facing footer label.
    std::string iconAssetPath;  ///< Primary icon asset path.
    std::string secondaryIconAssetPath;  ///< Optional secondary icon asset path.
    bool emphasized = false;  ///< True when the action should receive visual emphasis.
    std::string keyLabel;  ///< Primary keyboard keycap label, replacing the primary controller icon.
    std::string secondaryKeyLabel;  ///< Optional second keyboard keycap label.
  };

  /**
   * @brief Render-ready bottom-right notification shown above the shell content.
   */
  struct ShellNotification {
    std::string title;  ///< Notification title.
    std::string message;  ///< Notification body message.
    std::vector<ShellFooterAction> actions;  ///< Footer actions shown with the notification.
    std::string iconAssetPath;  ///< Severity icon shown beside the event title.
  };

  /**
   * @brief Render-ready numeric or text key with optional explicit keyboard geometry.
   */
  struct ShellModalButton {
    std::string label;  ///< User-facing button label.
    bool enabled = true;  ///< True when the button can be activated.
    bool selected = false;  ///< True when the button currently has focus.
    int column = -1;  ///< Explicit half-column origin for the shared keyboard; negative uses the numeric grid.
    int row = 0;  ///< Explicit keyboard row.
    int width = 1;  ///< Explicit key width in half-columns.
    int height = 1;  ///< Explicit key height in rows.
  };

  /**
   * @brief Shell-wide frame metadata shared across all render paths.
   */
  struct ShellFrameView {
    app::ScreenId screen = app::ScreenId::hosts;  ///< Active screen being rendered.
    std::string title;  ///< Shell-wide title.
    std::string pageTitle;  ///< Primary page heading.
    std::string subtitle;  ///< Concise page context, collection count, or loading state.
    std::string statusMessage;  ///< Status line shown near the footer.
    std::vector<ShellFooterAction> footerActions;  ///< Footer actions shown for the current screen.
  };

  /**
   * @brief Main page content shown behind overlays and modals.
   */
  struct ShellPageContentView {
    std::vector<ShellToolbarButton> toolbarButtons;  ///< Toolbar buttons for the hosts page.
    std::vector<ShellHostTile> hostTiles;  ///< Host tiles shown on the hosts page.
    std::size_t hostColumnCount = 3U;  ///< Number of columns used to lay out host tiles.
    std::vector<ShellAppTile> appTiles;  ///< App tiles shown on the apps page.
    std::size_t appColumnCount = 4U;  ///< Number of columns used to lay out app tiles.
    std::vector<std::string> bodyLines;  ///< Generic body lines for text-driven screens.
    std::vector<ShellActionRow> menuRows;  ///< Primary action rows for the active screen.
    std::vector<ShellActionRow> detailMenuRows;  ///< Secondary action rows for details or settings.
    std::string selectedMenuRowLabel;  ///< Label of the currently selected primary menu row.
    std::string selectedMenuRowDescription;  ///< Description of the currently selected row, when available.
    bool leftPanelActive = false;  ///< True when the left navigation list owns controller focus.
    bool rightPanelActive = false;  ///< True when the settings options own controller focus.
  };

  /**
   * @brief Transient notification view rendered above the main shell content.
   */
  struct ShellNotificationView {
    bool visible = false;  ///< True when a transient notification should be rendered.
    std::vector<ShellNotification> items;  ///< Outstanding notifications, oldest first.
  };

  /**
   * @brief Diagnostics overlay view rendered above the shell content.
   */
  struct ShellOverlayView {
    bool visible = false;  ///< True when the diagnostics overlay should be rendered.
    std::string title;  ///< Diagnostics overlay title.
    std::vector<std::string> lines;  ///< Diagnostics overlay body lines.
  };

  /**
   * @brief Modal dialog view rendered on top of the current shell page.
   */
  struct ShellModalView {
    bool visible = false;  ///< True when a modal dialog should be rendered.
    bool busy = false;  ///< Show progress instead of dismissal controls while work is pending.
    bool folderBrowser = false;  ///< Retain the folder dialog while only list focus changes.
    std::string title;  ///< Modal dialog title.
    std::string iconAssetPath;  ///< Optional status icon beside the dialog title.
    std::vector<std::string> lines;  ///< Modal dialog body lines.
    std::vector<ShellActionRow> actions;  ///< Modal action rows.
    std::vector<ShellFooterAction> footerActions;  ///< Footer actions displayed while a modal is open.
  };

  /**
   * @brief Check whether folder-dialog pixels can be retained while list selection changes.
   * @param previous Previously painted dialog.
   * @param current Current dialog, including path, error, entries and input helpers.
   * @return True when every visible property except the focused action is unchanged.
   */
  bool same_folder_dialog_content(const ShellModalView &previous, const ShellModalView &current);

  /**
   * @brief Embedded log viewer state surfaced by the log-file modal.
   */
  struct ShellLogViewerView {
    bool visible = false;  ///< True when the log viewer should be rendered.
    std::string path;  ///< Path of the currently loaded log file.
    std::vector<std::string> lines;  ///< Loaded log lines shown in the viewer.
    std::size_t scrollOffset = 0U;  ///< Vertical scroll offset inside the log viewer.
    app::LogViewerPlacement placement = app::LogViewerPlacement::full;  ///< Placement of the log viewer pane.
  };

  /**
   * @brief Local numeric or text entry modal, shared by host entry and folder creation.
   */
  struct ShellKeypadModalView {
    bool visible = false;  ///< True when the keypad modal should be rendered.
    std::string title;  ///< Title shown at the top of the keypad modal.
    std::string value;  ///< Staged input displayed in the keypad text field.
    bool segmentedAddress = false;  ///< Render four IPv4 sections with fixed period separators.
    std::array<std::string, 4> addressOctets {};  ///< Draft digits displayed in each IPv4 section.
    std::size_t activeOctet = 0U;  ///< Highlighted IPv4 section.
    std::vector<std::string> lines;  ///< Instruction and draft lines shown in the keypad modal.
    std::vector<ShellModalButton> buttons;  ///< Buttons rendered inside the keypad modal.
    std::size_t columnCount = 0U;  ///< Number of columns used to lay out keypad buttons.
    std::size_t rowCount = 0U;  ///< Explicit keyboard row count; zero derives rows from the numeric grid.
    std::vector<ShellFooterAction> footerActions;  ///< Input, delete, cancel and accept controls inside the dialog.
  };

  /**
   * @brief Current screenshot and captions for the local full-screen image viewer.
   */
  struct ShellScreenshotView {
    bool visible = false;  ///< True while the app screenshot viewer owns the display.
    std::string path;  ///< Current bitmap path; empty when no captures are indexed.
    std::string caption;  ///< Capture time and position within this app's library.
    std::string status;  ///< Empty-library guidance or a recoverable journal warning.
  };

  /**
   * @brief Render-ready shell state derived from the app model.
   */
  struct ShellViewModel {
    ShellFrameView frame;  ///< Shell-wide frame metadata.
    ShellNotificationView notification;  ///< Transient notification rendered above the page.
    ShellPageContentView content;  ///< Main page content for the active screen.
    ShellOverlayView overlay;  ///< Diagnostics overlay content.
    ShellModalView modal;  ///< Modal dialog content.
    ShellLogViewerView logViewer;  ///< Embedded log viewer surfaced by the log modal.
    ShellKeypadModalView keypad;  ///< Add-host keypad modal content.
    ShellScreenshotView screenshot;  ///< Single-image local viewer with app-specific navigation.
  };

  /**
   * @brief Build a render-ready shell view from app state and diagnostics.
   *
   * @param state Current app shell state.
   * @param logEntries Recent log entries for the optional overlay.
   * @param statsLines Optional streaming statistics overlay lines.
   * @return A render-ready shell view model.
   */
  ShellViewModel build_shell_view_model(
    const app::ClientState &state,
    const std::vector<logging::LogEntry> &logEntries,
    const std::vector<std::string> &statsLines = {}
  );

}  // namespace ui
