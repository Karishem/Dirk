#pragma once
// EngineContext: the shared state the registered native (dirkXxx/singeXxx)
// Lua functions in src/lua/api_singe.cpp read and write, and that
// src/core/main.cpp drives the render/video loop from. Kept as a plain
// struct with no behavior of its own -- it's a hand-off point between the
// three subsystems (video, Lua API, SDL rendering), not a god-object.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "video/VideoSource.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_AudioStream; // movie audio, native SDL3 (see movieAudioStream below)
struct MIX_Mixer;       // short one-shot SFX (soundLoad/soundPlay), SDL3_mixer's
struct MIX_Audio;       // rewritten Mixer/Audio/Track API (replaces SDL2_mixer's
struct MIX_Track;       // Mix_Chunk/Mix_PlayChannel/Mix_HaltChannel entirely --
                         // no source-compatible shim exists, see SoundResource below)

namespace dirk { struct LaunchSettings; } // see core/LaunchSettings.h -- only a
                                           // pointer is needed here, so the
                                           // full header (which pulls in
                                           // SDL_Window*/SDL_Renderer* fwd
                                           // decls of its own) isn't included.
// TTF_Font is intentionally NOT forward-declared here. SDL2_ttf has shipped
// two incompatible opaque-struct spellings across versions --
// `typedef struct _TTF_Font TTF_Font;` (older) vs.
// `typedef struct TTF_Font TTF_Font;` (2.24+, e.g. the MSYS2/mingw64
// package) -- and guessing either tag name conflicts with the other when
// SDL_ttf.h is later included for real (see api_singe.cpp, which does
// include it). FontResource stores a void* instead and api_singe.cpp casts
// it back to TTF_Font* at the 2-3 call sites that need it, keeping this
// header fully decoupled from SDL_ttf's internal naming.

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
    // Non-empty only for a fontToSprite() sprite: its key in
    // EngineContext::textSpriteCache, whose texture this handle SHARES with
    // every other live fontToSprite() handle for the same font/color/text
    // (see that member's comment). spriteUnload() then releases one
    // reference instead of destroying the texture outright.
    std::string textCacheKey;
    // 2026-10-09 (Cyril: the Wizard logo of Create fading out): opacity
    // set by dirkSpriteAlpha(), 255 = opaque (every sprite, as before).
    // Applied only for the duration of this handle's own spriteDraw(), so a
    // texture shared by several fontToSprite() handles is never left tinted.
    int alpha = 255;
};

// One rendered fontToSprite() texture, shared by every live handle made
// for the same (font, color, text) -- see EngineContext::textSpriteCache.
struct TextSpriteEntry {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    int refs = 0; // live sprite handles pointing at `texture`
};

// One fontPrint() texture kept between frames -- see
// EngineContext::fontPrintCache.
struct FontPrintEntry {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    uint64_t lastUsedFrame = 0; // EngineContext::overlayFrame when last drawn
};

// SDL3_mixer 3.x has no Mix_Chunk/channel-number model at all (no source-
// compatible rename either -- confirmed against the real installed header,
// this is a genuine API redesign): a loaded sound is a MIX_Audio, and
// playing/stopping/tracking one specific live playback needs its own
// MIX_Track bound to that audio, played through MIX_PlayTrack/MIX_StopTrack.
// One track per handle, created lazily on first soundPlay() and reused for
// every later soundPlay()/soundStop() on that same handle -- mirrors the old
// "lastChannel" one-active-playback-per-handle tracking as closely as the
// new API allows.
struct SoundResource {
    MIX_Audio* audio = nullptr;
    MIX_Track* track = nullptr;
};

struct FontResource {
    std::string path;
    int size = 0;
    void* font = nullptr; // SDL2_ttf TTF_Font* handle, opened at fontLoad() time --
                           // stored as void* to avoid EngineContext.h depending on
                           // SDL_ttf.h's opaque-struct spelling (see note above).
};

// videoLoad/videoPlay/videoSeek/videoDraw/... (menu.singe's secondary video
// layer -- e.g. per-game attract-mode preview clips in a game-select menu).
// Distinct from the primary `disc` movie: each handle owns its own
// VideoSource decoding its own file, drawn scaled into an arbitrary
// rectangle on the overlay rather than filling the whole window. These are
// always muted in every real call site seen so far (videoSetVolume(h,0,0)),
// so unlike the primary disc, no audio decode is wired up for them.
struct VideoResource {
    std::unique_ptr<VideoSource> source;
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    bool playing = false;
    uint32_t lastTickMs = 0;
    double frameAccumulator = 0.0;
};

