#!/usr/bin/env bash
# Build static OpenSSL and FFmpeg for a mobile SDK. Desktop uses build-deps instead.
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 android|ios|tvos|ios-simulator|tvos-simulator <dependency-prefix>" >&2
  exit 2
fi

target="$1"
source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p -- "$2"
prefix="$(cd -- "$2" && pwd)"
work_dir="${prefix}/sources"
mkdir -p -- "${work_dir}"

lock_value() {
  local variable="$1"
  sed -nE "s/^set\(${variable} ([^)]*)\).*$/\1/p" "${source_root}/package-lock.cmake"
}

# Each source directory includes its version so a dependency update gets a clean checkout.
checkout() {
  local repository="$1" tag="$2" destination="$3"
  if [[ ! -d "${destination}/.git" ]]; then
    git -c core.autocrlf=false clone --quiet --depth=1 --branch "${tag}" "${repository}" "${destination}"
  fi
}

openssl_tag="$(lock_value OPENSSL_TAG)"
ffmpeg_tag="n$(lock_value FFMPEG_VERSION)"
dav1d_tag="$(lock_value DAV1D_CLIENT_VERSION)"
openssl_src="${work_dir}/${openssl_tag}"
ffmpeg_src="${work_dir}/ffmpeg-${ffmpeg_tag}"
dav1d_src="${work_dir}/dav1d-${dav1d_tag}"
checkout https://github.com/openssl/openssl.git "${openssl_tag}" "${openssl_src}"
checkout https://github.com/FFmpeg/FFmpeg.git "${ffmpeg_tag}" "${ffmpeg_src}"
checkout https://github.com/videolan/dav1d.git "${dav1d_tag}" "${dav1d_src}"

openssl_options=(no-shared no-tests no-apps)
openssl_env=("CFLAGS=${CFLAGS:-}" "LDFLAGS=${LDFLAGS:-}")
target_flags='[]'
meson_cc=
meson_prefix="${prefix}"
meson_setup=(meson setup)
meson_install=(meson install)
ffmpeg_options=(
  --arch=aarch64 --enable-cross-compile --enable-pic
  --disable-autodetect --disable-debug --disable-doc --disable-programs
  --disable-shared --enable-static --disable-everything
  --disable-avdevice --disable-avformat --disable-avfilter --disable-network
  --enable-libdav1d '--enable-decoder=h264,hevc,av1,libdav1d,opus'
  '--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,av1_frame_split'
)

case "${target}" in
  android)
    : "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to the Android NDK}"
    case "$(uname -s)" in
      Darwin) ndk_host=darwin-x86_64 ;;
      Linux) ndk_host=linux-x86_64 ;;
      MINGW*|MSYS*)
        ndk_host=windows-x86_64
        ANDROID_NDK_HOME="$(cygpath -u "${ANDROID_NDK_HOME}")"
        ffmpeg_options+=("--host-cc=${MINGW_PREFIX:?Use the MSYS2 UCRT64 shell}/bin/clang")
        ;;
      *) echo "Build Android dependencies from Linux, macOS, or MSYS2." >&2; exit 2 ;;
    esac
    ndk_bin="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${ndk_host}/bin"
    export PATH="${ndk_bin}:${PATH}"
    cc=aarch64-linux-android26-clang
    if [[ "${ndk_host}" == windows-x86_64 ]]; then
      # Native Meson launches Windows executables directly. The NDK's shell wrapper is for Bash.
      meson_cc="['$(cygpath -m "${ndk_bin}/clang.exe")', '--target=aarch64-linux-android26']"
      # Meson validates the target's POSIX prefix and stages it into the Windows install directory.
      meson_prefix=/
      meson_setup=(env MSYS2_ARG_CONV_EXCL=--prefix= meson setup)
      meson_install+=(--destdir "$(cygpath -m "${prefix}")")
    fi
    ar=llvm-ar
    strip=llvm-strip
    system=android
    export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
    openssl_target=android-arm64
    openssl_options+=(-D__ANDROID_API__=26)
    ffmpeg_options+=(--target-os=android --enable-jni --enable-mediacodec
      '--enable-decoder=h264_mediacodec,hevc_mediacodec,av1_mediacodec')
    sdl_tag="release-$(lock_value SDL3_VERSION)"
    sdl_src="${work_dir}/sdl-${sdl_tag}"
    checkout https://github.com/libsdl-org/SDL.git "${sdl_tag}" "${sdl_src}"
    sdl_patch="${source_root}/cmake/patches/sdl3-steam-controller-trigger-clicks-and-haptics.patch"
    if ! git -C "${sdl_src}" apply --reverse --check "${sdl_patch}" 2>/dev/null; then
      git -C "${sdl_src}" apply "${sdl_patch}"
    fi
    ;;
  ios|tvos|ios-simulator|tvos-simulator)
    case "${target}" in
      ios) sdk=iphoneos; triple=arm64-apple-ios15.0 ;;
      tvos) sdk=appletvos; triple=arm64-apple-tvos15.0 ;;
      ios-simulator) sdk=iphonesimulator; triple=arm64-apple-ios15.0-simulator ;;
      tvos-simulator) sdk=appletvsimulator; triple=arm64-apple-tvos15.0-simulator ;;
      *) echo "Unsupported Apple target: ${target}" >&2; exit 2 ;;
    esac
    sysroot="$(xcrun --sdk "${sdk}" --show-sdk-path)"
    cc="$(xcrun --sdk "${sdk}" --find clang)"
    ar="$(xcrun --sdk "${sdk}" --find ar)"
    strip="$(xcrun --sdk "${sdk}" --find strip)"
    system=darwin
    target_flags="['-target', '${triple}', '-isysroot', '${sysroot}']"
    openssl_target=darwin64-arm64-cc
    openssl_options+=(no-asm no-async)
    # Configure treats a bare target triple as a second OpenSSL platform name.
    sdk_flags="-target ${triple} -isysroot ${sysroot}"
    openssl_env=("CFLAGS=${sdk_flags} ${CFLAGS:-}" "LDFLAGS=${sdk_flags} ${LDFLAGS:-}")
    ffmpeg_options+=(--target-os=darwin --enable-videotoolbox
      '--enable-hwaccel=h264_videotoolbox,hevc_videotoolbox,av1_videotoolbox'
      "--extra-cflags=-target ${triple} -isysroot ${sysroot}"
      "--extra-ldflags=-target ${triple} -isysroot ${sysroot}")
    ;;
  *) echo "Unsupported target: ${target}" >&2; exit 2 ;;
