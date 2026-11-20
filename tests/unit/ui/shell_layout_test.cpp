/**
 * @file tests/unit/ui/shell_layout_test.cpp
 * @brief Verifies TV-safe layout and focus visibility across Xbox display modes.
 */
// test header include
#include "src/ui/shell_layout.h"

// standard includes
#include <cstdlib>
#include <limits>
#include <utility>

// lib includes
#include <lizardbyte/common/testing.h>

namespace {

  /**
   * @brief Keep settings rows anchored when headings or help wrap differently.
   */
  TEST(ShellLayoutTest, AnchorsSettingsOptionsIndependentlyOfTextWidth) {
    for (const auto [width, height] : {std::pair {640, 480}, {1280, 720}, {1920, 1080}}) {
      const auto shell = ui::calculate_shell_layout(width, height);
      const auto panel = ui::calculate_settings_panel(shell);
      const int padding = 14;
      const ui::ShellRect interior {panel.x + padding, panel.y + padding, panel.w - padding * 2, panel.h - padding * 2};
      const int lineHeight = height / 40;
      const auto normal = ui::calculate_settings_detail_layout(interior, lineHeight);
      auto narrower = interior;
      narrower.w /= 2;
      const auto wrapped = ui::calculate_settings_detail_layout(narrower, lineHeight);
      EXPECT_EQ(normal.description.y, wrapped.description.y);
      EXPECT_EQ(normal.description.h, wrapped.description.h);
      EXPECT_EQ(normal.options.y, wrapped.options.y);
      EXPECT_EQ(normal.options.h, wrapped.options.h);
      EXPECT_GE(normal.description.h, lineHeight * 4);
      EXPECT_GT(normal.description.y, normal.title.y + normal.title.h);
      EXPECT_GT(normal.optionsHeader.y, normal.description.y + normal.description.h);
      EXPECT_EQ(normal.options.y, normal.optionsHeader.y + normal.optionsHeader.h);
      EXPECT_EQ(normal.options.y + normal.options.h, interior.y + interior.h);
      EXPECT_GE(normal.options.h, interior.h / 3);
    }
  }

  /**
   * @brief Bound settings regions in tiny panels and reject unusable layout inputs.
   */
  TEST(ShellLayoutTest, BoundsSettingsHelpInSmallAreas) {
    const auto compact = ui::calculate_settings_detail_layout({10, 20, 50, 30}, 20);
    for (const auto rect : {compact.title, compact.description, compact.optionsHeader, compact.options}) {
      EXPECT_GE(rect.y, 20);
      EXPECT_GE(rect.h, 0);
      EXPECT_LE(rect.y + rect.h, 50);
    }
    EXPECT_EQ(ui::calculate_settings_detail_layout({0, 0, 0, 100}, 16).options.h, 0);
    EXPECT_EQ(ui::calculate_settings_detail_layout({0, 0, 100, 0}, 16).options.h, 0);
    EXPECT_EQ(ui::calculate_settings_detail_layout({0, 0, 100, 100}, 0).options.h, 0);
  }

  /**
   * @brief Numeric key and IPv4 columns stay compact and centered at SD and HD sizes.
   */
  TEST(ShellLayoutTest, CentersCompactNumericColumnsWithinWideDialogs) {
    for (const int width : {476, 684}) {
      for (const std::size_t count : {3U, 4U}) {
        const ui::ShellRect bounds {30, 70, width, 40};
        const int gap = count == 4U ? 18 : 8;
        const auto columns = ui::calculate_keypad_columns(bounds, count, 80, gap);
        ASSERT_EQ(columns.size(), count);
        EXPECT_GT(columns.front().x, bounds.x);
        const int right = columns.back().x + columns.back().w;
        EXPECT_LE(std::abs((columns.front().x - bounds.x) - (bounds.x + bounds.w - right)), 1);
        for (const auto &column : columns) {
          EXPECT_EQ(column.w, 80);
          EXPECT_EQ(column.y, bounds.y);
          EXPECT_EQ(column.h, bounds.h);
        }
        for (std::size_t index = 1; index < columns.size(); ++index) {
          EXPECT_EQ(columns[index].x - columns[index - 1U].x - columns[index - 1U].w, gap);
        }
      }
    }
  }

