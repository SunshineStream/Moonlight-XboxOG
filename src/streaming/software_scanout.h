/**
 * @file src/streaming/software_scanout.h
 * @brief Buffered Xbox scanout for CPU-composited video.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>

// local includes
#include "src/streaming/video_acceleration.h"

namespace streaming {
  /**
   * @brief Display CPU-composited frames using page flips in SD and a fixed scanout in HD.
   *
   * The active video mode must remain fixed until reset. All operations belong
   * to the stream render thread.
   */
  class SoftwareScanout {
  public:
    SoftwareScanout() = default;
    /**
     * @brief Restore the nxdk scanout before releasing the extra page.
     */
    ~SoftwareScanout();
    SoftwareScanout(const SoftwareScanout &) = delete;
    SoftwareScanout &operator=(const SoftwareScanout &) = delete;

    /**
     * @brief Get a buffer outside active scanout and clear pixels outside the new video area.
     * @param size Framebuffer size in bytes for the current display mode.
     * @param destination Video area that will be completely redrawn.
     * @return Writable frame storage, or nullptr when unavailable.
     */
    std::uint8_t *back_buffer(std::size_t size, const VideoRectangle &destination);

    /**
     * @brief Display the completed frame by flipping pages or copying to the primary framebuffer.
     * @return True when presentation succeeded and the drawing buffer is available again.
     */
    bool present();

    /**
     * @brief Restore nxdk scanout and release the extra framebuffer.
     * @return True if the framebuffer could be safely released.
     */
    bool reset();

  private:
    std::array<std::uint8_t *, 2> pages_ {};  ///< nxdk's primary framebuffer and the additional drawing buffer.
    std::array<VideoRectangle, 2> destinations_ {};  ///< Video area last drawn on each page.
    std::unique_ptr<std::byte, decltype(&std::free)> staging_ {nullptr, &std::free};  ///< Owns HD staging storage before alignment.
    std::size_t stagingSize_ = 0;  ///< Bytes to copy from the staged frame to scanout.
    std::size_t front_ = 0;  ///< Page most recently submitted to scanout.
    bool synchronized_ = true;  ///< A completed vblank has retired the previous page.
  };
}  // namespace streaming
