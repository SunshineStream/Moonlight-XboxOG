/**
 * @file src/ui/mobile_orientation_ios.mm
 * @brief Requests iOS interface rotation at the stream boundary.
 */
// class header include
#include "src/ui/mobile_orientation.h"

// lib includes
#import <UIKit/UIKit.h>

namespace ui {

  /**
   * @brief Update UIKit orientation support and request landscape when starting a stream.
   * @param window SDL window shared by the menu and stream.
   * @param streaming True while starting or running a stream.
   */
  void set_mobile_streaming_orientation(SDL_Window *window, bool streaming) {
    if (streaming) {
      SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    } else {
      SDL_ResetHint(SDL_HINT_ORIENTATIONS);
    }

    @autoreleasepool {
      UIWindow *nativeWindow = (__bridge UIWindow *) SDL_GetPointerProperty(
        SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,
        nullptr
      );
      if (@available(iOS 16.0, *)) {
        [nativeWindow.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
        if (streaming) {
          UIInterfaceOrientationMask landscape = UIInterfaceOrientationMaskLandscape;
          UIWindowSceneGeometryPreferencesIOS *preferences = [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:landscape];
          [nativeWindow.windowScene requestGeometryUpdateWithPreferences:preferences errorHandler:nil];
        }
      } else {
        [UIViewController attemptRotationToDeviceOrientation];
      }
    }
  }

}  // namespace ui
