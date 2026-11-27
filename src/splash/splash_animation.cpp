/**
 * @file src/splash/splash_animation.cpp
 * @brief Implements the Sunshine logo's staggered reveal, looping glow and final hold.
 */
// class header include
#include "src/splash/splash_animation.h"

// standard includes
#include <algorithm>

namespace {

  constexpr std::uint64_t FRAME_INTERVAL_MS = 32;  ///< Interval between artwork samples.
  constexpr std::uint64_t REVEAL_STEP_MS = 120;  ///< Delay between adjacent arcs at startup.
  constexpr std::uint64_t REVEAL_DURATION_MS = 280;  ///< Time for each arc to fade in.
  constexpr std::uint64_t GLOW_STEP_MS = 240;  ///< Delay between peaks on adjacent arcs.
  constexpr std::uint64_t GLOW_PERIOD_MS = GLOW_STEP_MS * splash::LOGO_ARC_COUNT;  ///< Time for one full loop.
  constexpr std::uint64_t GLOW_RADIUS_MS = 320;  ///< Time from an arc's dimmest point to its peak.
  constexpr std::uint64_t MINIMUM_GLOW_END_MS = 1600;  ///< Time for the reveal and one complete glow.
  constexpr std::uint64_t FADE_DURATION_MS = 180;  ///< Time to bring every arc to full color.
  constexpr std::uint64_t HOLD_DURATION_MS = 180;  ///< Time to show the complete logo before opening the menu.
  constexpr std::uint8_t DIM_OPACITY = 132;  ///< Minimum opacity during the looping glow.
  constexpr std::uint8_t FULL_OPACITY = 255;  ///< Unmodified Sunshine colors.

  /**
   * @brief Ease a fraction in and out over the interval from zero to one.
   * @param progress Linear fraction between zero and one.
   * @return Smoothed fraction with the same endpoints.
   */
  float smooth_progress(float progress) {
    return progress * progress * (3.0f - 2.0f * progress);
  }

}  // namespace

namespace splash {

  LogoOpacity logo_glow_opacity(std::uint64_t elapsedMilliseconds) {
    LogoOpacity opacity {};
    const auto elapsed = elapsedMilliseconds % GLOW_PERIOD_MS;
    for (std::size_t arc = 0; arc < opacity.size(); ++arc) {
      const auto phase = (elapsed + GLOW_PERIOD_MS - arc * GLOW_STEP_MS) % GLOW_PERIOD_MS;
      const auto distance = std::min({phase, GLOW_PERIOD_MS - phase, GLOW_RADIUS_MS});
      const float glow = smooth_progress(1.0f - static_cast<float>(distance) / GLOW_RADIUS_MS);
      opacity[arc] = DIM_OPACITY + static_cast<std::uint8_t>((FULL_OPACITY - DIM_OPACITY) * glow);
    }
    return opacity;
  }

  SplashAnimation::SplashAnimation():
      holdDuration_(HOLD_DURATION_MS),
      fadeDuration_(FADE_DURATION_MS) {}

  SplashAnimation::SplashAnimation(std::uint64_t durationMilliseconds):
      holdDuration_(std::min(HOLD_DURATION_MS, durationMilliseconds)),
      fadeDuration_(std::min(FADE_DURATION_MS, durationMilliseconds - holdDuration_)),
      fadeStart_(durationMilliseconds - fadeDuration_ - holdDuration_),
      holdStart_(durationMilliseconds - holdDuration_) {}

  SplashAnimationFrame SplashAnimation::advance(std::uint64_t elapsedMilliseconds, bool startupComplete) {
    if (!fadeStart_ && startupComplete && elapsedMilliseconds >= MINIMUM_GLOW_END_MS) {
      fadeStart_ = elapsedMilliseconds;
    }

    SplashAnimationFrame frame;
    float settle = 0.0f;
    if (fadeStart_ && elapsedMilliseconds >= *fadeStart_) {
      const auto fadeAge = elapsedMilliseconds - *fadeStart_;
      if (fadeAge >= fadeDuration_) {
        if (!holdStart_) {
          holdStart_ = elapsedMilliseconds;
        }
        frame.opacity.fill(FULL_OPACITY);
        frame.finished = elapsedMilliseconds - *holdStart_ >= holdDuration_;
        return frame;
      }
      const auto fadeSample = fadeAge - fadeAge % FRAME_INTERVAL_MS;
      settle = smooth_progress(static_cast<float>(fadeSample) / fadeDuration_);
    }

    const auto sampleTime = elapsedMilliseconds - elapsedMilliseconds % FRAME_INTERVAL_MS;
    frame.opacity = logo_glow_opacity(sampleTime);
    for (std::size_t arc = 0; arc < frame.opacity.size(); ++arc) {
      const auto revealStart = arc * REVEAL_STEP_MS;
      const auto revealAge = sampleTime - std::min(sampleTime, revealStart);
      const float reveal = smooth_progress(static_cast<float>(std::min(revealAge, REVEAL_DURATION_MS)) / REVEAL_DURATION_MS);
      const float opacity = frame.opacity[arc] * reveal;
      frame.opacity[arc] = static_cast<std::uint8_t>(opacity + (FULL_OPACITY - opacity) * settle);
    }
    return frame;
  }

}  // namespace splash
