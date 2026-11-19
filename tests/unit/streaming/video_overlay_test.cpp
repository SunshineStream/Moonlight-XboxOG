/**
 * @file tests/unit/streaming/video_overlay_test.cpp
 * @brief Verifies equivalent GPU/software panel pixels and destination bounds.
 */
// test header include
#include "src/streaming/video_overlay.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

// lib includes
#include <lizardbyte/common/testing.h>

/**
 * @brief Full-screen dialog repaints reuse their color allocation while screenshots own additional buffers.
 */
TEST(VideoOverlayTest, ReusesFullScreenSurfaceStorageAcrossDialogRepaints) {
  constexpr int width = 1280;
  constexpr int height = 720;
  std::vector<std::uint8_t> source(width * height * 4, 150);
  streaming::VideoOverlay overlay;
  ASSERT_TRUE(overlay.set_argb_surface(width, height, source.data(), source.size(), width * 4, streaming::VideoOverlayPlacement::full_screen));
  const auto *allocation = overlay.argb_pixels().data();
  source[0] = 15;
  source[1] = 30;
  source[2] = 60;
  source[3] = 0;
  ASSERT_TRUE(overlay.set_argb_surface(width, height, source.data(), source.size(), width * 4, streaming::VideoOverlayPlacement::full_screen));
  EXPECT_EQ(overlay.argb_pixels().data(), allocation);
  EXPECT_EQ(overlay.argb_pixels(), streaming::extract_overlay_bitmap(source.data(), source.size(), width * 4, width, height));
  EXPECT_EQ(overlay.argb_pixels()[0], 0);
  ASSERT_TRUE(overlay.set_argb_surface(8, 8, source.data(), source.size(), width * 4, streaming::VideoOverlayPlacement::bottom_center, true));
  EXPECT_LT(overlay.argb_pixels().capacity(), static_cast<std::size_t>(width * height));
  EXPECT_EQ(overlay.argb_pixels()[3], 255);
  const auto expected = overlay.argb_pixels();
  EXPECT_FALSE(overlay.set_argb_surface(8, 8, nullptr, source.size(), width * 4, streaming::VideoOverlayPlacement::top_left));
  EXPECT_FALSE(overlay.set_argb_surface(2049, 1, source.data(), source.size(), 2049 * 4, streaming::VideoOverlayPlacement::top_left));
  EXPECT_FALSE(overlay.set_argb_surface(1, 2049, source.data(), source.size(), 4, streaming::VideoOverlayPlacement::top_left));
  EXPECT_FALSE(overlay.set_argb_surface(8, 8, source.data(), 1, width * 4, streaming::VideoOverlayPlacement::top_left));
  EXPECT_EQ(overlay.argb_pixels(), expected);
  EXPECT_EQ(overlay.width(), 8);
}

/**
 * @brief Reused SDL surfaces refresh GPU and software caches when their colors or opacity change.
 */
