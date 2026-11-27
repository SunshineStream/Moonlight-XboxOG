/**
 * @file tests/gpu/pvideo_presenter_test.cpp
 * @brief Exercises the production presenter against deterministic NV2A, pbkit and allocator shims.
 */
// test header include
#include "src/streaming/pvideo_presenter.h"

// standard includes
#include <algorithm>
#include <array>
#include <map>
#include <utility>
#include <vector>

// lib includes
#include <hal/video.h>
#include <lizardbyte/common/testing.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

// local includes
#include "src/streaming/software_scanout.h"

namespace {
  constexpr std::uint32_t CRTC_START = 0x600800;  ///< Physical scanout base.
  constexpr std::uint32_t CRTC_INTERRUPT = 0x600100;  ///< Vertical blank status.
  constexpr std::uint32_t CPU_FRAMEBUFFER = 0x40000;  ///< Fake physical address of the nxdk page.
  constexpr std::uint32_t VIDEO_BUFFER = 0x8700;  ///< Overlay buffer submission register.
  constexpr std::uint32_t VIDEO_ADDRESS = 0x8920;  ///< Bank zero overlay DMA offset.

  /**
   * @brief Track the surface latched by the fake software display at vblank.
   */
  struct SoftwareDisplay {
    bool pollVblank = false;  ///< Simulate software polling of the vblank status.
    bool signalVblank = true;  ///< Signal vblank when simulated time advances.
    std::uint32_t flushedScanout = 0;  ///< Scanout base when framebuffer writes were flushed.
    std::uint32_t displayedScanout = CPU_FRAMEBUFFER;  ///< Page latched by the display at the last vblank.
    std::uint8_t primaryPixelAtVblank = 0;  ///< First byte of the primary framebuffer when retrace starts.
  };

  /**
   * @brief Track the fake GPU's vblank counter and when it resumes after a simulated stall.
   */
  struct GpuVblank {
    DWORD count = 0;  ///< Number of simulated GPU blanks.
    DWORD resumeTick = 0;  ///< First tick at which the GPU may signal vblank.
    std::uint32_t background = 0;  ///< Scanout address when the overlay request reached a blank.
    std::uint32_t overlay = 0;  ///< Overlay submission at the last blank.
    std::uint32_t overlayAddress = 0;  ///< Overlay address at the last blank.
    std::size_t packedBuffers = 0;  ///< Buffers populated with this fixture's nonzero luma before the blank.
  };

  /** @brief Track depth detachment and the time until its GPU commands finish. */
  struct GpuDepthSurface {
    unsigned int detachments = 0;  ///< Number of times depth detachment was queued.
    unsigned int releases = 0;  ///< Number of times a detached surface was released.
    std::size_t overlayBuffersAtDetach = 0;  ///< Number of overlay buffers allocated before depth detachment.
    DWORD detachDuration = 0;  ///< Simulated duration of the queued commands.
    DWORD idleTick = 0;  ///< Earliest tick when the GPU reports those commands complete.
  };

  /** @brief Track ownership and the fake physical address of a DMA buffer. */
  struct DmaAllocation {
    std::vector<std::uint8_t> storage;  ///< The allocated byte storage.
    std::uint32_t physicalAddress;  ///< Simulated bus address.
  };

  /** @brief Store indexed VGA registers and their active selector together. */
  struct IndexedVgaRegisters {
    std::array<std::uint8_t, 256> values {};  ///< Indexed VGA register values.
    std::uint8_t index = 0;  ///< Active VGA selector.
  };

  /**
   * @brief Mutable fake hardware, reset independently for each lifecycle test.
   */
  struct Hardware {
    std::array<std::uint32_t, 0x700000 / 4> registers {};  ///< MMIO storage.
    IndexedVgaRegisters vga;  ///< Indexed VGA state.
    std::array<unsigned char, 640 * 480 * 4> framebuffer {};  ///< CPU-owned framebuffer.
    std::array<std::uint32_t, 640 * 480> gpuBuffer {};  ///< Separate GPU-owned scanout.
    VIDEO_MODE mode {640, 480, 32, 60};  ///< Active output mode.
    DWORD ticks = 0;  ///< Deterministic clock.
    GpuVblank gpuVblank;  ///< Simulates pbkit's vblank counter.
    GpuDepthSurface depthSurface;  ///< Simulates retirement of pbkit's depth surface.
    int initResult = 0;  ///< Injected pbkit initialization result.
    int allocationsUntilFailure = -1;  ///< Negative disables allocation failure injection.
    int initializations = 0;  ///< Number of GPU context acquisitions.
    int shutdowns = 0;  ///< Number of GPU context releases.
    unsigned int pitch = 640 * 4;  ///< GPU buffer pitch.
    std::uint32_t originalStart = 0;  ///< CPU scanout restored by pb_kill.
    bool videoEnabled = true;  ///< TV encoder output state.
    bool backgroundCleared = false;  ///< GPU background was cleared before overlay submission.
    bool staticScanout = false;  ///< The caller requested a single GPU background buffer.
    SoftwareDisplay softwareDisplay;  ///< Simulates software presentation timing.
    std::map<const void *, DmaAllocation> allocations;  ///< Live DMA allocations and addresses.
    std::uint32_t nextPhysical = 0x100000;  ///< Next fake physical allocation address.
  };

  /**
   * @brief Access the hardware state shared by the API shims.
   * @return State reset before each test.
   */
  Hardware &hardware() {
    static Hardware state;
    return state;
  }

