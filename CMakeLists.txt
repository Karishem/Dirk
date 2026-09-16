cmake_minimum_required(VERSION 3.16)
project(dirk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_MIXER REQUIRED SDL2_mixer)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(LUA REQUIRED lua5.1)

add_library(dirk_core
    src/video/VideoSource.cpp
    src/lua/LuaEngine.cpp
    src/lua/api_singe.cpp
)

target_include_directories(dirk_core PUBLIC
    src
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_MIXER_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${LUA_INCLUDE_DIRS}
)

target_link_libraries(dirk_core PUBLIC
    ${SDL2_LIBRARIES}
    ${SDL2_MIXER_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${LUA_LIBRARIES}
)

target_compile_options(dirk_core PUBLIC
    ${SDL2_CFLAGS_OTHER}
    ${AVFORMAT_CFLAGS_OTHER}
    ${AVCODEC_CFLAGS_OTHER}
    ${AVUTIL_CFLAGS_OTHER}
    ${SWSCALE_CFLAGS_OTHER}
    ${LUA_CFLAGS_OTHER}
)

# Main engine executable (windowed, SDL2). Not the focus of the first milestone
# but wired up so the project is a real, runnable skeleton.
add_executable(dirk src/core/main.cpp)
target_link_libraries(dirk PRIVATE dirk_core)

# Standalone smoke test for the frame-accurate video seeking pipeline --
# the single highest-risk item identified in the feasibility study.
# Builds/uses a frame index and verifies seeks land on the exact requested
# frame by reading a frame-number burned into the test video's pixels.
add_executable(seek_test tools/seek_test.cpp)
target_link_libraries(seek_test PRIVATE dirk_core)

enable_testing()
add_test(NAME seek_smoke_test COMMAND seek_test ${CMAKE_SOURCE_DIR}/assets/test_pattern.mp4)
