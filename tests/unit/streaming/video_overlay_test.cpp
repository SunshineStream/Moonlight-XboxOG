/**
 * @file tests/unit/streaming/video_overlay_test.cpp
 * @brief Verifies equivalent GPU/software panel pixels and destination bounds.
 */
// test includes
#include <lizardbyte/common/testing.h>

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// local includes
#include "src/streaming/video_overlay.h"

/**
 * @brief Opaque OSK colors are not brightened or made translucent by legacy SDL alpha rounding.
 */
TEST(VideoOverlayTest, KeepsTheOskOpaqueDespiteSdlAlphaRounding) {
  const std::array<std::uint8_t, 4> pixel {100, 150, 200, 254};
  EXPECT_EQ(streaming::extract_overlay_bitmap(pixel.data(), pixel.size(), 4, 1, 1, true), (std::vector<std::uint8_t> {100, 150, 200, 255}));
}

/**
 * @brief The same original SVG colors and translucent edges survive all Xbox RGB depths.
 */
TEST(VideoOverlayTest, PreservesSvgColorsAcrossRgbFramebuffers) {
  streaming::VideoOverlay overlay;
  const std::vector<std::uint8_t> colors {0, 255, 0, 255, 0, 0, 255, 128, 255, 0, 0, 255, 255, 255, 255, 0};
  overlay.set_argb_bitmap(4, 1, colors);
  const std::array<int, 3> depths {15, 16, 32};
  const std::array<std::array<std::uint32_t, 4>, 3> expectedColors {{
    {0x83e0U, 0xc00fU, 0x801fU, 0x801fU},
    {0x07e0U, 0x800fU, 0x001fU, 0x001fU},
    {0xff00ff00U, 0xff80007fU, 0xff0000ffU, 0xff0000ffU},
  }};
  for (std::size_t format = 0; format < depths.size(); ++format) {
    const int bytes = depths[format] == 32 ? 4 : 2;
    const int pitch = 24 * bytes + 5;
    std::vector<std::uint8_t> frame(pitch * 16, 0xcc);
    for (int column = 0; column < 4; ++column) {
      std::memcpy(frame.data() + 8 * pitch + (8 + column) * bytes, &expectedColors[format][3], bytes);
    }
    auto expected = frame;
    for (int column = 0; column < 4; ++column) {
      std::memcpy(expected.data() + 8 * pitch + (8 + column) * bytes, &expectedColors[format][column], bytes);
    }
    ASSERT_TRUE(overlay.draw_rgb(frame.data(), frame.size(), pitch, 24, 16, depths[format], {0, 0, 24, 16}));
    EXPECT_EQ(frame, expected);
  }
  EXPECT_EQ(overlay.argb_pixels(), colors);
}

/**
 * @brief Colored GPU panels follow BT.601 and BT.709 and invalidate cached chroma when the matrix changes.
 */
TEST(VideoOverlayTest, EncodesSvgColorsUsingTheHostVideoMatrix) {
  streaming::VideoOverlay overlay;
  overlay.set_argb_bitmap(4, 1, {0, 0, 255, 255, 0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255});
  constexpr int pitch = 24 * 2;
  std::array<std::uint8_t, pitch * 16> frame;
  frame.fill(70);
  ASSERT_TRUE(overlay.draw_yuy2(frame.data(), frame.size(), pitch, 24, 16, {0, 0, 24, 16}, streaming::VideoColorMatrix::bt601));
  const std::array<std::uint8_t, 8> bt601 {81, 90, 81, 240, 145, 147, 41, 72};
  EXPECT_TRUE(std::equal(bt601.begin(), bt601.end(), frame.begin() + 8 * pitch + 8 * 2));
  frame.fill(70);
  ASSERT_TRUE(overlay.draw_yuy2(frame.data(), frame.size(), pitch, 24, 16, {0, 0, 24, 16}, streaming::VideoColorMatrix::bt709));
  const std::array<std::uint8_t, 8> bt709 {63, 102, 63, 240, 173, 141, 32, 72};
  EXPECT_TRUE(std::equal(bt709.begin(), bt709.end(), frame.begin() + 8 * pitch + 8 * 2));
  const auto before = frame;
  EXPECT_FALSE(overlay.draw_yuy2(frame.data(), frame.size(), pitch, 24, 16, {0, 0, 24, 16}, streaming::VideoColorMatrix::unsupported));
  EXPECT_EQ(frame, before);
}

/**
 * @brief Transparent colored pixels do not tint shared GPU chroma beside an opaque glyph.
 */