  /**
   * @brief Presenter fixture with a small valid YUV420 picture and clean hardware state.
   */
  class PvideoPresenterTest: public testing::Test {
  protected:
    /**
     * @brief Reset hardware without placing its large register bank on the stack.
     */
    void SetUp() override {
      hardware().registers.fill(0);
      hardware().vga = {};
      hardware().mode = {640, 480, 32, 60};
      hardware().ticks = 0;
      hardware().gpuVblank = {};
      hardware().depthSurface = {};
      hardware().initResult = 0;
      hardware().allocationsUntilFailure = -1;
      hardware().initializations = 0;
      hardware().shutdowns = 0;
      hardware().pitch = 640 * 4;
      hardware().videoEnabled = true;
      hardware().backgroundCleared = false;
      hardware().staticScanout = false;
      hardware().softwareDisplay = {};
      hardware().nextPhysical = 0x100000;
      ASSERT_TRUE(hardware().allocations.empty());
    }

    /**
     * @brief Require each test to release all DMA storage.
     */
    void TearDown() override {
      EXPECT_TRUE(hardware().allocations.empty());
    }

    /**
     * @brief Point the presenter at a small valid YUV420 picture.
     * @return Source view valid for the lifetime of the fixture.
     */
    streaming::Yuv420FrameView frame() const {
      return {2, 2, {luma_.data(), chroma_.data(), chroma_.data()}, {4, 1, 1}, {2, 1, 1}};
    }

    /**
     * @brief Place the source into a letterboxed output rectangle.
     * @return The visible video region.
     */
    static constexpr streaming::VideoRectangle destination() {
      return {0, 60, 640, 360};
    }

  private:
    const std::array<std::uint8_t, 4> luma_ {16, 81, 145, 235};  ///< Source luma.
    const std::array<std::uint8_t, 1> chroma_ {128};  ///< Neutral source chroma.
  };
}  // namespace

std::uint32_t &pvideo_test_register(std::uint32_t address) {
  return hardware().registers.at(address / 4);
}

std::uint8_t &pvideo_test_register8(std::uint32_t address) {
  auto &vga = hardware().vga;
  return address == 0x6013d4 ? vga.index : vga.values.at(vga.index);
}

VIDEO_MODE XVideoGetMode() {
  return hardware().mode;
}

unsigned char *XVideoGetFB() {
  return hardware().framebuffer.data();
}

void XVideoFlushFB() {
  hardware().softwareDisplay.flushedScanout = VIDEOREG(CRTC_START);
}

void XVideoSetVideoEnable(int enable) {
  hardware().videoEnabled = enable != 0;
}

DWORD GetTickCount() {
  if (hardware().softwareDisplay.pollVblank) {
    // The software wait acknowledges stale status before reading the clock.
    VIDEOREG(CRTC_INTERRUPT) = 0;
  }
  return hardware().ticks;
}

void Sleep(DWORD milliseconds) {
  hardware().ticks += milliseconds;
  if (hardware().ticks >= hardware().gpuVblank.resumeTick) {
    ++hardware().gpuVblank.count;
    hardware().gpuVblank.background = VIDEOREG(CRTC_START);
    hardware().gpuVblank.overlay = VIDEOREG(VIDEO_BUFFER);
    hardware().gpuVblank.overlayAddress = VIDEOREG(VIDEO_ADDRESS);
    const auto &allocations = hardware().allocations;
    hardware().gpuVblank.packedBuffers = static_cast<std::size_t>(std::count_if(allocations.begin(), allocations.end(), [](const auto &allocation) {
      return allocation.second.storage.front() == 16;
    }));
  }
  if (hardware().softwareDisplay.pollVblank && hardware().softwareDisplay.signalVblank) {
    hardware().softwareDisplay.displayedScanout = VIDEOREG(CRTC_START);
    hardware().softwareDisplay.primaryPixelAtVblank = hardware().framebuffer[0];
    VIDEOREG(CRTC_INTERRUPT) = 1;
  }
}

std::uint32_t pb_get_vbl_counter() {
  return hardware().gpuVblank.count;
}

int pb_busy() {
  return hardware().ticks < hardware().depthSurface.idleTick;
}

/**
 * @brief Simulate PVIDEO acquiring pbkit while preserving the visible framebuffer.
 * @param static_scanout Whether the presenter requested a single color page.
 * @return The simulated initialization result.
 */
extern "C" int sunshine_pb_init_pvideo_scanout(bool static_scanout) {
  ++hardware().initializations;
  hardware().staticScanout = static_scanout;
  hardware().originalStart = VIDEOREG(CRTC_START);
  hardware().vga.values[31] = 87;
  hardware().vga.values[82] += 4;
  hardware().vga.values[32] = 41;
  hardware().vga.values[27] = 5;
  VIDEOREG(0x8910) = 0x1000;
  VIDEOREG(0x600804) = 2;
  if (hardware().initResult != 0) {
    hardware().videoEnabled = false;
  }
  return hardware().initResult;
}

/** @brief Queue depth detachment after the GPU background and overlay buffers are ready. */
extern "C" void sunshine_pb_detach_depth_stencil() {
  EXPECT_TRUE(hardware().staticScanout);
  EXPECT_TRUE(hardware().backgroundCleared);
  EXPECT_GE(hardware().allocations.size(), 2U);
  EXPECT_EQ(VIDEOREG(VIDEO_BUFFER), 0U);
  auto &depth = hardware().depthSurface;
  ++depth.detachments;
  depth.overlayBuffersAtDetach = hardware().allocations.size();
  depth.idleTick = hardware().ticks + depth.detachDuration;
}

