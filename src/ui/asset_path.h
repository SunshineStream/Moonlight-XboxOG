/**
 * @file src/ui/asset_path.h
 * @brief Shared Xbox asset paths for shell and streaming UI.
 */
#pragma once

// standard includes
#include <string>

namespace ui {
  /**
   * @brief Resolve a bundled asset with the separators required by Xbox SDL file I/O.
   * @param relativePath Path inside the assets directory, using either separator.
   * @return Absolute Xbox path using backslashes throughout.
   */
  std::string build_asset_path(const std::string &relativePath);
}  // namespace ui
