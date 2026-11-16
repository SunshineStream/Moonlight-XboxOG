/**
 * @file tests/unit/ui/asset_path_test.cpp
 * @brief Verifies that shell and streaming assets use valid Xbox SDL paths.
 */
// test includes
#include <lizardbyte/common/testing.h>

// local includes
#include "src/ui/asset_path.h"

/**
 * @brief Existing shell paths and portable menu paths resolve to the same Xbox file.
 */
TEST(AssetPathTest, NormalizesShellAndStreamingSeparators) {
  EXPECT_EQ(ui::build_asset_path("icons/keyboard.svg"), "D:\\assets\\icons\\keyboard.svg");
  EXPECT_EQ(ui::build_asset_path("icons\\keyboard.svg"), ui::build_asset_path("icons/keyboard.svg"));
  EXPECT_EQ(ui::build_asset_path("icons/subdirectory\\button.svg"), "D:\\assets\\icons\\subdirectory\\button.svg");
  EXPECT_EQ(ui::build_asset_path("fonts\\vegur-regular.ttf"), "D:\\assets\\fonts\\vegur-regular.ttf");
}
