/**
 * @file src/input/stream_menu.cpp
 * @brief Implements radial selection and host keyboard navigation.
 */
// class header include
#include "src/input/stream_menu.h"

// standard includes
#include <algorithm>
#include <cstdlib>

// lib includes
#include "third-party/moonlight-common-c/src/Limelight.h"

namespace {
  /**
   * @brief Arrange a central character grid with Xbox-style side controls and a wide space bar.
   * @param keys Host key catalog indices for the four character rows.
   * @return Complete focusable keyboard geometry.
   */
  std::array<input::StreamKeyboardCell, 47> make_keyboard_cells(const std::array<int, 40> &keys) {
    std::array<input::StreamKeyboardCell, 47> cells {};
    for (int i = 0; i < 40; ++i) {
      cells[i] = {keys[i], 3 + (i % 10) * 2, i / 10, 2, 1};
    }
    cells[40] = {44, 3, 4, 20, 1};
    cells[41] = {input::StreamMenu::KEYBOARD_PAGE_KEY, 0, 0, 3, 1};
    cells[42] = {57, 0, 1, 3, 2};
    cells[43] = {input::StreamMenu::KEYBOARD_CAPS_KEY, 0, 3, 3, 2};
    cells[44] = {47, 23, 0, 3, 1};
    cells[45] = {59, 23, 1, 3, 2};
    cells[46] = {46, 23, 3, 3, 2};
    return cells;
  }
}  // namespace

