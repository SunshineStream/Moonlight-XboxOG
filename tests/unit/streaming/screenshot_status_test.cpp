/**
 * @file tests/unit/streaming/screenshot_status_test.cpp
 * @brief Verifies persistent save progress and timed screenshot completion feedback.
 */
// test includes
#include <lizardbyte/common/testing.h>

// standard includes
#include <cstdint>
#include <limits>

// local includes
#include "src/streaming/screenshot_status.h"

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
