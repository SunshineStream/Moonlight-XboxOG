# Xbox experiments

These optional Xbox programs exercise video presentation, fractional sampling and audio playback
independently of a streaming host. They share production code and are disabled in the default build.

## Build and run

From the repository root, using the normal build prerequisites and toolchain:

```bash
cmake -S . -B cmake-build-experiments -DCMAKE_BUILD_TYPE=Release -DBUILD_DOCS=OFF -DMOONLIGHT_BUILD_VIDEO_EXPERIMENTS=ON -DMOONLIGHT_BUILD_AUDIO_EXPERIMENTS=ON
cmake --build cmake-build-experiments
```

On Windows, use an MSYS2 UCRT64 shell and add `-G "MinGW Makefiles"` to the configure command.
The Xbox child build selects Unix Makefiles automatically. The generated files are:

| Probe             | ISO                                                        | XBE                                                                  |
| ----------------- | ---------------------------------------------------------- | -------------------------------------------------------------------- |
| Presentation      | `cmake-build-experiments/xbox/PvideoPresentationProbe.iso` | `cmake-build-experiments/xbox/pvideo-presentation-probe/default.xbe` |
| MPEG-2 prediction | `cmake-build-experiments/xbox/Mpeg2MotionProbe.iso`        | `cmake-build-experiments/xbox/mpeg2-motion-probe/default.xbe`        |
| Audio playback    | `cmake-build-experiments/xbox/AudioPlaybackProbe.iso`      | `cmake-build-experiments/xbox/audio-playback-probe/default.xbe`      |

Launch an ISO with the repository's configured xemu installation, for example:

```bash
./scripts/run-xemu.sh --iso cmake-build-experiments/xbox/PvideoPresentationProbe.iso
```

From a Windows command prompt, use `scripts\run-xemu.cmd --iso` with the same ISO path.
The video probes use GPU presentation directly. The streaming client selects its path in
**Settings → Display → Video Presentation**.

## Presentation probe

The probe checks numeric statistics formatting, displays a gray CPU framebuffer, then runs two
overlay sessions. Each shows rotating red, green, blue and white bars scaled from an HD source.
After both sessions, **PVIDEO PROBE COMPLETE** appears on the restored CPU framebuffer.

Inspect the colors, scaling, buffer updates and framebuffer restoration. Submission or teardown
failures display an error. In xemu, use a graphical display: display updates drive its emulated
vertical blank, so a headless run cannot validate the presentation path.

## MPEG-2 prediction probe

The probe cycles integer and half-pixel motion offsets. A CPU-computed luma reference appears on
the left, and GPU sampling appears on the right. Compare the two while checking the displayed
motion vector. The sequence repeats until the program is closed.

This is a visual comparison of one prediction stage, not a compressed-video decoder or a
bit-exact reconstruction test. The overlay is composed during scanout and cannot be read back
through the RGB framebuffer. Console testing is needed to assess image correctness and
performance on hardware; emulator output alone cannot establish those properties.

## Audio playback probe

Launch `AudioPlaybackProbe.iso` with the same launcher. The probe runs two short tone tests through
the stream client's FFmpeg Opus decoder, mixer and SDL playback device.

| Test   | Encoded tone | Expected Xbox output |
| ------ | ------------ | -------------------- |
| Stereo | Front left   | 500 Hz on the left   |
| Mono   | Front left   | 500 Hz on both sides |

Listen for the listed speaker in each test. **AUDIO PROBE COMPLETE** confirms every mode decoded and
queued the expected PCM byte count and the SDL queue drained. Audible output or a host audio capture
verifies the channel routing. The probe uses local Opus packets and can run with Sunshine closed.

## 5.1 channel check

Generate the host-side test files from the repository root with Python and FFmpeg on your path.
Choose an output directory under the current directory. Use a fresh directory for each run to
preserve any existing test files.

```bash
python experiments/generate_surround_channel_check.py cmake-build-experiments/audio
```

The 5.1 WAV plays a one-second tone on each channel with half a second of silence between tones.
Play it through a 5.1-capable player and output to check speaker routing.

| Time      | Active channel |
| --------- | -------------- |
| 0.5–1.5 s | Front left     |
| 2.0–3.0 s | Front right    |
| 3.5–4.5 s | Front center   |
| 5.0–6.0 s | LFE            |
| 6.5–7.5 s | Surround left  |
| 8.0–9.0 s | Surround right |

The `.spdif` file carries the same sequence as IEC 61937 bursts containing Dolby Digital AC-3.
It provides test data for a future Xbox digital output backend. The xemu playback probe above
exercises the existing stereo PCM output.
