/**
 * @file src/streaming/video_overlay.cpp
 * @brief Converts cached UI panels for GPU scanout and software framebuffers.
 */
// class header include
#include "src/streaming/video_overlay.h"

// standard includes
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {
  /**
   * @brief Validate destination storage before any panel copy.
   * @param pixels Destination pointer.
   * @param capacity Accessible bytes.
   * @param pitch Byte stride.
   * @param width Pixel width.
   * @param height Pixel height.
   * @param bytes Bytes per pixel.
   * @return True when every row fits its storage.
   */
  bool valid_storage(const std::uint8_t *pixels, std::size_t capacity, int pitch, int width, int height, int bytes) {
    return pixels != nullptr && width > 0 && height > 0 && bytes > 0 && pitch > 0 && static_cast<std::int64_t>(pitch) >= static_cast<std::int64_t>(width) * bytes &&
           capacity >= static_cast<std::uint64_t>(height - 1) * pitch + static_cast<std::uint64_t>(width) * bytes;
  }

  /**
   * @brief Blend one component with exact transparent and opaque endpoints.
   * @param background Existing component.
   * @param foreground UI component.
   * @param opacity UI coverage in the range 0 through 255.
   * @return Rounded composite component.
   */
  std::uint8_t blend_byte(unsigned background, unsigned foreground, unsigned opacity) {
    return static_cast<std::uint8_t>((foreground * opacity + background * (255 - opacity) + 127) / 255);
  }

  /**
   * @brief Blend a grayscale pixel into a packed Xbox RGB framebuffer.
   * @param pixel Writable destination pixel.
   * @param bpp Validated RGB depth: 15, 16 or 32.
   * @param foreground Straight foreground ARGB color.
   * @param opacity Nonzero UI coverage.
   */
  void blend_rgb_pixel(std::uint8_t *pixel, int bpp, std::uint32_t foreground, unsigned opacity) {
    std::uint32_t packed = 0;
    const int bytes = bpp == 32 ? 4 : 2;
    std::memcpy(&packed, pixel, bytes);
    unsigned red = 0;
    unsigned green = 0;
    unsigned blue = 0;
    if (bpp == 32) {
      red = (packed >> 16) & 255;
      green = (packed >> 8) & 255;
      blue = packed & 255;
    } else {
      red = (packed >> (bpp == 16 ? 11 : 10)) & 31;
      green = (packed >> 5) & (bpp == 16 ? 63 : 31);
      blue = packed & 31;
      red = (red << 3) | (red >> 2);
      green = bpp == 16 ? (green << 2) | (green >> 4) : (green << 3) | (green >> 2);
      blue = (blue << 3) | (blue >> 2);
    }
    red = blend_byte(red, (foreground >> 16) & 255, opacity);
    green = blend_byte(green, (foreground >> 8) & 255, opacity);
    blue = blend_byte(blue, foreground & 255, opacity);
    if (bpp == 32) {
      packed = 0xff000000U | (red << 16) | (green << 8) | blue;
    } else if (bpp == 16) {
      packed = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
    } else {
      packed = 0x8000U | ((red >> 3) << 10) | ((green >> 3) << 5) | (blue >> 3);
    }
    std::memcpy(pixel, &packed, bytes);
  }

  /**
   * @brief Encode an original UI color in the host video's limited-range YUV matrix.
   * @param color Straight ARGB source pixel.
   * @param matrix Validated BT.601 or BT.709 matrix.
   * @return Luma, U and V components.
   */
  std::array<std::uint8_t, 3> rgb_to_yuv(std::uint32_t color, streaming::VideoColorMatrix matrix) {
    const int red = (color >> 16) & 255;
    const int green = (color >> 8) & 255;
    const int blue = color & 255;
    if (red == green && green == blue) {
      return {static_cast<std::uint8_t>(16 + (red * 219 + 127) / 255), 128, 128};
    }
    // 16-bit fixed-point RGB-to-limited-YUV coefficients, using the same
    // BT.601/709 primaries and 219/224 ranges as FFmpeg's colorspace conversion.
    constexpr std::array<int, 9> bt601 {16829, 33039, 6416, -9714, -19071, 28784, 28784, -24103, -4681};
    constexpr std::array<int, 9> bt709 {11966, 40254, 4064, -6596, -22189, 28784, 28784, -26145, -2639};
    const auto &coefficients = matrix == streaming::VideoColorMatrix::bt709 ? bt709 : bt601;
    std::array<std::uint8_t, 3> result {};
    for (int component = 0; component < 3; ++component) {
      const int offset = component == 0 ? 16 : 128;
      const int value = offset * 65536 + coefficients[component * 3] * red + coefficients[component * 3 + 1] * green + coefficients[component * 3 + 2] * blue;
      result[component] = std::clamp((value + 32768) / 65536, 0, 255);
    }
    return result;
  }

  /**
   * @brief Bound changed ARGB rows and merge damage not yet presented.
   * @param before Previous validated bitmap.
   * @param after Replacement bitmap with matching geometry.
   * @param width Bitmap width.
   * @param height Bitmap height.
   * @param pending Previously accumulated damage.
   * @return Union of changed bounds and pending damage, or an empty rectangle for invalid storage.
   */
  streaming::VideoRectangle argb_damage(const std::vector<std::uint8_t> &before, const std::vector<std::uint8_t> &after, int width, int height, const streaming::VideoRectangle &pending) {
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) {
      return {};
    }
    if (const auto expected = static_cast<std::size_t>(width) * height * 4; before.size() != expected || after.size() != expected) {
      return {};
    }
    int left = width;
    int right = 0;
    int top = height;
    int bottom = 0;
    for (int row = 0; row < height; ++row) {
      const auto offset = static_cast<std::size_t>(row) * width * 4;
      if (std::memcmp(before.data() + offset, after.data() + offset, static_cast<std::size_t>(width) * 4) == 0) {
        continue;
      }
      int first = 0;
      while (first < width && std::memcmp(before.data() + offset + first * 4, after.data() + offset + first * 4, 4) == 0) {
        ++first;
      }
      int last = width - 1;
      while (last > first && std::memcmp(before.data() + offset + last * 4, after.data() + offset + last * 4, 4) == 0) {
        --last;
      }
      left = std::min(left, first);
      right = std::max(right, last + 1);
      top = std::min(top, row);
      bottom = row + 1;
    }
    if (pending.width > 0 && pending.height > 0) {
      left = std::min(left, pending.x);
      right = std::max(right, pending.x + pending.width);
      top = std::min(top, pending.y);
      bottom = std::max(bottom, pending.y + pending.height);
    }
    return {left, top, right - left, bottom - top};
  }

}  // namespace

