/**
 * @file src/ui/host_sun_animation.h
 * @brief Samples the corona animation and blends artwork when a host's status changes.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// local includes
#include "src/app/host_records.h"
#include "src/ui/shell_layout.h"

namespace ui {
  inline constexpr int HOST_SUN_TILE_SIZE = 128;  ///< Width and height of one animation frame in the atlas.
  inline constexpr int HOST_SUN_ATLAS_COLUMNS = 4;  ///< Number of frames per row in the atlas.
  inline constexpr int HOST_SUN_FRAME_COUNT = 16;  ///< Number of distinct frames in the corona loop.
  inline constexpr std::uint64_t HOST_SUN_FRAME_MS = 100;  ///< Time each corona atlas frame is displayed.
  inline constexpr std::uint64_t HOST_SUN_FADE_MS = 440;  ///< Duration of the artwork dissolve.
  inline constexpr std::uint64_t HOST_SUN_TRANSITION_MS = 540;  ///< Duration of the centered scale transition.

  /**
   * @brief Artwork for the host grid, ordered from warm sun to ash-gray sun for compositing.
   */
  enum class HostSunState {
    online,  ///< Warm corona for a reachable, paired host.
    needs_pairing,  ///< Blue corona for a reachable, unpaired host.
    offline,  ///< Still ash-gray sun for an offline or unchecked host.
  };

  inline constexpr std::array HOST_SUN_STATES {HostSunState::online, HostSunState::needs_pairing, HostSunState::offline};  ///< Layer compositing order.

  /**
   * @brief Select the solar artwork for a host's reported connection state.
   * @param reachability Most recent reachability result.
   * @param pairing Current pairing state for this client.
   * @return Artwork to show, including a still sun while waiting for a probe.
   */
  constexpr HostSunState host_sun_state(app::HostReachability reachability, app::PairingState pairing) {
    if (reachability != app::HostReachability::online) {
      return HostSunState::offline;
    }
    return pairing == app::PairingState::paired ? HostSunState::online : HostSunState::needs_pairing;
  }

  /**
   * @brief Select the raster artwork for a host state.
   * @param state Artwork requested for the tile.
   * @return Bundled PNG asset path.
   */
  constexpr const char *host_sun_asset(HostSunState state) {
    constexpr std::array paths {"hosts\\sun-corona.png", "hosts\\sun-corona-blue.png", "hosts\\sun-ash.png"};
    return paths[static_cast<std::size_t>(state)];
  }

  /**
   * @brief Sample the current corona frame at the host grid's animation rate.
   * @param nowMilliseconds Monotonic UI clock in milliseconds.
   * @return Zero-based atlas frame, wrapping after a complete loop.
   */
  constexpr int host_sun_frame(std::uint64_t nowMilliseconds) {
    return static_cast<int>((nowMilliseconds / HOST_SUN_FRAME_MS) % HOST_SUN_FRAME_COUNT);
  }

  /**
   * @brief Locate a sampled frame in the row-major atlas.
   * @param frame Frame index returned by host_sun_frame().
   * @return Source rectangle in atlas pixels.
   */
  constexpr ShellRect host_sun_frame_bounds(int frame) {
    return {
      (frame % HOST_SUN_ATLAS_COLUMNS) * HOST_SUN_TILE_SIZE,
      (frame / HOST_SUN_ATLAS_COLUMNS) * HOST_SUN_TILE_SIZE,
      HOST_SUN_TILE_SIZE,
      HOST_SUN_TILE_SIZE,
    };
  }

  /**
   * @brief One centered solar layer sampled during the status dissolve.
   */
  struct HostSunLayer {
    float opacity = 0.0f;  ///< Texture opacity between zero and one.
    float scale = 0.88f;  ///< Icon scale relative to its settled size.
  };

  /**
   * @brief Track a visible host's current artwork and allow the dissolve to be redirected mid-flight.
   */
  class HostSunTransition {
  public:
    /**
     * @brief Initialize a newly visible tile directly in its current state.
     * @param state Initial host artwork.
     */
    explicit HostSunTransition(HostSunState state):
        target_ {state} {}

    /**
     * @brief Retarget the dissolve while preserving its current appearance.
     * @param state Newly reported artwork.
     * @param nowMilliseconds Monotonic UI clock in milliseconds.
     */
    void retarget(HostSunState state, std::uint64_t nowMilliseconds) {
      if (target_ == state) {
        return;
      }
      start_ = layers(nowMilliseconds);
      target_ = state;
      startTime_ = nowMilliseconds;
      started_ = true;
    }

    /**
     * @brief Check whether changes in the artwork still require redrawing.
     * @param nowMilliseconds Monotonic UI clock in milliseconds.
     * @return True until the scale and opacity have settled.
     */
    bool active(std::uint64_t nowMilliseconds) const {
      return started_ && nowMilliseconds - startTime_ < HOST_SUN_TRANSITION_MS;
    }

    /**
     * @brief Sample the opacity and centered scale of each artwork layer.
     * @param nowMilliseconds Monotonic UI clock in milliseconds.
     * @return Layer values in compositing order.
     */
    std::array<HostSunLayer, HOST_SUN_STATES.size()> layers(std::uint64_t nowMilliseconds) const {
      const auto target = static_cast<std::size_t>(target_);
      std::array<HostSunLayer, HOST_SUN_STATES.size()> result {};
      result[target] = {1.0f, 1.0f};
      if (!active(nowMilliseconds)) {
        return result;
      }

      const auto elapsed = static_cast<float>(nowMilliseconds - startTime_);
      const float fade = std::min(elapsed / HOST_SUN_FADE_MS, 1.0f);
      const float fadeAmount = fade * fade * (3.0f - 2.0f * fade);
      const float scaleRemaining = 1.0f - elapsed / HOST_SUN_TRANSITION_MS;
      const float scaleAmount = 1.0f - scaleRemaining * scaleRemaining * scaleRemaining;
      for (std::size_t i = 0; i < result.size(); ++i) {
        result[i].opacity = start_[i].opacity + (result[i].opacity - start_[i].opacity) * fadeAmount;
        result[i].scale = start_[i].scale + (result[i].scale - start_[i].scale) * scaleAmount;
      }
      return result;
    }

  private:
    std::array<HostSunLayer, HOST_SUN_STATES.size()> start_ {};  ///< Visible layers at the last status change.
    HostSunState target_;  ///< Artwork requested by the current reachability and pairing state.
    std::uint64_t startTime_ = 0;  ///< Clock sample when the last status change started.
    bool started_ = false;  ///< True after the tile has experienced its first status change.
  };
}  // namespace ui
