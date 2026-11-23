/**
 * @file tests/unit/streaming/video_codec_selection_test.cpp
 * @brief Checks automatic capabilities and strict codec requests.
 */
// test header include
#include "src/streaming/video_codec_selection.h"

// lib includes
#include <Limelight.h>
#include <lizardbyte/common/testing.h>

/**
 * @brief Automatic mode advertises every decoder implemented by this client.
 */
TEST(VideoCodecSelectionTest, AutomaticModeAdvertisesSupportedCodecs) {
  const int formats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_MPEG2 | VIDEO_FORMAT_H263P;
  EXPECT_EQ(streaming::supported_video_formats(app::VideoCodecSelection::autoDetect, SCM_H264 | SCM_MPEG2), formats);
  EXPECT_EQ(streaming::supported_video_formats(static_cast<app::VideoCodecSelection>(-1), 0), formats);
}

/** @brief The default tries MPEG-2 on advertising hosts and preserves negotiation with other hosts. */
TEST(VideoCodecSelectionTest, Mpeg2PreferenceRequiresHostSupport) {
  const auto preference = app::VideoCodecSelection::preferMpeg2;
  const int formats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_MPEG2 | VIDEO_FORMAT_H263P;
  EXPECT_EQ(streaming::supported_video_formats(preference, SCM_H264 | SCM_MPEG2 | SCM_H263P), VIDEO_FORMAT_MPEG2);
  EXPECT_EQ(streaming::supported_video_formats(preference, SCM_H264 | SCM_H263P), formats);
  EXPECT_EQ(streaming::supported_video_formats(preference, 0), formats);
}

/**
 * @brief Explicit choices cannot negotiate a different codec as a fallback.
 */
TEST(VideoCodecSelectionTest, ExplicitChoicesAdvertiseOnlyTheRequestedCodec) {
  EXPECT_EQ(streaming::supported_video_formats(app::VideoCodecSelection::h264, SCM_MPEG2), VIDEO_FORMAT_H264);
  EXPECT_EQ(streaming::supported_video_formats(app::VideoCodecSelection::mpeg2, SCM_H264), VIDEO_FORMAT_MPEG2);
  EXPECT_EQ(streaming::supported_video_formats(app::VideoCodecSelection::h263p, SCM_H264), VIDEO_FORMAT_H263P);
}