TEST(VideoOverlayTest, WeightsGpuChromaByVisibleColorCoverage) {
  streaming::VideoOverlay overlay;
  overlay.set_argb_bitmap(4, 1, {0, 0, 255, 255, 255, 0, 0, 0, 0, 255, 0, 0, 255, 0, 0, 0});
  constexpr int pitch = 24 * 2;
  std::array<std::uint8_t, pitch * 16> frame;
  frame.fill(128);
  const int at = 8 * pitch + 8 * 2;
  frame[at] = frame[at + 2] = 70;
  ASSERT_TRUE(overlay.draw_yuy2(frame.data(), frame.size(), pitch, 24, 16, {0, 0, 24, 16}));
  EXPECT_EQ(frame[at], 81);
  EXPECT_EQ(frame[at + 1], 109);
  EXPECT_EQ(frame[at + 2], 70);
  EXPECT_EQ(frame[at + 3], 184);
  EXPECT_EQ(frame[at + 4], 128);
  EXPECT_EQ(frame[at + 5], 128);
}

/**
 * @brief Switching between opaque colored OSK pixels and grayscale statistics cannot reuse stale caches.
 */
TEST(VideoOverlayTest, SwitchesBetweenColorAndStatisticsAndRejectsInvalidArgb) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(2, 1, {64, 128});
  EXPECT_EQ(overlay.argb_pixels().size(), 8);
  const std::vector<std::uint8_t> colors {0, 0, 255, 255, 0, 255, 0, 255};
  overlay.set_argb_bitmap(2, 1, colors);
  std::array<std::uint8_t, 16 * 16 * 4> frame {};
  ASSERT_TRUE(overlay.draw_rgb(frame.data(), frame.size(), 16 * 4, 16, 16, 32, {0, 0, 16, 16}));
  EXPECT_TRUE(std::equal(colors.begin(), colors.end(), frame.begin() + (8 * 16 + 8) * 4));
  overlay.set_bitmap(1, 1, {64});
  std::uint32_t gray = 0;
  std::memcpy(&gray, overlay.argb_pixels().data(), 4);
  EXPECT_EQ(gray, 0xff404040U);
  overlay.set_argb_bitmap(2, 1, {0});
  EXPECT_EQ(overlay.width(), 0);
  EXPECT_TRUE(overlay.argb_pixels().empty());
  overlay.set_argb_bitmap(0, 1, {});
  EXPECT_EQ(overlay.height(), 0);
  overlay.set_argb_bitmap(1, 0, {});
  EXPECT_EQ(overlay.width(), 0);
  overlay.set_argb_bitmap(2049, 1, {});
  EXPECT_EQ(overlay.width(), 0);
  overlay.set_argb_bitmap(1, 2049, {});
  EXPECT_EQ(overlay.height(), 0);
}

/**
 * @brief Extracted text keeps its opacity without darkening antialiased edges twice.
 */
TEST(VideoOverlayTest, ExtractsOriginalColorsFromTransparentArgb) {
  const std::array<std::uint8_t, 24> surface {
    64,
    64,
    64,
    128,
    255,
    255,
    255,
    0,
    1,
    2,
    3,
    4,
    0,
    0,
    255,
    255,
    128,
    128,
    128,
    128,
    5,
    6,
    7,
    8,
  };
  const auto bitmap = streaming::extract_overlay_bitmap(surface.data(), surface.size(), 12, 2, 2);
  EXPECT_EQ(bitmap, (std::vector<std::uint8_t> {128, 128, 128, 128, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255, 255, 128}));
  EXPECT_TRUE(streaming::extract_overlay_bitmap(nullptr, surface.size(), 12, 2, 2).empty());
  EXPECT_TRUE(streaming::extract_overlay_bitmap(surface.data(), 1, 12, 2, 2).empty());
  std::array<std::uint8_t, 2049 * 4> oversized {};
  EXPECT_TRUE(streaming::extract_overlay_bitmap(oversized.data(), oversized.size(), 2049 * 4, 2049, 1).empty());
  EXPECT_TRUE(streaming::extract_overlay_bitmap(oversized.data(), oversized.size(), 4, 1, 2049).empty());
}

/**
 * @brief Transparent areas preserve the host framebuffer while text blends in every Xbox RGB format.
 */
