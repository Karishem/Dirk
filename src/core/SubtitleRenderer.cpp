#include "core/SubtitleRenderer.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dirk {

namespace {

// Share of the picture's height: font size, outline, bottom margin, and the
// widest a line may be before it is shrunk. Picked to match common subtitle
// practice (text about 1/19 of the picture height, sitting well above the
// bottom edge so a CRT bezel or overscan doesn't eat it).
constexpr float kFontShare = 0.052f;
constexpr float kBottomMarginShare = 0.06f;
constexpr float kMaxWidthShare = 0.92f;

} // namespace

int SubtitleRenderer::fontPixelsFor(float pictureHeight, int sizePercent) {
    return std::max(10, static_cast<int>(std::lround(pictureHeight * kFontShare * sizePercent / 100.0f)));
}

bool SubtitleRenderer::ensureFonts(int px) {
    if (fill_ && px == fontPx_) return true;
    if (fill_) { TTF_CloseFont(fill_); fill_ = nullptr; }
    fontPx_ = px;
    fill_ = TTF_OpenFont(fontPath_.c_str(), static_cast<float>(px));
    if (!fill_) {
        if (!fontMissingLogged_) {
            fontMissingLogged_ = true;
            std::fprintf(stderr, "[dirk] subtitles: can't open font %s (%s) -- subtitles not shown\n",
                         fontPath_.c_str(), SDL_GetError());
        }
        return false;
    }
    outlinePx_ = std::max(1, px / 13);
    return true;
}

// One line: the text in black stamped all around (16 directions, at the
// full and half outline radius), then the same text in white on top. Built
// from the SAME rendering of the line, so the outline can't drift from the
// letters -- SDL_ttf's own outline mode was tried first and left white
// fragments beside some glyphs ("(" in DejaVu Sans Bold, seen on a real
// capture), its outlined glyphs being positioned slightly differently.
static SDL_Surface* renderOutlinedLine(TTF_Font* font, const std::string& text, int r) {
    SDL_Surface* white = TTF_RenderText_Blended(font, text.c_str(), 0, SDL_Color{255, 255, 255, 255});
    SDL_Surface* black = TTF_RenderText_Blended(font, text.c_str(), 0, SDL_Color{0, 0, 0, 255});
    SDL_Surface* out = nullptr;
    if (white && black) {
        out = SDL_CreateSurface(white->w + 2 * r, white->h + 2 * r, SDL_PIXELFORMAT_ARGB8888);
    }
    if (out) {
        SDL_FillSurfaceRect(out, nullptr, 0); // fully transparent
        SDL_SetSurfaceBlendMode(black, SDL_BLENDMODE_BLEND);
        SDL_SetSurfaceBlendMode(white, SDL_BLENDMODE_BLEND);
        for (int ring = 0; ring < 2; ++ring) {
            const float radius = ring == 0 ? static_cast<float>(r) : r * 0.5f;
            for (int k = 0; k < 16; ++k) {
                const float a = k * (6.2831853f / 16.0f);
                SDL_Rect at{r + static_cast<int>(std::lround(radius * std::cos(a))),
                            r + static_cast<int>(std::lround(radius * std::sin(a))), black->w, black->h};
                SDL_BlitSurface(black, nullptr, out, &at);
            }
        }
        SDL_Rect at{r, r, white->w, white->h};
        SDL_BlitSurface(white, nullptr, out, &at);
    }
    if (white) SDL_DestroySurface(white);
    if (black) SDL_DestroySurface(black);
    return out;
}

void SubtitleRenderer::draw(SDL_Renderer* renderer, const std::vector<std::string>& lines, const SDL_FRect& area,
                            int sizePercent) {
    lastPlaced_ = Placed{};
    if (lines.empty() || area.w <= 0 || area.h <= 0) {
        // Nothing on screen: drop the texture now rather than holding it
        // until the next cue.
        if (tex_) { SDL_DestroyTexture(tex_); tex_ = nullptr; }
        cachedKey_.clear();
        return;
    }
    const int px = fontPixelsFor(area.h, sizePercent);
    std::string key = std::to_string(px);
    for (const std::string& l : lines) { key += '\n'; key += l; }

    if (key != cachedKey_ || !tex_) {
        if (tex_) { SDL_DestroyTexture(tex_); tex_ = nullptr; }
        cachedKey_.clear();
        if (!ensureFonts(px)) return;

        std::vector<SDL_Surface*> rendered;
        int totalW = 0, totalH = 0;
        const int step = TTF_GetFontLineSkip(fill_);
        for (const std::string& l : lines) {
            if (SDL_Surface* line = renderOutlinedLine(fill_, l, outlinePx_)) {
                totalW = std::max(totalW, line->w);
                rendered.push_back(line);
            }
        }
        if (rendered.empty()) return;
        totalH = step * static_cast<int>(rendered.size() - 1) + rendered.back()->h;

        SDL_Surface* all = SDL_CreateSurface(totalW, totalH, SDL_PIXELFORMAT_ARGB8888);
        if (all) {
            SDL_FillSurfaceRect(all, nullptr, 0); // fully transparent
            int y = 0;
            for (SDL_Surface* s : rendered) {
                SDL_Rect at{(totalW - s->w) / 2, y, s->w, s->h}; // each line centered
                SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_BLEND);
                SDL_BlitSurface(s, nullptr, all, &at);
                y += step;
            }
            tex_ = SDL_CreateTextureFromSurface(renderer, all);
            if (tex_) {
                SDL_SetTextureBlendMode(tex_, SDL_BLENDMODE_BLEND);
                texW_ = all->w;
                texH_ = all->h;
                cachedKey_ = key;
            }
            SDL_DestroySurface(all);
        }
        for (SDL_Surface* s : rendered) SDL_DestroySurface(s);
        if (!tex_) return;
    }

    // Too wide for the picture: shrink the whole block (same font, same
    // proportions) rather than cut it or wrap it.
    const float maxW = area.w * kMaxWidthShare;
    const float scale = std::min(1.0f, maxW / static_cast<float>(texW_));
    const float w = texW_ * scale;
    const float h = texH_ * scale;
    SDL_FRect dst{area.x + (area.w - w) / 2.0f, area.y + area.h - area.h * kBottomMarginShare - h, w, h};
    if (dst.y < area.y) dst.y = area.y;
    SDL_RenderTexture(renderer, tex_, nullptr, &dst);
    lastPlaced_ = Placed{dst.x, dst.y, dst.w, dst.h};
}

void SubtitleRenderer::shutdown() {
    if (tex_) { SDL_DestroyTexture(tex_); tex_ = nullptr; }
    if (fill_) { TTF_CloseFont(fill_); fill_ = nullptr; }
    cachedKey_.clear();
    fontPx_ = 0;
}

} // namespace dirk