namespace input {
  const std::array<StreamKeyboardKey, 118> StreamMenu::KEYS {{
    {"1", "!", '1'},
    {"2", "@", '2'},
    {"3", "#", '3'},
    {"4", "$", '4'},
    {"5", "%", '5'},
    {"6", "^", '6'},
    {"7", "&", '7'},
    {"8", "*", '8'},
    {"9", "(", '9'},
    {"0", ")", '0'},
    {"q", "Q", 'Q'},
    {"w", "W", 'W'},
    {"e", "E", 'E'},
    {"r", "R", 'R'},
    {"t", "T", 'T'},
    {"y", "Y", 'Y'},
    {"u", "U", 'U'},
    {"i", "I", 'I'},
    {"o", "O", 'O'},
    {"p", "P", 'P'},
    {"a", "A", 'A'},
    {"s", "S", 'S'},
    {"d", "D", 'D'},
    {"f", "F", 'F'},
    {"g", "G", 'G'},
    {"h", "H", 'H'},
    {"j", "J", 'J'},
    {"k", "K", 'K'},
    {"l", "L", 'L'},
    {";", ":", 0xBA},
    {"z", "Z", 'Z'},
    {"x", "X", 'X'},
    {"c", "C", 'C'},
    {"v", "V", 'V'},
    {"b", "B", 'B'},
    {"n", "N", 'N'},
    {"m", "M", 'M'},
    {",", "<", 0xBC},
    {".", ">", 0xBE},
    {"/", "?", 0xBF},
    {"Shift", "Shift", -MODIFIER_SHIFT},
    {"Ctrl", "Ctrl", -MODIFIER_CTRL},
    {"Alt", "Alt", -MODIFIER_ALT},
    {"Win", "Win", -MODIFIER_META},
    {"Space", "Space", 0x20},
    {"Tab", "Tab", 0x09},
    {"Enter", "Enter", 0x0D},
    {"Bksp", "Bksp", 0x08},
    {"Del", "Del", 0x12E},
    {"Esc", "Esc", 0x1B},
    {"-", "_", 0xBD},
    {"=", "+", 0xBB},
    {"[", "{", 0xDB},
    {"]", "}", 0xDD},
    {"\\", "|", 0xDC},
    {"'", "\"", 0xDE},
    {"`", "~", 0xC0},
    {"Left", "Left", 0x125},
    {"Down", "Down", 0x128},
    {"Right", "Right", 0x127},
    {"F1", "F1", 0x70},
    {"F2", "F2", 0x71},
    {"F3", "F3", 0x72},
    {"F4", "F4", 0x73},
    {"F5", "F5", 0x74},
    {"F6", "F6", 0x75},
    {"Home", "Home", 0x124},
    {"End", "End", 0x123},
    {"Up", "Up", 0x126},
    {"PgDn", "PgDn", 0x122},
    {"F7", "F7", 0x76},
    {"F8", "F8", 0x77},
    {"F9", "F9", 0x78},
    {"F10", "F10", 0x79},
    {"F11", "F11", 0x7A},
    {"F12", "F12", 0x7B},
    {"PgUp", "PgUp", 0x121},
    {"Ins", "Ins", 0x12D},
    {"Caps", "Caps", 0x14},
    {"PrtSc", "PrtSc", 0x12C},
    {"!", "!", '1', MODIFIER_SHIFT},
    {"@", "@", '2', MODIFIER_SHIFT},
    {"#", "#", '3', MODIFIER_SHIFT},
    {"$", "$", '4', MODIFIER_SHIFT},
    {"%", "%", '5', MODIFIER_SHIFT},
    {"^", "^", '6', MODIFIER_SHIFT},
    {"&", "&", '7', MODIFIER_SHIFT},
    {"*", "*", '8', MODIFIER_SHIFT},
    {"(", "(", '9', MODIFIER_SHIFT},
    {")", ")", '0', MODIFIER_SHIFT},
    {"_", "_", 0xBD, MODIFIER_SHIFT},
    {"+", "+", 0xBB, MODIFIER_SHIFT},
    {"{", "{", 0xDB, MODIFIER_SHIFT},
    {"}", "}", 0xDD, MODIFIER_SHIFT},
    {"|", "|", 0xDC, MODIFIER_SHIFT},
    {":", ":", 0xBA, MODIFIER_SHIFT},
    {"\"", "\"", 0xDE, MODIFIER_SHIFT},
    {"~", "~", 0xC0, MODIFIER_SHIFT},
    {"<", "<", 0xBC, MODIFIER_SHIFT},
    {">", ">", 0xBE, MODIFIER_SHIFT},
    {"?", "?", 0xBF, MODIFIER_SHIFT},
    {"-", "-", 0xBD, 0},
    {"=", "=", 0xBB, 0},
    {"[", "[", 0xDB, 0},
    {"]", "]", 0xDD, 0},
    {"\\", "\\", 0xDC, 0},
    {"Copy", "Copy", 'C', MODIFIER_CTRL},
    {"Paste", "Paste", 'V', MODIFIER_CTRL},
    {"Cut", "Cut", 'X', MODIFIER_CTRL},
    {"Undo", "Undo", 'Z', MODIFIER_CTRL},
    {"Redo", "Redo", 'Y', MODIFIER_CTRL},
    {"All", "All", 'A', MODIFIER_CTRL},
    {"Task", "Task", 0x09, MODIFIER_ALT},
    {"Close", "Close", 0x73, MODIFIER_ALT},
    {"C+A+Del", "C+A+Del", 0x12E, MODIFIER_CTRL | MODIFIER_ALT},
    {"Desk", "Desk", 'D', MODIFIER_META},
    {"Explorer", "Explorer", 'E', MODIFIER_META},
    {"Run", "Run", 'R', MODIFIER_META},
  }};

  void StreamMenu::open(int device) {
    disconnectAction = StreamDisconnectAction::cancel;
    owner = device;
    page = StreamMenuPage::radial;
    selected = StreamAction::none;
    modifiers = 0;
    logOffset = 0;
    keyIndex = 10;
    keyboardLayout = 0;
    shiftOnce = false;
    caps = false;
    keyboardColumn_ = 4;
    keyboardRow_ = 1;
    previousSticks_ = {};
  }

  void StreamMenu::close() {
    page = StreamMenuPage::closed;
    selected = StreamAction::none;
    modifiers = 0;
    shiftOnce = false;
    caps = false;
  }

