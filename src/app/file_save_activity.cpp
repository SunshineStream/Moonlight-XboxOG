/**
 * @file src/app/file_save_activity.cpp
 * @brief Tracks file saves independently of the thread that writes the file.
 */
// class header include
#include "src/app/file_save_activity.h"

// standard includes
#include <atomic>

namespace {
  constexpr std::uint64_t GENERATION_STEP = std::uint64_t {1} << 32;  ///< Increment the upper half of the packed state.
  std::atomic<std::uint64_t> saveState {0};  ///< Save generation in the upper word and active writer count in the lower word.
}  // namespace

namespace app {
  FileSaveActivity::FileSaveActivity() {
    auto previous = saveState.load(std::memory_order_relaxed);
    for (;;) {
      const auto increment = static_cast<std::uint32_t>(previous) == 0 ? GENERATION_STEP + 1 : 1;
      if (saveState.compare_exchange_weak(previous, previous + increment, std::memory_order_relaxed)) {
        break;
      }
    }
  }

  FileSaveActivity::~FileSaveActivity() {
    saveState.fetch_sub(1, std::memory_order_relaxed);
  }

  FileSaveSnapshot file_save_snapshot() {
    const auto state = saveState.load(std::memory_order_relaxed);
    return {static_cast<std::uint32_t>(state >> 32), static_cast<std::uint32_t>(state) != 0};
  }
}  // namespace app
