/**
 * @file src/input/stream_input.cpp
 * @brief Routes ordered controller, keyboard and pointer input to Moonlight.
 */
#include "src/input/stream_input.h"

#include "third-party/moonlight-common-c/src/Limelight.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

  int StreamInput::connect(int device) {
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

  void StreamInput::disconnect(int device) {
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

  void StreamInput::controller(int device, ControllerState state) {
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

  void StreamInput::set_mouse_device(int device) {
    release_source(VIRTUAL_MOUSE_SOURCE);
    const int previous = mouseDevice_;
    mouseDevice_ = device;
    mouseX_ = mouseY_ = 0;
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
    for (const auto &slot : slots_) {
      if (slot.device != mouseDevice_ || slot.blocked) {
        continue;
      }
      const auto &state = slot.sampled;
      mouse_button(VIRTUAL_MOUSE_SOURCE, BUTTON_LEFT, (state.buttonFlags & A_FLAG) != 0);
      mouse_button(VIRTUAL_MOUSE_SOURCE, BUTTON_RIGHT, (state.buttonFlags & B_FLAG) != 0);
      const double seconds = std::min(milliseconds, 50U) / 1000.0;
      mouseX_ += mouse_axis(state.leftStickX) * 900 * seconds;
      mouseY_ -= mouse_axis(state.leftStickY) * 900 * seconds;
      const int x = static_cast<int>(mouseX_);
      const int y = static_cast<int>(mouseY_);
      mouseX_ -= x;
      mouseY_ -= y;
      mouse_move(x, y);
    }
  }

  void StreamInput::digital(StreamPacketType type, std::map<int, std::set<int>> &held, int source, int code, bool pressed, int modifiers) {
    if (code <= 0 || (captured_ && pressed)) {
      return;
    }
    auto &sources = held[code];
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

  void StreamInput::mouse_move(int x, int y) {
    if (!captured_ && (x != 0 || y != 0)) {
      StreamPacket packet {StreamPacketType::mouse_move};
      packet.x = std::clamp(x, -32768, 32767);
      packet.y = std::clamp(y, -32768, 32767);
      sink_(packet);
    }
  }

  void StreamInput::scroll(int x, int y) {
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
    switch (usage) {
      case 39:
        return '0';
      case 40:
        return 0x0D;
      case 41:
        return 0x1B;
      case 42:
        return 0x08;
      case 43:
        return 0x09;
      case 44:
        return 0x20;
      case 45:
        return 0xBD;
      case 46:
        return 0xBB;
      case 47:
        return 0xDB;
      case 48:
        return 0xDD;
      case 49:
      case 50:
      case 100:
        return 0xDC;
      case 51:
        return 0xBA;
      case 52:
        return 0xDE;
      case 53:
        return 0xC0;
      case 54:
        return 0xBC;
      case 55:
        return 0xBE;
      case 56:
        return 0xBF;
      case 57:
        return 0x14;
      case 70:
        return 0x12C;
      case 71:
        return 0x91;
      case 72:
        return 0x13;
      case 73:
        return 0x12D;
      case 74:
        return 0x124;
      case 75:
        return 0x121;
      case 76:
        return 0x12E;
      case 77:
        return 0x123;
      case 78:
        return 0x122;
      case 79:
        return 0x127;
      case 80:
        return 0x125;
      case 81:
        return 0x128;
      case 82:
        return 0x126;
      case 83:
        return 0x190;
      case 84:
        return 0x16F;
      case 85:
        return 0x6A;
      case 86:
        return 0x6D;
      case 87:
        return 0x6B;
      case 88:
        return 0x10D;
      case 98:
        return 0x60;
      case 99:
        return 0x6E;
      case 101:
        return 0x15D;
      case 224:
        return 0xA2;
      case 225:
        return 0xA0;
      case 226:
        return 0xA4;
      case 227:
        return 0x15B;
      case 228:
        return 0x1A3;
      case 229:
        return 0xA1;
      case 230:
        return 0x1A5;
      case 231:
        return 0x15C;
      default:
        return 0;
    }
  }
}  // namespace input
