#!/usr/bin/env bash
# Builds MINIMAL, STATIC versions of SDL3_image, SDL3_ttf and SDL3_mixer for
# dirk's Windows (DIRK_STATIC=ON) build -- SDL3 equivalent of
# tools/build_sdl_satellites_minimal_windows.sh (the SDL2 one), same reason:
# the MSYS2 packages for these are general-purpose builds (every image
# format, full text-shaping stack, every tracker/audio format) that pull in
# a big cascade of DLLs dirk never actually uses. Same three restrictions as
# the SDL2 script: PNG only, FreeType only (no harfbuzz), WAV only.
#
# UNLIKE the SDL2 satellites, SDL3 core itself does NOT need a custom build
# here -- MSYS2 already packages it (mingw-w64-x86_64-sdl3, confirmed on
# packages.msys2.org as version 3.4.16, the exact version this was tested
# against). Install it the normal way, once, before running this script:
#   pacman -S --needed mingw-w64-x86_64-sdl3
#
# Run this in an "MSYS2 MinGW64" shell (same as the ffmpeg script).
#
# UNTESTED end to end on real Windows/MinGW -- there's no Windows toolchain
# available on my side to verify this against. What IS verified: these exact
# option names (SDLIMAGE_*/SDLTTF_*/SDLMIXER_* -- no "2" or "3" in the
# prefix, confirmed by reading the real CMakeLists.txt of each library's
# SDL3 branch) and these exact tags build cleanly together against a real
# SDL3 3.4.16 install on Linux. If configure rejects an option name anyway
# (a newer/older tag renamed something), `cmake -S . -B build -LH | grep -i
# <keyword>` from inside that library's source dir lists the names it
# actually understands -- send me that output rather than guessing further.
#
# Usage:
#   ./build_sdl3_satellites_minimal_windows.sh [install-prefix]
# Default install-prefix: $HOME/sdl3-satellites-dirk-static

set -euo pipefail

PREFIX="${1:-$HOME/sdl3-satellites-dirk-static}"
JOBS="$(nproc 2>/dev/null || echo 4)"
SRC_ROOT="$HOME/sdl3-satellites-src"

echo "== dirk: minimal static SDL3_image/SDL3_ttf/SDL3_mixer build =="
echo "install prefix: $PREFIX"
echo

if [[ "${MSYSTEM:-}" != "MINGW64" ]]; then
    echo "This must run in an 'MSYS2 MinGW64' shell (found MSYSTEM='${MSYSTEM:-<unset>}')." >&2
    exit 1
fi
command -v cmake >/dev/null || { echo "cmake not found -- pacman -S --needed mingw-w64-x86_64-cmake" >&2; exit 1; }
command -v git >/dev/null || { echo "git not found -- pacman -S --needed git" >&2; exit 1; }
pkg-config --exists sdl3 || {
    echo "SDL3 core not found via pkg-config -- install it first:" >&2
    echo "  pacman -S --needed mingw-w64-x86_64-sdl3" >&2
    exit 1
}

mkdir -p "$SRC_ROOT"

build_one() {
    local name="$1" repo="$2" tag="$3"; shift 3
    local src_dir="$SRC_ROOT/$name"
    if [[ ! -d "$src_dir" ]]; then
        echo "-- cloning $name ($tag) --"
        git clone --depth 1 --branch "$tag" --recursive "$repo" "$src_dir"
    else
        echo "-- reusing existing checkout: $src_dir --"
    fi
    cmake -S "$src_dir" -B "$src_dir/build" -G "MSYS Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DBUILD_SHARED_LIBS=OFF \
        "$@"
    cmake --build "$src_dir/build" -j"$JOBS"
    cmake --install "$src_dir/build"
}