/** @brief Record release only after a queued depth detachment has finished. */
extern "C" void sunshine_pb_release_detached_depth_stencil() {
  auto &depth = hardware().depthSurface;
  EXPECT_GT(depth.detachments, depth.releases);
  EXPECT_EQ(pb_busy(), 0);
  EXPECT_EQ(VIDEOREG(VIDEO_BUFFER), 0U);
  ++depth.releases;
}

void pb_kill() {
  ++hardware().shutdowns;
  VIDEOREG(0x600800) = hardware().originalStart;
  hardware().videoEnabled = false;
}

void pb_set_color_format(unsigned int format, bool swizzled) {
  EXPECT_EQ(format, hardware().mode.bpp == 16 ? NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5 : NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8);
  EXPECT_FALSE(swizzled);
}

void pb_fill(int x, int y, int width, int height, std::uint32_t color) {
  EXPECT_EQ(x, 0);
  EXPECT_EQ(y, 0);
  EXPECT_EQ(width, hardware().mode.width);
  EXPECT_EQ(height, hardware().mode.height);
  EXPECT_EQ(color, hardware().mode.bpp == 16 ? 0U : 0xff000000U);
  hardware().backgroundCleared = true;
}

std::uint32_t *pb_back_buffer() {
  return hardware().gpuBuffer.data();
}

unsigned int pb_back_buffer_pitch() {
  return hardware().pitch;
}

void *MmAllocateContiguousMemoryEx(std::size_t size, unsigned int, unsigned int, unsigned int, unsigned int) {  // NOSONAR(cpp:S5008) The shim mirrors the nxdk allocation signature.
  if (hardware().allocationsUntilFailure == 0) {
    return nullptr;
  }
  if (hardware().allocationsUntilFailure > 0) {
    --hardware().allocationsUntilFailure;
  }
  // Seed the fake allocation so the tests can detect missing clears in production.
  std::vector<std::uint8_t> storage(size, 0xcd);
  auto *buffer = storage.data();
  hardware().allocations.try_emplace(buffer, DmaAllocation {std::move(storage), hardware().nextPhysical});
  hardware().nextPhysical += 0x10000;
  return buffer;
}

void MmFreeContiguousMemory(void *buffer) {  // NOSONAR(cpp:S5008) The shim mirrors the nxdk deallocation signature.
  EXPECT_EQ(hardware().allocations.erase(buffer), 1U);
}

std::uintptr_t MmGetPhysicalAddress(void *buffer) {  // NOSONAR(cpp:S5008) The shim mirrors the nxdk address lookup signature.
  if (buffer == hardware().framebuffer.data()) {
    return CPU_FRAMEBUFFER;
  }
  return buffer == hardware().gpuBuffer.data() ? 0x80000 : hardware().allocations.at(buffer).physicalAddress;
}

/**
 * @brief Keep each composed software page intact until the display has latched its replacement.
 */
TEST_F(PvideoPresenterTest, RetiresSoftwarePagesAfterTheAddressLatches) {
  hardware().softwareDisplay.pollVblank = true;
  VIDEOREG(CRTC_START) = CPU_FRAMEBUFFER;
  streaming::SoftwareScanout scanout;
  EXPECT_FALSE(scanout.present());
  const std::size_t size = hardware().framebuffer.size();
  auto *primary = XVideoGetFB();
  primary[0] = 0x73;
  auto *first = scanout.back_buffer(size, destination());
  ASSERT_NE(first, nullptr);
  EXPECT_NE(first, primary);
  EXPECT_EQ(first[0], 0);
  EXPECT_EQ(primary[0], 0x73);
  first[0] = 0x24;
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(1, 1, {0xff}, streaming::VideoOverlayPlacement::full_screen);
  ASSERT_TRUE(overlay.draw_rgb(first, size, hardware().mode.width * 4, hardware().mode.width, hardware().mode.height, hardware().mode.bpp, destination()));
  const auto overlayOffset = static_cast<std::size_t>(destination().y) * hardware().mode.width * 4;
  EXPECT_EQ(first[overlayOffset], 0xff);
  EXPECT_EQ(primary[overlayOffset], 0);
  EXPECT_EQ(VIDEOREG(CRTC_START), CPU_FRAMEBUFFER);
  ASSERT_TRUE(scanout.present());
  EXPECT_EQ(hardware().softwareDisplay.flushedScanout, CPU_FRAMEBUFFER);
  EXPECT_EQ(hardware().softwareDisplay.displayedScanout, MmGetPhysicalAddress(first));
  EXPECT_EQ(XVideoGetFB(), primary);

  auto *second = scanout.back_buffer(size, destination());
  ASSERT_EQ(second, primary);
  EXPECT_EQ(second[0], 0);
  EXPECT_EQ(VIDEOREG(CRTC_START), MmGetPhysicalAddress(first));
  ASSERT_TRUE(scanout.present());
  EXPECT_EQ(hardware().softwareDisplay.displayedScanout, CPU_FRAMEBUFFER);
  EXPECT_EQ(scanout.back_buffer(size, destination())[0], 0x24);
  EXPECT_EQ(scanout.back_buffer(size, {0, 0, 640, 480})[0], 0);
  ASSERT_TRUE(scanout.present());
  EXPECT_EQ(hardware().softwareDisplay.displayedScanout, MmGetPhysicalAddress(first));

  EXPECT_TRUE(scanout.reset());
  EXPECT_EQ(hardware().softwareDisplay.displayedScanout, CPU_FRAMEBUFFER);
  EXPECT_TRUE(hardware().allocations.empty());
}

