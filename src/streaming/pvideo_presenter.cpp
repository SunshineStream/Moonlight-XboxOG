/**
 * @file src/streaming/pvideo_presenter.cpp
 * @brief Submits bounded YUY2 overlay buffers using documented NV2A PVIDEO registers.
 */
// class header include
#include "src/streaming/pvideo_presenter.h"

#ifdef NXDK
  // standard includes
  #include <algorithm>
  #include <cstring>

  // nxdk includes
  #include <hal/video.h>
  #include <pbkit/pbkit.h>
  #include <synchapi.h>
  #include <sysinfoapi.h>
  #include <xboxkrnl/xboxkrnl.h>

/**
 * @brief Initialize pbkit without replacing the framebuffer currently on screen.
 *
 * @param static_scanout Allocate a single color page to fit the 1080i memory budget.
 * @return Zero on success or a pbkit initialization error.
 */
extern "C" int moonlight_pb_init_pvideo_scanout(bool static_scanout);

/** @brief Queue depth and stencil detachment from the 1080i background target. */
extern "C" void moonlight_pb_detach_depth_stencil();

/** @brief Release the detached depth surface after the GPU has completed the detachment. */
extern "C" void moonlight_pb_release_detached_depth_stencil();

namespace {
  constexpr std::uint32_t CRTC_START = 0x600800;  ///< Physical address of the scanout background.
  constexpr std::uint32_t VIDEO_BUFFER = 0x8700;  ///< Overlay buffer submission register.
  constexpr std::uint32_t VIDEO_STOP = 0x8704;  ///< Normal overlay stop register.
  constexpr std::array<std::uint8_t, 4> CRTC_REGISTERS {31, 82, 32, 27};  ///< VGA controls changed by pb_init but not restored by pb_kill.

  /** @brief Keep DMA storage resident and disable overlays after synchronization fails. */
  struct OverlayQuarantine {
    bool disabled = false;  ///< Whether a timeout prevents future overlays.
    std::array<std::uint8_t *, 3> buffers {};  ///< Allocations the display may still read.
  };

  /**
   * @brief Share quarantine state across presenter instances for the process lifetime.
   * @return State accessed by the presentation thread.
   */
  OverlayQuarantine &overlay_quarantine() {
    static OverlayQuarantine quarantine;
    return quarantine;
  }

  /**
   * @brief Wait for a vblank after the supplied pbkit counter value.
   *
   * xemu's raster register increments on reads rather than reporting scanlines,
   * so a raster-wrap poll cannot reliably synchronize buffer reuse there.
   *
   * @param previous Counter snapshot taken after initialization, submission or stop.
   * @return True if the counter has advanced, or false after 50 milliseconds.
   */
  bool wait_for_scanout_after(std::uint32_t previous) {
    const auto started = GetTickCount();
    while (pb_get_vbl_counter() == previous) {
      if (GetTickCount() - started >= 50U) {
        return false;
      }
      Sleep(1);
    }
    return true;
  }

  /**
   * @brief Bound GPU command completion before releasing the scanout context.
   *
   * @return True when the command processor is idle, or false after 100 milliseconds.
   */
  bool wait_for_gpu_idle() {
    const auto started = GetTickCount();
    while (pb_busy()) {
      if (GetTickCount() - started >= 100U) {
        return false;
      }
      Sleep(1);
    }
    return true;
  }

  constexpr std::array<std::uint32_t, 28> SAVED_REGISTERS {
    0x8140,
    0x8704,
    0x8900,
    0x8908,
    0x8910,
    0x8918,
    0x8920,
    0x8928,
    0x8930,
    0x8938,
    0x8940,
    0x8948,
    0x8950,
    0x8958,
    0x8904,
    0x890c,
    0x8914,
    0x891c,
    0x8924,
    0x892c,
    0x8934,
    0x893c,
    0x8944,
    0x894c,
    0x8954,
    0x895c,
    0x8088,
    0x808c,
  };  ///< Interrupt, buffer and fetch state touched by the overlay presenter.

  /**
   * @brief Encode an endpoint-aligned 12.20 scaling increment.
   *
   * @param source Source extent in pixels.
   * @param destination Destination extent in pixels.
   *
   * @return Hardware scaling increment.
   */
  std::uint32_t scaling_increment(int source, int destination) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(source - 1) << 20U) / static_cast<unsigned int>(destination - 1));
  }
}  // namespace
#endif

namespace streaming {
  PvideoPresenter::~PvideoPresenter() {
    reset();
  }

  bool PvideoPresenter::active() const {
    return active_;
  }