TEST(VideoOverlayTest, RefreshesPresentationCachesAfterReusingSurfaceStorage) {
  streaming::VideoOverlay overlay;
  for (const std::uint8_t opacity : {255, 128, 0, 255}) {
    std::vector<std::uint8_t> source(8 * 8 * 4);
    for (std::size_t index = 0; index < source.size(); index += 4) {
      source[index] = 20;
      source[index + 1] = 40;
      source[index + 2] = 60;
      source[index + 3] = opacity;
    }
    ASSERT_TRUE(overlay.set_argb_surface(8, 8, source.data(), source.size(), 32, streaming::VideoOverlayPlacement::top_left));
    streaming::VideoOverlay fresh;
    fresh.set_argb_bitmap(8, 8, streaming::extract_overlay_bitmap(source.data(), source.size(), 32, 8, 8));
    for (const auto matrix : {streaming::VideoColorMatrix::bt601, streaming::VideoColorMatrix::bt709}) {
      std::vector<std::uint8_t> actual(32 * 32 * 2, 100);
      auto expected = actual;
      ASSERT_TRUE(overlay.draw_yuy2(actual.data(), actual.size(), 64, 32, 32, {0, 0, 32, 32}, matrix));
      ASSERT_TRUE(fresh.draw_yuy2(expected.data(), expected.size(), 64, 32, 32, {0, 0, 32, 32}, matrix));
      EXPECT_EQ(actual, expected);
    }
    for (const int bpp : {15, 16, 32}) {
      const int bytes = bpp == 32 ? 4 : 2;
      std::vector<std::uint8_t> actual(32 * 32 * bytes, 100);
      auto expected = actual;
      ASSERT_TRUE(overlay.draw_rgb(actual.data(), actual.size(), 32 * bytes, 32, 32, bpp, {0, 0, 32, 32}));
      ASSERT_TRUE(fresh.draw_rgb(expected.data(), expected.size(), 32 * bytes, 32, 32, bpp, {0, 0, 32, 32}));
      EXPECT_EQ(actual, expected);
    }
  }
}

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
  frame[at] = 70;
  frame[at + 2] = 70;
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
      frame[at] = 50;
      frame[at + 2] = 50;
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
    ASSERT_TRUE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), pitch, 20, 20}, {10, 10, 6, selected}));
    for (int sector = 0; sector < 4; ++sector) {
      EXPECT_EQ(pixels[y[sector] * pitch + x[sector] * 4], sector == selected ? 85 : 36);
      EXPECT_EQ(pixels[y[sector] * pitch + x[sector] * 4 + 3], 255);
    }
    EXPECT_EQ(pixels[6 * pitch + 14 * 4], 110);
    EXPECT_EQ(pixels[10 * pitch + 10 * 4], 0xcc);
    EXPECT_EQ(pixels[0], 0xcc);
    EXPECT_EQ(pixels[10 * pitch + 20 * 4], 0xcc);
  }
  ASSERT_TRUE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), pitch, 20, 20}, {0, 0, 4, -1}));
  EXPECT_EQ(pixels[3 * 4], 36);
}

/**
 * @brief Invalid wheel storage and geometry are rejected before a pixel is written.
 */
TEST(VideoOverlayTest, RejectsInvalidWheelRasterStorage) {
  std::array<std::uint8_t, 20 * 20 * 4> pixels;
  pixels.fill(0xcc);
  EXPECT_FALSE(streaming::rasterize_menu_wheel({nullptr, pixels.size(), 80, 20, 20}, {10, 10, 6, 0}));
  EXPECT_FALSE(streaming::rasterize_menu_wheel({pixels.data(), 1, 80, 20, 20}, {10, 10, 6, 0}));
  EXPECT_FALSE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 80, 20, 20}, {-1, 10, 6, 0}));
  EXPECT_FALSE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 80, 20, 20}, {20, 10, 6, 0}));
  EXPECT_FALSE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 80, 20, 20}, {10, -1, 6, 0}));
  EXPECT_FALSE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 80, 20, 20}, {10, 20, 6, 0}));
  EXPECT_FALSE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 80, 20, 20}, {10, 10, 0, 0}));
  EXPECT_FALSE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 80, 20, 20}, {10, 10, 2049, 0}));
  std::array<std::uint8_t, 2049 * 4> oversized {};
  EXPECT_FALSE(streaming::rasterize_menu_wheel({oversized.data(), oversized.size(), 2049 * 4, 2049, 1}, {0, 0, 1, 0}));
  EXPECT_FALSE(streaming::rasterize_menu_wheel({oversized.data(), oversized.size(), 4, 1, 2049}, {0, 0, 1, 0}));
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

