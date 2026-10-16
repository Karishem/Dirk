#!/bin/bash
# Builds the Linux x86_64 Dirk bundle for Batocera (2026-10-01):
#   <out>/dirk/dirk             the program (ffmpeg minimal + Lua + C++
#                               runtime linked in; RUNPATH $ORIGIN/lib)
#   <out>/dirk/lib/             SDL3, SDL3_mixer/image/ttf + the libraries
#                               they load on demand (PNG, Ogg Vorbis)
#   <out>/dirk/dirk-launch.sh   launcher called by EmulationStation
#   <out>/configs/es_systems_dirk.cfg
#
# Needs: an SDL3 prefix (SDL3 3.4 + satellites, all video/audio backends
# loaded on demand -- X11, Wayland, KMSDRM, PipeWire, PulseAudio, ALSA),
# nasm, Lua 5.1 static library (liblua5.1-dev).
# usage: tools/build_linux_bundle.sh <sdl3-prefix> <out-dir>
set -euo pipefail
SDL_PREFIX=$(realpath "$1"); OUT=$(realpath -m "$2")
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$HOME/dirk-linux-work}
FF_PREFIX=$WORK/ffmpeg-dirk-linux
JOBS=$(nproc)
mkdir -p "$WORK"

# 1. ffmpeg 7.1, same minimal decode-only LGPL configuration as Windows.
if [ ! -f "$FF_PREFIX/lib/libavcodec.a" ]; then
    [ -d "$WORK/ffmpeg-src" ] || git clone --depth 1 --branch release/7.1 https://github.com/FFmpeg/FFmpeg.git "$WORK/ffmpeg-src"
    (cd "$WORK/ffmpeg-src" && ./configure --prefix="$FF_PREFIX" --enable-static --disable-shared --enable-pic \
        --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-postproc \
        --disable-network --disable-autodetect --disable-everything --disable-vaapi --disable-vdpau \
        --disable-cuvid --disable-nvenc --disable-nvdec --disable-v4l2-m2m --disable-libsoxr \
        --enable-decoder=h264,hevc,aac,mp3float,mpeg2video,vorbis \
        --enable-parser=h264,hevc,aac,mpegaudio,mpeg2video,mpegvideo,vorbis \
        --enable-demuxer=mov,mpegvideo,ogg,matroska --enable-muxer=matroska --enable-protocol=file \
        --enable-swscale --enable-swresample && make -j"$JOBS" && make install)
fi

# 2. Lua 5.1 linked in: a pkg-config file pointing at a folder holding only
#    the static library (so the linker can't pick the shared one).
LUA_A=$(ls /usr/lib/*/liblua5.1.a | head -1)
mkdir -p "$WORK/pc/lib"; ln -sf "$LUA_A" "$WORK/pc/lib/liblua5.1.a"
cat > "$WORK/pc/lua5.1.pc" <<PC
Name: Lua
Description: Lua 5.1, static
Version: 5.1.5
Libs: -L$WORK/pc/lib -llua5.1 -ldl -lm
Cflags: -I/usr/include/lua5.1
PC

# 3. dirk.
PCP="$WORK/pc:$FF_PREFIX/lib/pkgconfig:$SDL_PREFIX/lib/pkgconfig"
mkdir -p "$WORK/build" && cd "$WORK/build"
PKG_CONFIG_PATH="$PCP" PKG_CONFIG_LIBDIR="$PCP" cmake "$HERE" -DCMAKE_BUILD_TYPE=Release -DCMAKE_SKIP_RPATH=ON \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc -Wl,-rpath,\$ORIGIN/lib -Wl,--enable-new-dtags"
cmake --build . --target dirk -j"$JOBS"

# 4. The bundle.
rm -rf "$OUT"; mkdir -p "$OUT/dirk/lib" "$OUT/configs"
install -m755 dirk "$OUT/dirk/dirk"; strip "$OUT/dirk/dirk"
for l in libSDL3.so.0 libSDL3_mixer.so.0 libSDL3_image.so.0 libSDL3_ttf.so.0 \
         libpng16.so.16 libvorbisfile.so.3 libvorbis.so.0 libogg.so.0; do
    cp -L "$SDL_PREFIX/lib/$l" "$OUT/dirk/lib/"
done
strip --strip-unneeded "$OUT"/dirk/lib/*
install -m755 "$HERE/batocera/dirk-launch.sh" "$OUT/dirk/"
cp "$HERE/batocera/es_systems_dirk.cfg" "$OUT/configs/"
echo "== bundle in $OUT; needs glibc $(objdump -T "$OUT/dirk/dirk" "$OUT"/dirk/lib/*.so.* | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)"
