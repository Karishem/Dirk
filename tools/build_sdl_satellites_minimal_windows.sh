#!/usr/bin/env bash
# Builds MINIMAL, STATIC versions of SDL2_image, SDL2_ttf and SDL2_mixer for
# dirk's Windows (DIRK_STATIC=ON) build -- same idea as
# build_ffmpeg_minimal_windows.sh, same reason: the MSYS2 packages for these
# are general-purpose builds (every image format, full text-shaping stack,
# every tracker/audio format) that pull in a big cascade of DLLs dirk never
# actually uses. Confirmed against Cyril's own `ldd dirk.exe` output:
#   - SDL2_image dragged in libjpeg/libtiff/libwebp/libavif/libjxl/cairo/
#     librsvg/pango -- dirk only ever spriteLoad()s .png files.
#   - SDL2_ttf dragged in harfbuzz/fontconfig/fribidi/graphite2 (full
#     Unicode text-shaping) -- dirk only renders plain Latin text for its
#     own settings screen (see LaunchSettings.cpp/EmbeddedFont.h).
#   - SDL2_mixer dragged in mpg123/FLAC/vorbis/opus/wavpack/xmp/lc3 -- dirk
#     only ever soundLoad()s .wav files, which SDL_mixer decodes with no
#     external library at all.
# So: PNG only, FreeType only (no harfbuzz), WAV only. Everything else off.
#
# Run this in an "MSYS2 MinGW64" shell (same as the ffmpeg script).
#
# UNTESTED end to end -- there's no Windows/MinGW toolchain available to
# verify this against, so treat the exact CMake option names below as "best
# knowledge, not confirmed for your exact installed version". If configure
# rejects one, `cmake -S . -B build -LH | grep -i <keyword>` from inside
# that library's source dir lists the option names it actually understands,
# and we adjust from there -- send me that output if something doesn't
# match, rather than guessing further.
#
# Usage:
#   ./build_sdl_satellites_minimal_windows.sh [install-prefix]
# Default install-prefix: $HOME/sdl2-satellites-dirk-static

set -euo pipefail

PREFIX="${1:-$HOME/sdl2-satellites-dirk-static}"
JOBS="$(nproc 2>/dev/null || echo 4)"
SRC_ROOT="$HOME/sdl2-satellites-src"

echo "== dirk: minimal static SDL2_image/SDL2_ttf/SDL2_mixer build =="
echo "install prefix: $PREFIX"
echo

if [[ "${MSYSTEM:-}" != "MINGW64" ]]; then
    echo "This must run in an 'MSYS2 MinGW64' shell (found MSYSTEM='${MSYSTEM:-<unset>}')." >&2
    exit 1
fi
command -v cmake >/dev/null || { echo "cmake not found -- pacman -S --needed mingw-w64-x86_64-cmake" >&2; exit 1; }
command -v ninja >/dev/null 2>&1 || true  # optional; falls back to make below
command -v git >/dev/null || { echo "git not found -- pacman -S --needed git" >&2; exit 1; }

mkdir -p "$SRC_ROOT"

build_one() {
    local name="$1" repo="$2" branch="$3"; shift 3
    local src_dir="$SRC_ROOT/$name"
    if [[ ! -d "$src_dir" ]]; then
        echo "-- cloning $name ($branch) --"
        git clone --depth 1 --branch "$branch" --recursive "$repo" "$src_dir"
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

# --- SDL2_image: PNG only ------------------------------------------------
# VENDORED=ON makes it build its own copy of zlib+libpng from its
# submodules instead of depending on the system ones -- so libpng-16.dll/
# zlib1.dll drop out of the final `ldd` too, not just the exotic formats.
build_one SDL2_image https://github.com/libsdl-org/SDL_image.git SDL2 \
    -DSDL2IMAGE_VENDORED=ON \
    -DSDL2IMAGE_PNG=ON \
    -DSDL2IMAGE_AVIF=OFF \
    -DSDL2IMAGE_JPG=OFF \
    -DSDL2IMAGE_JXL=OFF \
    -DSDL2IMAGE_TIF=OFF \
    -DSDL2IMAGE_WEBP=OFF \
    -DSDL2IMAGE_SVG=OFF \
    -DSDL2IMAGE_SAMPLES=OFF \
    -DSDL2IMAGE_TESTS=OFF

# --- SDL2_ttf: FreeType only, no HarfBuzz text shaping -------------------
build_one SDL2_ttf https://github.com/libsdl-org/SDL_ttf.git SDL2 \
    -DSDL2TTF_VENDORED=ON \
    -DSDL2TTF_HARFBUZZ=OFF \
    -DSDL2TTF_FREETYPE=ON \
    -DSDL2TTF_SAMPLES=OFF

# --- SDL2_mixer: WAV only -------------------------------------------------
build_one SDL2_mixer https://github.com/libsdl-org/SDL_mixer.git SDL2 \
    -DSDL2MIXER_VENDORED=ON \
    -DSDL2MIXER_WAV=ON \
    -DSDL2MIXER_FLAC=OFF \
    -DSDL2MIXER_MP3=OFF \
    -DSDL2MIXER_MOD=OFF \
    -DSDL2MIXER_MIDI=OFF \
    -DSDL2MIXER_OPUS=OFF \
    -DSDL2MIXER_VORBIS=OFF \
    -DSDL2MIXER_WAVPACK=OFF \
    -DSDL2MIXER_SAMPLES=OFF

echo
echo "== done =="
echo "Static SDL2_image/SDL2_ttf/SDL2_mixer installed to: $PREFIX"
echo
echo "Combine with the ffmpeg prefix when configuring dirk:"
echo
echo "  PKG_CONFIG_PATH=\"$HOME/ffmpeg-dirk-static/lib/pkgconfig:$PREFIX/lib/pkgconfig\" \\"
echo "    cmake .. -DDIRK_STATIC=ON"
echo "  cmake --build ."
echo
echo "(Deliberately no -G here -- an existing build/ directory already has a"
echo "generator locked in from its first configure; passing a different one"
echo "errors out instead of switching. Only pass -G on a brand-new build/.)"