/**
 * @brief Unavailable contiguous memory leaves the original software framebuffer active.
 */
TEST_F(PvideoPresenterTest, LeavesPrimaryScanoutActiveWhenSoftwareAllocationFails) {
  hardware().allocationsUntilFailure = 0;
  VIDEOREG(CRTC_START) = CPU_FRAMEBUFFER;
  streaming::SoftwareScanout scanout;
  EXPECT_EQ(scanout.back_buffer(hardware().framebuffer.size(), destination()), nullptr);
  EXPECT_EQ(VIDEOREG(CRTC_START), CPU_FRAMEBUFFER);
}

/**
 * @brief Missing retrace stops page reuse while the display may still read the previous frame.
 */
TEST_F(PvideoPresenterTest, BlocksSoftwarePageReuseOnVblankTimeout) {
  hardware().softwareDisplay.pollVblank = true;
  hardware().softwareDisplay.signalVblank = false;
  VIDEOREG(CRTC_START) = CPU_FRAMEBUFFER;
  streaming::SoftwareScanout scanout;
  auto *back = scanout.back_buffer(hardware().framebuffer.size(), destination());
  ASSERT_NE(back, nullptr);
  EXPECT_FALSE(scanout.present());
  EXPECT_EQ(hardware().ticks, 50U);
  EXPECT_EQ(VIDEOREG(CRTC_START), MmGetPhysicalAddress(back));
  EXPECT_EQ(hardware().softwareDisplay.displayedScanout, CPU_FRAMEBUFFER);
  EXPECT_EQ(scanout.back_buffer(hardware().framebuffer.size(), destination()), nullptr);
  EXPECT_FALSE(scanout.present());
  hardware().softwareDisplay.signalVblank = true;
  EXPECT_TRUE(scanout.reset());
}

/**
 * @brief 720p copies a fully composed frame at vblank without scanning out the staging allocation.
 */
TEST_F(PvideoPresenterTest, CopiesHighDefinitionFrameWithoutChangingScanoutAddress) {
  hardware().mode = {1280, 720, 32, 60};
  hardware().softwareDisplay.pollVblank = true;
  VIDEOREG(CRTC_START) = CPU_FRAMEBUFFER;
  streaming::SoftwareScanout scanout;
  auto *primary = XVideoGetFB();
  constexpr std::size_t frameBytes = 32;
  std::fill_n(primary, frameBytes + 1, 0x57);
  auto *staged = scanout.back_buffer(frameBytes, {0, 0, 1280, 720});
  ASSERT_NE(staged, nullptr);
  EXPECT_NE(staged, primary);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(staged) % 32U, 0U);
  std::fill_n(staged, frameBytes, 0x35);
  EXPECT_EQ(primary[0], 0x57);
  EXPECT_TRUE(hardware().allocations.empty());

  ASSERT_TRUE(scanout.present());
  EXPECT_EQ(hardware().softwareDisplay.primaryPixelAtVblank, 0x57);
  EXPECT_EQ(VIDEOREG(CRTC_START), CPU_FRAMEBUFFER);
  EXPECT_EQ(hardware().softwareDisplay.displayedScanout, CPU_FRAMEBUFFER);
  EXPECT_TRUE(std::equal(staged, staged + frameBytes, primary));
  EXPECT_EQ(primary[frameBytes], 0x57);
  EXPECT_EQ(scanout.back_buffer(frameBytes, {0, 0, 1280, 720}), staged);
  EXPECT_TRUE(scanout.reset());
  EXPECT_EQ(VIDEOREG(CRTC_START), CPU_FRAMEBUFFER);
}

/**
 * @brief On retrace timeout, keep the currently displayed HD frame intact.
 */
TEST_F(PvideoPresenterTest, DoesNotCopyHighDefinitionFrameOnVblankTimeout) {
  hardware().mode = {1280, 720, 32, 60};
  hardware().softwareDisplay.pollVblank = true;
  hardware().softwareDisplay.signalVblank = false;
  VIDEOREG(CRTC_START) = CPU_FRAMEBUFFER;
  streaming::SoftwareScanout scanout;
  XVideoGetFB()[0] = 0x67;
  auto *staged = scanout.back_buffer(32, {0, 0, 1280, 720});
  ASSERT_NE(staged, nullptr);
  staged[0] = 0x29;
  EXPECT_FALSE(scanout.present());
  EXPECT_EQ(XVideoGetFB()[0], 0x67);
  EXPECT_EQ(VIDEOREG(CRTC_START), CPU_FRAMEBUFFER);
  EXPECT_TRUE(scanout.reset());
}

/**
 * @brief Both RAM buffers are submitted through bank zero with source dimensions in pixels.
 */