/** @brief Shared in-place focus updates use the same colors and full panel bounds as a copied raster. */
TEST(VideoOverlayTest, SharesTheMenuRasterAcrossFullAndPartialRepaints) {
  constexpr int width = 16;
  constexpr int height = 12;
  const streaming::VideoRectangle destination {0, 0, width, height};
  const auto placement = streaming::VideoOverlayPlacement::full_screen;
  auto raster = std::make_shared<std::vector<std::uint8_t>>(width * height * 4, 0);
  std::weak_ptr<std::vector<std::uint8_t>> retained = raster;
  for (std::size_t index = 3; index < raster->size(); index += 4) {
    (*raster)[index] = 150;
  }
  streaming::VideoOverlay overlay;
  EXPECT_FALSE(overlay.update_shared_argb_surface({0, 0, 1, 1}));
  ASSERT_TRUE(overlay.share_argb_surface(width, height, {raster, raster->data()}, raster->size(), placement, false));
  EXPECT_FALSE(overlay.share_argb_surface(width, height, {raster, raster->data()}, raster->size() - 1, placement, false));
  EXPECT_FALSE(overlay.update_shared_argb_surface({width, 0, 1, 1}));
  for (bool repaint : {false, true}) {
    if (repaint) {
      const std::size_t offset = (3 * width + 4) * 4;
      (*raster)[offset] = 64;
      (*raster)[offset + 1] = 5;
      (*raster)[offset + 2] = 32;
      (*raster)[offset + 3] = 128;
      ASSERT_TRUE(overlay.update_shared_argb_surface({4, 3, 1, 1}));
    }
    streaming::VideoOverlay copied;
    ASSERT_TRUE(copied.set_argb_surface(width, height, raster->data(), raster->size(), width * 4, placement));
    EXPECT_EQ(overlay.argb_pixels(), copied.argb_pixels());
    std::array<std::uint8_t, 16> texture {};
    texture.fill(0xaa);
    EXPECT_FALSE(overlay.copy_argb_region({4, 3, 1, 2}, texture.data(), 3));
    ASSERT_TRUE(overlay.copy_argb_region({4, 3, 1, 2}, texture.data(), 8));
    const auto &expected = copied.argb_pixels();
    for (int row = 0; row < 2; ++row) {
      const auto *pixel = expected.data() + ((3 + row) * width + 4) * 4;
      EXPECT_TRUE(std::equal(pixel, pixel + 4, texture.data() + row * 8));
      EXPECT_EQ(texture[row * 8 + 4], 0xaa);
    }

    std::vector<std::uint8_t> output(width * height * 2, 150);
    auto reference = output;
    ASSERT_TRUE(overlay.draw_yuy2(output.data(), output.size(), width * 2, width, height, destination));
    ASSERT_TRUE(copied.draw_yuy2(reference.data(), reference.size(), width * 2, width, height, destination));
    EXPECT_EQ(output, reference);

    output.assign(output.size(), 150);
    reference = output;
    ASSERT_TRUE(overlay.draw_rgb(output.data(), output.size(), width * 2, width, height, 16, destination));
    ASSERT_TRUE(copied.draw_rgb(reference.data(), reference.size(), width * 2, width, height, 16, destination));
    EXPECT_EQ(output, reference);
  }

  constexpr int scaledWidth = width * 2;
  streaming::VideoOverlay copied;
  ASSERT_TRUE(copied.set_argb_surface(width, height, raster->data(), raster->size(), width * 4, placement));
  std::vector<std::uint8_t> scaled(scaledWidth * height * 2, 150);
  auto reference = scaled;
  ASSERT_TRUE(overlay.draw_yuy2(scaled.data(), scaled.size(), scaledWidth * 2, scaledWidth, height, destination));
  ASSERT_TRUE(copied.draw_yuy2(reference.data(), reference.size(), scaledWidth * 2, scaledWidth, height, destination));
  EXPECT_EQ(scaled, reference);

  raster.reset();
  EXPECT_FALSE(retained.expired());
  EXPECT_EQ(overlay.rectangle(destination).width, width);
  EXPECT_EQ(overlay.rectangle(destination).height, height);
  overlay.set_bitmap(0, 0, {});
  EXPECT_TRUE(retained.expired());
}

