/**
 * @file src/splash/splash_animation.h
 * @brief Describes the startup animation of the Moonlight logo.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace splash {

  constexpr std::size_t SPLASH_WEDGE_COUNT = 8;  ///< Number of wedges in the logo.
  constexpr float SPLASH_MAX_WEDGE_SCALE = 1.28f;  ///< Maximum enlargement of the upper-left wedge.

  /**
   * @brief Sample the continuous dimming wave without the splash entrance or exit.
   * @param elapsedMilliseconds Wave time in milliseconds.
   * @return Brightness multipliers in clockwise wedge order.
   */
  std::array<std::uint8_t, SPLASH_WEDGE_COUNT> logo_wave_brightness(std::uint64_t elapsedMilliseconds);

  /**
   * @brief Appearance of the logo at one point in the startup animation.
   */
  struct SplashAnimationFrame {
    float firstWedgeScale = 1.0f;  ///< Scale around the center of the logo for the first wedge.
    std::array<std::uint8_t, SPLASH_WEDGE_COUNT> brightness {};  ///< RGB multipliers clockwise from the leftmost upper wedge, with 255 at full brightness.
    bool finished = false;  ///< True once the complete logo has been held for the final beat.
  };

  /**
   * @brief Loops the dimming wave until the splash is ready to exit.
   */
  class SplashAnimation {
  public:
    /**
     * @brief Create an animation that runs until startup is ready.
     */
    SplashAnimation();

    /**
     * @brief Create an animation with a fixed time budget.
     *
     * @param durationMilliseconds Total time available for the splash including the final pose.
     */
    explicit SplashAnimation(std::uint64_t durationMilliseconds);

    /**
     * @brief Sample the artwork and advance the exit sequence when startup is ready.
     *
     * @param elapsedMilliseconds Time since the splash started, in nondecreasing milliseconds.
     * @param startupComplete Whether the background startup task has completed.
     * @return Artwork and whether the final pose has been held long enough to exit.
     */
    SplashAnimationFrame advance(std::uint64_t elapsedMilliseconds, bool startupComplete);

  private:
    std::uint64_t holdDuration_;  ///< Time to display the complete logo.
    std::uint64_t fadeDuration_;  ///< Time over which the moving wave returns to full brightness.
    std::optional<std::uint64_t> fadeStart_;  ///< Time at which the wave begins fading out, once scheduled.
    std::optional<std::uint64_t> holdStart_;  ///< Time at which the complete logo is first visible.
  };

}  // namespace splash