TEST_F(PvideoPresenterTest, AlternatesMemoryWithoutSelectingUnsupportedOverlayBank) {
  streaming::PvideoPresenter presenter;
  std::string error;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  EXPECT_TRUE(hardware().backgroundCleared);
  EXPECT_EQ(VIDEOREG(0x600800), 0x80000U);
  EXPECT_EQ(VIDEOREG(0x8928), 2U | (2U << 16U));
  EXPECT_EQ(VIDEOREG(0x8700), 1U);
  const auto first = VIDEOREG(0x8920);
  const auto firstLimit = VIDEOREG(0x8908);
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  EXPECT_EQ(VIDEOREG(0x8700), 1U);
  EXPECT_NE(VIDEOREG(0x8920), first);
  const auto second = VIDEOREG(0x8920);
  const auto bytes = 2U * 64U;
  // A compositor can retain either frame's limit while reading either address.
  EXPECT_LE(second + bytes, firstLimit);
  EXPECT_EQ(VIDEOREG(0x8908), firstLimit);
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  EXPECT_EQ(VIDEOREG(0x8920), first);
  EXPECT_LE(second + bytes, VIDEOREG(0x8908));
  EXPECT_EQ(VIDEOREG(0x8908), firstLimit);
  EXPECT_EQ(VIDEOREG(0x8924), 0U);
  EXPECT_GE(VIDEOREG(0x8908) - VIDEOREG(0x8920) + 1U, 2U * 64U + 64U);
}

/**
 * @brief The production GPU path composites the cached panel before submitting either buffer.
 */
TEST_F(PvideoPresenterTest, CompositesStatisticsIntoSubmittedVideoBuffer) {
  streaming::PvideoPresenter presenter;
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(2, 2, {255, 255, 0, 0});
  std::string error;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error, {}, &overlay)) << error;
  const auto allocation = std::find_if(hardware().allocations.begin(), hardware().allocations.end(), [](const auto &entry) {
    return entry.second.physicalAddress == VIDEOREG(0x8920);
  });
  ASSERT_NE(allocation, hardware().allocations.end());
  const auto *pixels = static_cast<const std::uint8_t *>(allocation->first);
  EXPECT_EQ(pixels[0], 235);
  EXPECT_EQ(pixels[1], 128);
  EXPECT_EQ(pixels[2], 235);
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  const auto next = std::find_if(hardware().allocations.begin(), hardware().allocations.end(), [](const auto &entry) {
    return entry.second.physicalAddress == VIDEOREG(0x8920);
  });
  ASSERT_NE(next, hardware().allocations.end());
  EXPECT_EQ(static_cast<const std::uint8_t *>(next->first)[2], 81);
}

/**
 * @brief Compose the transparent save icon over an existing panel and remove it on the next submission.
 */
TEST_F(PvideoPresenterTest, CompositesAndClearsAnIndependentSaveIcon) {
  constexpr int width = 64;
  constexpr int height = 48;
  const std::vector<std::uint8_t> y(width * height, 81);
  const std::vector<std::uint8_t> uv(width * height / 4, 128);
  const streaming::Yuv420FrameView source {width, height, {y.data(), uv.data(), uv.data()}, {y.size(), uv.size(), uv.size()}, {width, width / 2, width / 2}};
  constexpr streaming::VideoRectangle target {0, 0, width, height};
  streaming::VideoOverlay panel;
  panel.set_bitmap(width, height, std::vector<std::uint8_t>(width * height, 64), streaming::VideoOverlayPlacement::full_screen);
  constexpr std::uint8_t panelLuma = 71;
  streaming::VideoOverlay indicator;
  std::vector<std::uint8_t> icon(8 * 8 * 4, 255);
  std::fill_n(icon.begin(), 4, 0);
  ASSERT_TRUE(indicator.set_argb_surface(8, 8, icon.data(), icon.size(), 32, streaming::VideoOverlayPlacement::bottom_right));
  const auto bounds = indicator.rectangle(target);
  const auto pixelOffset = (bounds.y * width + bounds.x) * 2;
  const auto submitted = [] {
    const auto allocation = std::find_if(hardware().allocations.begin(), hardware().allocations.end(), [](const auto &entry) {
      return entry.second.physicalAddress == VIDEOREG(0x8920);
    });
    return allocation == hardware().allocations.end() ? nullptr : static_cast<const std::uint8_t *>(allocation->first);
  };
  streaming::PvideoPresenter presenter;
  std::string error;
  ASSERT_TRUE(presenter.present(source, target, &error, {}, &panel, &indicator)) << error;
  ASSERT_NE(submitted(), nullptr);
  EXPECT_EQ(submitted()[pixelOffset], panelLuma);
  EXPECT_EQ(submitted()[pixelOffset + 2], 235);
  EXPECT_EQ(submitted()[pixelOffset - 2], panelLuma);

  indicator.set_bitmap(0, 0, {});
  ASSERT_TRUE(presenter.present(source, target, &error, {}, &panel, &indicator)) << error;
  ASSERT_NE(submitted(), nullptr);
  EXPECT_EQ(submitted()[pixelOffset + 2], panelLuma);
}

/** @brief Return to the CPU framebuffer if the GPU compositor cannot draw the panel. */
TEST_F(PvideoPresenterTest, ReleasesGpuResourcesWhenOverlayCompositionFails) {
  streaming::PvideoPresenter presenter;
  streaming::VideoOverlay overlay;
  overlay.set_bitmap(2, 2, {255, 255, 0, 0});
  VIDEOREG(CRTC_START) = CPU_FRAMEBUFFER;
  std::string error;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error, {}, &overlay)) << error;

  const streaming::VideoRectangle tooSmallForPanel {0, 0, 8, 8};
  EXPECT_FALSE(presenter.present(frame(), tooSmallForPanel, &error, {}, &overlay));
  EXPECT_NE(error.find("Could not compose"), std::string::npos);
  EXPECT_EQ(VIDEOREG(CRTC_START), CPU_FRAMEBUFFER);
  EXPECT_EQ(VIDEOREG(0x8700), 0U);
  EXPECT_TRUE(hardware().allocations.empty());
  EXPECT_FALSE(presenter.active());
}

