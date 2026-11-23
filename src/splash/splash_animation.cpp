/**
 * @file src/splash/splash_animation.cpp
 * @brief Implements timing for the Moonlight logo startup animation.
 */
// class header include
#include "src/splash/splash_animation.h"

// standard includes
#include <algorithm>

namespace {

  constexpr std::uint64_t FRAME_INTERVAL_MS = 30;  ///< Interval between artwork samples.
  constexpr std::uint64_t MINIMUM_WAVE_END_MS = 2220;  ///< Earliest fade-out time, allowing the wave to complete a turn.
  constexpr std::uint64_t FADE_DURATION_MS = 180;  ///< Time to let the wave settle into the complete logo.
  constexpr std::uint64_t HOLD_DURATION_MS = 180;  ///< Time to show the complete logo before opening the menu.
  constexpr float INITIAL_WEDGE_SCALE = 1.08f;  ///< Slight extension of the first wedge on the opening frame.
  constexpr std::uint64_t POP_GROWTH_MS = 240;  ///< Time to reach full wedge size.
  constexpr std::uint64_t POP_RETURN_START_MS = 730;  ///< Time when the enlarged wedge starts to retract.
  constexpr std::uint64_t POP_END_MS = 880;  ///< Time when the first wedge is back at its resting size.
  constexpr std::uint64_t WAVE_LEAD_IN_MS = 840;  ///< First dimming pulse as the expanded wedge retracts.
  constexpr std::uint64_t WAVE_START_MS = 870;  ///< First dimming pulse on the second wedge.
  constexpr std::uint64_t WAVE_STEP_MS = 150;  ///< Delay from one wedge to the next.
  constexpr std::uint64_t WAVE_PERIOD_MS = WAVE_STEP_MS * splash::SPLASH_WEDGE_COUNT;  ///< Time for one turn around the logo.
  constexpr std::uint64_t DIM_IN_MS = 90;  ///< Time to dim a wedge.
  constexpr std::uint64_t DIM_OUT_MS = 180;  ///< Time to return a dimmed wedge to full brightness.
  constexpr std::uint64_t DIM_RELEASE_MS = 420;  ///< Time from dimming onset to the start of release.
  constexpr std::uint8_t FULL_BRIGHTNESS = 255;  ///< Unmodified RGB multiplier.
  constexpr std::uint8_t DIM_BRIGHTNESS = 156;  ///< RGB multiplier at the darkest point of the wave.

  /**
   * @brief Evaluate the brightness of a wedge during one pass of the wave.
   *
   * @param age Time since the dimming pulse reached this wedge.
   * @param releaseMilliseconds Time from dimming onset to the start of release.
   * @return RGB multiplier for the wedge.
   */
  std::uint8_t wave_brightness(std::uint64_t age, std::uint64_t releaseMilliseconds) {
    if (age >= releaseMilliseconds + DIM_OUT_MS) {
      return FULL_BRIGHTNESS;
    }

    constexpr std::uint64_t dimming = FULL_BRIGHTNESS - DIM_BRIGHTNESS;
    const auto amount = std::min({dimming, dimming * age / DIM_IN_MS, dimming * (releaseMilliseconds + DIM_OUT_MS - age) / DIM_OUT_MS});
    return static_cast<std::uint8_t>(FULL_BRIGHTNESS - amount);
  }

  /**
   * @brief Sample the pulse for one wedge on the clockwise loop.
   *
   * @param elapsedMilliseconds Time since the first animation frame.
   * @param wedgeIndex Wedge index in clockwise order.
   * @return RGB multiplier before the final fade.
   */
  std::uint8_t looping_brightness(std::uint64_t elapsedMilliseconds, std::size_t wedgeIndex) {
    // The expanded wedge has a short lead-in pulse and then takes its place after the last wedge.
    constexpr auto firstWedgeLoopStart = WAVE_START_MS + WAVE_PERIOD_MS - WAVE_STEP_MS;
    if (wedgeIndex == 0 && elapsedMilliseconds < firstWedgeLoopStart) {
      return elapsedMilliseconds >= WAVE_LEAD_IN_MS ? wave_brightness(elapsedMilliseconds - WAVE_LEAD_IN_MS, 330) : FULL_BRIGHTNESS;
    }

    const auto start = wedgeIndex == 0 ? firstWedgeLoopStart : WAVE_START_MS + (wedgeIndex - 1) * WAVE_STEP_MS;
    return elapsedMilliseconds >= start ? wave_brightness((elapsedMilliseconds - start) % WAVE_PERIOD_MS, DIM_RELEASE_MS) : FULL_BRIGHTNESS;
  }

}  // namespace

namespace splash {

  SplashAnimation::SplashAnimation():
      holdDuration_(HOLD_DURATION_MS),
      fadeDuration_(FADE_DURATION_MS) {}

  SplashAnimation::SplashAnimation(std::uint64_t durationMilliseconds):
      holdDuration_(std::min(HOLD_DURATION_MS, durationMilliseconds)),
      fadeDuration_(std::min(FADE_DURATION_MS, durationMilliseconds - holdDuration_)),
      fadeStart_(durationMilliseconds - fadeDuration_ - holdDuration_),
      holdStart_(durationMilliseconds - holdDuration_) {}

  SplashAnimationFrame SplashAnimation::advance(std::uint64_t elapsedMilliseconds, bool startupComplete) {
    if (!fadeStart_ && startupComplete && elapsedMilliseconds >= MINIMUM_WAVE_END_MS) {
      fadeStart_ = elapsedMilliseconds;
    }

    SplashAnimationFrame frame;
    float motionStrength = 1.0f;
    if (fadeStart_ && elapsedMilliseconds >= *fadeStart_) {
      const auto fadeAge = elapsedMilliseconds - *fadeStart_;
      if (fadeAge >= fadeDuration_) {
        if (!holdStart_) {
          holdStart_ = elapsedMilliseconds;
        }
        frame.brightness.fill(FULL_BRIGHTNESS);
        frame.finished = elapsedMilliseconds - *holdStart_ >= holdDuration_;
        return frame;
      }
      const auto fadeSample = fadeAge - fadeAge % FRAME_INTERVAL_MS;
      motionStrength = static_cast<float>(fadeDuration_ - fadeSample) / fadeDuration_;
    }

    const auto sampleTime = elapsedMilliseconds - elapsedMilliseconds % FRAME_INTERVAL_MS;
    if (sampleTime < POP_END_MS) {
      const float growth = static_cast<float>(sampleTime) / POP_GROWTH_MS;
      const float growingScale = INITIAL_WEDGE_SCALE + (SPLASH_MAX_WEDGE_SCALE - INITIAL_WEDGE_SCALE) * std::min(1.0f, growth);
      const float returnProgress = static_cast<float>(POP_END_MS - sampleTime) / (POP_END_MS - POP_RETURN_START_MS);
      const float returningScale = 1.0f + (SPLASH_MAX_WEDGE_SCALE - 1.0f) * std::min(1.0f, returnProgress);
      frame.firstWedgeScale += (std::min(growingScale, returningScale) - 1.0f) * motionStrength;
    }
    for (std::size_t i = 0; i < frame.brightness.size(); ++i) {
      const auto dimming = FULL_BRIGHTNESS - looping_brightness(sampleTime, i);
      frame.brightness[i] = FULL_BRIGHTNESS - static_cast<std::uint8_t>(dimming * motionStrength);
    }
    return frame;
  }

}  // namespace splash
