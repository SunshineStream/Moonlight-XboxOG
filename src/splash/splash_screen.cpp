/**
 * @file src/splash/splash_screen.cpp
 * @brief Implements the splash screen workflow.
 */
// class header include
#include "src/splash/splash_screen.h"

// standard includes
#include <optional>
#include <string>

// lib includes
#include <SDL3/SDL.h>

#ifdef __ANDROID__
  #include <jni.h>
#endif

#ifdef NXDK
  #include <hal/xbox.h>
#endif

// local includes
#include "src/logging/logger.h"
#include "src/platform/filesystem_utils.h"
#include "src/splash/splash_animation.h"
#include "src/splash/splash_layout.h"
#include "src/ui/brand_colors.h"
#include "src/ui/sunshine_logo.h"

namespace {

  /**
   * @brief Remove the Android window cover after SDL has a frame ready to display.
   */
  void reveal_splash_surface() {
#ifdef __ANDROID__
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    if (env == nullptr) {
      return;
    }
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (activity == nullptr) {
      return;
    }
    if (const auto activityClass = env->GetObjectClass(activity); activityClass != nullptr) {
      if (const auto reveal = env->GetMethodID(activityClass, "revealStartupSurface", "()V"); reveal != nullptr) {
        env->CallVoidMethod(activity, reveal);
      }
      env->DeleteLocalRef(activityClass);
    }
    env->DeleteLocalRef(activity);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      logging::warn("splash", "Could not remove the Android launch background.");
    }
#endif
  }

  /**
   * @brief Log a splash rendering error and reboot the original Xbox after a delay.
   */
  void printSDLErrorAndReboot() {
    logging::error("splash", std::string("SDL error: ") + SDL_GetError());
#ifdef NXDK
    logging::warn("splash", "Rebooting in 5 seconds.");
    SDL_Delay(5000);
    XReboot();
#endif
  }

  /**
   * @brief Find the centered logo region with the active framebuffer's aspect correction.
   * @param screenSurface Window surface used for sizing.
   * @param videoMode Active display mode.
   * @return Logo bounds in framebuffer pixels.
   */
  SDL_Rect splash_logo_bounds(const SDL_Surface &screenSurface, const VIDEO_MODE &videoMode) {
#ifdef NXDK
    const auto encoderSettings = XVideoGetEncoderSettings();
#else
    const auto encoderSettings = VIDEO_WIDESCREEN;
#endif
    constexpr int logoSize = 256;
    const auto layout = splash::calculate_logo_destination(screenSurface.w, screenSurface.h, logoSize, logoSize, videoMode, encoderSettings);
    return {layout.x, layout.y, layout.width, layout.height};
  }

  /**
   * @brief Present the boot splash and optionally save its final pose for desktop documentation.
   *
   * @param window Startup window.
   * @param videoMode Active Xbox display mode.
   * @param keepShowing Startup predicate polled while the splash is visible.
   * @param animation Timeline that controls the splash artwork and exit.
   */
  void run_splash_screen(SDL_Window *window, const VIDEO_MODE &videoMode, const std::function<bool()> &keepShowing, splash::SplashAnimation animation) {  // NOSONAR(cpp:S5213) splash callback API is cold-path and already shared by callers
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
    SDL_Surface *screenSurface = SDL_GetWindowSurface(window);
    ui::SunshineLogo logo;
    SDL_Rect bounds {};
    const auto loadLogo = [&] {
      if (screenSurface == nullptr) {
        return false;
      }
      bounds = splash_logo_bounds(*screenSurface, videoMode);
      return logo.load(bounds.w, bounds.h);
    };
    if (!loadLogo()) {
      printSDLErrorAndReboot();
      SDL_DestroyWindowSurface(window);
      reveal_splash_surface();
      return;
    }

    const Uint64 startTicks = SDL_GetTicks();
    std::optional<splash::LogoOpacity> lastOpacity;
    const SDL_WindowID windowId = SDL_GetWindowID(window);
    bool surfaceRevealed = false;
    bool done = false;
    do {
      bool resized = false;
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        done |= event.type == SDL_EVENT_QUIT;
        resized |= event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && event.window.windowID == windowId;
      }
      if (resized) {
        screenSurface = SDL_GetWindowSurface(window);
        if (!loadLogo()) {
          printSDLErrorAndReboot();
          break;
        }
        lastOpacity.reset();
      }
      const Uint64 elapsed = SDL_GetTicks() - startTicks;
      const auto frame = animation.advance(elapsed, !keepShowing());
      if (!lastOpacity || *lastOpacity != frame.opacity) {
        const auto color = ui::brand::BACKGROUND;
        const Uint32 background = SDL_MapSurfaceRGB(screenSurface, color.r, color.g, color.b);
        // Paint the full background once. Subsequent frames touch only the animated region.
        const SDL_Rect *dirty = lastOpacity ? &bounds : nullptr;
        const bool presented = SDL_FillSurfaceRect(screenSurface, dirty, background) &&
                               logo.draw(screenSurface, bounds, frame.opacity) &&
                               (dirty ? SDL_UpdateWindowSurfaceRects(window, dirty, 1) : SDL_UpdateWindowSurface(window));
        if (presented && !surfaceRevealed) {
          reveal_splash_surface();
          surfaceRevealed = true;
        }
        if (presented) {
          lastOpacity = frame.opacity;
        } else {
          printSDLErrorAndReboot();
          done = true;
        }
      }
      done |= frame.finished;
      if (!done) {
        SDL_Delay(16);
      }
    } while (!done);

#if defined(SUNSHINE_DESKTOP) && defined(SUNSHINE_SCREENSHOT_DEMO)
    const char *directory = SDL_getenv("SUNSHINE_SCREENSHOT_OUTPUT_DIR");
    if (directory != nullptr && *directory != '\0' && screenSurface != nullptr && lastOpacity.has_value()) {
      const std::string path = platform::join_path(directory, "desktop-splash.png");
      if (!SDL_SavePNG(screenSurface, path.c_str())) {
        logging::warn("splash", std::string("Could not save splash PNG: ") + SDL_GetError());
      }
    }
#endif

    // Release the splash framebuffer before creating the interactive renderer.
    SDL_DestroyWindowSurface(window);
    if (!surfaceRevealed) {
      reveal_splash_surface();
    }
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
