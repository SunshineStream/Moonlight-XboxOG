/**
 * @file src/startup/storage_paths.cpp
 * @brief Implements persistent storage path helpers.
 */
// class header include
#include "src/startup/storage_paths.h"

// standard includes
#include <string>

#ifdef SUNSHINE_NATIVE_CLIENT
  #include <SDL3/SDL.h>
#endif

// nxdk includes
#if defined(__has_include) && __has_include(<nxdk/xbe.h>)
  #include <nxdk/xbe.h>
  #if __has_include(<nxdk/mount.h>)
    #include <nxdk/mount.h>
  #endif

  // standard includes
  #include <array>
  #include <cstdio>

  // platform includes
  #include <winnt.h>
#endif

namespace startup {

  std::string title_scoped_storage_root() {
#if defined(__has_include) && __has_include(<nxdk/xbe.h>)
  #if __has_include(<nxdk/mount.h>)
    if (!nxIsDriveMounted('E') && !nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\")) {
      return {};
    }
  #endif

    std::array<char, 9> titleIdBuffer {};
    std::snprintf(titleIdBuffer.data(), titleIdBuffer.size(), "%08X", CURRENT_XBE_HEADER->CertificateHeader->TitleID);
    return std::string("E:\\UDATA\\") + titleIdBuffer.data() + "\\";
#else
    return {};
#endif
  }

  std::string default_storage_path(std::string_view entryName) {
#ifdef SUNSHINE_NATIVE_CLIENT
    // Keep the original SDL directory so existing installations can still read their settings and pairing identity.
    static const auto root = [] {
      char *path = SDL_GetPrefPath("SunshineStream", "Moonlight");
      std::string result = path == nullptr ? "" : path;
      SDL_free(path);
      return result;
    }();
    return root + std::string(entryName);
#else
    if (const std::string titleScopedRoot = title_scoped_storage_root(); !titleScopedRoot.empty()) {
      return titleScopedRoot + std::string(entryName);
    }

    return std::string(entryName);
#endif
  }

  std::string screenshot_storage_path(std::string_view directory, std::string_view filename) {
    std::string path = directory.empty() ? default_storage_path("screenshots") : std::string(directory);
    if (path.back() != '/' && path.back() != '\\') {
      path += path.find('\\') == std::string::npos ? '/' : '\\';
    }
    return path + std::string(filename);
  }
}  // namespace startup
