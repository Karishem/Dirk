#!/bin/bash
# Builds Dirk for Android (arm64-v8a) and packs it into an APK, without
# Gradle: only the Android SDK command-line tools (javac, aapt2, d8,
# zipalign, apksigner) and the NDK are used, so no Maven repository is
# needed. Everything Dirk uses (SDL3 and its satellites, FFmpeg, Lua) is
# linked statically into a single libmain.so, which SDL's Java side loads.
#
# usage: tools/build_android.sh <src-dir> <out-dir> [libs|dirk|apk|all]
#   <src-dir> holds SDL3/ SDL_image/ SDL_mixer/ SDL_ttf/ ffmpeg/ (same
#   sources as tools/build_linux_bundle_zig.sh)
# Needs ANDROID_HOME (platforms;android-35, build-tools;35.0.0,
# ndk;28.2.13676358) and a JDK 17 or later.
# Signing: DIRK_KEYSTORE (default: a test key made in $WORK), password
# DIRK_KS_PASS, alias dirk. DIRK_UNSIGNED=1 skips signing (see build_apk).
# DIRK_VERSION_NAME and DIRK_VERSION_CODE set the APK's version.
set -euo pipefail
SRC=$(realpath "$1"); OUT=$(realpath -m "$2"); STEP=${3:-all}
HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${ANDROID_HOME:?set ANDROID_HOME}"
NDK=${ANDROID_NDK:-$ANDROID_HOME/ndk/28.2.13676358}
BT=$ANDROID_HOME/build-tools/35.0.0
PLATFORM_JAR=$ANDROID_HOME/platforms/android-35/android.jar
API=30
ABI=arm64-v8a
W=${WORK:-$HOME/dirk-android-work}; JOBS=$(nproc)
PREFIX=$W/prefix
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
mkdir -p "$W" "$PREFIX"

C="-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API
   -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_PREFIX_PATH=$PREFIX
   -DCMAKE_FIND_ROOT_PATH=$PREFIX -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -G Ninja"

