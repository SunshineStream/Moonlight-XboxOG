/**
 * @file src/streaming/video_codec_selection.h
 * @brief Declares the codec capabilities associated with client preferences.
 */
#pragma once

#include "src/app/client_state.h"

namespace streaming {
  /**
   * @brief Intersect the portable decoder set with the host's SDR codecs.
   * @param decoderFormats Available Moonlight client format flags.
   * @param serverCodecModeSupport Codec capability mask advertised by the host.
   * @return The negotiable SDR video formats.
   */
  int portable_video_formats(int decoderFormats, int serverCodecModeSupport);

  /**
   * @brief Select the decoder capability mask for the initial negotiation attempt.
   *
   * @param selection Codec preference selected by the user.
   * @param serverCodecModeSupport Codec capability mask advertised by the host.
   * @return Moonlight video format mask used for negotiation.
   */
  int supported_video_formats(app::VideoCodecSelection selection, int serverCodecModeSupport);
}  // namespace streaming
