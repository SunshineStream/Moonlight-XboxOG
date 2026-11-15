/**
 * @file src/ui/shell_layout.h
 * @brief Defines resolution-independent geometry for the controller-driven shell.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ui {

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
   * @brief Shared screen regions with television-safe margins.
   */
  struct ShellLayout {
    ShellRect navigation;  ///< Compact brand and toolbar strip.
    ShellRect heading;  ///< Page title and contextual subtitle.
    ShellRect content;  ///< Unframed page content.
    ShellRect footer;  ///< Controller action hints.
    int gap = 0;  ///< Spacing between cards and panels.
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
   * @brief Divide a supported Xbox display into safe, nonoverlapping shell regions.
   *
   * @param width Logical display width, after pixel-aspect correction.
   * @param height Framebuffer height.
   * @return Regions scaled for 480-line and HD output.
   */
  ShellLayout calculate_shell_layout(int width, int height);

  /**
   * @brief Extend settings beside the navigation and heading.
   *
   * @param layout Shared shell regions.
   * @return Detail panel bounded by the top and footer safe margins.
   */
  ShellRect calculate_settings_panel(const ShellLayout &layout);

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
