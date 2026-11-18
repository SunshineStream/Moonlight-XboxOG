/**
 * @file src/streaming/audio_player.h
 * @brief Declares the FFmpeg Opus and SDL playback path shared by streaming and the audio probe.
 */
#pragma once

// standard includes
#include <string>

// lib includes
#include <SDL.h>

// local includes
#include "src/streaming/aligned_media_buffer.h"
#include "src/streaming/audio_configuration.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
struct _OPUS_MULTISTREAM_CONFIGURATION;

namespace streaming {

  /**
   * @brief Decode one Moonlight audio stream and queue its local mix to SDL.
   *
   * The transport configures playback before starting and releases the decoder
   * after joining its audio thread.
   */
  class AudioPlayer {
  public:
    /** @brief Construct an idle player. */
    AudioPlayer() = default;
    /** @brief Close the playback device and release decoder resources. */
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    /**
     * @brief Set playback preferences before initialization.
     * @param mode How to mix the selected stream to the Xbox output.
     * @param enabled Whether to decode and queue audio.
     */
    void configure(AudioMode mode, bool enabled);

    /**
     * @brief Check whether playback was enabled for this session.
     * @return True if streaming audio should be decoded.
     */
    bool enabled() const;

    /**
     * @brief Prepare the decoder and open the stereo SDL device.
     * @param audioConfiguration Moonlight stream layout.
     * @param opus Negotiated codec parameters.
     * @return Zero on success, including when playback is disabled.
     */
    int initialize(int audioConfiguration, const _OPUS_MULTISTREAM_CONFIGURATION *opus);

    /** @brief Start SDL audio playback with an empty queue. */
    void start() const;

    /** @brief Pause playback. */
    void stop() const;

    /** @brief Release playback and codec resources, retaining the configured preferences. */
    void cleanup();

    /**
     * @brief Decode and queue a Moonlight Opus payload.
     * @param data Opus payload, or null for a lost packet.
     * @param length Payload size in bytes.
     * @return Number of PCM bytes queued.
     */
    Uint32 decode_and_queue(const char *data, int length);

    /**
     * @brief Inspect the SDL queue for playback diagnostics.
     * @return Pending PCM bytes, or zero if the device is closed.
     */
    Uint32 pending_bytes() const;

    /**
     * @brief Describe the configured audio path and its queue for session diagnostics.
     * @return Text describing disabled, idle or active playback.
     */
    std::string describe_playback() const;

  private:
    /**
     * @brief Create the SDL PCM converter for the decoder's fixed Opus format.
     * @return True when ready to convert frames.
     */
    bool initialize_resampler();

    /**
     * @brief Convert the current decoded frame and queue it with bounded latency.
     * @return PCM bytes queued, or zero on failure.
     */
    Uint32 queue_decoded_frame();

    AudioMode mode_ = AudioMode::stereo;  ///< Selected local mix.
    bool enabled_ = false;  ///< Whether the session plays audio.
    AVCodecContext *codec_ = nullptr;  ///< Open Opus decoder.
    SwrContext *resampler_ = nullptr;  ///< Stereo S16 format converter.
    AVFrame *frame_ = nullptr;  ///< Reusable decoded Opus frame.
    AVPacket *packet_ = nullptr;  ///< Reusable encoded Opus packet.
    SDL_AudioDeviceID device_ = 0;  ///< Open SDL device, or zero.
    SDL_AudioSpec output_ {};  ///< SDL queue format.
    AlignedMediaBuffer pcm_;  ///< Aligned PCM mix buffer.
  };

}  // namespace streaming
