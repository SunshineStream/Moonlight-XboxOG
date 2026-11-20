/**
 * @file src/streaming/audio_player.cpp
 * @brief Implements Opus decoding, mixing and queued SDL playback.
 */
// class header include
#include "src/streaming/audio_player.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstring>

// lib includes
#include "third-party/moonlight-common-c/src/Limelight.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

// local includes
#include "src/logging/logger.h"

namespace {

  constexpr int OUTPUT_SAMPLE_RATE = 48000;
  constexpr int OUTPUT_CHANNELS = 2;
  constexpr Uint32 QUEUE_LIMIT_MS = 250;

  /**
   * @brief Convert an FFmpeg error code to a log message.
   * @param code Negative FFmpeg error code.
   * @return Readable error message.
   */
  std::string ffmpeg_error(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer {};
    av_strerror(code, buffer.data(), buffer.size());
    return buffer.data();
  }

}  // namespace

namespace streaming {

  AudioPlayer::~AudioPlayer() {
    cleanup();
  }

  void AudioPlayer::configure(AudioMode mode, bool enabled) {
    mode_ = mode;
    enabled_ = enabled;
  }

  bool AudioPlayer::enabled() const {
    return enabled_;
  }

  int AudioPlayer::initialize(int audioConfiguration, const _OPUS_MULTISTREAM_CONFIGURATION *opus) {
    cleanup();
    if (!enabled_) {
      return 0;
    }

    if (audioConfiguration != AUDIO_CONFIGURATION_STEREO || opus == nullptr || opus->channelCount != OUTPUT_CHANNELS || opus->sampleRate != OUTPUT_SAMPLE_RATE || opus->streams != 1 || opus->coupledStreams != 1) {
      logging::error("stream", "Invalid stereo Opus configuration");
      return -1;
    }

    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (decoder == nullptr) {
      logging::error("stream", "FFmpeg did not provide an Opus decoder");
      return -1;
    }
    codec_ = avcodec_alloc_context3(decoder);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (codec_ == nullptr || frame_ == nullptr || packet_ == nullptr) {
      logging::error("stream", "Failed to allocate FFmpeg audio decoder resources");
      cleanup();
      return -1;
    }
    // OpusHead for 48 kHz stereo, mapping family 0, with no pre-skip or gain.
    constexpr std::array<std::uint8_t, 19> header {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0};
    codec_->extradata = static_cast<std::uint8_t *>(av_mallocz(header.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (codec_->extradata == nullptr) {
      logging::error("stream", "Failed to allocate Opus decoder extradata for FFmpeg");
      cleanup();
      return -1;
    }
    codec_->extradata_size = static_cast<int>(header.size());
    std::memcpy(codec_->extradata, header.data(), header.size());
    if (const int result = avcodec_open2(codec_, decoder, nullptr); result < 0) {
      logging::error("stream", "Failed to open the Opus decoder: " + ffmpeg_error(result));
      cleanup();
      return result;
    }

    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0 && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      logging::error("stream", std::string("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: ") + SDL_GetError());
      cleanup();
      return -1;
    }
    // SDL opens a paused stream. The Xbox backend consumes 48 kHz stereo PCM.
    const SDL_AudioSpec format {SDL_AUDIO_S16, OUTPUT_CHANNELS, OUTPUT_SAMPLE_RATE};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &format, nullptr, nullptr);
    if (stream_ == nullptr) {
      logging::error("stream", std::string("SDL_OpenAudioDeviceStream failed: ") + SDL_GetError());
      cleanup();
      return -1;
    }
    return 0;
  }

  void AudioPlayer::start() const {
    if (stream_ != nullptr) {
      SDL_ClearAudioStream(stream_);
      SDL_ResumeAudioStreamDevice(stream_);
    }
  }

  void AudioPlayer::stop() const {
    if (stream_ != nullptr) {
      SDL_PauseAudioStreamDevice(stream_);
    }
  }

  void AudioPlayer::cleanup() {
    if (stream_ != nullptr) {
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&codec_);
    swr_free(&resampler_);
    pcm_.clear();
  }

