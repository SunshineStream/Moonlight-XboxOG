/**
 * @file src/streaming/aligned_media_buffer.h
 * @brief Provides aligned, padded storage for buffers passed to FFmpeg SIMD routines.
 */
#pragma once

#include "src/_nxdk_compat/ffmpeg_memory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>

namespace streaming {
  /**
   * @brief Owns byte storage with a 64-byte-aligned start and zeroed read padding.
   *
   * Moving a buffer transfers its allocation and leaves the source empty.
   */
  class AlignedMediaBuffer {
  public:
    static constexpr std::size_t PADDING = 64;  ///< FFmpeg's required input overread padding.

    /**
     * @brief Resize storage, discarding prior payload contents and zeroing trailing read padding.
     *
     * @param size Required payload bytes.
     */
    void resize(std::size_t size) {
      if (size > std::numeric_limits<std::size_t>::max() - PADDING) {
        std::abort();
      }
      const auto required = size + PADDING;
      if (!storage_ || required > capacity_) {
        auto *allocation = static_cast<std::uint8_t *>(moonlight_ffmpeg_aligned_malloc(required, ALIGNMENT));
        if (allocation == nullptr) {
          std::abort();
        }
        storage_.reset(allocation);
        capacity_ = required;
      }
      used_ = required;
      std::fill_n(storage_.get() + size, PADDING, std::uint8_t {});
    }

    /**
     * @brief Return the aligned payload address.
     * @return Payload storage, or nullptr when no payload has been prepared.
     */
    std::uint8_t *data() {
      return used_ == 0 ? nullptr : storage_.get();
    }

    /**
     * @brief Return the aligned read-only payload address.
     * @return Payload storage, or nullptr when no payload has been prepared.
     */
    const std::uint8_t *data() const {
      return used_ == 0 ? nullptr : storage_.get();
    }

    /**
     * @brief Return the payload size excluding padding.
     * @return Usable byte count.
     */
    std::size_t size() const {
      return data() == nullptr ? 0 : used_ - PADDING;
    }

    /**
     * @brief Clear the payload while retaining reusable allocation capacity.
     */
    void clear() {
      used_ = 0;
    }

  private:
    static constexpr std::size_t ALIGNMENT = 64;  ///< Alignment required by FFmpeg SIMD routines.
    std::unique_ptr<std::uint8_t, decltype(&moonlight_ffmpeg_aligned_free)> storage_ {nullptr, &moonlight_ffmpeg_aligned_free};  ///< Owned aligned storage.
    std::size_t capacity_ = 0;  ///< Allocated bytes, meaningful while storage is owned.
    std::size_t used_ = 0;  ///< Payload and padding bytes, or zero when cleared.
  };
}  // namespace streaming
