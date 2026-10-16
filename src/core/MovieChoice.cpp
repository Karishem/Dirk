#include "core/MovieChoice.h"
#include "core/JoystickProfile.h" // resolveSharedAsset

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace dirk {

namespace {

bool isVideoFile(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov"; // same list as findVideoInSubfolder
}

// Picture size and display aspect, read from the container only (no
// decoding): a fraction of a second even for a big file.
void probe(MovieVersion& v) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, v.path.c_str(), nullptr, nullptr) != 0) return;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (si >= 0) {
            AVStream* st = fmt->streams[si];
            v.width = st->codecpar->width;
            v.height = st->codecpar->height;
            double sar = 1.0;
            const AVRational r = av_guess_sample_aspect_ratio(fmt, st, nullptr);
            if (r.num > 0 && r.den > 0) {
                const double x = static_cast<double>(r.num) / r.den;
                if (x > 0.25 && x < 4.0) sar = x; // same sanity range as VideoSource
            }
            if (v.height > 0) v.aspect = v.width * sar / v.height;
        }
    }
    avformat_close_input(&fmt);
}

std::string aspectLabel(double a) {
    if (a <= 0.0) return "";
    if (std::fabs(a - 4.0 / 3.0) < 0.06) return "4/3";
    if (std::fabs(a - 16.0 / 9.0) < 0.06) return "16/9";
    if (std::fabs(a - 16.0 / 10.0) < 0.03) return "16/10";
    if (std::fabs(a - 5.0 / 4.0) < 0.03) return "5/4";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f:1", a);
    return buf;
}

struct Text {
    SDL_Texture* tex = nullptr;
    float w = 0, h = 0;
};

Text makeText(SDL_Renderer* r, TTF_Font* font, const std::string& s, SDL_Color c) {
    Text t;
    if (!font || s.empty()) return t;
    SDL_Surface* surf = TTF_RenderText_Blended(font, s.c_str(), 0, c);
    if (!surf) return t;
    t.tex = SDL_CreateTextureFromSurface(r, surf);
    t.w = static_cast<float>(surf->w);
    t.h = static_cast<float>(surf->h);
    SDL_DestroySurface(surf);
    return t;
}

void drawText(SDL_Renderer* r, const Text& t, float cx, float y, float scale = 1.0f) {
    if (!t.tex) return;
    SDL_FRect dst{cx - t.w * scale / 2, y, t.w * scale, t.h * scale};
    SDL_RenderTexture(r, t.tex, nullptr, &dst);
}

// SDL_RenderDebugText fallback (no font file): 8x8 glyphs, scaled.
void drawDebug(SDL_Renderer* r, const std::string& s, float cx, float y, float scale, SDL_Color c) {
    SDL_SetRenderScale(r, scale, scale);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderDebugText(r, (cx / scale) - s.size() * 4.0f, y / scale, s.c_str());
    SDL_SetRenderScale(r, 1.0f, 1.0f);
}

} // namespace

std::vector<MovieVersion> findMovieVersions(const std::string& videoPath) {
    namespace fs = std::filesystem;
    std::vector<MovieVersion> out;
    std::error_code ec;
    const fs::path dir = fs::path(videoPath).parent_path();
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || !isVideoFile(e.path())) continue;
        MovieVersion v;
        v.path = e.path().string();
        out.push_back(v);
    }
    if (out.size() < 2) return {};
    for (auto& v : out) probe(v);
    std::sort(out.begin(), out.end(), [](const MovieVersion& a, const MovieVersion& b) { return a.path < b.path; });
    for (auto& v : out) {
        v.label = fs::path(v.path).stem().string();
        v.detail = aspectLabel(v.aspect);
        if (v.width > 0) v.detail += (v.detail.empty() ? "" : "  ") + std::to_string(v.width) + "x" + std::to_string(v.height);
    }
    return out;
}

