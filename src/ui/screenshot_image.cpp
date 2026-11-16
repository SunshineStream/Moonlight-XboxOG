/**
 * @file src/ui/screenshot_image.cpp
 * @brief Implements bounded BMP validation and aspect-preserving screenshot layout.
 */
// class header include
#include "src/ui/screenshot_image.h"

// standard includes
#include <algorithm>

namespace {
  /**
   * @brief Read one little-endian unsigned header field without unaligned pointer casts.
   * @param header Already length-checked BMP header.
   * @param offset First field byte.
   * @param count Number of field bytes, at most four.
   * @return Decoded unsigned value.
   */
  std::uint32_t little_endian(std::string_view header, std::size_t offset, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
      value |= static_cast<std::uint32_t>(static_cast<unsigned char>(header[offset + i])) << (8U * i);
    }
    return value;
  }
}  // namespace

namespace ui {
  ScreenshotImageSize screenshot_bitmap_size(std::string_view header, std::uint64_t fileSize) {
    if (header.size() < 54 || header.substr(0, 2) != "BM" || fileSize > 9U * 1024U * 1024U) {
      return {};
    }
    const auto offset = little_endian(header, 10, 4);
    const auto dibSize = little_endian(header, 14, 4);
    const auto width = little_endian(header, 18, 4);
    const auto height = little_endian(header, 22, 4);
    const auto planes = little_endian(header, 26, 2);
    const auto bits = little_endian(header, 28, 2);
    const auto compression = little_endian(header, 30, 4);
    // Captures are 24-bit BI_RGB. Also accept ordinary 32-bit BI_RGB files, with
    // a two-megapixel ceiling so a damaged header cannot exhaust the Xbox heap.
    if (dibSize < 40 || dibSize > 124 || offset < 14U + dibSize || width == 0 || width > 1920 || height == 0 || height > 1080 || planes != 1 || (bits != 24 && bits != 32) || compression != 0) {
      return {};
    }
    const std::uint64_t stride = (static_cast<std::uint64_t>(width) * bits + 31U) / 32U * 4U;
    if (offset + stride * height > fileSize) {
      return {};
    }
    return {static_cast<int>(width), static_cast<int>(height)};
  }

  ShellRect fit_screenshot(ScreenshotImageSize image, ShellRect area) {
    if (image.width <= 0 || image.height <= 0 || area.w <= 0 || area.h <= 0) {
      return {};
    }
    const bool fitWidth = static_cast<std::int64_t>(image.width) * area.h > static_cast<std::int64_t>(image.height) * area.w;
    const int width = fitWidth ? area.w : std::max(1, static_cast<int>(static_cast<std::int64_t>(image.width) * area.h / image.height));
    const int height = fitWidth ? std::max(1, static_cast<int>(static_cast<std::int64_t>(image.height) * area.w / image.width)) : area.h;
    return {area.x + (area.w - width) / 2, area.y + (area.h - height) / 2, width, height};
  }
}  // namespace ui