esac

jobs="${MOONLIGHT_BUILD_JOBS:-4}"
meson_cc="${meson_cc:-'${cc}'}"
cat > "${work_dir}/dav1d-cross.ini" <<EOF
[binaries]
c = ${meson_cc}
ar = '${ar}'
strip = '${strip}'

[host_machine]
system = '${system}'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[built-in options]
c_args = ${target_flags}
c_link_args = ${target_flags}
EOF

"${meson_setup[@]}" --cross-file "${work_dir}/dav1d-cross.ini" "--prefix=${meson_prefix}" --libdir lib \
  --default-library static --buildtype release -Denable_tools=false -Denable_tests=false \
  "${work_dir}/dav1d-build" "${dav1d_src}"
meson compile -C "${work_dir}/dav1d-build" -j "${jobs}"
"${meson_install[@]}" -C "${work_dir}/dav1d-build"
if [[ "${meson_prefix}" == / ]]; then
  # Override the target prefix in dav1d.pc before FFmpeg looks it up on the build host.
  sed -i 's|^prefix=/$|prefix=${pcfiledir}/../..|' "${prefix}/lib/pkgconfig/dav1d.pc"
fi

# OpenSSL 4.1 generates this configuration header even with no-apps.
mkdir -p -- "${work_dir}/openssl-build/apps/include" "${work_dir}/ffmpeg-build"
(
  cd -- "${work_dir}/openssl-build"
  env CC="${cc}" "${openssl_env[@]}" perl "${openssl_src}/Configure" "${openssl_target}" \
    --prefix="${prefix}" --libdir=lib "${openssl_options[@]}"
  make -j "${jobs}" build_libs
  make install_dev
)
(
  cd -- "${work_dir}/ffmpeg-build"
  PKG_CONFIG_LIBDIR="${prefix}/lib/pkgconfig" PKG_CONFIG_PATH='' \
    "${ffmpeg_src}/configure" --cc="${cc}" --ar="${ar}" --strip="${strip}" --pkg-config=pkg-config \
    --pkg-config-flags=--static --prefix="${prefix}" "${ffmpeg_options[@]}"
  make -j "${jobs}"
  make install
)

if [[ "${target}" == android ]]; then
  # The ARM64 libraries can be staged for Android Studio on a different host.
  pkgconfig_prefix='${prefix}'
  relative_prefix='prefix=${pcfiledir}/../..'
  build_prefixes=("${prefix}")
  if [[ "${meson_prefix}" == / ]]; then
    build_prefixes+=("$(cygpath -m "${prefix}")")
  fi
  for metadata in "${prefix}"/lib/pkgconfig/*.pc; do
    contents=$(<"${metadata}")
    for build_prefix in "${build_prefixes[@]}"; do
      contents="${contents//"${build_prefix}"/$pkgconfig_prefix}"
    done
    printf '%s\n' "${contents/#prefix="$pkgconfig_prefix"/$relative_prefix}" > "${metadata}"
  done
fi
