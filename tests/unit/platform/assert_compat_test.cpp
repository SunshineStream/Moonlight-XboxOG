/**
 * @file tests/unit/platform/assert_compat_test.cpp
 * @brief Verifies nxdk assertion reporting through the application logger.
 */
// standard includes
#include <string>
#include <vector>

// lib includes
#include <lizardbyte/common/testing.h>

// local includes
#include "src/_nxdk_compat/assert_compat.h"
#include "src/logging/logger.h"

namespace {

  /**
   * @brief Register a runtime logger without enabling the startup framebuffer console.
   */
  class AssertCompatTest: public BaseTest {  // NOSONAR(cpp:S3656) protected members are required by gtest
  protected:
    logging::Logger logger;  ///< Structured logger receiving assertion diagnostics.

    /**
     * @brief Register the logger with ordinary runtime logging enabled.
     */
    void SetUp() override {
      BaseTest::SetUp();
      logger.set_minimum_level(logging::LogLevel::trace);
      logger.set_startup_debug_enabled(false);
      logging::set_global_logger(&logger);
    }

    /**
     * @brief Remove the logger before the fixture releases it.
     */
    void TearDown() override {
      logging::set_global_logger(nullptr);
      BaseTest::TearDown();
    }
  };

  /**
   * @brief Assertion diagnostics retain their expression, function and original location in configured sinks.
   */
  TEST_F(AssertCompatTest, ReportsFailuresToRuntimeLogAndConfiguredFileSink) {
    std::vector<logging::LogEntry> writtenEntries;
    logger.set_file_minimum_level(logging::LogLevel::error);
    logger.set_file_sink([&writtenEntries](const logging::LogEntry &entry) {
      writtenEntries.push_back(entry);
    });

    nxdk_compat::report_assertion_failure("value != nullptr", "lib/decoder.c", "decode_frame", 42);

    ASSERT_EQ(logger.entries().size(), 1U);
    const logging::LogEntry &entry = logger.entries().front();
    EXPECT_EQ(entry.level, logging::LogLevel::error);
    EXPECT_EQ(entry.category, "assert");
    EXPECT_EQ(entry.message, "Assertion failed: 'value != nullptr' in function 'decode_frame'");
    EXPECT_STREQ(entry.sourceLocation.file, "lib/decoder.c");
    EXPECT_EQ(entry.sourceLocation.line, 42);
    ASSERT_EQ(writtenEntries.size(), 1U);
    EXPECT_EQ(logging::format_entry(writtenEntries.front()), "[ERROR] [decoder.c:42] assert: Assertion failed: 'value != nullptr' in function 'decode_frame'");
    EXPECT_FALSE(logger.startup_debug_enabled());
  }

  /**
   * @brief Missing source metadata still produces a useful assertion record.
   */
  TEST_F(AssertCompatTest, ReportsFailuresWithoutSourceMetadata) {
    nxdk_compat::report_assertion_failure(nullptr, nullptr, nullptr, 0);

    ASSERT_EQ(logger.entries().size(), 1U);
    EXPECT_EQ(logger.entries().front().message, "Assertion failed: '<unknown>' in function '<unknown>'");
    EXPECT_FALSE(logger.entries().front().sourceLocation.valid());
  }

  /**
   * @brief Assertions before logger registration do not require an initialized application logger.
   */
  TEST_F(AssertCompatTest, AcceptsFailuresBeforeLoggerInitialization) {
    logging::set_global_logger(nullptr);

    nxdk_compat::report_assertion_failure("0", "lib/runtime.c", "initialize", 6);

    EXPECT_TRUE(logger.entries().empty());
  }

}  // namespace