struct EngineContext {
    VideoSource* disc = nullptr;
    SDL_Renderer* renderer = nullptr;
    // The main window, set once in main() right after SDL_CreateWindow() --
    // needed here so singeSetGameName() (see api_singe.cpp) can retitle it.
    // Classic Singe games (e.g. Mad Dog McCree's maddog.dirk, initVLDP())
    // call this once, right before their own state machine leaves its
    // initial setup state; real title text ("Maddog Mccree singe 2") beats
    // the generic "Dirk" the window is created with.
    SDL_Window* window = nullptr;

    // The movie's own SDL3 audio stream (see main.cpp) -- SDL3 replaced the
    // SDL2 "open a device, SDL_QueueAudio straight to its SDL_AudioDeviceID"
    // model with an SDL_AudioStream bound to the device (SDL_PutAudioStreamData
    // instead of SDL_QueueAudio, SDL_PauseAudioStreamDevice/
    // SDL_ResumeAudioStreamDevice instead of SDL_PauseAudioDevice's old 2-arg
    // toggle -- see ScopedAudioFreeze below). Stored as a real typed pointer
    // (unlike the old plain-uint32_t device id) since SDL_AudioStream is just
    // forward-declared above -- no <SDL.h>-in-this-header problem the way
    // TTF_Font's opaque-struct-name clash was. nullptr means "no movie audio
    // stream" (hasAudio() was false, or SDL_OpenAudioDeviceStream failed) --
    // always check before use.
    //
    // Why api_singe.cpp needs this at all: fontLoad/spriteLoad/soundLoad
    // (TTF_OpenFont/IMG_Load/MIX_LoadAudio) are synchronous disk I/O, called
    // straight from a KimmyScript script mid-game, not just at startup --
    // whatever they cost blocks this whole thread, so video decode and
    // presentation are already stalled for exactly as long as they take.
    // The movie's audio ISN'T naturally stalled by that: it drains out of a
    // real hardware device on SDL's own thread at a fixed real-time rate,
    // with no idea the main loop has stopped. That mismatch was confirmed
    // for real on a live capture: four fontLoad() calls back-to-back, at
    // one particular scene transition, measured as a ~900ms jump in the
    // engine's own avOffsetMs diagnostic logged at exactly that moment --
    // a permanent, unrecovered chunk of AV sync lost to nothing more than
    // loading a font mid-game. See ScopedAudioFreeze in api_singe.cpp,
    // which pauses this stream for the duration of a load so it stays
    // frozen in lockstep with the video that's already stuck behind it,
    // instead of quietly drifting a little further out of sync every time
    // a game loads a resource after startup.
    SDL_AudioStream* movieAudioStream = nullptr;

    // Separate SDL3_mixer mixer for short one-shot soundLoad()/soundPlay()
    // SFX -- kept deliberately apart from movieAudioStream above, same
    // reasoning as the SDL2 build (see main.cpp's init): different tuning,
    // different API shape, no callback. nullptr if MIX_Init()/
    // MIX_CreateMixerDevice() failed (soundLoad/soundPlay/soundStop then
    // silently no-op, same graceful-degradation policy as everything else
    // in this file).
    MIX_Mixer* soundMixer = nullptr;

    std::string scriptPath; // directory containing the running script
    std::string dataPath;   // directory for data assets

    bool discPaused = false;
    // Bumped every time the SCRIPT changes the pause state (discPause/
    // discPlay/discStop, and discSkipToFrame's auto-resume -- see
    // api_singe.cpp). Native code that freezes the disc on its own (the
    // Pause hotkey, the F9 wizard) records this when it freezes and only
    // lifts its freeze if it hasn't moved since: if the script has touched
    // the pause state in between, the script's state stands. (2026-09-26,
    // review point 4: the script keeps running during a native pause, and
    // a seek it made there could otherwise leave the native side holding a
    // stale "I paused it" flag -- the next Pause press then un-froze a menu
    // the script had frozen on purpose, playing the movie behind it.)
    uint64_t discPauseScriptWrites = 0;
    double discFps = 29.97; // framework.singe's default (discSetFPS(29.97))

    bool quitRequested = false;

