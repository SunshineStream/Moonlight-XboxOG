/**
 * @file src/splash/splash_animation.h
 * @brief Describes the startup animation of the Sunshine logo.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace splash {

  constexpr std::size_t LOGO_ARC_COUNT = 4;  ///< Number of colored arcs in the Sunshine logo.
  using LogoOpacity = std::array<std::uint8_t, LOGO_ARC_COUNT>;  ///< Opacities in the source SVG's drawing order.

  /**
   * @brief Sample the continuous glow without the splash entrance or exit.
   * @param elapsedMilliseconds Time in milliseconds.
   * @return Opacities for the four arcs from yellow through orange.
   */
  LogoOpacity logo_glow_opacity(std::uint64_t elapsedMilliseconds);

  /**
   * @brief Appearance of the logo at one point in the startup animation.
   */
  struct SplashAnimationFrame {
    LogoOpacity opacity {};  ///< Opacity of each arc, with 255 at full color.
    bool finished = false;  ///< True once the complete logo has been held for the final beat.
  };

  /**
   * @brief Reveals the logo from the inner arc out and loops its glow until startup is ready.
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
    std::uint64_t fadeDuration_;  ///< Time over which the glow settles into full color.
    std::optional<std::uint64_t> fadeStart_;  ///< Time at which the glow starts to settle, once scheduled.
    std::optional<std::uint64_t> holdStart_;  ///< Time at which the complete logo is first visible.
  };

}  // namespace splash