/** @brief Reuse a buffer as soon as its submission has crossed vblank. */
TEST_F(PvideoPresenterTest, SkipsWaitingWhenVblankHasAlreadyRetiredThePreviousBuffer) {
  streaming::PvideoPresenter presenter;
  std::string error;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;

  ++hardware().gpuVblank.count;
  const auto readyTick = hardware().ticks;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  EXPECT_EQ(hardware().ticks, readyTick);

  // The newest submission still needs one blank before its old buffer is safe.
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  EXPECT_GT(hardware().ticks, readyTick);
}

/** @brief Pack the 720p spare before the blank, then replace the pending address after it latches. */
TEST_F(PvideoPresenterTest, Prepares720pSpareWhileThePriorRequestIsPending) {
  hardware().mode = {1280, 720, 32, 60};
  hardware().pitch = 1280 * 4;
  streaming::PvideoPresenter presenter;
  std::string error;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  EXPECT_EQ(hardware().allocations.size(), 3U);
  EXPECT_EQ(hardware().depthSurface.detachments, 0U);
  const auto first = VIDEOREG(VIDEO_ADDRESS);

  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  const auto second = VIDEOREG(VIDEO_ADDRESS);
  EXPECT_NE(first, second);

  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  const auto third = VIDEOREG(VIDEO_ADDRESS);
  EXPECT_NE(third, first);
  EXPECT_NE(third, second);
  EXPECT_EQ(hardware().gpuVblank.packedBuffers, 3U);
  EXPECT_EQ(hardware().gpuVblank.overlayAddress, second);
  EXPECT_EQ(VIDEOREG(0x8908), third + 64U * 3U - 1U);

  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  EXPECT_EQ(hardware().gpuVblank.overlayAddress, third);
  EXPECT_EQ(VIDEOREG(VIDEO_ADDRESS), first);
}

/** @brief Preserve double-buffered 720p presentation when the spare cannot be allocated. */
TEST_F(PvideoPresenterTest, ReusesTwo720pBuffersWhenTheSpareAllocationFails) {
  hardware().mode = {1280, 720, 32, 60};
  hardware().pitch = 1280 * 4;
  hardware().allocationsUntilFailure = 2;
  streaming::PvideoPresenter presenter;
  std::string error;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  const auto first = VIDEOREG(VIDEO_ADDRESS);
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  const auto second = VIDEOREG(VIDEO_ADDRESS);
  EXPECT_NE(first, second);

  const auto pendingTick = hardware().ticks;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  EXPECT_GT(hardware().ticks, pendingTick);
  EXPECT_EQ(hardware().gpuVblank.overlayAddress, second);
  EXPECT_EQ(VIDEOREG(VIDEO_ADDRESS), first);
  EXPECT_EQ(hardware().allocations.size(), 2U);
}

/** @brief Retire the pending request if a packed 720p spare cannot be submitted before the timeout. */
TEST_F(PvideoPresenterTest, RestoresSoftwareOutputWhen720pSpareCannotBeSubmitted) {
  hardware().mode = {1280, 720, 32, 60};
  hardware().pitch = 1280 * 4;
  VIDEOREG(CRTC_START) = CPU_FRAMEBUFFER;
  streaming::PvideoPresenter presenter;
  std::string error;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  const auto pendingAddress = VIDEOREG(VIDEO_ADDRESS);

  hardware().gpuVblank.resumeTick = hardware().ticks + 51U;
  EXPECT_FALSE(presenter.present(frame(), destination(), &error));
  EXPECT_NE(error.find("Timed out waiting for GPU scanout"), std::string::npos);
  EXPECT_EQ(hardware().gpuVblank.packedBuffers, 3U);
  EXPECT_EQ(hardware().gpuVblank.overlayAddress, pendingAddress);
  EXPECT_EQ(VIDEOREG(VIDEO_BUFFER), 0U);
  EXPECT_EQ(VIDEOREG(CRTC_START), CPU_FRAMEBUFFER);
  EXPECT_FALSE(presenter.active());
  EXPECT_TRUE(hardware().allocations.empty());
}

/** @brief The progress card remains visible while the first overlay frame is submitted and latched. */
TEST_F(PvideoPresenterTest, KeepsStartupPageUntilFirstOverlayLatch) {
  VIDEOREG(CRTC_START) = CPU_FRAMEBUFFER;
  XVideoGetFB()[0] = 0x67;
  streaming::PvideoPresenter presenter;
  std::string error;

  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  EXPECT_EQ(hardware().gpuVblank.background, CPU_FRAMEBUFFER);
  EXPECT_EQ(hardware().gpuVblank.overlay, 1U);
  EXPECT_EQ(VIDEOREG(CRTC_START), MmGetPhysicalAddress(pb_back_buffer()));
  EXPECT_EQ(XVideoGetFB()[0], 0x67);
}

/** @brief A stalled first overlay is retired without replacing the startup page. */
TEST_F(PvideoPresenterTest, KeepsStartupPageWhenFirstOverlayCannotLatch) {
  VIDEOREG(CRTC_START) = CPU_FRAMEBUFFER;
  hardware().gpuVblank.resumeTick = 51;
  streaming::PvideoPresenter presenter;
  std::string error;

  EXPECT_FALSE(presenter.present(frame(), destination(), &error));
  EXPECT_NE(error.find("Timed out waiting for the first GPU frame"), std::string::npos);
  EXPECT_EQ(hardware().gpuVblank.background, CPU_FRAMEBUFFER);
  EXPECT_EQ(VIDEOREG(CRTC_START), CPU_FRAMEBUFFER);
  EXPECT_EQ(VIDEOREG(VIDEO_BUFFER), 0U);
  EXPECT_FALSE(presenter.active());
  EXPECT_TRUE(hardware().allocations.empty());
}

