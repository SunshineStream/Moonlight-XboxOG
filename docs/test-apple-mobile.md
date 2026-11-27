# Testing on iPhone and Apple TV

Build on a Mac with Xcode and the iOS or tvOS platform support for your target. From the repository
root, initialize the submodules and install CMake, Meson, Ninja, and pkg-config. The dependency
build also uses Perl and Make.

```bash
git submodule update --init --recursive
brew install cmake meson ninja pkgconf
```

## Generate the Xcode project

Use the values for your run destination. The current simulator builds use ARM64 and run on Apple
silicon Macs. CI builds the iOS and tvOS simulator targets in parallel.

| Run destination    | `target`         | `system` | `sdk`              |
| ------------------ | ---------------- | -------- | ------------------ |
| iPhone device      | `ios`            | `iOS`    | `iphoneos`         |
| Apple TV device    | `tvos`           | `tvOS`   | `appletvos`        |
| iPhone simulator   | `ios-simulator`  | `iOS`    | `iphonesimulator`  |
| Apple TV simulator | `tvos-simulator` | `tvOS`   | `appletvsimulator` |

The example configures an iPhone device build. Set the three target values from the table and use the
appropriate signing argument for the destination.

```bash
target=ios
system=iOS
sdk=iphoneos
signing_arg=-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=YOUR_TEAM_ID
# For a simulator, use:
# signing_arg=-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO

prefix="$PWD/cmake-build-$target-deps"
build_dir="cmake-build-$target"
cmake -S cmake/mobile-deps -B "$prefix" -G Ninja "-DMOONLIGHT_MOBILE_TARGET=$target"
cmake --build "$prefix"
cmake -S . -B "$build_dir" -G Xcode \
  -DBUILD_NATIVE_CLIENT=ON -DBUILD_XBOX=OFF -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF \
  -DCMAKE_XCODE_GENERATE_SCHEME=ON \
  "-DCMAKE_SYSTEM_NAME=$system" "-DCMAKE_OSX_SYSROOT=$sdk" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  "-DCMAKE_FIND_ROOT_PATH=$prefix" "-DOPENSSL_ROOT_DIR=$prefix" \
  "-DFFMPEG_PREPARED_BINARIES=$prefix" "$signing_arg"
open "$build_dir/Moonlight.xcodeproj"
```

The target-specific build directories keep device SDKs and simulator SDKs in separate CMake caches.
To build from the terminal with the configured signing settings, run
`cmake --build "$build_dir" --config Release --target moonlight_client`.

## Run on a physical device

1. **iPhone.** Connect it to the Mac by USB, trust the Mac, and select the phone in Xcode's
   **Manage Devices** window. Follow the pairing prompts and enable Developer Mode if Xcode
   asks you to. [Apple’s Developer Mode guide](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device)
2. **Apple TV.** Connect the Mac and Apple TV to the same IPv6-enabled LAN. On the TV, open
   **Settings → Remotes and Devices → Remote App and Devices**. In Xcode, open **Manage Devices**,
   pair the Apple TV as a nearby device, and enter the PIN shown on the TV.
   [Apple’s device pairing guide](https://developer.apple.com/documentation/xcode/managing-your-simulated-and-physical-devices-in-device-hub)
3. **Sign and install.** Sign in to your Apple Account in Xcode. In the `moonlight_client` target's
   **Signing & Capabilities** pane, enable **Automatically manage signing** and choose your team.
   Select the physical device as the run destination and click **Run**. Xcode registers the device
   and prepares a development provisioning profile for it.
   [Apple’s device signing guide](https://developer.apple.com/documentation/xcode/running-your-app-on-simulated-or-physical-devices)

If your team cannot register `dev.lizardbyte.app.Moonlight`, set a unique test bundle ID in
`MACOSX_BUNDLE_GUI_IDENTIFIER` in your local `cmake/client-build.cmake`, regenerate the project,
and match that ID in Xcode's signing settings.

## Run in a simulator

Generate the project for **iPhone simulator** or **Apple TV simulator** with the simulator signing
argument. In Xcode, select the `moonlight_client` scheme, choose the matching simulator as the run
destination, and click **Run**. Use **Manage Devices** to add a simulator if you need a different
model or OS version.
[Apple’s simulator guide](https://developer.apple.com/documentation/xcode/managing-your-simulated-and-physical-devices-in-device-hub)

## Test with Sunshine

1. Allow local network access if prompted. In **Add Host**, enter the LAN IPv4 address of the
   computer running Sunshine. The Sunshine host can be a Windows PC.
2. Pair with Sunshine and launch **Desktop**. Check video and audio, then stop the stream and
   confirm Moonlight returns to its app list.
3. On hardware, check touch navigation on iPhone, remote navigation on Apple TV, and gamepad
   input during a stream. Use hardware to validate decoding performance and display behavior.
