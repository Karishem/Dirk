#pragma once
// EngineContext: the shared state the registered native (dirkXxx/singeXxx)
// Lua functions in src/lua/api_singe.cpp read and write, and that
// src/core/main.cpp drives the render/video loop from. Kept as a plain
// struct with no behavior of its own -- it's a hand-off point between the
// three subsystems (video, Lua API, SDL rendering), not a god-object.

#include <cstdint>
#include <string>
#include <unordered_map>

#include "video/VideoSource.h"

struct SDL_Renderer;
struct SDL_Texture;
struct Mix_Chunk;

namespace dirk {

using Handle = int;

// spriteLoad/fontLoad/soundLoad hand back opaque numeric handles in real
// Singe (inferred signatures, feasibility study section 2). We mirror that
// with a simple incrementing allocator + lookup tables rather than trying
// to match Singe's actual handle values, which are never Lua-visible as
// anything but opaque numbers.
struct SpriteResource {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

struct SoundResource {
    Mix_Chunk* chunk = nullptr;
    int lastChannel = -1; // best-effort tracking for soundStop(handle)
};

struct FontResource {
    std::string path;
    int size = 0;
    // TODO(dirk): no real glyph rasterizer wired up yet (needs SDL2_ttf,
    // not yet installed in this environment). fontPrint currently draws a
    // placeholder box per character -- see api_singe.cpp.
};

struct EngineContext {
    VideoSource* disc = nullptr;
    SDL_Renderer* renderer = nullptr;

    std::string scriptPath; // directory containing the running script
    std::string dataPath;   // directory for data assets

    bool discPaused = false;
    double discFps = 29.97; // framework.singe's default (discSetFPS(29.97))

    bool quitRequested = false;

    // colorForeground(r,g,b[,o]) / colorBackground(...) state -- confirmed
    // signature per the Singe manual (feasibility study section 2).
    uint8_t fgR = 255, fgG = 255, fgB = 255, fgA = 255;
    uint8_t bgR = 0, bgG = 0, bgB = 0, bgA = 255;

    int overlayWidth = 0;
    int overlayHeight = 0;

    std::unordered_map<Handle, SpriteResource> sprites;
    std::unordered_map<Handle, SoundResource> sounds;
    std::unordered_map<Handle, FontResource> fonts;
    Handle nextHandle = 1;
    Handle activeFontHandle = 0;

    Handle allocHandle() { return nextHandle++; }
};

} // namespace dirk
