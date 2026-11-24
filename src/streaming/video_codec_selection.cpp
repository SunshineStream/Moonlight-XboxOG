/**
 * @file src/streaming/video_codec_selection.cpp
 * @brief Maps client codec preferences to shared decoder capability bits.
 */
#include "src/streaming/video_codec_selection.h"

#include <Limelight.h>

namespace streaming {
  int portable_video_formats(int decoderFormats, int serverCodecModeSupport) {
    int hostFormats = VIDEO_FORMAT_H264;
    if ((serverCodecModeSupport & SCM_HEVC) != 0) {
      hostFormats |= VIDEO_FORMAT_H265;
    }
    if ((serverCodecModeSupport & SCM_AV1_MAIN8) != 0) {
      hostFormats |= VIDEO_FORMAT_AV1_MAIN8;
    }
    return decoderFormats & hostFormats;
  }

  int supported_video_formats(app::VideoCodecSelection selection, int serverCodecModeSupport) {
    constexpr int ALL_VIDEO_FORMATS = VIDEO_FORMAT_H264 | VIDEO_FORMAT_MPEG2 | VIDEO_FORMAT_H263P;
    switch (selection) {
      case app::VideoCodecSelection::preferMpeg2:
        return (serverCodecModeSupport & SCM_MPEG2) != 0 ? VIDEO_FORMAT_MPEG2 : ALL_VIDEO_FORMATS;
      case app::VideoCodecSelection::h264:
        return VIDEO_FORMAT_H264;
      case app::VideoCodecSelection::mpeg2:
        return VIDEO_FORMAT_MPEG2;
      case app::VideoCodecSelection::h263p:
        return VIDEO_FORMAT_H263P;
      case app::VideoCodecSelection::autoDetect:
      default:
        return ALL_VIDEO_FORMATS;
    }
  }
}  // namespace streaming
