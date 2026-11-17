/**
 * @file tests/controller/shell_controller_test.cpp
 * @brief Verifies recovery from removed virtual gamepads and preservation of live menu-controller references.
 */
// test header include
#include "src/input/shell_controller.h"

// standard includes
#include <algorithm>
#include <array>
#include <vector>

// lib includes
#include <lizardbyte/common/testing.h>

/**
 * @brief Stable controller handle whose device can disappear from enumeration while its reference remains open.
 */
struct _SDL_GameController {
  int id = 0;  ///< Registry identity, independent of shifting SDL device indices.
};

namespace {
  /**
   * @brief Simulated SDL device and reference ownership counters.
   */
  struct Device {
    bool attached = true;  ///< Current connectivity.
    bool mapped = true;  ///< Whether the device is a gamepad.
    bool openable = true;  ///< Whether opening the device succeeds.
    bool removedWhileOpening = false;  ///< Simulate removal between enumeration and acquisition.
    int references = 0;  ///< Outstanding SDL controller references.
    int opens = 0;  ///< Successful open calls.
    int closes = 0;  ///< Close calls, including detached candidates.
  };

  /**
   * @brief Mutable fake SDL data owned by the controller-test registry.
   */
  struct ControllerRegistry {
    std::array<Device, 5> devices;  ///< Devices survive removal until all open references are closed.
    std::array<SDL_GameController, 5> handles;  ///< Stable objects returned by successful opens.
    std::vector<int> available;  ///< Current enumeration order; removal changes indices, not identities.
    int enumerationCalls = 0;  ///< Detect unnecessary rescans while the active pad remains attached.
  };

  /**
   * @brief Access the registry shared with the C callback adapter.
   * @return Stable test registry, reset before every scenario.
   */
  ControllerRegistry &registry() {
    static ControllerRegistry state;
    return state;
  }

  /**
   * @brief Own an isolated SDL registry for one controller recovery scenario.
   */
  class ShellControllerTest: public BaseTest {  // NOSONAR(cpp:S3656) gtest fixture lifetime
  protected:
    /**
     * @brief Reset the fake registry and all reference counters before each test.
     */
    void SetUp() override {
      BaseTest::SetUp();
      registry().devices = {};
      registry().handles = {};
      registry().available.clear();
      registry().enumerationCalls = 0;
    }

    /**
     * @brief Add a device without delivering a device-added notification to the recovery code.
     * @param id Stable registry identity.
     */
    void attach(int id) const {
      registry().handles[id].id = id;
      registry().available.push_back(id);
    }

    /**
     * @brief Remove a device from enumeration while retaining its outstanding SDL handle.
     * @param id Stable registry identity.
     */
    void detach(int id) const {
      registry().devices[id].attached = false;
      registry().available.erase(std::remove(registry().available.begin(), registry().available.end(), id), registry().available.end());
    }
  };
}  // namespace

extern "C" {
  int SDL_NumJoysticks() {
    ++registry().enumerationCalls;
    return static_cast<int>(registry().available.size());
  }

  SDL_bool SDL_IsGameController(int index) {
    return registry().devices[registry().available[index]].mapped ? SDL_TRUE : SDL_FALSE;
  }

  SDL_GameController *SDL_GameControllerOpen(int index) {
    const int id = registry().available[index];
    auto &device = registry().devices[id];
    if (!device.openable) {
      return nullptr;
    }
    ++device.references;
    ++device.opens;
    if (device.removedWhileOpening) {
      device.attached = false;
    }
    return &registry().handles[id];
  }

  void SDL_GameControllerClose(const SDL_GameController *controller) {
    auto &device = registry().devices[controller->id];
    EXPECT_GT(device.references, 0);
    --device.references;
    ++device.closes;
  }

  SDL_bool SDL_GameControllerGetAttached(const SDL_GameController *controller) {
    return registry().devices[controller->id].attached ? SDL_TRUE : SDL_FALSE;
  }
}