  bool PvideoPresenter::initialize_scanout_surface(std::string *error) {
#ifdef NXDK
    const VIDEO_MODE mode = XVideoGetMode();
    const auto *framebuffer = XVideoGetFB();
    const int pitch = mode.width * (mode.bpp / 8);
    if (framebuffer == nullptr || (mode.bpp != 16 && mode.bpp != 32) || mode.width <= 0 || mode.height <= 0 || (pitch & 63) != 0) {
      *error = "GPU scanout requires a 16-bit or 32-bit framebuffer with a 64-byte aligned pitch";
      return false;
    }
    for (std::size_t index = 0; index < SAVED_REGISTERS.size(); ++index) {
      savedRegisters_[index] = VIDEOREG(SAVED_REGISTERS[index]);
    }
    const auto crtcIndex = VIDEOREG8(0x6013d4);
    for (std::size_t index = 0; index < CRTC_REGISTERS.size(); ++index) {
      VIDEOREG8(0x6013d4) = CRTC_REGISTERS[index];
      savedCrtcRegisters_[index] = VIDEOREG8(0x6013d5);
    }
    VIDEOREG8(0x6013d4) = crtcIndex;
    savedCrtcConfig_ = VIDEOREG(0x600804);
    scanoutStateSaved_ = true;
    pb_set_color_format(mode.bpp == 16 ? NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5 : NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, false);
    if (const int status = moonlight_pb_init_pvideo_scanout(mode.height >= 1080); status != 0) {
      *error = "GPU scanout initialization failed (pbkit " + std::to_string(status) + ")";
      return false;
    }
    surfaceActive_ = true;
    if (pb_back_buffer_pitch() != static_cast<unsigned int>(pitch)) {
      *error = "GPU scanout pitch does not match the active display mode";
      return false;
    }
    // Keep SDL's CPU framebuffer outside the GPU render targets. Rendering
    // into it would leave a cached GPU surface over later CPU menu updates.
    pb_fill(0, 0, mode.width, mode.height, mode.bpp == 16 ? 0U : 0xff000000U);
    if (!wait_for_gpu_idle()) {
      *error = "Timed out preparing the GPU scanout surface";
      return false;
    }
    return true;
#else
    *error = "GPU scanout is unavailable on this platform";
    return false;
#endif
  }

