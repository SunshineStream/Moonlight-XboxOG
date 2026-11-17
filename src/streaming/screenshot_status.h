/**
 * @file src/streaming/screenshot_status.h
 * @brief Tracks screenshot notification progress and completion lifetime without SDL dependencies.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <string_view>

namespace streaming {

  /**
   * @brief Content-sized screenshot popup constrained to the video safe area.
   */
  struct ScreenshotNotificationLayout {
    int width = 0;  ///< Panel width including text padding.
    int height = 0;  ///< One line while saving, two lines on completion.
  };

  /**
   * @brief Fit a notification to measured text, reserving a second line only for a completed result.
   * @param width Available video width.
   * @param height Available video height.
   * @param lineHeight Font line height.
   * @param titleWidth Measured single-line title width.
   * @param detailWidth Measured path or error width, ignored while saving.
   * @param saving True while a capture is pending.
   * @return Bounded panel dimensions, or an empty layout for an unusable viewport or font.
   */
  ScreenshotNotificationLayout layout_screenshot_notification(int width, int height, int lineHeight, int titleWidth, int detailWidth, bool saving);

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
    /**
     * @brief Return the saved path or error without repeating the popup title.
     * @return Single-line detail, empty while saving.
     */
    std::string_view body() const;

    bool visible = false;  ///< True while saving or before the completion timer expires.
    bool saving = false;  ///< True until a worker publishes its result.
    bool saved = false;  ///< Whether the completed image exists on disk.
    int percent = 0;  ///< Displayed progress, with 100 reserved for successful completion.
    std::string detail;  ///< Completed path or an actionable failure description.

  private:
    std::uint32_t completedAt_ = 0;  ///< Tick at which completion was observed by the UI.
  };
}  // namespace streaming