namespace streaming {
  std::vector<std::uint8_t> extract_overlay_bitmap(const std::uint8_t *pixels, std::size_t capacity, int pitch, int width, int height, bool opaqueBackground, std::vector<std::uint8_t> bitmap) {
    if (!valid_storage(pixels, capacity, pitch, width, height, 4) || width > 2048 || height > 2048) {
      return {};
    }
    bitmap.resize(static_cast<std::size_t>(width) * height * 4);
    for (int row = 0; row < height; ++row) {
      for (int column = 0; column < width; ++column) {
        const auto *pixel = pixels + static_cast<std::size_t>(row) * pitch + column * 4;
        const unsigned opacity = pixel[3];
        const auto index = (static_cast<std::size_t>(row) * width + column) * 4;
        bitmap[index + 3] = opacity;
        if (opaqueBackground || opacity == 255 || (pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0)) {
          // nxdk is freestanding: a four-byte std::memcpy becomes a library
          // call for every pixel. Scalar byte accesses also permit unaligned
          // SDL rows while allowing native compilers to combine the loads.
          bitmap[index] = pixel[0];
          bitmap[index + 1] = pixel[1];
          bitmap[index + 2] = pixel[2];
          // Legacy SDL blitters can round an opaque destination alpha to 254.
          // The OSK is already composited onto its solid panel; keep it opaque.
          bitmap[index + 3] = opaqueBackground ? 255 : opacity;
          continue;
        }
        if (opacity == 0) {
          bitmap[index] = 0;
          bitmap[index + 1] = 0;
          bitmap[index + 2] = 0;
          continue;
        }
        for (int component = 0; component < 3; ++component) {
          // SDL composites onto transparent black using premultiplied color.
          // Undo that once, preserving each original SVG color for every path.
          bitmap[index + component] = std::min(255U, (pixel[component] * 255 + opacity / 2) / opacity);
        }
      }
    }
    return bitmap;
  }

