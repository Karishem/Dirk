cmake_minimum_required(VERSION 3.16)
project(dirk C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# SDL3 EXPERIMENT (dirk-engine-sdl3) -- fork of the working SDL2 build, kept
# on the side per Cyril's request, to try porting to SDL3 without touching
# the real dirk-engine tree. SDL3/SDL3_image/SDL3_ttf/SDL3_mixer aren't
# packaged for Ubuntu 24.04 (too new), so this Linux sandbox build expects
# them pre-built into a local prefix and needs that prefix's pkgconfig dir
# on PKG_CONFIG_PATH when you run cmake, e.g.:
#   PKG_CONFIG_PATH=/home/claude/sdl3-prefix/lib/pkgconfig cmake -S . -B build
# Their pkg-config module names are lowercase-hyphenated in the versions
# built here (sdl3, sdl3-image, sdl3-ttf, sdl3-mixer), not SDL3_image etc.
# On Cyril's real MSYS2 toolchain, once mingw-w64-x86_64-sdl3* packages
# exist there, this should need no changes -- MSYS2 packages already put
# their .pc files on the default PKG_CONFIG_PATH.
#
# Statically link SDL3/SDL3_mixer/SDL3_image/SDL3_ttf/ffmpeg/lua, and the
# MinGW C/C++ runtime itself, into a single dirk.exe -- nothing to ship or
# install alongside it, no dependency on the Windows PATH. Meant for a
# MinGW-w64 (MSYS2) toolchain: every "mingw-w64-x86_64-<pkg>" package
# installs both the normal shared-library .pc data and pkg-config's
# "--static" variant, and CMake's pkg_check_modules() below always computes
# both automatically -- as plain <PREFIX>_LIBRARIES/_CFLAGS_OTHER and as
# <PREFIX>_STATIC_LIBRARIES/_STATIC_CFLAGS_OTHER -- so this option only
# switches which set gets used, nothing more involved than that.
#
# Off by default so the everyday dev build (this repo's own sandbox, a
# normal Linux desktop, ...) keeps linking dynamically exactly as before;
# turn it on with -DDIRK_STATIC=ON specifically for a distributable Windows
# build. Not verified end-to-end here (no MinGW-w64 toolchain, and none of
# the Windows static libs, available in this Linux sandbox) -- expect some
# back-and-forth the first time this runs for real in MSYS2, most likely
# around whichever optional codec libs the local SDL3_mixer/SDL3_image/
# ffmpeg packages were themselves built against (see README).
#
# Worth knowing before shipping a build made this way: MSYS2's prebuilt
# mingw-w64-x86_64-ffmpeg package is normally compiled with some GPL-only
# codecs enabled (x264 and friends) -- statically linking against it pulls
# that licensing in for the resulting dirk.exe as a whole. Fine for personal
# use; worth a second look before distributing the binary publicly.
option(DIRK_STATIC "Statically link everything (and the MinGW runtime) for a single dependency-free .exe" OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED sdl3)
pkg_check_modules(SDL2_MIXER REQUIRED sdl3-mixer)
pkg_check_modules(SDL2_IMAGE REQUIRED sdl3-image)
pkg_check_modules(SDL2_TTF REQUIRED sdl3-ttf)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

# Lua 5.1 (2026-10-16): KimmyScript and Wizard need exactly Lua 5.1, which
# not every system has (Batocera, for one, enables no Lua 5.1). A copy of
# Lua 5.1.5 lives in src/third_party/lua-5.1.5 (see its README.dirk), the
# way hypseus-singe carries its own. It is used when pkg-config finds no
# lua5.1, or always with -DDIRK_BUNDLED_LUA=ON (what a distribution package
# should pass, so the result never depends on what the build machine has).
option(DIRK_BUNDLED_LUA "Build the bundled Lua 5.1.5 instead of using the system's lua5.1" OFF)
if(NOT DIRK_BUNDLED_LUA)
    pkg_check_modules(LUA QUIET lua5.1)
    if(NOT LUA_FOUND)
        message(STATUS "lua5.1 not found through pkg-config: using the bundled Lua 5.1.5")
        set(DIRK_BUNDLED_LUA ON)
    endif()
endif()
if(DIRK_BUNDLED_LUA)
    set(_dirk_lua_dir ${CMAKE_SOURCE_DIR}/src/third_party/lua-5.1.5/src)
    set(_dirk_lua_sources lapi.c lcode.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c
        lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c
        lundump.c lvm.c lzio.c lauxlib.c lbaselib.c ldblib.c liolib.c lmathlib.c
        loslib.c ltablib.c lstrlib.c loadlib.c linit.c)
    list(TRANSFORM _dirk_lua_sources PREPEND ${_dirk_lua_dir}/)
    add_library(dirk_lua51 STATIC ${_dirk_lua_sources})
    set_target_properties(dirk_lua51 PROPERTIES LINKER_LANGUAGE C POSITION_INDEPENDENT_CODE ON)
    target_include_directories(dirk_lua51 PUBLIC ${_dirk_lua_dir})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(dirk_lua51 PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
        target_link_libraries(dirk_lua51 PUBLIC m ${CMAKE_DL_LIBS})
    elseif(APPLE)
        target_compile_definitions(dirk_lua51 PRIVATE LUA_USE_MACOSX)
    elseif(UNIX)
        target_compile_definitions(dirk_lua51 PRIVATE LUA_USE_POSIX)
        target_link_libraries(dirk_lua51 PUBLIC m)
    endif()
    # The bundled luac, for tools/embed_wizard.py where no luac5.1 is
    # installed (not built by default): cmake --build <dir> --target dirk_luac
    add_executable(dirk_luac EXCLUDE_FROM_ALL ${_dirk_lua_dir}/luac.c ${_dirk_lua_dir}/print.c)
    target_link_libraries(dirk_luac PRIVATE dirk_lua51)
    # The pkg-config variables below stay empty; dirk_core links dirk_lua51.
    foreach(_v INCLUDE_DIRS LIBRARY_DIRS LIBRARIES CFLAGS_OTHER)
        set(LUA_${_v} "")
        set(LUA_STATIC_${_v} "")
    endforeach()
    message(STATUS "Lua: bundled Lua 5.1.5 (src/third_party/lua-5.1.5)")
else()
    message(STATUS "Lua: system lua5.1 ${LUA_VERSION}")
endif()

add_library(dirk_core
    src/video/VideoSource.cpp
    src/video/HypseusRemux.cpp
    src/video/VideoTexture.cpp
    src/lua/LuaEngine.cpp
    src/lua/api_singe.cpp
    src/lua/InputMap.cpp
    src/core/LaunchSettings.cpp
    src/core/Bezel.cpp
    src/core/JoystickProfile.cpp
    src/core/JoystickWizard.cpp
    src/core/MovieChoice.cpp
    src/core/BlackScan.cpp
    src/core/ScriptErrorLog.cpp
    src/core/KimmyFingerprint.cpp
    src/core/Subtitles.cpp
    src/core/Languages.cpp
    src/core/GameArchive.cpp
    src/third_party/miniz/miniz.c
    src/core/SubtitleRenderer.cpp
    src/audio/AudioLatency.cpp
)

# miniz (vendored, MIT -- src/third_party/miniz/LICENSE) reads .zdirk
# games. No zlib-compatible names: a static build also links the real zlib
# (through ffmpeg/SDL_image), whose compress()/inflate() must not clash.
set_source_files_properties(src/third_party/miniz/miniz.c PROPERTIES
    COMPILE_DEFINITIONS "MINIZ_NO_ZLIB_COMPATIBLE_NAMES")
target_compile_definitions(dirk_core PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# AudioLatency.cpp's WASAPI path (Windows only -- a no-op fallback stub
# covers every other platform, see that file) needs ole32 for the COM calls
# it makes (CoInitializeEx/CoCreateInstance/CoTaskMemFree/CoUninitialize).
if(WIN32)
    target_link_libraries(dirk_core PUBLIC ole32)
endif()

# Selects the plain (shared-linking) or "_STATIC_" pkg-config variable set
# for every dependency below, in one place, per DIRK_STATIC.
if(DIRK_STATIC)
    set(_dirk_libsuffix "STATIC_")
else()
    set(_dirk_libsuffix "")
endif()

target_include_directories(dirk_core PUBLIC
    src
    ${SDL2_${_dirk_libsuffix}INCLUDE_DIRS}
    ${SDL2_MIXER_${_dirk_libsuffix}INCLUDE_DIRS}
    ${SDL2_IMAGE_${_dirk_libsuffix}INCLUDE_DIRS}
    ${SDL2_TTF_${_dirk_libsuffix}INCLUDE_DIRS}
    ${AVFORMAT_${_dirk_libsuffix}INCLUDE_DIRS}
    ${AVCODEC_${_dirk_libsuffix}INCLUDE_DIRS}
    ${AVUTIL_${_dirk_libsuffix}INCLUDE_DIRS}
    ${SWSCALE_${_dirk_libsuffix}INCLUDE_DIRS}
    ${SWRESAMPLE_${_dirk_libsuffix}INCLUDE_DIRS}
    ${LUA_${_dirk_libsuffix}INCLUDE_DIRS}
)

# pkg_check_modules() reports which libraries to link (*_LIBRARIES, just
# names like "avutil") and separately where to find them (*_LIBRARY_DIRS,
# the -L paths) -- target_link_libraries() below only consumes the former.
# Without also telling the linker about the latter, "-lavutil" resolves
# via the toolchain's own default search path (MSYS2's system
# mingw64/lib), NOT the minimal custom builds' install prefixes -- even
# though PKG_CONFIG_PATH correctly picks up their .pc files for every
# other purpose. This is what silently linked the system (full-featured,
# vaapi/soxr/rsvg-enabled) static libraries instead of the trimmed ones
# built by tools/build_ffmpeg_minimal_windows.sh and
# tools/build_sdl_satellites_minimal_windows.sh, no matter how correct
# PKG_CONFIG_PATH was. Explicit -L directories always win over the
# toolchain's implicit ones, so this line alone fixes it.
target_link_directories(dirk_core PUBLIC
    ${SDL2_${_dirk_libsuffix}LIBRARY_DIRS}
    ${SDL2_MIXER_${_dirk_libsuffix}LIBRARY_DIRS}
    ${SDL2_IMAGE_${_dirk_libsuffix}LIBRARY_DIRS}
    ${SDL2_TTF_${_dirk_libsuffix}LIBRARY_DIRS}
    ${AVFORMAT_${_dirk_libsuffix}LIBRARY_DIRS}
    ${AVCODEC_${_dirk_libsuffix}LIBRARY_DIRS}
    ${AVUTIL_${_dirk_libsuffix}LIBRARY_DIRS}
    ${SWSCALE_${_dirk_libsuffix}LIBRARY_DIRS}
    ${SWRESAMPLE_${_dirk_libsuffix}LIBRARY_DIRS}
    ${LUA_${_dirk_libsuffix}LIBRARY_DIRS}
)

target_link_libraries(dirk_core PUBLIC
    ${SDL2_${_dirk_libsuffix}LIBRARIES}
    ${SDL2_MIXER_${_dirk_libsuffix}LIBRARIES}
    ${SDL2_IMAGE_${_dirk_libsuffix}LIBRARIES}
    ${SDL2_TTF_${_dirk_libsuffix}LIBRARIES}
    ${AVFORMAT_${_dirk_libsuffix}LIBRARIES}
    ${AVCODEC_${_dirk_libsuffix}LIBRARIES}
    ${AVUTIL_${_dirk_libsuffix}LIBRARIES}
    ${SWSCALE_${_dirk_libsuffix}LIBRARIES}
    ${SWRESAMPLE_${_dirk_libsuffix}LIBRARIES}
    ${LUA_${_dirk_libsuffix}LIBRARIES}
)
if(DIRK_BUNDLED_LUA)
    target_link_libraries(dirk_core PUBLIC dirk_lua51)
endif()

target_compile_options(dirk_core PUBLIC
    ${SDL2_${_dirk_libsuffix}CFLAGS_OTHER}
    ${SDL2_MIXER_${_dirk_libsuffix}CFLAGS_OTHER}
    ${SDL2_IMAGE_${_dirk_libsuffix}CFLAGS_OTHER}
    ${SDL2_TTF_${_dirk_libsuffix}CFLAGS_OTHER}
    ${AVFORMAT_${_dirk_libsuffix}CFLAGS_OTHER}
    ${AVCODEC_${_dirk_libsuffix}CFLAGS_OTHER}
    ${AVUTIL_${_dirk_libsuffix}CFLAGS_OTHER}
    ${SWSCALE_${_dirk_libsuffix}CFLAGS_OTHER}
    ${SWRESAMPLE_${_dirk_libsuffix}CFLAGS_OTHER}
    ${LUA_${_dirk_libsuffix}CFLAGS_OTHER}
)

# Static C/C++ runtime too -- without this, a "statically linked" build
# still quietly depends on libgcc_s_seh-1.dll/libstdc++-6.dll/
# libwinpthread-1.dll from the MinGW install, which defeats the point of a
# single dependency-free .exe. MinGW-specific flags, so gated on MINGW
# rather than just DIRK_STATIC (a static build on another compiler --
# MSVC's /MT, say -- would need a different mechanism entirely).
if(DIRK_STATIC AND MINGW)
    target_link_options(dirk_core PUBLIC -static -static-libgcc -static-libstdc++)
endif()

# Main engine executable (windowed, SDL2). Not the focus of the first milestone
# but wired up so the project is a real, runnable skeleton.
add_executable(dirk src/core/main.cpp)
target_link_libraries(dirk PRIVATE dirk_core)

# std::thread for main.cpp's loading-screen animation, which runs on the
# main thread alongside VideoSource::finishOpening() (the potentially-slow
# first-time index build) on a background one -- see main.cpp's own
# comment at that call site. Threads::Threads resolves to -pthread on
# Linux/MinGW (needed for std::thread to actually link, not just compile)
# and to nothing extra on a toolchain that doesn't need it.
find_package(Threads REQUIRED)
target_link_libraries(dirk PRIVATE Threads::Threads)

# Windows-only: embed FileDescription/ProductName/FileVersion PE metadata
# (see src/core/dirk.rc) -- a bare MinGW/GCC build has none of this by
# default, and Windows' "always use this app" default-app picker has been
# seen to refuse to persist a choice for an .exe with zero version-info
# resources. No-op on every other platform (enable_language(RC) and the
# .rc source are both skipped entirely when not MINGW).
if(MINGW)
    enable_language(RC)
    target_sources(dirk PRIVATE src/core/dirk.rc)
endif()

# Windows-only (2026-10-10): Wizard.exe, the small program people copy into
# a game folder to make its sheet (KimmyScript's create mode). It only finds
# the installed dirk.exe and runs `dirk.exe --wizard <its folder>`; Wizard
# itself is built into dirk (src/lua/WizardChunk.h, made by
# tools/embed_wizard.py from wizard.kimmy). Plain Win32 C, no SDL, static
# runtime: a single few-hundred-KB file.
if(MINGW)
    add_executable(wizard WIN32 src/launcher/wizard_launcher.c src/launcher/wizard.rc)
    set_target_properties(wizard PROPERTIES OUTPUT_NAME Wizard)
    target_link_options(wizard PRIVATE -municode -static)
    target_link_libraries(wizard PRIVATE shlwapi advapi32 user32 ole32 shell32 uuid)
endif()

# Windows-only: build dirk as a GUI-subsystem executable instead of
# MinGW's default of "console" -- no black console window flashing open
# behind the game on every launch (main() below still restores stdout/
# stderr when run from an existing terminal, via AttachConsole), and
# real-world testing found Windows' file-association "always use this
# app" picker to behave much better against a GUI-subsystem .exe than a
# console-subsystem one. Only the `dirk` target -- seek_test stays a
# console tool, it's only ever run by hand from a terminal.
if(MINGW)
    target_link_options(dirk PRIVATE -mwindows)
endif()

# Standalone smoke test for the frame-accurate video seeking pipeline --
# the single highest-risk item identified in the feasibility study.
# Builds/uses a frame index and verifies seeks land on the exact requested
# frame by reading a frame-number burned into the test video's pixels.
add_executable(seek_test tools/seek_test.cpp)
target_link_libraries(seek_test PRIVATE dirk_core)


# End-to-end test of the native joystick setup wizard (src/core/
# JoystickWizard.*) -- drives it through SDL's real virtual-joystick API
# instead of unit-testing its internals directly, so the test exercises
# the exact same event path main.cpp itself uses. See the file's own
# top comment for what it covers.
add_executable(joystick_wizard_test tools/joystick_wizard_test.cpp)
target_link_libraries(joystick_wizard_test PRIVATE dirk_core)

# Unit test for dirk::nudgeVolumes() (LaunchSettings.h/.cpp), the shared
# clamping logic behind main.cpp's +/- volume hotkeys. Pure-function test,
# no SDL involved -- see the test file's own top comment.
add_executable(volume_nudge_test tools/volume_nudge_test.cpp)
target_link_libraries(volume_nudge_test PRIVATE dirk_core)

# Unit test for dirk::buildProfileKey() and JoystickProfileStore's
# composite-key fallback (JoystickProfile.h/.cpp) -- the fix for two
# identical raw joysticks/gamepads reporting the same SDL GUID (real bug,
# see the test file's own top comment). No SDL window/event loop needed.
add_executable(joystick_profile_test tools/joystick_profile_test.cpp)
target_link_libraries(joystick_profile_test PRIVATE dirk_core)

# Unit test for the per-game Custom/ override on sound files (see
# customOverridePath() in src/lua/api_singe.cpp) -- drives the real
# soundLoad() through registerSingeApi() with a real SDL3_mixer mixer and
# real WAV files on disk. See the test file's own top comment.
add_executable(custom_sound_override_test tools/custom_sound_override_test.cpp)
target_link_libraries(custom_sound_override_test PRIVATE dirk_core)

# Unit test for the always-on dirk_error.txt (src/core/ScriptErrorLog.*)
# and the Lua traceback LuaEngine::pcall() now captures for it -- review
# point 2, 2026-09-26. Real files on disk, real Lua state; no SDL window.
add_executable(script_error_log_test tools/script_error_log_test.cpp)
target_link_libraries(script_error_log_test PRIVATE dirk_core)

# Regression test for the fontToSprite shared-texture cache and the
# fontLoad cache (EngineContext::textSpriteCache/fontCache) -- review
# point 3, 2026-09-26. Real bindings, real renderer (dummy video driver),
# real TTF font; replays Mad Dog's and KimmyScript's actual call patterns.
add_executable(text_resource_cache_test tools/text_resource_cache_test.cpp)
target_link_libraries(text_resource_cache_test PRIVATE dirk_core)
add_executable(kimmy_logo_test tools/kimmy_logo_test.cpp)
target_link_libraries(kimmy_logo_test PRIVATE dirk_core)

# "Which KimmyScript did this run use" (src/core/KimmyFingerprint.*),
# 2026-09-27: folder + short MD5 per script file, in dirk_log.txt and the
# dirk_error.txt header. RFC 1321 test vectors on real files.
add_executable(kimmy_fingerprint_test tools/kimmy_fingerprint_test.cpp)
target_link_libraries(kimmy_fingerprint_test PRIVATE dirk_core)

# .srt subtitles next to the movie (src/core/Subtitles.*), 2026-09-27:
# parsing (encodings, line endings, tags, broken blocks), cue lookup at the
# frame's time, finding Movie.srt / Movie.xx.srt on disk.
add_executable(game_archive_test tools/game_archive_test.cpp)
target_link_libraries(game_archive_test PRIVATE dirk_core)

add_executable(languages_test tools/languages_test.cpp)
target_link_libraries(languages_test PRIVATE dirk_core)

add_executable(subtitles_test tools/subtitles_test.cpp)
target_link_libraries(subtitles_test PRIVATE dirk_core)

# When a bezel is available (2026-10-02): its window has the movie's
# shape, and the screen has room beside the picture (core/BezelFit.h).
add_executable(bezel_fit_test tools/bezel_fit_test.cpp)
target_include_directories(bezel_fit_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Several soundtracks (2026-09-27): Hypseus <name>-fr.ogg alternates merged
# into the cached .mkv, listed and switched by VideoSource. Real files,
# three sine soundtracks told apart by their frequency.
add_executable(audio_tracks_test tools/audio_tracks_test.cpp)
target_link_libraries(audio_tracks_test PRIVATE dirk_core)

# Regression test for the .dirkindex validity check (review point 7,
# 2026-09-26): a movie re-encoded at the same resolution must never reuse
# its old index. Uses two versions of the same synthetic clip (see the
# test file's own top comment); no SDL.
add_executable(index_cache_test tools/index_cache_test.cpp)
target_link_libraries(index_cache_test PRIVATE dirk_core)

# End-to-end test of per-movie colour handling (review point 8,
# 2026-09-26): tagged BT.709/BT.601, untagged HD, full-range VP9 clips ->
# VideoSource detection -> createVideoTexture() -> pixel actually drawn.
add_executable(video_colors_test tools/video_colors_test.cpp)
target_link_libraries(video_colors_test PRIVATE dirk_core)

# Seek speed-up, step 1 (review point 1, 2026-09-26): non-reference frames
# before a seek target are no longer decoded. Checks every frame of a
# B-frame clip lands byte-identical to the no-skip decode, and that normal
# playback after a seek skips nothing.
add_executable(seek_skip_test tools/seek_skip_test.cpp)
target_link_libraries(seek_skip_test PRIVATE dirk_core)

# Multi-threaded decoding, audio side (review point 1 step 2, 2026-09-26):
# after a seek, the queued audio must start at the landed frame and be the
# right audio (compared sample by sample to a straight-through decode).
add_executable(seek_audio_align_test tools/seek_audio_align_test.cpp)
target_link_libraries(seek_audio_align_test PRIVATE dirk_core)

enable_testing()
add_test(NAME seek_smoke_test COMMAND seek_test ${CMAKE_SOURCE_DIR}/assets/test_pattern.mp4)
add_test(NAME joystick_wizard_test COMMAND joystick_wizard_test
         ${CMAKE_SOURCE_DIR}/assets/dirk_loading/dirk_loading_font.ttf)
# Waits out a real ~8s prompt timeout on purpose (see the test's own
# comment on why that's not faked) -- default ctest timeout is plenty, but
# named explicitly so this doesn't silently regress if a global timeout is
# ever tightened elsewhere.
set_tests_properties(joystick_wizard_test PROPERTIES TIMEOUT 60)
add_test(NAME volume_nudge_test COMMAND volume_nudge_test)
add_test(NAME joystick_profile_test COMMAND joystick_profile_test)
add_test(NAME custom_sound_override_test COMMAND custom_sound_override_test)
add_test(NAME script_error_log_test COMMAND script_error_log_test)
add_test(NAME kimmy_logo_test COMMAND kimmy_logo_test)
add_test(NAME kimmy_fingerprint_test COMMAND kimmy_fingerprint_test)
add_test(NAME game_archive_test COMMAND game_archive_test)
add_test(NAME languages_test COMMAND languages_test)
add_test(NAME subtitles_test COMMAND subtitles_test)
add_test(NAME bezel_fit_test COMMAND bezel_fit_test)
add_test(NAME audio_tracks_test COMMAND audio_tracks_test ${CMAKE_SOURCE_DIR}/assets/hypseus_multiaudio)
add_test(NAME text_resource_cache_test COMMAND text_resource_cache_test
         ${CMAKE_SOURCE_DIR}/assets/dirk_loading/dirk_loading_font.ttf)
add_test(NAME index_cache_test COMMAND index_cache_test
         ${CMAKE_SOURCE_DIR}/assets/test_pattern.mp4 ${CMAKE_SOURCE_DIR}/assets/test_pattern_gop7.mp4)
add_test(NAME video_colors_test COMMAND video_colors_test ${CMAKE_SOURCE_DIR}/assets)
add_test(NAME seek_skip_test COMMAND seek_skip_test ${CMAKE_SOURCE_DIR}/assets/test_pattern_bframes.mp4)
add_test(NAME seek_audio_align_test COMMAND seek_audio_align_test ${CMAKE_SOURCE_DIR}/assets/test_av_chirp.mp4
         ${CMAKE_SOURCE_DIR}/assets/test_av_chirp_blocks_vfirst.mp4 ${CMAKE_SOURCE_DIR}/assets/test_av_chirp_blocks_afirst.mp4)

# The unit tests that open SDL video or audio run on SDL's dummy drivers,
# so ctest passes on a machine with no screen or sound card (a build server).
set_tests_properties(joystick_wizard_test custom_sound_override_test text_resource_cache_test
                     kimmy_logo_test video_colors_test
                     PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")

# Review points 4 and 6 (2026-09-26): end-to-end checks through a real dirk
# binary, driven headless by SDL virtual joysticks and synthetic keys from
# an LD_PRELOAD shim (tools/e2e/input_shim.cpp) -- Linux dev builds only;
# the shim is a test helper, never linked into dirk itself.
#   native_pause_e2e_test   Pause on every input path + who owns the freeze
#   settings_apply_e2e_test settings apply/persist only what changed
#   first_launch_e2e_test   controllers work on the launch that builds the index
#   subtitles_e2e_test      .srt subtitles drawn for the frame on screen
#   audio_tracks_e2e_test   several soundtracks, switched live, heard in the output
#   zdirk_e2e_test          a zipped game (.zdirk) read in place: saves beside it, Custom/
#   bezel_e2e_test          bezel only when it fits the movie and the screen
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT DIRK_STATIC)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_library(dirk_e2e_input_shim SHARED tools/e2e/input_shim.cpp)
        target_include_directories(dirk_e2e_input_shim PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_directories(dirk_e2e_input_shim PRIVATE ${SDL2_LIBRARY_DIRS})
        target_link_libraries(dirk_e2e_input_shim PRIVATE ${SDL2_LIBRARIES} ${CMAKE_DL_LIBS})
        foreach(_e2e native_pause settings_apply first_launch)
            add_test(NAME ${_e2e}_e2e_test
                     COMMAND bash ${CMAKE_SOURCE_DIR}/tools/e2e/${_e2e}/run.sh
                             $<TARGET_FILE:dirk> $<TARGET_FILE:dirk_e2e_input_shim>
                             ${CMAKE_SOURCE_DIR}/assets/test_av_chirp.mp4 ${Python3_EXECUTABLE})
            set_tests_properties(${_e2e}_e2e_test PROPERTIES TIMEOUT 60)
        endforeach()
        # 2026-09-27: .srt subtitles on screen (jumps, off, other track,
        # too-wide line), measured on screenshots of a solid-colour clip.
        add_test(NAME subtitles_e2e_test
                 COMMAND bash ${CMAKE_SOURCE_DIR}/tools/e2e/subtitles/run.sh
                         $<TARGET_FILE:dirk> $<TARGET_FILE:dirk_e2e_input_shim>
                         ${CMAKE_SOURCE_DIR}/assets/subtitle_clip.mp4
                         ${CMAKE_SOURCE_DIR}/assets/dirk_loading/dirk_loading_font.ttf ${Python3_EXECUTABLE})
        set_tests_properties(subtitles_e2e_test PROPERTIES TIMEOUT 60)
        add_test(NAME zdirk_e2e_test
                 COMMAND bash ${CMAKE_SOURCE_DIR}/tools/e2e/zdirk/run.sh
                         $<TARGET_FILE:dirk> $<TARGET_FILE:dirk_e2e_input_shim>
                         ${CMAKE_SOURCE_DIR}/assets/test_av_chirp.mp4
                         ${CMAKE_SOURCE_DIR}/assets/dirk_loading/dirk_loading_font.ttf ${Python3_EXECUTABLE})
        set_tests_properties(zdirk_e2e_test PROPERTIES TIMEOUT 90)
        # 2026-09-27: several soundtracks (Hypseus <name>-fr.ogg alternates),
        # switched live; Dirk's real audio output recorded (SDL disk driver).
        add_test(NAME audio_tracks_e2e_test
                 COMMAND bash ${CMAKE_SOURCE_DIR}/tools/e2e/audio_tracks/run.sh
                         $<TARGET_FILE:dirk> $<TARGET_FILE:dirk_e2e_input_shim>
                         ${CMAKE_SOURCE_DIR}/assets/hypseus_multiaudio ${Python3_EXECUTABLE})
        set_tests_properties(audio_tracks_e2e_test PROPERTIES TIMEOUT 90)
        # 2026-10-02: bezels only when they fit (window shape = movie shape,
        # room beside the picture), movies at their real shape (8:9 pixels);
        # fullscreen on virtual X screens (skipped without Xvfb).
        add_test(NAME bezel_e2e_test
                 COMMAND bash ${CMAKE_SOURCE_DIR}/tools/e2e/bezel/run.sh
                         $<TARGET_FILE:dirk> $<TARGET_FILE:dirk_e2e_input_shim>
                         ${CMAKE_SOURCE_DIR}/assets ${Python3_EXECUTABLE})
        set_tests_properties(bezel_e2e_test PROPERTIES TIMEOUT 120)
        # 2026-10-03 (R36S): a script error repeated every frame with a file
        # left open must not pile up open files ("Too many open files").
        add_test(NAME script_error_files_e2e_test
                 COMMAND bash ${CMAKE_SOURCE_DIR}/tools/e2e/script_error_files/run.sh
                         $<TARGET_FILE:dirk> ${CMAKE_SOURCE_DIR}/assets/test_av_chirp.mp4)
        set_tests_properties(script_error_files_e2e_test PROPERTIES TIMEOUT 60)
        # 2026-10-03: joystick wizard opened from the script (Dirk Options
        # menu) and driven with a gamepad only, through controls.cfg.
        add_test(NAME wizard_nav_e2e_test
                 COMMAND bash ${CMAKE_SOURCE_DIR}/tools/e2e/wizard_nav/run.sh
                         $<TARGET_FILE:dirk> $<TARGET_FILE:dirk_e2e_input_shim>
                         ${CMAKE_SOURCE_DIR}/assets/test_av_chirp.mp4 ${Python3_EXECUTABLE})
        set_tests_properties(wizard_nav_e2e_test PROPERTIES TIMEOUT 60)
        # 2026-10-08: forced controls (forcedControls global, for Create.dirk)
        # win over the game's controls.cfg and over a joystick profile.
        add_test(NAME forced_controls_e2e_test
                 COMMAND bash ${CMAKE_SOURCE_DIR}/tools/e2e/forced_controls/run.sh
                         $<TARGET_FILE:dirk> $<TARGET_FILE:dirk_e2e_input_shim>
                         ${CMAKE_SOURCE_DIR}/assets/test_av_chirp.mp4 ${Python3_EXECUTABLE})
        set_tests_properties(forced_controls_e2e_test PROPERTIES TIMEOUT 60)
    endif()
endif()