TEST(VideoOverlayTest, AlphaBlendsRgbAndPreservesUncoveredStorage) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(4, 1, {255, 255, 64, 32}, streaming::VideoOverlayPlacement::top_left, {0, 128, 255, 255});
  const std::array<int, 3> depths {15, 16, 32};
  const std::array<std::array<std::uint32_t, 4>, 3> colors {{
    {0xc210U, 0xe318U, 0xa108U, 0x9084U},
    {0x8410U, 0xc618U, 0x4208U, 0x2104U},
    {0x77102030U, 0xff889098U, 0xff404040U, 0xff202020U},
  }};
  for (std::size_t format = 0; format < depths.size(); ++format) {
    const int bytes = depths[format] == 32 ? 4 : 2;
    const int pitch = 24 * bytes + 7;
    std::vector<std::uint8_t> frame(pitch * 16, 0xcc);
    for (int pixel = 0; pixel < 4; ++pixel) {
      std::memcpy(frame.data() + 8 * pitch + (8 + pixel) * bytes, &colors[format][0], bytes);
    }
    auto expected = frame;
    for (int pixel = 0; pixel < 4; ++pixel) {
      std::memcpy(expected.data() + 8 * pitch + (8 + pixel) * bytes, &colors[format][pixel], bytes);
    }
    ASSERT_TRUE(overlay.draw_rgb(frame.data(), frame.size(), pitch, 24, 16, depths[format], {0, 0, 24, 16}));
    EXPECT_EQ(frame, expected);
  }
  const auto &argb = overlay.argb_pixels();
  std::uint32_t transparent = 0;
  std::uint32_t half = 0;
  std::memcpy(&transparent, argb.data(), 4);
  std::memcpy(&half, argb.data() + 4, 4);
  EXPECT_EQ(transparent, 0x00ffffffU);
  EXPECT_EQ(half, 0x80ffffffU);
}

/**
 * @brief GPU composition preserves transparent luma and averages coverage for shared YUY2 chroma.
 */
TEST(VideoOverlayTest, AlphaBlendsYuy2WithoutBakingTheVideoIntoItsCache) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(6, 1, {255, 255, 64, 192, 255, 255}, streaming::VideoOverlayPlacement::top_left, {0, 128, 255, 255, 0, 0});
  constexpr int pitch = 24 * 2 + 4;
  std::array<std::uint8_t, pitch * 16> frame;
  frame.fill(0xcc);
  for (int row = 0; row < 16; ++row) {
    for (int column = 0; column < 24; column += 2) {
      const int at = row * pitch + column * 2;
      frame[at] = frame[at + 2] = 50;
      frame[at + 1] = 90;
      frame[at + 3] = 170;
    }
  }
  auto expected = frame;
  const std::array<std::uint8_t, 8> composite {50, 100, 143, 159, 71, 128, 181, 128};
  std::copy(composite.begin(), composite.end(), expected.begin() + 8 * pitch + 8 * 2);
  ASSERT_TRUE(overlay.draw_yuy2(frame.data(), frame.size(), pitch, 24, 16, {0, 0, 24, 16}));
  EXPECT_EQ(frame, expected);

  // A new host frame must supply the background, even with the same cached UI.
  frame.fill(70);
  ASSERT_TRUE(overlay.draw_yuy2(frame.data(), frame.size(), pitch, 24, 16, {0, 0, 24, 16}));
  EXPECT_EQ(frame[8 * pitch + 8 * 2], 70);
  EXPECT_EQ(frame[8 * pitch + 9 * 2], 153);
  EXPECT_EQ(frame[8 * pitch + 12 * 2], 70);
}

/**
 * @brief GPU upscaling retains transparent coverage instead of filling the whole overlay rectangle.
 */
TEST(VideoOverlayTest, ScalesOpacityAlongsideGpuColors) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(2, 1, {255, 255}, streaming::VideoOverlayPlacement::bottom_center, {0, 255});
  std::vector<std::uint8_t> frame(64 * 64 * 2, 40);
  ASSERT_TRUE(overlay.draw_yuy2(frame.data(), frame.size(), 128, 64, 64, {0, 0, 32, 32}));
  EXPECT_EQ(frame[46 * 128 + 30 * 2], 40);
  EXPECT_EQ(frame[46 * 128 + 31 * 2], 40);
  EXPECT_EQ(frame[46 * 128 + 32 * 2], 235);
  EXPECT_EQ(frame[46 * 128 + 32 * 2 + 1], 128);
  EXPECT_EQ(frame[45 * 128 + 32 * 2], 40);
}

/**
 * @brief Malformed coverage clears converted caches, and subsequent opaque statistics remain opaque.
 */
