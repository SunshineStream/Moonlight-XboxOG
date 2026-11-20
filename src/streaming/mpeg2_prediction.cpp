/**
 * @file src/streaming/mpeg2_prediction.cpp
 * @brief Implements an integer reference for the experimental MPEG-2 prediction stage.
 */
#include "src/streaming/mpeg2_prediction.h"

#include <algorithm>

namespace {
  /**
   * @brief Round a signed half-pixel coordinate toward negative infinity.
   *
   * @param value Half-pixel coordinate.
   *
   * @return Integer sample position.
   */
  std::int64_t floor_half(std::int64_t value) {
    return value >= 0 ? value / 2 : (value - 1) / 2;
  }
}  // namespace

namespace streaming {
  bool predict_mpeg2_luma(const Mpeg2LumaPlane &reference, int xHalfPel, int yHalfPel, const Mpeg2LumaBlock &destination) {
    if (reference.data == nullptr || reference.width < 1 || reference.height < 1 || reference.pitch < reference.width || reference.size < static_cast<std::uint64_t>(reference.height - 1) * static_cast<unsigned int>(reference.pitch) + static_cast<unsigned int>(reference.width) || destination.data == nullptr || destination.width < 1 || destination.height < 1 || destination.width > 128 || destination.height > 128 || destination.pitch < destination.width || destination.size < static_cast<std::uint64_t>(destination.height - 1) * static_cast<unsigned int>(destination.pitch) + static_cast<unsigned int>(destination.width)) {
      return false;
    }
    for (int row = 0; row < destination.height; ++row) {
      const auto sy = static_cast<std::int64_t>(yHalfPel) + row * 2;
      const auto iy = floor_half(sy);
      const bool halfY = sy != iy * 2;
      for (int column = 0; column < destination.width; ++column) {
        const auto sx = static_cast<std::int64_t>(xHalfPel) + column * 2;
        const auto ix = floor_half(sx);
        const bool halfX = sx != ix * 2;
        const auto sample = [&](std::int64_t x, std::int64_t y) {
          return static_cast<unsigned int>(reference.data[static_cast<std::size_t>(std::clamp<std::int64_t>(y, 0, reference.height - 1)) * reference.pitch + static_cast<std::size_t>(std::clamp<std::int64_t>(x, 0, reference.width - 1))]);
        };
        unsigned int value = sample(ix, iy);
        if (halfX && halfY) {
          value = (value + sample(ix + 1, iy) + sample(ix, iy + 1) + sample(ix + 1, iy + 1) + 2U) / 4U;
        } else if (halfX) {
          value = (value + sample(ix + 1, iy) + 1U) / 2U;
        } else if (halfY) {
          value = (value + sample(ix, iy + 1) + 1U) / 2U;
        }
        destination.data[static_cast<std::size_t>(row) * destination.pitch + column] = static_cast<std::uint8_t>(value);
      }
    }
    return true;
  }

  void add_mpeg2_residual(std::uint8_t *prediction, const std::int16_t *residual, std::size_t count) {
    if (prediction == nullptr || residual == nullptr) {
      return;
    }
    for (std::size_t index = 0; index < count; ++index) {
      prediction[index] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(prediction[index]) + residual[index], 0, 255));
    }
  }
}  // namespace streaming
