/**
 * @file src/input/ui_input.cpp
 * @brief Implements repeat timing without depending on SDL or platform key repeat.
 */
// class header include
#include "src/input/ui_input.h"

// standard includes
#include <algorithm>
#include <cstdlib>

namespace input {
  UiCommand direction_command(int x, int y) {
    if (x != 0) {
      return x > 0 ? UiCommand::move_right : UiCommand::move_left;
    }
    if (y != 0) {
      return y > 0 ? UiCommand::move_down : UiCommand::move_up;
    }
    return UiCommand::none;
  }

  std::pair<int, int> navigation_step(UiCommand command) {
    switch (command) {
      case UiCommand::move_left:
        return {-1, 0};
      case UiCommand::move_right:
        return {1, 0};
      case UiCommand::move_up:
        return {0, -1};
      case UiCommand::move_down:
        return {0, 1};
      default:
        return {0, 0};
    }
  }

  UiCommand DirectionNavigation::update_stick(short x, short y, std::uint32_t now) {
    const int horizontal = std::abs(static_cast<int>(x));
    const int vertical = std::abs(static_cast<int>(y));
    const auto direction = horizontal > vertical ? direction_command(x, 0) : direction_command(0, -static_cast<int>(y));
    const int threshold = direction == command_ ? STICK_NAVIGATION_RELEASE_THRESHOLD : STICK_NAVIGATION_THRESHOLD;
    return update(std::max(horizontal, vertical) >= threshold ? direction : UiCommand::none, now);
  }

  UiCommand DirectionNavigation::update(UiCommand command, std::uint32_t now) {
    if (command == UiCommand::none || !is_repeatable_ui_command(command)) {
      reset();
      return UiCommand::none;
    }
    if (command != command_) {
      command_ = command;
      pressedAt_ = now;
      repeatedAt_ = now;
      repeated_ = false;
      return command;
    }
    if ((!repeated_ && now - pressedAt_ >= NAVIGATION_INITIAL_REPEAT_MILLISECONDS) || (repeated_ && now - repeatedAt_ >= NAVIGATION_REPEAT_MILLISECONDS)) {
      repeatedAt_ = now;
      repeated_ = true;
      return command;
    }
    return UiCommand::none;
  }

  void DirectionNavigation::reset() {
    *this = {};
  }

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
