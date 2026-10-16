#!/bin/bash
# Builds, into one local prefix, the same library versions Batocera ships
# (batocera.linux master, checked 2026-10-16): SDL3 3.4.14, SDL3_image
# 3.4.4, SDL3_mixer 3.2.4, SDL3_ttf 3.2.2 and FFmpeg 8.1.2. Used to check
# that Dirk builds and passes ctest against them before proposing a
# Batocera package. A normal desktop build (shared libraries, host glibc);
# it is NOT the build used for the "pret a copier" bundles (those are
# tools/build_linux_bundle_zig.sh).
#
# usage: tools/build_linux_batocera_libs.sh <src-dir> <prefix>
#   <src-dir> holds the release tarballs:
#     SDL3-3.4.14.tar.gz SDL3_image-3.4.4.tar.gz SDL3_mixer-3.2.4.tar.gz
#     SDL3_ttf-3.2.2.tar.gz ffmpeg-8.1.2.tar.xz
# Ubuntu 24.04 packages needed: build-essential cmake ninja-build nasm
#   pkg-config libx11-dev libxext-dev libxrandr-dev libxcursor-dev
#   libxi-dev libxfixes-dev libxss-dev libxtst-dev libasound2-dev
#   libpulse-dev libwayland-dev libxkbcommon-dev wayland-protocols
#   libdrm-dev libgbm-dev libegl-dev libgles-dev libudev-dev
#   libfreetype-dev libharfbuzz-dev
# Then build Dirk with:
#   PKG_CONFIG_PATH=<prefix>/lib/pkgconfig cmake -S . -B build-batocera -G Ninja \
#     -DCMAKE_BUILD_RPATH=<prefix>/lib
set -euo pipefail
SRC=$(realpath "$1"); PREFIX=$(realpath -m "$2"); JOBS=$(nproc)
W=$PREFIX-work; mkdir -p "$W" "$PREFIX"
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig

unpack() { rm -rf "$W/$2"; tar -C "$W" -xf "$SRC/$1"; }

cm() { # <dir> <extra cmake args...>
    local d=$1; shift
    cmake -S "$W/$d" -B "$W/$d/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_PREFIX_PATH="$PREFIX" "$@"
    cmake --build "$W/$d/build" -j"$JOBS"
    cmake --install "$W/$d/build"
}

unpack SDL3-3.4.14.tar.gz SDL3-3.4.14
cm SDL3-3.4.14 -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF

unpack SDL3_image-3.4.4.tar.gz SDL3_image-3.4.4
cm SDL3_image-3.4.4 -DSDLIMAGE_VENDORED=OFF -DSDLIMAGE_SAMPLES=OFF -DSDLIMAGE_TESTS=OFF \
   -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_JXL=OFF -DSDLIMAGE_TIF=OFF -DSDLIMAGE_WEBP=OFF

unpack SDL3_mixer-3.2.4.tar.gz SDL3_mixer-3.2.4
cm SDL3_mixer-3.2.4 -DSDLMIXER_VENDORED=OFF -DSDLMIXER_EXAMPLES=OFF -DSDLMIXER_TESTS=OFF \
   -DSDLMIXER_GME=OFF -DSDLMIXER_MOD=OFF -DSDLMIXER_MIDI=OFF -DSDLMIXER_WAVPACK=OFF \
   -DSDLMIXER_OPUS=OFF -DSDLMIXER_FLAC_LIBFLAC=OFF -DSDLMIXER_MP3_MPG123=OFF

unpack SDL3_ttf-3.2.2.tar.gz SDL3_ttf-3.2.2
cm SDL3_ttf-3.2.2 -DSDLTTF_VENDORED=OFF -DSDLTTF_HARFBUZZ=ON -DSDLTTF_PLUTOSVG=OFF -DSDLTTF_SAMPLES=OFF

if [ ! -f "$SRC/ffmpeg-8.1.2.tar.xz" ]; then echo "ffmpeg-8.1.2.tar.xz missing: FFmpeg skipped"; exit 0; fi
unpack ffmpeg-8.1.2.tar.xz ffmpeg-8.1.2
( cd "$W/ffmpeg-8.1.2" && ./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" \
      --enable-shared --disable-static --disable-programs --disable-doc \
      --disable-network --disable-autodetect --enable-zlib --enable-pic \
  && make -j"$JOBS" && make install )

echo "done: $PREFIX"