/** @brief The 1080i wheel and keyboard raster scales up without losing color, alpha or focus updates. */
TEST(VideoOverlayTest, DoublesSharedRasterForGpuAndSoftwareScanout) {
  constexpr int width = 16;
  constexpr int height = 12;
  constexpr int outputWidth = width * 2;
  constexpr int outputHeight = 48;
  const streaming::VideoRectangle destination {0, 0, outputWidth, outputHeight};
  EXPECT_EQ(streaming::controls_raster_scale(720), 1);
  ASSERT_EQ(streaming::controls_raster_scale(1080), 2);

  for (const auto placement : {streaming::VideoOverlayPlacement::full_screen, streaming::VideoOverlayPlacement::dock_top, streaming::VideoOverlayPlacement::dock_bottom}) {
    const bool opaque = placement != streaming::VideoOverlayPlacement::full_screen;
    auto raster = std::make_shared<std::vector<std::uint8_t>>(width * height * 4, 0);
    for (int i = 0; i < width * height; ++i) {
      const int opacity = opaque ? 255 : i % 3 * 100;
      (*raster)[i * 4] = (i % 256) * opacity / 255;
      (*raster)[i * 4 + 1] = 70 * opacity / 255;
      (*raster)[i * 4 + 2] = 150 * opacity / 255;
      (*raster)[i * 4 + 3] = opacity;
    }

    streaming::VideoOverlay compact;
    ASSERT_FALSE(compact.share_argb_surface(width, height, {raster, raster->data()}, raster->size(), placement, opaque, 0));
    ASSERT_TRUE(compact.share_argb_surface(width, height, {raster, raster->data()}, raster->size(), placement, opaque, 2));
    const auto bounds = compact.rectangle(destination);
    EXPECT_EQ(bounds.y, placement == streaming::VideoOverlayPlacement::dock_bottom ? outputHeight - height * 2 : 0);
    EXPECT_EQ(bounds.width, outputWidth);
    EXPECT_EQ(bounds.height, height * 2);
    const auto bitmap = compact.bitmap_rectangle(bounds);
    EXPECT_EQ(bitmap.width, width);
    EXPECT_EQ(bitmap.height, height);

    for (bool repaint : {false, true}) {
      if (repaint) {
        const int offset = (width * 7 + 5) * 4;
        (*raster)[offset] = 255;
        (*raster)[offset + 3] = 255;
        ASSERT_TRUE(compact.update_shared_argb_surface({5, 7, 1, 1}));
      }
      std::vector<std::uint8_t> expanded(outputWidth * height * 2 * 4);
      for (int y = 0; y < height * 2; ++y) {
        for (int x = 0; x < outputWidth; ++x) {
          std::memcpy(expanded.data() + (y * outputWidth + x) * 4, raster->data() + (y / 2 * width + x / 2) * 4, 4);
        }
      }
      streaming::VideoOverlay reference;
      ASSERT_TRUE(reference.set_argb_surface(outputWidth, height * 2, expanded.data(), expanded.size(), outputWidth * 4, placement, opaque));
      std::vector<std::uint8_t> actual(outputWidth * outputHeight * 2, 120);
      auto expected = actual;
      ASSERT_TRUE(compact.draw_yuy2(actual.data(), actual.size(), outputWidth * 2, outputWidth, outputHeight, destination));
      ASSERT_TRUE(reference.draw_yuy2(expected.data(), expected.size(), outputWidth * 2, outputWidth, outputHeight, destination));
      EXPECT_EQ(actual, expected);

      actual.assign(actual.size(), 120);
      expected = actual;
      ASSERT_TRUE(compact.draw_rgb(actual.data(), actual.size(), outputWidth * 2, outputWidth, outputHeight, 16, destination));
      ASSERT_TRUE(reference.draw_rgb(expected.data(), expected.size(), outputWidth * 2, outputWidth, outputHeight, 16, destination));
      EXPECT_EQ(actual, expected);
    }
  }
}

