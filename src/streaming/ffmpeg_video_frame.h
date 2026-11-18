/**
 * @file src/streaming/ffmpeg_video_frame.h
 * @brief Owns decoded FFmpeg planes across publication, presentation and capture.
 */
#pragma once

// local includes
#include "src/streaming/video_acceleration.h"

struct AVFrame;

namespace streaming {
  /**
   * @brief A reusable slot holding one reference-counted I420 frame and its read-only view.
   *
   * Each slot has a single thread owner. Holding the reference keeps the planes valid until
   * that owner is done, even if the decoder flushes or reuses its receive frame.
   */
  class FfmpegVideoFrame {
  public:
    /** @brief Construct an empty slot. */
    FfmpegVideoFrame() = default;
    /** @brief Release the FFmpeg frame and its buffers. */
    ~FfmpegVideoFrame();
    FfmpegVideoFrame(const FfmpegVideoFrame &) = delete;
    FfmpegVideoFrame &operator=(const FfmpegVideoFrame &) = delete;

    /**
     * @brief Move a validated I420 frame reference from the decoder into this slot.
     * @param source The decoder frame, emptied on success.
     * @return True when the reference was transferred.
     */
    bool take(AVFrame &source);

    /**
     * @brief Retain a separate read-only reference for an asynchronous consumer.
     * @param source Slot whose pixels should remain available.
     * @return True when the reference was acquired.
     */
    bool retain(const FfmpegVideoFrame &source);

    /** @brief Release the held buffers while keeping the reusable FFmpeg frame slot. */
    void clear();

    /**
     * @brief Exchange ownership of two slots without touching the pixel buffers.
     * @param other Slot to exchange with this one.
     */
    void swap(FfmpegVideoFrame &other) noexcept;

    /**
     * @brief Read the frame metadata and planes while this reference is held.
     * @return Frame view, with zero dimensions when empty.
     */
    [[nodiscard]] const Yuv420FrameView &view() const noexcept;

  private:
    AVFrame *frame_ = nullptr;  ///< Lazily allocated frame structure reused on each handoff.
    Yuv420FrameView view_ {};  ///< Bounds and metadata for the buffers owned by frame_.
  };
}  // namespace streaming
