#!/bin/bash
# Builds the Linux ARM64 (aarch64) Dirk bundle for Batocera on Raspberry Pi
# 4/5 and ARM handhelds (2026-10-02), cross-compiled from an x86_64 Linux.
# Same layout as tools/build_linux_bundle.sh (x86_64):
#   <out>/dirk/dirk, <out>/dirk/lib/ (SDL3 + mixer/image/ttf, self-contained),
#   <out>/dirk/dirk-launch.sh, <out>/configs/es_systems_dirk.cfg
#
# Needs: aarch64-linux-gnu-gcc/g++ (Ubuntu: gcc-aarch64-linux-gnu,
# g++-aarch64-linux-gnu), cmake, nasm not needed, wayland-scanner, the
# HOST's development headers of X11/Wayland/xkbcommon/libdrm/gbm/EGL/GLES/
# ALSA/PulseAudio/PipeWire/udev/libdecor (only headers are used: SDL loads
# all of those libraries at run time, so a "stub" sysroot with empty ARM64
# libraries carrying the right names is enough to configure SDL), and the
# sources: SDL3, SDL3_image, SDL3_mixer, SDL3_ttf (with their external/
# vendored dependencies), FFmpeg release/7.1, Lua 5.1.5.
# usage: tools/build_linux_bundle_arm64.sh <src-dir> <out-dir>
#   <src-dir> holds SDL3/ SDL_image/ SDL_mixer/ SDL_ttf/ ffmpeg/ lua-5.1.5/
set -euo pipefail
SRC=$(realpath "$1"); OUT=$(realpath -m "$2")
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=${WORK:-$HOME/dirk-arm64-work}; JOBS=$(nproc)
STUB=$W/stub; PREFIX=$W/prefix; FF=$W/ffmpeg; LUA=$W/lua
mkdir -p "$W"

# 1. Stub sysroot: host headers + empty ARM64 libraries with real sonames.
rm -rf "$STUB"; mkdir -p "$STUB/usr/include" "$STUB/usr/lib/pkgconfig"
for d in X11 xkbcommon EGL GLES2 GLES3 KHR GL alsa pulse pipewire-0.3 spa-0.2 libdrm libdecor-0; do ln -s /usr/include/$d "$STUB/usr/include/$d"; done
for f in /usr/include/wayland-*.h /usr/include/gbm.h /usr/include/xf86drm.h /usr/include/xf86drmMode.h /usr/include/libudev.h; do ln -s "$f" "$STUB/usr/include/"; done
echo 'void dirk_stub(void){}' > "$W/stub.c"
mk() { aarch64-linux-gnu-gcc -shared -fPIC -o "$STUB/usr/lib/$2.0" "$W/stub.c" -Wl,-soname,$2
       ln -sf $2.0 "$STUB/usr/lib/$2"; ln -sf $2 "$STUB/usr/lib/$1"; }
