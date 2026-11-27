/**
 * @file src/input/stream_input.h
 * @brief Platform-neutral stream input routing and host input lifetime management.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>

namespace input {
  /**
   * @brief Decide whether desktop gameplay should own the system pointer.
   * @param playing True while the stream is connected and running.
   * @param overlayVisible True while a local stream menu is open.
   * @param focused True while the stream window has input focus.
   * @return True when the window should use relative mouse input.
   */
  constexpr bool should_capture_stream_mouse(bool playing, bool overlayVisible, bool focused) {
    return playing && !overlayVisible && focused;
  }

  /**
   * @brief One complete Moonlight gamepad sample.
   */
  struct ControllerState {
    int buttonFlags = 0;  ///< Moonlight digital button flags.
    unsigned char leftTrigger = 0;  ///< Left trigger pressure.
    unsigned char rightTrigger = 0;  ///< Right trigger pressure.
    short leftStickX = 0;  ///< Left horizontal axis.
    short leftStickY = 0;  ///< Left vertical axis, positive upward.
    short rightStickX = 0;  ///< Right horizontal axis.
    short rightStickY = 0;  ///< Right vertical axis, positive upward.
  };

  /**
   * @brief Input packet type delivered to the transport adapter.
   */
  enum class StreamPacketType {
    arrival,  ///< Controller arrival and capability announcement.
    controller,  ///< Full controller sample or removal.
    key,  ///< Keyboard virtual key transition.
    mouse_button,  ///< Mouse button transition.
    mouse_move,  ///< Relative pointer movement.
    scroll,  ///< Horizontal and vertical wheel ticks.
  };

  /**
   * @brief Transport-independent input packet; unused fields remain zero.
   */
  struct StreamPacket {
    StreamPacketType type {};  ///< Selected transport operation.
    int slot = 0;  ///< Stable host controller index.
    std::uint16_t mask = 0;  ///< All connected host controller slots.
    ControllerState controller {};  ///< Complete controller state.
    int code = 0;  ///< Virtual key or Moonlight mouse button.
    bool pressed = false;  ///< Key or mouse button transition.
    int modifiers = 0;  ///< Moonlight keyboard modifier flags.
    int x = 0;  ///< Horizontal movement or scroll ticks.
    int y = 0;  ///< Vertical movement or scroll ticks.
  };

  /**
   * @brief Routes four stable controller slots and merges physical and virtual input sources.
   */
  class StreamInput {
  public:
    using Sink = std::function<void(const StreamPacket &)>;  ///< Transport packet consumer.
    /**
     * @brief Construct a router owned exclusively by the SDL event thread.
     * @param sink Receives ordered packets; Moonlight transport calls only enqueue data.
     */
    explicit StreamInput(Sink sink);
    /**
     * @brief Reserve the lowest free controller slot without renumbering connected pads.
     * @param device Unsigned SDL3 instance ID represented in the signed slot domain.
     * @return Slot index, or -1 when four controllers are already connected.
     */
    int connect(std::int64_t device);
    /**
     * @brief Release a disconnected pad and update the host's active mask.
     * @param device Unsigned SDL3 instance ID represented in the signed slot domain.
     */
    void disconnect(std::int64_t device);
    /**
     * @brief Forward a controller sample unless local controls own it.
     * @param device Unsigned SDL3 instance ID represented in the signed slot domain.
     * @param state Sampled state.
     */
    void controller(std::int64_t device, ControllerState state);
    /**
     * @brief Release host input before a menu or focus transition and suppress gameplay.
     * @param captured True while a local menu owns input.
     */
    void set_captured(bool captured);
    /**
     * @brief Change the controller that drives virtual mouse input.
     * @param device SDL instance ID, or -1 to disable mouse mode.
     */
    void set_mouse_device(std::int64_t device);
    /**
     * @brief Integrate stick movement at up to 125 reports/second, preserving fractional motion and immediate clicks.
     * @param milliseconds Elapsed event-thread time.
     */
    void tick(unsigned milliseconds);
    /**
     * @brief Merge a physical or virtual key transition by source.
     * @param source Unique input source ID.
     * @param code Win32 virtual key code, including the optional extended bit at 0x100.
     * @param pressed True for a press.
     * @param modifiers Moonlight modifiers.
     */
    void key(int source, int code, bool pressed, int modifiers = 0);
    /**
     * @brief Merge a physical or virtual mouse button transition by source.
     * @param source Unique input source ID.
     * @param button Moonlight button number.
     * @param pressed True for a press.
     */
    void mouse_button(int source, int button, bool pressed);
    /**
     * @brief Forward relative pointer movement.
     * @param x Horizontal delta.
     * @param y Vertical delta.
     */
    void mouse_move(int x, int y) const;
    /**
     * @brief Forward wheel ticks on both axes.
     * @param x Horizontal ticks.
     * @param y Vertical ticks.
     */
    void scroll(int x, int y) const;
    /**
     * @brief Release every key and mouse button belonging to one source.
     * @param source Source that disconnected or changed mode.
     */
    void release_source(int source);
    /**
     * @brief Release all held host input before transport shutdown.
     */
    void release_all();
    /**
     * @brief Read the connected controller bit mask.
     * @return Bits zero through three.
     */
    std::uint16_t active_mask() const;
    /**
     * @brief Resolve a host slot to its current local device.
     * @param slot Host controller number.
     * @return SDL instance ID, or -1 when unassigned.
     */
    std::int64_t slot_device(unsigned int slot) const;

  private:
    /**
     * @brief Stable controller slot state.
     */
    struct Slot {
      std::int64_t device = -1;  ///< SDL instance ID, or free.
      ControllerState last {};  ///< Last host state.
      ControllerState sampled {};  ///< Latest local state.
      bool blocked = false;  ///< Wait for neutral after capture ends.
    };

    /**
     * @brief Send a complete controller sample.
     * @param slot Host controller slot.
     * @param state State to send.
     */
    void send_controller(int slot, ControllerState state);
    /**
     * @brief Merge a shared digital input transition.
     * @param type Key or mouse packet type.
     * @param held Source sets per host input.
     * @param source Input source.
     * @param code Host input code.
     * @param pressed Transition direction.
     * @param modifiers Keyboard modifiers.
     */
    void digital(StreamPacketType type, std::map<int, std::set<int>> &held, int source, int code, bool pressed, int modifiers) const;
    Sink sink_;  ///< Ordered transport sink.
    std::array<Slot, 4> slots_ {};  ///< Stable slots, never compacted on removal.
    std::map<int, std::set<int>> keys_;  ///< Sources holding each host key.
    std::map<int, std::set<int>> buttons_;  ///< Sources holding each host mouse button.
    bool captured_ = false;  ///< Local UI currently owns gameplay input.
    std::int64_t mouseDevice_ = -1;  ///< Pad controlling the mouse, or disabled.
    double mouseX_ = 0;  ///< Fractional horizontal motion.
    double mouseY_ = 0;  ///< Fractional vertical motion.
    unsigned mouseElapsed_ = 0;  ///< Milliseconds accumulated toward the next 8 ms pointer report.
  };

  /**
   * @brief Map a USB HID/SDL scancode to a normalized US virtual key.
   * @param usage Keyboard usage page code.
   * @return Virtual key with 0x100 for extended keys, or zero for unsupported usages.
   */
  int keyboard_virtual_key(int usage);
  /**
   * @brief Invert an SDL axis without overflowing at -32768.
   * @param value SDL axis value.
   * @return Saturated inverted value.
   */
  short invert_stick_axis(short value);
}  // namespace input
