/**
 * @file src/input/stream_menu.h
 * @brief Testable dual-stick stream menu and on-screen keyboard navigation.
 */
#pragma once

#include <array>
#include <string>

namespace input {
  /**
   * @brief Actions arranged clockwise from the top of each stick wheel.
   */
  enum class StreamAction {
    keyboard,  ///< Left wheel, up.
    mouse,  ///< Left wheel, right.
    logs,  ///< Left wheel, down.
    quit,  ///< Left wheel, left.
    guide,  ///< Right wheel, up.
    share,  ///< Right wheel, right.
    screenshot,  ///< Right wheel, down.
    statistics,  ///< Right wheel, left.
    none,  ///< No highlighted action.
  };
  /**
   * @brief Local stream control screen.
   */
  enum class StreamMenuPage {
    closed,  ///< Host owns input.
    radial,  ///< Two stick wheels.
    keyboard,  ///< On-screen keyboard.
    logs,  ///< Scrollable runtime log.
    quit_confirmation,  ///< Explicit confirmation before disconnecting.
  };

  /**
   * @brief A keyboard cell with an unshifted and shifted label.
   */
  struct StreamKeyboardKey {
    const char *label;  ///< Base label.
    const char *shifted;  ///< Shifted label.
    int code;  ///< Win32 virtual key, or modifier toggle code.
    int modifiers = 0;  ///< Required modifiers for a symbol or a named keyboard shortcut.
  };

  /**
   * @brief One focusable key in the five-row keyboard, measured in half-key column units.
   */
  struct StreamKeyboardCell {
    int key = 0;  ///< Index in the host key catalog, or KEYBOARD_PAGE_KEY for the alternate layout.
    int column = 0;  ///< Left column in the 26-unit keyboard.
    int row = 0;  ///< Top row in the five-row keyboard.
    int width = 2;  ///< Horizontal span in half-key units.
    int height = 1;  ///< Vertical span in rows.
  };

  /**
   * @brief Dual-wheel selection with persistent keyboard and log navigation.
   */
  class StreamMenu {
  public:
    /**
     * @brief Open the menu on the controller that invoked the shortcut.
     * @param device SDL instance ID, or -1 for keyboard control.
     */
    void open(int device);
    /**
     * @brief Close local controls and reset the highlighted action.
     */
    void close();
    /**
     * @brief Select a cardinal sector with either stick; centered sticks retain selection.
     * @param right True for the right wheel.
     * @param x Stick X, positive rightward.
     * @param y Stick Y, positive upward.
     * @return True only when the highlighted action changes and needs repainting.
     */
    bool select_stick(bool right, short x, short y);
    /**
     * @brief Update wheel selection from changed sampled axes, retaining the last active stick.
     * @param leftX Left-stick X.
     * @param leftY Left-stick Y, positive upward.
     * @param rightX Right-stick X.
     * @param rightY Right-stick Y, positive upward.
     * @return True when selection changes.
     */
    bool sample_sticks(short leftX, short leftY, short rightX, short rightY);
    /**
     * @brief Move keyboard focus with wraparound, or scroll logs.
     * @param x Horizontal step.
     * @param y Vertical step, positive downward.
     */
    void navigate(int x, int y);
    /**
     * @brief Return the selected on-screen key and apply modifier toggles.
     * @return A virtual key to tap, or zero for a modifier toggle.
     */
    int keyboard_key();
    /**
     * @brief Return a cell's label according to the current Caps and one-shot Shift state.
     * @param index Cell index.
     * @return Visible label.
     */
    std::string key_label(int index) const;
    /**
     * @brief Get the current letters, symbols or desktop-shortcuts layout.
     * @return Focusable cells, including wide space and tall side controls.
     */
    const std::array<StreamKeyboardCell, 47> &keyboard_cells() const;
    /**
     * @brief Toggle the alternate keyboard without losing the focused key's visual location.
     */
    void toggle_keyboard_page();
    /**
     * @brief Compute host modifiers, preserving explicit symbols-page bindings regardless of one-shot Shift.
     * @param code Host virtual key to send.
     * @return Moonlight modifier flags.
     */
    int key_modifiers(int code) const;
    /**
     * @brief Toggle Shift for the next host key without changing Caps.
     */
    void toggle_shift();
    /**
     * @brief Consume one-shot Shift after sending a host key.
     */
    void key_sent();
    /**
     * @brief Complete the disconnect dialog directly, without directional selection.
     * @param accept True for A/Enter to disconnect; false for B/Escape to cancel.
     * @return True only for an accepted disconnect on the confirmation page.
     */
    bool confirm_disconnect(bool accept);
    StreamMenuPage page = StreamMenuPage::closed;  ///< Current screen.
    StreamAction selected = StreamAction::none;  ///< Highlighted wheel action.
    int owner = -1;  ///< Controller that opened local controls.
    int keyIndex = 10;  ///< Selected visual cell; the letter Q is focused initially.
    int keyboardLayout = 0;  ///< Letters, symbols, or desktop keys and shortcuts (0 through 2).
    bool shiftOnce = false;  ///< Shift applies to the next host key only.
    bool caps = false;  ///< Persistent uppercase letters, independent of the host Caps Lock state.
    int modifiers = 0;  ///< Latched Moonlight modifier toggles.
    int logOffset = 0;  ///< Lines scrolled backward from the newest log.
    static constexpr int KEYBOARD_COLUMNS = 26;  ///< Half-key column units, including both side control rails.
    static constexpr int KEYBOARD_ROWS = 5;  ///< Four character rows and one space-bar row.
    static constexpr int KEYBOARD_PAGE_KEY = 128;  ///< Layout toggle, never sent to the host.
    static constexpr int KEYBOARD_CAPS_KEY = 129;  ///< Local Caps toggle, never sent to the host.
    static constexpr int WHEEL_DEADZONE = 9000;  ///< Deliberate wheel movement without requiring a half-stick deflection.
    static const std::array<StreamKeyboardKey, 118> KEYS;  ///< Host keys, explicit symbols and common desktop shortcuts.

  private:
    /**
     * @brief Resolve a cell's binding, applying Caps and one-shot Shift outside the explicit symbols page.
     * @param index Host key catalog index.
     * @return Effective modifiers for that binding.
     */
    int binding_modifiers(int index) const;
    int keyboardColumn_ = 4;  ///< Preferred column retained while traversing wide keys.
    int keyboardRow_ = 1;  ///< Preferred row retained while traversing tall side controls.
    std::array<short, 4> previousSticks_ {};  ///< Previous sampled axes so a stationary stick cannot steal the other wheel's selection.
  };

  /**
   * @brief Return the visible wheel action label.
   * @param action Action to label.
   * @param mouseEnabled Current virtual mouse state.
   * @param statisticsEnabled Current statistics state.
   * @return Visible action label.
   */
  const char *stream_action_label(StreamAction action, bool mouseEnabled, bool statisticsEnabled);
}  // namespace input