/** @brief Alpha rounding on the SDL keyboard surface keeps the docked panel opaque. */
TEST(VideoOverlayTest, SharesSolidKeyboardColorsAndDocking) {
  constexpr int width = 12;
  constexpr int height = 12;
  auto raster = std::make_shared<std::vector<std::uint8_t>>(width * height * 4, 0xfe);
  streaming::VideoOverlay overlay;
  ASSERT_TRUE(overlay.share_argb_surface(width, height, {raster, raster->data()}, raster->size(), streaming::VideoOverlayPlacement::dock_top, true));
  const auto bounds = overlay.rectangle({0, 0, 32, 24});
  EXPECT_EQ(bounds.x, 10);
  EXPECT_EQ(bounds.y, 0);
  EXPECT_EQ(bounds.width, width);
  EXPECT_EQ(bounds.height, height);
  std::array<std::uint8_t, 4> texture {};
  ASSERT_TRUE(overlay.copy_argb_region({0, 0, 1, 1}, texture.data(), 4));
  const std::array<std::uint8_t, 4> expected {254, 254, 254, 255};
  EXPECT_EQ(texture, expected);
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

namespace {
  /**
   * @brief Compare incremental and complete overlay conversion for one output scale and color matrix.
   * @param scale Output size relative to the source panel.
   * @param matrix Host color matrix.
   */
  void compare_recolored_overlay(int scale, streaming::VideoColorMatrix matrix) {
    streaming::VideoOverlay incremental;
    std::vector<std::uint8_t> pixels(16 * 12 * 4, 0);
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
      pixels[i] = 150;
    }
    const streaming::VideoRectangle destination {0, 0, scale, scale * 3 / 4};
    for (int step = 0; step < 8; ++step) {
      const auto index = static_cast<std::size_t>((step * 7) % (16 * 12)) * 4;
      pixels[index] = 200;
      pixels[index + 1] = 40;
      pixels[index + 2] = 70;
      pixels[index + 3] = step % 2 ? 255 : 0;
      incremental.set_argb_bitmap(16, 12, pixels, streaming::VideoOverlayPlacement::full_screen);
      if (step % 2 == 0) {
        const auto other = static_cast<std::size_t>(16 * 12 - step - 1) * 4;
        pixels[other + 1] = 240;
        incremental.set_argb_bitmap(16, 12, pixels, streaming::VideoOverlayPlacement::full_screen);
      }
      // An identical update must preserve pending damage until presentation.
      incremental.set_argb_bitmap(16, 12, pixels, streaming::VideoOverlayPlacement::full_screen);
      streaming::VideoOverlay fresh;
      fresh.set_argb_bitmap(16, 12, pixels, streaming::VideoOverlayPlacement::full_screen);
      std::vector<std::uint8_t> actual(64 * 48 * 2, 100);
      auto expected = actual;
      ASSERT_TRUE(incremental.draw_yuy2(actual.data(), actual.size(), 128, 64, 48, destination, matrix));
      ASSERT_TRUE(fresh.draw_yuy2(expected.data(), expected.size(), 128, 64, 48, destination, matrix));
      EXPECT_EQ(actual, expected) << "scale=" << scale << " step=" << step;
      // A new host frame must still be composed with the cached UI.
      actual.assign(actual.size(), 180);
      expected = actual;
      ASSERT_TRUE(incremental.draw_yuy2(actual.data(), actual.size(), 128, 64, 48, destination, matrix));
      ASSERT_TRUE(fresh.draw_yuy2(expected.data(), expected.size(), 128, 64, 48, destination, matrix));
      EXPECT_EQ(actual, expected);
    }
  }
}  // namespace

/**
 * @brief Cached UI damage produces the same colors, alpha and chroma as a fresh overlay at both scales.
 */
TEST(VideoOverlayTest, RecolorsChangedRegionsWithoutStaleGpuPixels) {
  for (const int scale : {32, 128}) {
    for (const auto matrix : {streaming::VideoColorMatrix::bt601, streaming::VideoColorMatrix::bt709}) {
      ASSERT_NO_FATAL_FAILURE(compare_recolored_overlay(scale, matrix));
    }
  }
}

/**
 * @brief RGB output and GPU conversion survive opacity transitions, clipped damage and a matrix change.
 */