build_libs() {
    # 1. SDL3, static (its Java half is compiled with the APK below).
    cmake -S "$SRC/SDL3" -B "$W/b-sdl" $C -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_TEST_LIBRARY=OFF -DSDL_ANDROID_JAR=OFF
    cmake --build "$W/b-sdl" -j"$JOBS"; cmake --install "$W/b-sdl"
    S="$C -DSDL3_DIR=$PREFIX/lib/cmake/SDL3"
    # 2. Satellites, same options as the Linux bundle, vendored and static.
    cmake -S "$SRC/SDL_image" -B "$W/b-img" $S -DSDLIMAGE_VENDORED=ON -DSDLIMAGE_DEPS_SHARED=OFF -DSDLIMAGE_BACKEND_STB=ON \
        -DSDLIMAGE_PNG=ON -DSDLIMAGE_PNG_LIBPNG=ON -DSDLIMAGE_JPG=ON -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_TIF=OFF -DSDLIMAGE_WEBP=OFF \
        -DSDLIMAGE_JXL=OFF -DSDLIMAGE_SAMPLES=OFF -DSDLIMAGE_TESTS=OFF
    cmake --build "$W/b-img" -j"$JOBS"; cmake --install "$W/b-img"
    # Its .pc names the vendored libpng/zlib with unexpanded CMake
    # generator expressions: give pkg-config the real library names.
    sed -i 's/^Libs.private: .*png_static.*/Libs.private: -lpng16 -lz/' "$PREFIX/lib/pkgconfig/sdl3-image.pc"
    cmake -S "$SRC/SDL_mixer" -B "$W/b-mix" $S -DSDLMIXER_VENDORED=ON -DSDLMIXER_DEPS_SHARED=OFF -DSDLMIXER_VORBIS_STB=ON \
        -DSDLMIXER_VORBIS_VORBISFILE=OFF -DSDLMIXER_VORBIS_TREMOR=OFF -DSDLMIXER_OPUS=OFF -DSDLMIXER_FLAC_LIBFLAC=OFF \
        -DSDLMIXER_GME=OFF -DSDLMIXER_MOD=OFF -DSDLMIXER_MOD_XMP=OFF -DSDLMIXER_WAVPACK=OFF -DSDLMIXER_MIDI=OFF \
        -DSDLMIXER_MP3_MPG123=OFF -DSDLMIXER_SAMPLES=OFF -DSDLMIXER_TESTS=OFF -DSDLMIXER_EXAMPLES=OFF
    cmake --build "$W/b-mix" -j"$JOBS"; cmake --install "$W/b-mix"
    cmake -S "$SRC/SDL_ttf" -B "$W/b-ttf" $S -DSDLTTF_VENDORED=ON -DSDLTTF_HARFBUZZ=ON -DSDLTTF_PLUTOSVG=ON \
        -DSDLTTF_SAMPLES=OFF -DSDLTTF_TESTS=OFF
    cmake --build "$W/b-ttf" -j"$JOBS"; cmake --install "$W/b-ttf"

    # 3. FFmpeg 7.1, the same minimal decode-only LGPL configuration.
    rm -rf "$W/ffmpeg-src"; cp -r "$SRC/ffmpeg" "$W/ffmpeg-src"
    (cd "$W/ffmpeg-src" && (make distclean >/dev/null 2>&1 || true) && ./configure --prefix="$PREFIX" \
        --enable-cross-compile --arch=aarch64 --target-os=android \
        --cc="$TC/aarch64-linux-android$API-clang" --cxx="$TC/aarch64-linux-android$API-clang++" \
        --ar="$TC/llvm-ar" --ranlib="$TC/llvm-ranlib" --nm="$TC/llvm-nm" --strip="$TC/llvm-strip" \
        --enable-static --disable-shared --enable-pic --disable-programs --disable-doc \
        --disable-avdevice --disable-avfilter --disable-postproc --disable-network --disable-autodetect --disable-everything \
        --disable-vulkan --disable-mediacodec --disable-jni \
        --enable-decoder=h264,hevc,aac,mp3float,mpeg2video,vorbis \
        --enable-parser=h264,hevc,aac,mpegaudio,mpeg2video,mpegvideo,vorbis \
        --enable-demuxer=mov,mpegvideo,ogg,matroska --enable-muxer=matroska --enable-protocol=file \
        --enable-swscale --enable-swresample)
    (cd "$W/ffmpeg-src" && make -j"$JOBS" && make install)
}

build_dirk() {
    # 4. dirk as libmain.so (CMakeLists.txt: ANDROID), bundled Lua.
    export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR=
    cmake -S "$HERE" -B "$W/b-dirk" $C -DDIRK_STATIC=ON -DDIRK_BUNDLED_LUA=ON \
        -DPKG_CONFIG_EXECUTABLE=/usr/bin/pkg-config
    cmake --build "$W/b-dirk" --target main -j"$JOBS"
}

