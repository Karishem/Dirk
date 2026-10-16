// Regression test for the Kimmy logo size (2026-09-26, Cyril: "the size is
// right in 1080, but on lower-resolution videos it gets a bit big").
// Drives the real dirkKimmyLogo() binding through Lua into an overlay-sized
// render target, for several movie resolutions and for KimmyScript's Lo
// mode (overlay at half the movie size), and measures the logo actually
// drawn. It must take the same share of the overlay height as on a 1080p
// movie (226/1080, the size Cyril validated) whatever the resolution, and
// keep its position (one LINEH = OVLH/80 from the top and right edges).
// Before the fix it was drawn at 226 px whenever the overlay matched the
// movie: 31% of a 720p picture, 47% of a 480p one.

#include "core/EngineContext.h"
#include "lua/api_singe.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <string>

static bool g_allPass = true;
static void check(bool cond, const std::string& what) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) g_allPass = false;
}

int main() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* window = SDL_CreateWindow("kimmy_logo_test", 640, 480, 0);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    check(renderer != nullptr, "renderer created (dummy video driver)");

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    dirk::EngineContext ctx;
    ctx.renderer = renderer;
    dirk::registerSingeApi(L, &ctx);

    struct Case { int w, h; const char* what; };
    const Case cases[] = {
        {1920, 1080, "1080p movie"}, {1440, 1080, "1440x1080 movie"}, {1280, 720, "720p movie"},
        {720, 480, "480p movie"}, {640, 480, "640x480 movie"},
        {960, 540, "1080p movie in Lo (half size)"}, {360, 240, "480p movie in Lo (half size)"},
    };
    const double expectedShare = 226.0 / 1080.0;
    for (const Case& c : cases) {
        ctx.overlayWidth = c.w;
        ctx.overlayHeight = c.h;
        SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, c.w, c.h);
        SDL_SetRenderTarget(renderer, target);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
        if (luaL_dostring(L, "dirkKimmyLogo()") != 0) { std::printf("lua error: %s\n", lua_tostring(L, -1)); lua_pop(L, 1); }
        SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_DestroyTexture(target);
        int minX = c.w, minY = c.h, maxX = -1, maxY = -1;
        if (s) {
            for (int y = 0; y < s->h; ++y) {
                const Uint8* row = static_cast<const Uint8*>(s->pixels) + static_cast<size_t>(y) * s->pitch;
                for (int x = 0; x < s->w; ++x) {
                    Uint8 r, g, b, a;
                    SDL_GetRGBA(reinterpret_cast<const Uint32*>(row)[x], SDL_GetPixelFormatDetails(s->format), nullptr, &r, &g, &b, &a);
                    if (a > 0) { if (x < minX) minX = x; if (x > maxX) maxX = x; if (y < minY) minY = y; if (y > maxY) maxY = y; }
                }
            }
            SDL_DestroySurface(s);
        }
        const int drawnH = maxY >= 0 ? maxY - minY + 1 : 0;
        const double share = static_cast<double>(drawnH) / c.h;
        const double lineH = c.h / 80.0;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (%dx%d): logo %d px high = %.1f%% of the height (expected %.1f%%), top %d, right margin %d (LINEH %.1f)",
                      c.what, c.w, c.h, drawnH, 100 * share, 100 * expectedShare, minY, c.w - 1 - maxX, lineH);
        // +-2 px: the PNG has soft (partly transparent) edges and the
        // scaled size is rounded to whole pixels.
        check(drawnH > 0 && std::fabs(drawnH - expectedShare * c.h) <= 2.0 &&
              std::fabs(minY - lineH) <= 2.0 && std::fabs((c.w - 1 - maxX) - lineH) <= 2.0, buf);
    }

    lua_close(L);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf(g_allPass ? "\nALL PASS\n" : "\nSOME FAILED\n");
    return g_allPass ? 0 : 1;
}
