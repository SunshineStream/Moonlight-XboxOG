/**
 * @file tests/unit/input/stream_input_test.cpp
 * @brief Verifies independent gamepads, capture lifetimes and merged pointer/keyboard input.
 */
// test header include
#include "src/input/stream_input.h"

// standard includes
#include <array>
#include <vector>

// lib includes
#include "third-party/moonlight-common-c/src/Limelight.h"

#include <lizardbyte/common/testing.h>

/**
 * @brief Captures ordered transport packets without a host connection.
 */
class StreamInputTest: public testing::Test {
public:
  std::vector<input::StreamPacket> packets;  ///< Packets emitted by the router.
  input::StreamInput router {[this](const input::StreamPacket &packet) {
    packets.push_back(packet);
  }};  ///< Router under test.
};

/**
 * @brief Four controllers arrive independently, and a removed slot is reused without shifting players.
 */
TEST_F(StreamInputTest, MaintainsFourStableSlotsAndRemovalMask) {
  EXPECT_EQ(router.connect(-1), -1);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(router.connect(10 + i), i);
    ASSERT_EQ(packets.size(), static_cast<std::size_t>((i + 1) * 2));
    EXPECT_EQ(packets[packets.size() - 2].type, input::StreamPacketType::arrival);
    EXPECT_EQ(packets.back().mask, (1 << (i + 1)) - 1);
  }
  EXPECT_EQ(router.connect(12), 2);
  EXPECT_EQ(router.connect(99), -1);
  EXPECT_EQ(router.active_mask(), 15);
  router.disconnect(11);
  EXPECT_EQ(packets.back().slot, 1);
  EXPECT_EQ(packets.back().mask, 13);
  EXPECT_EQ(packets.back().controller.buttonFlags, 0);
  EXPECT_EQ(router.connect(99), 1);
  EXPECT_EQ(router.connect(12), 2);
  EXPECT_EQ(router.connect(13), 3);
  const auto count = packets.size();
  router.disconnect(400);
  EXPECT_EQ(packets.size(), count);
}

/**
 * @brief SDL3 IDs use the full unsigned 32-bit range even though the router reserves a negative sentinel.
 */
TEST_F(StreamInputTest, RoutesTheLargestSdlGamepadId) {
  constexpr std::uint32_t instance = 0xffffffffU;
  ASSERT_EQ(router.connect(instance), 0);
  router.controller(instance, {A_FLAG});
  EXPECT_EQ(packets.back().controller.buttonFlags, A_FLAG);
  router.disconnect(instance);
  EXPECT_EQ(router.active_mask(), 0);
  EXPECT_EQ(packets.back().controller.buttonFlags, 0);
}

/**
 * @brief Full analog state and quick digital press/release transitions reach each pad's own slot.
 */
TEST_F(StreamInputTest, PreservesAxesTriggersAndQuickTransitions) {
  router.connect(30);
  router.connect(31);
  input::ControllerState sample {A_FLAG, 128, 255, -32768, 32767, 12345, -12345};
  packets.clear();
  router.controller(31, sample);
  router.controller(31, sample);
  router.controller(31, {});
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(packets[0].slot, 1);
  EXPECT_EQ(packets[0].mask, 3);
  EXPECT_EQ(packets[0].controller.leftTrigger, 128);
  EXPECT_EQ(packets[0].controller.rightTrigger, 255);
  EXPECT_EQ(packets[0].controller.leftStickX, -32768);
  EXPECT_EQ(packets[0].controller.leftStickY, 32767);
  EXPECT_EQ(packets[0].controller.rightStickX, 12345);
  EXPECT_EQ(packets[0].controller.rightStickY, -12345);
  EXPECT_EQ(packets[1].controller.buttonFlags, 0);
  router.controller(400, sample);
  EXPECT_EQ(packets.size(), 2);
}

/**
 * @brief Opening local controls releases all host devices and waits for pad neutral before resuming.
 */
TEST_F(StreamInputTest, CaptureReleasesHostAndPreventsInputLeakage) {
  router.connect(1);
  router.connect(2);
  router.controller(1, {A_FLAG});
  router.controller(2, {B_FLAG});
  router.key(0, 'W', true);
  router.mouse_button(1, BUTTON_LEFT, true);
  packets.clear();
  router.set_captured(true);
  ASSERT_EQ(packets.size(), 4);
  EXPECT_EQ(packets[0].controller.buttonFlags, 0);
  EXPECT_EQ(packets[1].controller.buttonFlags, 0);
  EXPECT_FALSE(packets[2].pressed);
  EXPECT_FALSE(packets[3].pressed);
  router.set_captured(true);
  router.controller(1, {X_FLAG});
  router.key(0, 'A', true);
  router.mouse_button(1, BUTTON_RIGHT, true);
  router.mouse_move(10, 20);
  router.scroll(1, 1);
  EXPECT_EQ(packets.size(), 4);
  router.set_captured(false);
  router.controller(1, {X_FLAG});
  EXPECT_EQ(packets.size(), 4);
  router.controller(1, {});
  router.controller(1, {X_FLAG});
  ASSERT_EQ(packets.size(), 5);
  EXPECT_EQ(packets.back().controller.buttonFlags, X_FLAG);
  // Connecting while a menu is open must also wait for neutral.
  router.set_captured(true);
  router.connect(3);
  router.set_captured(false);
  packets.clear();
  router.controller(3, {Y_FLAG});
  EXPECT_TRUE(packets.empty());
  router.controller(3, {});
  router.controller(3, {Y_FLAG});
  EXPECT_EQ(packets.size(), 1);
}