for p in X11:6 Xext:6 Xcursor:1 Xi:6 Xfixes:3 Xrandr:2 Xrender:1 Xss:1 Xtst:6 asound:2 pulse:0 pipewire-0.3:0 udev:1 \
         drm:2 gbm:1 EGL:1 GLESv2:2 wayland-client:0 wayland-egl:1 wayland-cursor:0 xkbcommon:0 decor-0:0; do
    mk lib${p%%:*}.so lib${p%%:*}.so.${p##*:}
done
pc() { printf 'prefix=%s/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: %s\nDescription: stub\nVersion: %s\nLibs: -L${libdir} %s\nCflags: -I${includedir} %s\n' \
       "$STUB" "$1" "$2" "$3" "${4:-}" > "$STUB/usr/lib/pkgconfig/$1.pc"; }
pc wayland-client 1.22.0 -lwayland-client; pc wayland-egl 18.1.0 -lwayland-egl; pc wayland-cursor 1.22.0 -lwayland-cursor
pc egl 1.5 -lEGL; pc glesv2 3.2 -lGLESv2; pc xkbcommon 1.6.0 -lxkbcommon; pc libdecor-0 0.2.2 -ldecor-0 '-I${includedir}/libdecor-0'
pc libdrm 2.4.120 -ldrm '-I${includedir}/libdrm'; pc gbm 24.0 -lgbm; pc alsa 1.2.11 -lasound; pc libpulse 16.1 -lpulse
pc libpipewire-0.3 1.0.5 -lpipewire-0.3 '-I${includedir}/pipewire-0.3 -I${includedir}/spa-0.2 -D_REENTRANT'; pc libudev 255 -ludev
for x in x11:X11 xext:Xext xcursor:Xcursor xi:Xi xfixes:Xfixes xrandr:Xrandr xrender:Xrender xscrnsaver:Xss xtst:Xtst; do pc ${x%%:*} 1.8 -l${x##*:}; done

cat > "$W/toolchain.cmake" <<TC
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH $STUB $PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
TC
export PKG_CONFIG_LIBDIR="$STUB/usr/lib/pkgconfig:$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=
C="-DCMAKE_TOOLCHAIN_FILE=$W/toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX"

# 2. SDL3 (every backend loaded at run time) and its satellites (vendored
#    dependencies linked in: nothing else to ship).
cmake -S "$SRC/SDL3" -B "$W/b-sdl" $C -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_STATIC=OFF \
    -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_LIBURING=OFF -DSDL_LIBUSB=OFF \
    -DSDL_FRIBIDI=OFF -DSDL_LIBTHAI=OFF -DSDL_RPATH=OFF
cmake --build "$W/b-sdl" -j"$JOBS" && cmake --install "$W/b-sdl"
S="$C -DSDL3_DIR=$PREFIX/lib/cmake/SDL3 -DCMAKE_POSITION_INDEPENDENT_CODE=ON"
cmake -S "$SRC/SDL_image" -B "$W/b-img" $S -DSDLIMAGE_VENDORED=ON -DSDLIMAGE_DEPS_SHARED=OFF -DSDLIMAGE_BACKEND_STB=ON \
    -DSDLIMAGE_PNG=ON -DSDLIMAGE_PNG_LIBPNG=ON -DSDLIMAGE_JPG=ON -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_TIF=OFF -DSDLIMAGE_WEBP=OFF \
    -DSDLIMAGE_JXL=OFF -DSDLIMAGE_SAMPLES=OFF -DSDLIMAGE_TESTS=OFF
cmake --build "$W/b-img" -j"$JOBS" && cmake --install "$W/b-img"
cmake -S "$SRC/SDL_mixer" -B "$W/b-mix" $S -DSDLMIXER_VENDORED=ON -DSDLMIXER_DEPS_SHARED=OFF -DSDLMIXER_VORBIS_STB=ON \
    -DSDLMIXER_VORBIS_VORBISFILE=OFF -DSDLMIXER_VORBIS_TREMOR=OFF -DSDLMIXER_OPUS=OFF -DSDLMIXER_FLAC_LIBFLAC=OFF \
    -DSDLMIXER_GME=OFF -DSDLMIXER_MOD=OFF -DSDLMIXER_MOD_XMP=OFF -DSDLMIXER_WAVPACK=OFF -DSDLMIXER_MIDI=OFF \
    -DSDLMIXER_MP3_MPG123=OFF -DSDLMIXER_SAMPLES=OFF -DSDLMIXER_TESTS=OFF -DSDLMIXER_EXAMPLES=OFF
cmake --build "$W/b-mix" -j"$JOBS" && cmake --install "$W/b-mix"
cmake -S "$SRC/SDL_ttf" -B "$W/b-ttf" $S -DSDLTTF_VENDORED=ON -DSDLTTF_HARFBUZZ=ON -DSDLTTF_PLUTOSVG=ON \
    -DSDLTTF_SAMPLES=OFF -DSDLTTF_TESTS=OFF
cmake --build "$W/b-ttf" -j"$JOBS" && cmake --install "$W/b-ttf"

# 3. FFmpeg 7.1, same minimal decode-only LGPL configuration as Windows/x86_64.
rm -rf "$W/ffmpeg-src"; cp -r "$SRC/ffmpeg" "$W/ffmpeg-src"
(cd "$W/ffmpeg-src" && (make distclean >/dev/null 2>&1 || true) && ./configure --prefix="$FF" --enable-cross-compile --arch=aarch64 --target-os=linux \
    --cross-prefix=aarch64-linux-gnu- --enable-static --disable-shared --enable-pic --disable-programs --disable-doc \
    --disable-avdevice --disable-avfilter --disable-postproc --disable-network --disable-autodetect --disable-everything \
    --disable-vaapi --disable-vdpau --disable-cuvid --disable-nvenc --disable-nvdec --disable-v4l2-m2m --disable-libsoxr \
    --enable-decoder=h264,hevc,aac,mp3float,mpeg2video,vorbis \
    --enable-parser=h264,hevc,aac,mpegaudio,mpeg2video,mpegvideo,vorbis \
    --enable-demuxer=mov,mpegvideo,ogg,matroska --enable-muxer=matroska --enable-protocol=file \
    --enable-swscale --enable-swresample && make -j"$JOBS" && make install)

# 4. Lua 5.1.5, static.
rm -rf "$LUA"; mkdir -p "$LUA/obj" "$LUA/lib" "$LUA/include"
for f in "$SRC"/lua-5.1.5/*.c; do
    case $(basename "$f") in lua.c|luac.c|print.c) continue;; esac
    aarch64-linux-gnu-gcc -O2 -fPIC -DLUA_USE_LINUX -c "$f" -o "$LUA/obj/$(basename "${f%.c}").o"
done
aarch64-linux-gnu-ar rcs "$LUA/lib/liblua5.1.a" "$LUA"/obj/*.o
cp "$SRC"/lua-5.1.5/{lua.h,luaconf.h,lualib.h,lauxlib.h} "$LUA/include/"
printf 'Name: Lua\nDescription: Lua 5.1.5 static\nVersion: 5.1.5\nLibs: -L%s/lib -llua5.1 -ldl -lm\nCflags: -I%s/include\n' "$LUA" "$LUA" > "$LUA/lua5.1.pc"

# 5. dirk.
export PKG_CONFIG_LIBDIR="$LUA:$FF/lib/pkgconfig:$PREFIX/lib/pkgconfig"
cmake -S "$HERE" -B "$W/b-dirk" $C -DCMAKE_SKIP_RPATH=ON \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc -Wl,-rpath,\$ORIGIN/lib -Wl,--enable-new-dtags"
cmake --build "$W/b-dirk" --target dirk -j"$JOBS"

# 6. The bundle.
rm -rf "$OUT"; mkdir -p "$OUT/dirk/lib" "$OUT/configs"
install -m755 "$W/b-dirk/dirk" "$OUT/dirk/dirk"; aarch64-linux-gnu-strip "$OUT/dirk/dirk"
for l in libSDL3.so.0 libSDL3_mixer.so.0 libSDL3_image.so.0 libSDL3_ttf.so.0; do cp -L "$PREFIX/lib/$l" "$OUT/dirk/lib/"; done
aarch64-linux-gnu-strip --strip-unneeded "$OUT"/dirk/lib/*
install -m755 "$HERE/batocera/dirk-launch.sh" "$OUT/dirk/"
cp "$HERE/batocera/es_systems_dirk.cfg" "$OUT/configs/"
echo "== ARM64 bundle in $OUT; needs glibc $(aarch64-linux-gnu-objdump -T "$OUT/dirk/dirk" "$OUT"/dirk/lib/*.so.0 | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)"
