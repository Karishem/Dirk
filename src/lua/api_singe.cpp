#include "lua/api_singe.h"
#include "core/EngineContext.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <SDL.h>
#include <SDL_mixer.h>

#include <cstdio>
#include <cstring>

namespace dirk {

namespace {

// Every registered function is a plain Lua C closure with the EngineContext*
// as its single upvalue (see regFn below) -- this is how each native
// function reaches renderer/disc/resource state without any globals.
EngineContext* ctxFromUpvalue(lua_State* L) {
    return static_cast<EngineContext*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// ---------------------------------------------------------------------
// Disc (primary video transport). Backed directly by VideoSource.
// Signatures: feasibility study section 2, "Signatures inférées".
// ---------------------------------------------------------------------

int l_discSkipToFrame(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    int64_t frame = static_cast<int64_t>(luaL_checknumber(L, 1));
    if (ctx->disc) ctx->disc->seekToFrame(frame);
    return 0;
}

int l_discPause(lua_State* L) {
    ctxFromUpvalue(L)->discPaused = true;
    return 0;
}

int l_discPlay(lua_State* L) {
    ctxFromUpvalue(L)->discPaused = false;
    return 0;
}

int l_discStop(lua_State* L) {
    // No distinct "stopped" state modeled yet -- treated as pause. TODO(dirk):
    // revisit once discGetState()-style callers show up (none do in the 38).
    ctxFromUpvalue(L)->discPaused = true;
    return 0;
}

int l_discGetFrame(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    int64_t frame = ctx->disc ? ctx->disc->currentFrameNumber() : 0;
    lua_pushnumber(L, static_cast<lua_Number>(frame));
    return 1;
}

int l_discSetFPS(lua_State* L) {
    ctxFromUpvalue(L)->discFps = luaL_checknumber(L, 1);
    return 0;
}

int l_discSearch(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    int64_t frame = static_cast<int64_t>(luaL_checknumber(L, 1));
    if (ctx->disc) ctx->disc->seekToFrame(frame);
    return 0;
}

int l_discAudio(lua_State* L) {
    // discAudio(track, enabled) -- toolbox.singe routes music/effects to the
    // two stereo channels. TODO(dirk): wire to real per-channel muting once
    // audio playback is more than SDL2_mixer one-shot chunks.
    luaL_checknumber(L, 1);
    // arg 2 is boolean-ish; ignore for now.
    return 0;
}

// ---------------------------------------------------------------------
// Video (secondary stream, menu.singe only). Explicitly deferred by the
// user ("Menu.singe on verra plus tard") and its call-site signatures were
// never confirmed against real code the way disc/sprite/font were. These
// are honest no-op stubs, not real implementations: they exist only so a
// script that references them doesn't crash with "attempt to call a nil
// value", and they log so that becomes visible during testing.
// ---------------------------------------------------------------------

int l_videoStub(lua_State* L) {
    lua_Debug ar;
    const char* name = "video*";
    if (lua_getstack(L, 0, &ar) && lua_getinfo(L, "n", &ar) && ar.name) {
        name = ar.name;
    }
    std::fprintf(stderr, "[dirk] warning: %s() called -- menu.singe video layer not implemented yet\n", name);
    return 0;
}

// ---------------------------------------------------------------------
// Render 2D: sprites, bitmap font, color, overlay sizing.
// ---------------------------------------------------------------------

int l_colorForeground(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    ctx->fgR = static_cast<uint8_t>(luaL_checkinteger(L, 1));
    ctx->fgG = static_cast<uint8_t>(luaL_checkinteger(L, 2));
    ctx->fgB = static_cast<uint8_t>(luaL_checkinteger(L, 3));
    ctx->fgA = static_cast<uint8_t>(luaL_optinteger(L, 4, 255));
    return 0;
}

int l_colorBackground(lua_State* L) {
    // Not confirmed among the 38 actually-called functions, but the manual
    // gives the same signature as colorForeground, so it's cheap and safe
    // to support.
    EngineContext* ctx = ctxFromUpvalue(L);
    ctx->bgR = static_cast<uint8_t>(luaL_checkinteger(L, 1));
    ctx->bgG = static_cast<uint8_t>(luaL_checkinteger(L, 2));
    ctx->bgB = static_cast<uint8_t>(luaL_checkinteger(L, 3));
    ctx->bgA = static_cast<uint8_t>(luaL_optinteger(L, 4, 255));
    return 0;
}

int l_spriteLoad(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const char* path = luaL_checkstring(L, 1);

    Handle h = ctx->allocHandle();
    SpriteResource res;

    // SDL2 core only decodes BMP without SDL2_image (not yet installed in
    // this environment -- most KimmyScript assets are almost certainly PNG,
    // so this will legitimately fail for real content today). We still hand
    // back a valid handle with a null texture rather than erroring out, so
    // the rest of the script keeps running; spriteDraw silently skips a
    // null texture. TODO(dirk): add SDL2_image and drop this limitation.
    SDL_Surface* surf = SDL_LoadBMP(path);
    if (surf) {
        res.width = surf->w;
        res.height = surf->h;
        if (ctx->renderer) {
            res.texture = SDL_CreateTextureFromSurface(ctx->renderer, surf);
        }
        SDL_FreeSurface(surf);
    } else {
        std::fprintf(stderr, "[dirk] warning: spriteLoad(\"%s\") failed (%s) -- handle %d has no image\n",
                     path, SDL_GetError(), h);
    }

    ctx->sprites[h] = res;
    lua_pushinteger(L, h);
    return 1;
}

int l_spriteUnload(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    auto it = ctx->sprites.find(h);
    if (it != ctx->sprites.end()) {
        if (it->second.texture) SDL_DestroyTexture(it->second.texture);
        ctx->sprites.erase(it);
    }
    return 0;
}

int l_spriteGetWidth(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    auto it = ctx->sprites.find(h);
    lua_pushinteger(L, it != ctx->sprites.end() ? it->second.width : 0);
    return 1;
}

int l_spriteGetHeight(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    auto it = ctx->sprites.find(h);
    lua_pushinteger(L, it != ctx->sprites.end() ? it->second.height : 0);
    return 1;
}

int l_spriteDraw(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    int x = static_cast<int>(luaL_checkinteger(L, 1));
    int y = static_cast<int>(luaL_checkinteger(L, 2));
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 3));

