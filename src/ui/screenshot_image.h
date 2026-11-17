/**
 * @file src/ui/screenshot_image.h
 * @brief Validates capture bitmaps before allocation and fits them without cropping.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string_view>

// local includes
#include "src/ui/shell_layout.h"

namespace ui {
  /**
   * @brief Dimensions of a supported uncompressed screenshot bitmap.
   */
  struct ScreenshotImageSize {
    int width = 0;  ///< Pixel width; zero for an unsupported or invalid bitmap.
    int height = 0;  ///< Pixel height; zero for an unsupported or invalid bitmap.
  };

  /**
   * @brief Validate a BMP header and file length before SDL allocates its image surface.
   * @param header First 54 bytes of the bitmap.
   * @param fileSize Total bytes available, including pixel data.
   * @return Supported dimensions, or zeroes for damaged, oversized or unsupported images.
   */
  ScreenshotImageSize screenshot_bitmap_size(std::string_view header, std::uint64_t fileSize);
  /**
   * @brief Center an entire screenshot in the available square-pixel UI area.
   * @param image Source pixel dimensions.
   * @param area Available rectangle before display encoder correction.
   * @return Aspect-preserving image bounds, or an empty rectangle for invalid inputs.
   */
  ShellRect fit_screenshot(ScreenshotImageSize image, ShellRect area);
}  // namespace ui