    // Live pointer into main()'s own LaunchSettings instance -- lets the
    // dirkGet/SetFullscreen / dirkGet/SetVolumeVldp / dirkGet/SetVolumeNonVldp
    // / dirkGet/SetScanlines / dirkGet/SetHideConsole / dirkGet/SetBezelIndex
    // Lua functions below (see api_singe.cpp), and the native F11 (bezel)
    // and F10 (fullscreen) hotkeys in main.cpp, read and change these fields
    // one at a time, so a game's own KimmyScript menu can list them as
    // ordinary toggleable/adjustable items -- e.g. a 4th "UI Options" screen
    // alongside its existing Game/Graphic/Extra Options ones -- the same way
    // it already does with its own dip_* fields. Set once in main() right
    // after `settings` is loaded; never null in practice, but every
    // accessor still null-checks it defensively rather than assuming.
    LaunchSettings* launchSettings = nullptr;
    // Set by any of those setters -- tells the main loop (which owns the
    // window/renderer/settingsPath these need) to re-apply the live effect
    // and persist to disk (the shared dirk_settings.cfg -- see
    // LaunchSettings.h) on its next iteration, one field at a time.
    bool launchSettingsDirty = false;

    // How many bezels loadBezels() found for the running game (see
    // Bezel.h) -- set once in main() right after that call, 0 if none.
    // Lets dirkGetBezelCount() tell a KimmyScript "UI Options" menu how
    // many there are to cycle through -- "N/A" when 0, exactly like its
    // Game/Graphic/Extra Options screens already show "N/A" for a dip_*
    // item that doesn't apply to the current dip_GameType -- and how to
    // display the active one (LaunchSettings::bezelIndex, above) as
    // "i/N" (or "Aucun" for index 0, none selected).
    int bezelCount = 0;
    // False while the screen has no room for a bezel beside the picture
    // (core/BezelFit.h): dirkGetBezelCount() then answers 0 -- the menu
    // shows N/A and can't change it -- while dirkGet/SetBezelIndex() keep
    // working on the saved choice, so writeConfig() still saves it.
    const bool* bezelRoom = nullptr; // nullptr = always room
    // Set by dirkOpenJoystickWizard() (a game's Dirk Options menu): the
    // main loop opens the joystick wizard, same as F9, on its next pass.
    bool joystickWizardRequested = false;
    // .srt subtitle tracks found next to this game's movie (core/Subtitles.h),
    // their menu labels ("On", "French", "English"...), in the order
    // LaunchSettings::subtitleIndex counts them (1-based there).
    std::vector<std::string> subtitleLabels;

    // colorForeground(r,g,b[,o]) / colorBackground(...) state -- confirmed
    // signature per the Singe manual (feasibility study section 2).
    uint8_t fgR = 255, fgG = 255, fgB = 255, fgA = 255;
    uint8_t bgR = 0, bgG = 0, bgB = 0, bgA = 255;

    int overlayWidth = 0;
    int overlayHeight = 0;

    // Kimmy branding logo, drawn by the native dirkKimmyLogo() Lua binding
    // (see l_dirkKimmyLogo in api_singe.cpp) instead of a KimmyScript
    // spriteLoad()/spriteDraw() pair -- unlike every sprite in `sprites`
    // above, its pixel data is compiled into the binary (src/lua/
    // KimmyLogoData.h), not read from a game folder file, so it can't be
    // swapped out the way a loose asset can. Texture is created lazily, on
    // the first dirkKimmyLogo() call (that's the first point a renderer is
    // guaranteed bound), and cached here for every call after.
    SDL_Texture* kimmyLogoTexture = nullptr;
    int kimmyLogoWidth = 0;  // native pixel size of the embedded PNG -- the SIZE it's drawn at scales off this (see kimmyLogoRefOverlayHeight), only the on-screen texture itself
    int kimmyLogoHeight = 0;
    bool kimmyLogoLoadAttempted = false; // true after the first attempt, success or not -- never retry a decode failure every frame
    // Note: an earlier version of this comment described a
    // kimmyLogoRefOverlayHeight field that cached whichever overlayHeight
    // was active the first time dirkKimmyLogo() ran, as the "correct size"
    // reference to scale every later draw against. Removed, 2026-09-23: it
    // wrongly assumed that first call always happens at Full resolution
    // (dip_Res == 0) -- broke for a user whose saved settings start a
    // session already on Lo/UltraLo (confirmed report: logo stays oversized
    // even after switching back to Full). dirkKimmyLogo() (api_singe.cpp)
    // now computes the Full-resolution reference fresh on every call, from
    // ctx->disc->height() -- the same value main.kimmy's own getRes()
    // assigns to OVLH for dip_Res == 0 -- so no state needs to be cached or
    // guessed here at all.

