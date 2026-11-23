/**
 * @file tests/unit/splash/splash_animation_test.cpp
 * @brief Verifies the startup logo's wedge animation.
 */
// test header include
#include "src/splash/splash_animation.h"

// standard includes
#include <algorithm>
#include <limits>

// lib includes
#include <lizardbyte/common/testing.h>

namespace {

  /**
   * @brief Check whether the first wedge is at rest and all eight wedges are fully lit.
   *
   * @param frame Splash artwork to examine.
   * @return True when the complete logo is visible.
   */
  bool is_complete_logo(const splash::SplashAnimationFrame &frame) {
    return frame.firstWedgeScale == 1.0f && std::all_of(frame.brightness.begin(), frame.brightness.end(), [](auto brightness) {
             return brightness == 255;
           });
  }

  /**
   * @brief Verify that the first wedge grows, holds, and retracts before the wave continues.
   */
  TEST(SplashAnimationTest, ExpandsAndRetractsTheFirstWedge) {
    splash::SplashAnimation animation;
    const auto opening = animation.advance(0, false);
    EXPECT_GT(opening.firstWedgeScale, 1.0f);
    const auto growing = animation.advance(120, false);
    EXPECT_GT(growing.firstWedgeScale, opening.firstWedgeScale);
    EXPECT_LT(growing.firstWedgeScale, splash::SPLASH_MAX_WEDGE_SCALE);
    for (auto brightness : growing.brightness) {
      EXPECT_EQ(brightness, 255);
    }
    EXPECT_FLOAT_EQ(animation.advance(240, false).firstWedgeScale, splash::SPLASH_MAX_WEDGE_SCALE);
    EXPECT_FLOAT_EQ(animation.advance(700, false).firstWedgeScale, splash::SPLASH_MAX_WEDGE_SCALE);
    const auto retracting = animation.advance(800, false).firstWedgeScale;
    EXPECT_GT(retracting, 1.0f);
    EXPECT_LT(retracting, splash::SPLASH_MAX_WEDGE_SCALE);
    EXPECT_FLOAT_EQ(animation.advance(900, false).firstWedgeScale, 1.0f);
  }

  /**
   * @brief Verify that the dimming wave visits each successive wedge and wraps to the first.
   */
  TEST(SplashAnimationTest, DimsWedgesInClockwiseOrderAndWrapsToTheFirst) {
    const auto sample = [](std::uint64_t elapsed) {
      return splash::SplashAnimation {}.advance(elapsed, false);
    };
    const auto waveStart = sample(930);
    EXPECT_EQ(waveStart.brightness[0], 156);
    EXPECT_GT(waveStart.brightness[1], waveStart.brightness[0]);
    EXPECT_LT(waveStart.brightness[1], 255);
    for (std::size_t i = 2; i < splash::SPLASH_WEDGE_COUNT; ++i) {
      const auto onset = 870 + (i - 1) * 150;
      EXPECT_EQ(sample(onset).brightness[i], 255) << i;
      EXPECT_EQ(sample(onset + 90).brightness[i], 156) << i;
      EXPECT_GT(sample(onset + 510).brightness[i], 156) << i;
      EXPECT_EQ(sample(onset + 600).brightness[i], 255) << i;
    }
    EXPECT_EQ(sample(1350).brightness.front(), 255);
    EXPECT_EQ(sample(2010).brightness.front(), 156);
    EXPECT_EQ(sample(2010).brightness[1], 255);
    EXPECT_EQ(sample(2160).brightness[1], 156);
    EXPECT_EQ(sample(2220).brightness, sample(3420).brightness);
  }

  /**
   * @brief Verify that slow startup keeps the artwork moving across multiple turns.
   */
  TEST(SplashAnimationTest, ReservesTheCompleteLogoForTheFinalPose) {
    splash::SplashAnimation animation;
    for (std::uint64_t elapsed = 0; elapsed <= 12000; elapsed += 30) {
      const auto frame = animation.advance(elapsed, false);
      EXPECT_FALSE(is_complete_logo(frame)) << elapsed;
      EXPECT_FALSE(frame.finished) << elapsed;
    }
    EXPECT_FALSE(is_complete_logo(animation.advance(std::numeric_limits<std::uint64_t>::max(), false)));
  }

