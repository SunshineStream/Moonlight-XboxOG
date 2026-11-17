/**
 * @file tests/unit/streaming/screenshot_status_test.cpp
 * @brief Verifies persistent save progress and timed screenshot completion feedback.
 */
// test header include
#include "src/streaming/screenshot_status.h"

// standard includes
#include <cstdint>
#include <limits>

// lib includes
#include <lizardbyte/common/testing.h>

/**
 * @brief Saving never times out, never reports premature completion, and avoids redraws for every disk row.
 */
TEST(ScreenshotNotificationTest, RetainsPendingCaptureAndThrottlesProgress) {
  streaming::ScreenshotNotification notification;
  EXPECT_FALSE(notification.visible);
  EXPECT_FALSE(notification.update(20));
  notification.begin();
  EXPECT_TRUE(notification.visible);
  EXPECT_TRUE(notification.saving);
  EXPECT_EQ(notification.title(), "Saving screenshot... 0%");
  EXPECT_FALSE(notification.update(-1));
  EXPECT_FALSE(notification.update(4));
  EXPECT_TRUE(notification.update(7));
  EXPECT_EQ(notification.percent, 5);
  EXPECT_FALSE(notification.update(9));
  EXPECT_FALSE(notification.update(0));
  EXPECT_TRUE(notification.update(1000));
  EXPECT_EQ(notification.percent, 95);
  EXPECT_FALSE(notification.tick(std::numeric_limits<std::uint32_t>::max()));
  EXPECT_TRUE(notification.visible);
}

/**
 * @brief Successful completion displays its path for five seconds, including across tick wraparound.
 */
TEST(ScreenshotNotificationTest, DisplaysSuccessUntilDeadlineAcrossTickWrap) {
  streaming::ScreenshotNotification notification;
  notification.begin();
  const auto completedAt = std::numeric_limits<std::uint32_t>::max() - 100U;
  notification.finish(true, "Saved E:\\Shots\\capture.bmp", completedAt);
  EXPECT_TRUE(notification.saved);
  EXPECT_FALSE(notification.saving);
  EXPECT_EQ(notification.percent, 100);
  EXPECT_EQ(notification.title(), "Screenshot saved");
  EXPECT_EQ(notification.detail, "Saved E:\\Shots\\capture.bmp");
  EXPECT_FALSE(notification.update(5));
  EXPECT_FALSE(notification.tick(completedAt + 4999U));
  EXPECT_TRUE(notification.tick(completedAt + 5000U));
  EXPECT_FALSE(notification.visible);
  EXPECT_FALSE(notification.tick(completedAt + 9000U));
}

/**
 * @brief Failed writes retain their explanation longer and a new capture clears the old result and timer.
 */
TEST(ScreenshotNotificationTest, DisplaysFailureAndResetsForAnotherCapture) {
  streaming::ScreenshotNotification notification;
  notification.begin();
  EXPECT_TRUE(notification.update(52));
  notification.finish(false, "Screenshot failed: disk is full", 0);
  EXPECT_FALSE(notification.saved);
  EXPECT_EQ(notification.percent, 50);
  EXPECT_EQ(notification.title(), "Screenshot failed");
  EXPECT_FALSE(notification.tick(7999));
  EXPECT_TRUE(notification.tick(8000));
  notification.begin();
  EXPECT_TRUE(notification.visible);
  EXPECT_TRUE(notification.saving);
  EXPECT_TRUE(notification.detail.empty());
  EXPECT_EQ(notification.percent, 0);
  EXPECT_FALSE(notification.tick(16000));
}

/**
 * @brief A pending popup needs one line; completion expands to fit the full path within the video safe area.
 */
TEST(ScreenshotNotificationTest, SizesPopupForContentAndFitsLongPaths) {
  const auto saving = streaming::layout_screenshot_notification(1280, 720, 24, 260, 1000, true);
  EXPECT_EQ(saving.width, 284);
  EXPECT_EQ(saving.height, 52);
  const auto saved = streaming::layout_screenshot_notification(1280, 720, 24, 200, 900, false);
  EXPECT_EQ(saved.width, 924);
  EXPECT_EQ(saved.height, 80);
  const auto longPath = streaming::layout_screenshot_notification(640, 480, 18, 180, 2000, false);
  EXPECT_EQ(longPath.width, 624);
  EXPECT_EQ(longPath.height, 68);
  const auto tiny = streaming::layout_screenshot_notification(100, 50, 18, 180, 2000, false);
  EXPECT_EQ(tiny.width, 84);
  EXPECT_EQ(tiny.height, 34);
  EXPECT_EQ(streaming::layout_screenshot_notification(16, 50, 18, 10, 10, false).width, 0);
  EXPECT_EQ(streaming::layout_screenshot_notification(50, 16, 18, 10, 10, false).height, 0);
  EXPECT_EQ(streaming::layout_screenshot_notification(50, 50, 0, 10, 10, false).width, 0);
}

/**
 * @brief Completion shows the path or error once, with no explanatory body while saving.
 */
TEST(ScreenshotNotificationTest, RemovesRedundantResultPrefixes) {
  streaming::ScreenshotNotification notification;
  notification.begin();
  EXPECT_TRUE(notification.body().empty());
  notification.finish(true, "Saved E:\\UDATA\\FFFF0002\\screenshots\\capture.bmp", 0);
  EXPECT_EQ(notification.body(), "E:\\UDATA\\FFFF0002\\screenshots\\capture.bmp");
  notification.finish(false, "Screenshot failed: Disk full", 0);
  EXPECT_EQ(notification.body(), "Disk full");
  notification.finish(false, "Cannot start capture", 0);
  EXPECT_EQ(notification.body(), "Cannot start capture");
}
