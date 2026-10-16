#pragma once
// LaunchSettings: the small set of "how do you want this to look/sound"
// choices dirk remembers between launches -- display (windowed/fullscreen),
// the two volumes real Singe exposes as --volume_vldp (the movie's own audio
// track) and --volume_nonvldp (sound effects), the CRT scanline overlay, and
// the per-game bezel on/off. There is no standalone native settings screen
// for these any more (there used to be one, opened by F10 -- removed once
// every field here became reachable from a game's own in-script "UI
// Options" menu instead, see api_singe.cpp's dirkGet/Set* functions and
// KimmyScript's service.kimmy): dirk now boots straight into the game every
// time, using the defaults below the very first time a given game is run,
// and whatever that game's own KimmyScript menu last saved (per-game, in
// its own Cfg/game.cfg -- see readConfig()/writeConfig() in service.kimmy)
// after that. This file only stores the *factory-default* fallback -- read
// once at startup and never written again by live play (2026-09-22, Cyril):
// see EngineContext::launchSettings for how a running game's menu edits
// these fields one at a time, and main.cpp for the three native hotkeys
// (F10 fullscreen, F11 bezel, F12 scanlines) that also go through this same
// struct -- every one of those live changes is reported straight onto the
// CURRENT game's own game.cfg instead (see main.cpp's
// persistPerGameDirkSettings()), never back into this shared file, so one
// game's session can no longer leak into becoming another, not-yet-
// configured game's starting point.
//
// Deliberately NOT a general command-line-replacement: dirk still accepts
// (and, later, should keep accepting/extending) real command-line flags for
// scripted/kiosk launches from an external frontend (MAME-style cabinet
// front-ends, Attract-Mode, etc.) -- these are just the defaults for the
// "launched by hand, no frontend" case.

#include <string>

