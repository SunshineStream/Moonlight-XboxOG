/**
 * @file src/ui/save_indicator_timing.cpp
 * @brief Samples the save indicator independently of the renderer.
 */
// class header include
#include "src/ui/save_indicator_timing.h"

namespace ui {
  SaveIndicatorTiming::SaveIndicatorTiming(std::uint32_t generation):
      generation_(generation) {}

  std::optional<std::uint64_t> SaveIndicatorTiming::sample(std::uint64_t nowMilliseconds, app::FileSaveSnapshot save) {
    constexpr std::uint64_t minimumVisible = 500;  ///< Minimum display time in milliseconds.
    constexpr std::uint64_t frameInterval = 60;  ///< Time between indicator frame samples.
    if (save.generation != generation_ || (save.active && !visibleSince_.has_value())) {
      generation_ = save.generation;
      visibleSince_ = nowMilliseconds;
    }

    if (!visibleSince_.has_value() || (!save.active && nowMilliseconds - *visibleSince_ >= minimumVisible)) {
      visibleSince_.reset();
      return std::nullopt;
    }

    return nowMilliseconds - nowMilliseconds % frameInterval;
  }
}  // namespace ui
