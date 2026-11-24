/**
 * @file tests/unit/ui/save_indicator_timing_test.cpp
 * @brief Verifies the save indicator's minimum display time and its response to real saves.
 */
// test header include
#include "src/ui/save_indicator_timing.h"

// lib includes
#include <lizardbyte/common/testing.h>

/**
 * @brief Show a write that completes between frames for a full half-second.
 */
TEST(SaveIndicatorTimingTest, ShowsASaveThatCompletesBetweenFrames) {
  ui::SaveIndicatorTiming indicator(app::file_save_snapshot().generation);
  EXPECT_FALSE(indicator.sample(1000, app::file_save_snapshot()).has_value());
  {
    const app::FileSaveActivity saving;
  }
  EXPECT_EQ(indicator.sample(1100, app::file_save_snapshot()), 1080);
  EXPECT_EQ(indicator.sample(1599, app::file_save_snapshot()), 1560);
  EXPECT_FALSE(indicator.sample(1600, app::file_save_snapshot()).has_value());
  EXPECT_FALSE(indicator.sample(2000, app::file_save_snapshot()).has_value());
}

/**
 * @brief Keep a long save visible until the writer exits, then hide the indicator promptly.
 */
TEST(SaveIndicatorTimingTest, KeepsTheLogoVisibleWhileALongSaveRuns) {
  ui::SaveIndicatorTiming indicator(10);
  EXPECT_EQ(indicator.sample(0, {11, true}), 0);
  EXPECT_EQ(indicator.sample(600, {11, true}), 600);
  EXPECT_EQ(indicator.sample(1200, {11, true}), 1200);
  EXPECT_FALSE(indicator.sample(1201, {11, false}).has_value());
}

/**
 * @brief Start the minimum display time over when a separate save arrives during the hold.
 */
TEST(SaveIndicatorTimingTest, ExtendsTheDisplayForANewSaveDuringTheHold) {
  ui::SaveIndicatorTiming indicator(10);
  EXPECT_EQ(indicator.sample(20, {11, false}), 0);
  EXPECT_EQ(indicator.sample(400, {12, false}), 360);
  EXPECT_EQ(indicator.sample(899, {12, false}), 840);
  EXPECT_FALSE(indicator.sample(900, {12, false}).has_value());
}

/**
 * @brief Show a save already in progress when rendering starts.
 */
TEST(SaveIndicatorTimingTest, ShowsAnActiveSaveFromBeforeTheFirstSample) {
  ui::SaveIndicatorTiming indicator(10);
  EXPECT_EQ(indicator.sample(300, {10, true}), 300);
  EXPECT_TRUE(indicator.sample(799, {10, false}).has_value());
  EXPECT_FALSE(indicator.sample(800, {10, false}).has_value());
}
