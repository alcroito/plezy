#!/usr/bin/env bash
# Builds the native player's FFmpeg like webos_plugins/webos_native_player/tools/build-ffmpeg.sh,
# plus https: mbedtls is built first and linked statically into libavformat.
#
#   scripts/webos/build_native_player_ffmpeg.sh <arm|host> <ffmpeg tarball> <mbedtls tarball> <out dir>
#
# arm:  cross build with the LG NDK (WEBOS_NDK_ENV overrides the environment-setup file).
# host: x86_64 build with the system gcc, plus ffprobe for testing.
# Output: <out dir>/{include,lib} (and bin/ffprobe for host).
set -euo pipefail
target=$1
ff_tar=$(realpath "$2"); mbed_tar=$(realpath "$3"); out=$(realpath -m "$4")
repo=$(cd "$(dirname "$0")/../.." && pwd)
patches=$repo/webos_plugins/webos_native_player/tools/ffmpeg-patches

work=$(mktemp -d "${TMPDIR:-/tmp}/ffbuild.XXXXXX")
trap 'rm -rf "$work"' EXIT
mbed_prefix=$work/mbedtls-prefix

cmake_args=(-DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
  -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DMBEDTLS_FATAL_WARNINGS=OFF -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="$mbed_prefix")
ff_args=()
case $target in
  arm)
    ndk_env=${WEBOS_NDK_ENV:-/usr/local/starfish-sdk-x86_64/environment-setup-ca9v1-webosmllib32-linux-gnueabi}
    # shellcheck disable=SC1090
    . "$ndk_env"
    # The NDK environment sets flags for its own build system; one (--dyld-prefix) breaks plain gcc.
    unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
    arm_flags="-mthumb -mfpu=neon -mfloat-abi=softfp"
    # $CC carries flags, so CMake gets the bare compiler plus a toolchain file.
    cat > "$work/toolchain.cmake" <<TC
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER ${TARGET_PREFIX}gcc)
set(CMAKE_SYSROOT $SDKTARGETSYSROOT)
set(CMAKE_C_FLAGS_INIT "$arm_flags")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
TC
    cmake_args+=(-DCMAKE_TOOLCHAIN_FILE="$work/toolchain.cmake")
    ff_args=(--enable-cross-compile --arch=arm --cpu=cortex-a9 --target-os=linux
      --cross-prefix="$TARGET_PREFIX" --sysroot="$SDKTARGETSYSROOT")
    ;;
  host)
    ff_args=(--disable-x86asm)
    ;;
  *) echo "target must be arm or host" >&2; exit 1 ;;
esac

mkdir "$work/mbedtls"
tar -xf "$mbed_tar" -C "$work/mbedtls" --strip-components=1
cmake -S "$work/mbedtls" -B "$work/mbedtls/build" "${cmake_args[@]}" >/dev/null
cmake --build "$work/mbedtls/build" -j"$(nproc)" >/dev/null
cmake --install "$work/mbedtls/build" >/dev/null

mkdir "$work/ffmpeg"
tar -xf "$ff_tar" -C "$work/ffmpeg" --strip-components=1
cd "$work/ffmpeg"
for p in "$patches"/*.patch; do patch -p1 --quiet < "$p"; done

extra_cflags="-I$mbed_prefix/include"
[ "$target" = arm ] && extra_cflags="-mthumb -mfpu=neon -mfloat-abi=softfp -O2 $extra_cflags"
# --enable-version3: mbedtls is Apache-2.0, which FFmpeg only accepts for LGPLv3.
./configure \
  --prefix="$out" \
  "${ff_args[@]}" \
  --extra-cflags="$extra_cflags" \
  --extra-ldflags="-L$mbed_prefix/lib" \
  --enable-shared --disable-static --enable-pic \
  --disable-programs --disable-doc --disable-debug \
  --disable-autodetect --disable-everything \
  --disable-avdevice --disable-swscale --enable-small \
  --enable-avfilter --enable-filter=abuffer,abuffersink,atempo,aformat,aresample \
  --enable-zlib \
  --enable-version3 --enable-mbedtls \
  --enable-protocol=file,http,https,tcp,tls,crypto,data \
  --enable-demuxer=matroska,mov,mpegts,hls,aac,ac3,eac3,flac,mp3,wav,srt,ass,webvtt \
  --enable-parser=h264,hevc,av1,vp9,aac,ac3,mpegaudio,dca,mlp,flac,opus,vorbis \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata,dovi_rpu,aac_adtstoasc,vp9_superframe_split \
  --enable-decoder=subrip,ass,ssa,text,webvtt,movtext,pgssub,dvdsub,dvbsub \
  --enable-decoder=aac,ac3,eac3,dca,truehd,mlp,mp3,flac,opus,vorbis,pcm_s16le,pcm_s24le \
  --enable-encoder=ac3,eac3 \
  --enable-swresample \
  $([ "$target" = host ] && echo --enable-ffprobe)
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "FFmpeg $(cat VERSION 2>/dev/null || echo 7.1.1) ($target) -> $out"
ls "$out/lib"
