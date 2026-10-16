#pragma once
// When a bezel can be shown at all (2026-10-02, Cyril). Two rules, both
// automatic, neither touching the player's saved choice
// (LaunchSettings::bezelIndex stays what it was):
//
// 1. The bezel's screen window must have the movie's shape. Dirk fits the
//    picture into that window, so a 16:9 movie in a bezel cut for 4:3 came
//    out squeezed. Such a bezel is left out for this movie when the bezels
//    are loaded (logged), like a file with no transparent window.
//
// 2. There must be room for it on the screen: a bezel dresses the black
//    bars beside the picture, and a screen hardly wider than the picture
//    (4:3 movie on a 4:3 cabinet or a 640x480 handheld, 16:9 movie on a
//    16:9 screen) has none -- the bezel would only shrink the picture.
//    Then no bezel is available: none drawn, F11 does nothing, the game's
//    menu shows N/A (dirkGetBezelCount() = 0). Measured against the
//    screen the window is on, not the window, so a 4:3 window on a 16:9
//    monitor behaves like that monitor in fullscreen. Checked every frame:
//    the same game moved to a wider screen gets its bezel back.
//
// Pure functions, no SDL: tools/bezel_fit_test.cpp checks them.

namespace dirk {

// Shapes within 5% of each other count as the same (bezel windows are
// cut by hand: 1436x1080 for a 1440x1080 movie is fine).
constexpr double kBezelShapeTolerance = 0.05;
// The screen must be at least 10% wider than the picture for a bezel to
// have bars to fill (4:3 picture: from 1.47, e.g. 3:2, 16:10, 16:9).
constexpr double kBezelRoomFactor = 1.10;

inline bool bezelWindowFitsMovie(int windowW, int windowH, double movieAspect) {
    if (windowW <= 0 || windowH <= 0 || movieAspect <= 0) return false;
    const double a = static_cast<double>(windowW) / windowH;
    const double ratio = a > movieAspect ? a / movieAspect : movieAspect / a;
    return ratio <= 1.0 + kBezelShapeTolerance;
}

inline bool screenHasRoomForBezel(int screenW, int screenH, double movieAspect) {
    if (screenW <= 0 || screenH <= 0 || movieAspect <= 0) return false;
    return static_cast<double>(screenW) / screenH >= movieAspect * kBezelRoomFactor;
}

} // namespace dirk
