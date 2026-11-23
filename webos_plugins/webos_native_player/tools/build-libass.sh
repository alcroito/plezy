#!/usr/bin/env bash
# Builds libass (ASS/SSA subtitle rendering) and what it needs, cross-compiled with the LG webOS NDK:
#   FreeType 2.13.3 (FreeType licence)  static, no zlib/png/bzip2/brotli
#   FriBidi 1.0.16  (LGPL-2.1)          shared: bundled as libfribidi.so.0, replaceable
#   HarfBuzz 10.1.0 (MIT)               static, with FreeType
#   libass 0.17.3   (ISC)               static, no fontconfig (fonts: the file's attachments plus
#                                        a default font file the player passes)
#
#   tools/build-libass.sh <dir with the four source tarballs> <out dir>
#
# Output: <out dir>/{include,lib}; webos/libass/ holds the copy the plugin links.
set -euo pipefail
src=$(realpath "$1"); out=$(realpath -m "$2")
here=$(cd "$(dirname "$0")/.." && pwd)
ndk_env=${WEBOS_NDK_ENV:-$here/../../.ref/flutter-webos/legacy-webos/toolchains/lg-ndk-11.2.0/environment-setup-ca9v1-webosmllib32-linux-gnueabi}
# shellcheck disable=SC1090
. "$ndk_env"
# The NDK's LDFLAGS carry --dyld-prefix, which plain gcc rejects.
export CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC" LDFLAGS="" CPPFLAGS=""
host=arm-webosmllib32-linux-gnueabi

check() {
  echo "$2  $src/$1" | sha256sum -c --quiet - || { echo "checksum mismatch: $1" >&2; exit 1; }
}
check freetype-2.13.3.tar.xz 0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
check fribidi-1.0.16.tar.xz 1b1cde5b235d40479e91be2f0e88a309e3214c8ab470ec8a2744d82a5a9ea05c
check harfbuzz-10.1.0.tar.xz 6ce3520f2d089a33cef0fc48321334b8e0b72141f6a763719aaaecd2779ecb82
check libass-0.17.3.tar.xz eae425da50f0015c21f7b3a9c7262a910f0218af469e22e2931462fed3c50959

work=$(mktemp -d "${TMPDIR:-/tmp}/assbuild.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"
export PKG_CONFIG_LIBDIR="$out/lib/pkgconfig" PKG_CONFIG_PATH="$out/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR=

cd "$work" && tar -xf "$src/freetype-2.13.3.tar.xz" && cd freetype-2.13.3
./configure --host=$host --prefix="$out" --enable-static --disable-shared --with-pic \
  --without-zlib --without-png --without-bzip2 --without-brotli --without-harfbuzz >/dev/null
make -j"$(nproc)" >/dev/null && make install >/dev/null

cd "$work" && tar -xf "$src/fribidi-1.0.16.tar.xz" && cd fribidi-1.0.16
./configure --host=$host --prefix="$out" --enable-shared --disable-static --disable-docs >/dev/null
make -j"$(nproc)" >/dev/null && make install >/dev/null

cd "$work" && tar -xf "$src/harfbuzz-10.1.0.tar.xz" && mkdir hb-build && cd hb-build
cmake ../harfbuzz-10.1.0 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
  -DCMAKE_C_COMPILER="${CC%% *}" -DCMAKE_CXX_COMPILER="${CXX%% *}" \
  -DCMAKE_C_FLAGS="${CC#* } -O2" -DCMAKE_CXX_FLAGS="${CXX#* } -O2 -fno-exceptions -fno-rtti" \
  -DCMAKE_FIND_ROOT_PATH="$out;$SDKTARGETSYSROOT" -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_INSTALL_PREFIX="$out" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DHB_HAVE_FREETYPE=ON -DHB_HAVE_GLIB=OFF -DHB_HAVE_ICU=OFF -DHB_HAVE_GOBJECT=OFF -DHB_BUILD_UTILS=OFF -DHB_BUILD_SUBSET=OFF \
  -DFREETYPE_INCLUDE_DIRS="$out/include/freetype2" -DFREETYPE_LIBRARY="$out/lib/libfreetype.a" >/dev/null
ninja >/dev/null && ninja install >/dev/null

cd "$work" && tar -xf "$src/libass-0.17.3.tar.xz" && cd libass-0.17.3
# The static HarfBuzz needs the C++ runtime when linked into a C program (configure's checks).
./configure --host=$host --prefix="$out" --enable-static --disable-shared --with-pic \
  --disable-fontconfig --disable-require-system-font-provider --disable-libunibreak --disable-asm \
  HARFBUZZ_LIBS="-L$out/lib -lharfbuzz -lstdc++ -lm" >/dev/null
make -j"$(nproc)" >/dev/null && make install >/dev/null

echo "libass 0.17.3 -> $out"
ls "$out/lib"
