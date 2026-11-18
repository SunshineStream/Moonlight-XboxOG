/**
 * @file src/streaming/audio_configuration.h
 * @brief Defines the supported Xbox audio modes.
 */
#pragma once

// standard includes
#include <optional>
#include <string_view>

namespace streaming {

  /** @brief Select the mix played through the Xbox stereo device. */
  enum class AudioMode {
    stereo,  ///< Play the stereo stream as received.
    mono,  ///< Mix stereo to mono and play it through both channels.
  };

  /**
   * @brief Get the display label for an audio mode.
   * @param mode Audio mode.
   * @return Label shown in settings.
   */
  const char *audio_mode_label(AudioMode mode);

  /**
   * @brief Get the stable TOML value for an audio mode.
   * @param mode Audio mode.
   * @return Value written to the settings file.
   */
  const char *audio_mode_key(AudioMode mode);

  /**
   * @brief Parse a saved audio mode.
   * @param key Value read from the settings file.
   * @return Matching mode, or no value for an unknown key.
   */
  std::optional<AudioMode> parse_audio_mode(std::string_view key);

  /**
   * @brief Advance to the next audio mode in settings.
   * @param mode Current audio mode.
   * @return Next audio mode, wrapping back to stereo.
   */
  AudioMode next_audio_mode(AudioMode mode);

}  // namespace streaming
