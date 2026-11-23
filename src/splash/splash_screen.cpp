/**
 * @file src/splash/splash_screen.cpp
 * @brief Implements the splash screen workflow.
 */
// class header include
#include "src/splash/splash_screen.h"

// standard includes
#include <array>
#include <memory>
#include <optional>
#include <string>

// nxdk includes
#include <hal/xbox.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <windows.h>  // NOSONAR(cpp:S3806) nxdk requires lowercase header names

// local includes
#include "src/logging/logger.h"
#include "src/os.h"
#include "src/splash/splash_animation.h"
#include "src/splash/splash_layout.h"

namespace {

  constexpr Uint8 SPLASH_BACKGROUND_RED = 0x56;
  constexpr Uint8 SPLASH_BACKGROUND_GREEN = 0x5C;
  constexpr Uint8 SPLASH_BACKGROUND_BLUE = 0x64;

  /**
   * @brief One independently animated wedge of the Moonlight logo.
   */
  struct LogoWedge {
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> pixels {nullptr, SDL_DestroySurface};  ///< Wedge rasterized at its resting size.
    SDL_Rect destination {};  ///< Resting location of the wedge on the framebuffer.
  };

  /**
   * @brief Pre-scaled artwork and the framebuffer region covered by the animation.
   */
  struct SplashLogo {
    std::array<LogoWedge, splash::SPLASH_WEDGE_COUNT> wedges;  ///< Wedges in clockwise order from the leftmost upper wedge.
    SDL_Rect bounds {};  ///< Area to clear and present between animation frames.
  };

  /**
   * @brief Log a splash rendering error and reboot after a delay.
   */
  void printSDLErrorAndReboot() {
    logging::error("splash", std::string("SDL error: ") + SDL_GetError());
    logging::warn("splash", "Rebooting in 5 seconds.");
    Sleep(5000);
    XReboot();
  }

  /**
   * @brief Expand the first wedge around its bottom-right corner, the center of the logo.
   *
   * @param destination Resting rectangle of the upper-left quadrant.
   * @param scale Enlargement to apply along both axes.
   * @return Expanded rectangle in framebuffer coordinates.
   */
  SDL_Rect scale_first_wedge(const SDL_Rect &destination, float scale) {
    // The dimensions are positive, and nxdk's lroundf() is an unimplemented stub.
    const int width = static_cast<int>(destination.w * scale + 0.5f);
    const int height = static_cast<int>(destination.h * scale + 0.5f);
    return {destination.x + destination.w - width, destination.y + destination.h - height, width, height};
  }

  /**
   * @brief Load the eight splash wedges and scale them to the encoder-corrected viewport.
   *
   * @param screenSurface Window surface used for sizing.
   * @param videoMode Active Xbox display mode.
   * @param logo Receives the scaled wedges and their animation bounds.
   * @return True when all wedge surfaces are ready to blend onto the splash background.
   */
  bool load_splash_logo(const SDL_Surface &screenSurface, const VIDEO_MODE &videoMode, SplashLogo &logo) {
    const auto path = std::string(DATA_PATH) + "assets" + PATH_SEP + "moonlight-logo-wedges.svg";
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> atlas(IMG_Load(path.c_str()), SDL_DestroySurface);
    if (!atlas) {
      logging::error("splash", "Failed to load splash asset " + path + ": " + SDL_GetError());
      return false;
    }

    // Each atlas tile covers one quadrant. Keep its alpha channel when scaling it.
    const int tileSize = atlas->h;
    if (atlas->w != tileSize * static_cast<int>(logo.wedges.size())) {
      return SDL_SetError("Invalid splash wedge atlas dimensions");
    }
    if (!SDL_SetSurfaceBlendMode(atlas.get(), SDL_BLENDMODE_NONE)) {
      return false;
    }

    const auto layout = splash::calculate_logo_destination(screenSurface.w, screenSurface.h, tileSize * 2, tileSize * 2, videoMode, XVideoGetEncoderSettings());
    const int left = layout.width / 2;
    const int top = layout.height / 2;
    const int right = layout.width - left;
    const int bottom = layout.height - top;
    const SDL_Rect logoRect {layout.x, layout.y, layout.width, layout.height};
    const std::array<SDL_Rect, 4> quadrants {{
      {layout.x, layout.y, left, top},
      {layout.x + left, layout.y, right, top},
      {layout.x + left, layout.y + top, right, bottom},
      {layout.x, layout.y + top, left, bottom},
    }};
    for (std::size_t i = 0; i < logo.wedges.size(); ++i) {
      auto &wedge = logo.wedges[i];
      wedge.destination = quadrants[i / 2];
      wedge.pixels.reset(SDL_CreateSurface(wedge.destination.w, wedge.destination.h, atlas->format));
      const SDL_Rect tile {static_cast<int>(i) * tileSize, 0, tileSize, tileSize};
      if (!wedge.pixels || !SDL_BlitSurfaceScaled(atlas.get(), &tile, wedge.pixels.get(), nullptr, SDL_SCALEMODE_LINEAR) || !SDL_SetSurfaceBlendMode(wedge.pixels.get(), SDL_BLENDMODE_BLEND)) {
        return false;
      }
    }

    const auto expanded = scale_first_wedge(quadrants.front(), splash::SPLASH_MAX_WEDGE_SCALE);
    return SDL_GetRectUnion(&logoRect, &expanded, &logo.bounds);
  }