  /**
   * @brief Verify that fast startup shows a full turn of the wave and the brief final hold.
   */
  TEST(SplashAnimationTest, KeepsTheWaveMovingUntilTheEarliestFade) {
    splash::SplashAnimation animation;
    splash::SplashAnimation looping;
    EXPECT_EQ(animation.advance(2219, true).brightness, looping.advance(2219, false).brightness);
    EXPECT_EQ(animation.advance(2220, true).brightness, looping.advance(2220, false).brightness);
    const auto fading = animation.advance(2310, true);
    EXPECT_GT(fading.brightness[2], looping.advance(2310, false).brightness[2]);
    EXPECT_FALSE(is_complete_logo(fading));
    EXPECT_TRUE(is_complete_logo(animation.advance(2400, true)));
    EXPECT_FALSE(animation.advance(2579, true).finished);
    EXPECT_TRUE(animation.advance(2580, true).finished);
  }

  /**
   * @brief Verify that the fade and hold follow readiness even after many rotations.
   */
  TEST(SplashAnimationTest, StartsTheFinalHoldWhenSlowStartupCompletes) {
    splash::SplashAnimation animation;
    const auto waiting = animation.advance(8999, false);
    EXPECT_FALSE(is_complete_logo(waiting));
    EXPECT_FALSE(waiting.finished);
    EXPECT_FALSE(is_complete_logo(animation.advance(9000, true)));
    EXPECT_FALSE(is_complete_logo(animation.advance(9179, true)));
    EXPECT_TRUE(is_complete_logo(animation.advance(9180, true)));
    EXPECT_FALSE(animation.advance(9359, true).finished);
    EXPECT_TRUE(animation.advance(9360, true).finished);
  }

  /**
   * @brief Verify that the logo remains visible for the final hold after a delayed frame.
   */
  TEST(SplashAnimationTest, HoldsTheCompleteLogoAfterDelayedPresentation) {
    splash::SplashAnimation animation;
    EXPECT_FALSE(animation.advance(5000, true).finished);
    const auto delayedFrame = animation.advance(5600, true);
    EXPECT_TRUE(is_complete_logo(delayedFrame));
    EXPECT_FALSE(delayedFrame.finished);
    EXPECT_FALSE(animation.advance(5779, true).finished);
    EXPECT_TRUE(animation.advance(5780, true).finished);
  }

  /**
   * @brief Verify that a fixed-duration splash fits its final pose into the available time.
   */
  TEST(SplashAnimationTest, FitsTheFadeAndHoldIntoAFixedDuration) {
    splash::SplashAnimation animation {1500};
    EXPECT_FALSE(is_complete_logo(animation.advance(1139, true)));
    EXPECT_FALSE(is_complete_logo(animation.advance(1319, false)));
    EXPECT_TRUE(is_complete_logo(animation.advance(1320, false)));
    EXPECT_FALSE(animation.advance(1499, false).finished);
    EXPECT_TRUE(animation.advance(1500, false).finished);

    splash::SplashAnimation shortAnimation {200};
    EXPECT_FALSE(is_complete_logo(shortAnimation.advance(0, false)));
    EXPECT_TRUE(is_complete_logo(shortAnimation.advance(20, false)));
    EXPECT_FALSE(shortAnimation.advance(199, false).finished);
    EXPECT_TRUE(shortAnimation.advance(200, false).finished);

    splash::SplashAnimation immediate {0};
    const auto finalFrame = immediate.advance(0, false);
    EXPECT_TRUE(is_complete_logo(finalFrame));
    EXPECT_TRUE(finalFrame.finished);
  }

}  // namespace