TEST(VideoOverlayTest, RejectsInvalidOpacityAndRestoresOpaqueStatistics) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(2, 1, {255, 255}, streaming::VideoOverlayPlacement::top_left, {0, 128});
  EXPECT_EQ(overlay.argb_pixels().size(), 8);
  overlay.set_bitmap(2, 1, {255, 255}, streaming::VideoOverlayPlacement::top_left, {0});
  EXPECT_EQ(overlay.width(), 0);
  EXPECT_TRUE(overlay.argb_pixels().empty());
  overlay.set_bitmap(1, 1, {64});
  std::uint32_t pixel = 0;
  std::memcpy(&pixel, overlay.argb_pixels().data(), 4);
  EXPECT_EQ(pixel, 0xff404040U);
}

/**
 * @brief Direct wheel painting highlights all sectors without touching the hole or row padding.
 */
TEST(VideoOverlayTest, RasterizesWheelWithoutSoftwareRenderCommands) {
  constexpr int pitch = 20 * 4 + 4;
  std::array<std::uint8_t, pitch * 20> pixels;
  const std::array<int, 4> x {10, 15, 10, 5};
  const std::array<int, 4> y {5, 10, 15, 10};
  for (int selected = 0; selected < 4; ++selected) {
    pixels.fill(0xcc);
    ASSERT_TRUE(streaming::rasterize_menu_wheel(pixels.data(), pixels.size(), pitch, 20, 20, 10, 10, 6, selected));
    for (int sector = 0; sector < 4; ++sector) {
      EXPECT_EQ(pixels[y[sector] * pitch + x[sector] * 4], sector == selected ? 85 : 36);
      EXPECT_EQ(pixels[y[sector] * pitch + x[sector] * 4 + 3], 255);
    }
    EXPECT_EQ(pixels[6 * pitch + 14 * 4], 110);
    EXPECT_EQ(pixels[10 * pitch + 10 * 4], 0xcc);
    EXPECT_EQ(pixels[0], 0xcc);
    EXPECT_EQ(pixels[10 * pitch + 20 * 4], 0xcc);
  }
  ASSERT_TRUE(streaming::rasterize_menu_wheel(pixels.data(), pixels.size(), pitch, 20, 20, 0, 0, 4, -1));
  EXPECT_EQ(pixels[3 * 4], 36);
}

/**
 * @brief Invalid wheel storage and geometry are rejected before a pixel is written.
 */
TEST(VideoOverlayTest, RejectsInvalidWheelRasterStorage) {
  std::array<std::uint8_t, 20 * 20 * 4> pixels;
  pixels.fill(0xcc);
  EXPECT_FALSE(streaming::rasterize_menu_wheel(nullptr, pixels.size(), 80, 20, 20, 10, 10, 6, 0));
  EXPECT_FALSE(streaming::rasterize_menu_wheel(pixels.data(), 1, 80, 20, 20, 10, 10, 6, 0));
  EXPECT_FALSE(streaming::rasterize_menu_wheel(pixels.data(), pixels.size(), 80, 20, 20, -1, 10, 6, 0));
  EXPECT_FALSE(streaming::rasterize_menu_wheel(pixels.data(), pixels.size(), 80, 20, 20, 20, 10, 6, 0));
  EXPECT_FALSE(streaming::rasterize_menu_wheel(pixels.data(), pixels.size(), 80, 20, 20, 10, -1, 6, 0));
  EXPECT_FALSE(streaming::rasterize_menu_wheel(pixels.data(), pixels.size(), 80, 20, 20, 10, 20, 6, 0));
  EXPECT_FALSE(streaming::rasterize_menu_wheel(pixels.data(), pixels.size(), 80, 20, 20, 10, 10, 0, 0));
  EXPECT_FALSE(streaming::rasterize_menu_wheel(pixels.data(), pixels.size(), 80, 20, 20, 10, 10, 2049, 0));
  std::array<std::uint8_t, 2049 * 4> oversized {};
  EXPECT_FALSE(streaming::rasterize_menu_wheel(oversized.data(), oversized.size(), 2049 * 4, 2049, 1, 0, 0, 1, 0));
  EXPECT_FALSE(streaming::rasterize_menu_wheel(oversized.data(), oversized.size(), 4, 1, 2049, 0, 0, 1, 0));
  EXPECT_TRUE(std::all_of(pixels.begin(), pixels.end(), [](std::uint8_t value) {
    return value == 0xcc;
  }));
}

/**
 * @brief Crop wrapped labels on every edge without changing the renderer clip state.
 */
