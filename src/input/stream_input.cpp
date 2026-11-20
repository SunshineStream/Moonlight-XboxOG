/**
 * @file src/input/stream_input.cpp
 * @brief Routes ordered controller, keyboard and pointer input to Moonlight.
 */
// class header include
#include "src/input/stream_input.h"

// standard includes
#include <algorithm>
#include <cmath>
#include <utility>

// lib includes
#include <Limelight.h>

namespace {
  constexpr int VIRTUAL_MOUSE_SOURCE = -2;  ///< Reserved source for controller pointer buttons.

  /**
   * @brief Compare every component of a host gamepad sample.
   * @param a First sample.
   * @param b Second sample.
   * @return True when every component matches.
   */
  bool equal(const input::ControllerState &a, const input::ControllerState &b) {
    return a.buttonFlags == b.buttonFlags && a.leftTrigger == b.leftTrigger && a.rightTrigger == b.rightTrigger && a.leftStickX == b.leftStickX && a.leftStickY == b.leftStickY && a.rightStickX == b.rightStickX && a.rightStickY == b.rightStickY;
  }

  /**
   * @brief Rescale an axis outside the mouse dead zone.
   * @param value Signed axis value.
   * @return Normalized deflection with a central dead zone.
   */
  double mouse_axis(short value) {
    constexpr int deadzone = 7000;
    const int magnitude = std::abs(static_cast<int>(value));
    return magnitude <= deadzone ? 0.0 : std::copysign((std::min(magnitude, 32767) - deadzone) / 25767.0, value);
  }
}  // namespace

namespace input {
  StreamInput::StreamInput(Sink sink):
      sink_(std::move(sink)) {}

  std::uint16_t StreamInput::active_mask() const {
    std::uint16_t mask = 0;
    for (int i = 0; i < 4; ++i) {
      if (slots_[i].device != -1) {
        mask |= 1U << i;
      }
    }
    return mask;
  }

  int StreamInput::connect(std::int64_t device) {
    if (device < 0) {
      return -1;
    }
    for (int i = 0; i < 4; ++i) {
      if (slots_[i].device == device) {
        return i;
      }
    }
    for (int i = 0; i < 4; ++i) {
      if (slots_[i].device == -1) {
        slots_[i] = {};
        slots_[i].device = device;
        slots_[i].blocked = captured_;
        sink_({StreamPacketType::arrival, i, active_mask()});
        send_controller(i, {});
        return i;
      }
    }
    return -1;
  }

  void StreamInput::send_controller(int slot, ControllerState state) {
    sink_({StreamPacketType::controller, slot, active_mask(), state});
    slots_[slot].last = state;
  }

  void StreamInput::disconnect(std::int64_t device) {
    for (int i = 0; i < 4; ++i) {
      if (slots_[i].device == device) {
        if (mouseDevice_ == device) {
          set_mouse_device(-1);
        }
        slots_[i] = {};
        send_controller(i, {});
      }
    }
  }

  void StreamInput::controller(std::int64_t device, ControllerState state) {
    for (int i = 0; i < 4; ++i) {
      auto &slot = slots_[i];
      if (slot.device != device) {
        continue;
      }
      slot.sampled = state;
      // Local buttons must be released, and sticks centered, before gameplay resumes.
      const bool neutral = state.buttonFlags == 0 && state.leftTrigger == 0 && state.rightTrigger == 0 && std::abs(static_cast<int>(state.leftStickX)) < 7000 && std::abs(static_cast<int>(state.leftStickY)) < 7000 && std::abs(static_cast<int>(state.rightStickX)) < 7000 && std::abs(static_cast<int>(state.rightStickY)) < 7000;
      if (captured_) {
        slot.blocked = true;
      } else if (neutral) {
        slot.blocked = false;
      }
      if (!captured_ && !slot.blocked && mouseDevice_ == device) {
        mouse_button(VIRTUAL_MOUSE_SOURCE, BUTTON_LEFT, (state.buttonFlags & A_FLAG) != 0);
        mouse_button(VIRTUAL_MOUSE_SOURCE, BUTTON_RIGHT, (state.buttonFlags & B_FLAG) != 0);
      }
      if (captured_ || slot.blocked || mouseDevice_ == device) {
        state = {};
      }
      if (!equal(state, slot.last)) {
        send_controller(i, state);
      }
    }
  }

  void StreamInput::set_captured(bool captured) {
    if (captured_ == captured) {
      return;
    }
    release_all();
    captured_ = captured;
    for (auto &slot : slots_) {
      slot.blocked = true;
    }
  }

  void StreamInput::set_mouse_device(std::int64_t device) {
    release_source(VIRTUAL_MOUSE_SOURCE);
    const auto previous = mouseDevice_;
    mouseDevice_ = device;
    mouseX_ = 0;
    mouseY_ = 0;
    mouseElapsed_ = 0;
    for (int i = 0; i < 4; ++i) {
      if (slots_[i].device != -1 && (slots_[i].device == previous || slots_[i].device == device)) {
        if (!equal(slots_[i].last, {})) {
          send_controller(i, {});
        }
        slots_[i].blocked = true;
      }
    }
  }

  void StreamInput::tick(unsigned milliseconds) {
    if (captured_ || mouseDevice_ < 0) {
      return;
    }
    mouseElapsed_ = std::min(50U, mouseElapsed_ + std::min(milliseconds, 50U));
    if (mouseElapsed_ < 8) {
      return;
    }
    const double seconds = mouseElapsed_ / 1000.0;
    mouseElapsed_ = 0;
    for (const auto &slot : slots_) {
      if (slot.device != mouseDevice_ || slot.blocked) {
        continue;
      }
      const auto &state = slot.sampled;
      mouse_button(VIRTUAL_MOUSE_SOURCE, BUTTON_LEFT, (state.buttonFlags & A_FLAG) != 0);
      mouse_button(VIRTUAL_MOUSE_SOURCE, BUTTON_RIGHT, (state.buttonFlags & B_FLAG) != 0);
      mouseX_ += mouse_axis(state.leftStickX) * 900 * seconds;
      mouseY_ -= mouse_axis(state.leftStickY) * 900 * seconds;
      const auto x = static_cast<int>(mouseX_);
      const auto y = static_cast<int>(mouseY_);
      mouseX_ -= x;
      mouseY_ -= y;
      mouse_move(x, y);
    }
  }