    auto it = ctx->sprites.find(h);
    if (it == ctx->sprites.end() || !it->second.texture || !ctx->renderer) {
        return 0; // unloaded/failed sprite, or no renderer bound -- skip quietly
    }
    SDL_SetTextureColorMod(it->second.texture, ctx->fgR, ctx->fgG, ctx->fgB);
    SDL_SetTextureAlphaMod(it->second.texture, ctx->fgA);
    SDL_Rect dst{x, y, it->second.width, it->second.height};
    SDL_RenderCopy(ctx->renderer, it->second.texture, nullptr, &dst);
    return 0;
}

int l_fontLoad(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const char* path = luaL_checkstring(L, 1);
    int size = static_cast<int>(luaL_checkinteger(L, 2));
    Handle h = ctx->allocHandle();
    ctx->fonts[h] = FontResource{path, size};
    lua_pushinteger(L, h);
    return 1;
}

int l_fontSelect(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    ctx->activeFontHandle = static_cast<Handle>(luaL_checkinteger(L, 1));
    return 0;
}

int l_fontQuality(lua_State* L) {
    // FONT_QUALITY_SOLID/SHADED/BLENDED -- accepted, not yet applied.
    // TODO(dirk): meaningful once real glyph rendering (SDL_ttf) exists.
    luaL_checkinteger(L, 1);
    return 0;
}

int l_fontToSprite(lua_State* L) {
    // Renders `text` (in the active font/color) to a new sprite handle.
    // TODO(dirk): no real rasterizer wired up yet -- returns a valid but
    // empty (null-texture) handle so callers that store/pass it around
    // don't crash; nothing will actually be drawn until SDL_ttf lands.
    EngineContext* ctx = ctxFromUpvalue(L);
    luaL_checkstring(L, 1);
    Handle h = ctx->allocHandle();
    ctx->sprites[h] = SpriteResource{};
    lua_pushinteger(L, h);
    return 1;
}

int l_fontPrint(lua_State* L) {
    // fontPrint(x, y, text) -- the single most-called function in the API
    // (483 call sites) but real bitmap/TTF rendering needs SDL2_ttf, not
    // yet installed in this environment (see FontResource TODO). Placeholder:
    // draw one fg-colored box per character so overlay composition and
    // layout can be visually smoke-tested before real glyphs exist.
    EngineContext* ctx = ctxFromUpvalue(L);
    int x = static_cast<int>(luaL_checkinteger(L, 1));
    int y = static_cast<int>(luaL_checkinteger(L, 2));
    const char* text = luaL_checkstring(L, 3);

    if (!ctx->renderer) return 0;
    SDL_SetRenderDrawColor(ctx->renderer, ctx->fgR, ctx->fgG, ctx->fgB, ctx->fgA);
    int len = static_cast<int>(std::strlen(text));
    const int glyphW = 8, glyphH = 12, advance = 9;
    for (int i = 0; i < len; ++i) {
        if (text[i] == ' ') continue;
        SDL_Rect r{x + i * advance, y, glyphW, glyphH};
        SDL_RenderFillRect(ctx->renderer, &r);
    }
    return 0;
}

int l_overlayClear(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    if (!ctx->renderer) return 0;
    SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 0);
    SDL_RenderClear(ctx->renderer);
    return 0;
}

