/**
 * @file src/platform/usb_input.cpp
 * @brief Bridges nxdk USB boot HID interfaces into SDL's keyboard and mouse event state.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

// lib includes
#include <SDL.h>

// nxdk includes
#include <xboxkrnl/xboxkrnl.h>

// lib includes
// usb.h depends on the Xbox configuration and must precede the HID class declarations.
extern "C" {
#include <usbh_hid.h>
#include <usbh_lib.h>
}

// local includes
#include "src/input/usb_hid.h"

// These are the vendored SDL2 internal event entrypoints. Calling them on the pump
// thread updates both SDL state and its queue; SDL_PushEvent alone cannot do that.
extern "C" {
  /**
   * @brief Update SDL keyboard state.
   *
   * @param state SDL press/release.
   * @param scancode USB usage.
   * @return SDL event result.
   */
  int SDL_SendKeyboardKey(Uint8 state, SDL_Scancode scancode);
  /**
   * @brief Update SDL pointer state.
   *
   * @param window Target window.
   * @param mouseID Pointer ID.
   * @param relative Relative mode.
   * @param x X delta.
   * @param y Y delta.
   * @return SDL event result.
   */
  int SDL_SendMouseMotion(SDL_Window *window, Uint32 mouseID, int relative, int x, int y);
  /**
   * @brief Update SDL mouse buttons.
   *
   * @param window Target window.
   * @param mouseID Pointer ID.
   * @param state Press/release.
   * @param button SDL button.
   * @return SDL event result.
   */
  int SDL_SendMouseButton(SDL_Window *window, Uint32 mouseID, Uint8 state, Uint8 button);
}

namespace {
  /**
   * @brief Fixed-size transfer copied at DPC level and decoded only by SDL's event thread.
   */
  struct UsbReport {
    int device = 0;  ///< Unique interface generation ID.
    int operation = 0;  ///< Zero for report, one/two for connect, minus one for disconnect.
    std::size_t length = 0;  ///< Valid report bytes.
    std::array<std::uint8_t, 8> bytes {};  ///< Maximum boot keyboard report.
  };

  /**
   * @brief Stable USB callback context; zero marks a reusable disconnected interface.
   */
  struct UsbConnection {
    int id = 0;  ///< Unique generation assigned when the interface connects.
  };

  /**
   * @brief Fixed storage shared by USB callbacks and the SDL event-thread consumer.
   */
  struct UsbInputState {
    std::array<UsbReport, 128> reports {};  ///< Single-CPU DPC-to-event-thread queue.
    unsigned head = 0;  ///< Next queue write index, guarded by DPC IRQL.
    unsigned tail = 0;  ///< Next queue read index, guarded by DPC IRQL.
    bool overflow = false;  ///< An overflow requires a full input release.
    int nextDevice = 1;  ///< Distinguishes reused USB interface addresses.
    std::array<UsbConnection, 128> connections {};  ///< Callback contexts never allocate at DPC level.
  };

  /**
   * @brief Access the statically initialized single-CPU USB bridge state.
   * @return Queue and callback contexts, guarded by DPC IRQL where required.
   */
  UsbInputState &usb_state() {
    static UsbInputState state {};
    return state;
  }

  /**
   * @brief Copy an operation without allocation or SDL calls from an interrupt callback.
   * @param report Boot report or lifecycle operation.
   */
  void enqueue(const UsbReport &report) {
    const KIRQL previous = KeRaiseIrqlToDpcLevel();
    if (const unsigned next = (usb_state().head + 1) % usb_state().reports.size(); next == usb_state().tail) {
      usb_state().overflow = true;
    } else {
      usb_state().reports[usb_state().head] = report;
      usb_state().head = next;
    }
    KfLowerIrql(previous);
  }

  /**
   * @brief Copy one successful interrupt report into the bounded queue.
   * @param device HID interface.
   * @param endpoint Interrupt endpoint address.
   * @param status Transfer result.
   * @param data Report bytes.
   * @param length Report length.
   */
  void receive(HID_DEV_T *device, std::uint16_t endpoint, int status, std::uint8_t *data, std::uint32_t length) {
    (void) endpoint;
    if (device == nullptr || device->user_data == nullptr) {
      return;
    }
    UsbReport report;
    report.device = static_cast<const UsbConnection *>(device->user_data)->id;
    if (status < 0) {
      report.operation = -1;
    } else {
      report.length = std::min<std::size_t>(length, report.bytes.size());
      if (data == nullptr) {
        return;
      }
      std::memcpy(report.bytes.data(), data, report.length);
    }
    enqueue(report);
  }