  /**
   * @brief Compact columns shrink to fit without negative spacing or zero-width keys.
   */
  TEST(ShellLayoutTest, FitsNumericColumnsIntoNarrowAreas) {
    const auto narrow = ui::calculate_keypad_columns({5, 10, 100, 30}, 3U, 80, 8);
    ASSERT_EQ(narrow.size(), 3U);
    EXPECT_EQ(narrow.front().x, 5);
    EXPECT_EQ(narrow.back().x + narrow.back().w, 105);
    EXPECT_EQ(narrow.front().w, 28);
    const auto crowded = ui::calculate_keypad_columns({0, 0, 4, 10}, 3U, 50, 18);
    ASSERT_EQ(crowded.size(), 3U);
    EXPECT_EQ(crowded.front().w, 1);
    const auto single = ui::calculate_keypad_columns({0, 0, 100, 10}, 1U, 20, 18);
    ASSERT_EQ(single.size(), 1U);
    EXPECT_EQ(single.front().x, 40);
    const auto minimum = ui::calculate_keypad_columns({0, 0, 100, 10}, 3U, 0, -10);
    ASSERT_EQ(minimum.size(), 3U);
    EXPECT_EQ(minimum.front().w, 1);
    EXPECT_EQ(minimum[1].x, minimum[0].x + 1);
    EXPECT_TRUE(ui::calculate_keypad_columns({0, 0, 0, 10}, 3U, 80, 8).empty());
    EXPECT_TRUE(ui::calculate_keypad_columns({0, 0, 100, 0}, 3U, 80, 8).empty());
    EXPECT_TRUE(ui::calculate_keypad_columns({0, 0, 100, 10}, 0U, 80, 8).empty());
    EXPECT_TRUE(ui::calculate_keypad_columns({0, 0, 2, 10}, 3U, 80, 8).empty());
  }

  /**
   * @brief Fit concurrent cards above the footer without overlapping on SD or HD output.
   */
  TEST(ShellLayoutTest, StacksRecentNotificationsWithinTheirSafeColumn) {
    for (const auto [width, height] : {std::pair {640, 480}, {1280, 720}}) {
      const auto layout = ui::calculate_shell_layout(width, height);
      const ui::ShellRect area {width - 332, layout.navigation.y, 300, layout.footer.y - layout.navigation.y - 14};
      const auto cards = ui::calculate_notification_stack(area, {140, 100, 100, 100});
      ASSERT_GE(cards.size(), 2U);
      EXPECT_LE(cards.size(), ui::MAX_VISIBLE_NOTIFICATIONS);
      EXPECT_EQ(cards.front().y + cards.front().h, area.y + area.h);
      for (std::size_t index = 0; index < cards.size(); ++index) {
        EXPECT_EQ(cards[index].x, area.x);
        EXPECT_EQ(cards[index].w, area.w);
        EXPECT_GE(cards[index].y, area.y);
        if (index > 0U) {
          EXPECT_LE(cards[index].y + cards[index].h + 10, cards[index - 1U].y);
        }
      }
    }
    const auto limited = ui::calculate_notification_stack({0, 0, 300, 1000}, {100, 100, 100, 100});
    EXPECT_EQ(limited.size(), ui::MAX_VISIBLE_NOTIFICATIONS);
    const auto tight = ui::calculate_notification_stack({0, 0, 300, 250}, {140, 100, 100});
    EXPECT_EQ(tight.size(), 2U);
  }

  /**
   * @brief Clamp oversized cards and handle absent or invalid notification areas safely.
   */
  TEST(ShellLayoutTest, ClampsNotificationCardsAndHandlesEmptyAreas) {
    EXPECT_TRUE(ui::calculate_notification_stack({0, 0, 0, 100}, {50}).empty());
    EXPECT_TRUE(ui::calculate_notification_stack({0, 0, 300, 0}, {50}).empty());
    EXPECT_TRUE(ui::calculate_notification_stack({0, 0, 300, 100}, {}).empty());
    const auto oversized = ui::calculate_notification_stack({20, 30, 300, 100}, {900, 50});
    ASSERT_EQ(oversized.size(), 1U);
    EXPECT_EQ(oversized[0].y, 30);
    EXPECT_EQ(oversized[0].h, 100);
    const auto minimum = ui::calculate_notification_stack({0, 0, 300, 100}, {0});
    ASSERT_EQ(minimum.size(), 1U);
    EXPECT_EQ(minimum[0].h, 1);
  }

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
   * @brief Reserve positions for the title, status, progress and hint throughout launch.
   */
  TEST(ShellLayoutTest, ReservesFixedStreamStartupRows) {
    for (const auto [width, height] : {std::pair {640, 480}, {1280, 720}, {1920, 1080}}) {
      constexpr int titleLine = 32;
      constexpr int bodyLine = 20;
      const auto layout = ui::calculate_stream_startup_layout(width, height, titleLine, bodyLine);
      EXPECT_GE(layout.card.x, width / 20);
      EXPECT_GE(layout.card.y, height / 20);
      EXPECT_EQ(layout.title.h, titleLine);
      EXPECT_EQ(layout.body.h, 3 * bodyLine);
      EXPECT_EQ(layout.hint.h, 2 * bodyLine);
      for (const auto rect : {layout.title, layout.body, layout.progress, layout.hint}) {
        EXPECT_GT(rect.x, layout.card.x);
        EXPECT_LT(rect.x + rect.w, layout.card.x + layout.card.w);
      }
      EXPECT_LT(layout.title.y + layout.title.h, layout.body.y);
      EXPECT_LT(layout.body.y + layout.body.h, layout.progress.y);
      EXPECT_LT(layout.progress.y + layout.progress.h, layout.hint.y);
      EXPECT_LT(layout.hint.y + layout.hint.h, layout.card.y + layout.card.h);
    }
  }

