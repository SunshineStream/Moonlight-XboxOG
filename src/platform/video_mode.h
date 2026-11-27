/**
 * @file src/platform/video_mode.h
 * @brief Exposes the video mode used by shared layout and stream settings.
 */
#pragma once

#ifdef SUNSHINE_NATIVE_CLIENT
/**
 * @brief Display or requested stream dimensions for the portable client.
 */
struct VIDEO_MODE {
  int width;  ///< Width in pixels.
  int height;  ///< Height in pixels.
  int bpp;  ///< Color depth metadata.
  int refresh;  ///< Refresh rate in whole Hz.
};

inline constexpr int REFRESH_DEFAULT = 0;  ///< Use the default display refresh rate.
inline constexpr unsigned long VIDEO_WIDESCREEN = 0x010000UL;  ///< Widescreen aspect hint for shared layout code.
#else
  #include <hal/video.h>
#endif
