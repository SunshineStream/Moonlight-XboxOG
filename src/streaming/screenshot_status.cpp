/**
 * @file src/streaming/screenshot_status.cpp
 * @brief Implements throttled screenshot progress and automatic popup dismissal.
 */
#include "src/streaming/screenshot_status.h"

// standard includes
#include <algorithm>
#include <utility>

namespace streaming {
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
    return saving ? "Saving screenshot... " + std::to_string(percent) + "%" : saved ? "Screenshot saved" :
                                                                                      "Screenshot failed";
  }
}  // namespace streaming