build_apk() {
    # 5. The APK: SDL's Java classes + DirkActivity, resources, libmain.so,
    # KimmyScript in assets/ (copied out on first launch, see DirkActivity).
    A=$HERE/android; G=$W/apk; rm -rf "$G"; mkdir -p "$G/classes" "$G/dex" "$G/stage/lib/$ABI" "$G/stage/assets"
    "$BT/aapt2" compile --dir "$A/res" -o "$G/res.zip"
    VC=${DIRK_VERSION_CODE:-1}; VN=${DIRK_VERSION_NAME:-1.0.0}
    "$BT/aapt2" link -I "$PLATFORM_JAR" --manifest "$A/AndroidManifest.xml" -o "$G/base.apk" "$G/res.zip" \
        --java "$G/gen" --min-sdk-version $API --target-sdk-version 35 --version-code "$VC" --version-name "$VN"
    find "$SRC/SDL3/android-project/app/src/main/java" "$A/java" "$G/gen" -name '*.java' > "$G/sources.txt"
    javac -encoding UTF-8 --release 11 -nowarn -classpath "$PLATFORM_JAR" -d "$G/classes" @"$G/sources.txt" 2>&1 | grep -v '^Note:' || true
    "$BT/d8" --release --min-api $API --lib "$PLATFORM_JAR" --output "$G/dex" $(find "$G/classes" -name '*.class')
    cp "$W/b-dirk/libmain.so" "$G/stage/lib/$ABI/libmain.so"
    "$TC/llvm-strip" --strip-unneeded "$G/stage/lib/$ABI/libmain.so"
    cp -r "$HERE/installer/payload/KimmyScript" "$G/stage/assets/KimmyScript"
    mkdir -p "$G/stage/assets/Licenses"; cp "$A"/Licenses/* "$G/stage/assets/Licenses/"
    cp "$HERE/LICENSE" "$G/stage/assets/Licenses/GPL-3.0.txt"
    cp "$A/THIRD_PARTY_NOTICES.txt" "$G/stage/assets/THIRD_PARTY_NOTICES.txt"
    find "$G/stage" -name luac.out -delete
    # The list DirkFiles.install() copies to Dirk/ on the device.
    (cd "$G/stage/assets" && find KimmyScript Licenses THIRD_PARTY_NOTICES.txt -type f | LC_ALL=C sort > dirk-files.txt)
    cp "$G/base.apk" "$G/unsigned.apk"
    (cd "$G/dex" && zip -X -D -q "$G/unsigned.apk" classes.dex)
    (cd "$G/stage" && zip -X -D -r -q "$G/unsigned.apk" assets && zip -X -D -r -q -0 "$G/unsigned.apk" lib)
    "$BT/zipalign" -f -P 16 4 "$G/unsigned.apk" "$G/aligned.apk"
    mkdir -p "$OUT"
    if [ "${DIRK_UNSIGNED:-0}" = 1 ]; then
        # DIRK_UNSIGNED=1: an aligned APK left unsigned, for whoever holds
        # the release key to sign on their own machine, with apksigner.jar
        # (Apache 2.0, NOTICE.txt of build-tools) so a JDK is enough there:
        #   java -jar apksigner.jar sign --ks <key> --ks-key-alias dirk \
        #       --out dirk-<version>-android-arm64.apk dirk-<version>-android-arm64-unsigned.apk
        cp "$G/aligned.apk" "$OUT/dirk-$VN-android-arm64-unsigned.apk"
        cp "$BT/lib/apksigner.jar" "$OUT/apksigner.jar"
        cp "$BT/NOTICE.txt" "$OUT/apksigner-NOTICE.txt"
        "$BT/zipalign" -c -P 16 4 "$OUT/dirk-$VN-android-arm64-unsigned.apk"
        echo "== $OUT/dirk-$VN-android-arm64-unsigned.apk (not signed)"
        return
    fi
    KS=${DIRK_KEYSTORE:-$W/dirk-debug.keystore}
    if [ -n "${DIRK_KEYSTORE:-}" ] && [ ! -f "$KS" ]; then
        echo "DIRK_KEYSTORE: no such file: $KS" >&2; exit 1
    fi
    if [ ! -f "$KS" ]; then
        keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias dirk -keyalg RSA -keysize 2048 \
            -validity 10000 -dname "CN=Dirk debug" 2>/dev/null
    fi
    "$BT/apksigner" sign --ks "$KS" --ks-pass pass:${DIRK_KS_PASS:-android} --ks-key-alias dirk \
        --out "$OUT/dirk-$VN-android-arm64.apk" "$G/aligned.apk"
    "$BT/apksigner" verify "$OUT/dirk-$VN-android-arm64.apk" && echo "== $OUT/dirk-$VN-android-arm64.apk"
}

case $STEP in
    libs) build_libs;; dirk) build_dirk;; apk) build_apk;;
    all) build_libs; build_dirk; build_apk;;
    *) echo "step?"; exit 1;;
esac
