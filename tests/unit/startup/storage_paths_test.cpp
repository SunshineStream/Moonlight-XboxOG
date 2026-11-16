/**
 * @file tests/unit/startup/storage_paths_test.cpp
 * @brief Verifies configured screenshot folders and the default save-data subdirectory.
 */
// test includes
#include <lizardbyte/common/testing.h>

// local includes
#include "src/startup/storage_paths.h"

/**
 * @brief Default screenshots live together under save data; custom Xbox and host separators are preserved.
 */
TEST(StoragePathsTest, ResolvesScreenshotFoldersAndTrailingSeparators) {
  EXPECT_EQ(startup::screenshot_storage_path("", "capture.bmp"), startup::default_storage_path("screenshots") + "/capture.bmp");
  EXPECT_EQ(startup::screenshot_storage_path("F:\\Screenshots", "capture.bmp"), "F:\\Screenshots\\capture.bmp");
  EXPECT_EQ(startup::screenshot_storage_path("F:\\Screenshots\\", "capture.bmp"), "F:\\Screenshots\\capture.bmp");
  EXPECT_EQ(startup::screenshot_storage_path("/tmp/screenshots", "capture.bmp"), "/tmp/screenshots/capture.bmp");
  EXPECT_EQ(startup::screenshot_storage_path("shots/", "capture.bmp"), "shots/capture.bmp");
}