namespace dirk {

// Range of LaunchSettings::subtitleSize (percent).
constexpr int kSubtitleSizeMin = 50;
constexpr int kSubtitleSizeMax = 150;

struct LaunchSettings {
    // Defaults below are the out-of-the-box experience for a game that has
    // never been launched before (no per-game game.cfg entry yet either) --
    // chosen deliberately, not inherited from older placeholder values:
    // fullscreen on, movie audio at 100, sound effects at 60 (quieter than
    // the movie track by default), no scanlines, no bezel until the player
    // turns one on for a game that has one.
    bool fullscreen = true;
    int volumeVldp = 100;    // movie's own audio track (dialogue/music), 0-100
    int volumeNonVldp = 60;  // sound effects (soundLoad/soundPlay), 0-100
    // CRT-style scanline overlay -- plain on/off (no intensity slider, by
    // design: see main.cpp for the fixed default intensity). Something real
    // Singe never offered; purely a display-engine feature, see main.cpp's
    // scanline texture setup.
    bool scanlines = false;
    // Windows only (no-op elsewhere): when true, dirk detaches from/never
    // attaches to whatever console it was launched from (see main.cpp's
    // applyConsoleVisibility()) instead of printing its stderr/stdout
    // diagnostics there. Off by default so the existing "launch from
    // PowerShell for debugging" workflow keeps working until explicitly
    // turned off. Only reachable from KimmyScript (dirkGet/SetHideConsole),
    // there's no dedicated UI for it -- see api_singe.cpp.
    bool hideConsole = false;
    // Which per-game bezel (see Bezel.h) to composite, if any: 0 means
    // none/off, N (1-based) means "bezel #N" of however many loadBezels()
    // found for this game, in its stable alphabetical order. 0 by default
    // -- unlike the other fields above, a fresh install shouldn't assume
    // every game wants a bezel shown; the player cycles to one from the UI
    // Options menu (or F11) once they've seen it. Harmless/no-op for a game
    // with none: main.cpp only ever draws a bezel when this is in range for
    // however many were actually loaded. Exposed to KimmyScript via
    // dirkGetBezelCount()/dirkGetBezelIndex()/dirkSetBezelIndex() (see
    // api_singe.cpp) so a game's own menu can offer this as a per-game "UI
    // Options" item (cycling 0..N and back, same "#1"-style pattern as the
    // skin/font selectors), and natively via the F11 hotkey (see main.cpp)
    // for the same cycle without going through any menu at all. Was a
    // plain on/off bool before 2026-09-23 (bezelEnabled) -- generalized to
    // an index once a game could have more than one bezel to choose from;
    // an old on/off value (0 or 1) still reads back correctly as
    // none/bezel-#1, see main.cpp's persistPerGameDirkSettings().
    int bezelIndex = 0;
    // 2026-09-27: .srt subtitles for the movie (core/Subtitles.h). Same
    // cycle as bezelIndex: 0 = off, N = subtitle track #N of however many
    // .srt files sit next to this game's movie (untagged file first, then
    // by language tag). 1 by default: a game that ships subtitles shows
    // them until the player turns them off; a game with none is unaffected.
    // Per game (game.cfg "dirk_SubtitleIndex"), from the UI Options menu via
    // dirkGetSubtitleCount/Index/Name() and dirkSetSubtitleIndex().
    int subtitleIndex = 1;
    // Subtitle text size, in percent of Dirk's normal size (about 5% of the
    // picture height). 50-150, steps of 10 in the menu. Per game
    // (game.cfg "dirk_SubtitleSize", "Subtitle Size" in Dirk Options);
    // dirk_settings.cfg "subtitle_size" sets the default for games that
    // never saved one. 2026-09-27, Cyril found 100% too big.
    int subtitleSize = 100;
    // Movie soundtrack (2026-09-27): 0 = the movie file's own default
    // track, N = audio track N (1-based) when the movie has several
    // languages (VideoSource::audioTrackCount(): an .mp4/.mkv with several
    // audio streams, or a Hypseus game's <name>-fr.ogg alternates). Per game
    // (game.cfg "dirk_AudioTrack", "Audio" in Dirk Options).
    int audioTrack = 0;
    // Label of that track ("FR"), saved next to the number
    // (game.cfg "dirk_AudioTrackName") so the choice survives a soundtrack
    // being added or removed later, which renumbers the tracks. Wins over
    // audioTrack when a track with that label still exists.
    std::string audioTrackName;
    // Zero-configuration troubleshooting log: when true, dirk redirects its
    // own stdout/stderr to "<game folder>/dirk_log.txt" for the rest of the
    // run (see main.cpp's redirectStdioToFile()/activateGenerateLog()) and
    // forces DIRK_VERBOSE/DIRK_DEBUG_INPUT on in-process for that same run,
    // so the file actually has something useful in it without the player
    // ever touching an environment variable. Off by default -- this is
    // meant to be turned on ON DEMAND, by talking a stuck player through
    // "Options -> Generate Log -> On" and having them reproduce whatever
    // they were hitting, then send the resulting dirk_log.txt. Reachable
    // from KimmyScript via dirkGet/SetGenerateLog() (see api_singe.cpp) so a
    // game's own "UI Options" menu can offer it right alongside Scanlines/
    // Bezel; turning it off only changes what the *next* launch defaults
    // to -- once a log is already being written for the current run,
    // dirkSetGenerateLog(false) does not try to un-redirect stdout/stderr
    // mid-run (see activateGenerateLog()'s own comment for why). This is
    // deliberately independent of DIRK_LOG_FILE (see main.cpp): that env
    // var stays the power-user/scripted-launch mechanism, unaffected by
    // this field either way, and whichever one activates first wins if
    // both are in play.
    bool generateLog = false;
    // 2026-09-26 (review point 2): shows a short banner at the top of the
    // screen for a few seconds the first time each distinct Lua script
    // error happens ("SCRIPT ERROR - details in dirk_error.txt" + where).
    // On by default -- it's what would have made the DL2 s4p.cfg bug
    // obvious immediately instead of a silent "inputs ignored". Install-
    // wide only (dirk_settings.cfg "error_banner=0" turns it off, e.g. for
    // a cabinet open to the public): not a per-game game.cfg field and not
    // in the in-game UI Options menu. Independent of dirk_error.txt itself,
    // which is always written either way (see ScriptErrorLog.h).
    bool errorBanner = true;
    // 2026-10-13 (Cyril, R36S under ArkOS): SDL3 names a gamepad's face
    // buttons by POSITION (A = bottom, B = right, X = left, Y = top, the
    // Xbox layout), so on a pad printed Nintendo-style (A right, B bottom,
    // X top, Y left) the letters players see don't match JOY_A..JOY_Y.
    // "swap_face_buttons=1" in dirk_settings.cfg swaps A<->B and X<->Y on
    // every recognized gamepad, right where SDL hands the button over --
    // before controls.cfg, the F9 profiles, Wizard's forced controls and
    // the F9 wizard itself -- so every game and Wizard follow the printed
    // letters. Install-wide only (it's a property of the machine): not a
    // per-game game.cfg field. Since 2026-10-13 KimmyScript's Dirk Options
    // has a "Swap A/B X/Y" line (dirkGet/SetSwapFaceButtons): it applies
    // at once and patches this one line of dirk_settings.cfg (see
    // setLaunchSettingLine), so every game follows. Raw (not recognized)
    // joysticks are untouched. Off by default; the ArkOS package ships it on.
    bool swapFaceButtons = false;
    // 2026-09-26 (review point 1, step 2): video decoder threads.
    // "auto" (stored as 0 here, the DEFAULT since Cyril's cabinet test the
    // same day: sync at cuts "perfect", seeks and playback decode faster)
    // = as many as the PC has cores, capped at 4 -- a 1-core PC simply
    // gets 1. "decode_threads=N" in dirk_settings.cfg = exactly N (capped
    // at 8); decode_threads=1 = the single-threaded decoder. Install-wide
    // only. See VideoSource::setDecodeThreads/seekKeptAudio_.
    int decodeThreads = 0;
};

// Missing/unreadable/corrupt file -> the defaults above. Never fails loudly:
// this is a convenience, not something worth blocking a launch over.
LaunchSettings loadLaunchSettings(const std::string& path);

// Best-effort; a failed save is silently ignored (same reasoning).
void saveLaunchSettings(const std::string& path, const LaunchSettings& settings);

// Rewrites only the "key=..." line of the file at `path` (appended when
// absent, the file created when missing), every other line kept as it is
// -- for an install-wide setting changed from a game's menu
// (swap_face_buttons), without rewriting the player's own file. Best-effort.
void setLaunchSettingLine(const std::string& path, const std::string& key, const std::string& value);

// Moves BOTH volumes by deltaPercent (positive to raise, negative to
// lower), each clamped independently to 0..100 against its OWN current
// value -- the two are never forced to match each other, since they
// aren't necessarily set the same to start with (main.cpp's +/- hotkeys:
// Cyril, 2026-09-23, "make + and - raise/lower both volumes by 10%,
// clamped to 0..100, even though the two aren't necessarily set the
// same"). A small free function specifically so the exact clamping
// behaviour that decides both hotkeys can be unit-tested on its own
// (see tools/volume_nudge_test.cpp) rather than only exercised live,
// in-engine.
void nudgeVolumes(LaunchSettings& settings, int deltaPercent);

} // namespace dirk
