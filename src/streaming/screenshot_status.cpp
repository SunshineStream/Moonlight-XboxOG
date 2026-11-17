/**
 * @file src/streaming/screenshot_status.cpp
 * @brief Implements throttled screenshot progress and automatic popup dismissal.
 */
// class header include
#include "src/streaming/screenshot_status.h"

// standard includes
#include <algorithm>
#include <utility>

namespace streaming {
  ScreenshotNotificationLayout layout_screenshot_notification(int width, int height, int lineHeight, int titleWidth, int detailWidth, bool saving) {
    if (width <= 16 || height <= 16 || lineHeight <= 0) {
      return {};
    }
    const int textWidth = std::max(titleWidth, saving ? 0 : detailWidth);
    return {std::min(width - 16, std::max(280, textWidth + 24)), std::min(height - 16, lineHeight * (saving ? 1 : 2) + (saving ? 28 : 32))};
  }

  void ScreenshotNotification::begin() {
    *this = {};
    visible = true;
    saving = true;
  }

  bool ScreenshotNotification::update(int progress) {
    if (!saving) {
      return false;
    }
    const int next = std::max(percent, std::clamp(progress, 0, 99) / 5 * 5);
    return std::exchange(percent, next) != next;
  }

  void ScreenshotNotification::finish(bool success, std::string message, std::uint32_t now) {
    visible = true;
    saving = false;
    saved = success;
    percent = success ? 100 : percent;
    detail = std::move(message);
    completedAt_ = now;
  }

  bool ScreenshotNotification::tick(std::uint32_t now) {
    if (!visible || saving || now - completedAt_ < (saved ? 5000U : 8000U)) {
      return false;
    }
    visible = false;
    return true;
  }

  std::string ScreenshotNotification::title() const {
    if (saving) {
      return "Saving screenshot... " + std::to_string(percent) + "%";
    }
    return saved ? "Screenshot saved" : "Screenshot failed";
  }

  std::string_view ScreenshotNotification::body() const {
    if (saving) {
      return {};
    }
    std::string_view text = detail;
    if (const std::string_view prefix = saved ? "Saved " : "Screenshot failed: "; text.substr(0, prefix.size()) == prefix) {
      text.remove_prefix(prefix.size());
    }
    return text;
  }
}  // namespace streaming