TEST(VideoOverlayTest, RebuildsCachesWhenOpacityOrMatrixChanges) {
  streaming::VideoOverlay overlay;
  std::vector<std::uint8_t> pixels(20 * 20 * 4, 255);
  const auto compare = [&](streaming::VideoColorMatrix matrix) {
    streaming::VideoOverlay fresh;
    fresh.set_argb_bitmap(20, 20, pixels, streaming::VideoOverlayPlacement::full_screen);
    for (int bpp : {15, 16, 32}) {
      const int bytes = bpp == 32 ? 4 : 2;
      std::vector<std::uint8_t> actual(16 * 16 * bytes, 90);
      auto expected = actual;
      ASSERT_TRUE(overlay.draw_rgb(actual.data(), actual.size(), 16 * bytes, 16, 16, bpp, {0, 0, 16, 16}));
      ASSERT_TRUE(fresh.draw_rgb(expected.data(), expected.size(), 16 * bytes, 16, 16, bpp, {0, 0, 16, 16}));
      EXPECT_EQ(actual, expected);
    }
    std::vector<std::uint8_t> actual(16 * 16 * 2, 90);
    auto expected = actual;
    ASSERT_TRUE(overlay.draw_yuy2(actual.data(), actual.size(), 32, 16, 16, {0, 0, 16, 16}, matrix));
    ASSERT_TRUE(fresh.draw_yuy2(expected.data(), expected.size(), 32, 16, 16, {0, 0, 16, 16}, matrix));
    EXPECT_EQ(actual, expected);
  };
  overlay.set_argb_bitmap(20, 20, pixels, streaming::VideoOverlayPlacement::full_screen);
  compare(streaming::VideoColorMatrix::bt601);
  pixels[3] = 80;
  overlay.set_argb_bitmap(20, 20, pixels, streaming::VideoOverlayPlacement::full_screen);
  compare(streaming::VideoColorMatrix::bt601);
  pixels[(19 * 20 + 19) * 4] = 40;
  overlay.set_argb_bitmap(20, 20, pixels, streaming::VideoOverlayPlacement::full_screen);
  compare(streaming::VideoColorMatrix::bt601);
  compare(streaming::VideoColorMatrix::bt709);
  pixels[3] = 255;
  overlay.set_argb_bitmap(20, 20, pixels, streaming::VideoOverlayPlacement::full_screen);
  compare(streaming::VideoColorMatrix::bt709);
}

/**
 * @brief Partial ARGB updates match full repaints in RGB and YUY2, including alpha changes and accumulated damage.
 */
TEST(VideoOverlayTest, AppliesSmallPatchesAcrossBothPresentationPaths) {
  for (const int scale : {12, 32, 80}) {
    streaming::VideoOverlay overlay;
    std::vector<std::uint8_t> pixels(20 * 16 * 4, 255);
    overlay.set_argb_bitmap(20, 16, pixels, streaming::VideoOverlayPlacement::full_screen);
    for (int step = 0; step < 6; ++step) {
      const streaming::VideoRectangle area {step * 3, step * 2, 3, 2};
      std::vector<std::uint8_t> patch(static_cast<std::size_t>(area.width) * area.height * 4, 0);
      for (std::size_t i = 0; i < patch.size(); i += 4) {
        patch[i] = step * 30;
        patch[i + 1] = 40;
        patch[i + 2] = 210;
        patch[i + 3] = std::array<std::uint8_t, 3> {255, 150, 0}.at(step % 3);
      }
      for (int row = 0; row < area.height; ++row) {
        std::copy_n(patch.data() + row * area.width * 4, area.width * 4, pixels.data() + ((area.y + row) * 20 + area.x) * 4);
      }
      ASSERT_TRUE(overlay.update_argb_bitmap(area, patch));
      EXPECT_EQ(overlay.argb_pixels(), pixels);
      // Accumulate two updates before presentation as a focus change does.
      if (step % 2 == 0) {
        continue;
      }
      streaming::VideoOverlay fresh;
      fresh.set_argb_bitmap(20, 16, pixels, streaming::VideoOverlayPlacement::full_screen);
      const streaming::VideoRectangle destination {0, 0, scale, scale};
      const streaming::VideoRectangle rgbDestination {0, 0, std::min(32, scale), std::min(32, scale)};
      for (int bpp : {15, 16, 32}) {
        const int bytes = bpp == 32 ? 4 : 2;
        std::vector<std::uint8_t> actual(32 * 32 * bytes, 100);
        auto expected = actual;
        ASSERT_TRUE(overlay.draw_rgb(actual.data(), actual.size(), 32 * bytes, 32, 32, bpp, rgbDestination));
        ASSERT_TRUE(fresh.draw_rgb(expected.data(), expected.size(), 32 * bytes, 32, 32, bpp, rgbDestination));
        EXPECT_EQ(actual, expected);
      }
      for (const auto matrix : {streaming::VideoColorMatrix::bt601, streaming::VideoColorMatrix::bt709}) {
        std::vector<std::uint8_t> actual(32 * 32 * 2, 100);
        auto expected = actual;
        ASSERT_TRUE(overlay.draw_yuy2(actual.data(), actual.size(), 64, 32, 32, destination, matrix));
        ASSERT_TRUE(fresh.draw_yuy2(expected.data(), expected.size(), 64, 32, 32, destination, matrix));
        EXPECT_EQ(actual, expected) << "step=" << step << " scale=" << scale;
      }
    }
  }
}

