#pragma once

// Two versions of the same movie (2026-10-14, Cyril): some games ship the
// disc twice -- a 4:3 and a 16:9 encode, or SD and HD -- in the same
// Movie/ (or Video/) folder. Dirk used to take the largest file. Now, when
// the folder holds two or more videos, the player picks one on a small
// screen shown before the movie is opened: B1 = the first, B2 = the second
// (more than two: UP/DOWN, then B1). Asked at every launch (Cyril's
// choice), in Wizard too. Every video of the folder is offered: Cyril,
// same day, "il n'y aura jamais le attract dans movie" (DLe's old
// menu-attract.mp4 there was a mistake). A movie passed explicitly on the
// command line is never questioned.
//
// The game's own controls.cfg isn't loaded yet at that point, so the
// buttons are those of the shared controls.cfg: B1 = Left Ctrl or 1, B2 =
// Left Alt or 2 (keyboard), A / B (gamepad, swap_face_buttons honoured),
// raw button 3 / 4 (arcade sticks, RAWJOY_n.BUTTON_3/4). ESC quits.
//
// Each version is shown by its file name (Cyril: two versions can have the
// same shape and size, e.g. Titan in 2.35 with its black bars and without),
// with its shape and picture size under it: name the files for the player
// ("Titan 2.35.mp4", "Titan plein ecran.mp4").
//
// Every version must have the same frame numbers: the sheet is shared.
// Dirk can't check that -- it is up to the game's author.

#include <cstdint>
#include <string>
#include <vector>

namespace dirk {

struct MovieVersion {
    std::string path;
    int width = 0;
    int height = 0;
    double aspect = 0.0;   // display aspect (pixel shape included)
    std::string label;     // what the player reads: the file name, without its extension
    std::string detail;    // under it, smaller: "16/9  1920x1080"
};

// Every video in `videoPath`'s folder (same extensions as the Movie/
// search), `videoPath` included, in file-name order (the author decides
// which one is B1). Fewer than two -> empty.
std::vector<MovieVersion> findMovieVersions(const std::string& videoPath);

// Shows the choice and waits for the player. Returns the index chosen, or
// -1 when the player quit (ESC, window closed). SDL video and gamepad
// subsystems are initialised here and left initialised (the caller's own
// SDL_Init calls are reference counted); device-added events seen while
// waiting are pushed back on the queue for the main loop, which opens the
// controllers from them.
int chooseMovieVersion(const std::vector<MovieVersion>& versions, const std::string& sharedEngineRoot,
                       bool fullscreen, bool swapFaceButtons);

} // namespace dirk
