#!/usr/bin/env bash
# Builds the FFmpeg the player bundles: LGPL only (no --enable-gpl / nonfree), shared libraries,
# demuxers, parsers and bitstream filters for what Jellyfin serves, subtitle decoders and the audio
# decoders for formats the TV cannot decode itself (TrueHD, DTS), atempo (playback speed with the
# pitch kept), no programs. zlib is the one
# external library (MKV tracks with zlib compression). Cross-compiled
# with the LG webOS NDK that legacy-webos installs.
#
#   tools/build-ffmpeg.sh <ffmpeg source tarball> <out dir>
#
# Patches in tools/ffmpeg-patches/ are applied first (dovi_rpu: profile 7 to 8.1).
#
# Output: <out dir>/{include,lib} (libavformat, libavcodec, libavutil, libswresample, libavfilter).
set -euo pipefail
src_tar=$(realpath "$1"); out=$(realpath -m "$2")
here=$(cd "$(dirname "$0")/.." && pwd)
ndk_env=${WEBOS_NDK_ENV:-$here/../../.ref/flutter-webos/legacy-webos/toolchains/lg-ndk-11.2.0/environment-setup-ca9v1-webosmllib32-linux-gnueabi}
# shellcheck disable=SC1090
. "$ndk_env"
# The NDK environment sets flags for its own build system; one (--dyld-prefix) breaks plain gcc.
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
work=$(mktemp -d "${TMPDIR:-/tmp}/ffbuild.XXXXXX")
trap 'rm -rf "$work"' EXIT
tar -xf "$src_tar" -C "$work" --strip-components=1
cd "$work"
# Our changes to FFmpeg (LGPL: the modified source is these patches on the official tarball).
for p in "$here"/tools/ffmpeg-patches/*.patch; do patch -p1 --quiet < "$p"; done
./configure \
  --prefix="$out" \
  --enable-cross-compile --arch=arm --cpu=cortex-a9 --target-os=linux \
  --cross-prefix="$TARGET_PREFIX" --sysroot="$SDKTARGETSYSROOT" \
  --extra-cflags="-mthumb -mfpu=neon -mfloat-abi=softfp -O2" \
  --enable-shared --disable-static --enable-pic \
  --disable-programs --disable-doc --disable-debug \
  --disable-autodetect --disable-everything \
  --disable-avdevice --disable-swscale --enable-small \
  --enable-avfilter --enable-filter=abuffer,abuffersink,atempo,aformat,aresample \
  --enable-zlib \
  --enable-protocol=file,http,tcp,crypto,data \
  --enable-demuxer=matroska,mov,mpegts,hls,aac,ac3,eac3,flac,mp3,wav,srt,ass,webvtt \
  --enable-parser=h264,hevc,av1,vp9,aac,ac3,mpegaudio,dca,mlp,flac,opus,vorbis \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata,dovi_rpu,aac_adtstoasc,vp9_superframe_split \
  --enable-decoder=subrip,ass,ssa,text,webvtt,movtext,pgssub,dvdsub,dvbsub \
  --enable-decoder=aac,ac3,eac3,dca,truehd,mlp,mp3,flac,opus,vorbis,pcm_s16le,pcm_s24le \
  --enable-encoder=ac3,eac3 \
  --enable-swresample
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "FFmpeg $(cat VERSION 2>/dev/null || echo 7.1.1) -> $out"
ls "$out/lib"