  /**
   * @brief Keep at least one status row in cramped layouts and reject unusable bounds.
   */
  TEST(ShellLayoutTest, ClampsStartupStatusToAvailableHeight) {
    const auto limited = ui::calculate_stream_startup_layout(320, 240, 28, 20);
    EXPECT_GE(limited.body.h, 20);
    EXPECT_LT(limited.body.h, 60);
    EXPECT_EQ(ui::calculate_stream_startup_layout(320, 100, 28, 20).card.w, 0);
    EXPECT_EQ(ui::calculate_stream_startup_layout(40, 480, 28, 20).card.w, 0);
    EXPECT_EQ(ui::calculate_stream_startup_layout(0, 0, 0, 0).card.w, 0);
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

/**
 * @brief A list retains its viewport until focus reaches an edge, then scrolls by the necessary rows.
 */
TEST(ShellListViewportTest, KeepsVisibleRowsStableAndScrollsAtTheEdges) {
  EXPECT_EQ(ui::stable_list_first_row(80, 0, 8, 0), 0);
  EXPECT_EQ(ui::stable_list_first_row(80, 7, 8, 0), 0);
  EXPECT_EQ(ui::stable_list_first_row(80, 8, 8, 0), 1);
  EXPECT_EQ(ui::stable_list_first_row(80, 9, 8, 1), 2);
  EXPECT_EQ(ui::stable_list_first_row(80, 2, 8, 2), 2);
  EXPECT_EQ(ui::stable_list_first_row(80, 1, 8, 2), 1);
  EXPECT_EQ(ui::stable_list_first_row(80, 79, 8, 0), 72);
  EXPECT_EQ(ui::stable_list_first_row(80, 0, 8, 72), 0);
}

/**
 * @brief Empty, short and resized lists clamp stale selection and viewport values without unsigned underflow.
 */
TEST(ShellListViewportTest, HandlesEmptyShortAndResizedLists) {
  EXPECT_EQ(ui::stable_list_first_row(0, 100, 8, 100), 0);
  EXPECT_EQ(ui::stable_list_first_row(80, 100, 0, 100), 0);
  EXPECT_EQ(ui::stable_list_first_row(4, 3, 8, 100), 0);
  EXPECT_EQ(ui::stable_list_first_row(8, 7, 8, 100), 0);
  EXPECT_EQ(ui::stable_list_first_row(20, 100, 8, 100), 12);
  EXPECT_EQ(ui::stable_list_first_row(20, 15, 8, 100), 12);
}

/**
 * @brief Partial presentation covers scaled row edges while remaining inside the physical framebuffer.
 */
TEST(ShellListViewportTest, MapsRepaintsToBoundedPhysicalPixels) {
  auto area = ui::framebuffer_repaint_rect({138, 155, 684, 42}, 960, 1280, 720);
  EXPECT_EQ(area.x, 182);
  EXPECT_EQ(area.y, 154);
  EXPECT_EQ(area.w, 916);
  EXPECT_EQ(area.h, 44);
  area = ui::framebuffer_repaint_rect({-10, -10, 1000, 800}, 960, 1280, 720);
  EXPECT_EQ(area.x, 0);
  EXPECT_EQ(area.y, 0);
  EXPECT_EQ(area.w, 1280);
  EXPECT_EQ(area.h, 720);
  EXPECT_EQ(ui::framebuffer_repaint_rect({2000, 0, 10, 10}, 960, 1280, 720).w, 0);
  EXPECT_EQ(ui::framebuffer_repaint_rect({0, 900, 10, 10}, 960, 1280, 720).h, 0);
  EXPECT_EQ(ui::framebuffer_repaint_rect({0, 0, 0, 10}, 960, 1280, 720).w, 0);
  EXPECT_EQ(ui::framebuffer_repaint_rect({0, 0, 10, -1}, 960, 1280, 720).h, 0);
  EXPECT_EQ(ui::framebuffer_repaint_rect({0, 0, 10, 10}, 0, 1280, 720).w, 0);
  EXPECT_EQ(ui::framebuffer_repaint_rect({0, 0, 10, 10}, 960, 0, 720).w, 0);
  EXPECT_EQ(ui::framebuffer_repaint_rect({0, 0, 10, 10}, 960, 1280, 0).w, 0);
}
