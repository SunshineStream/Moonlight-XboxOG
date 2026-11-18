/**
 * @file src/streaming/audio_configuration.cpp
 * @brief Implements menu labels and persistence for the supported audio modes.
 */
// class header include
#include "src/streaming/audio_configuration.h"

namespace streaming {

  const char *audio_mode_label(AudioMode mode) {
    return mode == AudioMode::mono ? "Mono" : "Stereo";
  }

  const char *audio_mode_key(AudioMode mode) {
    return mode == AudioMode::mono ? "mono" : "stereo";
  }

  std::optional<AudioMode> parse_audio_mode(std::string_view key) {
    if (key == audio_mode_key(AudioMode::mono)) {
      return AudioMode::mono;
    }
    if (key == audio_mode_key(AudioMode::stereo)) {
      return AudioMode::stereo;
    }
    return std::nullopt;
  }

  AudioMode next_audio_mode(AudioMode mode) {
    return mode == AudioMode::stereo ? AudioMode::mono : AudioMode::stereo;
  }

}  // namespace streaming
