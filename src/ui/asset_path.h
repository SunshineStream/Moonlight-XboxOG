/**
 * @file src/ui/asset_path.h
 * @brief Shared asset paths for the shell and stream controls.
 */
#pragma once

// standard includes
#include <string>

namespace ui {
  /**
   * @brief Resolve a bundled asset for the current platform.
   * @param relativePath Path inside the assets directory, using either separator.
   * @return Absolute Xbox path using backslashes throughout.
   */
  std::string build_asset_path(const std::string &relativePath);
}  // namespace ui
