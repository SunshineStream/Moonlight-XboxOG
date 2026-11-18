/**
 * @file src/streaming/ffmpeg_video_frame.cpp
 * @brief Transfers FFmpeg frame references without copying pixels.
 */
// class header include
#include "src/streaming/ffmpeg_video_frame.h"

// standard includes
#include <utility>

// lib includes
extern "C" {
#include <libavutil/frame.h>
}

namespace {
  /**
   * @brief Map FFmpeg's color space to the console overlay's color matrix.
   * @param colorspace FFmpeg frame color space.
   * @return Supported overlay matrix or a marker requiring software conversion.
   */
  streaming::VideoColorMatrix overlay_matrix(AVColorSpace colorspace) {
    switch (colorspace) {
      case AVCOL_SPC_BT709:
        return streaming::VideoColorMatrix::bt709;
      case AVCOL_SPC_UNSPECIFIED:
      case AVCOL_SPC_BT470BG:
      case AVCOL_SPC_SMPTE170M:
        return streaming::VideoColorMatrix::bt601;
      default:
        return streaming::VideoColorMatrix::unsupported;
    }
  }

  /**
   * @brief Validate each positive-stride plane against its FFmpeg-owned allocation.
   * @param frame Frame proposed for display.
   * @return Read-only view, or an empty view when the layout cannot be displayed safely.
   */
  streaming::Yuv420FrameView make_view(const AVFrame &frame) {
    if (frame.format != AV_PIX_FMT_YUV420P || frame.width <= 0 || frame.height <= 0) {
      return {};
    }
    streaming::Yuv420FrameView view;
    view.width = frame.width;
    view.height = frame.height;
    view.matrix = overlay_matrix(frame.colorspace);
    view.fullRange = frame.color_range == AVCOL_RANGE_JPEG;
    view.interlaced = (frame.flags & AV_FRAME_FLAG_INTERLACED) != 0;
    for (std::size_t i = 0; i < view.planes.size(); ++i) {
      const int width = i == 0 ? frame.width : frame.width / 2 + frame.width % 2;
      const int height = i == 0 ? frame.height : frame.height / 2 + frame.height % 2;
      if (frame.data[i] == nullptr || frame.linesize[i] < width) {
        return {};
      }
      const AVBufferRef *buffer = av_frame_get_plane_buffer(&frame, static_cast<int>(i));
      if (buffer == nullptr) {
        return {};
      }
      const auto offset = reinterpret_cast<std::uintptr_t>(frame.data[i]) - reinterpret_cast<std::uintptr_t>(buffer->data);
      if (const auto required = static_cast<std::uint64_t>(height - 1) * frame.linesize[i] + width; offset > buffer->size || required > buffer->size - offset) {
        return {};
      }
      view.planes[i] = frame.data[i];
      view.sizes[i] = buffer->size - offset;
      view.pitches[i] = frame.linesize[i];
    }
    return view;
  }
}  // namespace

namespace streaming {
  FfmpegVideoFrame::~FfmpegVideoFrame() {
    av_frame_free(&frame_);
  }

  bool FfmpegVideoFrame::ensure_frame_allocated() {
    if (frame_ == nullptr) {
      frame_ = av_frame_alloc();
    }
    return frame_ != nullptr;
  }

  bool FfmpegVideoFrame::take(AVFrame &source) {
    const auto view = make_view(source);
    if (view.width == 0) {
      return false;
    }
    if (!ensure_frame_allocated()) {
      return false;
    }
    av_frame_unref(frame_);
    av_frame_move_ref(frame_, &source);
    view_ = view;
    return true;
  }

  bool FfmpegVideoFrame::retain(const FfmpegVideoFrame &source) {
    if (this == &source) {
      return view_.width > 0;
    }
    if (source.view_.width == 0) {
      return false;
    }
    if (!ensure_frame_allocated()) {
      return false;
    }
    clear();
    if (av_frame_ref(frame_, source.frame_) < 0) {
      return false;
    }
    view_ = source.view_;
    return true;
  }

  void FfmpegVideoFrame::clear() {
    if (frame_ != nullptr) {
      av_frame_unref(frame_);
    }
    view_ = {};
  }

  void FfmpegVideoFrame::swap(FfmpegVideoFrame &other) noexcept {
    std::swap(frame_, other.frame_);
    std::swap(view_, other.view_);
  }

  const Yuv420FrameView &FfmpegVideoFrame::view() const noexcept {
    return view_;
  }
}  // namespace streaming
