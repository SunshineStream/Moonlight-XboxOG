/**
 * @file tests/unit/streaming/video_acceleration_test.cpp
 * @brief Covers overlay buffer boundaries and persistent fallback diagnostics.
 */
// test header include
#include "src/streaming/video_acceleration.h"

// standard includes
#include <array>
#include <numeric>
#include <utility>

// lib includes
#include <lizardbyte/common/testing.h>

// local includes
#include "src/streaming/aligned_media_buffer.h"

/**
 * @brief Packing respects source strides, odd heights, chroma reuse and destination padding.
 */
TEST(VideoAccelerationTest, PacksStridedPlanesWithoutOverwritingPadding) {
  const std::array<std::uint8_t, 12> y {10, 20, 99, 99, 30, 40, 99, 99, 50, 60, 99, 99};
  const std::array<std::uint8_t, 4> u {70, 99, 80, 99};
  const std::array<std::uint8_t, 4> v {90, 99, 100, 99};
  const streaming::Yuv420FrameView frame {2, 3, {y.data(), u.data(), v.data()}, {y.size(), u.size(), v.size()}, {4, 2, 2}};
  std::array<std::uint8_t, 18> output;
  output.fill(0xcc);
  ASSERT_TRUE(streaming::pack_overlay_yuy2(frame, output.data(), output.size(), 6));
  EXPECT_EQ(output, (std::array<std::uint8_t, 18> {10, 70, 20, 90, 0xcc, 0xcc, 30, 70, 40, 90, 0xcc, 0xcc, 50, 80, 60, 100, 0xcc, 0xcc}));
}

/**
 * @brief Wide packing handles unaligned planes, a partial block and exact last-row bounds.
 */
TEST(VideoAccelerationTest, PacksWideRowsAndTailWithUnalignedStorage) {
  constexpr int width = 34;
  constexpr int height = 3;
  constexpr int lumaPitch = 37;
  constexpr int chromaPitch = 19;
  constexpr int outputPitch = 73;
  constexpr std::size_t outputBytes = (height - 1) * outputPitch + width * 2;
  std::array<std::uint8_t, 1 + (height - 1) * lumaPitch + width> y;
  std::array<std::uint8_t, 1 + chromaPitch + width / 2> u;
  std::array<std::uint8_t, 1 + chromaPitch + width / 2> v;
  std::iota(y.begin(), y.end(), 0);
  std::iota(u.begin(), u.end(), 120);
  std::iota(v.begin(), v.end(), 170);
  const streaming::Yuv420FrameView frame {
    width,
    height,
    {y.data() + 1, u.data() + 1, v.data() + 1},
    {y.size() - 1, u.size() - 1, v.size() - 1},
    {lumaPitch, chromaPitch, chromaPitch},
  };
  std::array<std::uint8_t, outputBytes + 2> output;
  output.fill(0xcc);
  auto expected = output;
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      const auto index = 1 + row * outputPitch + column * 2;
      const auto &chroma = column % 2 == 0 ? u : v;
      expected[index] = y[1 + row * lumaPitch + column];
      expected[index + 1] = chroma[1 + (row / 2) * chromaPitch + column / 2];
    }
  }
  ASSERT_TRUE(streaming::pack_overlay_yuy2(frame, output.data() + 1, outputBytes, outputPitch));
  EXPECT_EQ(output, expected);
}

/**
 * @brief Truncated planes and unsupported dimensions never touch destination storage.
 */
TEST(VideoAccelerationTest, RejectsInvalidBuffersBeforeWriting) {
  const std::array<std::uint8_t, 4> y {16, 32, 48, 64};
  const std::array<std::uint8_t, 1> chroma {128};
  streaming::Yuv420FrameView frame {2, 2, {y.data(), chroma.data(), chroma.data()}, {4, 1, 1}, {2, 1, 1}};
  std::array<std::uint8_t, 8> output;
  output.fill(0xcc);
  EXPECT_FALSE(streaming::pack_overlay_yuy2(frame, output.data(), 7, 4));
  frame.sizes[1] = 0;
  EXPECT_FALSE(streaming::pack_overlay_yuy2(frame, output.data(), output.size(), 4));
  frame.width = 3;
  EXPECT_FALSE(streaming::pack_overlay_yuy2(frame, output.data(), output.size(), 6));
  for (const auto byte : output) {
    EXPECT_EQ(byte, 0xcc);
  }
}

/**
 * @brief Each absent source plane is rejected without modifying the destination.
 */
