/**
 * @file src/app/screenshot_demo.cpp
 * @brief Supplies fictional host and app data for documentation builds.
 */
// class header include
#include "src/app/client_state.h"

namespace app {
  std::vector<HostRecord> screenshot_demo_hosts() {
    HostRecord paired;
    paired.displayName = "Sunroom";
    paired.address = "192.0.2.10";
    paired.pairingState = PairingState::paired;
    paired.reachability = HostReachability::online;
    paired.appListState = HostAppListState::ready;
    paired.apps = {
      {"Desktop", 1, true},
      {"Aurora Drift", 2, false, false, true},
      {"Orbit Foundry", 3},
      {"Starfall Trails", 4},
    };

    HostRecord unpaired;
    unpaired.displayName = "Observatory";
    unpaired.address = "192.0.2.20";
    unpaired.reachability = HostReachability::online;

    HostRecord offline;
    offline.displayName = "Northstar";
    offline.address = "192.0.2.30";
    offline.pairingState = PairingState::paired;
    offline.reachability = HostReachability::offline;

    return {paired, unpaired, offline};
  }
}  // namespace app
