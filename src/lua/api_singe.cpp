#include "lua/api_singe.h"
#include "core/GameArchive.h"
#include "core/BlackScan.h"
#include "core/EngineContext.h"
#include "core/LaunchSettings.h" // full LaunchSettings definition -- EngineContext.h
                                  // only forward-declares it (see there); the
                                  // dirkGet/Set* settings accessors below need
                                  // the real struct to read/write its fields.
#include "lua/KimmyLogoData.h"   // kKimmyLogoPng/kKimmyLogoPngLen -- see l_dirkKimmyLogo below
#include "video/VideoTexture.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <cstdio>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <memory>
#include <system_error>

namespace dirk {

namespace {

// Every registered function is a plain Lua C closure with the EngineContext*
// as its single upvalue (see regFn below) -- this is how each native
// function reaches renderer/disc/resource state without any globals.
EngineContext* ctxFromUpvalue(lua_State* L) {
    return static_cast<EngineContext*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// Optional per-game Overlay/Sounds/Fonts: a real, explicit request -- not
// every game should have to ship its own Overlay/, Sounds/ and Fonts/
// folders any more. Every real call site (grep-verified across every
// game's *.kimmy/*.dirk) builds these paths as MYDIR .. "/Overlay/<name>",
// MYDIR .. "/Sounds/<name>" or MYDIR .. "/Fonts/<name>" -- MYDIR being
// either that game's own real folder (dirkGetScriptPath()) or, for an
// older un-migrated script, a literal folder name resolved against dirk's
// own cwd (the shared engine root -- see findSharedEngineRoot()/the chdir
// in main.cpp, still in effect for the whole run).
//
// Where each falls back to, and why they're no longer symmetric (revised
// after two real logs -- see below): originally all three fell back to a
// blind KimmyScript/Default<Tag>/ folder the player had to maintain
// separately from the Skin/ folders. Explicit request: retire that
// separate folder for Overlay and Fonts and reuse KimmyScript/Skin/DEFAULT/
// instead, since it already exists, is already curated, and is already
// reachable/switchable from the in-game "Skin" options menu -- one asset
// set serving two purposes instead of two to keep in sync.
//   - Overlay -> skinAssetFallbackPath(): KimmyScript/Skin<SKINDIR>/Overlay/
//     <same tail>, SKINDIR defaulting to "/DEFAULT" (globals.kimmy sets it
//     unconditionally before any per-game code runs, and setSkin()'s
//     dip_ScType==0 branch never touches it -- so this is "/DEFAULT" unless
//     the player picked a different skin for this game). Tail-preserving,
//     same as before: a missing "crosshair.png" still looks for a
//     "crosshair.png"-shaped stand-in, now in Skin/DEFAULT/Overlay/ rather
//     than a bespoke DefaultOverlay/.
//   - Fonts -> skinDefaultFontFallbackPath(): tail-preserving, but flat --
//     no "/Fonts/" segment in the candidate, unlike Overlay. Real
//     Skin/<NAME>/ folders have no Fonts/ subfolder at all -- just
//     font.ttf/font2.ttf/fontul.ttf directly at the skin's own root
//     (confirmed: KimmyScript/Skin/DEFAULT/font.ttf exists, .../DEFAULT/
//     Fonts/font.ttf does not). A naive tail-preserving attempt that
//     inserted "/Fonts/" the same way Overlay does therefore silently never
//     matched -- confirmed directly from a player's real dirk_log.txt: a
//     game whose own font was named "font.ttf" got no fallback at all once
//     the old DefaultFonts/ folder was removed. But every real non-shooting
//     game names its own font file "font.ttf" (or "font2.ttf" for a second
//     one) -- confirmed by the same player -- which is exactly the flat
//     naming Skin/<NAME>/ already uses. So the tail (just "font.ttf" or
//     "font2.ttf", nothing more, since these games never nest their Fonts/
//     folder) still gets preserved, only the "/Fonts/" middle segment is
//     dropped: MYDIR.."/Fonts/font2.ttf" missing -> tries
//     Skin<SKINDIR>/font2.ttf, not Skin<SKINDIR>/font.ttf -- so a game with
//     two distinct fonts still gets two distinct (skin-appropriate)
//     fallbacks instead of both collapsing onto the same one. A shooting
//     game's own differently-named font (gambler.ttf, helldorado.ttf, ...)
//     still won't have a same-named match in Skin/<NAME>/ and falls through
//     to "no font" exactly as before -- expected, since those games keep
//     their own Fonts/ folder per an earlier conversation.
//   - Sounds -> defaultAssetFallbackPath(): unchanged, still
//     KimmyScript/DefaultSounds/<same tail>. No real Skin/<NAME>/ folder
//     ships a Sounds/ subfolder (skins are visual/typographic only), so
//     there's no equivalent shared asset to reuse here -- this one genuinely
//     needs its own folder, which the player keeps maintaining directly.
std::string assetPathTail(const std::string& originalPath, const char* folderTag) {
    const std::string marker = std::string("/") + folderTag + "/";
    // Every real game script builds these paths with forward slashes
    // (MYDIR .. "/Overlay/..."), even when MYDIR itself is a native
    // Windows path that starts with backslashes -- so only the forward-
    // slash marker is ever actually produced, confirmed by grep across
    // every game. Deliberately not also matching a backslash form, so this
    // never fires on some unrelated path that happens to contain
    // "\Overlay\" for a different reason.
    size_t pos = originalPath.rfind(marker);
    if (pos == std::string::npos) return {};
    return originalPath.substr(pos + marker.size());
}

// Sounds only now -- see the block comment above for why Overlay and Fonts
// moved to the Skin/-based fallbacks below instead.
std::string defaultAssetFallbackPath(const std::string& originalPath, const char* folderTag) {
    std::string tail = assetPathTail(originalPath, folderTag);
    if (tail.empty()) return {};
    return std::string("./KimmyScript/Default") + folderTag + "/" + tail;
}

// Reads SKINDIR live off the Lua global (always a plain script global,
// never routed through EngineContext, so this needs no new engine state).
// Returns "" if SKINDIR isn't set at all yet (called unusually early,
// before globals.kimmy's own init line runs) -- genuinely nothing to fall
// back to in that case, vs. every normal case where it's at least
// "/DEFAULT". See the block comment above for the full SKINDIR/setSkin()
// story (it IS a real per-game persisted choice, via Cfg/game.cfg, not
// just the front-end menu's own look) and why Overlay stays tail-preserving
// while Fonts (below) doesn't.
std::string readSkinDir(lua_State* L) {
    lua_getglobal(L, "SKINDIR");
    std::string skinDir;
    if (lua_isstring(L, -1)) skinDir = lua_tostring(L, -1);
    lua_pop(L, 1);
    return skinDir;
}

std::string skinAssetFallbackPath(lua_State* L, const std::string& originalPath, const char* folderTag) {
    std::string tail = assetPathTail(originalPath, folderTag);
    if (tail.empty()) return {};

    std::string skinDir = readSkinDir(L);
    if (skinDir.empty()) return {};

    return std::string("./KimmyScript/Skin") + skinDir + "/" + folderTag + "/" + tail;
}

// Fonts-specific: tail-preserving like skinAssetFallbackPath, but flat --
// no folderTag segment inserted, since real Skin/<NAME>/ folders keep their
// fonts directly at their own root. See the block comment above for why.
std::string skinDefaultFontFallbackPath(lua_State* L, const std::string& originalPath) {
    std::string tail = assetPathTail(originalPath, "Fonts");
    if (tail.empty()) return {};

    std::string skinDir = readSkinDir(L);
    if (skinDir.empty()) return {};

    return std::string("./KimmyScript/Skin") + skinDir + "/" + tail;
}

// Per-game Custom/ override for sounds (2026-09-23, Cyril, after already
// having this for Overlay/Fonts): "vu que maintenant on a la fonctionalite
// /custom peut etre qu'on pourrait aussi mettre les sons dedans". Same idea
// as Custom/ for Overlay/Fonts (a game folder's own Custom/ subfolder wins
// over whatever would otherwise be used, skin included), but Custom/ for
// Overlay/Fonts lives entirely in main.kimmy (fillOVL()/getFont()) -- pure
// KimmyScript, no engine change, since those two already go through a
// SHARED script every game calls, which fillOVL/getFont branch on skin
// (dip_ScType) BEFORE ever reaching the C++ side. Sound has no such shared
// branching point: every game's own globals.kimmy (or equivalent) calls
// soundLoad(DIR.."/Sounds/<name>") directly, and skin/default fallback for
// sound (skinAssetFallbackPath/defaultAssetFallbackPath just above/below,
// see the block comment at the top of this file) already lives entirely on
// this C++ side, in l_soundLoad. So this is the one place that can add a
// Custom/ override for sound without editing every game's own script by
// hand -- symmetric result, different layer, for a structural reason, not
// an inconsistency.
//
// Flat, like Custom/font.ttf (not nested under a "Custom/Sounds/" folder)
// -- matches every other Custom/ file's own convention (see main.kimmy's
// comment on why only Overlay/Lores needs a subfolder: a same-named-file
// collision between the main and Lores sprite sets that sound has no
// equivalent of), and keeps this a single flat folder for whatever a
// player wants to override, sounds included, no separate Sounds/
// subfolder to remember. Checked BEFORE the game's own literal DIR..
// "/Sounds/<name>" path (an override replaces it, same as Custom/ does for
// Overlay/Fonts -- not merely a fallback for a missing file).
std::string customOverridePath(const std::string& originalPath, const char* folderTag) {
    std::string tail = assetPathTail(originalPath, folderTag);
    if (tail.empty()) return {};

    const std::string marker = std::string("/") + folderTag + "/";
    size_t pos = originalPath.rfind(marker); // can't be npos: assetPathTail() above just found this same marker
    std::string gameFolder = originalPath.substr(0, pos);
    return gameFolder + "/Custom/" + tail;
}

// See EngineContext::movieAudioDeviceId for the full story of why this
// exists. Wrap any blocking disk-I/O loader (TTF_OpenFont, IMG_Load,
// Mix_LoadWAV, ...) that a KimmyScript script can call mid-game in one of
// these, for the duration of the call -- nothing else needed:
//   ScopedAudioFreeze freeze(ctx);
//   res.font = TTF_OpenFont(path, size);
// Restores whatever the movie's own pause state was before the call
// (mirroring the exact same save/pause/restore pattern main.cpp already
// uses around the native settings panel) rather than unconditionally
// resuming -- calling this while the disc is legitimately paused (the
// player has the game paused, or the settings panel is open) must leave it
// paused afterward, not accidentally start audio playing again.
class ScopedAudioFreeze {
public:
    explicit ScopedAudioFreeze(EngineContext* ctx)
        : stream_(ctx ? ctx->movieAudioStream : nullptr),
          resumePaused_(ctx && ctx->discPaused) {
        // SDL3 split the old 2-arg SDL_PauseAudioDevice(dev, pause_on)
        // toggle into two separate calls (Pause.../Resume...), and it now
        // acts on the audio STREAM, not a raw device id -- see
        // EngineContext::movieAudioStream.
        if (stream_) SDL_PauseAudioStreamDevice(stream_);
    }
    ~ScopedAudioFreeze() {
        if (stream_) {
            if (resumePaused_) SDL_PauseAudioStreamDevice(stream_); // already paused for another reason -- stay paused
            else SDL_ResumeAudioStreamDevice(stream_);
        }
    }
    ScopedAudioFreeze(const ScopedAudioFreeze&) = delete;
    ScopedAudioFreeze& operator=(const ScopedAudioFreeze&) = delete;

private:
    SDL_AudioStream* stream_;
    bool resumePaused_;
};

// ---------------------------------------------------------------------
// Disc (primary video transport). Backed directly by VideoSource.
// Signatures: feasibility study section 2, "Signatures inférées".
// ---------------------------------------------------------------------

int l_discSkipToFrame(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    int64_t frame = static_cast<int64_t>(luaL_checknumber(L, 1));
    // seekToFrame() below decodes-and-discards every frame between the
    // nearest preceding keyframe and `frame` -- real, sometimes non-trivial
    // decode work (see VideoSource.h) that blocks this whole thread exactly
    // like fontLoad/spriteLoad/soundLoad's disk I/O does (see
    // ScopedAudioFreeze above for the full story of why that matters). This
    // is the one that actually turned out to matter in practice: main.singe
    // calls discSkipToFrame() at the start of every level/scene (see the
    // comment below), so unlike a one-off font load, this runs at literally
    // every scene cut in the game -- confirmed on a real capture to be
    // exactly where a ~900ms AV-sync hit was appearing, at a scene
    // transition, with no font/sprite/sound loading anywhere near the size
    // needed to explain it.
    ScopedAudioFreeze freeze(ctx);
    if (ctx->disc && ctx->disc->seekToFrame(frame)) {
        // Real (V)LDP hardware treats a search as "seek, then keep playing"
        // -- pausing is a separate, explicit act. KimmyScript relies on
        // this: main.singe calls discSkipToFrame() at the start of every
        // level/scene to cue up its clip, and never follows most of those
        // with discPlay() (grep across every *.singe file: discPlay() is
        // only ever called from menu.singe, discSkipToFrame() dozens of
        // times from main.singe). Every menu/freeze-frame screen (New Game,
        // Quit, ...) instead does `discSkipToFrame(x); discPause()` --
        // seek-and-resume immediately followed by an explicit pause to
        // freeze there -- so auto-resuming here doesn't fight that pattern,
        // it's what makes it work: without this, once any menu paused the
        // disc, nothing in main.singe ever un-paused it again, and every
        // subsequent discSkipToFrame() (e.g. into real gameplay) landed on
        // the right frame but stayed frozen -- "the game doesn't go back
        // into play mode" after any pausing menu.
        ctx->discPaused = false;
        ++ctx->discPauseScriptWrites; // see EngineContext::discPauseScriptWrites
    }
    return 0;
}

int l_discPause(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    ctx->discPaused = true;
    ++ctx->discPauseScriptWrites; // see EngineContext::discPauseScriptWrites
    return 0;
}

int l_discPlay(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    ctx->discPaused = false;
    ++ctx->discPauseScriptWrites; // see EngineContext::discPauseScriptWrites
    return 0;
}

int l_discStop(lua_State* L) {
    // No distinct "stopped" state modeled yet -- treated as pause. TODO(dirk):
    // revisit once discGetState()-style callers show up (none do in the 38).
    EngineContext* ctx = ctxFromUpvalue(L);
    ctx->discPaused = true;
    ++ctx->discPauseScriptWrites; // see EngineContext::discPauseScriptWrites
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
    // Used to force-clear ctx->discPaused unconditionally, same as
    // l_discSkipToFrame above -- on the assumption that "search" and "skip
    // to frame" share the same real-hardware "seek, then keep playing"
    // semantics. Traced back to a real, reported Mad Dog McCree bug
    // ("les bouteilles se cassent toutes seules"): doLevelTut() (the
    // bottle-shooting practice level) does `discPause();
    // discSearch(move[currentMove][frameShootStart]); lvlState=lvlRunning`
    // -- no discPlay() anywhere in that branch -- clearly relying on the
    // disc STAYING paused at the target frame so lvlRunning's own
    // `if (currentFrame == move[currentMove][frameShootEnd])` check (gating
    // both the shot-detection and the waitFor(3) miss-timeout) gets
    // evaluated across many real ticks while the player aims. With the old
    // unconditional clear here, that explicit discPause() was immediately
    // undone by this very call: the disc kept playing, blew straight past
    // frameShootEnd in a single frame period, and neither the shot check
    // nor the 3-second timeout ever got a real window to run -- looking
    // exactly like every bottle "breaking itself" the instant the target
    // frame is reached, then playback continuing on into whatever content
    // follows (confirmed matching the report: it kept going into the level-
    // select/continue screens). This is a widespread idiom in this game,
    // not a one-off: about 40% of this file's own discSearch() calls have
    // no discPlay() anywhere nearby, and are the same "land here and hold"
    // pattern as this one.
    //
    // Fix: search only seeks now -- it no longer touches ctx->discPaused at
    // all, so whatever the disc's play/pause state was going into the call
    // is exactly what it is coming out. This is safe for the calls that DO
    // follow up with an explicit discPlay() (unaffected either way, since
    // that call sets ctx->discPaused = false itself); it only changes
    // behavior for a search made while already explicitly paused with no
    // discPlay() following -- exactly the "land here and hold" idiom this
    // was breaking. l_discSkipToFrame above is deliberately left with its
    // own auto-resume intact -- its doc comment above justifies that
    // specifically from main.singe's own real usage (skip-then-pause, never
    // the reverse), a different, still-valid idiom from this one.
    ScopedAudioFreeze freeze(ctx);
    if (ctx->disc) {
        ctx->disc->seekToFrame(frame);
    }
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

// discChangeSpeed(numerator, denominator) was here as a no-op stub, purely
// to stop the 7 lightgun games' shared setupClip() (toolbox.singe) from
// crashing on a missing global. Removed again, deliberately, once the real
// fix landed: every one of those 7 games is Cyril's own maintained code
// (revived from RDG2010's now-inactive original Singe1 releases), not
// third-party content to work around -- so the dead call itself came out
// of setupClip() in all 7 toolbox.singe files instead of living on here
// forever doing nothing. If a future game genuinely needs disc speed
// control, implement it against a real call site then, not speculatively.

// ---------------------------------------------------------------------
// Video (secondary stream, menu.singe only) -- e.g. the per-game
// attract-mode preview clip in a game-select menu:
//   VIDEO_ATTRACT = videoLoad(GAME_LIST[i].ATTRACT)
//   videoPlay(VIDEO_ATTRACT); videoSeek(VIDEO_ATTRACT, START); videoSetVolume(VIDEO_ATTRACT, 0, 0)
//   -- every frame: --
//   videoDraw(VIDEO_ATTRACT, x1, y1, x2, y2)
//   if videoGetFrame(VIDEO_ATTRACT) > END then videoSeek(VIDEO_ATTRACT, START) end
//
// Distinct from the primary `disc` movie (VideoSource in EngineContext):
// each handle here owns its own VideoSource decoding its own file, and is
// drawn scaled into an arbitrary rectangle rather than filling the window.
// Was previously stubbed out entirely (a real l_videoStub that no-op'd and
// logged) -- harmless for a script that merely *calls* these, but
// videoGetFrame() returning no value at all meant `videoGetFrame(h) > END`
// raised "attempt to compare nil with number" every time the menu's draw
// loop ran, which is almost certainly the "bug when I go into the menu"
// this replaces. Every real call site mutes these clips
// (videoSetVolume(h, 0, 0)), so unlike the primary disc, no audio decode is
// wired up here -- videoSetVolume is accepted and ignored.
// ---------------------------------------------------------------------

void uploadPictureToTexture(SDL_Texture* tex, const AVFrame* f) {
    if (!tex || !f) return;
    SDL_UpdateYUVTexture(tex, nullptr,
                          f->data[0], f->linesize[0],
                          f->data[1], f->linesize[1],
                          f->data[2], f->linesize[2]);
}

int l_videoLoad(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const char* path = luaL_checkstring(L, 1);

    Handle h = ctx->allocHandle();
    VideoResource res;
    res.source = std::make_unique<VideoSource>();
    if (res.source->open(path)) {
        res.width = res.source->width();
        res.height = res.source->height();
        if (ctx->renderer) {
            // Same per-movie colour matrix/range as the main movie (review
            // point 8, see video/VideoTexture.h).
            res.texture = createVideoTexture(ctx->renderer, *res.source, res.width, res.height);
        }
        res.source->stepForward(); // prime a first picture so an immediate videoDraw shows something
        uploadPictureToTexture(res.texture, res.source->currentPicture());
    } else {
        std::fprintf(stderr, "[dirk] warning: videoLoad(\"%s\") failed -- handle %d has no video\n", path, h);
    }
    ctx->videos[h] = std::move(res);
    lua_pushinteger(L, h);
    return 1;
}

int l_videoUnload(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    auto it = ctx->videos.find(h);
    if (it != ctx->videos.end()) {
        if (it->second.texture) SDL_DestroyTexture(it->second.texture);
        ctx->videos.erase(it);
    }
    return 0;
}

int l_videoPlay(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    auto it = ctx->videos.find(h);
    if (it != ctx->videos.end()) {
        it->second.playing = true;
        it->second.lastTickMs = SDL_GetTicks();
        it->second.frameAccumulator = 0.0;
    }
    return 0;
}

int l_videoSeek(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    int64_t frame = static_cast<int64_t>(luaL_checknumber(L, 2));
    auto it = ctx->videos.find(h);
    if (it != ctx->videos.end() && it->second.source) {
        it->second.source->seekToFrame(frame);
        it->second.frameAccumulator = 0.0;
        it->second.lastTickMs = SDL_GetTicks();
        uploadPictureToTexture(it->second.texture, it->second.source->currentPicture());
    }
    return 0;
}

int l_videoSetVolume(lua_State* L) {
    // videoSetVolume(handle, left, right) -- accepted, intentionally
    // ignored; see the comment above this section.
    luaL_checkinteger(L, 1);
    return 0;
}

int l_videoDraw(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    int x1 = static_cast<int>(luaL_checkinteger(L, 2));
    int y1 = static_cast<int>(luaL_checkinteger(L, 3));
    int x2 = static_cast<int>(luaL_checkinteger(L, 4));
    int y2 = static_cast<int>(luaL_checkinteger(L, 5));

    auto it = ctx->videos.find(h);
    if (it == ctx->videos.end() || !it->second.texture || !it->second.source || !ctx->renderer) {
        return 0; // unloaded/failed video, or no renderer bound -- skip quietly, matching spriteDraw
    }
    VideoResource& res = it->second;

    if (res.playing) {
        Uint32 now = SDL_GetTicks();
        double dt = (now - res.lastTickMs) / 1000.0;
        res.lastTickMs = now;
        double fps = res.source->fps();
        if (fps > 0.0) {
            res.frameAccumulator += dt * fps;
            if (res.frameAccumulator > 5.0) res.frameAccumulator = 5.0; // cap catch-up, same as the primary disc
            while (res.frameAccumulator >= 1.0) {
                if (!res.source->stepForward()) break; // end of stream -- the script's own loop-at-ATTRACT_END logic reseeks
                res.frameAccumulator -= 1.0;
            }
            uploadPictureToTexture(res.texture, res.source->currentPicture());
        }
    }

    SDL_Rect dst{x1, y1, x2 - x1, y2 - y1};
    SDL_FRect dstF; SDL_RectToFRect(&dst, &dstF);
    SDL_RenderTexture(ctx->renderer, res.texture, nullptr, &dstF);
    return 0;
}

int l_videoGetFrame(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    auto it = ctx->videos.find(h);
    int64_t frame = (it != ctx->videos.end() && it->second.source) ? it->second.source->currentFrameNumber() : 0;
    lua_pushnumber(L, static_cast<lua_Number>(frame));
    return 1;
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

    // IMG_Load auto-detects format (PNG, JPG, BMP, ...) from file content,
    // which covers KimmyScript's real assets (almost certainly PNG). We
    // still hand back a valid handle with a null texture on failure rather
    // than erroring out, so the rest of the script keeps running;
    // spriteDraw silently skips a null texture.
    SDL_Surface* surf;
    {
        ScopedAudioFreeze freeze(ctx);
        // A .zdirk game's image may be only in its zip: decoded from memory.
        std::vector<unsigned char> zipped;
        if (dirk::gameArchive().readArchiveFile(path, zipped)) {
            surf = IMG_Load_IO(SDL_IOFromConstMem(zipped.data(), zipped.size()), true);
        } else {
            surf = IMG_Load(path);
        }
    }
    // The game's own Overlay/ folder (or this one file in it) is missing --
    // fall back to this game's own chosen skin's Overlay/ (KimmyScript/
    // Skin<SKINDIR>/Overlay/..., SKINDIR defaulting to "/DEFAULT") before
    // giving up. See skinAssetFallbackPath's own comment: safe to attempt
    // unconditionally, a no-op (empty candidate) for any path that isn't a
    // per-game ".../Overlay/..." reference to begin with.
    std::string fallbackPath;
    if (!surf) {
        fallbackPath = skinAssetFallbackPath(L, path, "Overlay");
        if (!fallbackPath.empty()) {
            ScopedAudioFreeze freeze(ctx);
            surf = IMG_Load(fallbackPath.c_str());
        }
    }
    if (surf) {
        res.width = surf->w;
        res.height = surf->h;
        if (ctx->renderer) {
            res.texture = SDL_CreateTextureFromSurface(ctx->renderer, surf);
        }
        SDL_DestroySurface(surf);
        if (!fallbackPath.empty()) {
            std::fprintf(stderr, "[dirk] spriteLoad(\"%s\") not found -- using \"%s\" instead\n",
                         path, fallbackPath.c_str());
        }
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
        if (!it->second.textCacheKey.empty()) {
            // Shared fontToSprite() texture (see EngineContext::
            // textSpriteCache): drop this handle's reference; the texture
            // itself only goes once no other live handle still uses it.
            auto cit = ctx->textSpriteCache.find(it->second.textCacheKey);
            if (cit != ctx->textSpriteCache.end() && --cit->second.refs <= 0) {
                if (cit->second.texture) SDL_DestroyTexture(cit->second.texture);
                ctx->textSpriteCache.erase(cit);
            }
        } else if (it->second.texture) {
            SDL_DestroyTexture(it->second.texture);
        }
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
    // Sprites draw with their own native pixel colors -- ctx->fgR/fgG/fgB/fgA
    // is colorForeground()/setFontColor()'s *text* color (see fontPrint/
    // fontToSprite below), not a per-draw tint for arbitrary artwork. This
    // used to apply that color as an SDL_SetTextureColorMod/AlphaMod on
    // every sprite, which sounds harmless until you notice setFontColor()
    // is called dozens of times a frame for menus/scores/etc. (grep across
    // every *.singe file: 401 spriteDraw call sites, almost none preceded
    // by a colorForeground/setFontColor meant for that sprite) -- so any
    // sprite drawn after the frame's last text color happened to be set
    // (very often setFontColor(mycolor2), yellow in every real game.cfg
    // seen so far) silently inherited that tint. That's the "couche jaune
    // sur tous les dessins" bug: every sprite multiplied by (255,255,0).
    SDL_Rect dst{x, y, it->second.width, it->second.height};
    SDL_FRect dstF; SDL_RectToFRect(&dst, &dstF);
    const bool faded = it->second.alpha < 255;
    if (faded) SDL_SetTextureAlphaMod(it->second.texture, static_cast<Uint8>(it->second.alpha));
    SDL_RenderTexture(ctx->renderer, it->second.texture, nullptr, &dstF);
    if (faded) SDL_SetTextureAlphaMod(it->second.texture, 255);
    return 0;
}

// dirkSpriteAlpha(handle, alpha) -- 2026-10-09 (Cyril): the opacity of one
// sprite for its next spriteDraw() calls, 0 (invisible) to 255 (opaque, the
// default). Added for the Wizard logo of Create.dirk to fade out; a script
// that never calls it draws exactly as before. Unknown handle: ignored.
int l_dirkSpriteAlpha(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    lua_Integer a = luaL_checkinteger(L, 2);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    auto it = ctx->sprites.find(h);
    if (it != ctx->sprites.end()) it->second.alpha = static_cast<int>(a);
    return 0;
}

// dirkKimmyLogo() -- draws the Kimmy branding logo top-right, at the exact
// position KimmyScript's own menu/options-screen print functions used to
// compute themselves: spriteDraw(OVLW-KIMW-LINEH, LINEH, sprite[KIMMY]),
// where OVLW/OVLH come from overlaySetResolution() (== ctx->overlayWidth/
// overlayHeight, already scaled for dip_Res's Full/Lores/Ultralo modes --
// see getRes() in main.kimmy) and LINEH = OVLH/80. Reproduced here with
// those same already-known values rather than any new constant, so the
// on-screen position is unchanged from before.
//
// Two things moved natively on the user's explicit request, after a
// "des petits malins l'ont déjà changé" tampering report: the image
// itself (kKimmyLogoPng, compiled into the binary -- see KimmyLogoData.h
// -- rather than read from a Kimmy.png file that a "clever" player could
// replace) and the draw position (computed here, not passed in from a Lua
// script that could pass different numbers). What's deliberately left in
// KimmyScript's own hands is *whether* this gets called at all, and from
// where -- same as every other native dirkXxx binding -- so the logo
// still only appears on the menu/options screens that choose to call it,
// same as the original spriteDraw() call sites did.
//
// Takes no arguments; the previous sprite/position calculation (sprite[KIMMY],
// spriteGetWidth(sprite[KIMMY]) into KIMW, OVLW-KIMW-LINEH, LINEH) is gone
// from KimmyScript entirely, along with the fillOVL(KIMMY,"Kimmy.png") load
// that fed it -- nothing else in either main.kimmy or service.kimmy still
// references the KIMMY sprite handle or KIMW after this change.
int l_dirkKimmyLogo(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);

    if (!ctx->kimmyLogoLoadAttempted) {
        ctx->kimmyLogoLoadAttempted = true;
        SDL_IOStream* rw = SDL_IOFromConstMem(kKimmyLogoPng, static_cast<int>(kKimmyLogoPngLen));
        SDL_Surface* surf = rw ? IMG_Load_IO(rw, true /* auto-closes rw */) : nullptr;
        if (surf) {
            ctx->kimmyLogoWidth = surf->w;
            ctx->kimmyLogoHeight = surf->h;
            if (ctx->renderer) {
                ctx->kimmyLogoTexture = SDL_CreateTextureFromSurface(ctx->renderer, surf);
            }
            SDL_DestroySurface(surf);
        }
        if (!ctx->kimmyLogoTexture) {
            // Same "degrade gracefully, never fail the game" philosophy as a
            // missing dirk_loading.png/bezel.png -- this really shouldn't
            // happen (the PNG bytes are compiled in), but a malformed
            // build or an unbound renderer at first-call time are still
            // possible, and losing a decorative logo is not worth a crash.
            std::fprintf(stderr, "[dirk] warning: dirkKimmyLogo() could not create its texture (%s) -- logo will not be drawn\n",
                         SDL_GetError());
        }
    }

    if (!ctx->kimmyLogoTexture || !ctx->renderer) {
        return 0;
    }

    // The draw SIZE below used to always be the PNG's raw pixel size, never
    // scaled with overlaySetResolution() the way the draw POSITION already
    // was -- correct only at whichever resolution it first got drawn at,
    // oversized at any smaller one (confirmed: "gros en Lo et très gros en
    // Ultralo"). A first fix captured whichever overlayHeight was active the
    // first time this function ran as the "this size is correct" reference
    // -- but that assumed the first call always happens at Full resolution
    // (dip_Res == 0), which isn't true for a user who leaves dip_Res saved
    // on Lo/UltraLo across restarts (confirmed report, 2026-09-23: some
    // users on older PCs always run in Lo and save it that way -- every
    // logo draw for the rest of that session then scaled up from that low,
    // wrongly-captured reference, oversized even after switching back to
    // Full/Hi).
    //
    // Fixed properly instead of re-guessing a new "first call" heuristic:
    // main.kimmy's own getRes() sets Full resolution (dip_Res == 0) as
    // exactly OVLH = vldpGetHeight() (grep-verified), with no division --
    // i.e. the disc's raw video height. vldpGetHeight() is itself just
    // ctx->disc->height() (see l_vldpGetHeight above), a value dirk already
    // knows natively, independent of whatever dip_Res/overlayHeight happens
    // to be active right now. So the reference is computed fresh from that
    // on every single call instead of cached from whichever resolution
    // happened to be active at some earlier "first" call -- no lazy
    // capture, no assumption about call order, correct starting in
    // Lo/UltraLo/Full, in any order, any session.
    //
    // 2026-09-26, Cyril: "the size is right in 1080, but on lower-resolution
    // videos it gets a bit big". The reference above was the disc's own
    // height, so at Full resolution the logo was always drawn at its raw
    // 220x226 pixels -- 21% of the screen height on a 1080p movie, but 47%
    // on a 480p one. The reference is now a fixed 1080 lines -- the size
    // Cyril validated -- so the logo keeps the same share of the screen
    // (226/1080) whatever the movie's resolution, and still shrinks with
    // Lo/UltraLo exactly as before (it scales with overlayHeight). On a
    // 1080p movie nothing changes at all (1080/1080 = 1).
    constexpr double kLogoReferenceHeight = 1080.0;
    const double logoScale = ctx->overlayHeight > 0
        ? static_cast<double>(ctx->overlayHeight) / kLogoReferenceHeight
        : 1.0;
    const int scaledW = static_cast<int>(ctx->kimmyLogoWidth * logoScale + 0.5);
    const int scaledH = static_cast<int>(ctx->kimmyLogoHeight * logoScale + 0.5);

    const double lineH = ctx->overlayHeight / 80.0; // matches KimmyScript's own LINEH = OVLH/80
    const int x = static_cast<int>(ctx->overlayWidth - scaledW - lineH);
    const int y = static_cast<int>(lineH);

    SDL_Rect dst{x, y, scaledW, scaledH};
    SDL_FRect dstF; SDL_RectToFRect(&dst, &dstF);
    SDL_RenderTexture(ctx->renderer, ctx->kimmyLogoTexture, nullptr, &dstF);
    return 0;
}

int l_fontLoad(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const char* path = luaL_checkstring(L, 1);
    int size = static_cast<int>(luaL_checkinteger(L, 2));
    Handle h = ctx->allocHandle();

    FontResource res{path, size, nullptr};

    // Review point 3 (see EngineContext::fontCache): the same path + size
    // opened earlier this run is reused as-is -- no disk access, no audio
    // freeze, no extra open file. A path+size that already FAILED this run
    // isn't retried either (that's what still sends it to the skin
    // fallback below, same as the first time). Returns whether `p` was
    // resolved from the cache; `out` then holds the font (maybe nullptr).
    auto cachedOpen = [&](const std::string& p, void*& out) -> bool {
        auto cit = ctx->fontCache.find(p + "|" + std::to_string(size));
        if (cit == ctx->fontCache.end()) return false;
        out = cit->second;
        return true;
    };
    auto openAndCache = [&](const std::string& p) -> void* {
        void* f = nullptr;
        if (cachedOpen(p, f)) return f;
        {
            ScopedAudioFreeze freeze(ctx);
            // A .zdirk game's font may be only in its zip: SDL_ttf keeps
            // reading it, so the bytes stay in memory (GameArchive keeps them).
            if (auto zipped = dirk::gameArchive().sharedArchiveFile(p)) {
                f = TTF_OpenFontIO(SDL_IOFromConstMem(zipped->data(), zipped->size()), true, size);
            } else {
                f = TTF_OpenFont(p.c_str(), size);
            }
        }
        ctx->fontCache[p + "|" + std::to_string(size)] = f;
        return f;
    };
    bool fromCache = false;
    {
        void* cached = nullptr;
        fromCache = cachedOpen(path, cached) && cached != nullptr;
    }
    res.font = openAndCache(path);
    // Optional per-game Fonts/ folder: every real game builds its font
    // paths as MYDIR .. "/Fonts/<name>.ttf" (grep-verified, e.g.
    // maddog.dirk's "/Fonts/gambler.ttf"). Unlike Overlay, the fallback
    // here is NOT tail-preserving -- see skinDefaultFontFallbackPath's own
    // comment (and the block comment above assetPathTail) for why: real
    // Skin/<NAME>/ folders have no Fonts/ subfolder and no per-game font
    // names, just a flat font.ttf at the skin's own root, so this
    // substitutes that outright rather than looking for a same-named file
    // that (confirmed directly from a real player log) is never there.
    std::string fallbackPath;
    if (!res.font) {
        // Resolved fresh each time (cheap -- no disk access): it depends on
        // the CURRENT skin, which can change mid-run. Only the opening of
        // whatever path it resolves to is cached.
        fallbackPath = skinDefaultFontFallbackPath(L, path);
        if (!fallbackPath.empty()) {
            void* cached = nullptr;
            fromCache = cachedOpen(fallbackPath, cached) && cached != nullptr;
            res.font = openAndCache(fallbackPath);
        }
    }
    if (!res.font) {
        std::fprintf(stderr, "[dirk] warning: fontLoad(\"%s\", %d) failed (%s) -- handle %d prints nothing\n",
                     path, size, SDL_GetError(), h);
    } else if (!fallbackPath.empty()) {
        std::fprintf(stderr, "[dirk] fontLoad(\"%s\", %d) not found -- using \"%s\" instead -- handle %d%s\n",
                     path, size, fallbackPath.c_str(), h, fromCache ? " (already open, reused)" : "");
    } else if (fromCache) {
        std::fprintf(stderr, "[dirk] fontLoad(\"%s\", %d) ok -- handle %d (already open, reused)\n", path, size, h);
    } else {
        // Fonts load rarely (a handful of calls per game, vs. dozens of
        // spriteLoad calls) so an unconditional success line here is cheap
        // and, per a real debugging session, worth having: a silently-empty
        // render can otherwise look identical whether fontLoad failed,
        // succeeded but nothing called fontSelect, or succeeded and the
        // game state just isn't drawing text yet.
        std::fprintf(stderr, "[dirk] fontLoad(\"%s\", %d) ok -- handle %d\n", path, size, h);
    }
    ctx->fonts[h] = res;
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

// Shared by fontPrint/fontToSprite: renders `text` with the currently
// active font (fontSelect) in the current foreground color, blended (alpha)
// quality -- see the FONT_QUALITY note on l_fontQuality for why we don't
// try to honor SOLID/SHADED separately. Returns nullptr (and logs once) if
// there's no active font, an empty string, or rendering fails; empty text
// is a legitimate no-op, not an error, so it doesn't log.
//
// "Logs once" was the intent all along but wasn't what the code did: both
// warnings below fired on EVERY call, and fontPrint runs many times per
// frame -- measured 2026-09-26 (review point 5): a game whose font failed
// to load wrote 3,000 identical lines (261 KB) to the log in 10 s of play,
// ~94 MB/hour, each one a separate unbuffered disk write. Now each warning
// is printed once per font handle (see EngineContext::
// fontWarnedNoFont/fontWarnedRenderFail), saying so.
SDL_Surface* renderActiveFontText(EngineContext* ctx, const char* text) {
    auto it = ctx->fonts.find(ctx->activeFontHandle);
    if (it == ctx->fonts.end() || !it->second.font) {
        if (ctx->fontWarnedNoFont.insert(ctx->activeFontHandle).second) {
            std::fprintf(stderr,
                         "[dirk] warning: fontPrint/fontToSprite with no valid active font (fontSelect handle %d) "
                         "-- text using this font draws nothing; not repeated for this handle\n",
                         ctx->activeFontHandle);
        }
        return nullptr;
    }
    if (text[0] == '\0') return nullptr;
    SDL_Color color{ctx->fgR, ctx->fgG, ctx->fgB, ctx->fgA};
    // SDL3_ttf added a `length` parameter (bytes, 0 = null-terminated) --
    // see TTF_RenderText_Blended's real signature in SDL_ttf.h.
    SDL_Surface* surf = TTF_RenderText_Blended(static_cast<TTF_Font*>(it->second.font), text, 0, color);
    if (!surf && ctx->fontWarnedRenderFail.insert(ctx->activeFontHandle).second) {
        std::fprintf(stderr,
                     "[dirk] warning: TTF_RenderText_Blended(\"%s\") failed (%s) "
                     "-- further render failures with font handle %d not logged\n",
                     text, SDL_GetError(), ctx->activeFontHandle);
    }
    return surf;
}

int l_fontToSprite(lua_State* L) {
    // Renders `text` (in the active font/color) to a new sprite handle.
    EngineContext* ctx = ctxFromUpvalue(L);
    const char* text = luaL_checkstring(L, 1);

    Handle h = ctx->allocHandle();
    SpriteResource res;

    // Shared-texture path (review point 3, see EngineContext::
    // textSpriteCache): same font + same color + same text -> reuse the
    // texture a still-loaded handle already has. Only when there IS an
    // active font and a renderer; everything else (no font, empty text,
    // no renderer) falls through to the original per-call path below,
    // unchanged, including its existing warnings.
    auto fit = ctx->fonts.find(ctx->activeFontHandle);
    const bool cacheable = ctx->renderer && text[0] != '\0' &&
                           fit != ctx->fonts.end() && fit->second.font;
    std::string key;
    if (cacheable) {
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "%p|%02x%02x%02x%02x|", fit->second.font,
                      ctx->fgR, ctx->fgG, ctx->fgB, ctx->fgA);
        key = std::string(prefix) + text;
        auto cit = ctx->textSpriteCache.find(key);
        if (cit != ctx->textSpriteCache.end()) {
            ++cit->second.refs;
            res.texture = cit->second.texture;
            res.width = cit->second.width;
            res.height = cit->second.height;
            res.textCacheKey = key;
            ctx->sprites[h] = res;
            lua_pushinteger(L, h);
            return 1;
        }
    }

    SDL_Surface* surf = renderActiveFontText(ctx, text);
    if (surf) {
        res.width = surf->w;
        res.height = surf->h;
        if (ctx->renderer) res.texture = SDL_CreateTextureFromSurface(ctx->renderer, surf);
        SDL_DestroySurface(surf);
    }
    if (cacheable && res.texture) {
        ctx->textSpriteCache[key] = TextSpriteEntry{res.texture, res.width, res.height, 1};
        res.textCacheKey = key;
    }
    ctx->sprites[h] = res;
    lua_pushinteger(L, h);
    return 1;
}


namespace {
// See EngineContext::fontPrintCache. 120 overlay frames = ~2 s: long enough
// that a screen shown every frame never misses, short enough that text that
// keeps changing (a timer, a name being typed) is freed quickly.
constexpr uint64_t kFontPrintKeepFrames = 120;
// Safety net only -- a normal screen uses a few MB (the 94-string
// high-score screen at 1080p: ~2 MB).
constexpr size_t kFontPrintCacheMaxBytes = 64u * 1024u * 1024u;

void dropFontPrintEntry(EngineContext* ctx, std::unordered_map<std::string, FontPrintEntry>::iterator it) {
    SDL_DestroyTexture(it->second.texture);
    ctx->fontPrintCacheBytes -= static_cast<size_t>(it->second.width) * it->second.height * 4;
    ctx->fontPrintCache.erase(it);
}
} // namespace

void advanceOverlayFrame(EngineContext* ctx) { // public entry point: endOverlayFrame(), end of file
    ++ctx->overlayFrame;
    if (ctx->fontPrintCache.empty() || ctx->overlayFrame % 30 != 0) return; // a scan every ~0.5 s is plenty
    for (auto it = ctx->fontPrintCache.begin(); it != ctx->fontPrintCache.end();) {
        auto next = std::next(it);
        if (ctx->overlayFrame - it->second.lastUsedFrame > kFontPrintKeepFrames) dropFontPrintEntry(ctx, it);
        it = next;
    }
}

int l_fontPrint(lua_State* L) {
    // fontPrint(x, y, text) -- the single most-called function in the API
    // (483 call sites), usually with the same strings frame after frame.
    // The rendered texture is kept and reused (see EngineContext::
    // fontPrintCache for the measurement and the eviction rules).
    EngineContext* ctx = ctxFromUpvalue(L);
    int x = static_cast<int>(luaL_checkinteger(L, 1));
    int y = static_cast<int>(luaL_checkinteger(L, 2));
    const char* text = luaL_checkstring(L, 3);

    if (!ctx->renderer) return 0;
    auto fit = ctx->fonts.find(ctx->activeFontHandle);
    const bool cacheable = text[0] != '\0' && fit != ctx->fonts.end() && fit->second.font;
    FontPrintEntry entry;
    std::string key;
    bool cached = false;
    if (cacheable) {
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "%p|%02x%02x%02x%02x|", fit->second.font,
                      ctx->fgR, ctx->fgG, ctx->fgB, ctx->fgA);
        key.reserve(std::strlen(prefix) + std::strlen(text));
        key.append(prefix).append(text);
        auto cit = ctx->fontPrintCache.find(key);
        if (cit != ctx->fontPrintCache.end()) {
            cit->second.lastUsedFrame = ctx->overlayFrame;
            entry = cit->second;
            cached = true;
        }
    }
    if (!cached) {
        SDL_Surface* surf = renderActiveFontText(ctx, text);
        if (!surf) return 0;
        entry.texture = SDL_CreateTextureFromSurface(ctx->renderer, surf);
        entry.width = surf->w;
        entry.height = surf->h;
        entry.lastUsedFrame = ctx->overlayFrame;
        SDL_DestroySurface(surf);
        if (!entry.texture) return 0;
    }
    SDL_Rect dst{x, y, entry.width, entry.height};
    SDL_FRect dstF; SDL_RectToFRect(&dst, &dstF);
    fitTextToTarget(ctx->renderer, dstF);
    SDL_RenderTexture(ctx->renderer, entry.texture, nullptr, &dstF);
    if (cached) return 0;
    const size_t bytes = static_cast<size_t>(entry.width) * entry.height * 4;
    if (!cacheable || bytes > kFontPrintCacheMaxBytes / 4) {
        SDL_DestroyTexture(entry.texture); // not cacheable, or one huge string: old behaviour
        return 0;
    }
    // Over the cap: drop least-recently-drawn entries (rare -- normal
    // screens stay far below it; see kFontPrintCacheMaxBytes).
    while (ctx->fontPrintCacheBytes + bytes > kFontPrintCacheMaxBytes && !ctx->fontPrintCache.empty()) {
        auto oldest = ctx->fontPrintCache.begin();
        for (auto it = ctx->fontPrintCache.begin(); it != ctx->fontPrintCache.end(); ++it) {
            if (it->second.lastUsedFrame < oldest->second.lastUsedFrame) oldest = it;
        }
        dropFontPrintEntry(ctx, oldest);
    }
    ctx->fontPrintCache.emplace(std::move(key), entry);
    ctx->fontPrintCacheBytes += bytes;
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

// The read-back half of overlaySetResolution() above -- some classic Singe
// games (confirmed: Mad Dog McCree's maddog.singe, right after its own
// overlaySetResolution(vldpGetWidth(), vldpGetHeight()) call) read the
// values straight back to compute their own UI scaling factors
// (UnX = overlayGetWidth()/1440, etc). Was missing entirely -- not stubbed,
// not aliased, genuinely absent from the registered function table -- so
// any script calling it hit "attempt to call global 'overlayGetWidth' (a
// nil value)" and aborted right there, before the rest of that script (in
// Mad Dog's case, everything past that point, including onInputPressed)
// ever got a chance to define anything.
int l_overlayGetWidth(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushinteger(L, ctx->overlayWidth);
    return 1;
}

int l_overlayGetHeight(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushinteger(L, ctx->overlayHeight);
    return 1;
}

// singeSetGameName(name) -- another genuinely-missing native binding
// (confirmed absent, same way overlayGetWidth/Height were: not stubbed, not
// aliased, just never registered). Confirmed call site: Mad Dog McCree's
// maddog.dirk, inside its own initVLDP() state machine, called exactly once
// per game launch, on the very first frame, immediately BEFORE the line that
// advances lvlState out of its initial "setup" value:
//
//   if lvlState == lvlSetup then
//       singeSetGameName("Maddog Mccree singe 2")
//       discSkipToFrame(41540)
//       lvlState = lvlRunning
//   ...
//
// Because that call sits before the lvlState assignment, a missing-global
// error there aborted the ENTIRE initVLDP() call every single time -- and
// since initVLDP() runs every frame from onOverlayUpdate, lvlState could
// never leave lvlSetup, permanently blocking the game's own state machine
// from progressing past its initial setup step. Native keyboard/mouse
// dispatch was never the problem (confirmed working via onInputPressed
// firing correctly in the log); the game's own per-frame logic just could
// never get anywhere once this line started throwing.
//
// Retitles the real window -- the original Singe engine used this to set
// the window/taskbar title to the game's own name instead of a generic
// one, and there's no reason not to do the same real thing here rather than
// a no-op stub: ctx->window is set once in main() right after
// SDL_CreateWindow(), so this is a direct, low-risk SDL_SetWindowTitle call.
int l_singeSetGameName(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const char* name = luaL_checkstring(L, 1);
    if (ctx->window) {
        SDL_SetWindowTitle(ctx->window, name);
    }
    return 0;
}

// singeWantsCrosshairs() -- another genuinely-missing native binding
// (confirmed absent, same way singeSetGameName was). Confirmed call site:
// Mad Dog McCree's maddog.dirk, at the very end of onOverlayUpdate, on
// EVERY frame:
//
//   if (bShowMouse and singeWantsCrosshairs()) then
//       spriteDraw(cursorx, cursory, sprCursor)
//       ...
//
// On real Singe hardware/software, some LaserDisc player backends can draw
// their own crosshair overlay natively, in which case a game asks the
// engine "are you already drawing one, so I shouldn't also draw my own
// sprite on top of it?" via this function. Dirk has no such native/hardware
// crosshair layer of its own -- the only crosshair that ever gets drawn is
// the game's own spriteDraw(cursorx, cursory, sprCursor) call, gated
// directly on this return value. So this always returns true: "no, I'm not
// drawing one myself, go ahead and draw yours" -- the only answer that
// lets the game's own aiming cursor ever appear at all.
int l_singeWantsCrosshairs(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
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

// MIX_LoadAudio (not predecoded), or the same from a .zdirk's zip when the
// file is only there -- the bytes then stay in memory (GameArchive keeps
// them), since the mixer keeps reading a non-predecoded sound.
MIX_Audio* loadAudioFileOrArchive(MIX_Mixer* mixer, const std::string& path) {
    if (auto zipped = dirk::gameArchive().sharedArchiveFile(path)) {
        return MIX_LoadAudio_IO(mixer, SDL_IOFromConstMem(zipped->data(), zipped->size()), false, true);
    }
    return MIX_LoadAudio(mixer, path.c_str(), false);
}

int l_soundLoad(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const char* path = luaL_checkstring(L, 1);
    Handle h = ctx->allocHandle();
    SoundResource res;

    // Custom/ override (2026-09-23, Cyril) -- see customOverridePath()'s
    // own comment for why this lives here rather than in KimmyScript like
    // Custom/ for Overlay/Fonts does. Tried FIRST, before the game's own
    // literal path: an override replaces the normal file, it doesn't just
    // fill in for a missing one, same as Custom/font.ttf/Custom/<sprite>.png
    // already do. usedCustom is only for the diagnostic log below --
    // res.audio itself is identical either way.
    bool usedCustom = false;
    std::string customPath = customOverridePath(path, "Sounds");
    if (!customPath.empty() && ctx->soundMixer) {
        ScopedAudioFreeze freeze(ctx);
        res.audio = loadAudioFileOrArchive(ctx->soundMixer, customPath);
        usedCustom = res.audio != nullptr;
    }

    if (!res.audio && ctx->soundMixer) {
        ScopedAudioFreeze freeze(ctx);
        res.audio = loadAudioFileOrArchive(ctx->soundMixer, path); /* predecode false -- streams/decodes on demand, matching Mix_LoadWAV's own lazy-enough behavior closely enough for short SFX */
    }
    // Same skin-then-shared-default fallback as spriteLoad's Overlay/
    // handling above, for a game with no Sounds/ folder (or missing this
    // one file) of its own -- see skinAssetFallbackPath/
    // defaultAssetFallbackPath's comments. No real Skin/<NAME>/ folder ships
    // a Sounds/ subfolder today (skins are purely visual/typographic), so
    // the skin attempt is expected to miss every time and fall through to
    // DefaultSounds/ -- harmless, same reasoning as l_fontLoad above.
    std::string fallbackPath;
    if (!res.audio && ctx->soundMixer) {
        fallbackPath = skinAssetFallbackPath(L, path, "Sounds");
        if (!fallbackPath.empty()) {
            ScopedAudioFreeze freeze(ctx);
            res.audio = MIX_LoadAudio(ctx->soundMixer, fallbackPath.c_str(), false);
        }
        if (!res.audio) {
            fallbackPath = defaultAssetFallbackPath(path, "Sounds");
            if (!fallbackPath.empty()) {
                ScopedAudioFreeze freeze(ctx);
                res.audio = MIX_LoadAudio(ctx->soundMixer, fallbackPath.c_str(), false);
            }
        }
    }
    if (usedCustom) {
        std::fprintf(stderr, "[dirk] soundLoad(\"%s\") overridden by \"%s\" (Custom/)\n",
                     path, customPath.c_str());
    } else if (res.audio && !fallbackPath.empty()) {
        std::fprintf(stderr, "[dirk] soundLoad(\"%s\") not found -- using \"%s\" instead\n",
                     path, fallbackPath.c_str());
    } else if (!res.audio) {
        std::fprintf(stderr, "[dirk] warning: soundLoad(\"%s\") failed (%s) -- handle %d is silent\n",
                     path, SDL_GetError(), h);
    }
    ctx->sounds[h] = res;
    lua_pushinteger(L, h);
    return 1;
}

int l_soundPlay(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    auto it = ctx->sounds.find(h);
    if (it != ctx->sounds.end() && it->second.audio && ctx->soundMixer) {
        // SDL3_mixer has no "fire and forget, pick any free channel" call
        // like the old Mix_PlayChannel(-1, chunk, loops) any more -- a
        // specific MIX_Track has to own the audio before it can play.
        // Created lazily on first play and reused for every later
        // soundPlay()/soundStop() on this same handle (mirrors the old
        // "lastChannel" one-active-playback-per-handle tracking).
        if (!it->second.track) {
            it->second.track = MIX_CreateTrack(ctx->soundMixer);
            if (it->second.track) MIX_SetTrackAudio(it->second.track, it->second.audio);
        }
        if (it->second.track) MIX_PlayTrack(it->second.track, 0);
    }
    // Used to return no value at all (`return 0`). Real Singe's soundPlay()
    // returns a handle meant to be stashed and handed back to soundStop()
    // later -- several games' scripts do exactly that (e.g. Mad Dog's
    // doTownSelect(): `sndHandle = soundPlay(sndSign)` ... later
    // `soundStop(sndHandle)`). With no return value, that assignment always
    // made sndHandle nil in Lua, so the later soundStop(sndHandle) call
    // hard-crashed onOverlayUpdate with "bad argument #1 to 'soundStop'
    // (number expected, got nil)" -- confirmed via a Mad Dog DIRK_DEBUG_TICK
    // log, at exactly the town-selection map screen's lvlEnd transition
    // (shoot a destination -> soundStop(sndHandle) -> crash -> that tick's
    // onOverlayUpdate aborts before reaching the cursor-draw call at the
    // bottom of the function, hence "cursor disappears and nothing
    // happens": the whole per-frame update silently stops running).
    //
    // Dirk only ever tracks one MIX_Track per handle (see the comment
    // above), so the same handle that was passed in to soundPlay() is
    // exactly what soundStop() needs to look this sound back up in
    // ctx->sounds -- returning it here is both minimal and correct, and
    // matches this apparently-universal "capture what soundPlay() gives
    // you, hand it back to soundStop()" idiom.
    lua_pushinteger(L, h);
    return 1;
}

int l_soundStop(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    Handle h = static_cast<Handle>(luaL_checkinteger(L, 1));
    auto it = ctx->sounds.find(h);
    if (it != ctx->sounds.end() && it->second.track) {
        MIX_StopTrack(it->second.track, 0); // 0 fade-out frames -- immediate stop, same as the old Mix_HaltChannel
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

// ---------------------------------------------------------------------
// Granular LaunchSettings accessors -- one get/set pair per field (see
// EngineContext::launchSettings and LaunchSettings.h). Let a game's own
// KimmyScript menu list these as ordinary items in its own UI (e.g. a 4th
// "UI Options" screen alongside Game/Graphic/Extra Options), toggled/
// adjusted directly the same way it already does with its own dip_*/
// ShowXxx fields. Every setter just writes the field and flags
// ctx->launchSettingsDirty; main.cpp's loop is what actually re-applies
// the live effect (fullscreen toggle, volume, scanline texture rebuild,
// console visibility) and persists to disk (the shared dirk_settings.cfg)
// on its next iteration -- these functions never touch SDL themselves.
int l_dirkGetFullscreen(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushboolean(L, ctx->launchSettings && ctx->launchSettings->fullscreen);
    return 1;
}

int l_dirkSetFullscreen(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    bool value = lua_toboolean(L, 1) != 0;
    if (ctx->launchSettings) {
        ctx->launchSettings->fullscreen = value;
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

int l_dirkGetVolumeVldp(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushinteger(L, ctx->launchSettings ? ctx->launchSettings->volumeVldp : 100);
    return 1;
}

int l_dirkSetVolumeVldp(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    int value = static_cast<int>(luaL_checkinteger(L, 1));
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    if (ctx->launchSettings) {
        ctx->launchSettings->volumeVldp = value;
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

int l_dirkGetVolumeNonVldp(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushinteger(L, ctx->launchSettings ? ctx->launchSettings->volumeNonVldp : 100);
    return 1;
}

int l_dirkSetVolumeNonVldp(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    int value = static_cast<int>(luaL_checkinteger(L, 1));
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    if (ctx->launchSettings) {
        ctx->launchSettings->volumeNonVldp = value;
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

int l_dirkGetScanlines(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushboolean(L, ctx->launchSettings && ctx->launchSettings->scanlines);
    return 1;
}

int l_dirkSetScanlines(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    bool value = lua_toboolean(L, 1) != 0;
    if (ctx->launchSettings) {
        ctx->launchSettings->scanlines = value;
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

// hideConsole only actually does anything on Windows (see main.cpp's
// applyConsoleVisibility(), #ifdef _WIN32) -- the getter/setter stay
// unconditional anyway so a menu built once in KimmyScript doesn't need
// its own platform check just to show/hide this one item; on other
// platforms it's simply a value that's stored and re-applied as a no-op.
int l_dirkGetHideConsole(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushboolean(L, ctx->launchSettings && ctx->launchSettings->hideConsole);
    return 1;
}

int l_dirkSetHideConsole(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    bool value = lua_toboolean(L, 1) != 0;
    if (ctx->launchSettings) {
        ctx->launchSettings->hideConsole = value;
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

// dirkGetBezelCount(): how many bezels loadBezels() found for THIS game
// (see EngineContext::bezelCount / Bezel.h) -- 0 for a game with none. Lets
// a KimmyScript menu grey out/show "N/A" for its bezel item for a game that
// has none, and know how many there are to cycle through (dirkSetBezelIndex
// below) and display ("i/N") for the ones that do -- same "N/A" pattern its
// other options screens already use for a dip_* item that doesn't apply to
// the current dip_GameType.
int l_dirkGetBezelCount(lua_State* L) {
    // 0 while there is no room for a bezel on this screen (BezelFit.h).
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushinteger(L, (!ctx->bezelRoom || *ctx->bezelRoom) ? ctx->bezelCount : 0);
    return 1;
}

// dirkOpenJoystickWizard() (2026-10-03): opens the joystick setup wizard,
// same as F9 -- for a "Controller Setup" item in a game's Dirk Options menu,
// since handhelds and cabinets have no F9 key. Without a keyboard the
// wizard is driven by the game's own controls (see main.cpp, wizardNav*).
int l_dirkOpenJoystickWizard(lua_State* L) {
    ctxFromUpvalue(L)->joystickWizardRequested = true;
    return 0;
}

// dirkGetBezelIndex(): which bezel is currently active -- 0 means none/off,
// N (1-based) means "bezel #N" of dirkGetBezelCount() total, in the same
// stable alphabetical order loadBezels() sorted them in.
int l_dirkGetBezelIndex(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushinteger(L, ctx->launchSettings ? ctx->launchSettings->bezelIndex : 0);
    return 1;
}

// dirkSetBezelIndex(n): clamped to [0, bezelCount] so a caller can't put
// this out of range by passing garbage -- a KimmyScript menu's own +/-
// wraparound logic (service.kimmy) is expected to stay in range on its own,
// this is just the same belt-and-suspenders every other dirkSetXxx() here
// applies to its own field.
int l_dirkSetBezelIndex(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_Integer value = lua_tointeger(L, 1);
    if (ctx->launchSettings) {
        if (value < 0) value = 0;
        if (value > ctx->bezelCount) value = ctx->bezelCount;
        ctx->launchSettings->bezelIndex = static_cast<int>(value);
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

// .srt subtitles (2026-09-27, core/Subtitles.h, LaunchSettings::
// subtitleIndex) -- same shape as the bezel trio above:
//   dirkGetSubtitleCount()   how many subtitle tracks this game's movie has (0 = none)
//   dirkGetSubtitleIndex()   0 = off, N = track #N. For a game with no track it
//                            returns the saved choice unchanged (so writeConfig()
//                            keeps it); a menu shows "N/A" from the count instead
//   dirkSetSubtitleIndex(n)  clamped to [0, count]; ignored when there is none,
//                            so a game without subtitles never overwrites a saved choice
//   dirkGetSubtitleName(n)   the track's label for a menu: "On" (untagged file)
//                            or its language name ("French", "English"...); "" if out of range
int l_dirkGetSubtitleCount(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(ctxFromUpvalue(L)->subtitleLabels.size()));
    return 1;
}

int l_dirkGetSubtitleIndex(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const int count = static_cast<int>(ctx->subtitleLabels.size());
    int value = ctx->launchSettings ? ctx->launchSettings->subtitleIndex : 0;
    if (value < 0) value = 0;
    if (count > 0 && value > count) value = count;
    lua_pushinteger(L, value);
    return 1;
}

int l_dirkSetSubtitleIndex(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const int count = static_cast<int>(ctx->subtitleLabels.size());
    lua_Integer value = lua_tointeger(L, 1);
    if (ctx->launchSettings && count > 0) {
        if (value < 0) value = 0;
        if (value > count) value = count;
        ctx->launchSettings->subtitleIndex = static_cast<int>(value);
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

int l_dirkGetSubtitleName(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const lua_Integer n = lua_tointeger(L, 1);
    if (n >= 1 && n <= static_cast<lua_Integer>(ctx->subtitleLabels.size())) {
        lua_pushstring(L, ctx->subtitleLabels[static_cast<size_t>(n - 1)].c_str());
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}

// Soundtracks (2026-09-27, VideoSource::audioTrackCount(), LaunchSettings::
// audioTrack) -- same shape as the subtitle functions:
//   dirkGetAudioTrackCount()   how many soundtracks the movie has (1 = no choice)
//   dirkGetAudioTrackIndex()   the one playing, 1-based
//   dirkSetAudioTrackIndex(n)  clamped to [1, count]; switches live (main loop)
//   dirkGetAudioTrackName(n)   menu label: "Original", "French", "Italian"...
int l_dirkGetAudioTrackCount(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushinteger(L, ctx->disc ? ctx->disc->audioTrackCount() : 0);
    return 1;
}

int l_dirkGetAudioTrackIndex(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    int value = 0;
    if (ctx->launchSettings && ctx->launchSettings->audioTrack >= 1) {
        value = ctx->launchSettings->audioTrack; // what the player chose (applied by the main loop)
    } else if (ctx->disc && ctx->disc->currentAudioTrack() >= 0) {
        value = ctx->disc->currentAudioTrack() + 1; // the file's default track
    }
    lua_pushinteger(L, value);
    return 1;
}

int l_dirkSetAudioTrackIndex(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const int count = ctx->disc ? ctx->disc->audioTrackCount() : 0;
    lua_Integer value = lua_tointeger(L, 1);
    if (ctx->launchSettings && count > 0) {
        if (value < 1) value = 1;
        if (value > count) value = count;
        ctx->launchSettings->audioTrack = static_cast<int>(value);
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

int l_dirkGetAudioTrackName(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const lua_Integer n = lua_tointeger(L, 1);
    lua_pushstring(L, ctx->disc ? ctx->disc->audioTrackLabel(static_cast<int>(n - 1)).c_str() : "");
    return 1;
}

// dirkGetSubtitleSize() / dirkSetSubtitleSize(percent): subtitle text size,
// 100 = normal, clamped to 50-150 (LaunchSettings::subtitleSize).
int l_dirkGetSubtitleSize(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushinteger(L, ctx->launchSettings ? ctx->launchSettings->subtitleSize : 100);
    return 1;
}

int l_dirkSetSubtitleSize(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_Integer value = lua_tointeger(L, 1);
    if (value < kSubtitleSizeMin) value = kSubtitleSizeMin;
    if (value > kSubtitleSizeMax) value = kSubtitleSizeMax;
    if (ctx->launchSettings) {
        ctx->launchSettings->subtitleSize = static_cast<int>(value);
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

// dirkGet/SetGenerateLog(): see LaunchSettings::generateLog for the full
// rationale (zero-configuration troubleshooting log for a stuck player,
// reachable from KimmyScript's own "UI Options" menu). Same shape as every
// other dirkGet/SetXxx() pair above -- the setter only flips the stored
// flag and marks launchSettingsDirty; main.cpp's launchSettingsDirty
// handling in the main loop is what actually starts the redirect (see
// activateGenerateLog() there), same indirection dirkSetHideConsole() and
// dirkSetBezelIndex() already use for their own not-purely-Lua side
// effects.
int l_dirkGetGenerateLog(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushboolean(L, ctx->launchSettings && ctx->launchSettings->generateLog);
    return 1;
}

int l_dirkSetGenerateLog(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    bool value = lua_toboolean(L, 1) != 0;
    if (ctx->launchSettings) {
        ctx->launchSettings->generateLog = value;
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

// dirkGet/SetSwapFaceButtons(): LaunchSettings::swapFaceButtons (A<->B,
// X<->Y on recognized gamepads), from KimmyScript's Dirk Options. The
// setter flips the field (the event loop reads it live) and marks
// launchSettingsDirty; main.cpp's dirty block then patches the
// swap_face_buttons line of the shared dirk_settings.cfg -- install-wide,
// never game.cfg (it belongs to the machine, not to a game).
int l_dirkGetSwapFaceButtons(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    lua_pushboolean(L, ctx->launchSettings && ctx->launchSettings->swapFaceButtons);
    return 1;
}

int l_dirkSetSwapFaceButtons(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    bool value = lua_toboolean(L, 1) != 0;
    if (ctx->launchSettings) {
        ctx->launchSettings->swapFaceButtons = value;
        ctx->launchSettingsDirty = true;
    }
    return 0;
}

// Wizard's AUTO CREATE LEVELS / DEATHS (2026-10-15): see core/BlackScan.h.
// One scan at a time, on the game's own movie.
//   dirkScanStart(a, b)  -> true when started
//   dirkScanStatus()     -> "idle" | "running" | "done" | "failed", progress 0..1, error text
//   dirkScanResult()     -> { {a, b}, ... } once done
//   dirkScanCancel()
dirk::BlackScan& wizardScan() {
    static dirk::BlackScan scan;
    return scan;
}

int l_dirkScanStart(lua_State* L) {
    EngineContext* ctx = ctxFromUpvalue(L);
    const lua_Integer a = luaL_checkinteger(L, 1);
    const lua_Integer b = luaL_checkinteger(L, 2);
    if (!ctx->disc || ctx->disc->path().empty()) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, wizardScan().start(ctx->disc->path(), a, b, ctx->disc->decodeThreads()) ? 1 : 0);
    return 1;
}

int l_dirkScanStatus(lua_State* L) {
    static const char* names[] = {"idle", "running", "done", "failed"};
    dirk::BlackScan& scan = wizardScan();
    lua_pushstring(L, names[static_cast<int>(scan.state())]);
    lua_pushnumber(L, scan.progress());
    lua_pushstring(L, scan.error().c_str());
    return 3;
}

int l_dirkScanResult(lua_State* L) {
    const std::vector<dirk::ScanPiece> pieces = wizardScan().pieces();
    lua_createtable(L, static_cast<int>(pieces.size()), 0);
    for (size_t i = 0; i < pieces.size(); ++i) {
        lua_createtable(L, 2, 0);
        lua_pushinteger(L, static_cast<lua_Integer>(pieces[i].a));
        lua_rawseti(L, -2, 1);
        lua_pushinteger(L, static_cast<lua_Integer>(pieces[i].b));
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int l_dirkScanCancel(lua_State*) {
    wizardScan().cancel();
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

// -- lfs.dir() / lfs.attributes() ------------------------------------------
// KimmyScript (globals.singe, main.singe's fillOVLlo(), hscore.singe,
// service.singe, menu.singe) calls a handful of LuaFileSystem functions to
// list Cfg/ and Overlay/Lores/ directories. Real LuaFileSystem happened to
// already be present as a system package on the Linux dev box this project
// started on, but it isn't packaged for MSYS2/mingw64 on Windows -- rather
// than depend on an external module that may or may not be installed, dirk
// implements the exact small subset KimmyScript actually calls (grep-
// verified: only `lfs.dir` and `lfs.attributes`, and only `.mode`/existence
// off the latter) natively, using std::filesystem so it works identically
// on every platform dirk builds for.

// lfs.dir(path) returns an iterator closure; Lua's generic `for x in
// lfs.dir(path) do` calls it repeatedly with no arguments it needs, since
// all the state (the entry list + a cursor) lives in the closure's own
// upvalues rather than the standard (s, var) generic-for slots.
int l_lfsDirNext(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(2)); // cursor box: a 1-element table
    lua_rawgeti(L, -1, 1);
    lua_Integer idx = lua_tointeger(L, -1) + 1;
    lua_pop(L, 1);
    lua_pushinteger(L, idx);
    lua_rawseti(L, -2, 1); // box[1] = idx
    lua_pop(L, 1);         // pop the box, entries table goes back on top next

    lua_pushvalue(L, lua_upvalueindex(1)); // entries table
    lua_rawgeti(L, -1, idx);               // entries[idx] -- nil past the end
    return 1;
}

int l_lfsDir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);

    lua_newtable(L); // entries
    int n = 0;
    lua_pushstring(L, ".");  lua_rawseti(L, -2, ++n);
    lua_pushstring(L, ".."); lua_rawseti(L, -2, ++n);

    // .zdirk game (see core/GameArchive.h): the folder on disk and the
    // zip's content, merged. Otherwise just the folder.
    std::vector<std::string> names;
    if (dirk::gameArchive().listDirectory(path, names)) {
        for (const auto& name : names) {
            lua_pushstring(L, name.c_str());
            lua_rawseti(L, -2, ++n);
        }
    } else {
        std::error_code ec;
        std::filesystem::directory_iterator it(path, ec);
        if (!ec) ec = std::make_error_code(std::errc::not_a_directory);
        // Real lfs.dir() throws a Lua error on a missing/unreadable
        // directory rather than returning an empty iterator -- some
        // KimmyScript call sites (fillOVLlo) rely on that to hard-fail
        // loudly instead of silently iterating zero files.
        return luaL_error(L, "lfs.dir(%s): %s", path, ec.message().c_str());
    }

    lua_newtable(L);       // cursor box
    lua_pushinteger(L, 0);
    lua_rawseti(L, -2, 1); // box[1] = 0

    lua_pushcclosure(L, l_lfsDirNext, 2); // captures entries + box
    return 1;
}

int l_lfsAttributes(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    // A .zdirk game: on disk, or in its zip.
    if (dirk::gameArchive().active()) {
        bool isDir = false;
        uint64_t size = 0;
        if (!dirk::gameArchive().exists(path, &isDir, &size)) {
            lua_pushnil(L);
            return 1;
        }
        std::error_code dec;
        if (!std::filesystem::exists(path, dec)) { // only in the zip
            lua_newtable(L);
            lua_pushstring(L, isDir ? "directory" : "file");
            lua_setfield(L, -2, "mode");
            if (!isDir) {
                lua_pushinteger(L, static_cast<lua_Integer>(size));
                lua_setfield(L, -2, "size");
            }
            return 1;
        }
    }
    std::error_code ec;
    auto st = std::filesystem::status(path, ec);
    if (ec || st.type() == std::filesystem::file_type::not_found) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    const char* mode = "other";
    switch (st.type()) {
        case std::filesystem::file_type::directory: mode = "directory"; break;
        case std::filesystem::file_type::regular:   mode = "file"; break;
        case std::filesystem::file_type::symlink:   mode = "link"; break;
        default: break;
    }
    lua_pushstring(L, mode);
    lua_setfield(L, -2, "mode");

    if (st.type() == std::filesystem::file_type::regular) {
        auto sz = std::filesystem::file_size(path, ec);
        if (!ec) {
            lua_pushinteger(L, static_cast<lua_Integer>(sz));
            lua_setfield(L, -2, "size");
        }
    }
    return 1;
}

void regLfs(lua_State* L) {
    lua_newtable(L); // the `lfs` table
    lua_pushcfunction(L, l_lfsDir);
    lua_setfield(L, -2, "dir");
    lua_pushcfunction(L, l_lfsAttributes);
    lua_setfield(L, -2, "attributes");

    // globals.singe does `require("lfs")`, not a bare `lfs.dir(...)` --
    // require() only consults package.loaded/package.preload, never plain
    // globals, so a plain `lua_setglobal(L, "lfs")` alone would still send
    // it hunting for a real lfs.so/lfs.dll and failing. Seed
    // package.loaded.lfs with this same table so require("lfs") returns it
    // immediately, exactly as if the module had "already been loaded".
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "loaded");
        if (lua_istable(L, -1)) {
            lua_pushvalue(L, -3); // the lfs table
            lua_setfield(L, -2, "lfs");
        }
        lua_pop(L, 1); // package.loaded
    }
    lua_pop(L, 1); // package

    lua_setglobal(L, "lfs"); // also expose it as a plain global, harmless
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
    regFn(L, ctx, "videoLoad", l_videoLoad);
    regFn(L, ctx, "videoUnload", l_videoUnload);
    regFn(L, ctx, "videoPlay", l_videoPlay);
    regFn(L, ctx, "videoSeek", l_videoSeek);
    regFn(L, ctx, "videoDraw", l_videoDraw);
    regFn(L, ctx, "videoGetFrame", l_videoGetFrame);
    regFn(L, ctx, "videoSetVolume", l_videoSetVolume);

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
    regFn(L, ctx, "overlayGetWidth", l_overlayGetWidth);
    regFn(L, ctx, "overlayGetHeight", l_overlayGetHeight);
    regFn(L, ctx, "singeSetGameName", l_singeSetGameName);
    regFn(L, ctx, "singeWantsCrosshairs", l_singeWantsCrosshairs);
    regFn(L, ctx, "vldpGetWidth", l_vldpGetWidth);
    regFn(L, ctx, "vldpGetHeight", l_vldpGetHeight);

    // Sound.
    regFn(L, ctx, "soundLoad", l_soundLoad);
    regFn(L, ctx, "soundPlay", l_soundPlay);
    regFn(L, ctx, "soundStop", l_soundStop);

    // System / scripting -- native dirkXxx names.
    regFn(L, ctx, "dirkGetScriptPath", l_dirkGetScriptPath);
    regFn(L, ctx, "dirkGetDataPath", l_dirkGetDataPath);
    regFn(L, ctx, "dirkKimmyLogo", l_dirkKimmyLogo);
    regFn(L, ctx, "dirkSpriteAlpha", l_dirkSpriteAlpha);
    regFn(L, ctx, "dirkQuit", l_dirkQuit);
    regFn(L, ctx, "dirkGetFullscreen", l_dirkGetFullscreen);
    regFn(L, ctx, "dirkSetFullscreen", l_dirkSetFullscreen);
    regFn(L, ctx, "dirkGetVolumeVldp", l_dirkGetVolumeVldp);
    regFn(L, ctx, "dirkSetVolumeVldp", l_dirkSetVolumeVldp);
    regFn(L, ctx, "dirkGetVolumeNonVldp", l_dirkGetVolumeNonVldp);
    regFn(L, ctx, "dirkSetVolumeNonVldp", l_dirkSetVolumeNonVldp);
    regFn(L, ctx, "dirkGetScanlines", l_dirkGetScanlines);
    regFn(L, ctx, "dirkSetScanlines", l_dirkSetScanlines);
    regFn(L, ctx, "dirkGetHideConsole", l_dirkGetHideConsole);
    regFn(L, ctx, "dirkSetHideConsole", l_dirkSetHideConsole);
    regFn(L, ctx, "dirkGetBezelCount", l_dirkGetBezelCount);
    regFn(L, ctx, "dirkGetBezelIndex", l_dirkGetBezelIndex);
    regFn(L, ctx, "dirkOpenJoystickWizard", l_dirkOpenJoystickWizard);
    regFn(L, ctx, "dirkSetBezelIndex", l_dirkSetBezelIndex);
    regFn(L, ctx, "dirkGetSubtitleCount", l_dirkGetSubtitleCount);
    regFn(L, ctx, "dirkGetSubtitleIndex", l_dirkGetSubtitleIndex);
    regFn(L, ctx, "dirkSetSubtitleIndex", l_dirkSetSubtitleIndex);
    regFn(L, ctx, "dirkGetSubtitleName", l_dirkGetSubtitleName);
    regFn(L, ctx, "dirkGetSubtitleSize", l_dirkGetSubtitleSize);
    regFn(L, ctx, "dirkGetAudioTrackCount", l_dirkGetAudioTrackCount);
    regFn(L, ctx, "dirkGetAudioTrackIndex", l_dirkGetAudioTrackIndex);
    regFn(L, ctx, "dirkSetAudioTrackIndex", l_dirkSetAudioTrackIndex);
    regFn(L, ctx, "dirkGetAudioTrackName", l_dirkGetAudioTrackName);
    regFn(L, ctx, "dirkSetSubtitleSize", l_dirkSetSubtitleSize);
    regFn(L, ctx, "dirkGetGenerateLog", l_dirkGetGenerateLog);
    regFn(L, ctx, "dirkSetGenerateLog", l_dirkSetGenerateLog);
    regFn(L, ctx, "dirkGetSwapFaceButtons", l_dirkGetSwapFaceButtons);
    regFn(L, ctx, "dirkSetSwapFaceButtons", l_dirkSetSwapFaceButtons);
    regFn(L, ctx, "dirkScanStart", l_dirkScanStart);
    regFn(L, ctx, "dirkScanStatus", l_dirkScanStatus);
    regFn(L, ctx, "dirkScanResult", l_dirkScanResult);
    regFn(L, ctx, "dirkScanCancel", l_dirkScanCancel);
    regFn(L, ctx, "scriptPush", l_scriptPush);
    regFn(L, ctx, "debugPrint", l_debugPrint);

    // Backward-compat aliases for the 3 Singe-named functions KimmyScript's
    // framework layer actually calls (framework.singe:30-31, menu.singe:
    // 360/702/745, service.singe:5842/5928) -- injected here rather than
    // editing those files, so zero existing KimmyScript source changes.
    aliasGlobal(L, "singeGetScriptPath", "dirkGetScriptPath");
    aliasGlobal(L, "singeGetDataPath", "dirkGetDataPath");
    aliasGlobal(L, "singeQuit", "dirkQuit");

    // lfs.dir() / lfs.attributes() -- see the comment above l_lfsDir.
    regLfs(L);
}

void endOverlayFrame(EngineContext* ctx) {
    advanceOverlayFrame(ctx);
}

// 2026-09-27, Cyril: some games' text runs off the screen (e.g. Space Ace's
// "58 completed moves out of 100 (0 Tilt: - 0%)" with its wide font).
// Safety net for every game and screen: a fontPrint() line WIDER than the
// screen -- so it would be cut off whatever its position -- is shrunk just
// enough to fit (one LINEH margin, OVLH/80, on each side, KimmyScript's own
// spacing unit), keeping its centre, then kept inside the screen. Same
// font, just smaller: no font switching. A line that already fits is never
// touched (not even moved), so nothing else changes by a single pixel.
// Wrapping onto two lines is deliberately left to the scripts: only they
// know what is drawn below.
void fitTextToTarget(SDL_Renderer* renderer, SDL_FRect& r) {
    int outW = 0, outH = 0;
    if (!SDL_GetCurrentRenderOutputSize(renderer, &outW, &outH) || outW <= 0 || r.w <= static_cast<float>(outW)) return;
    const float margin = static_cast<float>(outH) / 80.0f;
    const float maxW = std::max(1.0f, static_cast<float>(outW) - 2.0f * margin);
    const float scale = maxW / r.w;
    const float cx = r.x + r.w * 0.5f;
    const float cy = r.y + r.h * 0.5f;
    r.w *= scale;
    r.h *= scale;
    r.x = std::min(std::max(cx - r.w * 0.5f, margin), static_cast<float>(outW) - margin - r.w);
    r.y = cy - r.h * 0.5f;
}

} // namespace dirk
