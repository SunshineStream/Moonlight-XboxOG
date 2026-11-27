#!/usr/bin/env bash
# Compile the Android Java sources while CodeQL traces javac.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project="${root}/packaging/android"

property() {
  sed -n "s/^${1}=//p" "${project}/gradle.properties"
}

platform="$(property sunshineCompileSdk)"
build_tools="$(property sunshineBuildTools)"
ndk="$(property sunshineNdk)"
cmake="$(property sunshineCmake)"
: "${platform:?Missing sunshineCompileSdk}" "${build_tools:?Missing sunshineBuildTools}" \
  "${ndk:?Missing sunshineNdk}" "${cmake:?Missing sunshineCmake}"

java_home="${JAVA_HOME_25_X64:-${JAVA_HOME_25_ARM64:-${JAVA_HOME:-}}}"
if [[ -n "${java_home}" ]]; then
  export JAVA_HOME="${java_home}"
  export PATH="${JAVA_HOME}/bin:${PATH}"
fi

export ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:?Set the Android SDK location}}"
"${ANDROID_HOME}/cmdline-tools/latest/bin/sdkmanager" --sdk_root="${ANDROID_HOME}" \
  "platforms;android-${platform}" "build-tools;${build_tools}" "ndk;${ndk}" "cmake;${cmake}"

if ! command -v meson >/dev/null || ! command -v ninja >/dev/null || ! command -v pkg-config >/dev/null; then
  sudo apt-get update
  sudo apt-get install -y meson ninja-build pkg-config
fi

# Keep SDL's Java sources outside the checkout so SARIF covers the client's Java sources.
dependencies="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/sunshine-android-deps.XXXXXX")"
trap 'rm -rf -- "${dependencies}"' EXIT
ANDROID_NDK_HOME="${ANDROID_HOME}/ndk/${ndk}" SUNSHINE_ANDROID_DEPS_BUILD_ID='' \
  bash "${root}/scripts/build-client-deps.sh" android "${dependencies}"
bash "${project}/gradlew" -p "${project}" --no-daemon --no-build-cache --rerun-tasks \
  :app:compileReleaseJavaWithJavac "-PsunshineAndroidDeps=${dependencies}"