TEST(VideoOverlayTest, CropsOversizedCenteredLabels) {
  const auto layout = streaming::layout_overlay_label(100, 90, {10, 20, 50, 30}, false);
  EXPECT_EQ(layout.source.x, 25);
  EXPECT_EQ(layout.source.y, 30);
  EXPECT_EQ(layout.source.width, 50);
  EXPECT_EQ(layout.source.height, 30);
  EXPECT_EQ(layout.destination.x, 10);
  EXPECT_EQ(layout.destination.y, 20);
  EXPECT_EQ(layout.destination.width, 50);
  EXPECT_EQ(layout.destination.height, 30);

  const auto small = streaming::layout_overlay_label(40, 12, {10, 20, 50, 30}, false);
  EXPECT_EQ(small.source.x, 0);
  EXPECT_EQ(small.source.y, 0);
  EXPECT_EQ(small.destination.x, 15);
  EXPECT_EQ(small.destination.y, 29);
  EXPECT_EQ(small.destination.width, 40);
  EXPECT_EQ(small.destination.height, 12);
}

/**
 * @brief Single-line key labels shrink within their cells while retaining the complete source.
 */
TEST(VideoOverlayTest, FitsSingleLineKeyLabelsWithoutCropping) {
  const auto layout = streaming::layout_overlay_label(100, 50, {10, 20, 50, 30}, true);
  EXPECT_EQ(layout.source.x, 0);
  EXPECT_EQ(layout.source.y, 0);
  EXPECT_EQ(layout.source.width, 100);
  EXPECT_EQ(layout.source.height, 50);
  EXPECT_EQ(layout.destination.x, 11);
  EXPECT_EQ(layout.destination.y, 23);
  EXPECT_EQ(layout.destination.width, 48);
  EXPECT_EQ(layout.destination.height, 24);

  const auto small = streaming::layout_overlay_label(12, 8, {0, 0, 50, 30}, true);
  EXPECT_EQ(small.destination.width, 12);
  EXPECT_EQ(small.destination.height, 8);
  const auto tall = streaming::layout_overlay_label(20, 100, {0, 0, 50, 30}, true);
  EXPECT_EQ(tall.destination.height, 28);
  const auto tiny = streaming::layout_overlay_label(100, 100, {0, 0, 1, 1}, true);
  EXPECT_EQ(tiny.destination.width, 1);
  EXPECT_EQ(tiny.destination.height, 1);
}

/**
 * @brief Missing text and empty label bounds never generate a copy rectangle.
 */
TEST(VideoOverlayTest, SkipsEmptyOverlayLabels) {
  EXPECT_EQ(streaming::layout_overlay_label(0, 10, {0, 0, 20, 20}, false).destination.width, 0);
  EXPECT_EQ(streaming::layout_overlay_label(10, -1, {0, 0, 20, 20}, true).destination.height, 0);
  EXPECT_EQ(streaming::layout_overlay_label(10, 10, {0, 0, 0, 20}, false).source.width, 0);
  EXPECT_EQ(streaming::layout_overlay_label(10, 10, {0, 0, 20, -1}, true).source.height, 0);
}

/**
 * @brief YUY2 drawing leaves padding untouched and uses limited-range neutral grayscale.
 */
TEST(VideoOverlayTest, CopiesLimitedRangePanelIntoPackedVideo) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(2, 2, {0, 255, 128, 64});
  std::array<std::uint8_t, 16 * 40> output;
  output.fill(0xcc);
  ASSERT_TRUE(overlay.draw_yuy2(output.data(), output.size(), 40, 16, 16, {0, 0, 16, 16}));
  EXPECT_EQ(output[8 * 40 + 8 * 2], 16);
  EXPECT_EQ(output[8 * 40 + 8 * 2 + 1], 128);
  EXPECT_EQ(output[8 * 40 + 9 * 2], 235);
  EXPECT_EQ(output[9 * 40 + 8 * 2], 126);
  EXPECT_EQ(output[9 * 40 + 9 * 2], 71);
  EXPECT_EQ(output[8 * 40 + 7 * 2], 0xcc);
  EXPECT_EQ(output[8 * 40 + 10 * 2], 0xcc);
  EXPECT_EQ(output[8 * 40 + 39], 0xcc);
}

/**
 * @brief A full menu fits a letterboxed stream on both GPU and software paths without clipping its footer.
 */
