// core/BezelFit.h (2026-10-02): when a bezel is available.
#include "core/BezelFit.h"

#include <cstdio>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

int main() {
    using namespace dirk;
    const double r43 = 4.0 / 3.0, r169 = 16.0 / 9.0;

    // Rule 1: the bezel's window has the movie's shape.
    check(bezelWindowFitsMovie(1440, 1080, r43), "4:3 window, 4:3 movie");
    check(bezelWindowFitsMovie(1436, 1078, r43), "hand-cut 1436x1078 window, 4:3 movie");
    check(bezelWindowFitsMovie(1400, 1080, 1440.0 / 1080), "1400x1080 window (3% narrower), 4:3 movie");
    check(!bezelWindowFitsMovie(1440, 1080, r169), "4:3 window, 16:9 movie: left out");
    check(!bezelWindowFitsMovie(1920, 1080, r43), "16:9 window, 4:3 movie: left out");
    check(bezelWindowFitsMovie(1920, 1080, r169), "16:9 window (21:9 bezel), 16:9 movie");
    check(bezelWindowFitsMovie(640, 480, 720.0 * 8 / (480 * 9)), "720x480 movie with 8:9 pixels is 4:3");
    check(!bezelWindowFitsMovie(640, 480, 720.0 / 480), "...and would not be, read as 3:2");
    check(!bezelWindowFitsMovie(0, 1080, r43) && !bezelWindowFitsMovie(1440, 1080, 0), "empty sizes");

    // Rule 2: room beside the picture on the screen.
    check(screenHasRoomForBezel(1920, 1080, r43), "4:3 movie, 16:9 screen: room");
    check(screenHasRoomForBezel(1280, 800, r43), "4:3 movie, 16:10 screen (Steam Deck): room");
    check(screenHasRoomForBezel(3440, 1440, r43), "4:3 movie, 21:9 screen: room");
    check(!screenHasRoomForBezel(1024, 768, r43), "4:3 movie, 4:3 screen (cabinet): no room");
    check(!screenHasRoomForBezel(640, 480, r43), "4:3 movie, 640x480 handheld: no room");
    check(!screenHasRoomForBezel(1280, 1024, r43), "4:3 movie, 5:4 screen: no room");
    check(!screenHasRoomForBezel(1920, 1080, r169), "16:9 movie, 16:9 screen: no room");
    check(screenHasRoomForBezel(2560, 1080, r169), "16:9 movie, 21:9 screen: room");
    check(!screenHasRoomForBezel(1920, 1200, r169), "16:9 movie, 16:10 screen: no room");

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
