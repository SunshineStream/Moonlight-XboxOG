/**
 * @file tests/unit/ui/host_sun_animation_test.cpp
 * @brief Verifies artwork selection, corona timing and host status transitions.
 */
// test header include
#include "src/ui/host_sun_animation.h"

// standard includes
#include <limits>

// lib includes
#include <lizardbyte/common/testing.h>

namespace {
  /**
   * @brief Select the appropriate artwork and retain a static fallback for each host state.
   */
  TEST(HostSunAnimationTest, SelectsArtworkAndFallbacks) {
    using app::HostReachability;
    using app::PairingState;
    using ui::HostSunState;
    EXPECT_EQ(ui::host_sun_state(HostReachability::online, PairingState::paired), HostSunState::online);
    EXPECT_EQ(ui::host_sun_state(HostReachability::online, PairingState::not_paired), HostSunState::needs_pairing);
    for (const auto reachability : {HostReachability::offline, HostReachability::unknown}) {
      EXPECT_EQ(ui::host_sun_state(reachability, PairingState::paired), HostSunState::offline);
      EXPECT_EQ(ui::host_sun_state(reachability, PairingState::not_paired), HostSunState::offline);
    }
    EXPECT_STREQ(ui::host_sun_fallback(HostSunState::online), "hosts\\sun-paired.svg");
    EXPECT_STREQ(ui::host_sun_fallback(HostSunState::needs_pairing), "hosts\\sun-unpaired.svg");
    EXPECT_STREQ(ui::host_sun_fallback(HostSunState::offline), "hosts\\sun-offline.svg");
  }

  /**
   * @brief Verify that a frame stays stable until the next redraw interval and that the loop wraps.
   */
  TEST(HostSunAnimationTest, AdvancesOnFrameBoundariesAndWraps) {
    constexpr auto interval = ui::HOST_SUN_FRAME_MS;
    constexpr auto period = interval * ui::HOST_SUN_FRAME_COUNT;
    for (int i = 0; i < ui::HOST_SUN_FRAME_COUNT; ++i) {
      const auto start = static_cast<std::uint64_t>(i) * interval;
      EXPECT_EQ(ui::host_sun_frame(start), i);
      EXPECT_EQ(ui::host_sun_frame(start + interval - 1), i);
      EXPECT_EQ(ui::host_sun_frame(period + start), i);
    }
    EXPECT_EQ(ui::host_sun_frame(period), 0);
    EXPECT_LT(ui::host_sun_frame(std::numeric_limits<std::uint64_t>::max()), ui::HOST_SUN_FRAME_COUNT);
  }

  /**
   * @brief Verify that frames map to distinct complete tiles in reading order.
   */
  TEST(HostSunAnimationTest, SamplesTheAtlasInRowMajorOrder) {
    constexpr int size = ui::HOST_SUN_TILE_SIZE;
    constexpr int columns = ui::HOST_SUN_ATLAS_COLUMNS;
    for (int i = 0; i < ui::HOST_SUN_FRAME_COUNT; ++i) {
      const auto bounds = ui::host_sun_frame_bounds(i);
      EXPECT_EQ(bounds.w, size);
      EXPECT_EQ(bounds.h, size);
      EXPECT_EQ(bounds.x, (i % columns) * size);
      EXPECT_EQ(bounds.y, (i / columns) * size);
    }
  }

  /**
   * @brief Show newly visible artwork immediately and dissolve between every pair of states.
   */
  TEST(HostSunAnimationTest, BlendsBetweenAllHostStates) {
    constexpr std::uint64_t start = 100;
    for (std::size_t from = 0; from < ui::HOST_SUN_STATES.size(); ++from) {
      const ui::HostSunTransition initial(ui::HOST_SUN_STATES[from]);
      EXPECT_FALSE(initial.active(start));
      EXPECT_FLOAT_EQ(initial.layers(start)[from].opacity, 1.0f);
      EXPECT_FLOAT_EQ(initial.layers(start)[from].scale, 1.0f);
      for (std::size_t to = 0; to < ui::HOST_SUN_STATES.size(); ++to) {
        if (from == to) {
          continue;
        }
        auto transition = initial;
        transition.retarget(ui::HOST_SUN_STATES[to], start);
        EXPECT_TRUE(transition.active(start));
        EXPECT_FLOAT_EQ(transition.layers(start)[from].opacity, 1.0f);
        EXPECT_FLOAT_EQ(transition.layers(start)[to].opacity, 0.0f);

        const auto middle = transition.layers(start + ui::HOST_SUN_FADE_MS / 2);
        EXPECT_FLOAT_EQ(middle[from].opacity, 0.5f);
        EXPECT_FLOAT_EQ(middle[to].opacity, 0.5f);
        EXPECT_LT(middle[from].scale, 1.0f);
        EXPECT_GT(middle[to].scale, 0.88f);

        const auto faded = transition.layers(start + ui::HOST_SUN_FADE_MS);
        EXPECT_FLOAT_EQ(faded[from].opacity, 0.0f);
        EXPECT_FLOAT_EQ(faded[to].opacity, 1.0f);
        EXPECT_TRUE(transition.active(start + ui::HOST_SUN_FADE_MS));
        EXPECT_FALSE(transition.active(start + ui::HOST_SUN_TRANSITION_MS));
        const auto settled = transition.layers(start + ui::HOST_SUN_TRANSITION_MS);
        EXPECT_FLOAT_EQ(settled[from].opacity, 0.0f);
        EXPECT_FLOAT_EQ(settled[to].opacity, 1.0f);
        EXPECT_FLOAT_EQ(settled[to].scale, 1.0f);
      }
    }
  }

  /**
   * @brief Start a rapid second transition from the current blend and keep identical updates from extending it.
   */
  TEST(HostSunAnimationTest, KeepsCurrentLayersWhenRetargeted) {
    ui::HostSunTransition transition(ui::HostSunState::online);
    transition.retarget(ui::HostSunState::needs_pairing, 100);
    const auto before = transition.layers(300);
    transition.retarget(ui::HostSunState::offline, 300);
    const auto after = transition.layers(300);
    for (std::size_t i = 0; i < before.size(); ++i) {
      EXPECT_FLOAT_EQ(after[i].opacity, before[i].opacity);
      EXPECT_FLOAT_EQ(after[i].scale, before[i].scale);
    }
    transition.retarget(ui::HostSunState::offline, 500);
    const auto end = 300 + ui::HOST_SUN_TRANSITION_MS;
    EXPECT_FALSE(transition.active(end));
    const auto settled = transition.layers(end);
    EXPECT_FLOAT_EQ(settled[0].opacity, 0.0f);
    EXPECT_FLOAT_EQ(settled[1].opacity, 0.0f);
    EXPECT_FLOAT_EQ(settled[2].opacity, 1.0f);
  }
}  // namespace