  bool StreamMenu::select_stick(bool right, short x, short y) {
    if (page != StreamMenuPage::radial || std::max(std::abs(static_cast<int>(x)), std::abs(static_cast<int>(y))) < WHEEL_DEADZONE) {
      return false;
    }
    int sector;
    if (std::abs(static_cast<int>(y)) >= std::abs(static_cast<int>(x))) {
      sector = y > 0 ? 0 : 2;
    } else {
      sector = x > 0 ? 1 : 3;
    }
    const auto action = static_cast<StreamAction>((right ? 4 : 0) + sector);
    const bool changed = action != selected;
    selected = action;
    return changed;
  }

  bool StreamMenu::sample_sticks(short leftX, short leftY, short rightX, short rightY) {
    const std::array<short, 4> current {leftX, leftY, rightX, rightY};
    const bool leftChanged = current[0] != previousSticks_[0] || current[1] != previousSticks_[1];
    const bool rightChanged = current[2] != previousSticks_[2] || current[3] != previousSticks_[3];
    previousSticks_ = current;
    bool changed = false;
    if (leftChanged || selected == StreamAction::none) {
      changed = select_stick(false, leftX, leftY);
    }
    if (rightChanged || selected == StreamAction::none) {
      changed = select_stick(true, rightX, rightY) || changed;
    }
    return changed;
  }

  void StreamMenu::navigate(int x, int y) {
    if (page == StreamMenuPage::keyboard) {
      const auto &cells = keyboard_cells();
      keyIndex = std::clamp(keyIndex, 0, static_cast<int>(cells.size()) - 1);
      const auto &current = cells[keyIndex];
      keyboardColumn_ = std::clamp(keyboardColumn_, current.column, current.column + current.width - 1);
      keyboardRow_ = std::clamp(keyboardRow_, current.row, current.row + current.height - 1);
      if (x != 0) {
        keyboardColumn_ = (x > 0 ? current.column + current.width : current.column - 1 + KEYBOARD_COLUMNS) % KEYBOARD_COLUMNS;
      }
      if (y != 0) {
        keyboardRow_ = (y > 0 ? current.row + current.height : current.row - 1 + KEYBOARD_ROWS) % KEYBOARD_ROWS;
      }
      const auto next = std::find_if(cells.begin(), cells.end(), [this](const StreamKeyboardCell &cell) {
        return keyboardColumn_ >= cell.column && keyboardColumn_ < cell.column + cell.width && keyboardRow_ >= cell.row && keyboardRow_ < cell.row + cell.height;
      });
      if (next != cells.end()) {
        keyIndex = static_cast<int>(next - cells.begin());
      }

    } else if (page == StreamMenuPage::logs) {
      logOffset = std::clamp(logOffset - y * 5, 0, 255);
    } else if (page == StreamMenuPage::radial && (x != 0 || y != 0)) {
      select_stick(false, static_cast<short>(x * 20000), static_cast<short>(-y * 20000));
    }
  }

  int StreamMenu::keyboard_key() {
    const int key = keyboard_cells().at(keyIndex).key;
    if (key == KEYBOARD_PAGE_KEY) {
      toggle_keyboard_page();
      return 0;
    }
    if (key == KEYBOARD_CAPS_KEY) {
      caps = !caps;
      return 0;
    }
    const int code = KEYS[key].code;
    if (code < 0) {
      if (code == -MODIFIER_SHIFT) {
        toggle_shift();
      } else {
        modifiers ^= -code;
      }
      return 0;
    }
    return code;
  }

  std::string StreamMenu::key_label(int index) const {
    const int catalogIndex = keyboard_cells().at(index).key;
    if (catalogIndex == KEYBOARD_PAGE_KEY) {
      if (keyboardLayout == 0) {
        return "?123";
      }
      return keyboardLayout == 1 ? "PC keys" : "ABC";
    }
    if (catalogIndex == KEYBOARD_CAPS_KEY) {
      return "Caps";
    }
    const auto &key = KEYS.at(catalogIndex);
    if (key.code == -MODIFIER_SHIFT) {
      return shiftOnce ? "[Shift]" : "Shift";
    }
    if (key.code < 0 && (modifiers & -key.code) != 0) {
      return std::string("[") + key.label + "]";
    }
    return (binding_modifiers(catalogIndex) & MODIFIER_SHIFT) != 0 ? key.shifted : key.label;
  }

