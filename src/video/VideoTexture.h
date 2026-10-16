#pragma once

// Creates the streaming IYUV texture a VideoSource's decoded pictures are
// uploaded into, with the colour matrix + range THAT movie was encoded
// with (review point 8, 2026-09-26 -- see VideoSource::yuvMatrix()).
// Shared by main.cpp (the main movie) and api_singe.cpp's videoLoad()
// (menu preview clips), so both always agree.
//
// Before this, both called plain SDL_CreateTexture(), i.e. SDL3's default
// BT.601 limited for every movie -- wrong for HD content, which is BT.709
// (measured: pure red displayed as RGB(233,0,2)).
//
// Never fails where the old call wouldn't have: if the renderer refuses a
// texture with an explicit colour space, falls back to exactly the old
// plain SDL_CreateTexture() call (and says so on stderr).

#include "video/VideoSource.h"

#include <string>

struct SDL_Renderer;
struct SDL_Texture;

namespace dirk {

SDL_Texture* createVideoTexture(SDL_Renderer* renderer, const VideoSource& video, int width, int height);

// One-line human description for the log, e.g.
// "BT.709, limited range (tagged in the file)" or
// "BT.601, limited range (not tagged -- guessed from 720x480)".
std::string describeVideoColors(const VideoSource& video);

} // namespace dirk
