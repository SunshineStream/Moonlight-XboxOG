/**
 * @file src/input/usb_hid.h
 * @brief Safe USB boot keyboard and mouse report decoding.
 */
#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>

namespace input {
  /**
   * @brief Decoded boot HID input operation.
   */
  enum class BootHidEventType {
    key,  ///< Aggregated keyboard usage transition.
    button,  ///< Aggregated mouse button transition.
    motion,  ///< Relative mouse movement.
  };

  /**
   * @brief A validated boot HID event for the platform adapter.
   */
  struct BootHidEvent {
    BootHidEventType type {};  ///< Operation type.
    int code = 0;  ///< Keyboard usage or USB mouse button index starting at one.
    bool pressed = false;  ///< Digital transition.
    int x = 0;  ///< Relative X motion.
    int y = 0;  ///< Relative Y motion.
  };

  /**
   * @brief Tracks independent boot devices and merges shared keys and mouse buttons.
   */
  class BootHidInput {
  public:
    using Sink = std::function<void(const BootHidEvent &)>;  ///< Validated event consumer.
    /**
     * @brief Construct a boot report decoder.
     * @param sink Ordered event consumer on the event thread.
     */
    explicit BootHidInput(Sink sink);
    /**
     * @brief Add a keyboard or mouse in boot protocol.
     * @param device Unique device generation ID.
     * @param protocol One for keyboard, two for mouse.
     */
    void connect(int device, int protocol);
    /**
     * @brief Release all keys or buttons belonging to a removed device.
     * @param device Unique device generation ID.
     */
    void disconnect(int device);
    /**
     * @brief Decode a report without reading beyond its storage.
     * @param device Unique device generation ID.
     * @param bytes Report data.
     * @param length Available bytes; boot mice use only the standard first three bytes.
     */
    void report(int device, const std::uint8_t *bytes, std::size_t length);
    /**
     * @brief Release all devices after a platform queue overflow.
     */
    void clear();

  private:
    /**
     * @brief Retained state for one HID interface.
     */
    struct Device {
      int protocol = 0;  ///< Boot interface protocol.
      std::bitset<256> held;  ///< Keyboard usages or mouse buttons currently down.
    };

    /**
     * @brief Publish aggregate transitions after one device changes state.
     * @param device Device being updated.
     * @param next New held inputs.
     */
    void update(Device &device, const std::bitset<256> &next);
    Sink sink_;  ///< Validated event consumer.
    std::map<int, Device> devices_;  ///< Per-interface state.
  };
}  // namespace input
