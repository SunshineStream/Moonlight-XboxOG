/**
 * @file src/input/ui_input.cpp
 * @brief Implements repeat timing without depending on SDL or platform key repeat.
 */
#include "src/input/ui_input.h"

#include <algorithm>
#include <cstdlib>

namespace input {
  bool is_gamepad_ui_activity(short value, bool trigger) {
    return trigger ? value >= 16000 : std::abs(static_cast<int>(value)) >= 16000;
  }

  bool is_repeatable_ui_command(UiCommand command) {
    switch (command) {
      case UiCommand::move_up:
      case UiCommand::move_down:
      case UiCommand::move_left:
      case UiCommand::move_right:
      case UiCommand::previous_page:
      case UiCommand::next_page:
      case UiCommand::fast_previous_page:
      case UiCommand::fast_next_page:
        return true;
      default:
        return false;
    }
  }

  UiCommand KeyboardNavigation::press(int key, UiCommand command, std::uint32_t now) {
    if (command == UiCommand::none || held_.count(key) != 0) {
      return UiCommand::none;
    }
    if (!is_repeatable_ui_command(command)) {
      // A held arrow must not carry through an activation into a different dialog.
      reset();
    }
    held_[key] = {command, now, now, ++order_};
    return command;
  }

  void KeyboardNavigation::release(int key) {
    held_.erase(key);
  }

  UiCommand KeyboardNavigation::poll(std::uint32_t now) {
    if (held_.empty()) {
      return UiCommand::none;
    }
    const auto newest = std::max_element(held_.begin(), held_.end(), [](const auto &a, const auto &b) {
      return a.second.order < b.second.order;
    });
    auto &key = newest->second;
    if (!is_repeatable_ui_command(key.command)) {
      return UiCommand::none;
    }
    if (now - key.pressedAt < NAVIGATION_INITIAL_REPEAT_MILLISECONDS || now - key.repeatedAt < NAVIGATION_REPEAT_MILLISECONDS) {
      return UiCommand::none;
    }
    key.repeatedAt = now;
    return key.command;
  }

  void KeyboardNavigation::reset() {
    held_.clear();
    order_ = 0;
  }
}  // namespace input
