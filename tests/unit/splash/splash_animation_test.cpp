/**
 * @file tests/unit/splash/splash_animation_test.cpp
 * @brief Verifies the Sunshine logo reveal, glow and startup exit timing.
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
   * @brief Check whether the logo has reached its full-color resting pose.
   * @param frame Sampled artwork to examine.
   * @return True when every arc is fully opaque.
   */
  bool is_complete_logo(const splash::SplashAnimationFrame &frame) {
    return std::all_of(frame.opacity.begin(), frame.opacity.end(), [](auto opacity) {
      return opacity == 255;
    });
  }

  /**
   * @brief Verify that the save animation pulses each arc, then repeats seamlessly.
   */
  TEST(SplashAnimationTest, LoopsThroughEachArcInDrawingOrder) {
    constexpr std::uint64_t step = 240;
    constexpr auto period = step * splash::LOGO_ARC_COUNT;
    for (std::size_t arc = 0; arc < splash::LOGO_ARC_COUNT; ++arc) {
      const auto peak = splash::logo_glow_opacity(arc * step);
      EXPECT_EQ(peak[arc], 255);
      EXPECT_EQ(std::count(peak.begin(), peak.end(), 255), 1);
      EXPECT_EQ(peak, splash::logo_glow_opacity(period + arc * step));
    }

    const auto last = splash::logo_glow_opacity(period - 1);
    const auto first = splash::logo_glow_opacity(0);
    for (std::size_t arc = 0; arc < first.size(); ++arc) {
      EXPECT_NEAR(last[arc], first[arc], 1);
    }
    EXPECT_NE(first, splash::logo_glow_opacity(step / 2));
  }

  /**
   * @brief Verify the arcs appear in order and that adjacent samples share a render frame.
   */
  TEST(SplashAnimationTest, RevealsTheLogoFromTheInnerArcOut) {
    splash::SplashAnimation animation;
    EXPECT_EQ(animation.advance(0, false).opacity, splash::LogoOpacity {});
    for (std::size_t arc = 0; arc < splash::LOGO_ARC_COUNT; ++arc) {
      const auto time = 32 + ((arc * 120 + 31) / 32) * 32;
      const auto frame = animation.advance(time, false);
      for (std::size_t i = 0; i < frame.opacity.size(); ++i) {
        EXPECT_EQ(frame.opacity[i] > 0, i <= arc) << time << ' ' << i;
      }
      EXPECT_EQ(frame.opacity, animation.advance(time + 31, false).opacity);
    }
    EXPECT_EQ(animation.advance(640, false).opacity, splash::logo_glow_opacity(640));
  }

  /**
   * @brief Verify that a slow startup continues to glow across many loops.
   */
  TEST(SplashAnimationTest, KeepsGlowingWhileStartupIsIncomplete) {
    splash::SplashAnimation animation;
    for (std::uint64_t elapsed = 0; elapsed <= 12000; elapsed += 32) {
      const auto frame = animation.advance(elapsed, false);
      EXPECT_FALSE(is_complete_logo(frame)) << elapsed;
      EXPECT_FALSE(frame.finished) << elapsed;
    }
    const auto time = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(animation.advance(time, false).opacity, splash::logo_glow_opacity(time - time % 32));
  }

  /**
   * @brief Verify the splash completes the first glow and holds the full logo before exiting.
   */
  TEST(SplashAnimationTest, WaitsForTheFirstGlowBeforeSettling) {
    splash::SplashAnimation animation;
    splash::SplashAnimation looping;
    EXPECT_EQ(animation.advance(1599, true).opacity, looping.advance(1599, false).opacity);
    EXPECT_EQ(animation.advance(1600, true).opacity, looping.advance(1600, false).opacity);
    const auto settling = animation.advance(1696, true);
    EXPECT_GT(settling.opacity[0], looping.advance(1696, false).opacity[0]);
    EXPECT_FALSE(is_complete_logo(settling));
    EXPECT_TRUE(is_complete_logo(animation.advance(1780, true)));
    EXPECT_FALSE(animation.advance(1959, true).finished);
    EXPECT_TRUE(animation.advance(1960, true).finished);
  }

  /**
   * @brief Verify the final hold starts after the full logo is sampled during a slow startup.
   */
  TEST(SplashAnimationTest, HoldsTheFullLogoAfterADelayedFrame) {
    splash::SplashAnimation animation;
    EXPECT_FALSE(animation.advance(8999, false).finished);
    EXPECT_FALSE(is_complete_logo(animation.advance(9000, true)));
    EXPECT_FALSE(is_complete_logo(animation.advance(9179, true)));
    const auto delayedFrame = animation.advance(9500, true);
    EXPECT_TRUE(is_complete_logo(delayedFrame));
    EXPECT_FALSE(delayedFrame.finished);
    EXPECT_FALSE(animation.advance(9679, true).finished);
    EXPECT_TRUE(animation.advance(9680, true).finished);
  }

  /**
   * @brief Verify that short or zero fixed budgets still end with the resting pose.
   */
  TEST(SplashAnimationTest, FitsTheFinalPoseIntoAFixedDuration) {
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
