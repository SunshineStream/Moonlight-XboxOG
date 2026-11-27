# Testing on Android and Android TV

The APK supports devices and emulator images that can run `arm64-v8a` apps on Android 8.0
(API 26) or later. The same APK provides phone and Android TV launcher entries.

## Build or download the APK

To test a pull request, download and extract the `Sunshine-client-Android` artifact from its GitHub
Actions run. The APK uses the release build variant and a test signing key for sideloading.
Push builds use the distribution key when configured. Without the key, they upload
`Sunshine-client-Android-unsigned`, which requires signing before installation.
You can install signed APKs from Windows, macOS, or Linux using
[Android SDK Platform Tools](https://developer.android.com/tools/releases/platform-tools).

To build from source on Linux or macOS, install Java 25, CMake, Meson, Ninja, pkg-config, Perl,
and Make. Install the SDK packages pinned in `packaging/android/gradle.properties`
using Android Studio's SDK Manager or `sdkmanager`. Set `ANDROID_HOME` to the Android SDK directory,
then run these commands from the repository root.

```bash
git submodule update --init --recursive
bash packaging/android/gradlew -p packaging/android :app:assembleRelease \
  -PsunshineSignReleaseForTesting=true
```

The signed test APK is in `packaging/android/app/build/outputs/apk/release/app-release.apk`.
For debugger builds, use `:app:assembleDebug` and omit `-PsunshineSignReleaseForTesting=true`.

### Install native build tools on Windows

Install the pinned SDK packages from the same Gradle properties file. In an **MSYS2 UCRT64**
shell, install the host tools and initialize the submodules from the repository root.

```bash
pacman -S --needed git make perl mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-meson \
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf
git submodule update --init --recursive
```

Gradle starts UCRT64 automatically from Android Studio. Set `MSYS2_ROOT` in the environment if
your MSYS2 installation is outside the default locations and its tools are absent from `PATH`.

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

### Set up CI signing

1. Create the release keystore and alias in Android Studio and store a secure backup. Decide
   whether the GitHub APK needs to share an app-signing certificate with Google Play before
   enrolling the app in Play App Signing.
2. In GitHub's **Settings > Secrets and variables > Actions**, add these repository secrets.

   - `ANDROID_KEYSTORE_BASE64` contains the Base64 encoding of the keystore file.
   - `ANDROID_KEYSTORE_PASSWORD` contains the keystore password.
   - `ANDROID_KEY_ALIAS` contains the signing key alias.
   - `ANDROID_KEY_PASSWORD` contains the signing key password.

3. On pushes to the release branch, CI signs the APK and the Android App Bundle. Download the
   APK from `Sunshine-client-Android` or the draft GitHub release. Download the signed `app-release.aab`
   from the workflow's `Sunshine-client-Android-AppBundle` artifact for Play Console. Each workflow run
   uses its run number as the Android version code. Pull requests use a test key.

Google Play currently requires new phone apps to target API 36 or newer. Confirm the target SDK
and test phone, TV, and streaming behavior before the first submission.
[Google Play target API requirements](https://support.google.com/googleplay/android-developer/answer/11926878)

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

Open **Sunshine** from the device's app launcher. On a TV, verify that it appears in the TV apps
list and can be opened with the remote.

## Run in Android Studio

### Test a prebuilt APK in the emulator

1. Open **Device Manager** in Android Studio and create a virtual device with a phone or TV profile.
   Choose a system image at API 26 or later that supports `arm64-v8a`, such as an ARM64 image on
   Apple silicon. [Android’s virtual device guide](https://developer.android.com/studio/run/managing-avds)
2. Start the emulator and drag the APK onto its screen, or use the same `adb install` command with
   the emulator's serial. Open Sunshine from the emulator's app launcher.
   [Android’s emulator installation guide](https://developer.android.com/studio/run/emulator-install-add-files)

If installation reports an ABI mismatch, check `adb -s DEVICE_SERIAL shell getprop ro.product.cpu.abilist`.
Use a compatible emulator image or a physical ARM64 device if the list lacks `arm64-v8a`.

### Build and run the source project

Open `packaging/android` as its own project in Android Studio. Its project settings live in
`packaging/android/.idea`, while CLion uses `.idea` at the repository root. Both IDEs can stay open.

On the first sync or build, Gradle fetches the pinned SDL3 source and builds FFmpeg, dav1d, and
OpenSSL in `cmake-build-android-deps` using the NDK selected by the Android Gradle plugin.
It reuses the files on subsequent runs and rebuilds when files are missing or the native
dependency pins, SDL3 patch, build script, or NDK version change. Allow network access for
the first preparation and when updating the dependencies.

To use a separately prepared prefix and SDL3 checkout, set their absolute paths in your user
Gradle properties file, usually `~/.gradle/gradle.properties`. Gradle checks these files as
supplied. The owner of the prepared prefix handles updates.

```properties
sunshineAndroidDeps=/path/to/checkout/cmake-build-android-deps
sunshineSdlSource=/path/to/prepared/SDL
```

In Android Studio, open **Settings > Build, Execution, Deployment > Build Tools > Gradle**.
Use the project's **Gradle Wrapper** distribution and sync the project. The checked-in Gradle
daemon JVM criteria select Java 25 for Android Studio, command-line builds, and CI. Gradle can
download a compatible JDK if it cannot find one locally.
[Gradle daemon JVM toolchains](https://docs.gradle.org/current/userguide/gradle_daemon.html#daemon_jvm_criteria)

Select the `app` run configuration and choose a connected device or emulator in the toolbar.
Click **Run** to build and install Sunshine with the default debug variant. To run the release
variant with your local test key, add `sunshineSignReleaseForTesting=true` to your Gradle properties,
sync, and select `release` in **Build Variants**.
[Android’s emulator run instructions](https://developer.android.com/studio/run/emulator)

## Test with Sunshine

1. In **Add Host**, enter the LAN IPv4 address of the computer running Sunshine. If Sunshine and
   the Android Emulator run on the same computer, `10.0.2.2` reaches that computer's loopback
   interface. [Android’s emulator networking guide](https://developer.android.com/studio/run/emulator-networking-address)
2. Pair with Sunshine and launch **Desktop**. Check video and audio, stop the stream, and confirm
   Sunshine returns to its app list.
3. On hardware, test touch navigation on a phone and directional navigation and Back on a TV.
   Check gamepad input and hardware decoding with a physical device.