  /**
   * @brief Negotiate standard boot protocol and start one keyboard or mouse interface.
   * @param device New HID interface.
   * @param status Connection status.
   */
  void connected(HID_DEV_T *device, int status) {
    (void) status;
    if (device->bSubClassCode != 1 || (device->bProtocolCode != 1 && device->bProtocolCode != 2)) {
      return;
    }
    if (usbh_hid_set_protocol(device, 0) != 0) {
      return;
    }
    auto &connections = usb_state().connections;
    auto *context = std::find_if(connections.data(), connections.data() + connections.size(), [](const UsbConnection &entry) {
      return entry.id == 0;
    });
    if (context == connections.data() + connections.size()) {
      return;
    }
    const int id = usb_state().nextDevice++;
    context->id = id;
    device->user_data = context;
    enqueue({id, device->bProtocolCode});
    if (usbh_hid_start_int_read(device, 0, receive) != 0) {
      enqueue({id, -1});
      context->id = 0;
      device->user_data = nullptr;
    }
  }

  /**
   * @brief Queue releases before the USB driver frees a device's state.
   * @param device Removed HID interface.
   * @param status Disconnect status.
   */
  void disconnected(HID_DEV_T *device, int status) {
    (void) status;
    if (device->user_data != nullptr) {
      auto *context = static_cast<UsbConnection *>(device->user_data);
      enqueue({context->id, -1});
      context->id = 0;
      device->user_data = nullptr;
    }
  }

  /**
   * @brief Feed decoded aggregate transitions into SDL on the event thread.
   * @param event Validated boot HID input.
   */
  void dispatch(const input::BootHidEvent &event) {
    if (event.type == input::BootHidEventType::key) {
      SDL_SendKeyboardKey(event.pressed ? SDL_PRESSED : SDL_RELEASED, static_cast<SDL_Scancode>(event.code));
    } else if (event.type == input::BootHidEventType::motion) {
      SDL_SendMouseMotion(SDL_GetKeyboardFocus(), 0, 1, event.x, event.y);
    } else {
      const Uint8 button = std::array<Uint8, 3> {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE}.at(event.code - 1);
      SDL_SendMouseButton(SDL_GetKeyboardFocus(), 0, event.pressed ? SDL_PRESSED : SDL_RELEASED, button);
    }
  }

  /**
   * @brief Re-register connected HID interfaces after releasing an overflowed report queue.
   * @param input Event-thread boot HID decoder.
   */
  void reconnect_boot_devices(input::BootHidInput &input) {
    for (auto *device = usbh_hid_get_device_list(); device != nullptr; device = device->next) {
      if (device->user_data == nullptr) {
        continue;
      }
      input.connect(static_cast<const UsbConnection *>(device->user_data)->id, device->bProtocolCode);
    }
  }
}  // namespace

/**
 * @brief Register boot HID before nxdk's SDL joystick driver begins USB enumeration.
 */
extern "C" void moonlight_usb_input_initialize() {
  usbh_hid_init();
  usbh_install_hid_conn_callback(connected, disconnected);
}

/**
 * @brief Drain USB input from SDL's platform event pump without calling SDL at DPC level.
 */
extern "C" void moonlight_usb_input_pump() {
  static input::BootHidInput input(dispatch);
  for (;;) {
    UsbReport report;
    const KIRQL previous = KeRaiseIrqlToDpcLevel();
    const bool reset = usb_state().overflow;
    usb_state().overflow = false;
    if (reset) {
      usb_state().tail = usb_state().head;
    }
    const bool available = usb_state().tail != usb_state().head;
    if (available) {
      report = usb_state().reports[usb_state().tail];
      usb_state().tail = (usb_state().tail + 1) % usb_state().reports.size();
    }
    KfLowerIrql(previous);
    // Keep device identities after overflow; release their old state and accept future reports.
    if (reset) {
      input.clear();
      reconnect_boot_devices(input);
    }
    if (!available) {
      break;
    }
    if (report.operation == -1) {
      input.disconnect(report.device);
    } else if (report.operation != 0) {
      input.connect(report.device, report.operation);
    } else {
      input.report(report.device, report.bytes.data(), report.length);
    }
  }
}
