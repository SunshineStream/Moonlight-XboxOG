/**
 * @file experiments/audio_playback_probe.cpp
 * @brief Play isolated Opus channel tests through the production Xbox audio path.
 */

// standard includes
#include <array>
#include <cstdint>

// lib includes
#include <hal/debug.h>
#include <hal/video.h>
#include <Limelight.h>
#include <SDL3/SDL.h>
#include <xboxkrnl/xboxkrnl.h>

// local includes
#include "src/logging/logger.h"
#include "src/streaming/audio_player.h"

// The staged SDL library references Moonlight's USB hooks even when only audio is initialized.
/** @brief Satisfy the SDL joystick initializer's USB hook for this input-free probe. */
extern "C" void moonlight_usb_input_initialize() {
  // This probe initializes no input devices.
}

/** @brief Satisfy the SDL event driver's USB hook for this input-free probe. */
extern "C" void moonlight_usb_input_pump() {
  // This probe has no input events to pump.
}

namespace {

  constexpr int SAMPLE_RATE = 48000;
  constexpr int PACKET_MS = 20;
  constexpr int SAMPLES_PER_PACKET = SAMPLE_RATE * PACKET_MS / 1000;
  constexpr int PLAYBACK_MS = 2000;
  constexpr Uint32 EXPECTED_PCM_BYTES = SAMPLES_PER_PACKET * 2 * sizeof(std::int16_t);

  // Encoded with libopus in restricted-low-delay mode. The 500 Hz tone fills ten
  // periods per packet, which allows a single steady-state packet to be repeated.
  /** @brief 20 ms of Opus with a 500 Hz tone in the front left channel and silence elsewhere. */
  constexpr std::array<std::uint8_t, 61> STEREO_PACKET {
    0x9c,
    0xad,
    0x86,
    0x72,
    0x9f,
    0x5d,
    0xc3,
    0x85,
    0xbd,
    0x8e,
    0x96,
    0x0a,
    0xe5,
    0x21,
    0x42,
    0x41,
    0xd9,
    0x02,
    0x07,
    0xb9,
    0xc4,
    0x38,
    0xe3,
    0xcc,
    0x50,
    0xe7,
    0xad,
    0x77,
    0xd2,
    0xd6,
    0xe1,
    0xd7,
    0x85,
    0x7f,
    0xf2,
    0x66,
    0x98,
    0xf7,
    0x05,
    0xc9,
    0x7f,
    0x79,
    0x5e,
    0x41,
    0xfc,
    0xcd,
    0x01,
    0x3b,
    0x69,
    0x5d,
    0x56,
    0x66,
    0xd6,
    0x95,
    0x57,
    0xa7,
    0x76,
    0xa6,
    0x93,
    0xb5,
    0xe1,
  };

  /**
   * @brief Give the audio device time to consume queued PCM.
   * @param player Playback device under test.
   * @param limit Maximum queued bytes before continuing.
   * @return True if the queue reached the limit before the timeout.
   */
  bool wait_for_queue(const streaming::AudioPlayer &player, Uint32 limit) {
    const Uint64 deadline = SDL_GetTicks() + 2000;
    while (player.pending_bytes() > limit && SDL_GetTicks() < deadline) {
      SDL_Delay(10);
    }
    if (const auto pending = player.pending_bytes(); pending > limit) {
      DbgPrint("AUDIO PROBE QUEUE TIMEOUT: %lu bytes remaining\n", static_cast<unsigned long>(pending));
      return false;
    }
    return true;
  }

  /**
   * @brief Exercise the production decoder and wait for SDL to consume the queued PCM.
   * @param player Reusable audio player.
   * @param mode Audio mode under test.
   * @param expected Description of the expected audible output.
   * @return True if the decoder queued the expected amount of PCM and playback consumed it.
   */
  bool play_test(streaming::AudioPlayer &player, streaming::AudioMode mode, const char *expected) {
    const OPUS_MULTISTREAM_CONFIGURATION opus {SAMPLE_RATE, 2, 1, 1, SAMPLES_PER_PACKET, {0, 1}};

    const char *label = streaming::audio_mode_label(mode);
    debugPrint("%s: %s\n", label, expected);
    DbgPrint("AUDIO PROBE START %s: %s\n", label, expected);
    player.configure(mode, true);
    if (player.initialize(AUDIO_CONFIGURATION_STEREO, &opus) != 0) {
      return false;
    }
    player.start();
    for (int elapsed = 0; elapsed < PLAYBACK_MS; elapsed += PACKET_MS) {
      // Pace the probe from the device queue so emulator clock differences cannot backlog it.
      if (!wait_for_queue(player, 3 * EXPECTED_PCM_BYTES)) {
        return false;
      }
      const auto queued = player.decode_and_queue(reinterpret_cast<const char *>(STEREO_PACKET.data()), static_cast<int>(STEREO_PACKET.size()));
      if (queued != EXPECTED_PCM_BYTES) {
        DbgPrint("AUDIO PROBE PACKET FAILED: queued %lu bytes\n", static_cast<unsigned long>(queued));
        return false;
      }
    }

    if (!wait_for_queue(player, 0)) {
      return false;
    }

    // Allow the driver to play its last handoff before closing the device.
    SDL_Delay(150);
    player.cleanup();
    DbgPrint("AUDIO PROBE PASSED %s\n", label);
    SDL_Delay(500);
    return true;
  }

}  // namespace

/** @brief Run the stereo and mono playback tests and leave the result on screen. */
int main() {
  XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
  logging::set_debugger_console_minimum_level(logging::LogLevel::warning);
  debugPrint("OPUS AUDIO PLAYBACK PROBE\n");
  SDL_Delay(1000);

  using streaming::AudioMode;
  streaming::AudioPlayer player;
  const bool passed = play_test(player, AudioMode::stereo, "500 Hz on left") && play_test(player, AudioMode::mono, "500 Hz on both");
  player.cleanup();
  const char *result = passed ? "AUDIO PROBE COMPLETE" : "AUDIO PROBE FAILED";
  debugPrint("%s\n", result);
  DbgPrint("%s\n", result);
  for (;;) {
    SDL_Delay(250);
  }
}