  /**
   * @brief Color and composite the wedges for a single splash frame.
   *
   * @param screenSurface Background-filled framebuffer.
   * @param logo Wedges and resting positions.
   * @param frame Scale and brightness values for this frame.
   * @return True when all wedges have been blitted.
   */
  bool draw_splash_logo(SDL_Surface *screenSurface, const SplashLogo &logo, const splash::SplashAnimationFrame &frame) {
    for (std::size_t i = 0; i < logo.wedges.size(); ++i) {
      const auto &wedge = logo.wedges[i];
      const auto brightness = frame.brightness[i];
      if (!SDL_SetSurfaceColorMod(wedge.pixels.get(), brightness, brightness, brightness)) {
        return false;
      }

      if (i == 0 && frame.firstWedgeScale > 1.0f) {
        const auto destination = scale_first_wedge(wedge.destination, frame.firstWedgeScale);
        if (!SDL_BlitSurfaceScaled(wedge.pixels.get(), nullptr, screenSurface, &destination, SDL_SCALEMODE_LINEAR)) {
          return false;
        }
      } else if (!SDL_BlitSurface(wedge.pixels.get(), nullptr, screenSurface, &wedge.destination)) {
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Present the boot splash on the CPU framebuffer while startup runs.
   *
   * @param window Startup window.
   * @param videoMode Active Xbox display mode.
   * @param keepShowing Startup predicate polled while the splash is visible.
   * @param animation Timeline that controls the splash artwork and exit.
   */
  void run_splash_screen(SDL_Window *window, const VIDEO_MODE &videoMode, const std::function<bool()> &keepShowing, splash::SplashAnimation animation) {  // NOSONAR(cpp:S5213) splash callback API is cold-path and already shared by callers
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
    SDL_Surface *screenSurface = SDL_GetWindowSurface(window);
    if (screenSurface == nullptr) {
      printSDLErrorAndReboot();
      return;
    }

    SplashLogo logo;
    if (!load_splash_logo(*screenSurface, videoMode, logo)) {
      printSDLErrorAndReboot();
      return;
    }

    const Uint32 background = SDL_MapSurfaceRGB(screenSurface, SPLASH_BACKGROUND_RED, SPLASH_BACKGROUND_GREEN, SPLASH_BACKGROUND_BLUE);
    const Uint64 startTicks = SDL_GetTicks();
    std::optional<splash::SplashAnimationFrame> lastFrame;
    bool done = false;
    do {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        done |= event.type == SDL_EVENT_QUIT;
      }
      const Uint64 elapsed = SDL_GetTicks() - startTicks;
      const auto frame = animation.advance(elapsed, !keepShowing());
      if (!lastFrame || lastFrame->firstWedgeScale != frame.firstWedgeScale || lastFrame->brightness != frame.brightness) {
        // Paint the full background once. Subsequent frames touch only the animated region.
        const SDL_Rect *dirty = lastFrame ? &logo.bounds : nullptr;
        if (!SDL_FillSurfaceRect(screenSurface, dirty, background) || !draw_splash_logo(screenSurface, logo, frame) || !(dirty ? SDL_UpdateWindowSurfaceRects(window, dirty, 1) : SDL_UpdateWindowSurface(window))) {
          printSDLErrorAndReboot();
          break;
        }
        lastFrame = frame;
      }
      done |= frame.finished;
      if (!done) {
        SDL_Delay(16);
      }
    } while (!done);

    // Release the splash framebuffer before creating the interactive renderer.
    SDL_DestroyWindowSurface(window);
  }

}  // namespace

namespace splash {

  void show_splash_screen(SDL_Window *window, const VIDEO_MODE &videoMode, unsigned int durationMilliseconds) {
    run_splash_screen(window, videoMode, []() {
      return true;
    },
                      SplashAnimation {durationMilliseconds});
  }

  void show_splash_screen(SDL_Window *window, const VIDEO_MODE &videoMode, const std::function<bool()> &keepShowing) {
    run_splash_screen(window, videoMode, keepShowing, SplashAnimation {});
  }

}  // namespace splash
