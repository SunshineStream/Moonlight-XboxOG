/**
 * @file tests/unit/platform/fenv_compat_test.cpp
 * @brief Verify the floating-point adapters used by nxdk and FFmpeg.
 */
// test header include
#include "src/_nxdk_compat/fenv_compat.h"

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>

// lib includes
#include <lizardbyte/common/testing.h>

// local includes
#include "src/_nxdk_compat/ffmpeg_math.h"

/** @brief Preserve FFmpeg's base-two exponential fallback for both precisions. */
TEST(FfmpegMathTest, EvaluatesBaseTwoExponentials) {
  EXPECT_DOUBLE_EQ(moonlight_ffmpeg_exp2(0.0), 1.0);
  EXPECT_DOUBLE_EQ(moonlight_ffmpeg_exp2(4.0), 16.0);
  EXPECT_DOUBLE_EQ(moonlight_ffmpeg_exp2(-3.0), 0.125);
  EXPECT_FLOAT_EQ(moonlight_ffmpeg_exp2f(0.5F), 1.41421356F);
  EXPECT_FLOAT_EQ(moonlight_ffmpeg_exp2f(-2.0F), 0.25F);
}

#if defined(__i386__) || defined(__x86_64__)
namespace {

  /**
   * @brief Read the x87 control word without changing the floating-point environment.
   *
   * @return Current x87 control word.
   */
  std::uint16_t read_control_word() {
    std::uint16_t controlWord = 0;
    asm volatile("fnstcw %0" : "=m"(controlWord));
    return controlWord;
  }

  /**
   * @brief Restore the x87 control word after a rounding test.
   */
  class FenvCompatTest: public BaseTest {  // NOSONAR(cpp:S3656) protected members are required by gtest
  protected:
    std::uint16_t originalControlWord = 0;  ///< Control word restored after each test.

    /**
     * @brief Save the control word before changing the rounding mode.
     */
    void SetUp() override {
      BaseTest::SetUp();
      originalControlWord = read_control_word();
    }

    /**
     * @brief Restore the original control word after each test.
     */
    void TearDown() override {
      asm volatile("fldcw %0" : : "m"(originalControlWord));
      BaseTest::TearDown();
    }
  };

  TEST_F(FenvCompatTest, PreservesExceptionMasksAndPrecisionWhenChangingRoundingMode) {
    const auto initialControlWord = static_cast<std::uint16_t>(originalControlWord | 0x003F);
    asm volatile("fldcw %0" : : "m"(initialControlWord));

    for (const int roundingMode : std::array {0x0000, 0x0400, 0x0800, 0x0C00}) {
      ASSERT_EQ(nxdk_compat::set_x87_rounding_mode(roundingMode), 0);
      const std::uint16_t controlWord = read_control_word();
      EXPECT_EQ(controlWord & 0x0C00, roundingMode);
      EXPECT_EQ(controlWord & ~0x0C00, initialControlWord & ~0x0C00);
    }
  }

  TEST_F(FenvCompatTest, RejectsInvalidRoundingModesWithoutChangingControlWord) {
    for (const int roundingMode : std::array {-1, 0x0001, 0x0100, 0x1000}) {
      EXPECT_EQ(nxdk_compat::set_x87_rounding_mode(roundingMode), -1);
      EXPECT_EQ(read_control_word(), originalControlWord);
    }
  }

  /** @brief FFmpeg integer conversions honor every x87 rounding mode in both precisions. */
  TEST_F(FenvCompatTest, RoundsFfmpegValuesUsingTheActiveMode) {
    const std::array modes {0x0000, 0x0400, 0x0800, 0x0C00};
    const std::array expectedPositive {2, 2, 3, 2};
    const std::array expectedNegative {-2, -3, -2, -2};

    for (std::size_t i = 0; i < modes.size(); ++i) {
      ASSERT_EQ(nxdk_compat::set_x87_rounding_mode(modes[i]), 0);
      EXPECT_EQ(moonlight_ffmpeg_lrint(2.5), expectedPositive[i]);
      EXPECT_EQ(moonlight_ffmpeg_lrintf(-2.5F), expectedNegative[i]);
      EXPECT_EQ(moonlight_ffmpeg_llrint(2.5), expectedPositive[i]);
      EXPECT_EQ(moonlight_ffmpeg_llrintf(-2.5F), expectedNegative[i]);
    }
  }

  /** @brief FFmpeg's wide conversion preserves integers larger than an Xbox long. */
  TEST_F(FenvCompatTest, RoundsFfmpegValuesBeyondThe32BitRange) {
    ASSERT_EQ(nxdk_compat::set_x87_rounding_mode(0x0000), 0);
    EXPECT_EQ(moonlight_ffmpeg_llrint(4294967297.75), 4294967298LL);
    EXPECT_EQ(moonlight_ffmpeg_llrintf(-4294967296.0F), -4294967296LL);
  }

}  // namespace
#endif
