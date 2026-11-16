/**
 * @file tests/unit/ui/screenshot_image_test.cpp
 * @brief Verifies bounded capture decoding and aspect-preserving image placement.
 */
// test includes
#include <lizardbyte/common/testing.h>

// standard includes
#include <cstdint>
#include <limits>
#include <string>

// local includes
#include "src/ui/screenshot_image.h"

namespace {
  /**
   * @brief Write a little-endian BMP header field for decoder validation cases.
   * @param header Mutable test header.
   * @param offset Field position.
   * @param value Field value.
   * @param bytes Field size in bytes.
   */
  void header_field(std::string &header, std::size_t offset, std::uint32_t value, std::size_t bytes = 4) {
    for (std::size_t i = 0; i < bytes; ++i) {
      header[offset + i] = static_cast<char>(value >> (i * 8U));
    }
  }

  /**
   * @brief Construct the uncompressed 720p BMP header emitted by capture saving.
   * @return Header with 24-bit pixels, a single plane and ordinary RGB encoding.
   */
  std::string bitmap_header() {
    std::string header(54, '\0');
    header[0] = 'B';
    header[1] = 'M';
    header_field(header, 10, 54);
    header_field(header, 14, 40);
    header_field(header, 18, 1280);
    header_field(header, 22, 720);
    header_field(header, 26, 1, 2);
    header_field(header, 28, 24, 2);
    return header;
  }

  /**
   * @brief Valid captures load while malformed dimensions and truncated pixels are rejected before allocation.
   */
  TEST(ScreenshotImageTest, ValidatesBitmapDimensionsAndPixelStorage) {
    const auto original = bitmap_header();
    constexpr std::uint64_t size = 54 + 1280 * 720 * 3;
    auto dimensions = ui::screenshot_bitmap_size(original, size);
    EXPECT_EQ(dimensions.width, 1280);
    EXPECT_EQ(dimensions.height, 720);
    EXPECT_EQ(ui::screenshot_bitmap_size(original.substr(0, 53), size).width, 0);
    EXPECT_EQ(ui::screenshot_bitmap_size("XX" + original.substr(2), size).width, 0);
    EXPECT_EQ(ui::screenshot_bitmap_size(original, size - 1).width, 0);
    EXPECT_EQ(ui::screenshot_bitmap_size(original, 10 * 1024 * 1024).width, 0);
    for (const auto &[offset, value] : {std::pair {10U, 53U}, {14U, 39U}, {14U, 125U}, {18U, 0U}, {18U, 1921U}, {22U, 0U}, {22U, 1081U}, {22U, 0xffffffffU}, {26U, 2U}, {28U, 16U}, {30U, 1U}}) {
      auto broken = original;
      header_field(broken, offset, value, offset == 26 || offset == 28 ? 2 : 4);
      EXPECT_EQ(ui::screenshot_bitmap_size(broken, size).width, 0) << offset << ": " << value;
    }
    auto rgba = original;
    header_field(rgba, 28, 32, 2);
    EXPECT_EQ(ui::screenshot_bitmap_size(rgba, 54 + 1280 * 720 * 4).width, 1280);
    auto padded = original;
    header_field(padded, 18, 3);
    header_field(padded, 22, 2);
    EXPECT_EQ(ui::screenshot_bitmap_size(padded, 54 + 12 * 2).height, 2);
    EXPECT_EQ(ui::screenshot_bitmap_size(padded, 54 + 9 * 2).width, 0);
  }

  /**
   * @brief Landscape, portrait and extreme aspect ratios stay centered inside SD and HD viewer bounds.
   */
  TEST(ScreenshotImageTest, FitsWithoutCroppingOrDistortingAspectRatio) {
    auto rect = ui::fit_screenshot({1280, 720}, {10, 20, 620, 400});
    EXPECT_EQ(rect.x, 10);
    EXPECT_EQ(rect.w, 620);
    EXPECT_EQ(rect.h, 348);
    EXPECT_EQ(rect.y, 46);
    rect = ui::fit_screenshot({480, 640}, {10, 20, 620, 400});
    EXPECT_EQ(rect.x, 170);
    EXPECT_EQ(rect.w, 300);
    EXPECT_EQ(rect.y, 20);
    EXPECT_EQ(rect.h, 400);
    rect = ui::fit_screenshot({1280, 720}, {0, 0, 1280, 720});
    EXPECT_EQ(rect.x, 0);
    EXPECT_EQ(rect.y, 0);
    EXPECT_EQ(rect.w, 1280);
    EXPECT_EQ(rect.h, 720);
    EXPECT_EQ(ui::fit_screenshot({0, 1}, {0, 0, 20, 20}).w, 0);
    EXPECT_EQ(ui::fit_screenshot({1, 0}, {0, 0, 20, 20}).w, 0);
    EXPECT_EQ(ui::fit_screenshot({1, 1}, {0, 0, 0, 20}).w, 0);
    EXPECT_EQ(ui::fit_screenshot({1, 1}, {0, 0, 20, 0}).w, 0);
    const int largest = std::numeric_limits<int>::max();
    rect = ui::fit_screenshot({largest, 1}, {0, 0, 1280, 720});
    EXPECT_EQ(rect.w, 1280);
    EXPECT_EQ(rect.h, 1);
    rect = ui::fit_screenshot({1, largest}, {0, 0, 1280, 720});
    EXPECT_EQ(rect.w, 1);
    EXPECT_EQ(rect.h, 720);
  }
}  // namespace
