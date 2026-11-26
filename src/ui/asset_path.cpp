/**
 * @file src/ui/asset_path.cpp
 * @brief Resolves bundled asset paths consistently across UI screens.
 */
// class header include
#include "src/ui/asset_path.h"

// standard includes
#include <algorithm>

#ifdef MOONLIGHT_NATIVE_CLIENT
  #include <SDL3/SDL.h>
#endif

// local includes
#include "src/os.h"

namespace ui {
  std::string build_asset_path(const std::string &relativePath) {
#ifdef MOONLIGHT_NATIVE_CLIENT
    std::string path = relativePath;
    std::replace(path.begin(), path.end(), '\\', '/');
  #ifdef __ANDROID__
    return "assets/" + path;
  #else
    const char *base = SDL_GetBasePath();
    const std::string root = base == nullptr ? "" : base;
    #ifdef MOONLIGHT_INSTALLED_ASSETS
    SDL_PathInfo info {};
    if (!SDL_GetPathInfo((root + "assets").c_str(), &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
      return std::string(MOONLIGHT_INSTALLED_ASSETS) + "/" + path;
    }
    #endif
    return root + "assets/" + path;
  #endif
#else
    std::string path = std::string(DATA_PATH) + "assets" + PATH_SEP + relativePath;
    // Xbox SDL rejects forward slashes even though native SDL accepts them.
    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
#endif
  }
}  // namespace ui