  void StreamInput::digital(StreamPacketType type, std::map<int, std::set<int>> &held, int source, int code, bool pressed, int modifiers) const {
    if (code <= 0 || (captured_ && pressed)) {
      return;
    }
    auto entry = held.find(code);
    if (entry == held.end()) {
      if (!pressed) {
        return;
      }
      entry = held.try_emplace(code).first;
    }
    auto &sources = entry->second;
    const bool wasPressed = !sources.empty();
    if (pressed) {
      sources.insert(source);
    } else {
      sources.erase(source);
    }
    if (wasPressed != !sources.empty()) {
      StreamPacket packet {type};
      packet.code = code;
      packet.pressed = !sources.empty();
      packet.modifiers = modifiers;
      sink_(packet);
    }
    if (sources.empty()) {
      held.erase(code);
    }
  }

  void StreamInput::key(int source, int code, bool pressed, int modifiers) {
    digital(StreamPacketType::key, keys_, source, code, pressed, modifiers);
  }

  void StreamInput::mouse_button(int source, int button, bool pressed) {
    digital(StreamPacketType::mouse_button, buttons_, source, button, pressed, 0);
  }

  void StreamInput::mouse_move(int x, int y) const {
    if (!captured_ && (x != 0 || y != 0)) {
      StreamPacket packet {StreamPacketType::mouse_move};
      packet.x = std::clamp(x, -32768, 32767);
      packet.y = std::clamp(y, -32768, 32767);
      sink_(packet);
    }
  }

  void StreamInput::scroll(int x, int y) const {
    if (!captured_ && (x != 0 || y != 0)) {
      StreamPacket packet {StreamPacketType::scroll};
      packet.x = std::clamp(x, -127, 127);
      packet.y = std::clamp(y, -127, 127);
      sink_(packet);
    }
  }

  void StreamInput::release_source(int source) {
    // Collect codes first because the transition removes empty map entries.
    for (auto *held : {&keys_, &buttons_}) {
      std::set<int> codes;
      for (const auto &[code, sources] : *held) {
        if (sources.count(source) != 0) {
          codes.insert(code);
        }
      }
      for (int code : codes) {
        digital(held == &keys_ ? StreamPacketType::key : StreamPacketType::mouse_button, *held, source, code, false, 0);
      }
    }
  }

  void StreamInput::release_all() {
    mouseElapsed_ = 0;
    for (int i = 0; i < 4; ++i) {
      if (slots_[i].device != -1 && !equal(slots_[i].last, {})) {
        send_controller(i, {});
      }
    }
    for (auto *held : {&keys_, &buttons_}) {
      for (const auto &[code, sources] : *held) {
        StreamPacket packet {held == &keys_ ? StreamPacketType::key : StreamPacketType::mouse_button};
        packet.code = code;
        sink_(packet);
      }
      held->clear();
    }
  }

  short invert_stick_axis(short value) {
    return static_cast<short>(std::min(-static_cast<int>(value), 32767));
  }

  int keyboard_virtual_key(int usage) {
    if (usage >= 4 && usage <= 29) {
      return 'A' + usage - 4;
    }
    if (usage >= 30 && usage <= 38) {
      return '1' + usage - 30;
    }
    if (usage >= 58 && usage <= 69) {
      return 0x70 + usage - 58;
    }
    if (usage >= 104 && usage <= 115) {
      return 0x7C + usage - 104;
    }
    if (usage >= 89 && usage <= 97) {
      return 0x61 + usage - 89;
    }
    constexpr std::array<std::pair<int, int>, 50> overrides {{
      {39, '0'},
      {40, 0x0D},
      {41, 0x1B},
      {42, 0x08},
      {43, 0x09},
      {44, 0x20},
      {45, 0xBD},
      {46, 0xBB},
      {47, 0xDB},
      {48, 0xDD},
      {49, 0xDC},
      {50, 0xDC},
      {51, 0xBA},
      {52, 0xDE},
      {53, 0xC0},
      {54, 0xBC},
      {55, 0xBE},
      {56, 0xBF},
      {57, 0x14},
      {70, 0x12C},
      {71, 0x91},
      {72, 0x13},
      {73, 0x12D},
      {74, 0x124},
      {75, 0x121},
      {76, 0x12E},
      {77, 0x123},
      {78, 0x122},
      {79, 0x127},
      {80, 0x125},
      {81, 0x128},
      {82, 0x126},
      {83, 0x190},
      {84, 0x16F},
      {85, 0x6A},
      {86, 0x6D},
      {87, 0x6B},
      {88, 0x10D},
      {98, 0x60},
      {99, 0x6E},
      {100, 0xDC},
      {101, 0x15D},
      {224, 0xA2},
      {225, 0xA0},
      {226, 0xA4},
      {227, 0x15B},
      {228, 0x1A3},
      {229, 0xA1},
      {230, 0x1A5},
      {231, 0x15C},
    }};
    const auto entry = std::lower_bound(overrides.begin(), overrides.end(), usage, [](const auto &binding, int key) {
      return binding.first < key;
    });
    return entry != overrides.end() && entry->first == usage ? entry->second : 0;
  }
}  // namespace input
