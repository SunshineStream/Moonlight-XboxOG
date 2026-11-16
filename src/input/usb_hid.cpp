/**
 * @file src/input/usb_hid.cpp
 * @brief Decodes standard boot HID input with rollover and disconnect handling.
 */
#include "src/input/usb_hid.h"

#include <algorithm>
#include <utility>

namespace input {
  BootHidInput::BootHidInput(Sink sink):
      sink_(std::move(sink)) {}

  void BootHidInput::connect(int device, int protocol) {
    disconnect(device);
    if (protocol == 1 || protocol == 2) {
      devices_[device].protocol = protocol;
    }
  }

  void BootHidInput::update(Device &device, const std::bitset<256> &next) {
    const auto changed = device.held ^ next;
    const auto aggregate = [&]() {
      std::bitset<256> all;
      for (const auto &[id, other] : devices_) {
        if (other.protocol == device.protocol) {
          all |= other.held;
        }
      }
      return all;
    };
    const auto before = aggregate();
    device.held = next;
    const auto after = aggregate();
    // Modifiers precede regular keys so SDL generates correct key symbols and modifier flags.
    for (int pass = 0; pass < 2; ++pass) {
      for (int code = 0; code < 256; ++code) {
        if ((code >= 224) != (pass == 0) || !changed[code] || before[code] == after[code]) {
          continue;
        }
        sink_({device.protocol == 1 ? BootHidEventType::key : BootHidEventType::button, code, after[code]});
      }
    }
  }

  void BootHidInput::disconnect(int device) {
    const auto found = devices_.find(device);
    if (found != devices_.end()) {
      update(found->second, {});
      devices_.erase(found);
    }
  }

  void BootHidInput::clear() {
    while (!devices_.empty()) {
      disconnect(devices_.begin()->first);
    }
  }

  void BootHidInput::report(int device, const std::uint8_t *bytes, std::size_t length) {
    const auto found = devices_.find(device);
    if (found == devices_.end() || bytes == nullptr) {
      return;
    }
    auto &state = found->second;
    if (state.protocol == 1) {
      if (length < 8) {
        return;
      }
      auto held = state.held;
      for (int bit = 0; bit < 8; ++bit) {
        held[224 + bit] = (bytes[0] & (1U << bit)) != 0;
      }
      const bool rollover = std::any_of(bytes + 2, bytes + 8, [](std::uint8_t code) {
        return code >= 1 && code <= 3;
      });
      if (!rollover) {
        for (int code = 0; code < 224; ++code) {
          held[code] = false;
        }
        for (int i = 2; i < 8; ++i) {
          if (bytes[i] >= 4 && bytes[i] < 224) {
            held[bytes[i]] = true;
          }
        }
      }
      update(state, held);
    } else if (length >= 3) {
      std::bitset<256> held;
      // Boot protocol guarantees exactly three buttons and two signed 8-bit axes.
      // Wheel/report-ID layouts require descriptor parsing and must not be guessed.
      for (int bit = 0; bit < 3; ++bit) {
        held[bit + 1] = (bytes[0] & (1U << bit)) != 0;
      }
      update(state, held);
      const int x = bytes[1] < 128 ? bytes[1] : bytes[1] - 256;
      const int y = bytes[2] < 128 ? bytes[2] : bytes[2] - 256;
      if (x != 0 || y != 0) {
        sink_({BootHidEventType::motion, 0, false, x, y});
      }
    }
  }
}  // namespace input
