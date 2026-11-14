<div align="center">
  <img
    src="moonlight-logo.svg"
    alt="Moonlight icon"
    width="256"
  />
  <h1 align="center">Moonlight-XboxOG</h1>
  <h4 align="center">Port of Moonlight for the Original Xbox.</h4>
</div>

<div align="center">
  <a href="https://github.com/SunshineStream/Moonlight-XboxOG"><img src="https://img.shields.io/github/stars/SunshineStream/Moonlight-XboxOG.svg?logo=github&style=for-the-badge" alt="GitHub stars"></a>
  <a href="https://github.com/SunshineStream/Moonlight-XboxOG/actions/workflows/ci.yml?query=branch%3Amaster"><img src="https://img.shields.io/github/actions/workflow/status/SunshineStream/Moonlight-XboxOG/ci.yml.svg?branch=master&label=CI%20build&logo=github&style=for-the-badge" alt="GitHub Workflow Status (CI)"></a>
  <a href="https://codecov.io/gh/SunshineStream/Moonlight-XboxOG"><img src="https://img.shields.io/endpoint.svg?url=https%3A%2F%2Fapp.lizardbyte.dev%2Fdashboard%2Fshields%2Fcodecov%2FMoonlight-XboxOG.json&style=for-the-badge&logo=codecov" alt="Codecov"></a>
  <a href="https://sonarcloud.io/project/overview?id=SunshineStream_Moonlight-XboxOG"><img src="https://img.shields.io/sonar/quality_gate/SunshineStream_Moonlight-XboxOG.svg?server=https%3A%2F%2Fsonarcloud.io&style=for-the-badge&logo=sonarqubecloud&label=sonarcloud" alt="SonarCloud"></a>
</div>

# Overview

![Splash Screen](./docs/images/screenshots/01-splash.png)
![Hosts Screen](./docs/images/screenshots/02-hosts.png)
![Apps Screen](./docs/images/screenshots/03-apps.png)

<details>
<summary>Settings, host setup and dialogs</summary>

![Settings](./docs/images/screenshots/04-settings.png)
![Add Host](./docs/images/screenshots/05-add-host.png)
![IP Address Entry](./docs/images/screenshots/06-ip-address.png)
![App Actions](./docs/images/screenshots/07-app-actions.png)
![Connection Test Progress](./docs/images/screenshots/08-connection-testing.png)
![Successful Connection Test](./docs/images/screenshots/09-connection-success.png)
![Failed Connection Test](./docs/images/screenshots/10-connection-failed.png)
![Notification Stack](./docs/images/screenshots/11-notifications.png)

</details>

The interface takes its compact navigation and PC cards from Moonlight Qt, with a library designed
for a controller and a television. Select a PC to pair or open its apps. Game covers have separate
captions, and a blue focus ring shows which card will open. Running apps carry a **PLAYING** badge.
Missing artwork uses Lucide desktop or gamepad placeholders.
Use the D-pad or left stick to navigate, **A** to select, **B** to go back and **Y** for host or app
actions. Move up from the first PC row to reach Settings, Support and Add Host.
With a keyboard, hold the arrows to navigate, use **Enter** to select, **Esc** to go back, and **M**
for host or app actions. Button hints follow the keyboard or controller you are using.
On the hosts page, **Alt + F4** exits the client. **PgUp / PgDn** scroll the log viewer;
hold **Shift** for faster scrolling. **Backspace** dismisses a notification or changes the log pane.

PCs use grayscale moon artwork inspired by One: a full moon for paired online PCs, a half moon for
online PCs awaiting pairing, and a crescent for offline PCs. A separate orbit shows controller focus.

Settings categories have icons and the detail panel extends beside the page heading, with a fixed help
area above its options so changing values keeps the list in place. Dialogs contain their controller
actions. Number pads use compact centered columns. IPv4 entry has four
sections with fixed periods: three digits advance automatically, and **White / Black** move
between sections. A new digit replaces a section after switching to it; **X** deletes the last digit.
Connection, pairing and save actions appear after a host address has been entered. **Test Connection**
shows a spinning progress indicator while contacting the PC, then keeps the result visible until dismissed.
The app menu uses checkboxes for **Hide app** and **Set favorite**.

