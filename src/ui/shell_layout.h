/**
 * @file src/ui/shell_layout.h
 * @brief Defines resolution-independent geometry for the controller-driven shell.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ui {

  /**
   * @brief Choose the Vegur font size for control hints at their raster resolution.
   * @param rasterHeight Height in raster pixels before scanout scaling.
   * @return Hint font size in points.
   */
  constexpr int control_hint_font_size(int rasterHeight) {
    return std::max(16, rasterHeight / 28);
  }

  /**
   * @brief Fit a controller glyph or keycap inside a menu or stream control hint.
   * @param lineHeight Font line spacing in raster pixels.
   * @param availableHeight Maximum height available in the row.
   * @param compact Use the smaller glyphs for shortcuts drawn inside keyboard keys.
   * @return Glyph height in raster pixels.
   */
  constexpr int control_hint_glyph_size(int lineHeight, int availableHeight, bool compact = false) {
    return std::min(availableHeight, compact ? std::max(12, lineHeight * 3 / 4) : lineHeight + 12);
  }

  /**
   * @brief Keep list focus visible without scrolling rows that already contain it.
   * @param count Total rows.
   * @param selected Focused row; oversized values clamp to the final row.
   * @param visible Maximum number of visible rows.
   * @param previousFirst First row of the last rendered window.
   * @return First visible row, or zero for an empty list or viewport.
   */
  std::size_t stable_list_first_row(std::size_t count, std::size_t selected, std::size_t visible, std::size_t previousFirst);

  inline constexpr std::size_t MAX_VISIBLE_NOTIFICATIONS = 3U;  ///< Maximum number of simultaneous notification cards.

  /**
   * @brief Rectangle in square-pixel shell coordinates, before encoder correction.
   */
  struct ShellRect {
    int x = 0;  ///< Left edge.
    int y = 0;  ///< Top edge.
    int w = 0;  ///< Width.
    int h = 0;  ///< Height.
  };

  /**
   * @brief Map a logical repaint to clipped framebuffer pixels with a small raster-rounding guard.
   * @param logical Logical rectangle, with unscaled vertical coordinates.
   * @param logicalWidth Logical display width before encoder correction.
   * @param framebufferWidth Physical framebuffer width.
   * @param framebufferHeight Physical framebuffer height.
   * @return Bounded physical rectangle; empty for invalid or wholly offscreen input.
   */
  ShellRect framebuffer_repaint_rect(ShellRect logical, int logicalWidth, int framebufferWidth, int framebufferHeight);

  /**
   * @brief Shared screen regions with television-safe margins.
   */
  struct ShellLayout {
    ShellRect navigation;  ///< Compact brand and toolbar strip.
    ShellRect heading;  ///< Page title and contextual subtitle.
    ShellRect content;  ///< Unframed page content.
    ShellRect footer;  ///< Strip from the row after the divider to the bottom of the screen.
    ShellRect footerHints;  ///< Left-aligned actions, with room for the save indicator at their right.
    ShellRect saveIndicator;  ///< Lower-right save animation bounds.
    int gap = 0;  ///< Spacing between cards and panels.
  };

  /**
   * @brief Stable settings help and options regions independent of the selected value's glyphs.
   */
  struct ShellSettingsDetailLayout {
    ShellRect title;  ///< Single-line selected setting or category title.
    ShellRect description;  ///< Reserved contextual help area.
    ShellRect optionsHeader;  ///< Options heading below the help divider.
    ShellRect options;  ///< Scrollable settings rows.
  };

  /**
   * @brief Visible rows in a grid that keeps the selected item on screen.
   */
  struct ShellGridLayout {
    int columns = 0;  ///< Number of navigation columns.
    int totalRows = 0;  ///< Total number of populated rows.
    int visibleRows = 0;  ///< Number of rows that fit in the content region.
    int startRow = 0;  ///< First visible row.
    int gap = 0;  ///< Spacing between cells.
    int cellWidth = 0;  ///< Width of each column.
    int cellHeight = 0;  ///< Height of each row, including its caption.
    int scrollbarWidth = 0;  ///< Reserved scrollbar width, or zero when all rows fit.
  };

  /**
   * @brief Divide the display into shell regions and center controls vertically below the footer divider.
   *
   * @param width Logical display width, after pixel-aspect correction.
   * @param height Framebuffer height.
   * @param hintGlyphSize Height of a controller glyph at the current output resolution.
   * @param hintRowsHeight Measured height of wrapped hints, or zero for one row.
   * @return Regions scaled for 480-line and HD output.
   */
  ShellLayout calculate_shell_layout(int width, int height, int hintGlyphSize, int hintRowsHeight = 0);

  /**
   * @brief Extend settings beside the navigation and heading.
   *
   * @param layout Shared shell regions.
   * @return Detail panel bounded by the top and footer safe margins.
   */
  ShellRect calculate_settings_panel(const ShellLayout &layout);

  /**
   * @brief Anchor settings options below a fixed font-based help area.
   *
   * @param bounds Padded interior of the settings panel.
   * @param lineHeight Supporting font's line spacing.
   * @return Stable title, help and options bounds; empty when the inputs are invalid.
   */
  ShellSettingsDetailLayout calculate_settings_detail_layout(ShellRect bounds, int lineHeight);

  /**
   * @brief Center compact numeric columns while preserving their size on wider dialogs.
   *
   * @param bounds Available input or keypad row area.
   * @param columnCount Number of equal-width columns.
   * @param preferredWidth Preferred width of each numeric field or key.
   * @param gap Space between adjacent columns.
   * @return Left-to-right column bounds; empty when the area cannot hold the columns.
   */
  std::vector<ShellRect> calculate_keypad_columns(ShellRect bounds, std::size_t columnCount, int preferredWidth, int gap);

  /**
   * @brief Center a dialog inside television-safe margins.
   *
   * @param width Logical display width.
   * @param height Framebuffer height.
   * @param desiredWidth Preferred dialog width.
   * @param desiredHeight Preferred dialog height.
   * @return Centered and clamped dialog bounds.
   */
  ShellRect calculate_shell_dialog(int width, int height, int desiredWidth, int desiredHeight);

  /**
   * @brief Fixed positions for the stream launch card across all startup steps.
   */
  struct StreamStartupLayout {
    ShellRect card;  ///< Centered dialog bounds.
    ShellRect title;  ///< Single line for the app name.
    ShellRect body;  ///< Reserved lines for status and optional diagnostics.
    ShellRect progress;  ///< Startup step indicators.
    ShellRect hint;  ///< Reserved space for the stream-menu hint.
  };

  /**
   * @brief Size the stream launch card without depending on the current step or text.
   * @param width Output width in pixels.
   * @param height Output height in pixels.
   * @param titleLineHeight Title font line spacing.
   * @param bodyLineHeight Body font line spacing.
   * @return Bounded card and text regions, or an empty layout when they cannot fit.
   */
  StreamStartupLayout calculate_stream_startup_layout(int width, int height, int titleLineHeight, int bodyLineHeight);

  /**
   * @brief Calculate a clockwise loading-icon rotation at a fixed speed.
   *
   * @param elapsedMilliseconds Monotonic elapsed time from the shell clock.
   * @return Rotation in degrees, wrapping every 1200 milliseconds.
   */
  double shell_spinner_angle(std::uint32_t elapsedMilliseconds);

  /**
   * @brief Stack recent notifications above the footer without leaving their safe area.
   *
   * @param area Available notification column.
   * @param newestFirstHeights Measured card heights, newest first.
   * @return Visible card bounds, newest first; older cards remain queued by the application.
   */
  std::vector<ShellRect> calculate_notification_stack(ShellRect area, const std::vector<int> &newestFirstHeights);

  /**
   * @brief Lay out a scrolling grid without stretching short collections vertically.
   *
   * @param bounds Available content rectangle.
   * @param itemCount Number of visible items.
   * @param columns Navigation column count, shared with the application model.
   * @param selectedIndex Currently focused item.
   * @param preferredHeight Desired cell height, including captions.
   * @param gap Spacing between cells.
   * @return Grid geometry; empty for invalid bounds or an empty collection.
   */
  ShellGridLayout calculate_shell_grid(ShellRect bounds, std::size_t itemCount, std::size_t columns, std::size_t selectedIndex, int preferredHeight, int gap);

  /**
   * @brief Resolve a visible item's cell in the scrolling grid.
   *
   * @param bounds Available content rectangle.
   * @param grid Calculated grid geometry.
   * @param index Absolute item index.
   * @return Cell rectangle, or an empty rectangle for an offscreen item.
   */
  ShellRect shell_grid_cell(ShellRect bounds, const ShellGridLayout &grid, std::size_t index);

}  // namespace ui