/**
 * @brief Physical and virtual sources cannot release each other's held key or mouse button.
 */
TEST_F(StreamInputTest, MergesDigitalSourcesAndReleasesDisconnects) {
  router.key(10, 'A', true, MODIFIER_SHIFT);
  router.key(10, 'A', true);
  router.key(20, 'A', true);
  router.key(10, 'B', true);
  router.mouse_button(10, BUTTON_LEFT, true);
  router.mouse_button(20, BUTTON_LEFT, true);
  EXPECT_EQ(packets.size(), 3);
  EXPECT_EQ(packets[0].modifiers, MODIFIER_SHIFT);
  router.release_source(10);
  ASSERT_EQ(packets.size(), 4);
  EXPECT_EQ(packets.back().code, 'B');
  EXPECT_FALSE(packets.back().pressed);
  router.release_source(20);
  ASSERT_EQ(packets.size(), 6);
  EXPECT_FALSE(packets[4].pressed);
  EXPECT_FALSE(packets[5].pressed);
  router.key(50, 0, true);
  router.key(50, 'Z', false);
  router.release_source(100);
  router.release_all();
  EXPECT_EQ(packets.size(), 6);
}

/**
 * @brief Controller pointer movement is time-based, dead-zoned and isolated from gameplay.
 */
TEST_F(StreamInputTest, MouseModeIntegratesMotionAndReleasesOnRemoval) {
  router.connect(10);
  router.connect(11);
  router.set_mouse_device(10);
  router.controller(10, {});
  packets.clear();
  router.controller(10, {A_FLAG | B_FLAG, 0, 0, 32767, 32767});
  router.controller(11, {X_FLAG});
  router.tick(10);
  ASSERT_EQ(packets.size(), 4);
  EXPECT_EQ(packets[0].code, BUTTON_LEFT);
  EXPECT_EQ(packets[1].code, BUTTON_RIGHT);
  EXPECT_EQ(packets[2].slot, 1);
  EXPECT_EQ(packets[3].x, 9);
  EXPECT_EQ(packets[3].y, -9);
  packets.clear();
  router.tick(5000);
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets[0].x, 45);
  router.disconnect(10);
  ASSERT_EQ(packets.size(), 4);
  EXPECT_FALSE(packets[1].pressed);
  EXPECT_FALSE(packets[2].pressed);
  EXPECT_EQ(packets.back().mask, 2);
  packets.clear();
  router.tick(10);
  EXPECT_TRUE(packets.empty());
}

/**
 * @brief Quick virtual pointer clicks survive a complete press/release within one event pump.
 */
TEST_F(StreamInputTest, PreservesQuickVirtualMouseClicks) {
  router.connect(5);
  router.set_mouse_device(5);
  router.controller(5, {});
  packets.clear();
  router.controller(5, {A_FLAG});
  router.controller(5, {});
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(packets[0].type, input::StreamPacketType::mouse_button);
  EXPECT_TRUE(packets[0].pressed);
  EXPECT_FALSE(packets[1].pressed);
}

/**
 * @brief Fractional pointer motion accumulates and capture/mode changes clear held clicks.
 */
TEST_F(StreamInputTest, MouseDeadzoneFractionAndCapture) {
  router.connect(1);
  router.set_mouse_device(1);
  router.controller(1, {});
  packets.clear();
  router.controller(1, {0, 0, 0, 6999, -6999});
  router.tick(10);
  EXPECT_TRUE(packets.empty());
  router.controller(1, {0, 0, 0, 7100, 0});
  for (int i = 0; i < 40; ++i) {
    router.tick(10);
  }
  EXPECT_FALSE(packets.empty());
  router.controller(1, {A_FLAG});
  router.tick(10);
  EXPECT_TRUE(packets.back().pressed);
  router.set_captured(true);
  EXPECT_FALSE(packets.back().pressed);
  const auto count = packets.size();
  router.tick(10);
  EXPECT_EQ(packets.size(), count);
  router.set_captured(false);
  router.controller(1, {});
  router.controller(1, {B_FLAG});
  router.tick(10);
  router.set_mouse_device(-1);
  EXPECT_EQ(packets.back().code, BUTTON_RIGHT);
  EXPECT_FALSE(packets.back().pressed);
}