TEST(VideoOverlayTest, FitsFullMenuInsideLetterboxedVideo) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(624, 344, std::vector<std::uint8_t>(624 * 344, 128));
  std::vector<std::uint8_t> packed(320 * 180 * 2, 0xcc);
  ASSERT_TRUE(overlay.draw_yuy2(packed.data(), packed.size(), 640, 320, 180, {0, 60, 640, 360}));
  EXPECT_EQ(packed[4 * 640 + 4 * 2], 126);
  EXPECT_EQ(packed[175 * 640 + 315 * 2], 126);
  EXPECT_EQ(packed[175 * 640 + 315 * 2 + 1], 128);
  EXPECT_EQ(packed[176 * 640 + 315 * 2], 0xcc);
  EXPECT_EQ(packed[175 * 640 + 316 * 2], 0xcc);

  std::vector<std::uint8_t> rgb(640 * 480 * 4, 0xcc);
  ASSERT_TRUE(overlay.draw_rgb(rgb.data(), rgb.size(), 640 * 4, 640, 480, 32, {0, 60, 640, 360}));
  std::uint32_t first = 0;
  std::uint32_t last = 0;
  std::memcpy(&first, rgb.data() + (68 * 640 + 8) * 4, 4);
  std::memcpy(&last, rgb.data() + (411 * 640 + 631) * 4, 4);
  EXPECT_EQ(first, 0xff808080U);
  EXPECT_EQ(last, first);
  EXPECT_EQ(rgb[(412 * 640 + 631) * 4], 0xcc);
  EXPECT_EQ(rgb[(411 * 640 + 632) * 4], 0xcc);
}

/**
 * @brief A docked keyboard occupies identical bottom-center bounds in GPU and software video.
 */
TEST(VideoOverlayTest, DocksKeyboardWithoutCoveringVideoAboveIt) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(16, 4, std::vector<std::uint8_t>(64, 255), streaming::VideoOverlayPlacement::bottom_center);
  const auto bounds = overlay.rectangle({4, 6, 40, 32});
  EXPECT_EQ(bounds.x, 16);
  EXPECT_EQ(bounds.y, 26);
  EXPECT_EQ(bounds.width, 16);
  EXPECT_EQ(bounds.height, 4);
  std::vector<std::uint8_t> rgb(48 * 48 * 4, 0xcc);
  ASSERT_TRUE(overlay.draw_rgb(rgb.data(), rgb.size(), 48 * 4, 48, 48, 32, {4, 6, 40, 32}));
  EXPECT_EQ(rgb[(26 * 48 + 16) * 4], 255);
  EXPECT_EQ(rgb[(29 * 48 + 31) * 4], 255);
  EXPECT_EQ(rgb[(25 * 48 + 16) * 4], 0xcc);
  EXPECT_EQ(rgb[(30 * 48 + 16) * 4], 0xcc);

  std::vector<std::uint8_t> yuy2(40 * 32 * 2, 0xcc);
  ASSERT_TRUE(overlay.draw_yuy2(yuy2.data(), yuy2.size(), 80, 40, 32, {4, 6, 40, 32}));
  EXPECT_EQ(yuy2[20 * 80 + 12 * 2], 235);
  EXPECT_EQ(yuy2[23 * 80 + 27 * 2], 235);
  EXPECT_EQ(yuy2[19 * 80 + 12 * 2], 0xcc);
  EXPECT_EQ(yuy2[24 * 80 + 12 * 2], 0xcc);

  // Changing the anchor must invalidate the cached GPU panel even at the same resolution.
  overlay.set_bitmap(16, 4, std::vector<std::uint8_t>(64, 255));
  std::fill(yuy2.begin(), yuy2.end(), 0xcc);
  ASSERT_TRUE(overlay.draw_yuy2(yuy2.data(), yuy2.size(), 80, 40, 32, {4, 6, 40, 32}));
  EXPECT_EQ(yuy2[8 * 80 + 8 * 2], 235);
  EXPECT_EQ(yuy2[20 * 80 + 12 * 2], 0xcc);
}

/**
 * @brief Small or empty presentation areas never produce negative keyboard panel copies.
 */
TEST(VideoOverlayTest, ClipsDockedPanelsAndSkipsTinyAreas) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(30, 20, std::vector<std::uint8_t>(600, 255), streaming::VideoOverlayPlacement::bottom_center);
  const auto bounds = overlay.rectangle({0, 0, 32, 24});
  EXPECT_EQ(bounds.x, 8);
  EXPECT_EQ(bounds.y, 8);
  EXPECT_EQ(bounds.width, 16);
  EXPECT_EQ(bounds.height, 8);
  EXPECT_EQ(overlay.rectangle({0, 0, 16, 16}).width, 0);
  std::array<std::uint8_t, 16 * 16 * 2> pixels {};
  EXPECT_TRUE(overlay.draw_yuy2(pixels.data(), pixels.size(), 32, 16, 16, {0, 0, 16, 16}));
  EXPECT_TRUE(overlay.draw_yuy2(pixels.data(), pixels.size(), 32, 16, 16, {0, 0, 16, 16}));
  EXPECT_TRUE(overlay.draw_rgb(pixels.data(), pixels.size(), 32, 16, 16, 16, {0, 0, 16, 16}));
  for (auto byte : pixels) {
    EXPECT_EQ(byte, 0);
  }
}

