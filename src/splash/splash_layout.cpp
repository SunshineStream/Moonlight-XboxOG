/**
 * @file src/splash/splash_layout.cpp
 * @brief Implements splash screen layout calculations.
 */
// class header include
#include "src/splash/splash_layout.h"

// standard includes
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace {

  constexpr float SPLASH_LOGO_MAX_WIDTH_RATIO = 0.72f;
  constexpr float SPLASH_LOGO_MAX_HEIGHT_RATIO = 0.32f;
  constexpr float SPLASH_ASPECT_RATIO_EPSILON = 0.05f;

  int clamp_scaled_dimension(float value) {
    if (const auto scaledValue = static_cast<int>(value); scaledValue > 0) {
      return scaledValue;
    }

    return 1;
  }

}  // namespace

namespace splash {

  float get_framebuffer_aspect_ratio(const VIDEO_MODE &videoMode) {
    return static_cast<float>(videoMode.width) / static_cast<float>(videoMode.height);
  }

  float get_display_aspect_ratio(const VIDEO_MODE &videoMode, unsigned long encoderSettings) {
    const float framebufferAspectRatio = get_framebuffer_aspect_ratio(videoMode);
    if (const float preferredDisplayAspectRatio = ((encoderSettings & VIDEO_WIDESCREEN) != 0UL) ? (16.0f / 9.0f) : (4.0f / 3.0f); std::fabs(framebufferAspectRatio - preferredDisplayAspectRatio) > SPLASH_ASPECT_RATIO_EPSILON) {
      return preferredDisplayAspectRatio;
    }

    return framebufferAspectRatio;
  }

  int calculate_display_width(int screenHeight, const VIDEO_MODE &videoMode, unsigned long encoderSettings) {
    if (screenHeight <= 0 || videoMode.width <= 0 || videoMode.height <= 0) {
      return 1;
    }

    const std::int64_t preferredNumerator = (encoderSettings & VIDEO_WIDESCREEN) != 0UL ? 16 : 4;
    const std::int64_t preferredDenominator = (encoderSettings & VIDEO_WIDESCREEN) != 0UL ? 9 : 3;
    constexpr int toleranceDenominator = 20;  ///< Exact 0.05 aspect tolerance used by get_display_aspect_ratio().
    const std::int64_t aspectDifference = std::abs(static_cast<std::int64_t>(videoMode.width) * preferredDenominator - static_cast<std::int64_t>(videoMode.height) * preferredNumerator);
    const bool matchesDisplay = aspectDifference * toleranceDenominator <= videoMode.height * preferredDenominator;
    const std::int64_t numerator = matchesDisplay ? videoMode.width : preferredNumerator;
    const std::int64_t denominator = matchesDisplay ? videoMode.height : preferredDenominator;

    // x87 intermediates can fall just below an exact pixel width after settings
    // arithmetic. Truncating that float briefly changed 1280 to 1279 and shifted
    // the entire shell. Keep the ratio integral and round to the nearest pixel.
    const std::int64_t width = (static_cast<std::int64_t>(screenHeight) * numerator + denominator / 2) / denominator;
    return static_cast<int>(std::clamp<std::int64_t>(width, 1, std::numeric_limits<int>::max()));
  }

  float get_logo_width_aspect_correction(const VIDEO_MODE &videoMode, unsigned long encoderSettings) {
    return get_framebuffer_aspect_ratio(videoMode) / get_display_aspect_ratio(videoMode, encoderSettings);
  }

  SplashLayout calculate_logo_destination(
    int screenWidth,
    int screenHeight,
    int logoWidth,
    int logoHeight,
    const VIDEO_MODE &videoMode,
    unsigned long encoderSettings
  ) {
    const float correctedLogoWidth = static_cast<float>(logoWidth) * get_logo_width_aspect_correction(videoMode, encoderSettings);
    const float maxLogoWidth = static_cast<float>(screenWidth) * SPLASH_LOGO_MAX_WIDTH_RATIO;
    const float maxLogoHeight = static_cast<float>(screenHeight) * SPLASH_LOGO_MAX_HEIGHT_RATIO;
    const float widthScale = maxLogoWidth / correctedLogoWidth;
    const float heightScale = maxLogoHeight / static_cast<float>(logoHeight);
    const float scale = std::min(widthScale, heightScale);

    SplashLayout layout {};
    layout.width = clamp_scaled_dimension(correctedLogoWidth * scale);
    layout.height = clamp_scaled_dimension(static_cast<float>(logoHeight) * scale);
    layout.x = (screenWidth - layout.width) / 2;
    layout.y = (screenHeight - layout.height) / 2;
    return layout;
  }

}  // namespace splash
