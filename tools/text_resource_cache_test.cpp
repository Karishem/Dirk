// Regression test for review point 3 (2026-09-26): the engine-side caches
// in EngineContext (textSpriteCache, fontCache) that bound memory for
// script patterns that used to leak without limit. Drives the REAL
// api_singe bindings through Lua, with a real (software/dummy) renderer
// and a real TTF font -- same calls, same shapes as the real scripts:
//   - Mad Dog's getMiddle(): spriteGetWidth(fontToSprite(p)), never
//     unloaded, every frame ("LOWER YOU GUN!!!");
//   - Mad Dog's drawName(): one fontToSprite per letter per frame;
//   - KimmyScript's getFont(): 4 fontLoad per call, called again and again.
// Measured before the fix with the same bindings: ~1 MB of texture per
// "LOWER YOU GUN!!!" call (150 MB after 5 s), 60 extra open fonts/files
// after 15 getFont() calls.
//
// Just as important: sharing must NOT change what a script can observe.
// Every fontToSprite() call still returns its own handle; unloading one
// handle never affects another (the KimmyScript getMiddle() pattern --
// fontToSprite + spriteUnload of a text that a permanent sprite also
// shows -- must leave that permanent sprite intact); a double unload is
// still a harmless no-op; the texture is destroyed once the last handle
// using it is unloaded.
//
// fontPrint texture cache (2026-09-26, EngineContext::fontPrintCache):
// fontPrint used to render + upload + destroy a texture on every call; now
// it keeps them between frames. Checked here: the pixels drawn are exactly
// what the old per-call code drew (reproduced in the test), on the first
// (cold) frame and on later (cached) frames; the same text is not rendered
// again; color/font/text each get their own entry; text that keeps
// changing is dropped after kFontPrintKeepFrames and the cache empties
// once nothing is drawn.
//
// Also covers review point 5: a font that failed to load used to print
// its "no valid active font" warning on EVERY fontPrint call (3,000
// identical lines in 10 s of play); now once per font handle.

#include "core/EngineContext.h"
#include "lua/api_singe.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <io.h>
#define DIRK_DUP _dup
#define DIRK_DUP2 _dup2
#else
#include <unistd.h>
#define DIRK_DUP dup
#define DIRK_DUP2 dup2
#endif

static bool g_allPass = true;

static void check(bool cond, const std::string& what) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) g_allPass = false;
}

static lua_State* L = nullptr;