  bool PvideoPresenter::reset() {
    bool synchronized = true;
#ifdef NXDK
    if (active_) {
      VIDEOREG(VIDEO_STOP) = 1;
      VIDEOREG(VIDEO_BUFFER) = 0;
      synchronized = wait_for_scanout_after(pb_get_vbl_counter());
      if (!synchronized) {
        auto &quarantine = overlay_quarantine();
        quarantine.disabled = true;
        quarantine.buffers = buffers_;
        buffers_.fill(nullptr);
      }
    }
    for (auto *&buffer : buffers_) {
      if (buffer != nullptr) {
        MmFreeContiguousMemory(buffer);
        buffer = nullptr;
      }
    }
    if (surfaceActive_) {
      if (wait_for_gpu_idle()) {
        pb_kill();
      } else {
        overlay_quarantine().disabled = true;
        synchronized = false;
      }
      surfaceActive_ = false;
    }
    if (scanoutStateSaved_) {
      for (std::size_t index = 0; index < SAVED_REGISTERS.size(); ++index) {
        VIDEOREG(SAVED_REGISTERS[index]) = savedRegisters_[index];
      }
      const auto crtcIndex = VIDEOREG8(0x6013d4);
      for (std::size_t index = 0; index < CRTC_REGISTERS.size(); ++index) {
        VIDEOREG8(0x6013d4) = CRTC_REGISTERS[index];
        VIDEOREG8(0x6013d5) = savedCrtcRegisters_[index];
      }
      VIDEOREG8(0x6013d4) = crtcIndex;
      VIDEOREG(0x600804) = savedCrtcConfig_;
      // pb_kill disables the TV encoder; the CPU framebuffer needs output
      // explicitly re-enabled even when the video mode has not changed.
      XVideoSetVideoEnable(true);
      scanoutStateSaved_ = false;
    }
#endif
    active_ = false;
    bufferSize_ = 0;
    bufferReuseVblank_ = 0;
    nextBuffer_ = 0;
    return synchronized;
  }

#ifdef NXDK
  void PvideoPresenter::configure_overlay_buffers(std::size_t required, int pitch, std::string &failure) {
    if (!reset()) {
      failure = "GPU overlay teardown timed out";
      return;
    }
    if ((VIDEOREG(VIDEO_BUFFER) & 0x11U) != 0) {
      failure = "Another owner is using the GPU overlay";
      return;
    }
    if (!initialize_scanout_surface(&failure)) {
      return;
    }
    // Bound the extra contiguous allocation to a 720p YUY2 frame and
    // leave smaller scanout modes and memory-constrained 1080i double-buffered.
    constexpr std::size_t MAX_SPARE_BYTES = 1280U * 720U * 2U + 64U;
    const int outputHeight = XVideoGetMode().height;
    const bool allowSpare = outputHeight == 720 && required <= MAX_SPARE_BYTES;
    const std::size_t requiredBufferCount = buffers_.size() - 1;
    const std::size_t bufferCount = allowSpare ? buffers_.size() : requiredBufferCount;
    decltype(MmGetPhysicalAddress(nullptr)) highestAddress = 0;
    for (std::size_t index = 0; index < bufferCount; ++index) {
      auto *&buffer = buffers_[index];
      buffer = static_cast<std::uint8_t *>(MmAllocateContiguousMemoryEx(required, 0, 0x03ffffff, 64, PAGE_READWRITE | PAGE_WRITECOMBINE));
      if (buffer == nullptr) {
        if (index < requiredBufferCount) {
          failure = "Not enough contiguous memory for GPU presentation";
          return;
        }
        break;
      }
      std::memset(buffer, 0, required);
      highestAddress = std::max(highestAddress, MmGetPhysicalAddress(buffer));
    }
    if (outputHeight == 1080) {
      // xemu is sensitive to the initial GPU buffer layout. Keep depth
      // allocated until the background and YUY2 buffers are ready.
      moonlight_pb_detach_depth_stencil();
      if (!wait_for_gpu_idle()) {
        failure = "Timed out detaching the GPU depth surface";
        return;
      }
      moonlight_pb_release_detached_depth_stencil();
    }
    bufferSize_ = required;
    pitch_ = pitch;
    // xemu reads the descriptor concurrently with MMIO writes. Keep a
    // single DMA window for all buffers so a new address paired with
    // the previous frame's limit is still within bounds.
    VIDEOREG(0x8900) = 0;
    VIDEOREG(0x8908) = static_cast<std::uint32_t>(highestAddress + bufferSize_ - 1U);
    VIDEOREG(0x8140) = 0;
    VIDEOREG(0x8100) = 0x11;
    VIDEOREG(0x8088) = (VIDEOREG(0x8088) & 0xf43ff43fU) | 0x04000400U;
    VIDEOREG(0x808c) = (VIDEOREG(0x808c) & 0xf40ff40fU) | 0x04000400U;
  }

  void PvideoPresenter::prepare_overlay_frame(const Yuv420FrameView &frame, const VideoRectangle &destination, VideoOverlay *overlay, std::string &failure) {
    // The previous submission must cross a vblank before the old scanout
    // buffer can be reused. With a spare, pack before waiting for that latch.
    const bool hasSpare = buffers_.back() != nullptr;
    if (active_ && !hasSpare && !wait_for_scanout_after(bufferReuseVblank_)) {
      failure = "Timed out waiting for GPU scanout; using software presentation";
    } else if (!pack_overlay_yuy2(frame, buffers_[nextBuffer_], bufferSize_, pitch_)) {
      failure = "Failed to pack the GPU overlay frame";
    } else if (overlay != nullptr && !overlay->draw_yuy2(buffers_[nextBuffer_], bufferSize_, pitch_, frame.width, frame.height, destination, frame.matrix)) {
      failure = "Could not compose stream controls with GPU presentation";
    } else if (active_ && hasSpare && !wait_for_scanout_after(bufferReuseVblank_)) {
      // The single overlay bank still holds the pending request until vblank.
      failure = "Timed out waiting for GPU scanout; using software presentation";
    }
  }