# --- SDL3_image: PNG only ------------------------------------------------
# Tag pinned to release-3.4.6 -- the exact version already confirmed (on a
# Linux sandbox) to build cleanly against SDL3 3.4.16.
# VENDORED=ON makes it build its own copy of zlib+libpng from its submodules
# instead of depending on system ones -- so libpng-16.dll/zlib1.dll drop out
# of the final `ldd` too, not just the exotic formats.
build_one SDL3_image https://github.com/libsdl-org/SDL_image.git release-3.4.6 \
    -DSDLIMAGE_VENDORED=ON \
    -DSDLIMAGE_PNG=ON \
    -DSDLIMAGE_ANI=OFF \
    -DSDLIMAGE_AVIF=OFF \
    -DSDLIMAGE_BMP=OFF \
    -DSDLIMAGE_GIF=OFF \
    -DSDLIMAGE_JPG=OFF \
    -DSDLIMAGE_JXL=OFF \
    -DSDLIMAGE_LBM=OFF \
    -DSDLIMAGE_PCX=OFF \
    -DSDLIMAGE_PNM=OFF \
    -DSDLIMAGE_QOI=OFF \
    -DSDLIMAGE_SVG=OFF \
    -DSDLIMAGE_TGA=OFF \
    -DSDLIMAGE_TIF=OFF \
    -DSDLIMAGE_WEBP=OFF \
    -DSDLIMAGE_XCF=OFF \
    -DSDLIMAGE_XPM=OFF \
    -DSDLIMAGE_XV=OFF \
    -DSDLIMAGE_SAMPLES=OFF \
    -DSDLIMAGE_TESTS=OFF

# --- SDL3_ttf: FreeType only, no HarfBuzz text shaping, no color emoji --
# Tag pinned to release-3.2.2 (confirmed against SDL3 3.4.16, same as above).
# FreeType itself is no longer optional in this SDL3_ttf version (always
# on) -- only HARFBUZZ and PLUTOSVG (color emoji via SVG glyphs, irrelevant
# for dirk's plain settings-screen text) are the real switches to flip off.
build_one SDL3_ttf https://github.com/libsdl-org/SDL_ttf.git release-3.2.2 \
    -DSDLTTF_VENDORED=ON \
    -DSDLTTF_HARFBUZZ=OFF \
    -DSDLTTF_PLUTOSVG=OFF \
    -DSDLTTF_SAMPLES=OFF

# --- SDL3_mixer: WAV only -------------------------------------------------
# Tag pinned to release-3.2.4 (confirmed against SDL3 3.4.16, same as
# above). Note this is the MIX_* API rewrite (no Mix_Chunk/channels) that
# dirk-engine-sdl3's own code already targets -- see api_singe.cpp.
build_one SDL3_mixer https://github.com/libsdl-org/SDL_mixer.git release-3.2.4 \
    -DSDLMIXER_VENDORED=ON \
    -DSDLMIXER_WAVE=ON \
    -DSDLMIXER_AIFF=OFF \
    -DSDLMIXER_VOC=OFF \
    -DSDLMIXER_AU=OFF \
    -DSDLMIXER_FLAC=OFF \
    -DSDLMIXER_GME=OFF \
    -DSDLMIXER_MOD=OFF \
    -DSDLMIXER_MP3=OFF \
    -DSDLMIXER_MIDI=OFF \
    -DSDLMIXER_OPUS=OFF \
    -DSDLMIXER_VORBIS_STB=OFF \
    -DSDLMIXER_VORBIS_VORBISFILE=OFF \
    -DSDLMIXER_WAVPACK=OFF \
    -DSDLMIXER_EXAMPLES=OFF \
    -DSDLMIXER_TESTS=OFF

echo
echo "== done =="
echo "Static SDL3_image/SDL3_ttf/SDL3_mixer installed to: $PREFIX"
echo
echo "Combine with the ffmpeg prefix when configuring dirk (SDL3 core itself"
echo "comes from the system mingw-w64-x86_64-sdl3 package, already on the"
echo "default PKG_CONFIG_PATH -- only these 3 satellites + ffmpeg need the"
echo "explicit path below):"
echo
echo "  PKG_CONFIG_PATH=\"\$HOME/ffmpeg-dirk-static/lib/pkgconfig:$PREFIX/lib/pkgconfig\" \\"
echo "    cmake .. -DDIRK_STATIC=ON"
echo "  cmake --build ."
echo
echo "(Deliberately no -G here -- an existing build/ directory already has a"
echo "generator locked in from its first configure; passing a different one"
echo "errors out instead of switching. Only pass -G on a brand-new build/.)"