/** @brief A stalled scanout fails presentation by the deadline and can still retire cleanly. */
TEST_F(PvideoPresenterTest, TimesOutWhenVblankCannotRetireThePreviousBuffer) {
  streaming::PvideoPresenter presenter;
  std::string error;
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
  // The first submission waits for its own latch. The second queues a frame
  // whose preceding buffer still has to retire.
  ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;

  hardware().gpuVblank.resumeTick = hardware().ticks + 51U;
  EXPECT_FALSE(presenter.present(frame(), destination(), &error));
  EXPECT_NE(error.find("Timed out waiting for GPU scanout"), std::string::npos);
  EXPECT_EQ(hardware().ticks, hardware().gpuVblank.resumeTick);
  EXPECT_EQ(VIDEOREG(0x8700), 0U);
  EXPECT_FALSE(presenter.active());
  EXPECT_TRUE(hardware().allocations.empty());
}

/** @brief Apply Auto and explicit buffer counts at each output resolution. */
TEST_F(PvideoPresenterTest, SelectsGpuBufferCountAcrossOutputModes) {
  using streaming::GpuBufferCount;
  for (const auto &mode : {VIDEO_MODE {640, 480, 32, 60}, VIDEO_MODE {1280, 720, 32, 60}, VIDEO_MODE {1920, 1080, 16, 60}}) {
    hardware().mode = mode;
    hardware().pitch = mode.width * (mode.bpp / 8);
    for (const auto count : {GpuBufferCount::automatic, GpuBufferCount::two, GpuBufferCount::three}) {
      SCOPED_TRACE(mode.height);
      SCOPED_TRACE(static_cast<int>(count));
      streaming::PvideoPresenter presenter;
      presenter.set_buffer_count(count);
      EXPECT_EQ(presenter.active_buffer_count(), 0U);

      std::string error;
      ASSERT_TRUE(presenter.present(frame(), {0, 0, mode.width, mode.height}, &error)) << error;
      const auto expected = count == GpuBufferCount::three || (count == GpuBufferCount::automatic && mode.height == 720) ? 3U : 2U;
      EXPECT_EQ(hardware().allocations.size(), expected);
      EXPECT_EQ(presenter.active_buffer_count(), expected);
      ASSERT_TRUE(presenter.reset());
      EXPECT_EQ(presenter.active_buffer_count(), 0U);
    }
  }
}

/** @brief Release depth after GPU detachment and after allocating the requested 1080i spare. */
TEST_F(PvideoPresenterTest, RetiresDepthBeforeSubmitting1080iOverlay) {
  hardware().mode = {1920, 1080, 16, 60};
  hardware().pitch = 1920 * 2;
  hardware().depthSurface.detachDuration = 3;
  const streaming::VideoRectangle output {0, 0, 1920, 1080};
  streaming::PvideoPresenter presenter;
  presenter.set_buffer_count(streaming::GpuBufferCount::three);
  std::string error;
  ASSERT_TRUE(presenter.present(frame(), output, &error)) << error;
  EXPECT_EQ(VIDEOREG(0x8950), 1920U | (1080U << 16U));
  EXPECT_EQ(hardware().allocations.size(), 3U);
  EXPECT_TRUE(hardware().backgroundCleared);
  EXPECT_TRUE(hardware().staticScanout);
  ASSERT_TRUE(presenter.present(frame(), output, &error)) << error;
  EXPECT_EQ(hardware().depthSurface.detachments, 1U);
  EXPECT_EQ(hardware().depthSurface.releases, 1U);
  EXPECT_EQ(hardware().depthSurface.overlayBuffersAtDetach, 3U);
}

/** @brief Keep 1080i GPU presentation double-buffered if the optional allocation fails. */
TEST_F(PvideoPresenterTest, ReusesTwo1080iBuffersWhenTheSpareAllocationFails) {
  hardware().mode = {1920, 1080, 16, 60};
  hardware().pitch = 1920 * 2;
  hardware().allocationsUntilFailure = 2;
  const streaming::VideoRectangle output {0, 0, 1920, 1080};
  streaming::PvideoPresenter presenter;
  presenter.set_buffer_count(streaming::GpuBufferCount::three);
  std::string error;

  ASSERT_TRUE(presenter.present(frame(), output, &error)) << error;
  const auto first = VIDEOREG(VIDEO_ADDRESS);
  ASSERT_TRUE(presenter.present(frame(), output, &error)) << error;
  EXPECT_NE(VIDEOREG(VIDEO_ADDRESS), first);
  ASSERT_TRUE(presenter.present(frame(), output, &error)) << error;
  EXPECT_EQ(VIDEOREG(VIDEO_ADDRESS), first);
  EXPECT_EQ(hardware().allocations.size(), 2U);
  EXPECT_EQ(presenter.active_buffer_count(), 2U);
  EXPECT_EQ(hardware().depthSurface.releases, 1U);
  EXPECT_EQ(hardware().depthSurface.overlayBuffersAtDetach, 2U);
}