  int StreamMenu::key_modifiers(int code) const {
    if (const int key = keyboard_cells().at(keyIndex).key; key < static_cast<int>(KEYS.size()) && KEYS[key].code == code) {
      return binding_modifiers(key);
    }
    return modifiers | ((shiftOnce != (caps && code >= 'A' && code <= 'Z')) ? MODIFIER_SHIFT : 0);
  }

  int StreamMenu::binding_modifiers(int index) const {
    const auto &key = KEYS.at(index);
    if (index >= 106) {
      return key.modifiers;
    }
    // This page already exposes both digits and their symbols as separate keys.
    // Its explicit bindings must agree with the labels even with one-shot Shift.
    const bool upper = keyboardLayout != 1 && (shiftOnce != (caps && key.code >= 'A' && key.code <= 'Z'));
    return modifiers | key.modifiers | (upper ? MODIFIER_SHIFT : 0);
  }

  bool StreamMenu::confirm_disconnect(StreamDisconnectAction action) {
    if (page != StreamMenuPage::quit_confirmation) {
      return false;
    }
    disconnectAction = action;
    if (action == StreamDisconnectAction::cancel) {
      page = StreamMenuPage::radial;
    }
    return action != StreamDisconnectAction::cancel;
  }

  void StreamMenu::toggle_shift() {
    shiftOnce = !shiftOnce;
  }

  void StreamMenu::key_sent() {
    shiftOnce = false;
  }

  const std::array<StreamKeyboardCell, 47> &StreamMenu::keyboard_cells() const {
    static const auto letters = make_keyboard_cells({{
      0,
      1,
      2,
      3,
      4,
      5,
      6,
      7,
      8,
      9,
      10,
      11,
      12,
      13,
      14,
      15,
      16,
      17,
      18,
      19,
      20,
      21,
      22,
      23,
      24,
      25,
      26,
      27,
      28,
      55,
      30,
      31,
      32,
      33,
      34,
      35,
      36,
      37,
      38,
      39,
    }});
    static const auto symbols = make_keyboard_cells({{
      0,
      1,
      2,
      3,
      4,
      5,
      6,
      7,
      8,
      9,
      80,
      81,
      82,
      83,
      84,
      85,
      86,
      87,
      88,
      89,
      101,
      102,
      103,
      104,
      105,
      55,
      96,
      56,
      97,
      95,
      90,
      91,
      92,
      93,
      94,
      98,
      99,
      100,
      37,
      38,
    }});
    static const auto desktop = make_keyboard_cells({{
      60,
      61,
      62,
      63,
      64,
      65,
      70,
      71,
      72,
      73,
      74,
      75,
      49,
      45,
      77,
      48,
      66,
      67,
      76,
      69,
      106,
      107,
      108,
      109,
      110,
      111,
      112,
      113,
      114,
      115,
      41,
      42,
      43,
      40,
      78,
      79,
      68,
      58,
      116,
      117,
    }});
    if (keyboardLayout == 0) {
      return letters;
    }
    return keyboardLayout == 1 ? symbols : desktop;
  }

  void StreamMenu::toggle_keyboard_page() {
    keyboardLayout = (keyboardLayout + 1) % 3;
  }

  const char *stream_action_label(StreamAction action, bool mouseEnabled, bool statisticsEnabled) {
    switch (action) {
      case StreamAction::keyboard:
        return "Keyboard";
      case StreamAction::mouse:
        return mouseEnabled ? "Mouse: On" : "Mouse: Off";
      case StreamAction::logs:
        return "View logs";
      case StreamAction::quit:
        return "Exit";
      case StreamAction::guide:
        return "Guide";
      case StreamAction::share:
        return "Share";
      case StreamAction::screenshot:
        return "Screenshot";
      case StreamAction::statistics:
        return statisticsEnabled ? "Stats: On" : "Stats: Off";
      case StreamAction::none:
        return "Select with either stick";
    }
    return "";
  }
}  // namespace input
