/**
 * @file src/app/file_save_activity.h
 * @brief Tracks overlapping user-facing file saves across threads.
 */
#pragma once

// standard includes
#include <cstdint>

namespace app {
  /**
   * @brief A consistent view of the latest tracked save and the writers still running.
   */
  struct FileSaveSnapshot {
    std::uint32_t generation;  ///< Changes whenever a save begins while no other save is active.
    bool active;  ///< At least one tracked save is still active.
  };

  /**
   * @brief Keep a save active until its file is finalized or the attempt fails.
   */
  class FileSaveActivity {
  public:
    /** @brief Begin a save. */
    FileSaveActivity();
    /** @brief End this save without affecting other active saves. */
    ~FileSaveActivity();
    FileSaveActivity(const FileSaveActivity &) = delete;  ///< One owner per save.
    FileSaveActivity &operator=(const FileSaveActivity &) = delete;  ///< One owner per save.
  };

  /**
   * @brief Observe active writers and detect a save that began and finished between UI frames.
   * @return Current save generation and whether any tracked save is active.
   */
  FileSaveSnapshot file_save_snapshot();
}  // namespace app
