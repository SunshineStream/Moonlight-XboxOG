/**
 * @file src/ui/asset_path.cpp
 * @brief Resolves bundled asset paths consistently across UI screens.
 */
#include "src/ui/asset_path.h"

#include "src/os.h"

#include <algorithm>

namespace ui {
  std::string build_asset_path(const std::string &relativePath) {
    std::string path = std::string(DATA_PATH) + "assets" + PATH_SEP + relativePath;
    // Xbox SDL rejects forward slashes even though native SDL accepts them.
    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
  }
}  // namespace ui