Notifications stack above the footer. Brief confirmations disappear automatically; actionable errors
stay until you press **X** to dismiss the newest card. Connection-test results stay in their dialog,
and settings show their updated values inline.

The layout scales with the output resolution and leaves a five-percent inset for television overscan.
Large libraries scroll to keep the selected game visible. Settings use a category list alongside
options and help for the selected item.

Navigation icons use [Lucide](https://lucide.dev/). CPM downloads the tagged source release declared in
`package-lock.cmake`; the inherited Renovate configuration updates its version.
Only the required SVGs and their license are packaged on the ISO.
No network connection or JavaScript runtime is needed to display the icons on the console.


## Video codec selection

**Settings → Display → Video Codec** defaults to **Auto (Prefer MPEG-2)**. Moonlight tries MPEG-2
when the host advertises it and uses standard automatic negotiation if it is unavailable or the
MPEG-2 handshake fails. Choose **Auto** for standard negotiation, which prefers H.264, then MPEG-2,
then H.263+. You can also select H.264, MPEG-2, or H.263+ directly.
An explicit choice requests only that codec and fails negotiation when it is unavailable.
Sunshine must enable `mpeg2_mode = 2` or `h263p_mode = 2` to advertise the corresponding legacy codec.
These Sunshine protocol extensions are available to any client implementing their negotiation and decoding.

The preference is saved as `streaming.video_codec` in the settings file. The default value is
`"prefer_mpeg2"`. Moonlight also reads the older `streaming.video_decoder` key for compatibility.
`video_codec` takes precedence when both exist.

## Video presentation

**Settings → Display → Video Presentation** defaults to **GPU**, which uses NV2A color conversion and
scaling with automatic software fallback. **Software** combines the decoded video and overlay on the
CPU before displaying each frame. Moonlight keeps your selection in `streaming.gpu_presentation`.

**Settings → Display → GPU Video Buffers** applies to new GPU streams at any resolution. **Auto**
tries three buffers at 720p when the packed source fits a 1280×720 buffer and uses two elsewhere. Choose **2**
to save memory or **3** to pack the next frame while the previous one waits for scanout. If Moonlight
cannot allocate the spare, it uses two buffers. The `streaming.gpu_buffer_count` settings key accepts
`0` (Auto), `2`, and `3`. The stream log reports the number of GPU buffers in use.

With 1080i enabled in the Xbox video settings, **Stream Resolution → 1920×1080** requests a
1920×1080 stream and uses native 1080i output. Moonlight uses a 16-bit RGB framebuffer in this mode
to leave memory for the H.264 decoder.

## Stream audio

Choose **Settings → Audio → Audio Mode** before starting or resuming a stream. **Stereo** keeps
left and right separate and is the default. **Mono** mixes the stereo stream locally and plays the
same signal through both outputs. The selection is saved as `streaming.audio_mode` with a value
of `stereo` or `mono`.

**Play Audio on Xbox** defaults to **On**. Turn it off to skip local audio decoding and playback.
**Play Audio on PC** defaults to **Off**. Turn it on to request playback on the host during the stream.
Saved choices take precedence. To check Xbox audio without a host, use the
[standalone audio probe](experiments/README.md#audio-playback-probe).

## Streaming controls and statistics

The default stream profile is **640×480 at 60 FPS and 3 Mbps**. Saved settings take precedence.
Changing **Settings → Display → Stream Resolution** or **Stream Frame Rate** resets **Stream Bitrate**
to a default calculated with Moonlight Android's resolution factors and pixel-count interpolation,
scaled by the selected FPS and rounded to whole Mbps. At 30 / 60 FPS, the defaults are 1 / 3 Mbps for
640x480, 2 / 3 Mbps for 720x480, 5 / 10 Mbps for 1280x720, and 10 / 20 Mbps for 1920x1080.
Bitrates are displayed in Mbps. You can then adjust **Stream Bitrate** manually; the setting is saved
until resolution or FPS changes again.
These are starting values for image quality and bandwidth, not a guarantee of Xbox decoding performance.

Press **Back/Select + Black + White (LB + RB)** together on any connected controller to open stream
controls. Release the buttons, select a choice with either stick, then press **A** to choose it.
**B** resumes playback. A USB keyboard can open the same menu with **Ctrl + Alt + Shift + Q**;
use **Tab** to cycle through all choices, arrows to select the left wheel, **Enter** to choose, and
**Esc** to go back. Center the sticks and release the buttons before resuming gameplay.
The same shortcut returns to the wheels from the keyboard, logs or exit confirmation.

| Stick | Up       | Right      | Down       | Left                   |
| ----- | -------- | ---------- | ---------- | ---------------------- |
| Left  | Keyboard | Mouse mode | View logs  | Exit                   |
| Right | Guide    | Share      | Screenshot | Enable / disable stats |

![Stream controls](./docs/images/screenshots/12-stream-controls.png)

**Exit** opens a confirmation dialog: **A / Enter** disconnects, **X / Q** quits the host app and **B / Esc** cancels.
**Disconnect** leaves the application running on the PC; select its pause-marked poster to resume.
Its app action menu also offers **Quit app**. Quitting can lose unsaved progress.

Use **D-pad / left stick** to move around the on-screen keyboard, **A** to type, **X** for Backspace,
**Y** for Space, and **Start** for host Enter. **Right stick click** applies Shift to the next key;
**left stick click** toggles Caps. **White / Black (LB / RB)** move the host text cursor left / right.
**LT** cycles through letters, symbols, and PC keys, including function keys and common shortcuts.
Focus a shortcut to see its combination above the keys. **RT** moves the keyboard between the top
and bottom of the screen; **B** returns to the wheels. Keys use the host's US keyboard layout.
When using a physical keyboard, use **Tab** to change page, **Shift** for one-shot Shift, **Caps Lock**
to toggle Caps, **F2** to move the panel, **Space / Backspace** to send those keys, and **Esc** to return.

![On-screen keyboard](./docs/images/screenshots/13-stream-keyboard.png)

**View logs** shows live entries. Scroll upward to read older entries; return to the bottom to follow
new entries. **Guide** and **Share** send their controller buttons to the PC; their behavior depends
on the host's virtual controller and operating system.

Screenshots are saved as BMP files, excluding local menus and statistics. The default folder is
`E:\UDATA\<title-id>\screenshots`. Open **Settings > Storage > Screenshot Folder** to browse drives
and subfolders, select the current folder, or create a new folder with the on-screen keyboard or a
USB keyboard. Press **Start / Enter** to create a folder after entering its name.
**Use default location** restores the default folder. **B / Esc** cancels without changing the destination.

Open a game or app's action menu and choose **Screenshots** to view its captures. The viewer works
offline and shows the newest capture first; use **LB / RB**, left / right or **PgUp / PgDn** to browse,
and **B / Esc** to return. Use **X / Backspace / Delete** to delete the selected image after confirmation.

Screenshots save in the background. A popup shows progress, then the saved path or an error.
The wheels close automatically after screenshot, Guide, Share, statistics and mouse-mode actions.

![Screenshot notification](./docs/images/screenshots/14-screenshot-notification.png)

Mouse mode uses the invoking controller's **left stick** for movement and **A / B** for left / right
clicks. Disable mouse mode through the wheels to use that controller for gameplay again.
Other gamepads and physical keyboards and mice can be used at the same time.

Up to four gamepads are supported as separate controllers in Sunshine.
Standard USB boot keyboards and three-button boot mice are supported through Xbox-to-USB adapters
attached to the controller ports. Devices must support HID boot protocol; mouse wheels, extra buttons
and nonstandard gaming devices may not work.

For xemu, open its Monitor with **~** and enter `device_add usb-kbd,id=kbd` or
`device_add usb-mouse,id=mouse` to attach an emulated USB device to an available port.
Remove them with `device_del kbd` and `device_del mouse`. See the
[xemu keyboard setup guide](https://xemu.app/docs/keyboard/) for explicit controller-port selection.

When Sunshine and xemu run on the same PC, prevent Sunshine's virtual controllers from being attached
to xemu. Disable automatic controller binding in **Input**, manually assign the physical controller,
and leave ports used for keyboards or mice unassigned to controllers to avoid USB port conflicts.

Enable **Settings → Display → Performance Stats** for a live panel during playback and additional
performance details in the summary. The panel works with GPU and software presentation, refreshes
once per second, and shows `N/A` for unavailable measurements. It includes measured incoming,
decoded and rendered FPS, video bitrate, timing, frame drops, buffering and the presentation path.
The stream header shows the requested FPS; the three FPS counters show measured rates.
Network latency is round-trip time. Present measures CPU frame preparation and submission,
including any scanout wait.
The stream menu can toggle these statistics for the current session; the saved setting sets the
initial state of the next session.

Video decoding runs on the CPU. Every stream summary reports GPU and software frame counts.
**SOFTWARE FALLBACK USED** includes the reason GPU presentation switched to software, even when
Performance Stats is disabled.

## 🛠️ Build

### Prerequisites

1. Clone the repository with submodules, or update them after cloning.

   ```bash
   git submodule update --init --recursive
   ```

2. Install nxdk prerequisites.

#### Windows

> [!NOTE]
> You must use the mingw64 shell on Windows.

```bash
pacman -Syu
nxdk_dependencies=(
  "bison"
  "flex"
  "git"
  "make"
  "mingw-w64-x86_64-cmake"
  "mingw-w64-x86_64-clang"
  "mingw-w64-x86_64-gcc"
  "mingw-w64-x86_64-lld"
  "mingw-w64-x86_64-llvm"
  "mingw-w64-x86_64-make"
  "mingw-w64-x86_64-nasm"
)
moonlight_dependencies=(
  "patch"
  "mingw-w64-x86_64-doxygen"
  "mingw-w64-x86_64-graphviz"
  "mingw-w64-ucrt-x86_64-nodejs"
)
dependencies=("${nxdk_dependencies[@]}" "${moonlight_dependencies[@]}")
pacman -S "${dependencies[@]}"
```

#### Debian/Ubuntu Linux

```bash
nxdk_dependencies=(
  "bison"
  "build-essential"
  "clang"
  "cmake"
  "flex"
  "git"
  "lld"
  "llvm"
  "nasm"
)
moonlight_dependencies=(
  "patch"
  "doxygen"
  "graphviz"
  "nodejs"
)
dependencies=("${nxdk_dependencies[@]}" "${moonlight_dependencies[@]}")
apt install "${dependencies[@]}"
```

#### macOS

```bash
nxdk_dependencies=(
  "cmake"
  "coreutils"
  "lld"
  "llvm"
  "nasm"
)
moonlight_dependencies=(
  "doxygen"
  "graphviz"
  "node"
)
dependencies=("${nxdk_dependencies[@]}" "${moonlight_dependencies[@]}")
brew install "${dependencies[@]}"
```

### Configure

Configure the top-level project with the normal host toolchain. CMake uses the vendored `third-party/nxdk` checkout, copies its tracked sources and nested submodules into the build directory, bootstraps the required `nxdk` outputs there, builds the host-native tests with the standard compiler/linker, and drives the Xbox build through an internal child configure that reuses the stock `nxdk` toolchain.

For host and Xbox builds, CMake checks whether the compiler supports Clang's `unused-but-set-global` warning. When supported, this warning remains nonfatal for `moonlight-common-c`, whose debug counters are read only by assertions that are disabled in release builds. Other warnings retain the dependency's existing error policy.

```bash
cmake -S . -B cmake-build-release -DBUILD_DOCS=OFF -DCMAKE_BUILD_TYPE=Release
```

Optional Xbox build settings:

| CMake option                        | Default | Purpose                                                                                        |
| ----------------------------------- | ------- | ---------------------------------------------------------------------------------------------- |
| `MOONLIGHT_BUILD_VIDEO_EXPERIMENTS` | `OFF`   | Build the standalone [video probes](experiments/README.md).                                    |
| `MOONLIGHT_BUILD_AUDIO_EXPERIMENTS` | `OFF`   | Build the standalone [audio playback probe](experiments/README.md#audio-playback-probe).       |
| `MOONLIGHT_INPUT_DIAGNOSTICS`       | `OFF`   | Show controller state and event counters. Select Software presentation to see the debug strip. |

Pass these options when configuring. Existing build directories retain cached build options until
explicitly reconfigured.

### Build

```bash
cmake --build cmake-build-release
```

### Combined script

This script takes care of everything, except installing the prerequisites.

```bash
./build.sh
```

The default build directory is `cmake-build-release`. You can override it or force a clean build:

```bash
./build.sh --build-dir cmake-build-debug
./build.sh --clean
./build.sh cmake-build-custom clean
```

To launch the same build from shells outside MSYS2 on Windows, use one of these wrappers:

```bat
build-mingw64.bat
```

```bash
./build-mingw64.sh
```

### Host-native unit tests

The Xbox executable cannot run directly on Windows, macOS, or Linux, so the top-level project builds `test_moonlight` natively while the Xbox binary is built by an internal child configure that uses the `nxdk` toolchain. Keep Xbox runtime code thin and move logic you want to test into platform-neutral sources that can be linked into `test_moonlight`.

#### Windows via MSYS2/mingw64

From `cmd.exe`, configure, build, and run the host tests after the helper locates your local MSYS2 installation:

```bat
call scripts\find-msys2.cmd && "%MOONLIGHT_MSYS2_SHELL%" -defterm -here -no-start -mingw64 -c "cd /c/Users/%USERNAME%/Dev/git/Moonlight-XboxOG && cmake -S . -B cmake-build-host-tests -DBUILD_DOCS=OFF -DBUILD_TESTS=ON -DBUILD_XBOX=OFF -DCMAKE_BUILD_TYPE=Debug && cmake --build cmake-build-host-tests --target test_moonlight && ctest --test-dir cmake-build-host-tests --output-on-failure"
```

If you are already inside a `mingw64` shell, the equivalent commands are:

```bash
cmake -S . -B cmake-build-host-tests -DBUILD_DOCS=OFF -DBUILD_TESTS=ON -DBUILD_XBOX=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-host-tests --target test_moonlight
ctest --test-dir cmake-build-host-tests --output-on-failure
```

#### Linux or macOS

```bash
cmake -S . -B cmake-build-host-tests -DBUILD_TESTS=ON -DBUILD_XBOX=OFF -DBUILD_DOCS=OFF
cmake --build cmake-build-host-tests --target test_moonlight
ctest --test-dir cmake-build-host-tests --output-on-failure
```

Coverage should come from this host-native test build instead of the cross-compiled Xbox build.

### Dependency compatibility on Xbox

The SDK submodule tracks upstream XboxDev/nxdk, including its upstream PDCLib. Moonlight's
Xbox adapters live in `src/_nxdk_compat` and are built by `cmake/nxdk-compat.cmake`.
All SDK libraries, host tools and generated lwIP files are built under the build
directory, including for direct Xbox presets. `cmake/nxdk-toolchain.cmake` prepares
that copy before compiler checks and then loads the stock nxdk toolchain. SDK
revision changes and local edits to tracked SDK files refresh the copy; unrelated
untracked files and existing build products are excluded. `MOONLIGHT_NXDK_DIR`
selects the source checkout; `MOONLIGHT_NXDK_BUILD_DIR` selects the build copy.

The Xbox child build fetches [nxdk-sdl3](https://github.com/Ryzee119/nxdk-sdl3),
SDL3_image, and SDL3_ttf at the revisions in `package-lock.cmake`.
Moonlight applies its boot USB keyboard and mouse hooks, video-mode integration,
and audio teardown changes to copies of the port's drivers in the build directory.
The shell uses the Xbox SDL3 renderer when the output mode supports it and falls
back to software rendering. Streaming uses a software SDL3 renderer for its
dialog and final fallback so the PVIDEO presenter can own pbkit.

The local network target builds upstream lwIP with the streaming receive limits,
socket options and mDNS enabled. The `network/nxdk-network.patch` compatibility
patch is applied with `git apply` to build-directory copies, preserving thread
priorities and safe packet drops on receive-pool exhaustion. The submodule
sources stay unchanged. `nxNetInit` is compiled with the same settings so its
network-interface layout matches lwIP.

The compatibility layer provides real wall-clock, sleep and reentrant string helpers.
FFmpeg's unused POSIX descriptor/file helpers and OpenSSL's optional POSIX directory
and metadata queries fail explicitly with `ENOSYS`. Moonlight's Win32 file APIs,
standard stdio and custom OpenSSL socket BIO continue to handle saved data and pairing.
FFmpeg configure probes still link actual Xbox libraries and the local timing helpers.

OpenSSL disables POSIX descriptor I/O; its socket BIO leaves socket ownership with
the caller. FFmpeg builds its original file helpers through its generic platform
path, with memory mapping and temporary-file creation disabled.

### CLion on Windows

The Windows preset in `CMakePresets.json` uses `MinGW Makefiles` for the host-native CLion build, auto-detects the local MSYS2 installation through `cmake/host-mingw64-clang.cmake`, and delegates the Xbox child build through a dedicated CMake driver that enters the vendored `nxdk` environment only for the child configure and build steps.

1. Open the project in CLion and import the `nxdk-release (mingw64)` preset from `CMakePresets.json`.
2. Use the normal build button with that profile selected. The top-level build will compile `test_moonlight` natively and configure the Xbox child build automatically.
3. When you use CLion's **Reset Cache and Reload Project**, the next configure will clean the `nxdk` build outputs and rebuild the required `nxdk` libraries and tools.
4. Build the `moonlight_xbox` target or the default `all` target. The generated ISO now lives at `cmake-build-release/xbox/Moonlight.iso`.

For the first xemu launch, you can either run the shared `Setup portable xemu` configuration or run the Windows wrapper manually:

```bat
scripts\setup-xemu.cmd
```

The repository now includes `.run/Run xemu.run.xml`, which launches `scripts\run-xemu.cmd` through `C:\Windows\System32\cmd.exe` without extra arguments and lets the launcher auto-discover a built Moonlight ISO.

If you create a local CLion run configuration that sets the working directory to a build output such as `$CMakeCurrentBuildDir$/xbox`, the Windows wrapper also treats that caller working directory as the xemu target path when no explicit launcher arguments or `MOONLIGHT_XEMU_*` overrides are provided.

The setup script downloads xemu and the emulator support files into `.local/xemu`, then refreshes launcher manifests used by `scripts/run-xemu.sh`. Existing files under `.local/xemu` are preserved by default so a local xemu install or support bundle is not overwritten unless you pass `--force`. The launcher accepts `MOONLIGHT_XEMU_BUILD_DIR`, `MOONLIGHT_XEMU_ISO_PATH`, `--build-dir <cmake-build-dir>`, `--iso <iso-path>`, or a single positional path that can point at either a build directory or an ISO file. If you do not pass a path, it falls back across available `cmake-build-*` outputs and prefers the newest built ISO.

When xemu runs with its default user-mode network, multicast mDNS traffic is not forwarded reliably, so automatic host discovery may not find your PC. For reliable discovery, launch xemu with `--network tap --tap-ifname <adapter>` or add the host manually from the Xbox UI.

If you only want the emulator without the ROM/HDD support bundle, run:

```bat
scripts\setup-xemu.cmd --skip-support-files
```

> [!NOTE]
> To expose widescreen and HDTV modes in xemu, use
> [Xbox EEPROM Editor](https://github.com/Ernegien/XboxEepromEditor). 1080i streaming is demanding
> on the emulated CPU, especially when using software presentation.

## 📋 Todo

- [ ] Debugger, see https://github.com/abaire/xbdm_gdb_bridge
- [ ] Dolby Digital 5.1 audio output over optical S/PDIF