int l_overlaySetResolution(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    ctx->overlayWidth = static_cast<int>(luaL_checkinteger(L, 1));
    ctx->overlayHeight = static_cast<int>(luaL_checkinteger(L, 2));
    return 0;
}

int l_vldpGetWidth(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushinteger(L, ctx->disc ? ctx->disc->width() : 0);
    return 1;
}

int l_vldpGetHeight(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushinteger(L, ctx->disc ? ctx->disc->height() : 0);
    return 1;
}

// ---------------------------------------------------------------------
// Sound.
// ---------------------------------------------------------------------

int l_soundLoad(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const char* path = luaL_checkstring(L, 1);
    Handle h = ctx->allocHandle();
    SoundResource res;
    res.chunk = Mix_LoadWAV(path);
    if (!res.chunk) {
        std::fprintf(stderr, "[dirk] warning: soundLoad(\"%s\") failed (%s) -- handle %d is silent\n",
                     path, Mix_GetError(), h);
    }
    ctx->sounds[h] = res;
    lua_pushinteger(L, h);
    return 1;
}

int l_soundPlay(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    auto it = ctx->sounds.find(h);
    if (it != ctx->sounds.end() && it->second.chunk) {
        it->second.lastChannel = Mix_PlayChannel(-1, it->second.chunk, 0);
    }
    return 0;
}

int l_soundStop(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    auto it = ctx->sounds.find(h);
    if (it != ctx->sounds.end() && it->second.lastChannel >= 0) {
        Mix_HaltChannel(it->second.lastChannel);
    }
    return 0;
}

// ---------------------------------------------------------------------
// System / scripting. dirkQuit/dirkGetScriptPath/dirkGetDataPath are the
// renamed Singe-family functions (user's explicit request); singeXxx
// aliases are injected right after registration, below.
// ---------------------------------------------------------------------

int l_dirkGetScriptPath(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushstring(L, ctx->scriptPath.c_str());
    return 1;
}

int l_dirkGetDataPath(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushstring(L, ctx->dataPath.c_str());
    return 1;
}

int l_dirkQuit(lua_State* L) {
    ctxFromUpvalue(L)->quitRequested = true;
    return 0;
}

int l_scriptPush(lua_State* L) {
    // Chains menu.singe -> a game script. Deferred along with the rest of
    // menu.singe; logs instead of silently doing nothing so the gap is
    // visible while testing.
    const char* path = luaL_checkstring(L, 1);
    std::fprintf(stderr, "[dirk] warning: scriptPush(\"%s\") not implemented yet (menu.singe deferred)\n", path);
    return 0;
}

