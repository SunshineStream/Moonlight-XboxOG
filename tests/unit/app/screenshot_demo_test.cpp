/**
 * @file tests/unit/app/screenshot_demo_test.cpp
 * @brief Verifies the sample hosts and app library used for documentation.
 */
// test header include
#include "src/app/client_state.h"

// lib includes
#include <lizardbyte/common/testing.h>

// local includes
#include "src/ui/host_sun_animation.h"

namespace {
  /**
   * @brief Keep an example of every host artwork state and an app library that can open immediately.
   */
  TEST(ScreenshotDemoTest, ProvidesAllHostStatesAndACachedLibrary) {
    const auto hosts = app::screenshot_demo_hosts();
    ASSERT_EQ(hosts.size(), 3U);
    EXPECT_EQ(hosts[0].address, "192.0.2.10");
    EXPECT_EQ(hosts[1].address, "192.0.2.20");
    EXPECT_EQ(hosts[2].address, "192.0.2.30");
    EXPECT_EQ(ui::host_sun_state(hosts[0].reachability, hosts[0].pairingState), ui::HostSunState::online);
    EXPECT_EQ(ui::host_sun_state(hosts[1].reachability, hosts[1].pairingState), ui::HostSunState::needs_pairing);
    EXPECT_EQ(ui::host_sun_state(hosts[2].reachability, hosts[2].pairingState), ui::HostSunState::offline);
    EXPECT_EQ(hosts[0].appListState, app::HostAppListState::ready);
    ASSERT_EQ(hosts[0].apps.size(), 4U);
    EXPECT_TRUE(hosts[0].apps[1].favorite);
    EXPECT_TRUE(hosts[1].apps.empty());
    EXPECT_TRUE(hosts[2].apps.empty());
  }
}  // namespace