int chooseMovieVersion(const std::vector<MovieVersion>& versions, const std::string& sharedEngineRoot,
                       bool fullscreen, bool swapFaceButtons) {
    namespace fs = std::filesystem;
    const int n = static_cast<int>(versions.size());
    if (n < 2) return 0;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[dirk] movie choice: SDL_Init failed (%s) -- using %s\n", SDL_GetError(),
                     fs::path(versions[0].path).filename().string().c_str());
        return 0;
    }
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    const bool ttfOk = TTF_Init();

    SDL_Window* window = SDL_CreateWindow("Dirk", 1024, 768, fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "[dirk] movie choice: no window (%s) -- using %s\n", SDL_GetError(),
                     fs::path(versions[0].path).filename().string().c_str());
        if (window) SDL_DestroyWindow(window);
        if (ttfOk) TTF_Quit();
        return 0;
    }

    std::fprintf(stderr, "[dirk] %d versions of the movie, the player chooses:", n);
    for (int i = 0; i < n; ++i) {
        std::fprintf(stderr, "%s %s (%s)", i ? "," : "", fs::path(versions[i].path).filename().string().c_str(),
                     versions[i].detail.c_str());
    }
    std::fprintf(stderr, "\n");

    SDL_Texture* logo = IMG_LoadTexture(renderer, resolveSharedAsset(sharedEngineRoot, "dirk_loading.png").c_str());
    const std::string fontPath = resolveSharedAsset(sharedEngineRoot, "dirk_loading_font.ttf");

    std::vector<SDL_Event> deviceEvents;
    std::vector<SDL_Gamepad*> pads;
    std::vector<SDL_Joystick*> sticks;

    int chosen = -2;   // -2 = still waiting, -1 = quit
    int sel = 0;
    int builtH = -1;
    TTF_Font* big = nullptr;
    TTF_Font* small = nullptr;
    Text title;
    std::vector<Text> lines, names;
    Text hint;
    bool axisHeld = false;

    auto freeTexts = [&]() {
        if (title.tex) SDL_DestroyTexture(title.tex);
        if (hint.tex) SDL_DestroyTexture(hint.tex);
        for (auto& t : lines) if (t.tex) SDL_DestroyTexture(t.tex);
        for (auto& t : names) if (t.tex) SDL_DestroyTexture(t.tex);
        title = hint = Text{};
        lines.clear();
        names.clear();
        if (big) TTF_CloseFont(big);
        if (small) TTF_CloseFont(small);
        big = small = nullptr;
    };
    auto prefix = [&](int i) {
        if (n == 2) return std::string(i == 0 ? "B1   " : "B2   ");
        return std::string();
    };
    auto move = [&](int d) {
        if (n > 2) sel = (sel + d + n) % n;
    };
    auto press = [&](int button) { // 1 = B1, 2 = B2
        if (n == 2) chosen = button - 1;
        else if (button == 1) chosen = sel;
    };

    while (chosen == -2) {
        SDL_Event ev;
        while (chosen == -2 && SDL_PollEvent(&ev)) {
            {
                switch (ev.type) {
                case SDL_EVENT_QUIT:
                    chosen = -1;
                    break;
                case SDL_EVENT_JOYSTICK_ADDED:
                    deviceEvents.push_back(ev);
                    if (!SDL_IsGamepad(ev.jdevice.which)) {
                        if (SDL_Joystick* j = SDL_OpenJoystick(ev.jdevice.which)) sticks.push_back(j);
                    }
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    deviceEvents.push_back(ev);
                    if (SDL_Gamepad* g = SDL_OpenGamepad(ev.gdevice.which)) pads.push_back(g);
                    break;
                case SDL_EVENT_JOYSTICK_REMOVED:
                case SDL_EVENT_GAMEPAD_REMOVED:
                case SDL_EVENT_GAMEPAD_REMAPPED:
                    deviceEvents.push_back(ev);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.repeat) break;
                    switch (ev.key.scancode) {
                    case SDL_SCANCODE_ESCAPE: chosen = -1; break;
                    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: press(1); break;
                    case SDL_SCANCODE_LALT: press(2); break;
                    case SDL_SCANCODE_1: if (n == 2) chosen = 0; break;
                    case SDL_SCANCODE_2: if (n == 2) chosen = 1; break;
                    case SDL_SCANCODE_UP: move(-1); break;
                    case SDL_SCANCODE_DOWN: move(1); break;
                    default: break;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                    int b = ev.gbutton.button;
                    if (swapFaceButtons) {
                        if (b == SDL_GAMEPAD_BUTTON_SOUTH) b = SDL_GAMEPAD_BUTTON_EAST;
                        else if (b == SDL_GAMEPAD_BUTTON_EAST) b = SDL_GAMEPAD_BUTTON_SOUTH;
                    }
                    if (b == SDL_GAMEPAD_BUTTON_SOUTH) press(1);
                    else if (b == SDL_GAMEPAD_BUTTON_EAST) press(2);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) move(-1);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) move(1);
                    break;
                }
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                        if (!axisHeld && std::abs(ev.gaxis.value) > 20000) { axisHeld = true; move(ev.gaxis.value < 0 ? -1 : 1); }
                        else if (std::abs(ev.gaxis.value) < 8000) axisHeld = false;
                    }
                    break;
                case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
                    if (SDL_IsGamepad(ev.jbutton.which)) break; // seen as a gamepad button above
                    if (ev.jbutton.button == 3) press(1);
                    else if (ev.jbutton.button == 4) press(2);
                    break;
                case SDL_EVENT_JOYSTICK_HAT_MOTION:
                    if (SDL_IsGamepad(ev.jhat.which)) break;
                    if (ev.jhat.value & SDL_HAT_UP) move(-1);
                    else if (ev.jhat.value & SDL_HAT_DOWN) move(1);
                    break;
                case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                    if (SDL_IsGamepad(ev.jaxis.which) || ev.jaxis.axis != 1) break;
                    if (!axisHeld && std::abs(ev.jaxis.value) > 20000) { axisHeld = true; move(ev.jaxis.value < 0 ? -1 : 1); }
                    else if (std::abs(ev.jaxis.value) < 8000) axisHeld = false;
                    break;
                default:
                    break;
                }
            }
        }
        if (chosen != -2) break;

        int outW = 0, outH = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &outW, &outH);
        if (outH != builtH) {
            freeTexts();
            builtH = outH;
            if (ttfOk) {
                big = TTF_OpenFont(fontPath.c_str(), std::max(14.0f, outH / 14.0f));
                small = TTF_OpenFont(fontPath.c_str(), std::max(10.0f, outH / 32.0f));
            }
            const SDL_Color white{255, 255, 255, 255}, grey{170, 170, 170, 255};
            title = makeText(renderer, big, "CHOOSE THE MOVIE", white);
            for (int i = 0; i < n; ++i) {
                lines.push_back(makeText(renderer, big, prefix(i) + versions[i].label, white));
                names.push_back(makeText(renderer, small, versions[i].detail, grey));
            }
            hint = makeText(renderer, small, n == 2 ? "B1 or B2      ESC: quit" : "UP / DOWN, then B1      ESC: quit", grey);
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        const float cx = outW / 2.0f;
        float y = outH * 0.06f;
        if (logo) {
            const float maxSide = 0.28f * std::min(outW, outH);
            const float scale = std::min(1.0f, maxSide / std::max(logo->w, logo->h));
            SDL_FRect dst{cx - logo->w * scale / 2, y, logo->w * scale, logo->h * scale};
            SDL_RenderTexture(renderer, logo, nullptr, &dst);
            y += dst.h + outH * 0.04f;
        }
        const SDL_Color white{255, 255, 255, 255}, grey{170, 170, 170, 255}, red{230, 40, 40, 255};
        const float bigStep = big ? 0.0f : 8.0f * (outH / 300.0f);
        if (big) { drawText(renderer, title, cx, y); y += title.h * 1.4f; }
        else { drawDebug(renderer, "CHOOSE THE MOVIE", cx, y, outH / 300.0f, white); y += bigStep * 2; }
        for (int i = 0; i < n; ++i) {
            const bool hi = n > 2 && i == sel;
            if (big) {
                if (lines[i].tex) {
                    if (hi) SDL_SetTextureColorMod(lines[i].tex, red.r, red.g, red.b);
                    else SDL_SetTextureColorMod(lines[i].tex, 255, 255, 255);
                }
                drawText(renderer, lines[i], cx, y);
                y += lines[i].h;
                drawText(renderer, names[i], cx, y);
                y += names[i].h * 1.6f;
            } else {
                drawDebug(renderer, prefix(i) + versions[i].label, cx, y, outH / 300.0f, hi ? red : white);
                y += bigStep * 1.4f;
                drawDebug(renderer, versions[i].detail, cx, y, outH / 600.0f, grey);
                y += bigStep * 1.6f;
            }
        }
        if (hint.tex) drawText(renderer, hint, cx, outH - hint.h * 2.0f);
        else drawDebug(renderer, n == 2 ? "B1 or B2      ESC: quit" : "UP / DOWN, then B1      ESC: quit", cx, outH - 24.0f, 1.5f, grey);
        SDL_RenderPresent(renderer);
        SDL_Delay(30);
    }

    freeTexts();
    if (logo) SDL_DestroyTexture(logo);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (ttfOk) TTF_Quit();
    for (auto* g : pads) SDL_CloseGamepad(g);
    for (auto* j : sticks) SDL_CloseJoystick(j);
    for (auto& e : deviceEvents) SDL_PushEvent(&e);

    if (chosen >= 0) {
        std::fprintf(stderr, "[dirk] movie chosen: %s (%s)\n", fs::path(versions[chosen].path).filename().string().c_str(),
                     versions[chosen].detail.c_str());
    } else {
        std::fprintf(stderr, "[dirk] movie choice: the player quit\n");
    }
    return chosen;
}

} // namespace dirk