/**
 * @brief Software output uses the same grayscale panel in every Xbox framebuffer format.
 */
TEST(VideoOverlayTest, DrawsGrayscaleInAllFramebufferFormats) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(2, 1, {0, 255});
  for (const int bpp : {15, 16, 32}) {
    const int bytes = bpp == 32 ? 4 : 2;
    std::vector<std::uint8_t> output(20 * 20 * bytes, 0xcc);
    ASSERT_TRUE(overlay.draw_rgb(output.data(), output.size(), 20 * bytes, 20, 20, bpp, {0, 0, 20, 20}));
    std::uint32_t black = 0;
    std::uint32_t white = 0;
    std::memcpy(&black, output.data() + (8 * 20 + 8) * bytes, bytes);
    std::memcpy(&white, output.data() + (8 * 20 + 9) * bytes, bytes);
    EXPECT_EQ(black, bpp == 32 ? 0xff000000U : (bpp == 15 ? 0x8000U : 0U));
    EXPECT_EQ(white, bpp == 32 ? 0xffffffffU : 0xffffU);
    EXPECT_EQ(output[0], 0xcc);
  }
}

/**
 * @brief Offset panels preserve every pixel and padding byte outside the drawing rectangle.
 */
TEST(VideoOverlayTest, DrawsOffsetPanelWithPaddedFramebufferRows) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(3, 2, {0, 255, 128, 255, 128, 0});
  const std::array<int, 3> bitDepths {15, 16, 32};
  const std::array<std::array<std::uint32_t, 3>, 3> colors {{{0x8000U, 0xffffU, 0xc210U}, {0x0000U, 0xffffU, 0x8410U}, {0xff000000U, 0xffffffffU, 0xff808080U}}};
  for (std::size_t format = 0; format < bitDepths.size(); ++format) {
    const int bpp = bitDepths[format];
    const std::size_t bytes = bpp == 32 ? 4 : 2;
    const std::size_t pitch = 32 * bytes + 7;
    std::vector<std::uint8_t> output(pitch * 24, 0xcc);
    auto expected = output;
    const std::array<std::size_t, 6> offsets {12 * pitch + 11 * bytes, 12 * pitch + 12 * bytes, 12 * pitch + 13 * bytes, 13 * pitch + 11 * bytes, 13 * pitch + 12 * bytes, 13 * pitch + 13 * bytes};
    const std::array<std::size_t, 6> samples {0, 1, 2, 1, 2, 0};
    for (std::size_t pixel = 0; pixel < offsets.size(); ++pixel) {
      std::memcpy(expected.data() + offsets[pixel], &colors[format][samples[pixel]], bytes);
    }
    ASSERT_TRUE(overlay.draw_rgb(output.data(), output.size(), static_cast<int>(pitch), 32, 24, bpp, {3, 4, 15, 14}));
    EXPECT_EQ(output, expected);
  }
}

/**
 * @brief Scaling is cached in source coordinates and refreshed when text or geometry changes.
 */
TEST(VideoOverlayTest, RefreshesScaledCacheAndHandlesTinyFrames) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(2, 1, {0, 255});
  std::vector<std::uint8_t> output(64 * 64 * 2, 0xcc);
  ASSERT_TRUE(overlay.draw_yuy2(output.data(), output.size(), 128, 64, 64, {0, 0, 32, 32}));
  EXPECT_EQ(output[16 * 128 + 16 * 2], 16);
  EXPECT_EQ(output[16 * 128 + 18 * 2], 235);
  overlay.set_bitmap(2, 1, {255, 0});
  ASSERT_TRUE(overlay.draw_yuy2(output.data(), output.size(), 128, 64, 64, {0, 0, 32, 32}));
  EXPECT_EQ(output[16 * 128 + 16 * 2], 235);
  EXPECT_EQ(output[16 * 128 + 18 * 2], 16);
  std::array<std::uint8_t, 8> tiny {};
  EXPECT_TRUE(overlay.draw_yuy2(tiny.data(), tiny.size(), 4, 2, 2, {0, 0, 9, 9}));
  EXPECT_TRUE(overlay.draw_yuy2(tiny.data(), tiny.size(), 4, 2, 2, {0, 0, 9, 9}));
}

