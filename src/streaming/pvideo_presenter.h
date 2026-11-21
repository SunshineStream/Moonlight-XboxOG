/**
 * @file src/streaming/pvideo_presenter.h
 * @brief Owns the NV2A overlay independently of SDL framebuffer rendering.
 */
#pragma once

// local includes
#include "src/streaming/video_acceleration.h"
#include "src/streaming/video_overlay.h"

namespace streaming {
  /**
   * @brief Optional source region for isolated half-pixel prediction experiments.
   */
  struct OverlaySamplingRegion {
    int xHalfPel = 0;  ///< Horizontal source origin in half-pixel units.
    int yHalfPel = 0;  ///< Vertical source origin in half-pixel units.
    int width = 0;  ///< Source extent; zero selects the whole frame.
    int height = 0;  ///< Source extent; zero selects the whole frame.
  };

  /**
   * @brief Main-thread-only NV2A YUV overlay presenter with an optional spare buffer at 720p.
   *
   * Supports progressive limited-range BT.601/BT.709 YUV420 with a compatible
   * 16-bit or 32-bit output mode. Callers select software presentation when present()
   * returns false. Successful register submission does not verify the image
   * displayed by the emulator or console.
   *
   * @see https://github.com/abaire/nxdk_pgraph_tests/blob/main/src/pvideo_control.cpp
   * @see https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/gl/display.c
   */
  class PvideoPresenter {
  public:
    /**
     * @brief Construct an inactive overlay owner.
     */
    PvideoPresenter() = default;
    /**
     * @brief Stop overlay DMA before releasing its contiguous buffers.
     */
    ~PvideoPresenter();
    PvideoPresenter(const PvideoPresenter &) = delete;
    PvideoPresenter &operator=(const PvideoPresenter &) = delete;

    /**
     * @brief Pack and submit one frame, or return an explicit fallback reason.
     *
     * The startup background stays visible until the first overlay has latched.
     *
     * @param frame Source YUV420 data.
     * @param destination Letterboxed output rectangle.
     * @param error Receives the reason acceleration could not be used.
     * @param sampling Optional crop used by the standalone prediction experiment.
     * @param overlay Optional cached UI or statistics, composited using the frame's color matrix before GPU scanout.
     *
     * @return True after the first overlay frame has latched or a subsequent frame has been submitted.
     */
    bool present(const Yuv420FrameView &frame, const VideoRectangle &destination, std::string *error, const OverlaySamplingRegion &sampling = {}, VideoOverlay *overlay = nullptr);

    /**
     * @brief Stop DMA and release buffers only after a bounded scanout synchronization.
     *
     * @return False when buffers had to be quarantined after a scanout timeout.
     */
    bool reset();

    /**
     * @brief Return whether the overlay is presenting video.
     * @return True after the first successful presentation until reset.
     */
    bool active() const;

  private:
    /**
     * @brief Prepare a black GPU background while the current CPU framebuffer remains visible.
     *
     * @param error Receives an initialization or GPU completion failure.
     * @return True after the background clear has completed.
     */
    bool initialize_scanout_surface(std::string *error);

#ifdef NXDK
    /**
     * @brief Replace the scanout surface and allocate the required buffers plus an optional 720p spare.
     * @param required Bytes per buffer including DMA padding.
     * @param pitch Packed bytes per row.
     * @param failure Receives the reason setup failed, if any.
     */
    void configure_overlay_buffers(std::size_t required, int pitch, std::string &failure);

    /**
     * @brief Pack the picture and optional UI, waiting for scanout before reuse or submission as needed.
     * @param frame Decoded video to pack.
     * @param destination Visible output rectangle used for UI placement.
     * @param overlay Optional UI to composite into the packed video.
     * @param failure Receives an explicit fallback reason if preparation fails.
     */
    void prepare_overlay_frame(const Yuv420FrameView &frame, const VideoRectangle &destination, VideoOverlay *overlay, std::string &failure);

    /**
     * @brief Program and verify the registers for the next packed picture.
     * @param frame Video frame whose pixels are stored in the DMA buffer.
     * @param destination Visible output rectangle.
     * @param sampling Validated source region with explicit width and height.
     * @return True if the selected address and format can be read back.
     */
    bool program_overlay_registers(const Yuv420FrameView &frame, const VideoRectangle &destination, const OverlaySamplingRegion &sampling) const;

    /**
     * @brief Submit the prepared overlay and switch the startup background when the first frame latches.
     * @return True if the submission succeeded or false if the first latch timed out.
     */
    bool submit_overlay();
#endif

    std::array<std::uint8_t *, 3> buffers_ {};  ///< Two contiguous write-combined YUY2 buffers and an optional spare.
    std::array<std::uint32_t, 28> savedRegisters_ {};  ///< Overlay state restored on teardown.
    std::array<std::uint8_t, 4> savedCrtcRegisters_ {};  ///< Legacy scanout controls changed by pbkit.
    std::uint32_t savedCrtcConfig_ = 0;  ///< Scanout start-address latching mode before pbkit.
    bool scanoutStateSaved_ = false;  ///< Legacy scanout and overlay state must be restored, including initialization failures.
    std::size_t bufferSize_ = 0;  ///< Allocated size of each DMA buffer.
    int pitch_ = 0;  ///< Packed source stride.
    std::uint32_t bufferReuseVblank_ = 0;  ///< Vblank counter that must advance before replacing the pending request.
    unsigned int nextBuffer_ = 0;  ///< Next buffer to pack once it is safe to write.
    bool active_ = false;  ///< An overlay request is active and must be retired before freeing buffers.
    bool surfaceActive_ = false;  ///< pbkit owns the GPU context and vblank counter for the scanout surface.
  };
}  // namespace streaming
