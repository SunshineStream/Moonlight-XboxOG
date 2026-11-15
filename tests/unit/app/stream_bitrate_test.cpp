/**
 * @file tests/unit/app/stream_bitrate_test.cpp
 * @brief Verifies Android-compatible bitrate defaults and Mbps labels.
 */
// standard includes
#include <array>
#include <limits>

// lib includes
#include <lizardbyte/common/testing.h>

// local includes
#include "src/app/stream_bitrate.h"

namespace {

  /**
   * @brief Verify each Xbox stream preset at every selectable frame rate.
   */
  TEST(StreamBitrateTest, CalculatesXboxDefaultsAtEverySupportedFrameRate) {
    constexpr std::array<int, 6> frameRates {15, 20, 24, 25, 30, 60};
    constexpr std::array<std::array<int, 8>, 4> cases {{
      {640, 480, 1000, 1000, 1000, 1000, 1000, 3000},
      {720, 480, 1000, 1000, 1000, 1000, 2000, 3000},
      {1280, 720, 3000, 3000, 4000, 4000, 5000, 10000},
      {1920, 1080, 5000, 7000, 8000, 8000, 10000, 20000},
    }};
    for (const auto &entry : cases) {
      for (std::size_t index = 0; index < frameRates.size(); ++index) {
        EXPECT_EQ(app::default_stream_bitrate_kbps(entry[0], entry[1], frameRates[index]), entry[index + 2])
          << entry[0] << 'x' << entry[1] << " at " << frameRates[index] << " FPS";
      }
    }
  }

  /**
   * @brief Check Android's reference resolutions, interpolation and half-Mbps rounding.
   */
  TEST(StreamBitrateTest, MatchesAndroidReferenceDefaultsAndInterpolatesByPixelCount) {
    EXPECT_EQ(app::default_stream_bitrate_kbps(640, 360, 30), 1000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(854, 480, 30), 2000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(1280, 720, 30), 5000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(1920, 1080, 30), 10000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(2560, 1440, 30), 20000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(3840, 2160, 30), 40000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(1600, 900, 30), 7000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(1600, 900, 60), 15000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(720, 1280, 60), 10000);
  }

  /**
   * @brief Preserve Android's rounding on either side of a half-Mbps boundary without nxdk math stubs.
   */
  TEST(StreamBitrateTest, RoundsHalfMegabitsUpUsingBoundedArithmetic) {
    EXPECT_EQ(app::default_stream_bitrate_kbps(1278, 720, 15), 2000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(1280, 720, 15), 3000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(1282, 720, 15), 3000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(640, 360, 15), 1000);
  }

  /**
   * @brief Keep missing, extreme or unsupported inputs inside the Xbox stream limits.
   */
  TEST(StreamBitrateTest, BoundsResolutionFactorsFrameRatesAndBitrates) {
    EXPECT_EQ(app::default_stream_bitrate_kbps(320, 240, 30), 1000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(0, 480, 30), 1000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(640, 0, 30), 1000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(-640, -480, 30), 1000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(7680, 4320, 30), 40000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(1280, 720, -1), 3000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(1280, 720, 120), 10000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(3840, 2160, 60), 50000);
    EXPECT_EQ(app::default_stream_bitrate_kbps(std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), 60), 50000);
  }

  /**
   * @brief Show Mbps while preserving fractional rates and removing redundant zeros.
   */
  TEST(StreamBitrateTest, FormatsWholeAndFractionalMegabitsWithoutPrecisionLoss) {
    EXPECT_EQ(app::format_stream_bitrate_mbps(0), "0 Mbps");
    EXPECT_EQ(app::format_stream_bitrate_mbps(500), "0.5 Mbps");
    EXPECT_EQ(app::format_stream_bitrate_mbps(750), "0.75 Mbps");
    EXPECT_EQ(app::format_stream_bitrate_mbps(1000), "1 Mbps");
    EXPECT_EQ(app::format_stream_bitrate_mbps(1500), "1.5 Mbps");
    EXPECT_EQ(app::format_stream_bitrate_mbps(1234), "1.234 Mbps");
    EXPECT_EQ(app::format_stream_bitrate_mbps(20000), "20 Mbps");
    EXPECT_EQ(app::format_stream_bitrate_mbps(-500), "-0.5 Mbps");
    EXPECT_EQ(app::format_stream_bitrate_mbps(std::numeric_limits<int>::min()), "-2147483.648 Mbps");
  }

}  // namespace
