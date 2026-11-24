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

  /**
   * @brief Access the packed activity state shared by save workers and UI readers.
   * @return Atomic generation in the upper word and active writer count in the lower word.
   */
  std::atomic<std::uint64_t> &save_state() {
    static std::atomic<std::uint64_t> state {0};
    return state;
  }
}  // namespace

namespace app {
  FileSaveActivity::FileSaveActivity() {
    auto &state = save_state();
    auto previous = state.load();
    for (;;) {
      const auto increment = static_cast<std::uint32_t>(previous) == 0 ? GENERATION_STEP + 1 : 1;
      if (state.compare_exchange_weak(previous, previous + increment)) {
        break;
      }
    }
  }

  FileSaveActivity::~FileSaveActivity() {
    save_state().fetch_sub(1);
  }

  FileSaveSnapshot file_save_snapshot() {
    const auto state = save_state().load();
    return {static_cast<std::uint32_t>(state >> 32), static_cast<std::uint32_t>(state) != 0};
  }
}  // namespace app
