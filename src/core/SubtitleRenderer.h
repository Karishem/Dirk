#pragma once

#include <string>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_FRect;
typedef struct TTF_Font TTF_Font;

namespace dirk {

// Draws the current subtitle lines (core/Subtitles.h) at the bottom of the
// picture. Cyril's rules (2026-09-27): always the same font whatever the
// game (Dirk's own dirk_loading_font.ttf, DejaVu Sans Bold -- never the
// skin's, so nothing looks like it changed by accident); line breaks are
// the .srt's own, Dirk never wraps; a line too wide for the picture is
// shrunk, same font. White with a black outline so it reads on any image.
// main.cpp draws it on the picture, under the game's overlay, so menus and
// scores the game draws always stay on top.
//
// Size follows the picture's on-screen height (so it looks the same in a
// window, fullscreen, with or without a bezel). The text is rendered into
// one texture only when the lines or the size change -- a cue on screen
// for 3 seconds costs one render, then a plain texture copy per frame.
class SubtitleRenderer {
public:
    explicit SubtitleRenderer(std::string fontPath) : fontPath_(std::move(fontPath)) {}
    ~SubtitleRenderer() { shutdown(); }
    SubtitleRenderer(const SubtitleRenderer&) = delete;
    SubtitleRenderer& operator=(const SubtitleRenderer&) = delete;

    // `area`: the picture's on-screen rectangle (window pixels). Empty
    // `lines` draws nothing (and keeps nothing cached beyond the fonts).
    // `sizePercent`: the player's "Subtitle Size" (LaunchSettings::
    // subtitleSize), 100 = normal; the outline and the bottom margin keep
    // their proportions.
    void draw(SDL_Renderer* renderer, const std::vector<std::string>& lines, const SDL_FRect& area,
              int sizePercent = 100);

    // Frees the texture and the fonts. Must run before TTF_Quit()/
    // SDL_DestroyRenderer(); safe to call twice.
    void shutdown();

    // Where the last draw() put the text (window pixels), for tests;
    // w == 0 when nothing was drawn.
    struct Placed { float x = 0, y = 0, w = 0, h = 0; };
    Placed lastPlaced() const { return lastPlaced_; }

    // Font size (pixels) for a picture `pictureHeight` pixels tall, at
    // `sizePercent` of the normal size.
    static int fontPixelsFor(float pictureHeight, int sizePercent = 100);

private:
    bool ensureFonts(int px);

    std::string fontPath_;
    bool fontMissingLogged_ = false;
    int fontPx_ = 0;
    TTF_Font* fill_ = nullptr;
    int outlinePx_ = 0; // outline thickness, ~1/13 of the font size

    std::string cachedKey_;
    SDL_Texture* tex_ = nullptr;
    int texW_ = 0, texH_ = 0;
    Placed lastPlaced_;
};

} // namespace dirk
