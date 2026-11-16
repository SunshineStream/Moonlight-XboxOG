/**
 * @file tests/unit/input/usb_hid_test.cpp
 * @brief Verifies boot HID rollover, signed motion, truncation and independent device lifetimes.
 */
// test includes
#include <lizardbyte/common/testing.h>

// standard includes
#include <array>
#include <vector>

// local includes
#include "src/input/usb_hid.h"

/**
 * @brief Records decoded HID operations without USB hardware.
 */
class BootHidTest: public testing::Test {
protected:
  std::vector<input::BootHidEvent> events;  ///< Decoded transitions.
  input::BootHidInput hid {[this](const input::BootHidEvent &event) {
    events.push_back(event);
  }};  ///< Decoder under test.
};

/**
 * @brief Modifiers precede ordinary keys and duplicate report slots create only one press.
 */
TEST_F(BootHidTest, DecodesKeyboardAndDisconnectReleases) {
  hid.connect(1, 1);
  const std::array<std::uint8_t, 8> report {0x22, 0, 4, 4, 5, 0, 0, 0};
  hid.report(1, report.data(), report.size());
  ASSERT_EQ(events.size(), 4);
  EXPECT_EQ(events[0].code, 225);
  EXPECT_EQ(events[1].code, 229);
  EXPECT_EQ(events[2].code, 4);
  EXPECT_EQ(events[3].code, 5);
  hid.report(1, report.data(), report.size());
  EXPECT_EQ(events.size(), 4);
  hid.disconnect(1);
  ASSERT_EQ(events.size(), 8);
  for (int i = 4; i < 8; ++i) {
    EXPECT_FALSE(events[i].pressed);
  }
  hid.disconnect(1);
  EXPECT_EQ(events.size(), 8);
}

/**
 * @brief Keyboard rollover retains held regular keys until a valid report resolves the state.
 */
TEST_F(BootHidTest, HandlesRolloverAndTruncatedReports) {
  hid.connect(1, 1);
  std::array<std::uint8_t, 8> report {0, 0, 4, 0, 0, 0, 0, 0};
  hid.report(1, report.data(), 8);
  for (int error = 1; error <= 3; ++error) {
    report[2] = error;
    hid.report(1, report.data(), 8);
  }
  EXPECT_EQ(events.size(), 1);
  hid.report(1, nullptr, 8);
  hid.report(1, report.data(), 7);
  hid.report(99, report.data(), 8);
  EXPECT_EQ(events.size(), 1);
  report = {};
  hid.report(1, report.data(), 8);
  ASSERT_EQ(events.size(), 2);
  EXPECT_FALSE(events.back().pressed);
}

/**
 * @brief Shared keys on two keyboards remain held until both keyboards release or disconnect.
 */
TEST_F(BootHidTest, MergesKeyboardSourcesAndReusedDeviceIds) {
  hid.connect(1, 1);
  hid.connect(2, 1);
  const std::array<std::uint8_t, 8> report {0, 0, 4, 0, 0, 0, 0, 0};
  hid.report(1, report.data(), 8);
  hid.report(2, report.data(), 8);
  hid.disconnect(1);
  EXPECT_EQ(events.size(), 1);
  hid.connect(2, 2);
  ASSERT_EQ(events.size(), 2);
  EXPECT_FALSE(events.back().pressed);
  hid.connect(3, 3);
  hid.report(3, report.data(), 8);
  EXPECT_EQ(events.size(), 2);
}

/**
 * @brief Boot mice decode signed extrema, merge buttons and ignore nonstandard extra bytes.
 */
TEST_F(BootHidTest, DecodesMouseWithoutGuessingReportLayouts) {
  hid.connect(1, 2);
  hid.connect(2, 2);
  const std::array<std::uint8_t, 4> report {7, 128, 127, 99};
  hid.report(1, report.data(), 4);
  ASSERT_EQ(events.size(), 4);
  EXPECT_EQ(events[0].code, 1);
  EXPECT_EQ(events[1].code, 2);
  EXPECT_EQ(events[2].code, 3);
  EXPECT_EQ(events[3].type, input::BootHidEventType::motion);
  EXPECT_EQ(events[3].x, -128);
  EXPECT_EQ(events[3].y, 127);
  const std::array<std::uint8_t, 3> held {7, 0, 0};
  hid.report(2, held.data(), 3);
  hid.report(1, held.data(), 2);
  hid.disconnect(1);
  EXPECT_EQ(events.size(), 4);
  hid.clear();
  ASSERT_EQ(events.size(), 7);
  EXPECT_FALSE(events.back().pressed);
  hid.clear();
  EXPECT_EQ(events.size(), 7);
}