  /**
   * @brief Validate a wheel's destination, geometry and optional repaint bounds.
   * @param target Writable ARGB bitmap.
   * @param wheel Wheel geometry and selection.
   * @param sector Requested sector, or minus one for the whole ring.
   * @param clip Optional repaint rectangle.
   * @return True when rasterization remains within the destination.
   */
  bool valid_menu_wheel(const OverlayBitmap &target, const MenuWheel &wheel, int sector, const VideoRectangle *clip) {
    const auto &[pixels, capacity, pitch, width, height] = target;
    const auto &[centerX, centerY, radius, selected] = wheel;

    if (!valid_storage(pixels, capacity, pitch, width, height, 4) || width > 2048 || height > 2048 || centerX < 0 || centerX >= width || centerY < 0 || centerY >= height || radius < 1 || radius > 2048 || sector < -1 || sector > 3) {
      return false;
    }
    if (clip != nullptr && (clip->x < 0 || clip->y < 0 || clip->width <= 0 || clip->height <= 0 || clip->width > width || clip->height > height || clip->x > width - clip->width || clip->y > height - clip->height)) {
      return false;
    }
    return true;
  }

  /**
   * @brief Bound a single sector's columns on one wheel row.
   * @param wheel Wheel geometry.
   * @param y Row displacement from its center.
   * @param first First visible column.
   * @param last Last visible column.
   * @param sector Requested sector, or minus one for the whole ring.
   * @return Inclusive clipped column bounds.
   */
  std::pair<int, int> wheel_columns(const MenuWheel &wheel, int y, int first, int last, int sector) {
    if (sector == 0 || sector == 2) {
      return {std::max(first, wheel.centerX - std::abs(y)), std::min(last, wheel.centerX + std::abs(y))};
    }
    if (sector == 1) {
      first = std::max(first, wheel.centerX + std::abs(y) + 1);
    }
    if (sector == 3) {
      last = std::min(last, wheel.centerX - std::abs(y) - 1);
    }
    return {first, last};
  }

  /**
   * @brief Resolve a pixel's clockwise wheel sector with vertical diagonal ties.
   * @param x Horizontal displacement.
   * @param y Vertical displacement, positive downward.
   * @return Up, right, down or left as zero through three.
   */
  int wheel_sector(int x, int y) {
    if (std::abs(y) >= std::abs(x)) {
      return y < 0 ? 0 : 2;
    }
    return x > 0 ? 1 : 3;
  }

  /**
   * @brief Choose the separator or normal/selected wheel shade.
   * @param x Horizontal displacement.
   * @param y Vertical displacement.
   * @param direction Pixel sector.
   * @param selected Highlighted sector.
   * @return Opaque grayscale component.
   */
  unsigned wheel_shade(int x, int y, int direction, int selected) {
    if (std::abs(std::abs(x) - std::abs(y)) < 2) {
      return 110;
    }
    return direction == selected ? 85 : 36;
  }