/**
 * @brief Invalid patch rectangles, storage and non-ARGB panels are rejected without changing any pixels.
 */
TEST(VideoOverlayTest, RejectsInvalidColorPatchesWithoutChangingThePanel) {
  streaming::VideoOverlay overlay;
  EXPECT_FALSE(overlay.update_argb_bitmap({0, 0, 1, 1}, {1, 2, 3, 255}));
  overlay.set_bitmap(2, 2, {1, 2, 3, 4});
  EXPECT_FALSE(overlay.update_argb_bitmap({0, 0, 1, 1}, {1, 2, 3, 255}));
  const std::vector<std::uint8_t> original(4 * 4 * 4, 255);
  overlay.set_argb_bitmap(4, 4, original);
  for (const auto area : {streaming::VideoRectangle {-1, 0, 1, 1}, {0, -1, 1, 1}, {0, 0, 0, 1}, {0, 0, 1, 0}, {0, 0, 5, 1}, {0, 0, 1, 5}, {4, 0, 1, 1}, {0, 4, 1, 1}, {1, 1, 4, 4}}) {
    EXPECT_FALSE(overlay.update_argb_bitmap(area, {1, 2, 3, 255}));
    EXPECT_EQ(overlay.argb_pixels(), original);
  }
  EXPECT_FALSE(overlay.update_argb_bitmap({0, 0, 1, 1}, {1, 2, 3}));
  EXPECT_EQ(overlay.argb_pixels(), original);
}

/**
 * @brief Repainting only the old and new wheel sectors is identical to rasterizing the complete ring.
 */
TEST(VideoOverlayTest, RepaintsWheelSectorsAndClippedLabelBackgrounds) {
  std::vector<std::uint8_t> pixels(40 * 40 * 4, 150);
  const auto backdrop = pixels;
  int previous = -1;
  for (int selected : {0, 1, 2, 3, -1, 2, 0}) {
    if (previous >= 0) {
      ASSERT_TRUE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 160, 40, 40}, {20, 20, 18, selected}, previous));
    }
    if (selected >= 0) {
      ASSERT_TRUE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 160, 40, 40}, {20, 20, 18, selected}, selected));
    }
    auto expected = backdrop;
    ASSERT_TRUE(streaming::rasterize_menu_wheel({expected.data(), expected.size(), 160, 40, 40}, {20, 20, 18, selected}));
    // Initialize the unchanged sectors on the first frame.
    if (previous == -1 && selected == 0) {
      pixels = expected;
    }
    EXPECT_EQ(pixels, expected);
    const streaming::VideoRectangle clip {17, 2, 6, 6};
    auto clipped = backdrop;
    ASSERT_TRUE(streaming::rasterize_menu_wheel({clipped.data(), clipped.size(), 160, 40, 40}, {20, 20, 18, selected}, -1, &clip));
    for (int y = 0; y < 40; ++y) {
      for (int x = 0; x < 40; ++x) {
        const int index = (y * 40 + x) * 4;
        EXPECT_EQ(clipped[index], x >= clip.x && x < clip.x + clip.width && y >= clip.y && y < clip.y + clip.height ? expected[index] : backdrop[index]);
      }
    }
    previous = selected;
  }
  EXPECT_FALSE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 160, 40, 40}, {20, 20, 18, 0}, -2));
  EXPECT_FALSE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 160, 40, 40}, {20, 20, 18, 0}, 4));
  for (const auto clip : {streaming::VideoRectangle {-1, 0, 1, 1}, {0, -1, 1, 1}, {0, 0, 0, 1}, {0, 0, 1, 0}, {0, 0, 41, 1}, {0, 0, 1, 41}, {40, 0, 1, 1}, {0, 40, 1, 1}}) {
    EXPECT_FALSE(streaming::rasterize_menu_wheel({pixels.data(), pixels.size(), 160, 40, 40}, {20, 20, 18, 0}, -1, &clip));
  }
}
