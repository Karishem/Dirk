#pragma once
// Optional per-game arcade bezel(s): a decorative frame image (box art,
// cabinet-style artwork, etc.) that fills the letterbox/pillarbox space
// around the game picture on a widescreen display. Most LaserDisc games are
// 4:3-ish (DLe is 1440x1080) while most screens today are 16:9, so instead
// of plain black bars either side of the picture, dirk can composite a
// bezel PNG behind which the game picture is inset -- something real Singe
// never offered.
//
// Purely a display-engine feature: no KimmyScript changes needed for the
// image loading/compositing itself, entirely optional. No bezel file
// present = today's plain letterbox behavior, unchanged.
//
// Convention (2026-09-23, generalized from a single fixed filename to a
// folder scan, on request -- a game can now ship as many bezels as it
// wants and cycle between them, instead of being limited to exactly one):
// every "*.png" directly inside a "Bezel" subfolder of the game's own data
// folder -- i.e. "Bezel/" next to the Cfg/Movie/Overlay siblings (see
// ctx.dataPath in main.cpp, i.e. dirOf(argv[2])) -- deliberately not
// directly in the game's root, to keep that root limited to what
// KimmyScript itself expects there.
//
// Some games ship more than one top-level `.dirk` launcher pointed at the
// same shared video/data folder (two different configs of the same disc --
// confirmed real case: Chantze Stone / Triad Stone), and want different
// bezels per launcher, cycled independently of one another even though
// their files sit in the same folder. loadBezels() takes the actually-
// launched .dirk file's own stem (its filename without extension, e.g.
// "Triad") for exactly this: it first looks for every file named
// "Bezel/<stem>_*.png" (e.g. "Bezel/Triad_bezel1.png",
// "Bezel/Triad_bezel2.png", any name starting with the stem followed by an
// underscore) -- if one or more exist, ONLY those are used, so Triad's
// launcher never sees Chantze's files or vice versa even though they share
// one Bezel/ folder. When no stem-prefixed file exists at all, every plain
// "*.png" directly in Bezel/ is used instead -- the normal case for a game
// with only one launcher, no prefix needed. Either way, the resulting set
// is sorted alphabetically by filename for a stable, predictable cycling
// order (e.g. name them "01_neon.png", "02_wood.png", ... for explicit
// control, or leave them as-is if the order doesn't matter). Exactly one
// match (either tier) behaves exactly like today's single-bezel setup,
// just discovered by scanning instead of a fixed name -- a plain
// "Bezel/bezel.png" still works with zero changes needed. Zero matches
// (empty folder or no Bezel/ at all) = today's "no bezel" behavior,
// unchanged. No KimmyScript/Lua involvement in the discovery/loading
// itself either way -- which files get picked is derived purely from the
// .dirk filename dirk was launched with plus what's physically in Bezel/,
// in C++, before any Lua runs. Which one is currently ACTIVE (none, or
// bezel #N of however many were found) is a runtime choice cycled via the
// native F11 hotkey and/or a game's own KimmyScript "UI Options" menu --
// see LaunchSettings::bezelIndex and api_singe.cpp's
// dirkGet/SetBezelIndex()/dirkGetBezelCount().
//
// The image is authored at whatever resolution/aspect the bezel artist
// chose (e.g. 1920x1080), with a rectangular "screen" cutout -- fully or
// mostly transparent, anti-aliased edges are fine -- where the game picture
// should show through. dirk detects that cutout itself by scanning the
// alpha channel once at load time, so there's no separate metadata file
// and no coordinates to hand-enter: any bezel PNG with a clean rectangular
// hole just works. A file with no usable transparent cutout is skipped
// (logged, not a hard error) -- same "best effort" treatment as a file
// that fails to decode at all.

#include <string>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace dirk {

struct Bezel {
    SDL_Texture* texture = nullptr;
    std::string name; // file name, for the log
    int width = 0;
    int height = 0;

    // Cutout rectangle where the game picture belongs, in the bezel
    // image's own pixel coordinates (top-left origin).
    int cutoutX = 0;
    int cutoutY = 0;
    int cutoutW = 0;
    int cutoutH = 0;

    bool valid() const { return texture != nullptr && cutoutW > 0 && cutoutH > 0; }
};

// Scans "Bezel/" under `gameDataPath` (expected to already end in a path
// separator, matching dirOf()'s convention elsewhere in main.cpp) and
// returns every usable bezel found, already loaded (texture + detected
// cutout) and sorted alphabetically by filename -- see this header's own
// comment above for the exact discovery rules (stem-prefixed set first,
// else every plain "*.png" in the folder). `scriptStem` is the launched
// .dirk's own filename without its extension or directory (e.g. "Triad")
// -- pass an empty string when it's not known (e.g. the bare-video-file
// legacy launch mode) to skip straight to the plain-name tier. Empty
// result (never a hard error) means no bezel for this game, same as
// before -- main.cpp treats that identically to the old "no bezel.png at
// all" case.
std::vector<Bezel> loadBezels(SDL_Renderer* renderer, const std::string& gameDataPath, const std::string& scriptStem);

// Frees the texture (if any) and resets `bezel` to its default (invalid)
// state. Safe to call on an already-invalid Bezel.
void destroyBezel(Bezel& bezel);

// Convenience: destroyBezel() on every element, e.g. at shutdown.
void destroyBezels(std::vector<Bezel>& bezels);

} // namespace dirk
