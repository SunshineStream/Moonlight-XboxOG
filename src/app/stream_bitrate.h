/**
 * @file src/app/stream_bitrate.h
 * @brief Declares resolution and frame rate based stream bitrate defaults.
 */
#pragma once

// standard includes
#include <string>

namespace app {

  /**
   * @brief Calculate the default bitrate using Moonlight Android's pixel-count interpolation.
   *
   * Resolution factors range from 1 Mbps at 640x360 to 40 Mbps at 3840x2160,
   * measured at 30 FPS. Intermediate pixel counts are interpolated linearly.
   * The result is rounded to whole Mbps and limited to the supported stream range.
   *
   * @param width Stream width in pixels; nonpositive values use the lowest resolution factor.
   * @param height Stream height in pixels; nonpositive values use the lowest resolution factor.
   * @param framerate Requested frame rate, clamped to the supported 15 through 60 FPS range.
   * @return Default stream bitrate in kilobits per second, between 1000 and 50000.
   */
  int default_stream_bitrate_kbps(int width, int height, int framerate);

  /**
   * @brief Format a bitrate in Mbps without trailing fractional zeros.
   *
   * @param bitrateKbps Bitrate stored in kilobits per second.
   * @return Bitrate label in megabits per second, preserving sub-Mbps precision.
   */
  std::string format_stream_bitrate_mbps(int bitrateKbps);

}  // namespace app