static long long luaInt(const std::string& expr) {
    std::string code = "return " + expr;
    if (luaL_dostring(L, code.c_str()) != 0) {
        std::printf("lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -999;
    }
    long long v = static_cast<long long>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return v;
}

static void luaRun(const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != 0) {
        std::printf("lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        g_allPass = false;
    }
}

static size_t distinctTextures(const dirk::EngineContext& ctx) {
    std::set<SDL_Texture*> s;
    for (const auto& [h, r] : ctx.sprites) if (r.texture) s.insert(r.texture);
    return s.size();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path to a .ttf font>\n", argv[0]);
        return 1;
    }
    const std::string fontPath = argv[1];

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) {
        std::fprintf(stderr, "FAIL: init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("text_resource_cache_test", 640, 480, 0);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    check(renderer != nullptr, "renderer created (dummy video driver)");

    L = luaL_newstate();
    luaL_openlibs(L);
    dirk::EngineContext ctx;
    ctx.renderer = renderer;
    dirk::registerSingeApi(L, &ctx);
    std::fflush(stderr);

    luaRun("F = '" + fontPath + "'\nfBig = fontLoad(F, 144)\nfSmall = fontLoad(F, 32)\nfontSelect(fBig)\ncolorForeground(255, 0, 0)");

    // --- fontToSprite sharing -------------------------------------------
    luaRun("a = fontToSprite('LOWER YOU GUN!!!')  b = fontToSprite('LOWER YOU GUN!!!')");
    check(luaInt("a") != luaInt("b"), "same text twice -> two DIFFERENT handles (handle identity unchanged)");
    check(distinctTextures(ctx) == 1 && ctx.textSpriteCache.size() == 1, "...sharing ONE texture");
    check(luaInt("spriteGetWidth(a)") > 0 && luaInt("spriteGetWidth(a)") == luaInt("spriteGetWidth(b)"),
          "both handles report the same real width");

    luaRun("spriteUnload(a)");
    check(ctx.sprites.count(static_cast<dirk::Handle>(luaInt("b"))) == 1 && ctx.textSpriteCache.size() == 1 &&
              ctx.sprites[static_cast<dirk::Handle>(luaInt("b"))].texture != nullptr,
          "unloading one handle leaves the other fully usable (texture kept)");
    luaRun("spriteDraw(0, 0, b)"); // must not touch a destroyed texture (would crash under ASan/driver)
    luaRun("spriteUnload(a)");
    check(ctx.textSpriteCache.size() == 1, "double unload of the same handle is still a harmless no-op");
    luaRun("spriteUnload(b)");
    check(ctx.textSpriteCache.empty() && distinctTextures(ctx) == 0,
          "last handle unloaded -> shared texture destroyed, cache empty");

    // KimmyScript getMiddle() pattern next to a permanent sprite of the same text.
    luaRun("perm = fontToSprite('PAUSED')\n"
           "function getMiddle(p) local d = fontToSprite(p) local x = 320 - spriteGetWidth(d) * 0.5 spriteUnload(d) return x end\n"
           "for i = 1, 50 do getMiddle('PAUSED') end");
    check(ctx.sprites.count(static_cast<dirk::Handle>(luaInt("perm"))) == 1 &&
              ctx.sprites[static_cast<dirk::Handle>(luaInt("perm"))].texture != nullptr &&
              luaInt("spriteGetWidth(perm)") > 0,
          "permanent 'PAUSED' sprite survives 50 getMiddle('PAUSED') fontToSprite+spriteUnload cycles");
    check(ctx.textSpriteCache.size() == 1 && ctx.textSpriteCache.begin()->second.refs == 1,
          "...and the shared entry is back to exactly 1 reference (the permanent one)");
    luaRun("spriteUnload(perm)");

    // Different color / text / font never share.
    luaRun("colorForeground(255, 0, 0) c1 = fontToSprite('SCORE')\n"
           "colorForeground(0, 255, 0) c2 = fontToSprite('SCORE')\n"
           "c3 = fontToSprite('LEVEL')\n"
           "fontSelect(fSmall) c4 = fontToSprite('LEVEL')");
    check(distinctTextures(ctx) == 4, "different color, text or font -> separate textures (4 of 4)");
    luaRun("spriteUnload(c1) spriteUnload(c2) spriteUnload(c3) spriteUnload(c4)");
    check(ctx.textSpriteCache.empty(), "all four released cleanly");

    // Mad Dog's real leaking patterns: bounded now.
    luaRun("fontSelect(fBig) colorForeground(255, 0, 0)\n"
           "function madDogGetMiddle(p) return 720 - spriteGetWidth(fontToSprite(p)) * 0.5 end\n"
           "for i = 1, 300 do fontPrint(madDogGetMiddle('LOWER YOU GUN!!!'), 400, 'LOWER YOU GUN!!!') end\n"
           "function drawName(s) for k = 1, string.len(s) do spriteDraw(0, 0, fontToSprite(string.sub(s, k, k))) end end\n"
           "for i = 1, 600 do drawName('ABC') end");
    check(distinctTextures(ctx) == 4,
          "300 never-unloaded 'LOWER YOU GUN!!!' + 600 frames of drawName('ABC') -> 4 textures total, not 2100");

    // Empty text / no active font keep their old per-call behavior.
    luaRun("e = fontToSprite('')");
    check(luaInt("spriteGetWidth(e)") == 0 && ctx.sprites[static_cast<dirk::Handle>(luaInt("e"))].textCacheKey.empty(),
          "empty text: not cached, same empty sprite as before");

    // --- fontLoad cache -------------------------------------------------
    const size_t fontsOpenBefore = ctx.fontCache.size();
    luaRun("function getFont() g1 = fontLoad(F, 112) g2 = fontLoad(F, 140) g3 = fontLoad(F, 168) g4 = fontLoad(F, 224) end\n"
           "getFont() first = g1\n"
           "for i = 1, 14 do getFont() end");
    check(luaInt("first") != luaInt("g1"), "fontLoad still returns a fresh handle every call");
    check(ctx.fonts[static_cast<dirk::Handle>(luaInt("first"))].font == ctx.fonts[static_cast<dirk::Handle>(luaInt("g1"))].font,
          "...backed by the SAME already-open font (no reopen)");
    check(ctx.fontCache.size() == fontsOpenBefore + 4, "15 getFont() calls -> only 4 fonts actually opened (one per size)");
    luaRun("m1 = fontLoad('/definitely/not/here.ttf', 40) m2 = fontLoad('/definitely/not/here.ttf', 40)");
    check(ctx.fonts[static_cast<dirk::Handle>(luaInt("m1"))].font == nullptr &&
              ctx.fonts[static_cast<dirk::Handle>(luaInt("m2"))].font == nullptr,
          "missing font still reported as missing (nullptr) on every call");

    // --- Review point 5: missing-font warning printed once, not per call ---
    // Real stderr output captured to a file and counted (not just the
    // internal bookkeeping): 10 s at 60 fps, 5 fontPrint per frame, with a
    // font that failed to load -- 3,000 identical lines before the fix.
    {
        const std::filesystem::path logPath = std::filesystem::temp_directory_path() / "dirk_text_cache_stderr.log";
        std::fflush(stderr);
        const int savedFd = DIRK_DUP(fileno(stderr));
        FILE* redirected = std::freopen(logPath.string().c_str(), "w", stderr);
        (void)redirected;
        luaRun("bad = fontLoad('/definitely/not/here/gambler.ttf', 48) fontSelect(bad)\n"
               "for frame = 1, 600 do for i = 1, 5 do fontPrint(10, 10 * i, 'SCORE ' .. frame) end end\n"
               "fontSelect(fBig)");
        std::fflush(stderr);
        DIRK_DUP2(savedFd, fileno(stderr));
        size_t warnings = 0, lines = 0;
        {
            std::ifstream in(logPath);
            std::string line;
            while (std::getline(in, line)) {
                ++lines;
                if (line.find("no valid active font") != std::string::npos) ++warnings;
            }
        }
        check(warnings == 1, "3,000 fontPrint with a failed font -> the warning is written ONCE (got " +
                             std::to_string(warnings) + ", " + std::to_string(lines) + " lines total)");
        luaRun("bad2 = fontLoad('/also/missing.ttf', 20) fontSelect(bad2) fontPrint(0, 0, 'x') fontSelect(fBig)");
        check(ctx.fontWarnedNoFont.size() == 2, "a DIFFERENT broken font still gets its own warning");
        std::filesystem::remove(logPath);
    }

    // --- fontPrint texture cache ---------------------------------------
    {
        SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 480);
        SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);
        auto grab = [&]() {
            SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr);
            std::vector<uint8_t> px;
            if (s) {
                for (int y = 0; y < s->h; ++y) {
                    const uint8_t* row = static_cast<const uint8_t*>(s->pixels) + static_cast<size_t>(y) * s->pitch;
                    px.insert(px.end(), row, row + s->w * 4);
                }
                SDL_DestroySurface(s);
            }
            return px;
        };
        auto clear = [&]() {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
        };
        // One "frame" of a KimmyScript-like screen: two fonts, two colors,
        // a repeated label.
        const std::string screen =
            "fontSelect(fSmall) colorForeground(255, 200, 0, 255)\n"
            "fontPrint(20, 10, 'RANKINGS') fontPrint(400, 10, 'DIRK') fontPrint(20, 60, 'NORMAL') fontPrint(300, 60, 'NORMAL')\n"
            "colorForeground(255, 255, 255, 255)\n"
            "for k = 1, 8 do fontPrint(20, 80 + k * 40, k .. '.') fontPrint(80, 80 + k * 40, 'DIRK') fontPrint(300, 80 + k * 40, tostring(100000 - k * 731)) end\n"
            "fontSelect(fBig) colorForeground(0, 128, 255, 200) fontPrint(330, 300, 'HI')\n";
        // Reference: exactly what fontPrint drew before the cache existed
        // (render, upload, draw, destroy -- per call), same inputs.
        std::vector<uint8_t> expected;
        {
            TTF_Font* small = static_cast<TTF_Font*>(ctx.fonts[static_cast<dirk::Handle>(luaInt("fSmall"))].font);
            TTF_Font* big = static_cast<TTF_Font*>(ctx.fonts[static_cast<dirk::Handle>(luaInt("fBig"))].font);
            auto oldPrint = [&](TTF_Font* f, SDL_Color c, float x, float y, const std::string& t) {
                SDL_Surface* surf = TTF_RenderText_Blended(f, t.c_str(), 0, c);
                SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
                SDL_FRect d{x, y, static_cast<float>(surf->w), static_cast<float>(surf->h)};
                SDL_DestroySurface(surf);
                SDL_RenderTexture(renderer, tex, nullptr, &d);
                SDL_DestroyTexture(tex);
            };
            SDL_SetRenderTarget(renderer, target);
            clear();
            const SDL_Color gold{255, 200, 0, 255}, white{255, 255, 255, 255}, blue{0, 128, 255, 200};
            oldPrint(small, gold, 20, 10, "RANKINGS"); oldPrint(small, gold, 400, 10, "DIRK"); oldPrint(small, gold, 20, 60, "NORMAL"); oldPrint(small, gold, 300, 60, "NORMAL");
            for (int k = 1; k <= 8; ++k) {
                const float y = static_cast<float>(80 + k * 40);
                oldPrint(small, white, 20, y, std::to_string(k) + ".");
                oldPrint(small, white, 80, y, "DIRK");
                oldPrint(small, white, 300, y, std::to_string(100000 - k * 731));
            }
            oldPrint(big, blue, 330, 300, "HI");
            expected = grab();
        }
        ctx.fontPrintCache.clear();
        ctx.fontPrintCacheBytes = 0;
        luaRun("fontSelect(fBig) colorForeground(255, 0, 0)"); // state left by the tests above, irrelevant here

        SDL_SetRenderTarget(renderer, target);
        clear(); luaRun(screen); dirk::endOverlayFrame(&ctx);
        const std::vector<uint8_t> cold = grab();
        const size_t entries = ctx.fontPrintCache.size();
        std::set<SDL_Texture*> texBefore;
        for (const auto& kv : ctx.fontPrintCache) texBefore.insert(kv.second.texture);
        clear(); luaRun(screen); dirk::endOverlayFrame(&ctx);
        const std::vector<uint8_t> warm = grab();
        std::set<SDL_Texture*> texAfter;
        for (const auto& kv : ctx.fontPrintCache) texAfter.insert(kv.second.texture);

        const size_t inked = static_cast<size_t>(std::count_if(expected.begin(), expected.end(), [](uint8_t b) { return b != 0; }));
        check(inked > 10000, "reference screen really has text on it (" + std::to_string(inked) + " non-zero bytes)");
        check(!expected.empty() && cold == expected, "fontPrint, first frame: pixels identical to the old per-call rendering");
        check(warm == expected, "fontPrint, cached frame: pixels identical to the old per-call rendering");
        // gold/small: RANKINGS, DIRK, NORMAL (twice) -> 3 entries; white: 8 ranks + DIRK + 8 scores -> 17
        // (DIRK in white is NOT the gold one); blue/big: HI -> 1. Total 21.
        check(entries == 21, "one cache entry per distinct (font, color, text): 21 for this screen (got " +
                             std::to_string(entries) + ")");
        check(texAfter == texBefore && ctx.fontPrintCache.size() == entries,
              "second frame: nothing rendered again, the same 21 textures reused");
        luaRun("fontSelect(fSmall) colorForeground(255, 255, 255, 255) fontPrint(0, 0, 'DIRK')"
               " colorForeground(255, 255, 254, 255) fontPrint(0, 0, 'DIRK')");
        check(ctx.fontPrintCache.size() == entries + 1, "a different color is a different entry, the same one is not");

        // A timer that changes every frame, for 10 s at 60 fps, next to a
        // static label: the cache must stay small, not grow by 600.
        size_t maxEntries = 0;
        for (int f = 0; f < 600; ++f) {
            clear();
            luaRun("fontPrint(0, 0, 'TIME') fontPrint(100, 0, tostring(" + std::to_string(f) + "))");
            dirk::endOverlayFrame(&ctx);
            maxEntries = std::max(maxEntries, ctx.fontPrintCache.size());
        }
        check(maxEntries <= 200, "a counter changing every frame for 600 frames: cache peaks at " +
                                 std::to_string(maxEntries) + " entries (bounded, old ones dropped)");
        for (int f = 0; f < 200; ++f) dirk::endOverlayFrame(&ctx);
        check(ctx.fontPrintCache.empty() && ctx.fontPrintCacheBytes == 0,
              "nothing printed for 200 frames -> cache empty, byte count back to 0");
        // --- 2026-09-27: text wider than the screen is shrunk to fit ---
        // Reproduces the Space Ace case: a long line in a big font, x
        // computed from a SHORTER template string (so it starts off-screen
        // on the left and runs off on the right too).
        auto inkBox = [&](const std::vector<uint8_t>& px, int& minX, int& maxX, int& minY, int& maxY) {
            minX = 640; maxX = -1; minY = 480; maxY = -1;
            for (int y = 0; y < 480; ++y)
                for (int x = 0; x < 640; ++x)
                    if (px[(static_cast<size_t>(y) * 640 + x) * 4 + 0] | px[(static_cast<size_t>(y) * 640 + x) * 4 + 3]) {
                        minX = std::min(minX, x); maxX = std::max(maxX, x); minY = std::min(minY, y); maxY = std::max(maxY, y);
                    }
        };
        const float margin = 480.0f / 80.0f;
        clear();
        luaRun("fontSelect(fBig) colorForeground(255, 255, 255, 255)\n"
               "local h = fontToSprite('58 completed') local tw = spriteGetWidth(h) spriteUnload(h)\n"
               "wide = '58 completed moves out of 100 (0 Tilt: - 0%)'\n"
               "local h2 = fontToSprite(wide) wideW = spriteGetWidth(h2) spriteUnload(h2)\n"
               "fontPrint(math.floor(320 - tw / 2), 100, wide)");
        {
            int x0, x1, y0, y1;
            inkBox(grab(), x0, x1, y0, y1);
            const long long wideW = luaInt("wideW");
            check(wideW > 640, "the test line really is wider than the screen (" + std::to_string(wideW) + " px for 640)");
            check(x0 >= static_cast<int>(margin) - 1 && x1 <= 640 - static_cast<int>(margin),
                  "too-wide line shrunk INSIDE the screen, same font (ink " + std::to_string(x0) + ".." + std::to_string(x1) + ")");
            check(x1 - x0 > 560, "...using the screen width, not shrunk more than needed (ink width " + std::to_string(x1 - x0) + ")");
            check(std::abs((x0 + x1) / 2 - 320) <= 12, "...and centred on the screen");
        }
        // A line that fits but sits partly off-screen (e.g. sliding in) is
        // NOT touched: same pixels as the old per-call rendering.
        {
            TTF_Font* small = static_cast<TTF_Font*>(ctx.fonts[static_cast<dirk::Handle>(luaInt("fSmall"))].font);
            clear();
            SDL_Surface* surf = TTF_RenderText_Blended(small, "SLIDING IN", 0, SDL_Color{255, 255, 255, 255});
            SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
            SDL_FRect d{560.0f, 200.0f, static_cast<float>(surf->w), static_cast<float>(surf->h)};
            SDL_DestroySurface(surf);
            SDL_RenderTexture(renderer, tex, nullptr, &d);
            SDL_DestroyTexture(tex);
            const std::vector<uint8_t> ref = grab();
            clear();
            luaRun("fontSelect(fSmall) colorForeground(255, 255, 255, 255) fontPrint(560, 200, 'SLIDING IN')");
            check(grab() == ref, "a line that fits but runs off the edge is left exactly as before (not moved, not shrunk)");
        }

        SDL_SetRenderTarget(renderer, nullptr);
        SDL_DestroyTexture(target);
    }

#ifdef __linux__
    {
        size_t fds = 0;
        for (const auto& e : std::filesystem::directory_iterator("/proc/self/fd")) { (void)e; ++fds; }
        // stdin/out/err + the iterator's own fd + 6 distinct fonts (144, 32, 112, 140, 168, 224), with some slack
        check(fds <= 16, "open file descriptors stay bounded (" + std::to_string(fds) + ")");
    }
#endif

    lua_close(L);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    std::printf(g_allPass ? "\nALL PASS\n" : "\nSOME FAILED\n");
    return g_allPass ? 0 : 1;
}
