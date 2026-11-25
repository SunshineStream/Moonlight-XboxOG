# Testing on Android and Android TV

The APK supports devices and emulator images that can run `arm64-v8a` apps on Android 8.0
(API 26) or later. The same APK provides phone and Android TV launcher entries.

## Build or download the APK

To test a pull request, download and extract the `Moonlight-Android` artifact from its GitHub
Actions run. The APK uses the release build variant and a test signing key for sideloading.
Other CI runs upload `Moonlight-Android-unsigned`, which requires signing before installation.
You can install signed APKs from Windows, macOS, or Linux using
[Android SDK Platform Tools](https://developer.android.com/tools/releases/platform-tools).

To build from source on Linux or macOS, install Java 25, Android SDK Platform 35, Build Tools 36.0.0,
NDK 28.2.13676358, and CMake 3.22.1. Also install Meson, Ninja, pkg-config, Perl, and Make. Set
`ANDROID_HOME` to the Android SDK directory, then run these commands from the repository root.

```bash
git submodule update --init --recursive
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/28.2.13676358"
prefix="$PWD/cmake-build-android-deps"
bash scripts/build-client-deps.sh android "$prefix"
bash packaging/android/gradlew -p packaging/android :app:assembleRelease \
  -PmoonlightSignReleaseForTesting=true
```

The signed test APK is in `packaging/android/app/build/outputs/apk/release/app-release.apk`.
For debugger builds, use `:app:assembleDebug` and omit `-PmoonlightSignReleaseForTesting=true`.

### Prepare dependencies on Windows

In Android Studio's **SDK Manager**, install Android SDK Platform 35, Build Tools 36.0.0,
NDK 28.2.13676358, and CMake 3.22.1. Open an **MSYS2 UCRT64** shell at the repository root
and run:

```bash
pacman -S --needed git make perl mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-meson \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf
git submodule update --init --recursive
export ANDROID_NDK_HOME="$(cygpath -u "$LOCALAPPDATA")/Android/Sdk/ndk/28.2.13676358"
bash scripts/build-client-deps.sh android "$PWD/cmake-build-android-deps"
```

If your Android SDK is in a different directory, set `ANDROID_NDK_HOME` to its
`ndk/28.2.13676358` directory. Repeat this setup after the Android dependency versions
or SDL3 patch change. For day-to-day changes, use Android Studio's normal **Run** and **Debug**
actions.

## Sign a distribution build

For direct distribution, create your own app signing key. In Android Studio, open
**Build > Generate Signed App Bundle or APK**, select **APK**, and click **Create new** to create
a keystore and key alias. Leave the test signing option unset, select the `release` variant, and build
the signed APK. Store a secure
backup of the keystore, alias, and passwords. Future APK updates use the same signing key.

For Google Play, select **Android App Bundle** and sign it with an upload key. When you set up
Play App Signing in Play Console, Google can generate the app signing key it uses to sign
downloads. If you need APKs distributed outside Play to update the same installation, you can
provide your own app signing key during setup and use it for direct distribution.
[Android's app signing guide](https://developer.android.com/studio/publish/app-signing)

## Run on a physical device

On a phone, enable **USB debugging** in **Developer options**, connect it to your computer, and
approve the debugging prompt on the phone.
[Android’s USB device setup](https://developer.android.com/studio/run/device)

For an Android TV with **Wireless debugging**, connect the TV and computer to the same LAN and open
**Pair device with pairing code** on the TV. Run `adb pair TV_IP:PAIRING_PORT` with the pairing
address shown on that screen and enter the displayed code. If needed, run
`adb connect TV_IP:DEBUG_PORT` using the address from the main **Wireless debugging** screen. TVs
that offer **Network debugging** can use `adb connect` with the address shown by the device.
[Android’s wireless ADB setup](https://developer.android.com/tools/adb)

List the devices and install the APK. Replace `DEVICE_SERIAL` with the first column from
`adb devices -l` and `app-release.apk` with the path to your APK.

```bash
adb devices -l
adb -s DEVICE_SERIAL install -r app-release.apk
```

Android requires the same signing key for app updates. CI runs and local builds can use different
keys. If Android reports a signature mismatch, uninstall the installed build before installing the
new APK. Uninstalling removes the app's local data.

Open **Moonlight** from the device's app launcher. On a TV, verify that it appears in the TV apps
list and can be opened with the remote.

## Run in Android Studio

### Test a prebuilt APK in the emulator

1. Open **Device Manager** in Android Studio and create a virtual device with a phone or TV profile.
   Choose a system image at API 26 or later that supports `arm64-v8a`, such as an ARM64 image on
   Apple silicon. [Android’s virtual device guide](https://developer.android.com/studio/run/managing-avds)
2. Start the emulator and drag the APK onto its screen, or use the same `adb install` command with
   the emulator's serial. Open Moonlight from the emulator's app launcher.
   [Android’s emulator installation guide](https://developer.android.com/studio/run/emulator-install-add-files)

If installation reports an ABI mismatch, check `adb -s DEVICE_SERIAL shell getprop ro.product.cpu.abilist`.
Use a compatible emulator image or a physical ARM64 device if the list lacks `arm64-v8a`.

### Build and run the source project

Open `packaging/android` as its own project in Android Studio. Its project settings live in
`packaging/android/.idea`, while CLion uses `.idea` at the repository root. Both IDEs can stay open.

Gradle looks for the prepared libraries in `cmake-build-android-deps` at the repository root,
and for the project's pinned SDL3 source under that directory's `sources/`. If you use other
locations, set these Gradle properties to their absolute paths in your user Gradle properties
file, usually `~/.gradle/gradle.properties`.

```properties
moonlightAndroidDeps=/path/to/checkout/cmake-build-android-deps
moonlightSdlSource=/path/to/checkout/cmake-build-android-deps/sources/sdl-release-3.4.18
```

In Android Studio, open **Settings > Build, Execution, Deployment > Build Tools > Gradle**.
Use the project's **Gradle Wrapper** distribution and sync the project. The checked-in Gradle
daemon JVM criteria select Java 25 for Android Studio, command-line builds, and CI. Gradle can
download a compatible JDK if it cannot find one locally.
[Gradle daemon JVM toolchains](https://docs.gradle.org/current/userguide/gradle_daemon.html#daemon_jvm_criteria)

Select the `app` run configuration and choose a connected device or emulator in the toolbar.
Click **Run** to build and install Moonlight with the default debug variant. To run the release
variant with your local test key, add `moonlightSignReleaseForTesting=true` to your Gradle properties,
sync, and select `release` in **Build Variants**.
[Android’s emulator run instructions](https://developer.android.com/studio/run/emulator)

## Test with Sunshine

1. In **Add Host**, enter the LAN IPv4 address of the computer running Sunshine. If Sunshine and
   the Android Emulator run on the same computer, `10.0.2.2` reaches that computer's loopback
   interface. [Android’s emulator networking guide](https://developer.android.com/studio/run/emulator-networking-address)
2. Pair with Sunshine and launch **Desktop**. Check video and audio, stop the stream, and confirm
   Moonlight returns to its app list.
3. On hardware, test touch navigation on a phone and directional navigation and Back on a TV.
   Check gamepad input and hardware decoding with a physical device.