    std::unordered_map<Handle, SpriteResource> sprites;
    std::unordered_map<Handle, SoundResource> sounds;
    std::unordered_map<Handle, FontResource> fonts;

    // 2026-09-26 (review point 3, measured): two engine-side caches that
    // bound memory for script patterns that were leaking without limit.
    //
    // textSpriteCache -- fontToSprite() used to render + upload a brand-new
    // texture on every call, freed only by an explicit spriteUnload(). Real
    // scripts call it every frame without ever unloading: Mad Dog's
    // getMiddle() (toolbox.singe: spriteGetWidth(fontToSprite(p)), used for
    // "LOWER YOU GUN!!!" while it blinks) and drawName() (maddog.dirk, one
    // fontToSprite per letter per frame on the high-score entry screen).
    // Measured with the real bindings: ~1 MB of texture per call at 144px,
    // 150 MB after 5 s of that message, +126 MB for 10 s of name entry.
    // Now every call still gets its OWN handle (handle identity and
    // spriteUnload semantics unchanged -- a double unload is still a no-op,
    // one owner unloading never affects another), but handles for the same
    // text share one texture, ref-counted, destroyed when the last handle
    // is unloaded. Memory is bounded by the number of DISTINCT strings, and
    // a repeat call skips the TTF render + upload entirely. Safe to share:
    // nothing ever modifies a sprite's texture after creation (spriteDraw
    // draws with native colors, no per-sprite color/alpha mod -- see
    // l_spriteDraw). Key: font pointer + RGBA + text (see api_singe.cpp).
    std::unordered_map<std::string, TextSpriteEntry> textSpriteCache;
    //
    // fontCache -- fontLoad() used to TTF_OpenFont on every call and never
    // close anything (there is no fontUnload in the Singe API), so
    // KimmyScript's getFont()/getRes() -- 4 fontLoad each, called from 15
    // places (every game start, every font size +/-) -- piled up open
    // fonts and open FILE HANDLES (measured: 15 calls -> 60 extra fonts,
    // 60 extra open files), each load also doing disk I/O under an audio
    // freeze. Now keyed by "<actual path opened>|<size>": a repeat returns
    // the already-open TTF_Font (still a fresh handle). Sharing is safe for
    // the same reason caching is possible at all: fonts are never closed
    // during a run. A nullptr value = that exact path+size already failed
    // to open this run (not retried -- see l_fontLoad).
    std::unordered_map<std::string, void*> fontCache;

    // fontPrintCache (2026-09-26, measured): fontPrint() used to render the
    // text with SDL_ttf, upload it as a brand-new texture, draw it and
    // destroy it -- on every call, every frame, even though a screen's text
    // is almost always the same from one frame to the next. KimmyScript's
    // high-score screen (drawHSAll: ~94 fontPrint per frame, 1080p) spent
    // ~2.5 ms of every frame just re-rendering and re-uploading the same 54
    // strings. Now the texture is kept, keyed exactly like textSpriteCache
    // (font pointer + RGBA + text -- fonts are never closed during a run,
    // see fontCache), and dropped once it hasn't been drawn for
    // kFontPrintKeepFrames overlay frames (endOverlayFrame() in
    // api_singe.cpp, called by main.cpp after every onOverlayUpdate), so
    // text that changes (timers, scores being entered) doesn't pile up.
    // Also capped in bytes as a safety net. The picture is identical: same
    // texture, drawn the same way.
    std::unordered_map<std::string, FontPrintEntry> fontPrintCache;
    size_t fontPrintCacheBytes = 0;
    uint64_t overlayFrame = 0;

    // Font handles whose "no valid active font" / "render failed" warning
    // has already been printed this run -- see renderActiveFontText() in
    // api_singe.cpp (review point 5): those used to repeat on every
    // fontPrint call, i.e. many times per frame.
    std::unordered_set<Handle> fontWarnedNoFont;
    std::unordered_set<Handle> fontWarnedRenderFail;
    std::unordered_map<Handle, VideoResource> videos;
    Handle nextHandle = 1;
    Handle activeFontHandle = 0;

    Handle allocHandle() { return nextHandle++; }
};

} // namespace dirk
