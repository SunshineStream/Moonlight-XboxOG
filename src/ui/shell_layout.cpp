/**
 * @file src/ui/shell_layout.cpp
 * @brief Implements the television-safe shell and scrolling card grid geometry.
 */
// class header include
#include "src/ui/shell_layout.h"

// standard includes
#include <algorithm>

namespace ui {

  ShellLayout calculate_shell_layout(int width, int height) {
    // A five-percent inset keeps controls clear of overscan on CRT televisions.
    const int marginX = width / 20;
    const int marginY = height / 20;
    const int gap = std::max(10, height / 40);
    const int navigationHeight = std::max(38, height / 12);
    const int headingHeight = std::max(60, height / 8);
    const int footerHeight = std::max(36, height / 14);
    const int innerWidth = std::max(1, width - marginX * 2);
    const int contentTop = marginY + navigationHeight + gap + headingHeight + gap;
    const int footerTop = height - marginY - footerHeight;
    return {
      {marginX, marginY, innerWidth, navigationHeight},
      {marginX, marginY + navigationHeight + gap, innerWidth, headingHeight},
      {marginX, contentTop, innerWidth, std::max(1, footerTop - gap - contentTop)},
      {marginX, footerTop, innerWidth, footerHeight},
      gap,
    };
  }

  ShellGridLayout calculate_shell_grid(ShellRect bounds, std::size_t itemCount, std::size_t columns, std::size_t selectedIndex, int preferredHeight, int gap) {
    if (bounds.w <= 0 || bounds.h <= 0 || itemCount == 0U || columns == 0U) {
      return {};
    }
    ShellGridLayout grid {};
    grid.columns = static_cast<int>(columns);
    grid.gap = std::max(0, gap);
    grid.totalRows = static_cast<int>((itemCount + columns - 1U) / columns);
    grid.visibleRows = std::min(grid.totalRows, std::max(1, (bounds.h + grid.gap) / (std::max(1, preferredHeight) + grid.gap)));
    const int selectedRow = static_cast<int>(std::min(selectedIndex, itemCount - 1U) / columns);
    grid.startRow = std::clamp(selectedRow + 1 - grid.visibleRows, 0, grid.totalRows - grid.visibleRows);
    grid.scrollbarWidth = grid.totalRows > grid.visibleRows ? 4 : 0;
    const int scrollbarSpace = grid.scrollbarWidth > 0 ? grid.scrollbarWidth + grid.gap : 0;
    grid.cellWidth = std::max(1, (bounds.w - scrollbarSpace - grid.gap * (grid.columns - 1)) / grid.columns);
    grid.cellHeight = std::max(1, std::min(std::max(1, preferredHeight), (bounds.h - grid.gap * (grid.visibleRows - 1)) / grid.visibleRows));
    return grid;
  }

  ShellRect shell_grid_cell(ShellRect bounds, const ShellGridLayout &grid, std::size_t index) {
    if (grid.columns <= 0 || grid.visibleRows <= 0) {
      return {};
    }
    const int row = static_cast<int>(index / static_cast<std::size_t>(grid.columns));
    if (row < grid.startRow || row >= grid.startRow + grid.visibleRows || row >= grid.totalRows) {
      return {};
    }
    return {
      bounds.x + static_cast<int>(index % static_cast<std::size_t>(grid.columns)) * (grid.cellWidth + grid.gap),
      bounds.y + (row - grid.startRow) * (grid.cellHeight + grid.gap),
      grid.cellWidth,
      grid.cellHeight,
    };
  }

}  // namespace ui
