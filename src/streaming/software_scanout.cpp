/**
 * @file src/streaming/software_scanout.cpp
 * @brief Composes software frames off-screen before publishing them through Xbox scanout.
 */
// class header include
#include "src/streaming/software_scanout.h"

// local includes
#include "src/_nxdk_compat/ffmpeg_memory.h"

#ifdef NXDK
  // standard includes
  #include <cstring>

  // nxdk includes
  #include <hal/video.h>
  #include <synchapi.h>
  #include <sysinfoapi.h>
  #include <xboxkrnl/xboxkrnl.h>

namespace {
  constexpr std::uint32_t CRTC_START = 0x600800;  ///< Physical address of the primary scanout page.
  constexpr std::uint32_t CRTC_INTERRUPT = 0x600100;  ///< Display interrupt status with write-one-to-clear semantics.
  constexpr std::uint32_t VBLANK = 1;  ///< End of active display.

  /**
   * @brief Wait for a fresh vblank without taking ownership of the display IRQ.
   * @return True when a vblank occurs within 50 milliseconds.
   */
  bool wait_for_vblank() {
    VIDEOREG(CRTC_INTERRUPT) = VBLANK;
    const auto start = GetTickCount();
    while ((VIDEOREG(CRTC_INTERRUPT) & VBLANK) == 0) {
      if (GetTickCount() - start >= 50U) {
        return false;
      }
      Sleep(1);
    }
    return true;
  }

  /**
   * @brief Program the scanout address, leaving nxdk's CPU framebuffer pointer intact for SDL.
   * @param page Contiguous framebuffer allocation.
   */
  void set_scanout(std::uint8_t *page) {
    VIDEOREG(CRTC_START) = static_cast<std::uint32_t>(MmGetPhysicalAddress(page));
  }
}  // namespace
#endif

namespace streaming {
  void SoftwareScanout::StagingBufferDeleter::operator()(std::byte *buffer) const noexcept {
    moonlight_ffmpeg_aligned_free(buffer);
  }

  SoftwareScanout::~SoftwareScanout() {
    reset();
  }

  std::uint8_t *SoftwareScanout::back_buffer(std::size_t size, const VideoRectangle &destination) {
#ifdef NXDK
    if (!synchronized_) {
      return nullptr;
    }
    if (pages_[1] == nullptr) {
      pages_[0] = XVideoGetFB();
      if (pages_[0] == nullptr || size == 0) {
        return nullptr;
      }
      if (XVideoGetMode().height >= 720) {
        // A second HD scanout allocation can reuse VRAM from the boot logo.
        // xemu may select the cached GPU texture at that address instead of
        // the CPU framebuffer. Keep the nxdk scanout address for HD modes.
        auto *allocation = moonlight_ffmpeg_aligned_malloc(size + 64U, 32U);
        staging_.reset(static_cast<std::byte *>(allocation));
        pages_[1] = static_cast<std::uint8_t *>(allocation);
        if (allocation != nullptr) {
          stagingSize_ = size;
        }
      } else if (!scanoutQuarantined_) {
        pages_[1] = static_cast<std::uint8_t *>(MmAllocateContiguousMemoryEx(size, 0, 0x03ffffff, 0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE));
      }
      if (pages_[1] == nullptr) {
        return nullptr;
      }
    }
    const auto back = front_ ^ 1U;
    if (auto &previous = destinations_[back]; previous.x != destination.x || previous.y != destination.y || previous.width != destination.width || previous.height != destination.height) {
      std::memset(pages_[back], 0, size);
      previous = destination;
    }
    return pages_[back];
#else
    (void) size;
    (void) destination;
    return nullptr;
#endif
  }

  bool SoftwareScanout::present() {
#ifdef NXDK
    if (pages_[1] == nullptr || !synchronized_) {
      return false;
    }
    if (staging_) {
      synchronized_ = wait_for_vblank();
      if (synchronized_) {
        std::memcpy(pages_[0], pages_[1], stagingSize_);
        XVideoFlushFB();
      }
      return synchronized_;
    }
    XVideoFlushFB();
    front_ ^= 1U;
    set_scanout(pages_[front_]);
    // xemu refreshes its VGA surface before signaling vblank. Its renderer can
    // keep reading the previous page until the first vblank after this write.
    synchronized_ = wait_for_vblank();
    return synchronized_;
#else
    return false;
#endif
  }

  bool SoftwareScanout::reset() {
#ifdef NXDK
    if (pages_[1] == nullptr) {
      return true;
    }
    bool retired = true;
    if (staging_) {
      staging_.reset();
      stagingSize_ = 0;
    } else {
      set_scanout(pages_[0]);
      retired = wait_for_vblank();
      if (retired) {
        MmFreeContiguousMemory(pages_[1]);
      } else {
        // Scanout may still read the extra page. Leave it allocated for the rest of the process.
        scanoutQuarantined_ = true;
      }
    }
    pages_ = {};
    destinations_ = {};
    front_ = 0;
    synchronized_ = true;
    return retired;
#else
    return true;
#endif
  }
}  // namespace streaming
