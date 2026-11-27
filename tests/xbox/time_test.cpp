/**
 * @file tests/xbox/time_test.cpp
 * @brief Verifies Xbox UTC epoch conversion, calendar fields and overflow rejection.
 */
// test header include
#include "src/_nxdk_compat/include/sunshine_time.h"

// standard includes
#include <cstdint>
#include <limits>

// lib includes
#include <lizardbyte/common/testing.h>

namespace {
  /**
   * @brief Scripted kernel reply and observed timestamp for one conversion scenario.
   */
  struct KernelTimeState {
    TIME_FIELDS reply;  ///< Calendar fields supplied by the kernel stub.
    std::int64_t ticks = 0;  ///< Most recently supplied NT timestamp.
    int calls = 0;  ///< Number of kernel conversions requested.
  };

  /**
   * @brief Access the state shared with the kernel test callback.
   * @return Registry reset before each scenario.
   */
  KernelTimeState &kernel_time() {
    static KernelTimeState state;
    return state;
  }

  /**
   * @brief Reset kernel responses and observation state for each UTC conversion test.
   */
  class XboxTimeTest: public BaseTest {
  protected:
    /**
     * @brief Start each test with the Unix epoch as its kernel reply.
     */
    void SetUp() override {
      BaseTest::SetUp();
      kernel_time() = {};
    }
  };
}  // namespace

void RtlTimeToTimeFields(const LARGE_INTEGER *time, TIME_FIELDS *fields) {
  auto &state = kernel_time();
  ++state.calls;
  state.ticks = time->QuadPart;
  *fields = state.reply;
}

/**
 * @brief The Unix epoch reaches the kernel as NT ticks and populates every standard calendar field.
 */
TEST_F(XboxTimeTest, ConvertsTheUnixEpochIntoCallerOwnedUtcFields) {
  std::tm result {};
  result.tm_isdst = 1;
  ASSERT_TRUE(sunshine_gmtime(0, result));
  EXPECT_EQ(kernel_time().calls, 1);
  EXPECT_EQ(kernel_time().ticks, 116444736000000000LL);
  EXPECT_EQ(result.tm_year, 70);
  EXPECT_EQ(result.tm_mon, 0);
  EXPECT_EQ(result.tm_mday, 1);
  EXPECT_EQ(result.tm_hour, 0);
  EXPECT_EQ(result.tm_min, 0);
  EXPECT_EQ(result.tm_sec, 0);
  EXPECT_EQ(result.tm_wday, 4);
  EXPECT_EQ(result.tm_yday, 0);
  EXPECT_EQ(result.tm_isdst, 0);
}

/**
 * @brief Leap day conversion preserves clock fields and a second conversion does not overwrite earlier output.
 */
TEST_F(XboxTimeTest, PreservesLeapDayAndIndependentResultStorage) {
  kernel_time().reply = {2000, 2, 29, 12, 34, 56, 0, 2};
  std::tm first {};
  ASSERT_TRUE(sunshine_gmtime(951827696, first));
  EXPECT_EQ(kernel_time().ticks, 125963012960000000LL);
  EXPECT_EQ(first.tm_year, 100);
  EXPECT_EQ(first.tm_mon, 1);
  EXPECT_EQ(first.tm_mday, 29);
  EXPECT_EQ(first.tm_hour, 12);
  EXPECT_EQ(first.tm_min, 34);
  EXPECT_EQ(first.tm_sec, 56);
  EXPECT_EQ(first.tm_wday, 2);
  EXPECT_EQ(first.tm_yday, 59);
  kernel_time().reply = {2000, 3, 1};
  std::tm second {};
  ASSERT_TRUE(sunshine_gmtime(951868800, second));
  EXPECT_EQ(second.tm_yday, 60);
  EXPECT_EQ(first.tm_mday, 29);
  EXPECT_EQ(first.tm_hour, 12);
}

/**
 * @brief Gregorian century exceptions and ordinary leap years produce the correct day of year.
 */
TEST_F(XboxTimeTest, AppliesGregorianCenturyAndLeapYearRules) {
  kernel_time().reply = {2100, 3, 1};
  std::tm result {};
  ASSERT_TRUE(sunshine_gmtime(4107542400LL, result));
  EXPECT_EQ(kernel_time().ticks, 157520160000000000LL);
  EXPECT_EQ(result.tm_year, 200);
  EXPECT_EQ(result.tm_mon, 2);
  EXPECT_EQ(result.tm_yday, 59);
  kernel_time().reply = {2024, 3, 1};
  ASSERT_TRUE(sunshine_gmtime(1709251200, result));
  EXPECT_EQ(result.tm_yday, 60);
}

/**
 * @brief Invalid and overflowing timestamps never call the kernel or alter caller-owned output.
 */
TEST_F(XboxTimeTest, RejectsNegativeTimeAndNtTickOverflow) {
  std::tm result {};
  result.tm_year = 123;
  EXPECT_FALSE(sunshine_gmtime(-1, result));
  constexpr std::int64_t lastSecond = std::numeric_limits<std::int64_t>::max() / 10000000 - 11644473600LL;
  EXPECT_FALSE(sunshine_gmtime(lastSecond + 1, result));
  EXPECT_EQ(kernel_time().calls, 0);
  EXPECT_EQ(result.tm_year, 123);
  EXPECT_TRUE(sunshine_gmtime(lastSecond, result));
  EXPECT_EQ(kernel_time().calls, 1);
  EXPECT_LE(kernel_time().ticks, std::numeric_limits<std::int64_t>::max());
}
