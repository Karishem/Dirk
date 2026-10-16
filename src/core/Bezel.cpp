#include "core/Bezel.h"
#include "core/GameArchive.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace dirk {

namespace {

// Alpha at/below this is treated as "part of the screen cutout". A plain
// 0 threshold would miss a softly anti-aliased edge (a pixel or two of
// partial transparency is normal/expected out of any real image editor --
// confirmed by scanning a real bezel PNG a user provided, which had a ~5px
// alpha gradient at the cutout edge), while a very high threshold would eat
// into the opaque frame artwork itself. This sits comfortably below
// "visibly semi-opaque".
constexpr Uint8 kCutoutAlphaThreshold = 24;

// Loads and decodes exactly one bezel image file: surface load, RGBA32
// convert, alpha-scan for the transparent cutout, texture upload. Shared by
// every candidate loadBezels() finds -- factored out of what used to be the
// single-file loadBezel() so the folder-scan version below can call it once
// per discovered file. Returns an invalid (texture == nullptr) Bezel on any
// failure (missing file, bad decode, no transparent cutout) -- all logged
// the same way the old single-bezel code did, except a plain "doesn't
// exist" miss, which stays silent (the normal case for every candidate
// filename this game *doesn't* happen to use).
Bezel loadBezelFile(SDL_Renderer* renderer, const std::string& path) {
    Bezel result;

    // A .zdirk game's bezel may be only in its zip: decoded from memory.
    std::vector<unsigned char> zipped;
    SDL_Surface* raw = dirk::gameArchive().readArchiveFile(path, zipped)
                           ? IMG_Load_IO(SDL_IOFromConstMem(zipped.data(), zipped.size()), true)
                           : IMG_Load(path.c_str());
    if (!raw) return result;

    SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (!rgba) {
        std::fprintf(stderr, "[dirk] bezel: failed to convert %s: %s\n",
                     path.c_str(), SDL_GetError());
        return result;
    }

    // SDL_PIXELFORMAT_RGBA32 guarantees byte-address order R,G,B,A
    // regardless of host endianness (unlike the *8888 formats, which are
    // defined by 32-bit word layout and so flip byte order across
    // endianness) -- so row[x*4+3] is always the alpha byte here.
    const int w = rgba->w;
    const int h = rgba->h;
    int minX = w, minY = h, maxX = -1, maxY = -1;
    if (SDL_LockSurface(rgba)) {
        const Uint8* pixels = static_cast<const Uint8*>(rgba->pixels);
        for (int y = 0; y < h; ++y) {
            const Uint8* row = pixels + static_cast<size_t>(y) * rgba->pitch;
            for (int x = 0; x < w; ++x) {
                Uint8 a = row[x * 4 + 3];
                if (a <= kCutoutAlphaThreshold) {
                    if (x < minX) minX = x;
                    if (y < minY) minY = y;
                    if (x > maxX) maxX = x;
                    if (y > maxY) maxY = y;
                }
            }
        }
        SDL_UnlockSurface(rgba);
    }

    if (maxX < minX || maxY < minY) {
        // No sufficiently-transparent pixels at all -- e.g. an opaque
        // reference image dropped in by mistake. Treat as "no usable
        // bezel" rather than drawing an opaque rectangle over the game.
        std::fprintf(stderr, "[dirk] bezel: %s has no transparent cutout, ignoring\n",
                     path.c_str());
        SDL_DestroySurface(rgba);
        return result;
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, rgba);
    SDL_DestroySurface(rgba);
    if (!tex) {
        std::fprintf(stderr, "[dirk] bezel: failed to create texture from %s: %s\n",
                     path.c_str(), SDL_GetError());
        return result;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    result.texture = tex;
    result.width = w;
    result.height = h;
    result.cutoutX = minX;
    result.cutoutY = minY;
    result.cutoutW = maxX - minX + 1;
    result.cutoutH = maxY - minY + 1;

    std::fprintf(stderr, "[dirk] bezel: loaded %s (%dx%d, cutout %d,%d %dx%d)\n",
                 path.c_str(), w, h, result.cutoutX, result.cutoutY,
                 result.cutoutW, result.cutoutH);
    return result;
}

// True if `filename` is a ".png" file, case-insensitively (matches
// hasDirkExtension()'s style elsewhere in the engine).
bool isPng(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return ext == ".png";
}

} // namespace

std::vector<Bezel> loadBezels(SDL_Renderer* renderer, const std::string& gameDataPath, const std::string& scriptStem) {
    std::vector<Bezel> result;
    if (!renderer || gameDataPath.empty()) return result;

    namespace fs = std::filesystem;
    const std::string bezelDir = gameDataPath + "Bezel/";
    // The folder's files: on disk, merged with a .zdirk's zip (see
    // core/GameArchive.h) -- just the disk otherwise.
    std::vector<std::string> names;
    if (!dirk::gameArchive().listDirectory(bezelDir, names)) return result; // no Bezel/ folder at all -- the normal case
    std::vector<fs::path> files;
    for (const auto& n : names) {
        bool isDir = true;
        if (dirk::gameArchive().exists(bezelDir + n, &isDir) && !isDir) files.push_back(fs::path(bezelDir) / n);
    }

    std::vector<fs::path> candidates;

    // Per-config override set first, e.g. two .dirk launchers sharing one
    // Movie/ folder (Chantze Stone / Triad Stone) but wanting different
    // bezels each -- see Bezel.h. Skipped entirely when the caller doesn't
    // know the launched .dirk's stem (empty string), not just when no file
    // happens to match it.
    if (!scriptStem.empty()) {
        for (const auto& file : files) {
            if (!isPng(file)) continue;
            const std::string name = file.filename().string();
            // "<stem>_" prefix, e.g. "Triad_bezel1.png" for stem "Triad" --
            // deliberately requires the underscore separator so "Triad2"
            // doesn't accidentally match stem "Triad".
            if (name.size() > scriptStem.size() + 1 &&
                name.compare(0, scriptStem.size(), scriptStem) == 0 &&
                name[scriptStem.size()] == '_') {
                candidates.push_back(file);
            }
        }
    }

    // No stem-specific files (or no stem known) -- fall back to every
    // plain "*.png" directly in Bezel/, the normal single-launcher case.
    if (candidates.empty()) {
        for (const auto& file : files) {
            if (isPng(file)) candidates.push_back(file);
        }
    }

    // Stable, predictable cycling order -- alphabetical by filename (not
    // directory_iterator's unspecified OS-dependent order).
    std::sort(candidates.begin(), candidates.end(),
              [](const fs::path& a, const fs::path& b) {
                  return a.filename().string() < b.filename().string();
              });

    result.reserve(candidates.size());
    for (const auto& path : candidates) {
        Bezel b = loadBezelFile(renderer, path.string());
        b.name = path.filename().string();
        if (b.valid()) result.push_back(b);
    }
    return result;
}

void destroyBezel(Bezel& bezel) {
    if (bezel.texture) {
        SDL_DestroyTexture(bezel.texture);
    }
    bezel = Bezel{};
}

void destroyBezels(std::vector<Bezel>& bezels) {
    for (auto& b : bezels) destroyBezel(b);
}

} // namespace dirk
