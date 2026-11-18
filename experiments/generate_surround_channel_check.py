"""Generate a 5.1 channel check as PCM WAV and Dolby Digital S/PDIF bursts."""

import argparse
import math
import subprocess
import sys
from array import array
from pathlib import Path

SAMPLE_RATE = 48_000
TONE_FRAMES = SAMPLE_RATE
GAP_FRAMES = SAMPLE_RATE // 2
FADE_FRAMES = SAMPLE_RATE // 100
# FFmpeg's 5.1(side) order is FL, FR, FC, LFE, SL, SR.
CHANNEL_FREQUENCIES = (500, 500, 500, 80, 500, 500)


def channel_check_pcm() -> bytes:
    """Return interleaved signed 16-bit PCM with one active channel per slot."""
    channels = len(CHANNEL_FREQUENCIES)
    slot_frames = TONE_FRAMES + GAP_FRAMES
    total_frames = GAP_FRAMES + slot_frames * channels
    samples = array("h", [0]) * (total_frames * channels)
    for channel, frequency in enumerate(CHANNEL_FREQUENCIES):
        tone = array(
            "h",
            (
                round(
                    6_500
                    * math.sin(math.tau * frequency * frame / SAMPLE_RATE)
                    * min(frame, TONE_FRAMES - 1 - frame, FADE_FRAMES)
                    / FADE_FRAMES
                )
                for frame in range(TONE_FRAMES)
            ),
        )
        first_sample = (GAP_FRAMES + channel * slot_frames) * channels + channel
        end_sample = first_sample + TONE_FRAMES * channels
        samples[first_sample:end_sample:channels] = tone

    if sys.byteorder == "big":
        samples.byteswap()
    return samples.tobytes()


def main() -> None:
    """Write both test files to the requested output directory."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg", help="FFmpeg executable to run")
    args = parser.parse_args()

    args.output_directory.mkdir(parents=True, exist_ok=True)
    wav = args.output_directory / "surround-channel-check.wav"
    spdif = args.output_directory / "surround-channel-check.spdif"
    subprocess.run(
        [
            args.ffmpeg,
            "-hide_banner", "-loglevel", "error", "-y",
            "-f", "s16le", "-ar", str(SAMPLE_RATE), "-channel_layout", "5.1(side)", "-i", "pipe:0",
            "-map", "0:a:0", "-c:a", "pcm_s16le", str(wav),
            "-map", "0:a:0", "-c:a", "ac3_fixed", "-b:a", "448k", "-f", "spdif", str(spdif),
        ],
        input=channel_check_pcm(),
        check=True,
    )
    print(wav)
    print(spdif)


if __name__ == "__main__":
    main()