int l_debugPrint(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    std::fprintf(stderr, "[lua] %s\n", text);
    return 0;
}

void regFn(lua_State* L, EngineContext* ctx, const char* name, lua_CFunction fn) {
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, fn, 1);
    lua_setglobal(L, name);
}

void aliasGlobal(lua_State* L, const char* oldName, const char* newName) {
    lua_getglobal(L, newName);
    lua_setglobal(L, oldName);
}

} // namespace

void registerSingeApi(lua_State* L, EngineContext* ctx) {
    // Disc.
    regFn(L, ctx, "discSkipToFrame", l_discSkipToFrame);
    regFn(L, ctx, "discPause", l_discPause);
    regFn(L, ctx, "discPlay", l_discPlay);
    regFn(L, ctx, "discStop", l_discStop);
    regFn(L, ctx, "discGetFrame", l_discGetFrame);
    regFn(L, ctx, "discSetFPS", l_discSetFPS);
    regFn(L, ctx, "discSearch", l_discSearch);
    regFn(L, ctx, "discAudio", l_discAudio);

    // Video (menu.singe, deferred) -- stubs only, see l_videoStub comment.
    regFn(L, ctx, "videoLoad", l_videoStub);
    regFn(L, ctx, "videoUnload", l_videoStub);
    regFn(L, ctx, "videoPlay", l_videoStub);
    regFn(L, ctx, "videoSeek", l_videoStub);
    regFn(L, ctx, "videoDraw", l_videoStub);
    regFn(L, ctx, "videoGetFrame", l_videoStub);
    regFn(L, ctx, "videoSetVolume", l_videoStub);

    // Render 2D.
    regFn(L, ctx, "colorForeground", l_colorForeground);
    regFn(L, ctx, "colorBackground", l_colorBackground);
    regFn(L, ctx, "spriteLoad", l_spriteLoad);
    regFn(L, ctx, "spriteUnload", l_spriteUnload);
    regFn(L, ctx, "spriteGetWidth", l_spriteGetWidth);
    regFn(L, ctx, "spriteGetHeight", l_spriteGetHeight);
    regFn(L, ctx, "spriteDraw", l_spriteDraw);
    regFn(L, ctx, "fontLoad", l_fontLoad);
    regFn(L, ctx, "fontSelect", l_fontSelect);
    regFn(L, ctx, "fontQuality", l_fontQuality);
    regFn(L, ctx, "fontToSprite", l_fontToSprite);
    regFn(L, ctx, "fontPrint", l_fontPrint);
    regFn(L, ctx, "overlayClear", l_overlayClear);
    regFn(L, ctx, "overlaySetResolution", l_overlaySetResolution);
    regFn(L, ctx, "vldpGetWidth", l_vldpGetWidth);
    regFn(L, ctx, "vldpGetHeight", l_vldpGetHeight);

    // Sound.
    regFn(L, ctx, "soundLoad", l_soundLoad);
    regFn(L, ctx, "soundPlay", l_soundPlay);
    regFn(L, ctx, "soundStop", l_soundStop);

    // System / scripting -- native dirkXxx names.
    regFn(L, ctx, "dirkGetScriptPath", l_dirkGetScriptPath);
    regFn(L, ctx, "dirkGetDataPath", l_dirkGetDataPath);
    regFn(L, ctx, "dirkQuit", l_dirkQuit);
    regFn(L, ctx, "scriptPush", l_scriptPush);
    regFn(L, ctx, "debugPrint", l_debugPrint);

    // Backward-compat aliases for the 3 Singe-named functions KimmyScript's
    // framework layer actually calls (framework.singe:30-31, menu.singe:
    // 360/702/745, service.singe:5842/5928) -- injected here rather than
    // editing those files, so zero existing KimmyScript source changes.
    aliasGlobal(L, "singeGetScriptPath", "dirkGetScriptPath");
    aliasGlobal(L, "singeGetDataPath", "dirkGetDataPath");
    aliasGlobal(L, "singeQuit", "dirkQuit");
}

} // namespace dirk
