/**
 * @file src/splash/splash_screen.cpp
 * @brief Implements the splash screen workflow.
 */
// class header include
#include "src/splash/splash_screen.h"

// standard includes
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

// nxdk includes
#include <hal/xbox.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <windows.h>  // NOSONAR(cpp:S3806) nxdk requires lowercase header names

// local includes
#include "src/logging/logger.h"
#include "src/os.h"
#include "src/splash/splash_layout.h"

namespace {

  constexpr Uint8 SPLASH_BACKGROUND_RED = 0x56;
  constexpr Uint8 SPLASH_BACKGROUND_GREEN = 0x5C;
  constexpr Uint8 SPLASH_BACKGROUND_BLUE = 0x64;

  void printSDLErrorAndReboot() {
    logging::error("splash", std::string("SDL error: ") + SDL_GetError());
    logging::warn("splash", "Rebooting in 5 seconds.");
    Sleep(5000);
    XReboot();
  }

  std::string buildAssetPath(const char *assetName) {
    return std::string(DATA_PATH) + "assets" + PATH_SEP + assetName;
  }

  SDL_Rect createCenteredRect(const SDL_Surface *screenSurface, int width, int height) {
    SDL_Rect destination {};
    if (screenSurface == nullptr) {
      return destination;
    }

    destination.w = width;
    destination.h = height;
    destination.x = (screenSurface->w - destination.w) / 2;
    destination.y = (screenSurface->h - destination.h) / 2;
    return destination;
  }

  SDL_Rect calculateLogoDestination(const SDL_Surface *screenSurface, int logoWidth, int logoHeight, const VIDEO_MODE &videoMode) {
    if (screenSurface == nullptr) {
      return {};
    }

    const splash::SplashLayout layout = splash::calculate_logo_destination(
      screenSurface->w,
      screenSurface->h,
      logoWidth,
      logoHeight,
      videoMode,
      XVideoGetEncoderSettings()
    );

    SDL_Rect destination {};
    destination.x = layout.x;
    destination.y = layout.y;
    destination.w = layout.width;
    destination.h = layout.height;
    return destination;
  }

  /**
   * @brief Scale the splash artwork to the encoder-corrected viewport.
   * @param screenSurface Window surface used for sizing.
   * @param videoMode Active Xbox display mode.
   * @return Owned scaled artwork with alpha blending enabled, or null on error.
   */
  SDL_Surface *load_splash_logo(const SDL_Surface *screenSurface, const VIDEO_MODE &videoMode) {
    const auto path = buildAssetPath("moonlight-logo-wedges.svg");
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> source(IMG_Load(path.c_str()), SDL_DestroySurface);
    if (!source) {
      logging::error("splash", "Failed to load splash asset " + path + ": " + SDL_GetError());
      return nullptr;
    }

    // Scale colors and alpha independently, then blend the result over the splash background.
    SDL_SetSurfaceBlendMode(source.get(), SDL_BLENDMODE_NONE);
    const auto destination = calculateLogoDestination(screenSurface, source->w, source->h, videoMode);
    auto *scaled = SDL_ScaleSurface(source.get(), destination.w, destination.h, SDL_SCALEMODE_LINEAR);
    if (scaled != nullptr && !SDL_SetSurfaceBlendMode(scaled, SDL_BLENDMODE_BLEND)) {
      SDL_DestroySurface(scaled);
      return nullptr;
    }
    return scaled;
  }

  /**
   * @brief Present the boot splash on the CPU framebuffer while startup runs.
   * @param window Startup window.
   * @param videoMode Active Xbox display mode.
   * @param keepShowing Startup predicate evaluated after each frame.
   */
  void runSplashScreen(SDL_Window *window, const VIDEO_MODE &videoMode, const std::function<bool()> &keepShowing) {  // NOSONAR(cpp:S5213) splash callback API is cold-path and already shared by callers
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
    SDL_Surface *screenSurface = SDL_GetWindowSurface(window);
    if (screenSurface == nullptr) {
      printSDLErrorAndReboot();
      return;
    }

    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> logo(load_splash_logo(screenSurface, videoMode), SDL_DestroySurface);
    if (!logo) {
      printSDLErrorAndReboot();
      return;
    }

    const SDL_Rect logoDestination = createCenteredRect(screenSurface, logo->w, logo->h);
    const Uint32 background = SDL_MapSurfaceRGB(screenSurface, SPLASH_BACKGROUND_RED, SPLASH_BACKGROUND_GREEN, SPLASH_BACKGROUND_BLUE);
    bool done = false;
    do {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        done |= event.type == SDL_EVENT_QUIT;
      }
      if (!SDL_FillSurfaceRect(screenSurface, nullptr, background) || !SDL_BlitSurface(logo.get(), nullptr, screenSurface, &logoDestination) || !SDL_UpdateWindowSurface(window)) {
        printSDLErrorAndReboot();
        break;
      }
      done |= !keepShowing();
      SDL_Delay(16);
    } while (!done);

    // Release the splash framebuffer before creating the interactive renderer.
    SDL_DestroyWindowSurface(window);
  }

}  // namespace

namespace splash {

  void show_splash_screen(SDL_Window *window, const VIDEO_MODE &videoMode, unsigned int durationMilliseconds) {
    const Uint64 startTicks = SDL_GetTicks();
    runSplashScreen(window, videoMode, [startTicks, durationMilliseconds]() {
      return SDL_GetTicks() - startTicks < durationMilliseconds;
    });
  }

  void show_splash_screen(SDL_Window *window, const VIDEO_MODE &videoMode, const std::function<bool()> &keepShowing) {
    runSplashScreen(window, videoMode, keepShowing);
  }

}  // namespace splash