/**
 * @brief Invalid storage never changes the destination and malformed bitmaps clear the panel.
 */
TEST(VideoOverlayTest, RejectsTruncatedStorageAndClearsInvalidBitmaps) {
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(2, 2, {0, 255, 0, 255});
  std::array<std::uint8_t, 512> output;
  output.fill(0xcc);
  EXPECT_FALSE(overlay.draw_yuy2(output.data(), output.size() - 1, 32, 16, 16, {0, 0, 16, 16}));
  EXPECT_FALSE(overlay.draw_rgb(output.data(), output.size(), 32, 16, 16, 24, {0, 0, 16, 16}));
  EXPECT_FALSE(overlay.draw_rgb(output.data(), output.size(), 32, 16, 16, 16, {-1, 0, 16, 16}));
  for (auto byte : output) {
    EXPECT_EQ(byte, 0xcc);
  }
  overlay.set_bitmap(2, 2, {1});
  EXPECT_EQ(overlay.width(), 0);
  EXPECT_TRUE(overlay.draw_rgb(nullptr, 0, 0, 0, 0, 0, {}));
}

/**
 * @brief Edge-docked keyboards and full-screen dimming use identical flush bounds for GPU and software paths.
 */
TEST(VideoOverlayTest, DocksToEdgesWithoutTheLegacyInset) {
  streaming::VideoOverlay overlay;
  const streaming::VideoRectangle target {10, 20, 100, 80};
  for (const auto placement : {streaming::VideoOverlayPlacement::dock_top, streaming::VideoOverlayPlacement::dock_bottom}) {
    overlay.set_argb_bitmap(40, 20, std::vector<std::uint8_t>(40 * 20 * 4, 255), placement);
    const auto bounds = overlay.rectangle(target);
    EXPECT_EQ(bounds.x, 40);
    EXPECT_EQ(bounds.y, placement == streaming::VideoOverlayPlacement::dock_top ? 20 : 80);
    EXPECT_EQ(bounds.width, 40);
    EXPECT_EQ(bounds.height, 20);
  }
  overlay.set_argb_bitmap(200, 200, std::vector<std::uint8_t>(200 * 200 * 4, 150), streaming::VideoOverlayPlacement::full_screen);
  const auto bounds = overlay.rectangle(target);
  EXPECT_EQ(bounds.x, 10);
  EXPECT_EQ(bounds.y, 20);
  EXPECT_EQ(bounds.width, 100);
  EXPECT_EQ(bounds.height, 80);
}

/**
 * @brief A translucent black backdrop dims live RGB and YUY2 frames instead of replacing the stream.
 */
TEST(VideoOverlayTest, DimsTheStreamThroughBothPresentationPaths) {
  streaming::VideoOverlay overlay;
  std::vector<std::uint8_t> backdrop(16 * 16 * 4, 0);
  std::vector<std::uint8_t> rgb(backdrop.size());
  auto expected = rgb;
  for (std::size_t i = 0; i < backdrop.size(); i += 4) {
    backdrop[i + 3] = 128;
    const std::array<std::uint8_t, 4> source {80, 100, 120, 255};
    const std::array<std::uint8_t, 4> dimmed {40, 50, 60, 255};
    std::copy(source.begin(), source.end(), rgb.begin() + i);
    std::copy(dimmed.begin(), dimmed.end(), expected.begin() + i);
  }
  overlay.set_argb_bitmap(16, 16, std::move(backdrop), streaming::VideoOverlayPlacement::full_screen);
  ASSERT_TRUE(overlay.draw_rgb(rgb.data(), rgb.size(), 64, 16, 16, 32, {0, 0, 16, 16}));
  EXPECT_EQ(rgb, expected);
  for (const auto matrix : {streaming::VideoColorMatrix::bt601, streaming::VideoColorMatrix::bt709}) {
    std::vector<std::uint8_t> yuy2(16 * 16 * 2);
    expected.resize(yuy2.size());
    for (std::size_t i = 0; i < yuy2.size(); i += 4) {
      const std::array<std::uint8_t, 4> source {100, 90, 150, 170};
      const std::array<std::uint8_t, 4> dimmed {58, 109, 83, 149};
      std::copy(source.begin(), source.end(), yuy2.begin() + i);
      std::copy(dimmed.begin(), dimmed.end(), expected.begin() + i);
    }
    ASSERT_TRUE(overlay.draw_yuy2(yuy2.data(), yuy2.size(), 32, 16, 16, {0, 0, 16, 16}, matrix));
    EXPECT_EQ(yuy2, expected);
  }
}
