/**
 * @file tests/unit/ui/shell_layout_test.cpp
 * @brief Verifies TV-safe layout and focus visibility across Xbox display modes.
 */
// test includes
#include <lizardbyte/common/testing.h>

// standard includes
#include <limits>
#include <utility>

// local includes
#include "src/ui/shell_layout.h"

namespace {

  /**
   * @brief Keep loading rotation smooth, clockwise and independent of frame frequency.
   */
  TEST(ShellLayoutTest, RotatesLoadingIconAtAFixedSpeedAndWrapsCleanly) {
    EXPECT_DOUBLE_EQ(ui::shell_spinner_angle(0U), 0.0);
    EXPECT_DOUBLE_EQ(ui::shell_spinner_angle(300U), 90.0);
    EXPECT_DOUBLE_EQ(ui::shell_spinner_angle(600U), 180.0);
    EXPECT_DOUBLE_EQ(ui::shell_spinner_angle(900U), 270.0);
    EXPECT_DOUBLE_EQ(ui::shell_spinner_angle(1200U), 0.0);
    EXPECT_DOUBLE_EQ(ui::shell_spinner_angle(1200300U), 90.0);
    EXPECT_GT(ui::shell_spinner_angle(1199U), ui::shell_spinner_angle(1198U));
    EXPECT_LT(ui::shell_spinner_angle(1199U), 360.0);
    const double lastClockAngle = ui::shell_spinner_angle(std::numeric_limits<std::uint32_t>::max());
    EXPECT_GE(lastClockAngle, 0.0);
    EXPECT_LT(lastClockAngle, 360.0);
  }

  /**
   * @brief Settings use the space beside the heading without overlapping the footer.
   */
  TEST(ShellLayoutTest, ExtendsSettingsAboveTheContentGrid) {
    for (const auto [width, height] : {std::pair {640, 480}, {1280, 720}, {1920, 1080}}) {
      const auto layout = ui::calculate_shell_layout(width, height);
      const auto panel = ui::calculate_settings_panel(layout);
      EXPECT_EQ(panel.y, layout.navigation.y);
      EXPECT_LT(panel.y, layout.content.y);
      EXPECT_GT(panel.x, layout.content.x + layout.content.w / 4);
      EXPECT_EQ(panel.x + panel.w, layout.content.x + layout.content.w);
      EXPECT_EQ(panel.y + panel.h, layout.footer.y - layout.gap);
    }
  }

  /**
   * @brief Long dialogs remain within overscan margins and short dialogs keep their requested size.
   */
  TEST(ShellLayoutTest, ClampsDialogsToTheSafeArea) {
    const auto oversized = ui::calculate_shell_dialog(640, 480, 900, 900);
    EXPECT_EQ(oversized.x, 32);
    EXPECT_EQ(oversized.y, 24);
    EXPECT_EQ(oversized.w, 576);
    EXPECT_EQ(oversized.h, 432);
    const auto compact = ui::calculate_shell_dialog(1280, 720, 640, 400);
    EXPECT_EQ(compact.x, 320);
    EXPECT_EQ(compact.y, 160);
    EXPECT_EQ(compact.w, 640);
    EXPECT_EQ(compact.h, 400);
    const auto empty = ui::calculate_shell_dialog(0, 0, 0, -1);
    EXPECT_EQ(empty.w, 1);
    EXPECT_EQ(empty.h, 1);
  }

  /**
   * @brief Keep navigation, headings, cards and controls inside the overscan inset.
   */
  TEST(ShellLayoutTest, FitsStandardAndWidescreenDisplayModes) {
    for (const auto [width, height] : {std::pair {640, 480}, {720, 480}, {854, 480}, {1280, 720}, {1920, 1080}}) {
      const auto layout = ui::calculate_shell_layout(width, height);
      EXPECT_EQ(layout.navigation.x, width / 20);
      EXPECT_EQ(layout.navigation.y, height / 20);
      EXPECT_GE(layout.heading.y, layout.navigation.y + layout.navigation.h + layout.gap);
      EXPECT_GE(layout.content.y, layout.heading.y + layout.heading.h + layout.gap);
      EXPECT_GE(layout.footer.y, layout.content.y + layout.content.h + layout.gap);
      EXPECT_EQ(layout.footer.y + layout.footer.h, height - height / 20);
      EXPECT_GT(layout.content.h, height / 2 - 30);
      for (const auto rect : {layout.navigation, layout.heading, layout.content, layout.footer}) {
        EXPECT_EQ(rect.w, width - (width / 20) * 2);
        EXPECT_GT(rect.h, 0);
      }
    }
  }