/**
 * @brief Physical relative movement and both wheel axes use bounded transport deltas.
 */
TEST_F(StreamInputTest, BoundsPhysicalPointerAndWheelEvents) {
  router.mouse_move(0, 0);
  router.scroll(0, 0);
  EXPECT_TRUE(packets.empty());
  router.mouse_move(90000, -90000);
  router.scroll(-500, 500);
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(packets[0].x, 32767);
  EXPECT_EQ(packets[0].y, -32768);
  EXPECT_EQ(packets[1].x, -127);
  EXPECT_EQ(packets[1].y, 127);
}

/**
 * @brief Saturating inversion preserves all extrema and USB usages map to distinct host keys.
 */
TEST(StreamKeyMappingTest, PreservesExtendedModifiersNavigationAndKeypad) {
  EXPECT_EQ(input::invert_stick_axis(-32768), 32767);
  EXPECT_EQ(input::invert_stick_axis(32767), -32767);
  EXPECT_EQ(input::invert_stick_axis(0), 0);
  EXPECT_EQ(input::keyboard_virtual_key(4), 'A');
  EXPECT_EQ(input::keyboard_virtual_key(29), 'Z');
  EXPECT_EQ(input::keyboard_virtual_key(30), '1');
  EXPECT_EQ(input::keyboard_virtual_key(39), '0');
  EXPECT_EQ(input::keyboard_virtual_key(40), 0x0D);
  EXPECT_EQ(input::keyboard_virtual_key(88), 0x10D);
  EXPECT_EQ(input::keyboard_virtual_key(224), 0xA2);
  EXPECT_EQ(input::keyboard_virtual_key(228), 0x1A3);
  EXPECT_EQ(input::keyboard_virtual_key(82), 0x126);
  EXPECT_EQ(input::keyboard_virtual_key(98), 0x60);
  EXPECT_EQ(input::keyboard_virtual_key(58), 0x70);
  EXPECT_EQ(input::keyboard_virtual_key(115), 0x87);
  EXPECT_EQ(input::keyboard_virtual_key(0), 0);
  EXPECT_EQ(input::keyboard_virtual_key(300), 0);
  // Ordinary keyboard, navigation, keypad and modifier blocks must all be usable.
  for (int usage = 4; usage <= 101; ++usage) {
    EXPECT_NE(input::keyboard_virtual_key(usage), 0) << usage;
  }
  for (int usage = 104; usage <= 115; ++usage) {
    EXPECT_NE(input::keyboard_virtual_key(usage), 0) << usage;
  }
  for (int usage = 224; usage <= 231; ++usage) {
    EXPECT_NE(input::keyboard_virtual_key(usage), 0) << usage;
  }
}

/**
 * @brief Stick mouse reports are capped at 125 Hz while press/release transitions remain immediate.
 */
TEST_F(StreamInputTest, BoundsMouseReportRateWithoutDelayingClicks) {
  router.connect(0);
  router.set_mouse_device(0);
  router.controller(0, {});
  packets.clear();
  router.controller(0, {0, 0, 0, 32767, 0});
  router.tick(1);
  EXPECT_TRUE(packets.empty());
  router.controller(0, {A_FLAG, 0, 0, 32767, 0});
  ASSERT_EQ(packets.size(), 1);
  EXPECT_TRUE(packets.back().pressed);
  router.controller(0, {0, 0, 0, 32767, 0});
  ASSERT_EQ(packets.size(), 2);
  EXPECT_FALSE(packets.back().pressed);
  router.tick(7);
  ASSERT_EQ(packets.size(), 3);
  EXPECT_EQ(packets.back().type, input::StreamPacketType::mouse_move);
  EXPECT_EQ(packets.back().x, 7);
  router.tick(0);
  EXPECT_EQ(packets.size(), 3);
}

/**
 * @brief A minute of high-frequency polling preserves mouse speed without flooding the transport.
 */
TEST(StreamInputRateTest, SustainsMouseMovementWithBoundedTraffic) {
  int reports = 0;
  int distance = 0;
  input::StreamInput router([&reports, &distance](const input::StreamPacket &packet) {
    if (packet.type == input::StreamPacketType::mouse_move) {
      ++reports;
      distance += packet.x;
      EXPECT_GE(packet.x, 0);
      EXPECT_LE(packet.x, 8);
    }
  });
  router.connect(0);
  router.set_mouse_device(0);
  router.controller(0, {});
  router.controller(0, {0, 0, 0, 32767, 0});
  for (int i = 0; i < 60000; ++i) {
    router.controller(0, {0, 0, 0, 32767, 0});
    router.tick(1);
  }
  EXPECT_EQ(reports, 7500);
  EXPECT_NEAR(distance, 54000, 1);
}
