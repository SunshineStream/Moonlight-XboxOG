/**
 * @file src/streaming/video_acceleration.cpp
 * @brief Implements safe overlay packing and stream acceleration diagnostics.
 */
#include "src/streaming/video_acceleration.h"

#if defined(__SSE__) && (defined(__clang__) || defined(__GNUC__))
  #include <xmmintrin.h>

namespace {
  constexpr int PACK_BLOCK_PAIRS = 16;  ///< YUY2 pairs staged for wide stores.
  constexpr int PACK_BLOCK_PIXELS = PACK_BLOCK_PAIRS * 2;  ///< Luma pixels covered by one block.

  /**
   * @brief Pack thirty-two pixels into YUY2 and copy them with wide SSE1 stores.
   *
   * Assemble the pixel words in cached storage so the writes to PVIDEO's
   * write-combined buffer can use 16-byte stores. The SSE operations copy
   * the word bits without floating-point conversion.
   *
   * @param y Thirty-two accessible luma bytes.
   * @param u Sixteen accessible blue chroma bytes.
   * @param v Sixteen accessible red chroma bytes.
   * @param output Sixty-four accessible destination bytes, possibly unaligned.
   */
  inline void pack_yuy2_block(const std::uint8_t *y, const std::uint8_t *u, const std::uint8_t *v, std::uint8_t *output) {
    alignas(16) std::array<float, PACK_BLOCK_PAIRS> packed;
    for (int pair = 0; pair < PACK_BLOCK_PAIRS; ++pair) {
      std::uint16_t luma;
      __builtin_memcpy(&luma, y + pair * 2, sizeof(luma));
      const auto lumaWord = static_cast<std::uint32_t>(luma);
      const auto lumaBytes = (lumaWord | (lumaWord << 8U)) & 0x00ff00ffU;
      const auto pixels = lumaBytes | (static_cast<std::uint32_t>(u[pair]) << 8U) | (static_cast<std::uint32_t>(v[pair]) << 24U);
      __builtin_memcpy(&packed[pair], &pixels, sizeof(pixels));
    }
    constexpr int PAIRS_PER_VECTOR = sizeof(__m128) / sizeof(float);
    for (int pair = 0; pair < PACK_BLOCK_PAIRS; pair += PAIRS_PER_VECTOR) {
      const auto pixels = _mm_load_ps(packed.data() + pair);
      // A fixed-size builtin copy allows an unaligned vector store even in nxdk's freestanding build.
      __builtin_memcpy(output + pair * sizeof(std::uint32_t), &pixels, sizeof(pixels));
    }
  }
}  // namespace
#endif

namespace streaming {
  std::string overlay_frame_error(const Yuv420FrameView &frame) {
    if (frame.width < 2 || frame.width > 2046 || frame.height < 2 || frame.height > 2047 || (frame.width & 1) != 0) {
      return "Frame dimensions are outside the GPU overlay limits";
    }
    if (frame.fullRange || frame.matrix == VideoColorMatrix::unsupported || frame.interlaced) {
      return "Source color range, matrix or interlacing requires software conversion";
    }
    for (std::size_t plane = 0; plane < frame.planes.size(); ++plane) {
      const int width = plane == 0 ? frame.width : frame.width / 2;
      const int height = plane == 0 ? frame.height : (frame.height + 1) / 2;
      if (frame.planes[plane] == nullptr || frame.pitches[plane] < width || frame.sizes[plane] < static_cast<std::uint64_t>(height - 1) * static_cast<unsigned int>(frame.pitches[plane]) + static_cast<unsigned int>(width)) {
        return "Source plane storage is invalid";
      }
    }
    return {};
  }

  bool pack_overlay_yuy2(const Yuv420FrameView &frame, std::uint8_t *destination, std::size_t capacity, int pitch) {
    const auto *luma = frame.planes[0];
    const auto *blueChroma = frame.planes[1];
    const auto *redChroma = frame.planes[2];
    if (luma == nullptr || blueChroma == nullptr || redChroma == nullptr) {
      return false;
    }
    if (!overlay_frame_error(frame).empty() || destination == nullptr || pitch < frame.width * 2 || capacity < static_cast<std::uint64_t>(frame.height - 1) * static_cast<unsigned int>(pitch) + static_cast<unsigned int>(frame.width * 2)) {
      return false;
    }
    for (int row = 0; row < frame.height; ++row) {
      const auto *y = luma + static_cast<std::size_t>(row) * frame.pitches[0];
      const auto *u = blueChroma + static_cast<std::size_t>(row / 2) * frame.pitches[1];
      const auto *v = redChroma + static_cast<std::size_t>(row / 2) * frame.pitches[2];
      auto *output = destination + static_cast<std::size_t>(row) * pitch;
      int column = 0;
#if defined(__SSE__) && (defined(__clang__) || defined(__GNUC__))
      for (; column <= frame.width - PACK_BLOCK_PIXELS; column += PACK_BLOCK_PIXELS) {
        pack_yuy2_block(y + column, u + column / 2, v + column / 2, output + column * 2);
      }
#endif
      for (; column < frame.width; column += 2) {
        output[column * 2] = y[column];
        output[column * 2 + 1] = u[column / 2];
        output[column * 2 + 2] = y[column + 1];
        output[column * 2 + 3] = v[column / 2];
      }
    }
    return true;
  }

  void VideoAccelerationReport::record_fallback(std::string_view reason) {
    if (fallbackReason.empty()) {
      fallbackReason = reason;
    }
  }

  std::vector<std::string> VideoAccelerationReport::summary_lines() const {
    const char *presentation = "GPU presentation: no software fallback";
    if (!fallbackReason.empty()) {
      presentation = "SOFTWARE FALLBACK USED";
    } else if (gpuFrames == 0 && softwareFrames == 0) {
      presentation = "No video frames were presented";
    } else if (!gpuPresentationRequested) {
      presentation = "Software presentation selected";
    }
    std::vector<std::string> lines {
      presentation,
      "GPU overlay: " + std::to_string(gpuFrames) + " frames | Software: " + std::to_string(softwareFrames) + " frames",
    };
    if (!fallbackReason.empty()) {
      lines.push_back("Reason: " + fallbackReason);
    }
    lines.push_back("FFmpeg: " + cpuFeatures + (threaded ? " | Threading enabled" : " | THREADING DISABLED"));
    lines.emplace_back("Video decoding: CPU");
    return lines;
  }
}  // namespace streaming