namespace {
  /**
   * @brief A newly appearing host virtual pad must not steal the working menu controller.
   */
  TEST_F(ShellControllerTest, PreservesTheWorkingControllerAndItsReference) {
    attach(0);
    auto *active = SDL_GameControllerOpen(0);
    attach(1);
    registry().available = {1, 0};
    EXPECT_FALSE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[0]);
    EXPECT_EQ(registry().devices[0].references, 1);
    EXPECT_EQ(registry().devices[0].closes, 0);
    EXPECT_EQ(registry().devices[1].opens, 0);
    EXPECT_EQ(registry().enumerationCalls, 0);
    SDL_GameControllerClose(active);
  }

  /**
   * @brief Quit-related removal falls back to a pad that was already attached, without any new ADDED event.
   */
  TEST_F(ShellControllerTest, ReplacesARemovedPrimaryWithAnAlreadyConnectedPad) {
    attach(0);
    attach(1);
    auto *active = SDL_GameControllerOpen(0);
    detach(0);
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[1]);
    EXPECT_EQ(registry().devices[0].closes, 1);
    EXPECT_EQ(registry().devices[0].references, 0);
    EXPECT_EQ(registry().devices[1].references, 1);
    EXPECT_FALSE(input::recover_shell_controller(&active));
    EXPECT_EQ(registry().devices[1].opens, 1);
    SDL_GameControllerClose(active);
  }

  /**
   * @brief A removal handler may already have closed the reference before input polling scans the remaining ports.
   */
  TEST_F(ShellControllerTest, AcquiresExistingDevicesAfterTheRemovalHandlerClosesThePrimary) {
    attach(0);
    attach(1);
    auto *active = SDL_GameControllerOpen(0);
    detach(0);
    SDL_GameControllerClose(active);
    active = nullptr;
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[1]);
    EXPECT_EQ(registry().devices[0].closes, 1);
    SDL_GameControllerClose(active);
  }

  /**
   * @brief Recovery retries after a temporary empty device list even when no hotplug event accompanies its return.
   */
  TEST_F(ShellControllerTest, RetriesWithoutRenderingOrDeviceAddedEvents) {
    attach(0);
    auto *active = SDL_GameControllerOpen(0);
    detach(0);
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, nullptr);
    EXPECT_FALSE(input::recover_shell_controller(&active));
    attach(1);
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[1]);
    EXPECT_EQ(registry().devices[0].closes, 1);
    SDL_GameControllerClose(active);
  }

  /**
   * @brief Unmapped, failed and concurrently removed candidates cannot prevent selection of a later live controller.
   */
  TEST_F(ShellControllerTest, SkipsUnavailableCandidatesAndClosesDetachedReferences) {
    for (int id = 0; id < 5; ++id) {
      attach(id);
    }
    registry().devices[0].mapped = false;
    registry().devices[1].openable = false;
    registry().devices[2].removedWhileOpening = true;
    SDL_GameController *active = nullptr;
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[3]);
    EXPECT_EQ(registry().devices[0].opens + registry().devices[1].opens, 0);
    EXPECT_EQ(registry().devices[2].closes, 1);
    EXPECT_EQ(registry().devices[2].references, 0);
    EXPECT_EQ(registry().devices[4].opens, 0);
    SDL_GameControllerClose(active);
  }

  /**
   * @brief No device or a null ownership slot does not report a spurious controller handoff every input frame.
   */
  TEST_F(ShellControllerTest, HandlesAbsentDevicesAndNullOwnershipSlots) {
    EXPECT_FALSE(input::recover_shell_controller(nullptr));
    EXPECT_EQ(registry().enumerationCalls, 0);
    SDL_GameController *active = nullptr;
    EXPECT_FALSE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, nullptr);
    attach(0);
    registry().devices[0].removedWhileOpening = true;
    EXPECT_FALSE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, nullptr);
    EXPECT_EQ(registry().devices[0].references, 0);
    EXPECT_EQ(registry().devices[0].closes, 1);
  }
}  // namespace