  bool rasterize_menu_wheel(const OverlayBitmap &target, const MenuWheel &wheel, int sector, const VideoRectangle *clip) {
    const auto &[pixels, capacity, pitch, width, height] = target;
    const auto &[centerX, centerY, radius, selected] = wheel;
    if (!valid_menu_wheel(target, wheel, sector, clip)) {
      return false;
    }
    const int firstRow = std::max({0, centerY - radius, clip != nullptr ? clip->y : 0});
    const int lastRow = std::min({height - 1, centerY + radius, clip != nullptr ? clip->y + clip->height - 1 : height - 1});
    const int firstColumn = std::max({0, centerX - radius, clip != nullptr ? clip->x : 0});
    const int lastColumn = std::min({width - 1, centerX + radius, clip != nullptr ? clip->x + clip->width - 1 : width - 1});
    for (int row = firstRow; row <= lastRow; ++row) {
      const int y = row - centerY;
      if ((sector == 0 && y > 0) || (sector == 2 && y < 0)) {
        continue;
      }
      const auto [start, end] = wheel_columns(wheel, y, firstColumn, lastColumn, sector);
      for (int column = start; column <= end; ++column) {
        const int x = column - centerX;
        if (const int distance = x * x + y * y; distance > radius * radius || distance < radius * radius / 9) {
          continue;
        }
        const int direction = wheel_sector(x, y);
        if (sector >= 0 && sector != direction) {
          continue;
        }
        const auto gray = wheel_shade(x, y, direction, selected);
        auto *pixel = pixels + static_cast<std::size_t>(row) * pitch + column * 4;
        pixel[0] = gray;
        pixel[1] = gray;
        pixel[2] = gray;
        pixel[3] = 255;
      }
    }
    return true;
  }

  OverlayLabelLayout layout_overlay_label(int width, int height, const VideoRectangle &bounds, bool shrinkToFit) {
    if (width <= 0 || height <= 0 || bounds.width <= 0 || bounds.height <= 0) {
      return {};
    }
    OverlayLabelLayout layout {{0, 0, width, height}, bounds};
    if (shrinkToFit) {
      const double scale = std::min({1.0, static_cast<double>(std::max(1, bounds.width - 2)) / width, static_cast<double>(std::max(1, bounds.height - 2)) / height});
      layout.destination.width = std::max(1, static_cast<int>(width * scale));
      layout.destination.height = std::max(1, static_cast<int>(height * scale));
    } else {
      layout.source = {std::max(0, (width - bounds.width) / 2), std::max(0, (height - bounds.height) / 2), std::min(width, bounds.width), std::min(height, bounds.height)};
      layout.destination.width = layout.source.width;
      layout.destination.height = layout.source.height;
    }
    layout.destination.x += (bounds.width - layout.destination.width) / 2;
    layout.destination.y += (bounds.height - layout.destination.height) / 2;
    return layout;
  }

