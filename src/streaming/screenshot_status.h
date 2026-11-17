/**
 * @file src/streaming/screenshot_status.h
 * @brief Tracks screenshot notification progress and completion lifetime without SDL dependencies.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>

namespace streaming {

  /**
   * @brief Keep saving feedback visible until completion, then dismiss it after a bounded interval.
   */
  class ScreenshotNotification {
  public:
    /**
     * @brief Start a new capture notification at zero percent.
     */
    void begin();
    /**
     * @brief Advance a pending capture, repainting only at five-percent boundaries.
     * @param percent Completed work, bounded below completion until the file and index are finalized.
     * @return True when the displayed progress changed.
     */
    bool update(int percent);
    /**
     * @brief Publish the final result and start its dismissal timer.
     * @param saved True when an image was saved successfully.
     * @param detail Saved path or failure information.
     * @param now Monotonic milliseconds, allowed to wrap.
     */
    void finish(bool saved, std::string detail, std::uint32_t now);
    /**
     * @brief Dismiss completed feedback after five seconds, or errors after eight seconds.
     * @param now Monotonic milliseconds, allowed to wrap.
     * @return True when visibility changed.
     */
    bool tick(std::uint32_t now);
    /**
     * @brief Describe current progress or the completed result for the popup title.
     * @return Human-readable notification title.
     */
    std::string title() const;

    bool visible = false;  ///< True while saving or before the completion timer expires.
    bool saving = false;  ///< True until a worker publishes its result.
    bool saved = false;  ///< Whether the completed image exists on disk.
    int percent = 0;  ///< Displayed progress, with 100 reserved for successful completion.
    std::string detail;  ///< Completed path or an actionable failure description.

  private:
    std::uint32_t completedAt_ = 0;  ///< Tick at which completion was observed by the UI.
  };
}  // namespace streaming
