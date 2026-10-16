#!/usr/bin/env bash
# Builds a MINIMAL, STATIC ffmpeg for dirk's Windows (DIRK_STATIC=ON) build --
# only the pieces dirk actually uses (H.264 + H.265/HEVC video, AAC + MP3
# audio, the MP4/MOV container, local file reading), nothing else. Run this
# in an "MSYS2 MinGW64" shell (NOT the plain "MSYS2" shell -- that one has
# the wrong toolchain in PATH).
#
# Why a custom build instead of MSYS2's own mingw-w64-x86_64-ffmpeg package:
# that package is a general-purpose build with dozens of optional codecs
# enabled (x264, x265, aom, dav1d, vpx, openmpt, gnutls, ...). None of those
# are things dirk decodes -- it only ever calls avformat/avcodec to read a
# game's own .mp4 movie -- but statically linking still requires every one
# of THOSE libraries' .a files to be present too, which is what produced the
# wall of "cannot find -lxxx" errors when DIRK_STATIC=ON was first tried
# against that package. A trimmed build sidesteps the whole problem: if a
# decoder/demuxer was never compiled in, static linking never asks for its
# dependencies either.
#
# Note on H.265/HEVC specifically: ffmpeg's HEVC *decoder* is native code
# inside libavcodec, no external library needed (x265 is an ENCODER only --
# irrelevant here, since dirk only ever plays movies back, never encodes).
#
# 2026-09-22 (Cyril): added mpeg2video/vorbis decoding + the mpegvideo (raw
# .m2v elementary stream) and ogg demuxers, plus matroska -- BOTH the muxer
# AND the demuxer -- for Hypseus/Singe-1-style games whose video and audio
# ship as two separate files (Video/<name>.m2v + Video/<name>.ogg, the
# classic Daphne-era LaserDisc rip convention, still required by Hypseus
# today) rather than one muxed .mp4 like every KimmyScript game seen so far.
# dirk remuxes that pair into a single Movie/<name>.mkv itself, once, the
# first time that game is launched (see HypseusRemux.cpp/.h) -- no external
# ffmpeg.exe, no manual step for whoever's launching the game, just this one
# extra codec set baked into the binary. Measured cost on a same-flags Linux
# build (this exact toolchain isn't available to measure directly on
# Windows, but the order of magnitude should hold): a stripped test binary
# linking the same way dirk does grew from 6012552 to 6373000 bytes with
# this whole set added (decoders + demuxers + the matroska muxer together)
# -- +352KB, small next to the H.264/HEVC decoders already here.
#
# 2026-09-22, correction (same day): the first version of this line only
# had `--enable-muxer=matroska`, without also enabling the matroska
# DEMUXER. dirk's own HypseusRemux writes the .mkv fine either way (that
# only needs the muxer), but VideoSource could then never correctly read
# that .mkv back -- confirmed directly (not guessed) by reproducing your
# exact failure here with a matching minimal ffmpeg: avformat_open_input()
# silently mis-detected the .mkv as a headerless raw MPEG-2 stream (the
# only demuxer left standing that would even attempt it) instead of
# matroska, which is also why no audio was found: that fallback demuxer
# doesn't know matroska has a second (audio) track at all. This is also
# the direct cause of the "no sound" you reported -- it wasn't a real
# muxing/encoding problem, the audio track was never being read in the
# first place under the old flags. `matroska` is now listed on BOTH
# --enable-demuxer and --enable-muxer below -- this is the actual fix.
#
# Usage:
#   ./build_ffmpeg_minimal_windows.sh [install-prefix]
# Default install-prefix: $HOME/ffmpeg-dirk-static
#
# After this completes, point dirk's own build at the result (see the
# printed instructions at the end, and README.md).

set -euo pipefail

PREFIX="${1:-$HOME/ffmpeg-dirk-static}"
JOBS="$(nproc 2>/dev/null || echo 4)"

echo "== dirk: minimal static ffmpeg build =="
echo "install prefix: $PREFIX"
echo "parallel jobs:  $JOBS"
echo

# --- 0. Sanity checks -------------------------------------------------
if [[ "${MSYSTEM:-}" != "MINGW64" ]]; then
    echo "This must run in an 'MSYS2 MinGW64' shell (found MSYSTEM='${MSYSTEM:-<unset>}')." >&2
    echo "Open 'MSYS2 MinGW64' from the Start menu, not 'MSYS2 MSYS' or 'MSYS2 UCRT64'." >&2
    exit 1
