/**
 * @file src/streaming/video_acceleration.cpp
 * @brief Implements safe overlay packing and stream acceleration diagnostics.
 */
#include "src/streaming/video_acceleration.h"

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
#if defined(__MMX__) && (defined(__clang__) || defined(__GNUC__))
    constexpr int PACK_BLOCK_PIXELS = 16;
#endif
    for (int row = 0; row < frame.height; ++row) {
      const auto *y = luma + static_cast<std::size_t>(row) * frame.pitches[0];
      const auto *u = blueChroma + static_cast<std::size_t>(row / 2) * frame.pitches[1];
      const auto *v = redChroma + static_cast<std::size_t>(row / 2) * frame.pitches[2];
      auto *output = destination + static_cast<std::size_t>(row) * pitch;
      int column = 0;
#if defined(__MMX__) && (defined(__clang__) || defined(__GNUC__))
      for (; column <= frame.width - PACK_BLOCK_PIXELS; column += PACK_BLOCK_PIXELS) {
        const int packedColumn = column * 2;
        // Interleave sixteen luma samples and eight U/V pairs. Wide stores
        // speed up writes to PVIDEO's write-combined buffers, even on
        // unaligned rows. The memory clobber covers all accessed bytes.
        asm volatile(
          "movq (%[u]), %%mm2\n\t"
          "movq (%[v]), %%mm3\n\t"
          "movq %%mm2, %%mm4\n\t"
          "punpcklbw %%mm3, %%mm2\n\t"
          "punpckhbw %%mm3, %%mm4\n\t"
          "movq (%[y]), %%mm0\n\t"
          "movq 8(%[y]), %%mm1\n\t"
          "movq %%mm0, %%mm3\n\t"
          "movq %%mm1, %%mm5\n\t"
          "punpcklbw %%mm2, %%mm0\n\t"
          "punpckhbw %%mm2, %%mm3\n\t"
          "punpcklbw %%mm4, %%mm1\n\t"
          "punpckhbw %%mm4, %%mm5\n\t"
          "movq %%mm0, %[out0]\n\t"
          "movq %%mm3, %[out1]\n\t"
          "movq %%mm1, %[out2]\n\t"
          "movq %%mm5, %[out3]"
          : [out0] "=m"(output[packedColumn]), [out1] "=m"(output[packedColumn + 8]), [out2] "=m"(output[packedColumn + 16]), [out3] "=m"(output[packedColumn + 24])
          : [y] "r"(y + column), [u] "r"(u + column / 2), [v] "r"(v + column / 2)
          : "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "memory"
        );
      }
#endif
      for (; column < frame.width; column += 2) {
        output[column * 2] = y[column];
        output[column * 2 + 1] = u[column / 2];
        output[column * 2 + 2] = y[column + 1];
        output[column * 2 + 3] = v[column / 2];
      }
    }
#if defined(__MMX__) && (defined(__clang__) || defined(__GNUC__))
    if (frame.width >= PACK_BLOCK_PIXELS) {
      // MMX aliases the x87 stack. Mark its registers empty before returning.
      asm volatile("emms");
    }
#endif
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
