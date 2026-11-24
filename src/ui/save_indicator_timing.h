/**
 * @file src/ui/save_indicator_timing.h
 * @brief Controls when a save indicator is visible and which frame it displays.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>

// local includes
#include "src/app/file_save_activity.h"

namespace ui {
  /**
   * @brief Keep a save visible long enough to notice, including a write that finished between samples.
   */
  class SaveIndicatorTiming {
  public:
    /**
     * @brief Start observing new saves from the current save generation.
     * @param generation Save generation already observed when the UI starts.
     */
    explicit SaveIndicatorTiming(std::uint32_t generation);

    /**
     * @brief Sample the indicator while a save is active or within its minimum display time.
     * @param nowMilliseconds Monotonic UI clock.
     * @param save Current save activity snapshot.
     * @return Animation time sampled at the indicator frame rate, or no frame when hidden.
     */
    std::optional<std::uint64_t> sample(std::uint64_t nowMilliseconds, app::FileSaveSnapshot save);

  private:
    std::uint32_t generation_;  ///< Last save generation observed by the UI.
    std::optional<std::uint64_t> visibleSince_;  ///< First sample for the latest observed save.
  };
}  // namespace ui