  bool PvideoPresenter::program_overlay_registers(const Yuv420FrameView &frame, const VideoRectangle &destination, const OverlaySamplingRegion &sampling) const {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    // Rotate memory buffers through overlay bank zero. xemu composes bank
    // zero only; alternating hardware overlay banks hides every other frame.
    constexpr std::uint32_t offset = 0;
    const auto physical = static_cast<std::uint32_t>(MmGetPhysicalAddress(buffers_[nextBuffer_]));
    VIDEOREG(0x8910 + offset) = 0x1000;
    VIDEOREG(0x8918 + offset) = 0x1000;
    VIDEOREG(0x8920 + offset) = physical;
    // SIZE_IN uses luma pixels; YUY2's two-pixel packing affects byte pitch only.
    VIDEOREG(0x8928 + offset) = static_cast<std::uint32_t>(frame.width) | (static_cast<std::uint32_t>(frame.height) << 16U);
    VIDEOREG(0x8930 + offset) = static_cast<std::uint32_t>(sampling.xHalfPel * 8) | (static_cast<std::uint32_t>(sampling.yHalfPel * 4) << 17U);
    VIDEOREG(0x8938 + offset) = scaling_increment(sampling.width, destination.width);
    VIDEOREG(0x8940 + offset) = scaling_increment(sampling.height, destination.height);
    VIDEOREG(0x8948 + offset) = static_cast<std::uint32_t>(destination.x) | (static_cast<std::uint32_t>(destination.y) << 16U);
    VIDEOREG(0x8950 + offset) = static_cast<std::uint32_t>(destination.width) | (static_cast<std::uint32_t>(destination.height) << 16U);
    const std::uint32_t format = static_cast<std::uint32_t>(pitch_) | (1U << 16U) | (frame.matrix == VideoColorMatrix::bt709 ? 1U << 24U : 0U);
    VIDEOREG(0x8958 + offset) = format;
    return VIDEOREG(0x8958 + offset) == format && VIDEOREG(0x8920 + offset) == physical;
  }

  bool PvideoPresenter::submit_overlay() {
    const bool firstSubmission = !active_;
    VIDEOREG(VIDEO_STOP) = 0;
    VIDEOREG(VIDEO_BUFFER) = 1U;
    active_ = true;
    // Sample after the request so a blank during register setup cannot
    // retire the old buffer prematurely.
    bufferReuseVblank_ = pb_get_vbl_counter();
    if (firstSubmission) {
      if (!wait_for_scanout_after(bufferReuseVblank_)) {
        return false;
      }
      // Switch the background at the blank so letterbox bars become black with video already ready.
      VIDEOREG(CRTC_START) = static_cast<std::uint32_t>(MmGetPhysicalAddress(pb_back_buffer()));
    }
    ++nextBuffer_;
    if (nextBuffer_ == buffers_.size() || buffers_[nextBuffer_] == nullptr) {
      nextBuffer_ = 0;
    }
    return true;
  }
#endif

  bool PvideoPresenter::present(const Yuv420FrameView &frame, const VideoRectangle &destination, std::string *error, const OverlaySamplingRegion &sampling, VideoOverlay *overlay) {
    std::string failure = overlay_frame_error(frame);
    auto source = sampling;
    source.width = sampling.width == 0 ? frame.width : sampling.width;
    source.height = sampling.height == 0 ? frame.height : sampling.height;
    if (failure.empty() && (destination.x < 0 || destination.y < 0 || destination.width < 2 || destination.height < 2 || destination.width > 4095 || destination.height > 4095 || destination.x > 4095 - destination.width || destination.y > 4095 - destination.height)) {
      failure = "Output rectangle is outside the GPU overlay limits";
    }
    if (failure.empty() && (source.width < 2 || source.height < 2 || sampling.xHalfPel < 0 || sampling.yHalfPel < 0 || static_cast<std::int64_t>(sampling.xHalfPel) + 2LL * (source.width - 1) + (sampling.xHalfPel & 1) > 2LL * (frame.width - 1) || static_cast<std::int64_t>(sampling.yHalfPel) + 2LL * (source.height - 1) + (sampling.yHalfPel & 1) > 2LL * (frame.height - 1))) {
      failure = "Source sampling region exceeds the GPU input buffer";
    }
#ifdef NXDK
    if (failure.empty() && overlay_quarantine().disabled) {
      failure = "GPU overlay disabled after a scanout timeout";
    }
    const int pitch = failure.empty() ? (frame.width * 2 + 63) & ~63 : 0;
    // Retain one cache line beyond the visible rows for fetch padding and an
    // inclusive DMA limit that also satisfies xemu's exclusive-end check.
    if (const std::size_t required = failure.empty() ? static_cast<std::size_t>(pitch) * static_cast<std::size_t>(frame.height) + 64U : 0; failure.empty() && (!active_ || required != bufferSize_ || pitch != pitch_)) {
      configure_overlay_buffers(required, pitch, failure);
    }
    if (failure.empty()) {
      prepare_overlay_frame(frame, destination, overlay, failure);
    }
    if (failure.empty() && !program_overlay_registers(frame, destination, source)) {
      failure = "GPU overlay register verification failed";
    }
    if (failure.empty()) {
      if (submit_overlay()) {
        return true;
      }
      failure = "Timed out waiting for the first GPU frame; using software presentation";
    }
#else
    if (failure.empty()) {
      failure = "GPU overlay is unavailable on this platform";
    }
#endif
    reset();
    if (error != nullptr) {
      *error = failure;
    }
    return false;
  }
}  // namespace streaming
