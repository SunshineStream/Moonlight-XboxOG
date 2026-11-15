/**
 * @file src/app/stream_bitrate.cpp
 * @brief Calculates stream bitrate defaults from resolution and frame rate.
 */
// class header include
#include "src/app/stream_bitrate.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>

namespace app {

  int default_stream_bitrate_kbps(int width, int height, int framerate) {
    // Keep the resolution factors and whole-Mbps rounding aligned with Moonlight Android:
    // https://github.com/moonlight-stream/moonlight-android/blob/master/app/src/main/java/com/limelight/preferences/PreferenceConfiguration.java
    constexpr std::array<int, 6> PIXEL_COUNTS {640 * 360, 854 * 480, 1280 * 720, 1920 * 1080, 2560 * 1440, 3840 * 2160};
    constexpr std::array<int, 6> BITRATES_MBPS {1, 2, 5, 10, 20, 40};
    const std::int64_t pixels = static_cast<std::int64_t>(std::max(width, 0)) * std::max(height, 0);
    float resolutionFactor = BITRATES_MBPS.back();
    for (std::size_t index = 0; index < PIXEL_COUNTS.size(); ++index) {
      if (pixels <= PIXEL_COUNTS[index]) {
        resolutionFactor = index == 0 ? BITRATES_MBPS.front() :
                                        static_cast<float>(pixels - PIXEL_COUNTS[index - 1]) / (PIXEL_COUNTS[index] - PIXEL_COUNTS[index - 1]) * (BITRATES_MBPS[index] - BITRATES_MBPS[index - 1]) + BITRATES_MBPS[index - 1];
        break;
      }
    }

    // Xbox supports at most 60 FPS, so Android's nonlinear scaling above 60 FPS is unnecessary.
    const double frameRateFactor = std::clamp(framerate, 15, 60) / 30.0;
    // nxdk's lround() is an assertion stub. The product is positive and at most 80,
    // so adding 0.5 before truncating safely matches Android's whole-Mbps rounding.
    const int bitrateMbps = static_cast<int>(resolutionFactor * frameRateFactor + 0.5);
    return std::min(bitrateMbps * 1000, 50000);
  }

  std::string format_stream_bitrate_mbps(int bitrateKbps) {
    const std::int64_t magnitude = bitrateKbps < 0 ? -static_cast<std::int64_t>(bitrateKbps) : bitrateKbps;
    std::string label = (bitrateKbps < 0 ? "-" : "") + std::to_string(magnitude / 1000);
    if (magnitude % 1000 != 0) {
      std::string fraction = std::to_string(1000 + magnitude % 1000).substr(1);
      while (fraction.back() == '0') {
        fraction.pop_back();
      }
      label += "." + fraction;
    }
    return label + " Mbps";
  }

}  // namespace app