  /**
   * @brief Empty collections and unusable dimensions do not produce grid cells.
   */
  TEST(ShellLayoutTest, RejectsEmptyOrInvalidGrids) {
    for (const auto bounds : {ui::ShellRect {0, 0, 0, 200}, {0, 0, 200, 0}, {0, 0, -1, 200}}) {
      EXPECT_EQ(ui::calculate_shell_grid(bounds, 4, 4, 0, 100, 12).visibleRows, 0);
    }
    EXPECT_EQ(ui::calculate_shell_grid({0, 0, 200, 200}, 0, 4, 0, 100, 12).visibleRows, 0);
    EXPECT_EQ(ui::calculate_shell_grid({0, 0, 200, 200}, 4, 0, 0, 100, 12).visibleRows, 0);
    EXPECT_EQ(ui::shell_grid_cell({0, 0, 200, 200}, {}, 0).w, 0);
  }

  /**
   * @brief A short host collection keeps its intended card size and column positions.
   */
  TEST(ShellLayoutTest, DoesNotStretchASingleRowToFillTheScreen) {
    const ui::ShellRect bounds {32, 164, 576, 244};
    const auto grid = ui::calculate_shell_grid(bounds, 2, 3, 1, 180, 12);
    EXPECT_EQ(grid.totalRows, 1);
    EXPECT_EQ(grid.visibleRows, 1);
    EXPECT_EQ(grid.cellHeight, 180);
    EXPECT_EQ(grid.scrollbarWidth, 0);
    const auto first = ui::shell_grid_cell(bounds, grid, 0);
    const auto second = ui::shell_grid_cell(bounds, grid, 1);
    EXPECT_EQ(first.x, bounds.x);
    EXPECT_EQ(second.x, first.x + first.w + 12);
    EXPECT_EQ(first.y, bounds.y);
    EXPECT_EQ(first.h, 180);
  }

  /**
   * @brief Scrolling retains a selected item in a partial final row without overflowing.
   */
  TEST(ShellLayoutTest, ScrollsToTheSelectedRowAndReservesAScrollbar) {
    const ui::ShellRect bounds {30, 160, 580, 260};
    const auto grid = ui::calculate_shell_grid(bounds, 11, 4, 10, 110, 12);
    EXPECT_EQ(grid.totalRows, 3);
    EXPECT_EQ(grid.visibleRows, 2);
    EXPECT_EQ(grid.startRow, 1);
    EXPECT_EQ(grid.scrollbarWidth, 4);
    EXPECT_EQ(ui::shell_grid_cell(bounds, grid, 0).h, 0);
    EXPECT_EQ(ui::shell_grid_cell(bounds, grid, 12).h, 0);
    const auto selected = ui::shell_grid_cell(bounds, grid, 10);
    EXPECT_EQ(selected.y, bounds.y + 122);
    EXPECT_LE(selected.y + selected.h, bounds.y + bounds.h);
    const auto right = ui::shell_grid_cell(bounds, grid, 7);
    EXPECT_LE(right.x + right.w + grid.scrollbarWidth + grid.gap, bounds.x + bounds.w);
  }

  /**
   * @brief Oversized cells shrink to the viewport and stale selection clamps to the last row.
   */
  TEST(ShellLayoutTest, ClampsOversizedCardsAndStaleFocus) {
    const ui::ShellRect bounds {10, 20, 500, 160};
    const auto grid = ui::calculate_shell_grid(bounds, 13, 4, 999, 400, 10);
    EXPECT_EQ(grid.visibleRows, 1);
    EXPECT_EQ(grid.startRow, 3);
    EXPECT_EQ(grid.cellHeight, 160);
    const auto selected = ui::shell_grid_cell(bounds, grid, 12);
    EXPECT_EQ(selected.y, bounds.y);
    EXPECT_EQ(selected.y + selected.h, bounds.y + bounds.h);
  }

  /**
   * @brief Invalid requested spacing and cell heights still yield usable geometry.
   */
  TEST(ShellLayoutTest, NormalizesNegativeSpacingAndHeight) {
    const auto grid = ui::calculate_shell_grid({0, 0, 50, 50}, 2, 2, 0, 0, -12);
    EXPECT_EQ(grid.gap, 0);
    EXPECT_EQ(grid.cellHeight, 1);
    EXPECT_EQ(grid.cellWidth, 25);
    EXPECT_EQ(grid.startRow, 0);
  }

  /**
   * @brief First-row focus never scrolls forward when more rows fit in the viewport.
   */
  TEST(ShellLayoutTest, KeepsFirstRowAtTop) {
    const auto grid = ui::calculate_shell_grid({0, 0, 400, 300}, 16, 4, 0, 100, 10);
    EXPECT_EQ(grid.visibleRows, 2);
    EXPECT_EQ(grid.startRow, 0);
    EXPECT_EQ(ui::shell_grid_cell({0, 0, 400, 300}, grid, 8).w, 0);
  }

}  // namespace
