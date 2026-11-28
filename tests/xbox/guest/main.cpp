/**
 * @file tests/xbox/guest/main.cpp
 * @brief Report GoogleTest results and coverage to the host from the Xbox test XBE.
 */
// standard includes
#include <memory>

// lib includes
#include <lizardbyte/common/testing.h>
#include <synchapi.h>
#include <xboxkrnl/xboxkrnl.h>

// local includes
#include "gcov_serial.h"

namespace {
  /** @brief Emit result records on the kernel's serial console for the test runner. */
  class SerialResults: public testing::EmptyTestEventListener {
    /**
     * @brief Mirror a failing assertion to the serial log for diagnosis.
     * @param result The assertion's result and diagnostic.
     */
    void OnTestPartResult(const testing::TestPartResult &result) override {
      if (result.failed()) {
        DbgPrint("Assertion at %s:%d\r\n", result.file_name(), result.line_number());
        DbgPrint("%.200s\r\n", result.summary());
      }
    }

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
      DbgPrint("SUNSHINE_TEST_CASE %s %s %s\r\n", status, info.test_suite_name(), info.name());
    }
  };
}  // namespace

/**
 * @brief Run the instrumented guest tests and report completion after exporting counters.
 * @return This process waits for the emulator runner to shut down the guest.
 */
int main() {
  testing::InitGoogleTest();
  auto *tests = testing::UnitTest::GetInstance();
  auto &listeners = tests->listeners();
  std::unique_ptr<testing::TestEventListener> defaultPrinter {
    listeners.Release(listeners.default_result_printer())
  };
  defaultPrinter.reset();
  listeners.Append(new SerialResults);

  const int result = RUN_ALL_TESTS();
  sunshine_write_guest_coverage();
  DbgPrint("SUNSHINE_TEST_DONE %d %d\r\n", result, tests->test_to_run_count());
  Sleep(INFINITE);
  return result;
}