  void VideoOverlay::set_bitmap(int width, int height, std::vector<std::uint8_t> pixels, VideoOverlayPlacement placement, std::vector<std::uint8_t> opacity) {
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048 || pixels.size() != static_cast<std::size_t>(width) * height || (!opacity.empty() && opacity.size() != pixels.size())) {
      width = height = 0;
      pixels.clear();
      opacity.clear();
    }
    width_ = width;
    height_ = height;
    placement_ = placement;
    grayscale_ = std::move(pixels);
    opacity_ = std::move(opacity);
    std::vector<std::uint8_t>().swap(argb_);
    opaque_ = opacity_.empty() || std::all_of(opacity_.begin(), opacity_.end(), [](std::uint8_t opacity) {
                return opacity == 255;
              });
    invalidate_caches();
  }

  void VideoOverlay::set_argb_bitmap(int width, int height, std::vector<std::uint8_t> pixels, VideoOverlayPlacement placement) {
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048 || pixels.size() != static_cast<std::size_t>(width) * height * 4) {
      set_bitmap(0, 0, {});
      return;
    }
    const bool sameGeometry = width_ == width && height_ == height && placement_ == placement && !argb_.empty();
    if (sameGeometry && argb_ == pixels) {
      return;
    }
    bool opaque = true;
    for (std::size_t index = 3; index < pixels.size(); index += 4) {
      if (pixels[index] != 255) {
        opaque = false;
        break;
      }
    }
    const bool reuse = sameGeometry && opaque == opaque_;
    VideoRectangle changed {};
    if (reuse) {
      changed = argb_damage(argb_, pixels, width, height, damage_);
    }
    width_ = width;
    height_ = height;
    placement_ = placement;
    argb_ = std::move(pixels);
    std::vector<std::uint8_t>().swap(grayscale_);
    std::vector<std::uint8_t>().swap(opacity_);
    opaque_ = opaque;
    if (reuse) {
      rgb_.clear();
      rgbBpp_ = 0;
      damage_ = changed;
    } else {
      invalidate_caches();
    }
  }

  bool VideoOverlay::update_argb_bitmap(const VideoRectangle &region, const std::vector<std::uint8_t> &pixels) {
    if (argb_.empty() || region.x < 0 || region.y < 0 || region.width <= 0 || region.height <= 0 || region.width > width_ || region.height > height_ || region.x > width_ - region.width || region.y > height_ - region.height || pixels.size() != static_cast<std::size_t>(region.width) * region.height * 4) {
      return false;
    }
    for (int row = 0; row < region.height; ++row) {
      std::memcpy(argb_.data() + (static_cast<std::size_t>(region.y + row) * width_ + region.x) * 4, pixels.data() + static_cast<std::size_t>(row) * region.width * 4, static_cast<std::size_t>(region.width) * 4);
    }
    if (opaque_) {
      for (std::size_t index = 3; index < pixels.size(); index += 4) {
        if (pixels[index] != 255) {
          opaque_ = false;
          // Cached opaque YUY2 storage has no alpha plane. Rebuild it before
          // accepting a patch that introduces transparency.
          invalidate_caches();
          break;
        }
      }
    }
    if (damage_.width > 0 && damage_.height > 0) {
      const int right = std::max(damage_.x + damage_.width, region.x + region.width);
      const int bottom = std::max(damage_.y + damage_.height, region.y + region.height);
      damage_.x = std::min(damage_.x, region.x);
      damage_.y = std::min(damage_.y, region.y);
      damage_.width = right - damage_.x;
      damage_.height = bottom - damage_.y;
    } else {
      damage_ = region;
    }
    rgb_.clear();
    rgbBpp_ = 0;
    return true;
  }

  bool VideoOverlay::set_argb_surface(int width, int height, const std::uint8_t *pixels, std::size_t capacity, int pitch, VideoOverlayPlacement placement, bool opaqueBackground) {
    if (!valid_storage(pixels, capacity, pitch, width, height, 4) || width > 2048 || height > 2048) {
      return false;
    }
    if (width != width_ || height != height_) {
      set_bitmap(0, 0, {});
    }
    auto bitmap = extract_overlay_bitmap(pixels, capacity, pitch, width, height, opaqueBackground, std::move(argb_));
    set_argb_bitmap(width, height, std::move(bitmap), placement);
    return true;
  }

  std::uint32_t VideoOverlay::color_at(std::size_t index) const {
    if (!argb_.empty()) {
      const auto *pixel = argb_.data() + index * 4;
      return static_cast<std::uint32_t>(pixel[0]) | (static_cast<std::uint32_t>(pixel[1]) << 8) | (static_cast<std::uint32_t>(pixel[2]) << 16) | (static_cast<std::uint32_t>(pixel[3]) << 24);
    }
    const std::uint32_t opacity = opacity_.empty() ? 255 : opacity_[index];
    return (opacity << 24) | grayscale_[index] * 0x010101U;
  }

  void VideoOverlay::invalidate_caches() {
    damage_ = {0, 0, width_, height_};
    rgb_.clear();
    rgbBpp_ = 0;
    yuy2_.clear();
    yuy2Opacity_.clear();
    if (width_ == 0) {
      // Local menus must return their converted buffers to the 64 MiB console
      // budget when dismissed, rather than retaining full-screen capacities.
      std::vector<std::uint8_t>().swap(rgb_);
      std::vector<std::uint8_t>().swap(yuy2_);
      std::vector<std::uint8_t>().swap(yuy2Opacity_);
    }
    frameWidth_ = 0;
  }

  int VideoOverlay::width() const {
    return width_;
  }

  int VideoOverlay::height() const {
    return height_;
  }

  VideoRectangle VideoOverlay::rectangle(const VideoRectangle &destination) const {
    const bool docked = placement_ == VideoOverlayPlacement::dock_bottom || placement_ == VideoOverlayPlacement::dock_top;
    const bool bottom = placement_ == VideoOverlayPlacement::bottom_center || placement_ == VideoOverlayPlacement::dock_bottom;
    const bool centered = bottom || docked;
    const int margin = docked || placement_ == VideoOverlayPlacement::full_screen ? 0 : MARGIN;
    const int width = std::min(width_, std::max(0, destination.width - margin * (centered ? 2 : 1)));
    const int height = std::min(height_, std::max(0, destination.height - margin * (centered ? 2 : 1)));
    if (width == 0 || height == 0) {
      return {};
    }
    return {destination.x + (centered ? (destination.width - width) / 2 : margin), destination.y + (bottom ? destination.height - height - margin : margin), width, height};
  }

  void VideoOverlay::prepare_rgb(int bpp) {
    if (rgbBpp_ == bpp || (bpp == 32 && !argb_.empty())) {
      return;
    }
    const int bytes = bpp == 32 ? 4 : 2;
    const auto count = static_cast<std::size_t>(width_) * height_;
    rgb_.resize(count * bytes);
    for (std::size_t i = 0; i < count; ++i) {
      const auto color = color_at(i);
      if (bpp == 32) {
        std::memcpy(rgb_.data() + i * 4, &color, 4);
      } else {
        const auto red = (color >> 16) & 255;
        const auto green = (color >> 8) & 255;
        const auto blue = color & 255;
        const auto packed = static_cast<std::uint16_t>(bpp == 16 ? ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3) : 0x8000U | ((red >> 3) << 10) | ((green >> 3) << 5) | (blue >> 3));
        std::memcpy(rgb_.data() + i * 2, &packed, 2);
      }
    }
    rgbBpp_ = bpp;
  }

  const std::vector<std::uint8_t> &VideoOverlay::argb_pixels() {
    if (!argb_.empty()) {
      return argb_;
    }
    prepare_rgb(32);
    return rgb_;
  }

  bool VideoOverlay::draw_rgb(std::uint8_t *pixels, std::size_t capacity, int pitch, int width, int height, int bpp, const VideoRectangle &destination) {
    if (width_ == 0) {
      return true;
    }
    const int bytes = bpp == 32 ? 4 : (bpp == 15 || bpp == 16 ? 2 : 0);
    if (!valid_storage(pixels, capacity, pitch, width, height, bytes) || destination.x < 0 || destination.y < 0 || destination.width <= MARGIN || destination.height <= MARGIN || destination.x > width - destination.width || destination.y > height - destination.height) {
      return false;
    }
    prepare_rgb(bpp);
    const auto &converted = bpp == 32 && !argb_.empty() ? argb_ : rgb_;
    const auto bounds = rectangle(destination);
    const std::size_t columnBytes = static_cast<std::size_t>(bounds.x) * bytes;
    const std::size_t copyBytes = static_cast<std::size_t>(bounds.width) * bytes;
    for (int row = 0; row < bounds.height; ++row) {
      const std::size_t framebufferRow = static_cast<std::size_t>(bounds.y) + row;
      auto *destinationRow = pixels + framebufferRow * pitch + columnBytes;
      const auto bitmapRow = static_cast<std::size_t>(row) * width_;
      if (opaque_) {
        std::memcpy(destinationRow, converted.data() + bitmapRow * bytes, copyBytes);
        continue;
      }
      {
        for (int column = 0; column < bounds.width; ++column) {
          const auto index = bitmapRow + column;
          const auto color = color_at(index);
          const auto opacity = color >> 24;
          if (opacity == 255) {
            std::memcpy(destinationRow + column * bytes, converted.data() + index * bytes, bytes);
          } else if (opacity != 0) {
            blend_rgb_pixel(destinationRow + column * bytes, bpp, color, opacity);
          }
        }
      }
    }
    return true;
  }

  bool VideoOverlay::prepare_yuy2(int frameWidth, int frameHeight, const VideoRectangle &destination, VideoColorMatrix matrix, bool &rebuild) {
    rebuild = frameWidth_ != frameWidth || frameHeight_ != frameHeight || outputWidth_ != destination.width || outputHeight_ != destination.height || matrix_ != matrix;
    if (rebuild) {
      matrix_ = matrix;
      frameWidth_ = frameWidth;
      frameHeight_ = frameHeight;
      outputWidth_ = destination.width;
      outputHeight_ = destination.height;
      const auto bounds = rectangle({0, 0, destination.width, destination.height});
      if (bounds.width == 0 || bounds.height == 0) {
        source_ = {};
        return false;
      }
      source_.x = (static_cast<int>(static_cast<std::int64_t>(bounds.x) * frameWidth / destination.width) + 1) & ~1;
      source_.y = static_cast<int>(static_cast<std::int64_t>(bounds.y) * frameHeight / destination.height);
      const int visibleWidth = bounds.width;
      const int visibleHeight = bounds.height;
      source_.width = std::min(frameWidth - source_.x, ((visibleWidth * frameWidth + destination.width - 1) / destination.width + 1) & ~1);
      source_.height = std::min(frameHeight - source_.y, (visibleHeight * frameHeight + destination.height - 1) / destination.height);
      if (source_.width <= 0 || source_.height <= 0) {
        return false;
      }
      yuy2_.resize(static_cast<std::size_t>(source_.width) * source_.height * 2);
      if (!opaque_) {
        yuy2Opacity_.resize(yuy2_.size() / 2);
      }
    }
    return true;
  }

  /**
   * @brief Weight shared chroma without allowing transparent pixels to tint their neighbors.
   * @param first First chroma component.
   * @param second Second chroma component.
   * @param firstAlpha First pixel coverage.
   * @param secondAlpha Second pixel coverage.
   * @return Coverage-weighted chroma, or neutral chroma for a transparent pair.
   */
  unsigned covered_chroma(unsigned first, unsigned second, unsigned firstAlpha, unsigned secondAlpha) {
    const unsigned coverage = firstAlpha + secondAlpha;
    if (coverage == 0) {
      return 128;
    }
    if (first == second) {
      return first;
    }
    return (first * firstAlpha + second * secondAlpha + coverage / 2) / coverage;
  }

  void VideoOverlay::convert_yuy2_pair(int row, int column, int bitmapRow, int firstBitmapColumn, int secondBitmapColumn, VideoColorMatrix matrix) {
    const auto firstColor = color_at(static_cast<std::size_t>(bitmapRow) * width_ + firstBitmapColumn);
    const auto secondColor = color_at(static_cast<std::size_t>(bitmapRow) * width_ + secondBitmapColumn);
    const auto first = rgb_to_yuv(firstColor, matrix);
    const auto second = rgb_to_yuv(secondColor, matrix);
    const unsigned firstAlpha = firstColor >> 24;
    const unsigned secondAlpha = secondColor >> 24;
    const auto offset = (static_cast<std::size_t>(row) * source_.width + column) * 2;
    yuy2_[offset] = first[0];
    yuy2_[offset + 2] = second[0];
    // Weight shared chroma by coverage so a transparent colored pixel
    // cannot tint its opaque neighbor or desaturate the host underneath.
    // Most wheel/key pixels are gray. Equal chroma needs no coverage
    // division, including antialiased text and partially dimmed video.
    yuy2_[offset + 1] = covered_chroma(first[1], second[1], firstAlpha, secondAlpha);
    yuy2_[offset + 3] = covered_chroma(first[2], second[2], firstAlpha, secondAlpha);
    if (!opaque_) {
      yuy2Opacity_[offset / 2] = firstAlpha;
      yuy2Opacity_[offset / 2 + 1] = secondAlpha;
    }
  }

  void VideoOverlay::update_yuy2(const VideoRectangle &destination, VideoColorMatrix matrix, bool rebuild) {
    const auto bounds = rectangle({0, 0, destination.width, destination.height});
    const int visibleWidth = bounds.width;
    const int visibleHeight = bounds.height;
    if (visibleWidth == 0 || visibleHeight == 0) {
      return;
    }
    const int firstRow = rebuild ? 0 : std::min(source_.height, damage_.y * source_.height / visibleHeight);
    const int lastRow = rebuild ? source_.height : std::min(source_.height, ((damage_.y + damage_.height) * source_.height + visibleHeight - 1) / visibleHeight);
    const int firstColumn = rebuild ? 0 : std::min(source_.width, (damage_.x * source_.width / visibleWidth) & ~1);
    const int lastColumn = rebuild ? source_.width : std::min(source_.width, ((((damage_.x + damage_.width) * source_.width + visibleWidth - 1) / visibleWidth) + 1) & ~1);
    const bool nativeWidth = visibleWidth == source_.width;
    for (int row = firstRow; row < lastRow; ++row) {
      const int bitmapRow = std::min(visibleHeight - 1, row * visibleHeight / source_.height);
      for (int column = firstColumn; column < lastColumn; column += 2) {
        const int firstBitmapColumn = nativeWidth ? column : std::min(visibleWidth - 1, column * visibleWidth / source_.width);
        const int secondBitmapColumn = nativeWidth ? column + 1 : std::min(visibleWidth - 1, (column + 1) * visibleWidth / source_.width);
        convert_yuy2_pair(row, column, bitmapRow, firstBitmapColumn, secondBitmapColumn, matrix);
      }
    }
  }

  void VideoOverlay::blend_yuy2(std::uint8_t *pixels, int pitch) {
    damage_ = {};

    if (source_.width <= 0 || source_.height <= 0) {
      return;
    }
    for (int row = 0; row < source_.height; ++row) {
      auto *destinationRow = pixels + static_cast<std::size_t>(source_.y + row) * pitch + source_.x * 2;
      const auto rowOffset = static_cast<std::size_t>(row) * source_.width;
      const auto *foreground = yuy2_.data() + rowOffset * 2;
      if (opaque_) {
        std::memcpy(destinationRow, foreground, source_.width * 2);
        continue;
      }
      {
        for (int column = 0; column < source_.width; column += 2) {
          const unsigned left = yuy2Opacity_[rowOffset + column];
          const unsigned right = yuy2Opacity_[rowOffset + column + 1];
          if (left == 0 && right == 0) {
            continue;
          }
          auto *pair = destinationRow + column * 2;
          if (left == 255 && right == 255) {
            pair[0] = foreground[column * 2];
            pair[1] = foreground[column * 2 + 1];
            pair[2] = foreground[column * 2 + 2];
            pair[3] = foreground[column * 2 + 3];
            continue;
          }
          pair[0] = blend_byte(pair[0], foreground[column * 2], left);
          pair[2] = blend_byte(pair[2], foreground[column * 2 + 2], right);
          // A YUY2 pair shares chroma: average its two luma coverages rather
          // than destroying the host color beside a transparent UI pixel.
          const unsigned chroma = (left + right + 1) / 2;
          pair[1] = blend_byte(pair[1], foreground[column * 2 + 1], chroma);
          pair[3] = blend_byte(pair[3], foreground[column * 2 + 3], chroma);
        }
      }
    }
  }

  bool VideoOverlay::draw_yuy2(std::uint8_t *pixels, std::size_t capacity, int pitch, int frameWidth, int frameHeight, const VideoRectangle &destination, VideoColorMatrix matrix) {
    if (width_ == 0) {
      return true;
    }
    if (!valid_storage(pixels, capacity, pitch, frameWidth, frameHeight, 2) || (frameWidth & 1) != 0 || frameWidth > 2046 || frameHeight > 2047 || destination.width <= MARGIN || destination.height <= MARGIN || matrix == VideoColorMatrix::unsupported) {
      return false;
    }
    bool rebuild = false;
    if (!prepare_yuy2(frameWidth, frameHeight, destination, matrix, rebuild)) {
      return true;
    }
    if (rebuild || (damage_.width > 0 && damage_.height > 0)) {
      update_yuy2(destination, matrix, rebuild);
    }
    blend_yuy2(pixels, pitch);
    return true;
  }
}  // namespace streaming
