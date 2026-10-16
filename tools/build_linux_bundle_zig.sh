#!/bin/bash
# Builds the Linux Dirk bundle (x86_64 or aarch64) for an OLD glibc
# (2026-10-02): runs on Batocera, Steam Deck, Raspberry Pi and the handheld
# distributions (ArkOS, ROCKNIX, Knulli...), not only on recent systems.
# Zig is used as the C/C++ compiler only because it can target any glibc
# version (here 2.31) from any machine: `pip install ziglang`.
#
# Layout of the result, same as before:
#   <out>/dirk/dirk, <out>/dirk/lib/ (SDL3 + mixer/image/ttf, self-contained),
#   <out>/dirk/dirk-launch.sh, <out>/configs/es_systems_dirk.cfg
#
# SDL3 keeps every backend loaded at run time (X11, Wayland, KMSDRM,
# PipeWire/PulseAudio/ALSA, udev): only their HEADERS are needed here
# (host /usr/include), plus empty "stub" libraries with the right names so
# SDL's configure step records which library to load.
# usage: tools/build_linux_bundle_zig.sh <x86_64|aarch64> <src-dir> <out-dir>
#   <src-dir> holds SDL3/ SDL_image/ SDL_mixer/ SDL_ttf/ ffmpeg/ lua-5.1.5/
set -euo pipefail
ARCH=$1; SRC=$(realpath "$2"); OUT=$(realpath -m "$3")
GLIBC=${GLIBC:-2.31}
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=${WORK:-$HOME/dirk-zig-work}/$ARCH; JOBS=$(nproc)
STUB=$W/stub; PREFIX=$W/prefix; FF=$W/ffmpeg; LUA=$W/lua; T=$W/bin
TARGET=$ARCH-linux-gnu.$GLIBC
case $ARCH in x86_64) STRIP=strip; OBJDUMP=objdump; FFARCH=x86_64;; aarch64) STRIP=aarch64-linux-gnu-strip; OBJDUMP=aarch64-linux-gnu-objdump; FFARCH=aarch64;; *) echo "arch?"; exit 1;; esac
mkdir -p "$W" "$T"