fi

command -v gcc >/dev/null || { echo "gcc not found -- pacman -S --needed mingw-w64-x86_64-toolchain" >&2; exit 1; }
command -v nasm >/dev/null || { echo "nasm not found -- pacman -S --needed mingw-w64-x86_64-nasm" >&2; exit 1; }
command -v pkg-config >/dev/null || { echo "pkg-config not found -- pacman -S --needed mingw-w64-x86_64-pkgconf" >&2; exit 1; }
command -v make >/dev/null || { echo "make not found -- pacman -S --needed make" >&2; exit 1; }
command -v git >/dev/null || { echo "git not found -- pacman -S --needed git" >&2; exit 1; }

# --- 1. Get the source --------------------------------------------------
SRC_DIR="$HOME/ffmpeg-dirk-src"
if [[ ! -d "$SRC_DIR" ]]; then
    echo "-- cloning ffmpeg (release/7.1 branch) --"
    git clone --depth 1 --branch release/7.1 https://github.com/FFmpeg/FFmpeg.git "$SRC_DIR"
else
    echo "-- reusing existing source checkout at $SRC_DIR --"
fi
cd "$SRC_DIR"

# --- 2. Configure: everything off, then only what dirk needs back on ---
# See VideoSource.cpp for exactly what dirk calls (avformat_open_input,
# avcodec_*, sws_scale, swr_convert) -- this list is deliberately just wide
# enough for "an .mp4 with an H.264 or HEVC video track and an AAC or MP3
# audio track", which is what every game movie seen so far uses.
#
# --disable-autodetect only covers the "--enable-libXXX" style external
# encoder/decoder libraries (x264, mp3lame, ...); it does NOT reliably
# stop hardware-acceleration contexts (vaapi, ...) or the optional soxr
# resampler from being auto-enabled when configure finds their headers/
# .pc files lying around in the MSYS2 install (which it did here: MSYS2
# ships mingw-w64-x86_64-libva and mingw-w64-x86_64-libsoxr as
# dependencies of *other* packages, and ffmpeg's configure happily
# autodetects both regardless of --disable-autodetect). dirk plays back
# small pre-rendered game movies with plain software decode -- no
# hardware acceleration needed at all -- so these are disabled by name
# explicitly rather than relying on autodetect being off.
./configure \
    --prefix="$PREFIX" \
    --enable-static \
    --disable-shared \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-avfilter \
    --disable-postproc \
    --disable-network \
    --disable-autodetect \
    --disable-everything \
    --disable-vaapi \
    --disable-vdpau \
    --disable-d3d11va \
    --disable-dxva2 \
    --disable-cuvid \
    --disable-nvenc \
    --disable-nvdec \
    --disable-videotoolbox \
    --disable-audiotoolbox \
    --disable-v4l2-m2m \
    --disable-libsoxr \
    --enable-decoder=h264,hevc,aac,mp3float,mpeg2video,vorbis \
    --enable-parser=h264,hevc,aac,mpegaudio,mpeg2video,mpegvideo,vorbis \
    --enable-demuxer=mov,mpegvideo,ogg,matroska \
    --enable-muxer=matroska \
    --enable-protocol=file \
    --enable-swscale \
    --enable-swresample

# --- 3. Build + install --------------------------------------------------
make -j"$JOBS"
make install

echo
echo "== done =="
echo "Static ffmpeg installed to: $PREFIX"
echo
echo "To build dirk against it (from dirk's build/ directory):"
echo
echo "  PKG_CONFIG_PATH=\"$PREFIX/lib/pkgconfig\" cmake .. -DDIRK_STATIC=ON"
echo "  cmake --build ."
echo
echo "(Deliberately no -G here -- an existing build/ directory already has a"
echo "generator locked in from its first configure; passing a different one"
echo "errors out instead of switching. Only pass -G on a brand-new build/.)"
echo
echo "PKG_CONFIG_PATH takes priority over the system mingw-w64-x86_64-ffmpeg"
echo "package's own .pc files, so this minimal build is the one CMake's"
echo "pkg_check_modules(... libavformat/libavcodec/libavutil/libswscale/"
echo "libswresample) picks up -- no CMakeLists.txt changes needed."