/** @brief Leave depth cleanup to pbkit and keep CPU output visible if detaching depth times out. */
TEST_F(PvideoPresenterTest, RestoresSoftwareOutputWhenDepthDetachmentTimesOut) {
  hardware().mode = {1920, 1080, 16, 60};
  hardware().pitch = 1920 * 2;
  // The commands complete in time for teardown, after the initial deadline.
  hardware().depthSurface.detachDuration = 101;
  VIDEOREG(CRTC_START) = CPU_FRAMEBUFFER;
  streaming::PvideoPresenter presenter;
  std::string error;

  EXPECT_FALSE(presenter.present(frame(), {0, 0, 1920, 1080}, &error));
  EXPECT_NE(error.find("Timed out detaching the GPU depth surface"), std::string::npos);
  EXPECT_EQ(hardware().depthSurface.detachments, 1U);
  EXPECT_EQ(hardware().depthSurface.releases, 0U);
  EXPECT_EQ(hardware().shutdowns, 1);
  EXPECT_EQ(VIDEOREG(VIDEO_BUFFER), 0U);
  EXPECT_EQ(VIDEOREG(CRTC_START), CPU_FRAMEBUFFER);
  EXPECT_TRUE(hardware().videoEnabled);
  EXPECT_FALSE(presenter.active());
  EXPECT_TRUE(hardware().allocations.empty());
}

/**
 * @brief A fresh blank after stop lets each session restore CPU scanout and legacy registers safely.
 */
TEST_F(PvideoPresenterTest, RestoresOutputAndRegistersAcrossRepeatedSessions) {
  constexpr std::array<int, 4> crtcRegisters {31, 82, 32, 27};
  const std::array<std::uint8_t, 4> originalCrtc {13, 14, 15, 16};
  for (std::size_t index = 0; index < originalCrtc.size(); ++index) {
    hardware().vga.values[crtcRegisters[index]] = originalCrtc[index];
  }
  VIDEOREG(0x8910) = 0x2000;
  VIDEOREG(0x600800) = 0x3eb4000;
  VIDEOREG(0x600804) = 3;
  streaming::PvideoPresenter presenter;
  for (int session = 0; session < 2; ++session) {
    std::string error;
    ASSERT_TRUE(presenter.present(frame(), destination(), &error)) << error;
    ++hardware().gpuVblank.count;
    const auto stopTick = hardware().ticks;
    ASSERT_TRUE(presenter.reset());
    EXPECT_GT(hardware().ticks, stopTick);
    EXPECT_FALSE(presenter.active());
    EXPECT_EQ(VIDEOREG(0x8700), 0U);
    EXPECT_EQ(VIDEOREG(0x8910), 0x2000U);
    EXPECT_EQ(VIDEOREG(0x600800), 0x3eb4000U);
    EXPECT_EQ(VIDEOREG(0x600804), 3U);
    EXPECT_TRUE(hardware().videoEnabled);
    EXPECT_TRUE(hardware().allocations.empty());
    for (std::size_t index = 0; index < originalCrtc.size(); ++index) {
      EXPECT_EQ(hardware().vga.values[crtcRegisters[index]], originalCrtc[index]);
    }
  }
  EXPECT_EQ(hardware().initializations, 2);
  EXPECT_EQ(hardware().shutdowns, 2);
  EXPECT_EQ(hardware().depthSurface.detachments, 0U);
}

/**
 * @brief A partial allocation failure releases GPU ownership and restores visible software output.
 */
TEST_F(PvideoPresenterTest, RestoresSoftwareOutputAfterPartialAllocationFailure) {
  hardware().mode = {1920, 1080, 16, 60};
  hardware().pitch = 1920 * 2;
  hardware().allocationsUntilFailure = 1;
  streaming::PvideoPresenter presenter;
  std::string error;
  EXPECT_FALSE(presenter.present(frame(), destination(), &error));
  EXPECT_NE(error.find("contiguous memory"), std::string::npos);
  EXPECT_TRUE(hardware().videoEnabled);
  EXPECT_TRUE(hardware().allocations.empty());
  EXPECT_EQ(hardware().shutdowns, 1);
  EXPECT_EQ(hardware().depthSurface.detachments, 0U);
}

/**
 * @brief pbkit failure may disable output internally, so initialization failures also restore it.
 */
TEST_F(PvideoPresenterTest, RestoresSoftwareOutputAfterGpuInitializationFailure) {
  hardware().initResult = -4;
  streaming::PvideoPresenter presenter;
  std::string error;
  EXPECT_FALSE(presenter.present(frame(), destination(), &error));
  EXPECT_NE(error.find("pbkit -4"), std::string::npos);
  EXPECT_TRUE(hardware().videoEnabled);
  EXPECT_EQ(hardware().shutdowns, 0);
}

/**
 * @brief Unsupported output format and scanout pitch select software with a clear explanation.
 */
TEST_F(PvideoPresenterTest, RejectsIncompatibleOutputBeforeSubmittingOverlay) {
  streaming::PvideoPresenter presenter;
  std::string error;
  hardware().mode.bpp = 15;
  EXPECT_FALSE(presenter.present(frame(), destination(), &error));
  EXPECT_NE(error.find("16-bit or 32-bit framebuffer"), std::string::npos);
  EXPECT_EQ(hardware().initializations, 0);
  hardware().mode.bpp = 32;
  hardware().pitch += 64;
  EXPECT_FALSE(presenter.present(frame(), destination(), &error));
  EXPECT_NE(error.find("pitch"), std::string::npos);
  EXPECT_EQ(VIDEOREG(0x8700), 0U);
  EXPECT_TRUE(hardware().videoEnabled);
  EXPECT_EQ(hardware().shutdowns, 1);
}