TEST(VideoAccelerationTest, RejectsMissingPlanesBeforeWriting) {
  const std::array<std::uint8_t, 4> y {16, 32, 48, 64};
  const std::array<std::uint8_t, 1> chroma {128};
  const streaming::Yuv420FrameView frame {2, 2, {y.data(), chroma.data(), chroma.data()}, {4, 1, 1}, {2, 1, 1}};
  std::array<std::uint8_t, 8> output;
  output.fill(0xcc);
  const auto unchanged = output;
  for (std::size_t plane = 0; plane < frame.planes.size(); ++plane) {
    auto invalid = frame;
    invalid.planes[plane] = nullptr;
    EXPECT_EQ(streaming::overlay_frame_error(invalid), "Source plane storage is invalid");
    EXPECT_FALSE(streaming::pack_overlay_yuy2(invalid, output.data(), output.size(), 4));
    EXPECT_EQ(output, unchanged);
  }
}

/**
 * @brief Full-range or interlaced sources explicitly select the software path.
 */
TEST(VideoAccelerationTest, ReportsUnsupportedColorAndInterlacing) {
  streaming::Yuv420FrameView frame;
  frame.width = 640;
  frame.height = 480;
  frame.fullRange = true;
  EXPECT_NE(streaming::overlay_frame_error(frame).find("color range"), std::string::npos);
  frame.fullRange = false;
  frame.interlaced = true;
  EXPECT_NE(streaming::overlay_frame_error(frame).find("interlacing"), std::string::npos);
}

/**
 * @brief Later GPU successes cannot erase a fallback from the end-of-stream summary.
 */
TEST(VideoAccelerationTest, RetainsFirstFallbackAfterAccelerationRecovers) {
  streaming::VideoAccelerationReport report;
  report.record_fallback("Not enough contiguous memory");
  report.softwareFrames = 2;
  report.gpuFrames = 5;
  report.record_fallback("Later failure");
  const auto lines = report.summary_lines();
  ASSERT_GE(lines.size(), 3U);
  EXPECT_EQ(lines[0], "SOFTWARE FALLBACK USED");
  EXPECT_NE(lines[1].find("5 frames"), std::string::npos);
  EXPECT_EQ(lines[2], "Reason: Not enough contiguous memory");
}

/**
 * @brief Explicit software rendering is distinct from a failed GPU attempt in the summary.
 */
TEST(VideoAccelerationTest, ReportsSelectedSoftwareWithoutAFallback) {
  streaming::VideoAccelerationReport report;
  report.gpuPresentationRequested = false;
  report.softwareFrames = 3;
  const auto lines = report.summary_lines();
  ASSERT_GE(lines.size(), 2U);
  EXPECT_EQ(lines[0], "Software presentation selected");
  EXPECT_EQ(lines[1], "GPU overlay: 0 frames | Software: 3 frames");
  EXPECT_TRUE(report.fallbackReason.empty());
}

/**
 * @brief Media scratch buffers retain alignment and zero padding after reuse and moves.
 */
TEST(VideoAccelerationTest, KeepsMediaBuffersAlignedAndPadded) {
  streaming::AlignedMediaBuffer buffer;
  for (const auto size : {1U, 73U, 4097U, 23U}) {
    buffer.resize(size);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buffer.data()) % 64U, 0U);
    EXPECT_EQ(buffer.size(), size);
    for (std::size_t index = 0; index < streaming::AlignedMediaBuffer::PADDING; ++index) {
      EXPECT_EQ(buffer.data()[size + index], 0);
    }
    buffer.data()[0] = 42;
  }
  auto moved = std::move(buffer);
  EXPECT_EQ(buffer.size(), 0U);  // NOSONAR(cpp:S5272) AlignedMediaBuffer guarantees an empty source after a move.
  EXPECT_EQ(buffer.data(), nullptr);
  EXPECT_EQ(moved.data()[0], 42);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(moved.data()) % 64U, 0U);
}

/** @brief Move assignment transfers the aligned address without losing reusable capacity. */
TEST(VideoAccelerationTest, TransfersAlignedStorageWhenReplacingAnAllocation) {
  streaming::AlignedMediaBuffer source;
  source.resize(65);
  auto *const aligned = source.data();
  aligned[64] = 42;
  streaming::AlignedMediaBuffer destination;
  destination.resize(512);

  destination = std::move(source);
  const auto &readOnly = destination;
  EXPECT_EQ(readOnly.data(), aligned);
  EXPECT_EQ(readOnly.size(), 65U);
  EXPECT_EQ(readOnly.data()[64], 42);
  EXPECT_EQ(readOnly.data()[65], 0);

  destination.clear();
  EXPECT_EQ(destination.data(), nullptr);
  EXPECT_EQ(destination.size(), 0U);
  destination.resize(0);
  EXPECT_EQ(destination.data(), aligned);
  EXPECT_EQ(destination.size(), 0U);
  EXPECT_EQ(destination.data()[0], 0);
}
