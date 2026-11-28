/**
 * @file tests/main.cpp
 * @brief Run the shared GoogleTest suite and export guest results from Xbox builds.
 */
#ifdef NXDK
  // standard includes
  #include <string>
#endif

// lib includes
#include <lizardbyte/common/testing.h>
#ifdef NXDK
  #include <synchapi.h>
  #include <xboxkrnl/xboxkrnl.h>

  // local includes
  #include "src/platform/filesystem_utils.h"

  // test includes
  #include "tests/support/filesystem_test_utils.h"
  #include "tests/xbox/guest/gcov_serial.h"

namespace {
  /** @brief Emit result records on COM2 for the test runner. */
  class SerialResults: public testing::EmptyTestEventListener {
    /**
     * @brief Emit a result record after each test.
     * @param info The completed test.
     */
    void OnTestEnd(const testing::TestInfo &info) override {
      const auto *result = info.result();
      const char *status = result->Passed() ? "PASS" : "FAIL";
      if (result->Skipped()) {
        status = "SKIP";
      }
      const auto record = std::string("SUNSHINE_TEST_CASE ") + status + ' ' + info.test_suite_name() + ' ' + info.name() + "\r\n";
      sunshine_write_guest_text(record.c_str());
    }
  };
}  // namespace
#endif

/**
 * @brief Run GoogleTest and export guest results and counters when running on Xbox.
 * @param argc Number of command-line arguments supplied by the runtime.
 * @param argv Command-line arguments supplied by the runtime.
 * @return Test status. The Xbox guest waits for the runner to stop the emulator after exporting results.
 */
int main([[maybe_unused]] int argc, [[maybe_unused]] char **argv) {
#ifdef NXDK
  // nxdk starts main with zero arguments, which makes the argument overload skip initialization.
  testing::InitGoogleTest();
  // The emulated serial port carries ANSI escapes even though the guest has no terminal device.
  GTEST_FLAG_SET(color, "yes");
  sunshine_prepare_guest_records();
  if (!platform::ensure_directory_exists(test_support::guest_scratch_directory)) {
    DbgPrint("Failed to create the Xbox guest test directory\r\n");
    sunshine_write_guest_text("SUNSHINE_TEST_DONE 1 0\r\n");
    return 1;
  }
  auto *tests = testing::UnitTest::GetInstance();
  tests->listeners().Append(new SerialResults);
#else
  testing::InitGoogleTest(&argc, argv);
#endif

  const int result = RUN_ALL_TESTS();
#ifdef NXDK
  sunshine_write_guest_coverage();
  const auto done = "SUNSHINE_TEST_DONE " + std::to_string(result) + ' ' + std::to_string(tests->test_to_run_count()) + "\r\n";
  sunshine_write_guest_text(done.c_str());
  Sleep(INFINITE);
#endif
  return result;
}