  Uint32 AudioPlayer::pending_bytes() const {
    return stream_ == nullptr ? 0U : static_cast<Uint32>(std::max(0, SDL_GetAudioStreamQueued(stream_)));
  }

  std::string AudioPlayer::describe_playback() const {
    if (!enabled_) {
      return "audio decode disabled";
    }
    return stream_ == nullptr ? "audio idle" : std::to_string(pending_bytes()) + " queued audio bytes";
  }

  bool AudioPlayer::initialize_resampler() {
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    int result = swr_alloc_set_opts2(&resampler_, &stereo, AV_SAMPLE_FMT_S16, OUTPUT_SAMPLE_RATE, &frame_->ch_layout, static_cast<AVSampleFormat>(frame_->format), frame_->sample_rate, 0, nullptr);
    if (result >= 0 && mode_ == AudioMode::mono) {
      // Play the mean of left and right on both outputs of the stereo device.
      constexpr std::array dualMono {0.5, 0.5, 0.5, 0.5};
      result = swr_set_matrix(resampler_, dualMono.data(), OUTPUT_CHANNELS);
    }
    if (result >= 0) {
      result = swr_init(resampler_);
    }
    if (result < 0) {
      logging::warn("stream", "Failed to initialize the streaming audio resampler: " + ffmpeg_error(result));
      swr_free(&resampler_);
      return false;
    }
    return true;
  }

  Uint32 AudioPlayer::queue_decoded_frame() {
    if (resampler_ == nullptr && !initialize_resampler()) {
      return 0;
    }

    const int outputSamples = swr_get_out_samples(resampler_, frame_->nb_samples);
    if (outputSamples <= 0) {
      return 0;
    }
    constexpr Uint32 bytesPerFrame = OUTPUT_CHANNELS * sizeof(std::int16_t);
    pcm_.resize(static_cast<std::size_t>(outputSamples) * bytesPerFrame);
    auto *output = pcm_.data();
    const int converted = swr_convert(resampler_, &output, outputSamples, const_cast<const std::uint8_t **>(frame_->extended_data), frame_->nb_samples);
    if (converted <= 0) {
      if (converted < 0) {
        logging::warn("stream", "Failed to convert decoded audio: " + ffmpeg_error(converted));
      }
      return 0;
    }

    const Uint32 bytes = static_cast<Uint32>(converted) * bytesPerFrame;
    if (const Uint32 maxQueuedBytes = OUTPUT_SAMPLE_RATE * bytesPerFrame * QUEUE_LIMIT_MS / 1000U; pending_bytes() + bytes > maxQueuedBytes) {
      SDL_ClearAudioStream(stream_);
    }
    if (!SDL_PutAudioStreamData(stream_, output, static_cast<int>(bytes))) {
      logging::warn("stream", std::string("Failed to queue decoded audio: ") + SDL_GetError());
      return 0;
    }
    return bytes;
  }

  Uint32 AudioPlayer::decode_and_queue(const char *data, int length) {
    if (stream_ == nullptr || data == nullptr || length <= 0) {
      return 0;
    }
    if (const int result = av_new_packet(packet_, length); result < 0) {
      logging::warn("stream", "Failed to allocate an Opus packet: " + ffmpeg_error(result));
      return 0;
    }
    std::memcpy(packet_->data, data, static_cast<std::size_t>(length));
    const int sendResult = avcodec_send_packet(codec_, packet_);
    av_packet_unref(packet_);
    if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
      logging::warn("stream", "Failed to submit an Opus packet: " + ffmpeg_error(sendResult));
      return 0;
    }

    Uint32 queued = 0;
    while (true) {
      const int result = avcodec_receive_frame(codec_, frame_);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return queued;
      }
      if (result < 0) {
        logging::warn("stream", "Failed to decode an Opus frame: " + ffmpeg_error(result));
        return queued;
      }
      queued += queue_decoded_frame();
      av_frame_unref(frame_);
    }
  }

}  // namespace streaming