# 0. Compiler wrappers (CMake and ffmpeg want plain commands).
ZIG=$(python3 -c 'import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),"zig"))')
for t in cc c++; do printf '#!/bin/sh\nexec "%s" %s -target %s "$@"\n' "$ZIG" $t $TARGET > "$T/$t"; done
for t in ar ranlib; do printf '#!/bin/sh\nexec "%s" %s "$@"\n' "$ZIG" $t > "$T/$t"; done
chmod +x "$T"/*

# 1. Stub sysroot: host headers + empty libraries with the real sonames.
rm -rf "$STUB"; mkdir -p "$STUB/usr/include" "$STUB/usr/lib/pkgconfig"
for d in X11 xcb xkbcommon EGL GLES2 GLES3 KHR GL alsa pulse pipewire-0.3 spa-0.2 libdrm libdecor-0; do ln -s /usr/include/$d "$STUB/usr/include/$d"; done
for f in /usr/include/wayland-*.h /usr/include/gbm.h /usr/include/xf86drm.h /usr/include/xf86drmMode.h /usr/include/libudev.h; do ln -s "$f" "$STUB/usr/include/"; done
echo 'void dirk_stub(void){}' > "$W/stub.c"
mk() { "$T/cc" -shared -fPIC -o "$STUB/usr/lib/$2.0" "$W/stub.c" -Wl,-soname,$2
       ln -sf $2.0 "$STUB/usr/lib/$2"; ln -sf $2 "$STUB/usr/lib/$1"; }
for p in X11:6 Xext:6 Xcursor:1 Xi:6 Xfixes:3 Xrandr:2 Xrender:1 Xss:1 Xtst:6 asound:2 pulse:0 pipewire-0.3:0 udev:1 \
         drm:2 gbm:1 EGL:1 GLESv2:2 GL:1 wayland-client:0 wayland-egl:1 wayland-cursor:0 xkbcommon:0 decor-0:0; do
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
set(CMAKE_SYSTEM_PROCESSOR $ARCH)
set(CMAKE_C_COMPILER $T/cc)
set(CMAKE_CXX_COMPILER $T/c++)
set(CMAKE_AR $T/ar)
set(CMAKE_RANLIB $T/ranlib)
set(CMAKE_FIND_ROOT_PATH $STUB $PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
TC
export PKG_CONFIG_LIBDIR="$STUB/usr/lib/pkgconfig:$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=
C="-DCMAKE_TOOLCHAIN_FILE=$W/toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX"

# 2. SDL3 and its satellites (vendored dependencies linked in).
# SDL3 fixes not yet in its release (tools/patches/sdl3-*.patch): applied
# once to the SDL3 source, skipped when already there.
for p in "$HERE"/tools/patches/sdl3-*.patch; do
    [ -f "$p" ] || continue
    if git -C "$SRC/SDL3" apply --check "$p" 2>/dev/null; then git -C "$SRC/SDL3" apply "$p"; echo "applied $(basename "$p")"
    elif git -C "$SRC/SDL3" apply --check -R "$p" 2>/dev/null; then echo "already applied: $(basename "$p")"
    else echo "cannot apply $(basename "$p")"; exit 1; fi
done
cmake -S "$SRC/SDL3" -B "$W/b-sdl" $C -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_STATIC=OFF \
    -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_LIBURING=OFF -DSDL_LIBUSB=OFF \
    -DSDL_FRIBIDI=OFF -DSDL_LIBTHAI=OFF -DSDL_RPATH=OFF
cmake --build "$W/b-sdl" -j"$JOBS"; cmake --install "$W/b-sdl"
S="$C -DSDL3_DIR=$PREFIX/lib/cmake/SDL3 -DCMAKE_POSITION_INDEPENDENT_CODE=ON"
cmake -S "$SRC/SDL_image" -B "$W/b-img" $S -DSDLIMAGE_VENDORED=ON -DSDLIMAGE_DEPS_SHARED=OFF -DSDLIMAGE_BACKEND_STB=ON \
    -DSDLIMAGE_PNG=ON -DSDLIMAGE_PNG_LIBPNG=ON -DSDLIMAGE_JPG=ON -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_TIF=OFF -DSDLIMAGE_WEBP=OFF \
    -DSDLIMAGE_JXL=OFF -DSDLIMAGE_SAMPLES=OFF -DSDLIMAGE_TESTS=OFF
cmake --build "$W/b-img" -j"$JOBS"; cmake --install "$W/b-img"
cmake -S "$SRC/SDL_mixer" -B "$W/b-mix" $S -DSDLMIXER_VENDORED=ON -DSDLMIXER_DEPS_SHARED=OFF -DSDLMIXER_VORBIS_STB=ON \
    -DSDLMIXER_VORBIS_VORBISFILE=OFF -DSDLMIXER_VORBIS_TREMOR=OFF -DSDLMIXER_OPUS=OFF -DSDLMIXER_FLAC_LIBFLAC=OFF \
    -DSDLMIXER_GME=OFF -DSDLMIXER_MOD=OFF -DSDLMIXER_MOD_XMP=OFF -DSDLMIXER_WAVPACK=OFF -DSDLMIXER_MIDI=OFF \
    -DSDLMIXER_MP3_MPG123=OFF -DSDLMIXER_SAMPLES=OFF -DSDLMIXER_TESTS=OFF -DSDLMIXER_EXAMPLES=OFF
cmake --build "$W/b-mix" -j"$JOBS"; cmake --install "$W/b-mix"
cmake -S "$SRC/SDL_ttf" -B "$W/b-ttf" $S -DSDLTTF_VENDORED=ON -DSDLTTF_HARFBUZZ=ON -DSDLTTF_PLUTOSVG=ON \
    -DSDLTTF_SAMPLES=OFF -DSDLTTF_TESTS=OFF
cmake --build "$W/b-ttf" -j"$JOBS"; cmake --install "$W/b-ttf"

# 3. FFmpeg 7.1, same minimal decode-only LGPL configuration as Windows.
rm -rf "$W/ffmpeg-src"; cp -r "$SRC/ffmpeg" "$W/ffmpeg-src"
(cd "$W/ffmpeg-src" && (make distclean >/dev/null 2>&1 || true) && ./configure --prefix="$FF" \
    --enable-cross-compile --arch=$FFARCH --target-os=linux --cc="$T/cc" --cxx="$T/c++" --ar="$T/ar" --ranlib="$T/ranlib" \
    --enable-static --disable-shared --enable-pic --disable-programs --disable-doc \
    --disable-avdevice --disable-avfilter --disable-postproc --disable-network --disable-autodetect --disable-everything \
    --disable-vaapi --disable-vdpau --disable-cuvid --disable-nvenc --disable-nvdec --disable-v4l2-m2m --disable-libsoxr \
    --enable-decoder=h264,hevc,aac,mp3float,mpeg2video,vorbis \
    --enable-parser=h264,hevc,aac,mpegaudio,mpeg2video,mpegvideo,vorbis \
    --enable-demuxer=mov,mpegvideo,ogg,matroska --enable-muxer=matroska --enable-protocol=file \
    --enable-swscale --enable-swresample)
# zig's glibc headers have no <sys/sysctl.h> (gone from glibc 2.32), but the
# 2.31 symbol exists, so configure believes it's usable: turn it off.
sed -i 's/#define HAVE_SYSCTL 1/#define HAVE_SYSCTL 0/' "$W/ffmpeg-src/config.h"
(cd "$W/ffmpeg-src" && make -j"$JOBS")
(cd "$W/ffmpeg-src" && make install)

# 4. Lua 5.1.5, static.
rm -rf "$LUA"; mkdir -p "$LUA/obj" "$LUA/lib" "$LUA/include"
for f in "$SRC"/lua-5.1.5/*.c; do
    case $(basename "$f") in lua.c|luac.c|print.c) continue;; esac
    "$T/cc" -O2 -fPIC -DLUA_USE_LINUX -c "$f" -o "$LUA/obj/$(basename "${f%.c}").o"
done
"$T/ar" rcs "$LUA/lib/liblua5.1.a" "$LUA"/obj/*.o
cp "$SRC"/lua-5.1.5/{lua.h,luaconf.h,lualib.h,lauxlib.h} "$LUA/include/"
printf 'Name: Lua\nDescription: Lua 5.1.5 static\nVersion: 5.1.5\nLibs: -L%s/lib -llua5.1 -ldl -lm\nCflags: -I%s/include\n' "$LUA" "$LUA" > "$LUA/lua5.1.pc"

# 5. dirk (zig's C++ runtime, libc++, is linked in).
export PKG_CONFIG_LIBDIR="$LUA:$FF/lib/pkgconfig:$PREFIX/lib/pkgconfig"
cmake -S "$HERE" -B "$W/b-dirk" $C -DCMAKE_SKIP_RPATH=ON "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath,\$ORIGIN/lib -Wl,--enable-new-dtags"
cmake --build "$W/b-dirk" --target dirk -j"$JOBS"

# 6. The bundle.
rm -rf "$OUT"; mkdir -p "$OUT/dirk/lib" "$OUT/configs"
install -m755 "$W/b-dirk/dirk" "$OUT/dirk/dirk"; $STRIP "$OUT/dirk/dirk"
for l in libSDL3.so.0 libSDL3_mixer.so.0 libSDL3_image.so.0 libSDL3_ttf.so.0; do cp -L "$PREFIX/lib/$l" "$OUT/dirk/lib/"; done
$STRIP --strip-unneeded "$OUT"/dirk/lib/*
install -m755 "$HERE/batocera/dirk-launch.sh" "$OUT/dirk/"
cp "$HERE/batocera/es_systems_dirk.cfg" "$OUT/configs/"
echo "== $ARCH bundle in $OUT; needs glibc $($OBJDUMP -T "$OUT/dirk/dirk" "$OUT"/dirk/lib/*.so.0 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)"
