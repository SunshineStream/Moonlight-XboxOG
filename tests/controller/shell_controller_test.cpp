/**
 * @file tests/controller/shell_controller_test.cpp
 * @brief Verifies recovery from removed virtual gamepads and preservation of live menu-controller references.
 */
// test header include
#include "src/input/shell_controller.h"

// standard includes
#include <algorithm>
#include <array>
#include <unordered_map>
#include <utility>
#include <vector>

// lib includes
#include <lizardbyte/common/testing.h>

/**
 * @brief Stable controller handle whose device can disappear from enumeration while its reference remains open.
 */
struct SDL_Gamepad {
  int slot = 0;  ///< Index into the fake registry, independent of the SDL instance ID.
};

namespace {
  // Exercise the upper half of SDL3's unsigned ID range.
  constexpr SDL_JoystickID FIRST_INSTANCE = 0x80000011U;

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
    std::array<SDL_Gamepad, 5> handles;  ///< Stable objects returned by successful opens.
    std::vector<int> available;  ///< Current enumeration order; removal changes indices, not identities.
    bool enumerationFailed = false;  ///< Simulated failure allocating the next enumeration.
    std::unordered_map<SDL_JoystickID *, std::vector<SDL_JoystickID>> enumerations;  ///< Snapshots owned by SDL until SDL_free.
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
      registry().enumerationFailed = false;
      registry().enumerations.clear();
    }

    /** @brief Check that all SDL enumeration snapshots were released. */
    void TearDown() override {
      EXPECT_TRUE(registry().enumerations.empty());
      BaseTest::TearDown();
    }

    /**
     * @brief Add a device without delivering a device-added notification to the recovery code.
     * @param id Stable registry identity.
     */
    void attach(int id) const {
      registry().handles[id].slot = id;
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
  SDL_JoystickID *SDL_GetGamepads(int *count) {
    ++registry().enumerationCalls;
    *count = 0;
    if (registry().enumerationFailed) {
      return nullptr;
    }
    std::vector<SDL_JoystickID> snapshot(registry().available.size() + 1);
    for (const int slot : registry().available) {
      if (registry().devices[slot].mapped) {
        snapshot[(*count)++] = FIRST_INSTANCE + slot;
      }
    }
    snapshot[*count] = 0;
    auto *ids = snapshot.data();
    registry().enumerations.emplace(ids, std::move(snapshot));
    return ids;
  }

  void SDL_free(void *memory) {
    if (memory != nullptr) {
      EXPECT_EQ(registry().enumerations.erase(static_cast<SDL_JoystickID *>(memory)), 1U);
    }
  }

  SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance) {
    if (instance < FIRST_INSTANCE || instance - FIRST_INSTANCE >= registry().devices.size()) {
      return nullptr;
    }
    const auto id = instance - FIRST_INSTANCE;
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

  void SDL_CloseGamepad(const SDL_Gamepad *controller) {
    auto &device = registry().devices[controller->slot];
    EXPECT_GT(device.references, 0);
    --device.references;
    ++device.closes;
  }

  bool SDL_GamepadConnected(const SDL_Gamepad *controller) {
    return registry().devices[controller->slot].attached;
  }
}

namespace {
  /**
   * @brief A newly appearing host virtual pad must not steal the working menu controller.
   */
  TEST_F(ShellControllerTest, PreservesTheWorkingControllerAndItsReference) {
    attach(0);
    auto *active = SDL_OpenGamepad(FIRST_INSTANCE);
    attach(1);
    registry().available = {1, 0};
    EXPECT_FALSE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[0]);
    EXPECT_EQ(registry().devices[0].references, 1);
    EXPECT_EQ(registry().devices[0].closes, 0);
    EXPECT_EQ(registry().devices[1].opens, 0);
    EXPECT_EQ(registry().enumerationCalls, 0);
    SDL_CloseGamepad(active);
  }

  /**
   * @brief Quit-related removal falls back to a pad that was already attached, without any new ADDED event.
   */
  TEST_F(ShellControllerTest, ReplacesARemovedPrimaryWithAnAlreadyConnectedPad) {
    attach(0);
    attach(1);
    auto *active = SDL_OpenGamepad(FIRST_INSTANCE);
    detach(0);
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[1]);
    EXPECT_EQ(registry().devices[0].closes, 1);
    EXPECT_EQ(registry().devices[0].references, 0);
    EXPECT_EQ(registry().devices[1].references, 1);
    EXPECT_FALSE(input::recover_shell_controller(&active));
    EXPECT_EQ(registry().devices[1].opens, 1);
    SDL_CloseGamepad(active);
  }

  /**
   * @brief A removal handler may already have closed the reference before input polling scans the remaining ports.
   */
  TEST_F(ShellControllerTest, AcquiresExistingDevicesAfterTheRemovalHandlerClosesThePrimary) {
    attach(0);
    attach(1);
    auto *active = SDL_OpenGamepad(FIRST_INSTANCE);
    detach(0);
    SDL_CloseGamepad(active);
    active = nullptr;
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[1]);
    EXPECT_EQ(registry().devices[0].closes, 1);
    SDL_CloseGamepad(active);
  }

  /**
   * @brief Recovery retries after a temporary empty device list even when no hotplug event accompanies its return.
   */
  TEST_F(ShellControllerTest, RetriesWithoutRenderingOrDeviceAddedEvents) {
    attach(0);
    auto *active = SDL_OpenGamepad(FIRST_INSTANCE);
    detach(0);
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, nullptr);
    EXPECT_FALSE(input::recover_shell_controller(&active));
    attach(1);
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[1]);
    EXPECT_EQ(registry().devices[0].closes, 1);
    SDL_CloseGamepad(active);
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
    SDL_Gamepad *active = nullptr;
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[3]);
    EXPECT_EQ(registry().devices[0].opens + registry().devices[1].opens, 0);
    EXPECT_EQ(registry().devices[2].closes, 1);
    EXPECT_EQ(registry().devices[2].references, 0);
    EXPECT_EQ(registry().devices[4].opens, 0);
    SDL_CloseGamepad(active);
  }

  /**
   * @brief No device or a null ownership slot does not report a spurious controller handoff every input frame.
   */
  TEST_F(ShellControllerTest, HandlesAbsentDevicesAndNullOwnershipSlots) {
    EXPECT_FALSE(input::recover_shell_controller(nullptr));
    EXPECT_EQ(registry().enumerationCalls, 0);
    SDL_Gamepad *active = nullptr;
    EXPECT_FALSE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, nullptr);
    attach(0);
    registry().devices[0].removedWhileOpening = true;
    EXPECT_FALSE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, nullptr);
    EXPECT_EQ(registry().devices[0].references, 0);
    EXPECT_EQ(registry().devices[0].closes, 1);
  }

  /**
   * @brief Recovery retries after SDL fails to allocate its gamepad snapshot.
   */
  TEST_F(ShellControllerTest, RecoversAfterAnEnumerationFailure) {
    attach(2);
    registry().enumerationFailed = true;
    SDL_Gamepad *active = nullptr;
    EXPECT_FALSE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, nullptr);
    registry().enumerationFailed = false;
    EXPECT_TRUE(input::recover_shell_controller(&active));
    EXPECT_EQ(active, &registry().handles[2]);
    SDL_CloseGamepad(active);
  }
}  // namespace
