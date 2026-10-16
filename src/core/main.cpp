// Dirk engine entry point -- Phase 1 milestone per the feasibility study
// (section 8): "Une fenetre, un fichier video qui se lit, un
// discSkipToFrame qui fonctionne, un sprite et du texte affiches par-dessus,
// une touche qui repond via onInputPressed."
//
// Usage: dirk <game-folder>            (normal use -- auto-discovers the
//                                        video + script inside it, see
//                                        resolveGameFolder() below)
//        dirk <video-file> [script.lua] (manual override / raw-video check)
//
// Both stale caveats from the original Phase 1 milestone have since been
// addressed: there's a real audio-clock-driven video pacing scheme now (see
// kAudioOutputHardwareLatencySeconds and audioClockSeconds below), and
// keyboard AND SDL_Gamepad input are both translated through
// controls.cfg into the logical SWITCH_* ids onInputPressed actually
// expects -- see src/lua/InputMap.h and this file's GamepadSlot handling.

#include "video/VideoSource.h"
#include "video/HypseusRemux.h"
#include "video/VideoTexture.h"
#include "audio/AudioLatency.h"
#include "lua/LuaEngine.h"
#include "lua/api_singe.h"
#include "lua/InputMap.h"
#include "core/EngineContext.h"
#include "core/LaunchSettings.h"
#include "core/Bezel.h"
#include "core/BezelFit.h"
#include "core/JoystickProfile.h"
#include "core/JoystickWizard.h"
#include "core/ScriptErrorLog.h"
#include "core/KimmyFingerprint.h"
#include "core/SubtitleRenderer.h"
#include "core/Subtitles.h"
#include "core/Languages.h"
#include "core/GameArchive.h"
#include "core/MovieChoice.h"
#include <chrono>

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
// AttachConsole/freopen console-reattachment for the GUI-subsystem build --
// see the comment at the top of main() below.
#include <windows.h>
// _open_osfhandle/_dup2/_fileno/_O_TEXT -- DIRK_LOG_FILE's Win32-level
// stdout/stderr redirection, see main()'s own comment on why freopen()
// alone isn't reliable for a GUI-subsystem .exe with no console at all.
#include <fcntl.h>
#include <io.h>
#else
#include <sys/stat.h> // chmod/stat -- clearing a read-only bit, mirrors the Win32 branch below
#include <unistd.h> // dup2/fileno -- DIRK_LOG_FILE's POSIX-side redirection
#endif

namespace {

// Gates every routine/informational "[dirk] ..." print that isn't itself a
// warning or an error a player launching normally needs to see (video
// resolution/frame count, measured audio latency, gamepad connect/
// disconnect, .kimmy/.singe compatibility notes, and the like) -- checked
// fresh each call rather than cached once, same as the existing
// DIRK_DEBUG_TICK/DIRK_DEBUG_INPUT/DIRK_DEBUG_CLICK/DIRK_DEBUG_SHOT checks
// scattered through this file and VideoSource.cpp/LuaEngine.cpp/
// InputMap.cpp, none of which bothered caching either -- this is a handful
// of getenv() calls total across a whole run, not a per-frame cost. Genuine
// failures (video won't open, SDL/Lua init failed, a script error) are
// never gated behind this: a lambda launching from a plain command line
// still needs to see those to have any idea what went wrong.
bool dirkVerbose() { return std::getenv("DIRK_VERBOSE") != nullptr; }

// Directory containing `path`, WITH a trailing slash. The trailing slash
// matters: framework.kimmy derives its own DIR global as
// `singeGetScriptPath():match("(.*[/\\])")` -- a Lua pattern that captures
// everything up to and including the last path separator. That only
// reproduces the intended directory if dirkGetScriptPath() already ends in
// a separator (confirmed by reading framework.kimmy:30-31, not guessed);
// otherwise the match would eat the last path component along with the
// filename it expects to be stripping.
// Fullscreen that also works with no window manager (2026-10-01, Batocera:
// on a Steam Deck the 1440x1080 picture overflowed the 1280x800 screen).
// SDL asks the window manager to make the window fullscreen; under Batocera's
// X11 nothing answers, the window stays at the movie's size, so the picture
// is cut or misplaced. When the request didn't take, Dirk covers the screen
// itself with a borderless window the size of the display -- the picture is
// then letterboxed inside it as usual (computeRenderRects).
// DIRK_FORCE_FULLSCREEN=1 (set by Batocera's dirk-launch.sh, 2026-10-02):
// always fullscreen, whatever the game's saved choice -- a game folder
// copied from a Windows PC where it was windowed would otherwise open as a
// window on a system with no desktop around it. F10 and the menu's
// "Fullscreen" line then have no effect.
bool fullscreenForced() {
    const char* v = std::getenv("DIRK_FORCE_FULLSCREEN");
    return v && v[0] == '1';
}

static bool g_fullscreenCovered = false; // the borderless fallback is in use
void applyFullscreen(SDL_Window* window, bool on, int windowedW, int windowedH) {
    if (!on) {
        SDL_SetWindowFullscreen(window, false);
        if (g_fullscreenCovered) {
            g_fullscreenCovered = false;
            SDL_SetWindowBordered(window, true);
            SDL_SetWindowSize(window, windowedW, windowedH);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        SDL_SyncWindow(window);
        return;
    }
    SDL_SetWindowFullscreen(window, true);
    SDL_SyncWindow(window); // waits (bounded) for the window manager's answer
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    SDL_Rect bounds{};
    if (!display || !SDL_GetDisplayBounds(display, &bounds) || bounds.w <= 0 || bounds.h <= 0) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) && w == bounds.w && h == bounds.h) return;
    SDL_SetWindowFullscreen(window, false);
    SDL_SetWindowBordered(window, false);
    SDL_SetWindowPosition(window, bounds.x, bounds.y);
    SDL_SetWindowSize(window, bounds.w, bounds.h);
    SDL_SyncWindow(window);
    g_fullscreenCovered = true;
    std::fprintf(stderr,
        "[dirk] fullscreen not applied by the system (window %dx%d, screen %dx%d) -- "
        "covering the screen with a borderless window instead\n", w, h, bounds.w, bounds.h);
}

std::string dirOf(const std::string& path) {
    // Both separators, not just '/': on Windows a script path can arrive
    // as "Hayate_HD\Hayate.dirk" (backslash) depending on how the game is
    // launched (relative path from a parent folder, a shortcut's "Target"
    // field, etc.) -- framework.kimmy's own DIR computation already
    // anticipates this (its Lua pattern is literally `[/\\]`, see the
    // comment above), so this needs to match that or `Hayate_HD\` silently
    // becomes `./`, KimmyScript's own relative opens go looking in the
    // wrong folder, and everything downstream that assumed DIR was set up
    // starts failing in ways that look unrelated (missing files, then nil
    // arguments to native calls that were supposed to be filled in by
    // code that never ran).
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "./" : path.substr(0, slash + 1);
}

// Auto-discovers a game's video + script from just its folder, so a
// launcher (shortcut, drag-and-drop, a one-line .bat) only needs to know
// where the game lives, not its internal file names. Every game folder
// seen so far (DLe, Hayate_HD, ...) follows the same shape: exactly one
// top-level launcher script (the shared engine and the game's own addon
// script both live one level deeper, as `*.kimmy` files, dofile'd by it,
// never launched directly), and the disc video sitting in `Movie/`.
//
// The top-level launcher itself is a `.dirk` file, not `.kimmy` -- same
// KimmyScript/Lua content either way (dirk just runs whatever file it's
// given, the extension means nothing to Lua), renamed by convention so
// it's unambiguous: every *other* `.kimmy` file in a game folder is one
// of those internal engine/addon scripts, never meant to be launched
// directly, so giving the actual entry point its own extension means
// double-clicking it (once associated with dirk.exe, see hasDirkExtension
// below) can never be confused with opening one of those by accident.
//
// The video is the one wrinkle: DLe's own Movie/ folder has two files --
// lair.mp4 (the actual disc, tens of thousands of frames) and
// menu-attract.mp4 (a short attract-mode loop) -- with nothing in the
// script or its .cfg files naming either one (the original Singe engine
// takes the video as a separate launch parameter too, it turns out, not
// something a game script declares). Size is the one reliable signal:
// the real disc is always dramatically bigger than a short loop, so the
// largest file in Movie/ wins, and which one got picked is always printed
// so a wrong guess is obvious rather than silently played.
//
// Scans <folder>/<subfolderName> for video files and returns the largest
// one (see findVideoInMovieFolder's own comment for why size is the
// picking signal), or false with nothing printed if that subfolder doesn't
// exist or has no video file in it -- silent on that specific failure
// because findVideoInMovieFolder below tries more than one subfolder name
// and only the LAST attempt should actually report an error to the player.
bool findVideoInSubfolder(const std::string& folder, const char* subfolderName, std::string& outVideo) {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path movieDir = fs::path(folder) / subfolderName;
    std::vector<fs::path> videos;
    for (const auto& entry : fs::directory_iterator(movieDir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                        [](unsigned char c) { return std::tolower(c); });
        if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov") {
            videos.push_back(entry.path());
        }
    }
    if (videos.empty()) return false;

    fs::path chosen = videos.front();
    uintmax_t bestSize = fs::file_size(chosen, ec);
    for (const auto& v : videos) {
        std::error_code sizeEc;
        uintmax_t sz = fs::file_size(v, sizeEc);
        if (!sizeEc && sz > bestSize) {
            bestSize = sz;
            chosen = v;
        }
    }
    if (videos.size() > 1) {
        std::fprintf(stderr, "[dirk] %s: %zu video files in %s/, using the largest: %s\n",
                     folder.c_str(), videos.size(), subfolderName, chosen.filename().string().c_str());
    }
    outVideo = chosen.string();
    return true;
}

// The video half of the above, split out so it can also be used when the
// script is already known (a .dirk file passed/double-clicked directly --
// see the hasDirkExtension branch in main() below) instead of discovered
// from the folder. Same false-plus-stderr-message contract as
// resolveGameFolder.
//
// Tries Movie/ first (the convention every game seen so far actually
// uses), then Video/ as a fallback before giving up -- a real, recurring
// naming mismatch on games converted from other frontends/engines, not a
// guess: rather than hard-fail a game just because its folder happens to
// be named the other way, both are tried and whichever one actually has a
// video wins. Movie/ stays first/preferred (unchanged default, and the
// name every one of Cyril's own 7 shooting games plus Chantze/Triad
// already use), Video/ is purely an extra chance before the real error.
// A Hypseus game's cached Movie/<name>.mkv (see HypseusRemux.h) is only
// good while it matches the soundtracks it was built from --
// "<name>.mkv.audio", see hypseusAudioSignature(). A .mkv made before
// alternate soundtracks were supported has no such file: still good as
// long as the game has no alternate soundtrack.
bool hypseusCacheMatches(const std::filesystem::path& cachedPath, const dirk::HypseusSegment& segment) {
    const std::filesystem::path signaturePath = cachedPath.string() + ".audio";
    std::ifstream sig{signaturePath, std::ios::binary};
    if (!sig) return segment.altAudio.empty();
    const std::string saved((std::istreambuf_iterator<char>(sig)), std::istreambuf_iterator<char>());
    return saved == dirk::hypseusAudioSignature(segment);
}

// Makes sure Movie/<framefile>.mkv exists and matches `ff`, converting it
// when it doesn't (the first launch, or after a soundtrack/piece changed).
// True with `outVideo` set once the .mkv is there.
bool ensureHypseusMovie(const std::string& folder, const dirk::HypseusFrameFile& ff, std::string& outVideo) {
    const dirk::HypseusSegment& segment = ff.segment;
    std::filesystem::path cachedPath = std::filesystem::path(folder) / "Movie" / (ff.baseName + ".mkv");
    const std::filesystem::path signaturePath = cachedPath.string() + ".audio";
    const std::string signature = dirk::hypseusAudioSignature(segment);
    std::error_code existsEc;
    if (std::filesystem::is_regular_file(cachedPath, existsEc) && hypseusCacheMatches(cachedPath, segment)) {
        outVideo = cachedPath.string();
        return true;
    }
    std::string what;
    if (segment.pieces.size() > 1) {
        what = std::to_string(segment.pieces.size()) + " pieces: ";
        for (size_t i = 0; i < segment.pieces.size(); ++i) {
            what += (i ? ", " : "") + std::filesystem::path(segment.pieces[i].videoPath).filename().string() + " at " +
                    std::to_string(segment.pieces[i].frame);
        }
    } else {
        std::string extras;
        for (const auto& alt : segment.altAudio) {
            extras += " + " + std::filesystem::path(alt.path).filename().string();
        }
        what = segment.videoPath + " + " + segment.audioPath + extras;
    }
    std::fprintf(stderr,
        "[dirk] %s: Hypseus-style game detected (%s) -- preparing it for the first time, one moment...\n",
        folder.c_str(), what.c_str());
    for (const auto& dropped : segment.droppedAudio) {
        std::fprintf(stderr, "[dirk] %s: %s left out -- another piece has no soundtrack in that language\n",
                     std::filesystem::path(ff.path).filename().string().c_str(), dropped.c_str());
    }
    if (dirk::remuxHypseusSegment(segment, cachedPath.string())) {
        std::fprintf(stderr, "[dirk] %s: ready (won't need to redo this on future launches)\n", cachedPath.string().c_str());
        if (std::ofstream sig{signaturePath, std::ios::binary | std::ios::trunc}) sig << signature;
        outVideo = cachedPath.string();
        return true;
    }
    std::fprintf(stderr, "[dirk] %s: Hypseus conversion failed -- see errors above\n", folder.c_str());
    return false;
}

bool findVideoInMovieFolder(const std::string& folder, std::string& outVideo) {
    // 2026-09-27: a Hypseus game whose soundtracks changed since its .mkv
    // was cached (e.g. a <name>-it.ogg added) -- drop that stale .mkv first,
    // otherwise the Movie/ search just below would keep finding it and the
    // new language would never show up. It's rebuilt further down.
    // 2026-10-14: every framefile of the folder (two versions of a game,
    // see HypseusRemux.h), and a multi-piece one's pieces count too.
    const std::vector<dirk::HypseusFrameFile> frameFiles = dirk::findHypseusFrameFiles(folder);
    for (const auto& ff : frameFiles) {
        const std::filesystem::path cached = std::filesystem::path(folder) / "Movie" / (ff.baseName + ".mkv");
        std::error_code ec;
        if (std::filesystem::is_regular_file(cached, ec) && !hypseusCacheMatches(cached, ff.segment)) {
            std::fprintf(stderr, "[dirk] %s: %s changed since %s was made -- rebuilding it\n",
                         folder.c_str(), ff.segment.pieces.size() > 1 ? "its pieces or soundtracks" : "soundtracks",
                         cached.filename().string().c_str());
            std::filesystem::remove(cached, ec);
            std::filesystem::remove(cached.string() + ".audio", ec);
            std::filesystem::remove(cached.string() + ".dirkindex", ec);
        }
    }
    // Two framefiles or more: each version gets its .mkv now, so Movie/
    // lists them all and the player picks one (core/MovieChoice.h) -- on
    // the first launch too, not only once each has been converted.
    if (frameFiles.size() > 1) {
        for (const auto& ff : frameFiles) {
            std::string made;
            ensureHypseusMovie(folder, ff, made);
        }
    }
    if (findVideoInSubfolder(folder, "Movie", outVideo)) {
        return true;
    }
    if (findVideoInSubfolder(folder, "Video", outVideo)) {
        std::fprintf(stderr,
            "[dirk] %s: no Movie/ folder (or none of its files looked like a video) -- found the video in Video/ instead, using that\n",
            folder.c_str());
        return true;
    }

    // Last resort before giving up: a Hypseus/Singe-1-style game, whose
    // video+audio ship as a separate .m2v/.ogg pair (named by a "framefile"
    // <Game>.txt at the game's root) instead of one muxed file. See
    // HypseusRemux.h for the full story and why this remuxes into a cached
    // .mkv rather than teaching VideoSource to read two files directly.
    for (const auto& ff : frameFiles) {
        if (ensureHypseusMovie(folder, ff, outVideo)) return true;
        // fall through to the ordinary "no video found" error below
    }

    std::fprintf(stderr,
        "[dirk] %s: no video file found in Movie/ or Video/ -- pass the video and script explicitly (dirk <video> <script.dirk>)\n",
        folder.c_str());
    return false;
}

// Returns false (after printing why on stderr) if either can't be
// resolved unambiguously -- the caller falls back to requiring both paths
// explicitly, which still works exactly as before for that case.
bool resolveGameFolder(const std::string& folder, std::string& outVideo, std::string& outScript) {
    namespace fs = std::filesystem;
    std::error_code ec;

    std::vector<fs::path> scripts;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dirk") {
            scripts.push_back(entry.path());
        }
    }
    if (scripts.empty()) {
        // 2026-09-28: a folder holding a .zdirk (zipped game) instead --
        // the caller opens it (see openZdirkGame()).
        std::vector<fs::path> zipped;
        for (const auto& entry : fs::directory_iterator(folder, ec)) {
            if (entry.is_regular_file() && dirk::hasZdirkExtension(entry.path().string())) zipped.push_back(entry.path());
        }
        if (zipped.size() == 1) {
            outScript = zipped.front().string();
            return true; // video: found by openZdirkGame()
        }
    }
    if (scripts.size() != 1) {
        std::fprintf(stderr,
            "[dirk] %s: expected exactly one .dirk file directly in this folder, found %zu%s\n",
            folder.c_str(), scripts.size(),
            scripts.empty() ? "" : " -- pass the video and script explicitly (dirk <video> <script.dirk>) to disambiguate");
        return false;
    }
    outScript = scripts.front().string();
    return findVideoInMovieFolder(folder, outVideo);
}

// True if `path`'s extension is `.dirk`, case-insensitively (Windows
// filesystems don't care about case, so a double-clicked file could in
// principle arrive as ".KIMMY").
bool hasDirkExtension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return ext == ".dirk";
}

// A .zdirk game (2026-09-28, see core/GameArchive.h): opens the zip, then
// finds the movie next to the .zdirk (Movie/ or Hypseus Video/) -- the
// movie is never read from inside the zip. `outScript` = the script's path
// as the game sees it (in the .zdirk's folder, read from the zip).
bool openZdirkGame(const std::string& zdirkPath, std::string& outVideo, std::string& outScript,
                   const std::string& wantedScript = "", const std::string& shortcutPath = "") {
    namespace fs = std::filesystem;
    dirk::GameArchive& archive = dirk::gameArchive();
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    if (!archive.open(zdirkPath, error, wantedScript, shortcutPath)) {
        std::fprintf(stderr, "[dirk] %s\n", error.c_str());
        return false;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!shortcutPath.empty()) {
        std::fprintf(stderr, "[dirk] %s: shortcut to %s in %s\n", shortcutPath.c_str(),
                     std::filesystem::path(archive.scriptPath()).filename().string().c_str(), zdirkPath.c_str());
    }
    std::fprintf(stderr, "[dirk] zipped game %s: %zu files, read from the zip (opened in %.0f ms%s)\n",
                 zdirkPath.c_str(), archive.fileCount(), ms,
                 archive.cfgFilesCopied() ? (", " + std::to_string(archive.cfgFilesCopied()) + " Cfg file(s) copied next to it").c_str() : "");
    outScript = archive.scriptPath();
    std::error_code ec;
    const fs::path real(archive.realFolder());
    bool movieInZip = false;
    for (const char* d : {"Movie", "Video"}) {
        bool isDir = false;
        if (!fs::is_directory(real / d, ec) && archive.exists((real / d).string(), &isDir) && isDir) movieInZip = true;
    }
    if (movieInZip) {
        std::fprintf(stderr, "[dirk] %s: the movie is inside the zip -- put the Movie folder next to the .zdirk instead "
                             "(outside the zip: it is already compressed, and jumps in it must stay instant)\n",
                     zdirkPath.c_str());
        return false;
    }
    return findVideoInMovieFolder(archive.realFolder(), outVideo);
}

// Where dirk actually needs to chdir to -- NOT the game's own folder.
// Confirmed by reading the real engine source: globals.kimmy dofile()s
// the shared engine as a literal "./KimmyScript/Script/xxx.kimmy" (cwd-
// relative, no DIR involved at all), and main.kimmy loads shared skin
// assets the same way ("./KimmyScript/Skin"..SKINDIR..). Everything
// game-specific (Script/addons.kimmy, Sounds/, Cfg/, Overlay/, Fonts/)
// goes through DIR instead (see dirOf() above / ctx.scriptPath), which is
// already an absolute path by the time this runs and so doesn't care
// what cwd is. So the "./KimmyScript/..." opens are the one thing that
// actually needs the right cwd, and per how these installs are laid out
// (confirmed directly, not guessed) that's the game folder's PARENT --
// KimmyScript/ sits beside each game's own folder as a shared sibling,
// not nested inside it. Walking a few levels up rather than hard-coding
// "exactly one level" covers a deeper install (games grouped under a
// category folder, say) too; falls back to the immediate parent -- the
// shape actually seen -- if no KimmyScript/ turns up in that search, so
// there's always a sensible default instead of silently doing nothing.
//
// The directory containing the running dirk.exe itself, resolved as
// reliably as each platform allows -- deliberately NOT argv[0] taken at
// face value, which can be a bare filename ("dirk", resolved via PATH) or
// a relative path depending on how dirk was actually launched, neither of
// which reliably yields a directory at all. findSharedEngineRoot() below
// checks this first, before anything else.
std::string exeDirectory(const char* argv0) {
#ifdef _WIN32
    // The reliable way to ask Windows "what file is this process actually
    // running from", independent of argv[0]/how dirk was launched (a
    // relative cmd invocation, a shortcut, file-association double-click,
    // ...) -- confirmed necessary this session, for the exact same
    // "launch-method-dependent" reasons DIRK_LOG_FILE exists at all.
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buf).parent_path().string();
    }
#elif defined(__linux__)
    // /proc/self/exe is the equivalent reliable answer on Linux -- no
    // portable POSIX equivalent exists (macOS needs _NSGetExecutablePath,
    // not implemented here -- falls through to the argv[0]-based guess
    // below on that platform, same as every other #ifdef _WIN32-gated
    // feature in this file that doesn't yet have a macOS-specific path,
    // see AudioLatency.cpp for the same pattern).
    {
        std::error_code ec;
        auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec && !self.empty()) {
            return self.parent_path().string();
        }
    }
#endif
    // Fallback for whatever the platform-specific lookup above didn't
    // cover (macOS, or the WinAPI call unexpectedly failing): the best
    // information left is argv[0] itself, resolved against cwd the same
    // way arg1 is further below -- less reliable (a bare "dirk" found via
    // PATH resolves to just that, with no real directory), but strictly
    // better than not trying at all.
    std::error_code ec;
    auto abs = std::filesystem::absolute(argv0, ec);
    return ec ? std::filesystem::path(argv0).parent_path().string()
              : abs.parent_path().string();
}

// Takes the GAME'S OWN FOLDER directly (not a script path -- callers pass
// dirOf(scriptPath) when what they have is a file), so the same search can
// also run very early in main(), before scriptPath is known, against
// whatever folder argv[1] resolves to (see settingsPath below). `exeDir`
// (see exeDirectory() just above) is checked FIRST, before the upward walk
// from the game's own folder: since the installer puts dirk.exe and
// KimmyScript/ in the same folder together (and dirk doesn't run without
// both anyway), that's now the more reliable fixed point, and -- unlike
// walking up from the game's folder -- it works no matter WHERE a game
// folder actually lives, including moved to a completely different drive
// with no shared ancestor with the install at all ("jeux deportes du
// repertoire de dirk", a real, confirmed ask -- previously the only way to
// make that work was keeping the game somewhere under the install's own
// tree). The upward walk stays as a fallback for setups where that
// isn't true (e.g. a dev/test tree with no installed exeDir to speak of).
std::string findSharedEngineRoot(const std::string& gameFolder, const std::string& exeDir) {
    namespace fs = std::filesystem;

    // DIRK_HOME (2026-10-02): set by the Linux launchers (Batocera, ArkOS) to
    // the folder holding dirk and KimmyScript/. Needed when dirk is started
    // through the system loader (an exFAT card where programs can't be run
    // directly): its own path is then the loader's, not dirk's.
    if (const char* home = std::getenv("DIRK_HOME")) {
        std::error_code ec;
        if (home[0] && fs::is_directory(fs::path(home) / "KimmyScript", ec)) return home;
    }

    if (!exeDir.empty()) {
        std::error_code ec;
        if (fs::is_directory(fs::path(exeDir) / "KimmyScript", ec)) {
            return exeDir;
        }
    }

    fs::path candidate = gameFolder; // the game's own folder
    fs::path fallback = candidate.parent_path();              // one level up -- the shape seen so far
    for (int i = 0; i < 6; ++i) {
        fs::path parent = candidate.parent_path();
        if (parent.empty() || parent == candidate) break; // hit a filesystem root
        candidate = parent;
        std::error_code ec;
        if (fs::is_directory(candidate / "KimmyScript", ec)) {
            return candidate.string();
        }
    }
    return fallback.string();
}

// New games (2026-10-08, Cyril): a game folder made with Create.dirk or by
// hand from Blank has no Cfg/ yet, and KimmyScript stops at once without its
// game.cfg (then hscore.cfg, then the save slots). Every file of
// <shared root>/KimmyScript/Cfg/NewGame/ that this game's Cfg/ lacks is
// copied there at launch, Cfg/ created if needed; nothing is ever
// overwritten, and a copy is always writable (the game saves into it).
// Whatever is put in NewGame/ is what a new game starts with.
// Names are compared without case (an existing "CFG" folder or "S1.cfg"
// counts on Linux too). Only for .dirk scripts: a .singe game is left alone.
size_t copyNewGameCfg(const std::string& gameFolder, const std::string& sharedRoot, const std::string& script) {
    namespace fs = std::filesystem;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto findNoCase = [&](const fs::path& dir, const std::string& name, fs::path& found) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return false;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (lower(it->path().filename().string()) == lower(name)) { found = it->path(); return true; }
        }
        return false;
    };
    size_t copied = 0;
    try { // a file name Windows can't give as a narrow string throws: then nothing more is copied
        std::error_code ec;
        if (gameFolder.empty() || lower(fs::path(script).extension().string()) != ".dirk") return 0;
        const fs::path from = fs::path(sharedRoot) / "KimmyScript" / "Cfg" / "NewGame";
        if (!fs::is_directory(from, ec)) return 0;
        fs::path cfg = fs::path(gameFolder) / "Cfg";
        fs::path existing;
        if (findNoCase(fs::path(gameFolder), "Cfg", existing)) cfg = existing;
        for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            const std::string name = it->path().filename().string();
            fs::path already;
            if (findNoCase(cfg, name, already)) continue;
            fs::create_directories(cfg, fec);
            fec.clear();
            if (!fs::copy_file(it->path(), cfg / name, fs::copy_options::skip_existing, fec) || fec) continue;
            fs::permissions(cfg / name, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, fec);
            ++copied;
        }
    } catch (const std::exception&) {
    }
    return copied;
}

void uploadFrameToTexture(SDL_Texture* tex, const AVFrame* f) {
    if (!tex || !f) return;
    SDL_UpdateYUVTexture(tex, nullptr,
                          f->data[0], f->linesize[0],
                          f->data[1], f->linesize[1],
                          f->data[2], f->linesize[2]);
}

// How long to hold the picture back, in real wall-clock time, right at the
// very start of playback (and ONLY then -- see below), so it lines up with
// when the corresponding audio actually becomes audible rather than when it
// left SDL's software queue. Compensates for audio output hardware/driver/
// OS latency (SDL_QueueAudio's own queue plays out in real time regardless
// of how deep it is, so decoding audio further ahead -- see
// VideoSource::setAudioLeadSeconds() -- doesn't touch this; only holding
// picture back does).
//
// This used to also re-arm on every discSkipToFrame() jump (death, scene
// cut, loop point), which did fix sync everywhere but caused a visible
// freeze-frame on every single cut -- not acceptable in actual gameplay.
// That's gone now: main.cpp no longer flushes the audio device queue on a
// jump either (see the audioGeneration() handling below), so the hardware
// latency only ever needs to be paid once, right here, at true startup --
// removing the per-jump re-arm entirely and confirming (by ear, on real
// hardware) that the constant real-time offset this establishes carries
// through every later cut on its own, no freeze needed.
//
// The actual hold DURATION used at runtime is NOT the constant below --
// this went through 0.06/0.15/0.30/0.45s tuning passes by ear on one
// developer's PC and never converged, because it was never a property of
// Dirk to begin with: it's a property of whatever sound card and driver
// the PLAYER has, which is different on every machine Dirk runs on. See
// src/audio/AudioLatency.h -- main.cpp now queries the actual output
// latency this specific machine reports (via WASAPI on Windows) once at
// startup and uses THAT, with a small fixed safety margin on top for
// whatever a shared-mode engine-latency query doesn't fully capture.
// kAudioHoldbackFallbackSeconds below is only what gets used if that query
// isn't available at all (a non-Windows build, or the query failing for
// some reason) -- a return to the old guessing-game, not a real answer, so
// deliberately left modest rather than tuned to any one machine.
constexpr double kAudioHoldbackFallbackSeconds = 0.10;
// Back to its original 0.03 -- a same-session experiment raised this to
// 0.124 to try to fold the F7-measured ~146ms real output latency in here,
// and that was the wrong mechanism: this constant only sizes the ONE-TIME
// startup hold before video starts advancing at all, while the STARTUP
// AUDIO PRIME below queues real audio (and it starts draining/playing for
// real) from t=0 regardless of that hold. A bigger hold just means picture
// stays frozen longer while audio gets a bigger head start, so when the
// hold lifts, the very first video-advance tick sees a bigger jump in the
// audio clock since it was last sampled (see audioClockSecondsAtLastPacing)
// and burns through however many frames that represents in one burst to
// catch up -- at 0.124+0.022=~146ms that's ~3.5 frames snapping forward at
// once, a real, visible stutter right at the start, exactly what raising
// this constant produced. It also never actually fixed the steady-state
// gap it was meant to close: this hold only calibrates video's position
// against SDL's OWN queue accounting, which has no idea a real device adds
// more delay downstream before anything is actually audible -- see
// kAudioOutputHardwareLatencySeconds below for where that correction
// actually belongs instead.
constexpr double kAudioHoldbackSafetyMarginSeconds = 0.03;
// The real fix for the F7-measured ~146ms gap between "SDL says this audio
// has played" (cumulativeAudioContentSeconds - backlog, i.e. what
// audioClockSeconds tracked below, before this) and "a human can actually
// hear it": that gap lives entirely downstream of SDL's own queue
// accounting -- inside the driver/device/OS mixing pipeline -- so nothing
// that calibrates against the queue (like the holdback above) can ever
// close it. It has to be subtracted directly from the position used for
// pacing and for the avOffsetMs diagnostic, continuously, for the whole
// session, not just once at startup. ~146ms was measured with WASAPI
// forced as the driver (SDL_HINT_AUDIO_DRIVER above) -- re-measure with F7
// if the driver choice or the machine changes; this is a property of the
// output hardware/driver, same reasoning as kAudioHoldbackFallbackSeconds
// above, just a different (and, per F7, much bigger) number than the
// WASAPI stream-latency query alone ever reported.
constexpr double kAudioOutputHardwareLatencySeconds = 0.146;

// Applies the launch-settings "volume video" percentage to the movie's PCM
// before it's queued -- SDL_QueueAudio has no per-queue gain control of its
// own, so this is the simplest way to honor that setting without an extra
// mixing layer. `pcm` is S16 (see VideoSource's kAudioOutFormat); a no-op
// fast path at 100% keeps the common case free of the extra pass.
void applyVolume(std::vector<uint8_t>& pcm, int volumePercent) {
    if (volumePercent >= 100) return;
    float scale = std::max(0, volumePercent) / 100.0f;
    int16_t* samples = reinterpret_cast<int16_t*>(pcm.data());
    size_t count = pcm.size() / sizeof(int16_t);
    for (size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<int16_t>(samples[i] * scale);
    }
}

#ifdef _WIN32
// dirk.exe is a GUI-subsystem executable (-mwindows, see CMakeLists.txt)
// rather than the MinGW default of "console" -- no black console window
// flashing open behind the game on every launch, which is both more
// polished and (per real-world testing) plays noticeably better with
// Windows' own "always use this app" default-app machinery for file-type
// associations than a console-subsystem .exe did. Real Singe, by contrast,
// always launches its own console alongside the game, errors and all --
// fine for us during development, but exactly the kind of thing that
// spooks a player who isn't expecting a black text window ("hideConsole"
// in LaunchSettings, below, is the opt-out for that).
//
// The one thing -mwindows costs on its own: every std::fprintf(stderr,
// ...) diagnostic in this file would otherwise go nowhere, including when
// dirk is launched the normal way for debugging -- from an already-open
// terminal. This function is called once at startup (before any window
// exists, from already-loaded LaunchSettings -- see main()) and again
// every time the current game's own KimmyScript UI Options menu sets
// hideConsole (dirkSetHideConsole -> launchSettingsDirty, see the main
// loop), so toggling the option takes effect immediately without a
// relaunch.
void applyConsoleVisibility(bool hide) {
    if (hide) {
        // Confirmed this session: this branch had exactly the same bug the
        // comment below describes fixing in the OTHER (hide == false)
        // branch -- unconditionally reassigning stdout/stderr regardless
        // of what they already pointed at -- just never fixed here too.
        // Real-world impact, worked out by reading this function while
        // chasing a "Generate Log always comes back empty" report: once
        // dirkSetGenerateLog(true)/DIRK_LOG_FILE has redirected stdout/
        // stderr to a real log file, THIS function still runs again every
        // single time ANY setting changes afterward (see the main loop's
        // launchSettingsDirty block, which calls this unconditionally on
        // every dirty tick, not just when hideConsole itself changed) --
        // and with hideConsole=true, it would silently reopen both streams
        // to NUL right out from under the log redirect, permanently, since
        // nothing re-establishes it afterward (dirkSetGenerateLog() is a
        // one-time, idempotent opt-in, not something that re-fires on
        // every settings change). From that point on every fprintf(stdout/
        // stderr, ...) for the rest of the run -- everything except
        // whatever had already been written in the brief window before
        // this ran again -- silently goes nowhere, which reads exactly
        // like "the log file stays empty" to whoever's looking at it.
        // Fixed the same way the other branch already handles it: check
        // GetFileType() on the raw inherited handles first, and leave
        // alone whichever one is already pointed at a real disk file or
        // pipe instead of blindly reopening it to NUL.
        DWORD outType = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
        DWORD errType = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
        bool outAlreadyRedirected = outType == FILE_TYPE_DISK || outType == FILE_TYPE_PIPE;
        bool errAlreadyRedirected = errType == FILE_TYPE_DISK || errType == FILE_TYPE_PIPE;
        // FreeConsole() only detaches from an actual console window --
        // harmless to call even when stdout/stderr are staying put,
        // doesn't touch either stream.
        FreeConsole();
        if (!outAlreadyRedirected) std::freopen("NUL", "w", stdout);
        if (!errAlreadyRedirected) std::freopen("NUL", "w", stderr);
    } else {
        // If stdout/stderr are already pointing somewhere real -- a file or
        // pipe from explicit shell redirection ("dirk.exe ... > log.txt
        // 2>&1") -- leave that alone. Unconditionally forcing them onto
        // CONOUT$ below would silently discard it: confirmed the hard way
        // (see this session's conversation) when DIRK_DEBUG_JOYSTICK
        // output redirected to a file came back completely empty, because
        // this function used to reattach stdout/stderr to the console
        // screen buffer regardless of what the shell had already wired
        // them to. GetFileType() on the raw inherited handles is the right
        // thing to check here, not the stdio FILE* -- CreateProcess()
        // hands a GUI-subsystem child a perfectly valid inherited disk/pipe
        // handle when the shell redirected it, same as any other process;
        // it's only the *absence* of one (the double-click launch, with
        // nothing to inherit at all) that GUI-subsystem processes handle
        // differently from console-subsystem ones.
        DWORD outType = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
        DWORD errType = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
        bool outAlreadyRedirected = outType == FILE_TYPE_DISK || outType == FILE_TYPE_PIPE;
        bool errAlreadyRedirected = errType == FILE_TYPE_DISK || errType == FILE_TYPE_PIPE;
        if (!outAlreadyRedirected || !errAlreadyRedirected) {
            // AttachConsole(ATTACH_PARENT_PROCESS) asks Windows "is there
            // already a console I was started from?"; if dirk was run from
            // cmd/PowerShell/MSYS2 with no redirection, this succeeds and
            // the freopen() calls below route stdout/stderr into that
            // existing console, restoring exactly the output you'd see
            // from a console-subsystem build. If dirk was launched by
            // double-click/file-association instead (no parent console to
            // attach to), this quietly does nothing.
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                if (!outAlreadyRedirected) std::freopen("CONOUT$", "w", stdout);
                if (!errAlreadyRedirected) std::freopen("CONOUT$", "w", stderr);
            }
        }
    }
}
#endif

// Opens `path` for writing and makes it stdout/stderr's actual destination
// for the rest of this run, on every platform including a Windows
// GUI-subsystem .exe with no console at all (see DIRK_LOG_FILE's own
// long-form comment in main() for exactly why a plain std::freopen() alone
// isn't reliable there). Shared by two independent opt-ins: the
// DIRK_LOG_FILE environment variable below (power users, scripted/kiosk
// launches) and dirkSetGenerateLog()'s zero-configuration in-game "Generate
// Log" toggle (see activateGenerateLog() just below and LaunchSettings.h) --
// same mechanism, two different ways to reach it. Returns false (and
// leaves stdout/stderr untouched) on failure -- there is nowhere useful to
// report that failure to, since a broken redirect is exactly what would
// have made anywhere else unreachable too.
bool redirectStdioToFile(const std::string& path) {
#ifdef _WIN32
    // dirk_log.txt can already exist marked read-only on disk (shipped
    // that way inside the game's own folder, left over from a previous
    // copy/extraction, ...). CREATE_ALWAYS-equivalent opens below cannot
    // overwrite a read-only file -- cleared here, first, before anything
    // else touches this path. Best-effort: GetFileAttributesA fails
    // harmlessly (returns INVALID_FILE_ATTRIBUTES) when the file doesn't
    // exist yet, which is the common case and not an error.
    DWORD existingAttrs = GetFileAttributesA(path.c_str());
    if (existingAttrs != INVALID_FILE_ATTRIBUTES &&
        (existingAttrs & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesA(path.c_str(), existingAttrs & ~FILE_ATTRIBUTE_READONLY);
    }

    // freopen_s() (not a raw CreateFileA + _dup2 dance) is what reliably
    // repairs stdout/stderr on a GUI-subsystem .exe with no console at
    // all: with no console attached, _fileno(stdout)/_fileno(stderr) can
    // both read -2 (the documented "_NO_CONSOLE_FILENO" sentinel), and
    // _dup2() is documented to fail with EBADF whenever its target fd is
    // negative -- so it alone can never repair either stream from that
    // state. freopen_s() doesn't have that restriction: it discards
    // whatever the stream currently points to (real fd, -2, anything) and
    // assigns a freshly opened, valid one in its place.
    FILE* newOut = nullptr;
    errno_t outErr = freopen_s(&newOut, path.c_str(), "w", stdout);
    if (outErr != 0 || !newOut) return false;

    // Share stdout's now-known-good fd with stderr. freopen_s() above
    // takes an exclusive lock on `path` -- confirmed on real hardware via
    // GetLastError()/ERROR_SHARING_VIOLATION while diagnosing this --
    // meaning stderr can't get its OWN independent handle on the same
    // path (a second freopen_s() call there just fails the same way). So
    // when _dup2() can't target stderr's fd directly (the -2 case above),
    // this instead: _dup()s stdout's ALREADY-OPEN fd onto a fresh,
    // CRT-chosen slot (no path is reopened, so no sharing violation is
    // possible, and no negative target is ever named, so EBADF can't
    // apply either), wraps that duplicate via _fdopen(), and copies its
    // fields directly into stderr's own FILE struct in place
    // (`*stderr = *tempErr`) -- the same kind of in-place rebind
    // freopen_s() does by design, just sourced from an fd already held
    // instead of a path that can't be reopened. tempErr's own FILE*
    // wrapper is deliberately never fclose()'d afterward: closing it
    // would close the fd it wraps, which is now the same fd stderr
    // depends on -- left to leak (a few dozen bytes, once per process)
    // rather than pull that fd out from under stderr.
    bool sharedFd = (_dup2(_fileno(stdout), _fileno(stderr)) == 0);
    if (!sharedFd) {
        int dupFd = _dup(_fileno(stdout));
        FILE* tempErr = (dupFd >= 0) ? _fdopen(dupFd, "a") : nullptr;
        if (tempErr) {
            *stderr = *tempErr;
            sharedFd = true;
        }
        // Deliberately not treating a failure here as fatal: stdout is
        // already successfully redirected at this point (freopen_s
        // succeeded above) -- abandoning the whole function over stderr
        // alone would throw that away for nothing. If this also fails,
        // _fileno(stderr) is left sitting at its -2 sentinel and stderr's
        // output for this run just doesn't land anywhere -- see
        // activateGenerateLog()'s own comment for why that's now the
        // deliberate choice instead of popping open a console window.
    }

    // Also point the raw Win32 standard handles at the same file, for any
    // code that calls WriteConsole/GetStdHandle directly instead of going
    // through the C stdio FILE*s above.
    HANDLE rawHandle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
    if (rawHandle != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_OUTPUT_HANDLE, rawHandle);
        SetStdHandle(STD_ERROR_HANDLE, rawHandle);
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::clearerr(stdout);
    std::clearerr(stderr);
    return true;
#else
    // Same real-world cause confirmed on the Windows build above, applied
    // here too for parity: an existing dirk_log.txt owned by this user but
    // missing the write bit would make freopen("w", ...) fail the same
    // silent way. Best-effort -- errors ignored, including "file doesn't
    // exist yet" (the common case).
    {
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && !(st.st_mode & S_IWUSR)) {
            ::chmod(path.c_str(), st.st_mode | S_IWUSR);
        }
    }
    FILE* outReopened = std::freopen(path.c_str(), "w", stdout);
    if (!outReopened) return false;
    dup2(fileno(stdout), fileno(stderr));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    // Same precaution as the Win32 branch above, cheap insurance even
    // though freopen() is generally more thorough than a raw dup2() about
    // resetting a stream's state.
    std::clearerr(stdout);
    std::clearerr(stderr);
    return true;
#endif
}

// Peeks at this game's own Cfg/game.cfg for whichever of ITS OWN dirk_*
// lines are present (whatever persistPerGameDirkSettings(), below,
// last wrote there) and overrides the matching fields of `settings` in
// place -- bypassing Lua entirely. Generalizes what used to be two
// separate single-field peeks (peekPerGameGenerateLog, then
// peekPerGameBezelIndex, added one bug report at a time -- see below).
//
// Why this needs to exist at all: `settings` (the parameter) starts out
// as loadLaunchSettings()'s return value, i.e. the SHARED, install-wide
// dirk_settings.cfg -- a "factory default" file, not this game's own
// saved preferences (see persistPerGameDirkSettings()'s own comment: a
// live change is never written back there any more, only to the current
// game's own game.cfg). Lua (specifically service.kimmy's readConfig(),
// called from initJob() a few frames into the render loop, not
// immediately at boot) is normally what applies a game's own per-game
// override on top of that shared default. Every dirk_* field is
// therefore, natively, whatever the shared file says for however many
// frames it takes Lua to get there -- confirmed directly, twice, by
// Cyril, for two different fields hit one at a time as each one's own
// symptom got reported: Titan_AE's session left dirk_settings.cfg's log
// flag on, and Hayate then started logging too despite its own
// "Generate Log" being off (fixed by what was peekPerGameGenerateLog());
// then a game saved with "Bezel: Aucun" still flashed bezel #1 for a
// couple of frames at boot before Lua corrected it back down (fixed by
// what was peekPerGameBezelIndex()). Rather than wait for the same class
// of bug to surface field by field for Fullscreen/VolumeVldp/
// VolumeNonVldp/Scanlines too, this reads every dirk_* key this game's
// own game.cfg might have in one pass and applies all of them here,
// before the very first frame -- so the shared file only ever has a say
// for a field THIS game has genuinely never saved, e.g. the very first
// launch of a brand-new game.
//
// Mirrors game.cfg's own "key = value" text format (see service.kimmy's
// writeConfig(), the only writer of this file) rather than
// dirk_settings.cfg's compact "key=value" one (see LaunchSettings.cpp) --
// two different files, two different conventions. A key simply not
// present in game.cfg leaves that one field alone (still whatever the
// shared file said) -- same "runs out of lines" tolerance service.kimmy's
// own readConfigLine() already has for an older/partial game.cfg, and
// exactly what lets a brand-new game.cfg (no dirk_* lines at all yet)
// fall through to the shared defaults untouched. dirk_BezelEnabled (the
// pre-2026-09-23 on/off key) is still recognized as a fallback for
// dirk_BezelIndex, same migration peekPerGameBezelIndex() used to do on
// its own, for a game.cfg that hasn't been re-saved since that rename.
// Deliberately only ever looks at game.cfg, never game2.cfg (altCfg) --
// altCfg is a runtime Lua choice this native, pre-Lua check has no way to
// know about, so a game that boots straight onto its alternate profile
// still gets the shared-file value for these fields, same as before any
// of this existed.
void peekPerGameDirkSettings(const std::string& gameDataPath, dirk::LaunchSettings& settings) {
    const std::string path = gameDataPath + "Cfg/game.cfg"; // gameDataPath already ends in a separator, see dirOf()
    std::ifstream in(path);
    if (!in) return;

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { s.clear(); return; }
        size_t end = s.find_last_not_of(" \t\r\n");
        s = s.substr(start, end - start + 1);
    };
    auto clampVolume = [](int v) { return std::max(0, std::min(100, v)); };

    bool haveBezelIndex = false;
    bool haveLegacyBezelEnabled = false;
    int legacyBezelValue = 0;

    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trim(key);
        trim(val);
        if (key == "dirk_Fullscreen") {
            settings.fullscreen = (val == "1");
        } else if (key == "dirk_VolumeVldp") {
            settings.volumeVldp = clampVolume(std::atoi(val.c_str()));
        } else if (key == "dirk_VolumeNonVldp") {
            settings.volumeNonVldp = clampVolume(std::atoi(val.c_str()));
        } else if (key == "dirk_Scanlines") {
            settings.scanlines = (val == "1");
        } else if (key == "dirk_BezelIndex") {
            settings.bezelIndex = std::max(0, std::atoi(val.c_str()));
            haveBezelIndex = true;
        } else if (key == "dirk_BezelEnabled") {
            legacyBezelValue = (val == "1") ? 1 : 0;
            haveLegacyBezelEnabled = true;
        } else if (key == "dirk_GenerateLog") {
            settings.generateLog = (val == "1");
        } else if (key == "dirk_SubtitleIndex") {
            settings.subtitleIndex = std::max(0, std::atoi(val.c_str()));
        } else if (key == "dirk_AudioTrack") {
            settings.audioTrack = std::max(0, std::atoi(val.c_str()));
        } else if (key == "dirk_AudioTrackName") {
            settings.audioTrackName = val;
        } else if (key == "dirk_SubtitleSize") {
            const int v = std::atoi(val.c_str());
            if (v > 0) settings.subtitleSize = std::max(dirk::kSubtitleSizeMin, std::min(dirk::kSubtitleSizeMax, v));
        }
    }

    if (!haveBezelIndex && haveLegacyBezelEnabled) {
        settings.bezelIndex = legacyBezelValue;
    }
}

// Patches THIS game's own Cfg/game.cfg with the current live LaunchSettings
// values, bypassing Lua -- the counterpart to peekPerGameDirkSettings() just
// above, called any time launchSettingsDirty fires (native F10/F11/F12
// hotkeys, or a dirkSetXxx() call from a game's own UI Options menu before
// that menu screen is actually exited).
//
// 2026-09-22 (Cyril): dirk_settings.cfg is a factory-default fallback ONLY
// now -- see LaunchSettings.h -- and must never be rewritten by live play,
// or one game's session leaks into being every other not-yet-configured
// game's starting point (exactly the Titan_AE -> Hayate_HD Generate Log
// leak peekPerGameDirkSettings()'s own comment documents). So instead of
// saving back to the shared file, every live change is now reported
// directly onto the CURRENT game's own game.cfg, in the exact same file
// and "key = value" text format service.kimmy's writeConfig() itself
// writes -- so a later readConfig() (this game's own menu, or its next
// launch) picks it up exactly as if the change had come from that menu.
//
// Deliberately conservative: this only ever overwrites the six dirk_*
// lines themselves (wherever they are -- normally the trailing six, in
// order, but found by key rather than assumed), never touches the ~37
// CONFIG_FIELDS (dip_*, mycolor*, mash, etc.) lines above them -- this
// function has no idea what correct per-game values those hold, only
// service.kimmy does. And if game.cfg doesn't exist yet, or doesn't
// already have all ~37 of those lines (brand new install, game never
// opened its own Options menu once), this is a deliberate no-op: there's
// no safe way to fabricate the rest of the file natively. The change
// simply stays session-only until the player opens the game's own menu
// once, which then saves everything -- dip_* fields AND the current live
// dirk_* values (writeConfig() reads them straight back via
// dirkGetFullscreen() etc.) -- together, the normal way.
// Review point 6 (2026-09-26): true when any field persistPerGameDirkSettings()
// writes differs -- the launchSettingsDirty block only rewrites game.cfg then.
// Keep in sync with the dirk_* lines written at the end of that function.
bool perGameDirkSettingsDiffer(const dirk::LaunchSettings& a, const dirk::LaunchSettings& b) {
    return a.fullscreen != b.fullscreen || a.volumeVldp != b.volumeVldp ||
           a.volumeNonVldp != b.volumeNonVldp || a.scanlines != b.scanlines ||
           a.bezelIndex != b.bezelIndex || a.generateLog != b.generateLog ||
           a.subtitleIndex != b.subtitleIndex || a.subtitleSize != b.subtitleSize ||
           a.audioTrack != b.audioTrack || a.audioTrackName != b.audioTrackName;
}

void persistPerGameDirkSettings(const std::string& gameDataPath, const dirk::LaunchSettings& settings) {
    const std::string path = gameDataPath + "Cfg/game.cfg"; // gameDataPath already ends in a separator, see dirOf()

    std::ifstream in(path);
    if (!in) return; // no per-game file yet -- nothing safe to patch, see comment above

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    in.close();

    auto trim = [](const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return std::string();
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    };

    // 7 entries, not 6: dirk_BezelEnabled is the old (pre-2026-09-23) name
    // for what's now dirk_BezelIndex (on/off -> which bezel of however many,
    // see LaunchSettings::bezelIndex) -- both must be stripped so an old
    // game.cfg still written with the old key doesn't end up with BOTH the
    // stale line AND the freshly-written one below, which would shift every
    // positional readConfigLine() call after it in service.kimmy's
    // readConfig() (this block is always exactly 6 lines, read purely by
    // position, not by key name).
    // dirk_SubtitleIndex, dirk_SubtitleSize, then dirk_AudioTrack (2026-09-27) are written LAST, after
    // dirk_GenerateLog: service.kimmy reads this block by position, so an
    // older service.kimmy still finds its six lines where it expects them.
    static const char* kDirkKeys[11] = {
        "dirk_Fullscreen", "dirk_VolumeVldp", "dirk_VolumeNonVldp",
        "dirk_Scanlines",  "dirk_BezelEnabled", "dirk_BezelIndex", "dirk_GenerateLog",
        "dirk_SubtitleIndex", "dirk_SubtitleSize", "dirk_AudioTrack", "dirk_AudioTrackName",
    };

    // Strip any existing dirk_* lines, wherever they land -- everything
    // else (the CONFIG_FIELDS lines, and anything unrecognized) is kept
    // exactly as found, in its original order.
    std::vector<std::string> kept;
    kept.reserve(lines.size());
    for (const auto& l : lines) {
        size_t eq = l.find('=');
        std::string key = eq == std::string::npos ? std::string() : trim(l.substr(0, eq));
        bool isDirkKey = false;
        for (const char* k : kDirkKeys) {
            if (key == k) { isDirkKey = true; break; }
        }
        if (!isDirkKey) kept.push_back(l);
    }

    // Bail out rather than guess: fewer than 37 remaining lines means this
    // game.cfg predates a full CONFIG_FIELDS save (or is mid-write/
    // corrupted) -- leave it untouched rather than write a half-file.
    if (kept.size() < 37) return;

    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    for (const auto& l : kept) out << l << "\n";
    out << "dirk_Fullscreen = " << (settings.fullscreen ? 1 : 0) << "\n";
    out << "dirk_VolumeVldp = " << settings.volumeVldp << "\n";
    out << "dirk_VolumeNonVldp = " << settings.volumeNonVldp << "\n";
    out << "dirk_Scanlines = " << (settings.scanlines ? 1 : 0) << "\n";
    out << "dirk_BezelIndex = " << settings.bezelIndex << "\n";
    out << "dirk_GenerateLog = " << (settings.generateLog ? 1 : 0) << "\n";
    out << "dirk_SubtitleIndex = " << settings.subtitleIndex << "\n";
    out << "dirk_SubtitleSize = " << settings.subtitleSize << "\n";
    out << "dirk_AudioTrack = " << settings.audioTrack << "\n";
    out << "dirk_AudioTrackName = " << settings.audioTrackName << "\n";
}

// The zero-configuration counterpart to DIRK_LOG_FILE: dirkSetGenerateLog(
// true) (see api_singe.cpp / LaunchSettings::generateLog) routes here,
// reachable from a game's own "UI Options" menu (KimmyScript's
// service.kimmy) so a stuck player can be talked through turning logging
// on without ever touching an environment variable -- see
// LaunchSettings.h for the full rationale. Idempotent (a no-op once
// `logFileActive` is already true, whichever path set it), and forces
// DIRK_VERBOSE/DIRK_DEBUG_INPUT on for the rest of THIS run via an
// in-process setenv/_putenv_s -- never written to the player's persistent
// environment, and re-checked fresh on every getenv() call elsewhere in
// this file (see dirkVerbose()'s own comment), so this takes effect
// immediately regardless of whether it runs before the main loop starts or
// from inside it (the live-toggle case). Input problems are deliberately
// the one extra category forced on here, not just DIRK_VERBOSE: they're
// the single most common class of "it's stuck" report this engine has
// actually seen (this file's own history). Never un-redirects: turning
// dirkSetGenerateLog back off only changes what the *next* launch defaults
// to (see that function's own comment) -- there is no clean way to hand
// stdout/stderr back to wherever they pointed before without risking
// losing output that was mid-flight, and the field is meant to be toggled
// on right before reproducing a problem, not toggled off again mid-run.
void activateGenerateLog(bool& logFileActive, const std::string& gameDataPath) {
    if (logFileActive) return;
    const std::string logPath = gameDataPath + "dirk_log.txt"; // gameDataPath already ends in a separator, see dirOf()
    if (!redirectStdioToFile(logPath)) return;
    // No more AllocConsole() fallback here if stderr's own redirect (dup2,
    // then the _dup()+struct-copy fallback inside redirectStdioToFile())
    // came up short -- removed on request, after Cyril found dirk popping
    // open a real black console window at the start of every launch. Root
    // cause: he'd left "Generate Log" on from earlier troubleshooting (it's
    // a persisted LaunchSettings field, so it fires on literally every
    // subsequent launch, not just the one it was turned on for -- see this
    // function's own doc comment above), and that console was this exact
    // fallback: on his machine specifically, stderr's file-redirect keeps
    // failing, so this used to paper over it with a visible console instead
    // ("something visible beats a log file that silently stays empty," per
    // the fallback's own old reasoning). He doesn't want a console at all,
    // trusting the log file alone -- so now, when stderr can't be repaired,
    // nothing is shown and nothing is silently substituted; whatever would
    // have printed through stderr just doesn't land anywhere this run.
    //
    // WORTH KNOWING, not yet confirmed either way: since nearly every
    // fprintf(...) in this file goes through stderr specifically (grep
    // main.cpp for it -- dozens of sites, vs. a literal handful through
    // stdout), if stderr really is failing to redirect on Cyril's machine,
    // dirk_log.txt may end up mostly/entirely empty on a run where this
    // would have fired the console before -- i.e. removing the console
    // might have traded "a visible but unwanted window" for "a log file
    // that looks fine but is missing almost everything," not actually
    // fixed the underlying stderr redirect. Next step if that turns out to
    // be the case: read dirk_log.txt after a real run and compare against
    // what used to show in the console -- that tells us for certain
    // whether stderr is actually reaching the file now, rather than
    // guessing at another fix blind.
    logFileActive = true;
#ifdef _WIN32
    _putenv_s("DIRK_VERBOSE", "1");
    _putenv_s("DIRK_DEBUG_INPUT", "1");
#else
    setenv("DIRK_VERBOSE", "1", 1);
    setenv("DIRK_DEBUG_INPUT", "1", 1);
#endif
    std::fprintf(stderr, "[dirk] Generate Log turned on -- writing this run's log to %s\n", logPath.c_str());
    // Explicit flush, belt-and-suspenders on top of the _IONBF unbuffered
    // mode redirectStdioToFile() already set: costs nothing if that's
    // already working, and rules out "the confirmation line above is
    // sitting in a buffer nothing ever flushed" as a remaining explanation
    // if the file still comes back empty.
    std::fflush(stderr);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    // DIRK_LOG_FILE, checked before literally anything else -- a way to
    // capture every std::fprintf(stderr/stdout, ...) diagnostic in this
    // file to a real file no matter how dirk was launched, including a
    // plain double-click with no console at all. applyConsoleVisibility()
    // below (see its own comment) only ever attaches to an EXISTING parent
    // console (cmd/PowerShell) or silently does nothing -- a double-click
    // launch has no parent console to attach to, so nothing prints
    // anywhere in that case, which is exactly the situation this exists
    // for: confirming what's actually happening on a run where the
    // symptom itself might depend on how dirk was launched -- e.g. a
    // launch-method-dependent difference can't be diagnosed by switching
    // to the OTHER launch method just to see the log, since that changes
    // the very thing being diagnosed (confirmed for real, this session:
    // double-clicking a .dirk file always runs whatever .exe is registered
    // as the default app for that extension, which is not necessarily the
    // same .exe a freshly-typed cmd command points at if more than one
    // build/copy of dirk.exe exists on disk -- the actual cause of a
    // "works from cmd, not on double-click" report that looked at first
    // like a controls.cfg bug, and turned out to just be a stale .exe
    // sitting at the double-click target). Set as a persistent user/system
    // environment
    // variable (Windows' System Properties -> Environment Variables, not
    // "set DIRK_LOG_FILE=... && dirk.exe" typed in cmd -- that's still a
    // console launch) so it's picked up on a genuine double-click too,
    // point it at a fixed path (e.g. DIRK_LOG_FILE=C:\dirk_log.txt), then
    // launch exactly the way that reproduces the issue and read that file
    // afterward. Deliberately independent of hideConsole/applyConsole-
    // Visibility below -- this always writes to the file when set,
    // regardless of that setting -- and left in place permanently rather
    // than only for this one investigation, since the exact same
    // "launch-method-dependent bug, can't see the log without changing
    // the launch method" trap will recur.
    // See redirectStdioToFile()'s own comment for exactly how this
    // redirect works and why a plain freopen() alone isn't enough on
    // Windows. logFileActive is threaded through the rest of main() so the
    // zero-configuration dirkSetGenerateLog() path below (see
    // activateGenerateLog()) knows not to redirect a second time on top of
    // this if both happen to be in play for the same run.
    bool logFileActive = false;
    if (const char* logFilePath = std::getenv("DIRK_LOG_FILE")) {
        logFileActive = redirectStdioToFile(logFilePath);
        // Failure (logFileActive left false): can't fprintf that failure
        // anywhere useful (that's the whole problem this env var exists to
        // solve) -- falls back to whatever stdout/stderr already were
        // (nowhere, on a plain double-click) rather than crashing over a
        // diagnostics feature failing to set itself up.
    }

    // Checked first, before anything else -- with no game argument at all
    // there's nothing to resolve settingsPath against below (see
    // findSharedEngineRoot()), so there's no sensible way to even decide
    // hideConsole before printing this. That's fine: a usage error should
    // always be visible regardless of whatever hideConsole a previous run
    // saved, so this no longer waits on loading settings first.
    //
    // Computed this early (argv[0] is always available, regardless of
    // argc) since findSharedEngineRoot() below needs it before it needs
    // anything else -- see exeDirectory()'s own comment.
    const std::string exeDir = exeDirectory(argv[0]);

    // 2026-10-10: dirk --wizard <game-folder> is how Wizard.exe (see
    // src/launcher/wizard_launcher.c) starts KimmyScript's create mode:
    // Wizard is built into dirk (LuaEngine::runWizardChunk) and runs on that
    // folder in place of a game script, as if a Wizard.dirk sat there.
    bool wizardMode = false;
    if (argc >= 3 && std::strcmp(argv[1], "--wizard") == 0) {
        wizardMode = true;
        argv[1] = argv[2];
        argc = 2;
    }

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <game-folder>\n", argv[0]);
        std::fprintf(stderr, "       %s <video-file> [script.lua]   (manual, e.g. to sanity-check a raw video)\n", argv[0]);
        return 1;
    }

    // Resolved to absolute right away, before any of the three branches
    // below touch it -- a path like "\Hayate_HD\Hayate_HD.dirk" (leading
    // backslash, no drive letter -- what a copy-pasted "relative" path or
    // certain shells can hand us) is NOT relative to the current
    // directory on Windows, it's relative to the current drive's ROOT
    // regardless of cwd. dirOf()/resolveGameFolder() building a Movie/
    // search path directly out of argv[1] therefore silently looked in
    // the wrong place (e.g. "E:\Hayate_HD\Movie" instead of
    // "E:\Dirk Blank\Hayate_HD\Movie") and failed with "no video file
    // found in Movie/" even though the folder was right there -- confirmed
    // against a real run, not theoretical. A plain relative path
    // ("Hayate_HD\Hayate_HD.dirk", no leading separator) happened to still
    // work, since std::filesystem resolves *that* against cwd same as
    // everywhere else, which is why this only showed up with the
    // leading-backslash form specifically.
    std::string arg1;
    {
        std::error_code absEc;
        auto abs = std::filesystem::absolute(argv[1], absEc);
        arg1 = absEc ? argv[1] : abs.string();
    }

    // dirk_settings.cfg lives at <shared engine root>/KimmyScript/Cfg/ --
    // Dirk and KimmyScript are installed together and neither runs without
    // the other, so that shared, install-independent folder (already home
    // to KimmyScript/Cfg/default.cfg -- see service.kimmy's readConfig())
    // is the fixed point every game shares. findSharedEngineRoot() checks
    // exeDir (dirk.exe's own install location) first, before falling back
    // to the exact same upward search used further below to chdir() for
    // KimmyScript's own "./KimmyScript/Script/..." dofile()s -- run here
    // first, directly on arg1's own folder (is_directory check mirrors the
    // one the auto-discover branch below does, since arg1 can be either a
    // game folder or a .dirk/video file at this point and the two need
    // different handling to land on the right starting folder). Needed this
    // early, before any window exists, so applyConsoleVisibility() below
    // can read LaunchSettings' hideConsole before deciding whether to
    // attach to a console at all.
    std::error_code arg1IsDirEc;
    const std::string gameFolderForSettings =
        std::filesystem::is_directory(arg1, arg1IsDirEc) ? arg1 : dirOf(arg1);
    // Also the base for the loading-screen assets below (dirk_loading.png/
    // dirk_loading_font.ttf) -- same reasoning as dirk_settings.cfg just
    // above: a fixed point that doesn't depend on the game's own folder
    // layout, shared by every game a given Dirk+KimmyScript install runs.
    const std::string sharedEngineRoot = findSharedEngineRoot(gameFolderForSettings, exeDir);
    const std::string settingsPath = sharedEngineRoot + "/KimmyScript/Cfg/dirk_settings.cfg";
    dirk::LaunchSettings settings = dirk::loadLaunchSettings(settingsPath);

#ifdef _WIN32
    applyConsoleVisibility(settings.hideConsole);
#endif

    std::string videoPath;
    bool videoDiscovered = true; // false only when the movie was given on the command line (see MovieChoice.h)
    std::string discoveredScript; // keeps resolveGameFolder's result alive; scriptPath below may point into it
    const char* scriptPath = nullptr;
    if (wizardMode) {
        if (!std::filesystem::is_directory(arg1) || !findVideoInMovieFolder(arg1, videoPath)) {
            std::fprintf(stderr, "[dirk] --wizard %s: Wizard goes in a game folder, next to its Movie folder\n", arg1.c_str());
            return 1;
        }
        discoveredScript = (std::filesystem::path(arg1) / "Wizard.dirk").string();
        scriptPath = discoveredScript.c_str();
        std::fprintf(stderr, "[dirk] Wizard %s on %s\n", dirk::wizardChunkTag(), arg1.c_str());
    } else if (argc == 2 && std::filesystem::is_directory(arg1)) {
        // Single argument and it's a folder: auto-discover mode (see
        // resolveGameFolder above) -- this is the whole point, letting a
        // launcher pass just "C:\Games\Hayate_HD" instead of having to
        // know and hard-code that game's own Movie\ filename and script
        // name. Hard-fails here rather than falling through to the
        // legacy path below: resolveGameFolder already printed exactly
        // why (ambiguous or missing files), and silently trying to
        // "open" a directory as a video would just bury that under a
        // second, less useful error.
        if (!resolveGameFolder(arg1, videoPath, discoveredScript)) {
            return 1;
        }
        std::string shortcutZdirk, shortcutScript;
        if (dirk::hasZdirkExtension(discoveredScript)) {
            const std::string zdirk = discoveredScript;
            if (!openZdirkGame(zdirk, videoPath, discoveredScript)) return 1;
        } else if (dirk::readZdirkShortcut(discoveredScript, shortcutZdirk, shortcutScript)) {
            const std::string shortcut = discoveredScript;
            if (!openZdirkGame(shortcutZdirk, videoPath, discoveredScript, shortcutScript, shortcut)) return 1;
        }
        scriptPath = discoveredScript.c_str();
        if (dirkVerbose()) {
            std::fprintf(stderr, "[dirk] %s -> video: %s, script: %s\n",
                         arg1.c_str(), videoPath.c_str(), discoveredScript.c_str());
        }
    } else if (argc == 2 && dirk::hasZdirkExtension(arg1)) {
        // A zipped game (.zdirk), double-clicked like a .dirk.
        if (!openZdirkGame(arg1, videoPath, discoveredScript)) {
            return 1;
        }
        scriptPath = discoveredScript.c_str();
    } else if (argc == 2 && hasDirkExtension(arg1)) {
        // Single argument and it's a .dirk file directly: the script is
        // already known, so only the video needs discovering (from the
        // folder that file lives in) -- this is what makes double-
        // clicking a .dirk in Explorer work as "launch the game" once
        // that extension is associated with dirk.exe (Properties on any
        // .dirk file -> "Open with" -> dirk.exe -> "Always use this
        // app"), no per-game shortcut needed at all.
        std::string folder = dirOf(arg1);
        std::error_code existsEc;
        if (std::filesystem::exists(arg1, existsEc) && !existsEc) {
            discoveredScript = arg1;
        } else {
            // The exact path given doesn't exist -- happens whenever
            // whoever/whatever typed it assumed the "<Folder>.dirk" naming
            // convention some games happen to follow (DLe/DLe.dirk) but
            // this particular game's own launcher doesn't (e.g. the real
            // file is Hayate_HD/Hayate.dirk, not Hayate_HD/Hayate_HD.dirk --
            // see the README's own Running section, which uses exactly that
            // example). Pressing on with a nonexistent scriptPath would
            // still "succeed" here (this branch never opens the file, just
            // strings) and only fail much later inside Lua with a far more
            // confusing cascade -- runFile's own "cannot open" error,
            // followed by controls.cfg failing too since framework.kimmy's
            // SCANCODE table never got defined. Recover the same way folder
            // mode already does: if exactly one .dirk file actually exists
            // in this folder, use that instead of the (wrong) name given.
            std::vector<std::filesystem::path> candidates;
            std::error_code dirEc;
            for (const auto& entry : std::filesystem::directory_iterator(folder, dirEc)) {
                if (entry.is_regular_file() && entry.path().extension() == ".dirk") {
                    candidates.push_back(entry.path());
                }
            }
            if (candidates.size() == 1) {
                discoveredScript = candidates.front().string();
                std::fprintf(stderr,
                    "[dirk] warning: %s not found -- using %s instead (the only .dirk file in this folder)\n",
                    arg1.c_str(), discoveredScript.c_str());
            } else {
                std::fprintf(stderr, "[dirk] %s: file not found\n", arg1.c_str());
                return 1;
            }
        }
        // 2026-09-28: a shortcut .dirk to one of the games of a .zdirk
        // (see dirk::readZdirkShortcut) -- e.g. HayatePSX.dirk next to
        // Hayate.zdirk.
        std::string shortcutZdirk, shortcutScript;
        if (dirk::readZdirkShortcut(discoveredScript, shortcutZdirk, shortcutScript)) {
            const std::string shortcut = discoveredScript;
            if (!openZdirkGame(shortcutZdirk, videoPath, discoveredScript, shortcutScript, shortcut)) {
                return 1;
            }
        } else if (!findVideoInMovieFolder(folder, videoPath)) {
            return 1;
        }
        scriptPath = discoveredScript.c_str();
        if (dirkVerbose()) {
            std::fprintf(stderr, "[dirk] %s -> video: %s\n", scriptPath, videoPath.c_str());
        }
    } else {
        // Legacy/manual form: a bare video file (no script -- just plays
        // it, e.g. to sanity-check a raw video per the README) or an
        // explicit video+script pair -- still exactly what it always was,
        // and still the way to override auto-discovery when a game
        // folder doesn't fit the one-script/one-obvious-video shape.
        videoPath = arg1;
        scriptPath = argc >= 3 ? argv[2] : nullptr;
        videoDiscovered = false;
    }
    // Absolute before anything below can change the working directory --
    // both are about to get resolved to a directory (dirOf() / the chdir()
    // just past it), and doing that against a path that's still relative
    // to the ORIGINAL cwd would silently double up ("Hayate_HD/" becomes
    // cwd, then "Hayate_HD/Hayate.dirk" resolves against THAT and looks
    // for "Hayate_HD/Hayate_HD/Hayate.dirk"). videoPath doesn't strictly
    // need this (it's opened before the chdir below), but there's no
    // reason for it to depend on that ordering either.
    std::string absScriptStorage;
    {
        std::error_code absEc;
        if (!videoPath.empty()) {
            auto abs = std::filesystem::absolute(videoPath, absEc);
            if (!absEc) videoPath = abs.string();
        }
        if (scriptPath) {
            absEc.clear();
            auto abs = std::filesystem::absolute(scriptPath, absEc);
            if (!absEc) {
                absScriptStorage = abs.string();
                scriptPath = absScriptStorage.c_str();
            }
        }
    }

    // The game's own data folder -- same directory as the launched .dirk
    // script, and (per the real KimmyScript layout, confirmed by reading
    // framework.kimmy/main.kimmy: DIR = dirOf(scriptPath) is what
    // Overlay/-relative spriteLoad calls are built from) also where
    // Cfg/Movie/Overlay live. An optional Bezel/bezel.png here is purely a
    // dirk-engine feature -- see Bezel.h -- with no Lua/KimmyScript
    // involvement at all.
    const std::string gameDataPath = scriptPath ? dirOf(scriptPath) : ""; // a .zdirk's own folder too (its script path is there)

    // The launched .dirk file's own stem (filename without extension or
    // directory, e.g. "Triad" for ".../Triad_HD/Triad.dirk") -- gameDataPath
    // above already collapses that filename away (dirOf() keeps only the
    // directory), so this is computed separately, straight from scriptPath,
    // specifically so loadBezels() below can still tell apart two .dirk
    // launchers that share the same gameDataPath (see Bezel.h). Empty for
    // the bare-video-file legacy launch mode (scriptPath == nullptr).
    const std::string scriptStem = scriptPath ? std::filesystem::path(scriptPath).stem().string() : "";

    // `settings` is still just the SHARED dirk_settings.cfg value at this
    // point (loaded once, way above, before gameDataPath was even known) --
    // Lua hasn't run yet, so nothing has applied this game's own per-game
    // overrides. Apply them here, natively, before literally anything below
    // consumes any dirk_* field -- see peekPerGameDirkSettings()'s own
    // comment for the full history (this used to be two separate one-field
    // fixes, added as each field's own symptom got reported one at a time;
    // generalized, 2026-09-23 (Cyril), so the shared file no longer has any
    // say at all for a field this game has already saved its own value
    // for -- only a genuinely brand-new game.cfg, or one predating this
    // whole mechanism, still falls through to it).
    if (scriptPath && !gameDataPath.empty()) {
        const size_t newCfg = copyNewGameCfg(gameDataPath, findSharedEngineRoot(gameDataPath, exeDir), scriptPath);
        if (newCfg) std::fprintf(stderr, "[dirk] %zu file(s) copied from KimmyScript/Cfg/NewGame to this game's Cfg folder\n", newCfg);
    }
    if (!gameDataPath.empty()) {
        peekPerGameDirkSettings(gameDataPath, settings);
    }
    if (fullscreenForced()) {
        if (!settings.fullscreen) std::fprintf(stderr, "[dirk] DIRK_FORCE_FULLSCREEN: fullscreen, although this game was saved windowed\n");
        settings.fullscreen = true;
    }

    // Zero-configuration troubleshooting log: whatever this game's own
    // "UI Options" menu last saved for dirkSetGenerateLog() (see
    // LaunchSettings::generateLog) takes effect from here, i.e. before
    // literally anything else that might print gets a chance to -- same
    // reasoning as DIRK_LOG_FILE above, just triggered by a persisted
    // setting instead of an environment variable. No-op if the setting is
    // off, if DIRK_LOG_FILE already redirected stdio above (logFileActive
    // already true, checked inside activateGenerateLog itself), or if
    // there's no game folder to log into at all (scriptPath == nullptr,
    // gameDataPath == ""). settings.generateLog above already reflects this
    // game's own per-game override, thanks to peekPerGameDirkSettings()
    // just above -- nothing further to peek at here.
    if (!gameDataPath.empty() && settings.generateLog) {
        activateGenerateLog(logFileActive, gameDataPath);
    }

    // Always-on <game>/dirk_error.txt (2026-09-26, review point 2 -- see
    // ScriptErrorLog.h for what it holds and why it's cheap). Resolved to
    // an absolute path HERE, before the chdir to the shared engine root
    // just below: the file itself is only created lazily, at the first
    // script error, possibly long after that chdir -- a relative
    // gameDataPath would then point somewhere else entirely. begin() also
    // deletes a leftover file from a previous run.
    // Which KimmyScript folder this run reads, with a short MD5 of each
    // script file (2026-09-27, Cyril had several KimmyScript copies and no
    // way to tell which one a run used -- see KimmyFingerprint.h). Same
    // findSharedEngineRoot() call as the chdir just below, so it names the
    // folder the script's "./KimmyScript/..." opens really resolve to.
    // Printed to stderr (dirk_log.txt with Generate Log on) and put in the
    // dirk_error.txt header, so any error report says which version ran.
    std::string kimmyDescription;
    if (scriptPath) {
        kimmyDescription = dirk::describeKimmyScript(findSharedEngineRoot(gameDataPath, exeDir));
        std::fprintf(stderr, "[dirk] %s\n", kimmyDescription.c_str());
    }

    dirk::ScriptErrorLog scriptErrors;
    if (!gameDataPath.empty()) {
        // 2026-10-12 (Cyril): dirk_error.log became dirk_error.txt -- a .txt
        // opens with a double-click everywhere and goes through mail/chat as
        // plain text. A dirk_error.log left by an older dirk is removed here,
        // so the game folder never holds a stale error file next to the new
        // one (same rule as begin(): an error file always means "the LAST run").
        std::error_code legacyEc;
        std::filesystem::remove(std::filesystem::path(gameDataPath) / "dirk_error.log", legacyEc);
        std::error_code absEc;
        std::filesystem::path errorLogPath =
            std::filesystem::absolute(std::filesystem::path(gameDataPath) / "dirk_error.txt", absEc);
        char startedAt[64] = "";
        std::time_t nowTime = std::time(nullptr);
        if (const std::tm* local = std::localtime(&nowTime)) {
            std::strftime(startedAt, sizeof(startedAt), "%Y-%m-%d %H:%M:%S", local);
        }
        scriptErrors.begin(absEc ? gameDataPath + "dirk_error.txt" : errorLogPath.string(),
                           std::string("Dirk script errors -- ") + (dirk::gameArchive().active() ? dirk::gameArchive().zdirkPath() : std::string(scriptPath)) + " -- run started " + startedAt +
                           (kimmyDescription.empty() ? std::string() : "\n" + kimmyDescription) +
                           "\n(one entry per distinct error, with its Lua call chain; repeats are counted at the end)");
    }

    // A handful of KimmyScript's own dofile()/spriteLoad() calls
    // (see findSharedEngineRoot() above) are relative paths resolved
    // against the process's actual working directory, not against
    // gameDataPath -- dirk knows gameDataPath internally, but plain Lua
    // io.open has no idea it exists. The original engine's setups are
    // built assuming cwd is the folder that has KimmyScript/ as a
    // sibling (that's the only way those specific relative opens were
    // ever going to work), which is why every .bat up to now had to cd
    // there first. Doing that chdir here instead means dirk works the
    // same regardless of how it's actually launched -- a shortcut, a
    // stray cmd.exe, or Explorer's drag-and-drop/double-click (which
    // sets cwd to dirk.exe's own folder, not the game's) -- removing the
    // last reason a per-game .bat was needed. Not done for
    // DIRK_SCREENSHOT specifically: that path (an env var, read fresh at
    // each use further down) is a debug aid of mine, not part of the
    // normal flow, so it stays relative to wherever dirk was actually
    // launched from -- pass it as an absolute path when testing headless.
    if (scriptPath) {
        std::string sharedRoot = findSharedEngineRoot(gameDataPath, exeDir); // gameDataPath == dirOf(scriptPath)
        std::error_code chEc;
        std::filesystem::current_path(sharedRoot, chEc);
        if (chEc) {
            std::fprintf(stderr,
                "[dirk] warning: couldn't switch to %s: %s -- "
                "the script's own relative file opens may fail\n",
                sharedRoot.c_str(), chEc.message().c_str());
        }
    }

    // Two (or more) versions of the movie in its folder, e.g. 4:3 and 16:9
    // (2026-10-14, Cyril): the player picks one, at every launch, before
    // anything is opened -- see MovieChoice.h.
    if (videoDiscovered && !videoPath.empty()) {
        const std::vector<dirk::MovieVersion> versions = dirk::findMovieVersions(videoPath);
        if (versions.size() >= 2) {
            const int pick = dirk::chooseMovieVersion(versions, sharedEngineRoot, settings.fullscreen, settings.swapFaceButtons);
            if (pick < 0) return 0;
            videoPath = versions[static_cast<size_t>(pick)].path;
        }
    }

    dirk::VideoSource disc;
    // Multi-threaded video decoding (review point 1, step 2; "auto" by
    // default, dirk_settings.cfg "decode_threads" overrides it, see
    // LaunchSettings::decodeThreads). "auto" = this PC's core count,
    // capped at 4 (more rarely helps a single 1080p stream).
    {
        int threads = settings.decodeThreads;
        if (threads == 0) {
            const unsigned cores = std::thread::hardware_concurrency();
            threads = cores == 0 ? 1 : static_cast<int>(std::min(cores, 4u));
        }
        disc.setDecodeThreads(threads);
        // This game's saved soundtrack (Dirk Options "Audio"), opened
        // directly -- no switch after the fact. 0 = the file's default.
        disc.setPreferredAudioTrack(settings.audioTrack - 1);
    }
    if (!disc.openStream(videoPath)) {
        std::fprintf(stderr, "[dirk] failed to open video: %s\n", videoPath.c_str());
        return 1;
    }
    // Whether the slow part (finishOpening(), below -- see its comment) is
    // about to actually run, decided now, before paying for either it or a
    // loading screen: a cheap stat() of the cache file, not a real attempt
    // to load it (see VideoSource::hasCachedIndexFile()). Almost always
    // true after the very first launch of a given game, since the cache
    // this checks for is exactly what that first run writes.
    const bool needsIndexBuild = !dirk::VideoSource::hasCachedIndexFile(videoPath);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[dirk] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    // Audio gets its own SDL_InitSubSystem call, separate from video above,
    // specifically so a bad audio driver choice can never take the whole
    // program down with it (see the fallback below) -- video always starts
    // even if audio ends up silent.
    //
    // Prefer WASAPI over SDL's own default driver pick on Windows. A direct,
    // controlled test this session (F7's test-tone-vs-flash measurement,
    // completely bypassing all of Dirk's own decode/seek/pacing code) found
    // real output latency of ~5 video frames on whatever SDL picked by
    // default -- confirmed via "driver=directsound" logged at device-open,
    // a legacy Windows compatibility layer -- versus ~3.5 frames with
    // SDL_AUDIODRIVER=wasapi forced. Not a full fix by itself (3.5 frames is
    // still a lot -- see kAudioHoldbackSafetyMarginSeconds above, raised
    // this same session to compensate for the remainder), but a real,
    // measured improvement with no downside on any machine where WASAPI is
    // available, which is effectively every Windows install Dirk targets.
    // Windows only (2026-10-04): elsewhere there is no WASAPI and the hint
    // only cost a failed first try and a confusing log line.
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "wasapi");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr,
            "[dirk] SDL audio init with driver hint 'wasapi' failed (%s) -- "
            "retrying with SDL's own default driver choice\n", SDL_GetError());
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, ""); // clear the hint -- empty string means "let SDL pick"
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            std::fprintf(stderr,
                "[dirk] SDL audio init failed entirely (%s) -- movie/sound will be silent\n",
                SDL_GetError());
        }
    }
#else
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[dirk] SDL audio init failed entirely (%s) -- movie/sound will be silent\n",
                     SDL_GetError());
    }
#endif
    // Own SDL_InitSubSystem call too, same reasoning as audio above -- no
    // controller plugged in, or a driver problem, should silence input, not
    // take down the whole program. SDL_INIT_GAMEPAD pulls in joystick
    // support as well (it's a superset), so this alone is enough for both
    // SDL_EVENT_GAMEPAD_BUTTON_DOWN/UP/AXIS_MOTION and the ADDED/REMOVED
    // events the main loop opens/closes controllers from below -- see
    // GamepadSlot and controls.cfg's GAMEPAD_0.* bindings (src/lua/
    // InputMap.h/.cpp). Previously not implemented at all (see this file's
    // old header comment) -- keyboard-only, confirmed missing directly by
    // the user ("mon joystick ne fonctionne pas").
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr,
            "[dirk] SDL game controller init failed (%s) -- joystick/gamepad input will be unavailable, keyboard still works\n",
            SDL_GetError());
    }
    // SDL2 scaled textures with nearest-neighbor by default, which was fine
    // at 1:1 (windowed, native size) but turned visibly blocky/jagged the
    // moment the letterbox rect scaled the picture up or down (any
    // fullscreen screen that isn't exactly disc.width() x disc.height()) --
    // e.g. the "crenele" look on a 21:9 monitor. SDL3's default texture
    // scale mode is already SDL_SCALEMODE_LINEAR (confirmed against the
    // real installed header's doc comment), so no hint-set is needed here
    // at all -- videoTex/overlayTex/the bezel below get smooth scaling for
    // free at creation time.
    // SDL3_mixer 3.x needs its own MIX_Init() (SDL2_mixer had no equivalent --
    // Mix_OpenAudio used to do both roles at once) before MIX_CreateMixerDevice.
    MIX_Mixer* soundMixer = nullptr; // handed to ctx.soundMixer once ctx exists, below
    if (!MIX_Init()) {
        std::fprintf(stderr, "[dirk] warning: MIX_Init failed: %s (sound disabled)\n", SDL_GetError());
    } else {
        soundMixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (!soundMixer) {
            std::fprintf(stderr, "[dirk] warning: MIX_CreateMixerDevice failed: %s (sound disabled)\n", SDL_GetError());
        }
    }

    // Separate SDL audio stream for the movie's own embedded audio track
    // (dialogue/music decoded by VideoSource -- see its hasAudio()/
    // takePendingAudio()). Deliberately NOT routed through SDL_mixer/
    // ctx.soundMixer above: that mixer is sized/tuned for short one-shot
    // soundLoad()/soundPlay() chunks, while this is a continuous PCM stream
    // pushed in arbitrarily-sized chunks via SDL_PutAudioStreamData --
    // simpler and more robust as its own stream with no callback.
    //
    // SDL3 also folds "open a device" and "hand me something to push PCM
    // into" into one call (SDL_OpenAudioDeviceStream) -- there's no more
    // separate want/have SDL_AudioSpec negotiation to inspect (SDL_AudioSpec
    // itself dropped the `samples` field entirely: SDL3 handles buffering/
    // resampling behind the stream, whatever format we ask for).
    SDL_AudioStream* movieAudioStream = nullptr;
    if (disc.hasAudio()) {
        SDL_AudioSpec want{};
        want.freq = disc.audioSampleRate();
        want.format = SDL_AUDIO_S16;
        want.channels = disc.audioChannels();
        movieAudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, nullptr, nullptr);
        if (!movieAudioStream) {
            std::fprintf(stderr, "[dirk] warning: SDL_OpenAudioDeviceStream failed: %s (movie will be silent)\n", SDL_GetError());
        } else {
            SDL_ResumeAudioStreamDevice(movieAudioStream); // start it running; an empty stream just plays silence
            if (dirkVerbose()) {
                std::fprintf(stderr, "[dirk] movie audio stream opened: requested %dHz/%dch, driver=%s\n",
                             want.freq, want.channels, SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
            }
        }
    }

    // Measured (not guessed) audio output latency for THIS machine -- see
    // AudioLatency.h and kAudioHoldbackFallbackSeconds above. This one
    // number drives both the one-time startup holdback (below) and
    // VideoSource's own audio decode-ahead margin, so a slow/high-latency
    // machine and a fast/low-latency one each get a build that's actually
    // correct for them, from the same .exe, with nothing for the player to
    // configure.
    const double measuredAudioLatencySeconds =
        dirk::queryAudioOutputLatencySeconds(kAudioHoldbackFallbackSeconds);
    const double audioHoldbackSeconds = measuredAudioLatencySeconds + kAudioHoldbackSafetyMarginSeconds;
    if (dirkVerbose()) {
        std::fprintf(stderr, "[dirk] audio holdback: %.1fms measured + %.1fms safety margin = %.1fms\n",
                     measuredAudioLatencySeconds * 1000.0, kAudioHoldbackSafetyMarginSeconds * 1000.0,
                     audioHoldbackSeconds * 1000.0);
    }
    // VideoSource's own audio decode-ahead lead used to be tied directly to
    // audioHoldbackSeconds (comfortably above it, so the catch-up loop's
    // margin never cancelled the holdback out via starvation -- see
    // VideoSource.cpp's decodeOneFrame()). That coupling assumed the
    // measured hardware-latency number was the main thing worth decoding
    // ahead for. A direct, controlled comparison on real hardware said
    // otherwise: the same PC, the same speakers, the same file plays
    // perfectly in sync in VLC, which rules hardware/driver/output latency
    // out as the cause of a real, persistent lag Dirk still had even with
    // that number correctly measured and compensated for. What WASN'T
    // ruled out: SDL_QueueAudio is a strict FIFO draining in real time, and
    // a deeper backlog sitting in it is a deeper delay between "queued" and
    // "audible" REGARDLESS of hardware latency -- confirmed on a real
    // capture, the steady-state backlog (audioQueuedMs in the tick log)
    // was running at ~230-260ms, well beyond the ~100ms this lead was
    // actually trying to target, simply because nothing capped it there.
    // Decoupling this from audioHoldbackSeconds and cutting it down to a
    // small, fixed margin -- just enough above one audio packet's own
    // duration (an AAC frame is ~21ms) to keep the catch-up loop from
    // starving the device between decode calls, not an attempt to
    // compensate for hardware latency at all -- is the direct experiment
    // for that: a shallower target should mean a shallower steady-state
    // backlog, which is what actually determines how old the audio the
    // device is currently playing is relative to what's on screen.
    // Originally 0.06s (see the reasoning above this comment) -- but a real
    // DIRK_DEBUG_TICK capture during ordinary play showed audioQueuedMs
    // sawtoothing between ~60ms (right after each stepForward() catch-up
    // decode) and ~18-20ms (right before the next one, ~41.7ms of video
    // content later at 23.976fps) -- and real, repeated AUDIO UNDERRUN
    // events every ~1-2s once that trough got shaved any further by
    // ordinary scheduling jitter. That's a genuine, audible glitch on its
    // own, and -- now that video pacing is itself driven by this same
    // queue's real played position (see audioClockSeconds above) -- an
    // underrun no longer just glitches the audio, it stalls video too
    // (guarded against separately above, but better not to hit it in the
    // first place). Raising this margin raises the whole sawtooth, giving
    // the trough real headroom above zero. This is safe to do now in a way
    // it wasn't when 0.06 was chosen: back then, video pacing was plain
    // wall-clock dt with no cap on how far the queue could grow relative to
    // real playback, so a deeper lead meant a deeper UNBOUNDED backlog (the
    // ~230-260ms runaway this constant was originally shrunk to fix).
    // Video pacing now tracks the queue's own real played position, so a
    // bigger fixed lead here just means a bigger fixed (bounded, already
    // accounted for in the avOffsetMs math) backlog, not a growing one.
    constexpr double kAudioLeadMarginSeconds = 0.12;
    // Runtime, adaptive copy of the constant above -- see the underrun
    // detection further down (audioQueueWasEmpty) for why this needs to be
    // mutable now. 0.12 was measured clean on one specific PC this session;
    // a real tester's own machine came back with continuous audio stutter
    // on that same build ("ca saccade tout le temps", not just after a
    // cut), meaning that machine's real audio pipeline needs more headroom
    // than this one does -- a different sound card/driver/CPU load, not a
    // bug in the margin itself. Rather than guess a bigger fixed constant
    // for everyone (this project's whole history of 0.06/0.15/0.30/0.45s
    // guesses never converged that way), this value now grows on its own,
    // live, the first time THIS machine actually shows a real underrun --
    // see the growth step below -- so it settles whatever a given machine
    // genuinely needs instead of shipping one number that's either too
    // tight for a slower machine or needlessly loose (more lag than
    // necessary) on a faster one.
    double audioLeadSecondsCurrent = kAudioLeadMarginSeconds;
    disc.setAudioLeadSeconds(audioLeadSecondsCurrent);

    int lastAudioGeneration = disc.audioGeneration();
    bool lastAudioPaused = false;
    // Total content-seconds of audio ever handed to SDL_QueueAudio so far.
    // Combined with SDL_GetQueuedAudioSize() (how much of that is still
    // sitting unplayed) this lets us compute, with no external measurement
    // needed, exactly what content position the device has ACTUALLY played
    // out so far -- see the DIRK_DEBUG_TICK block below, where this becomes
    // a direct avOffsetMs number instead of a screen-recording comparison.
    double cumulativeAudioContentSeconds = 0.0;
    // Underrun detection (see the AUDIO UNDERRUN log below, right after this
    // tick's SDL_QueueAudio call): avOffsetMs is provably blind to a real
    // silence gap -- confirmed on a real capture, OBS measured a genuine,
    // constant ~208ms (5 frames) lag in the first few seconds of play, no
    // seek involved at all, while avOffsetMs sat at a reassuring ~25-44ms
    // the whole time. Why the formula can't see it: it infers "how much
    // audio has actually played" purely from (content ever queued) minus
    // (content still sitting in the queue) -- if the queue runs dry for a
    // stretch (SDL just feeds the hardware silence, with no API to ask it
    // "did that just happen"), the moment fresh content is queued again
    // this same math instantly reads as "fully caught up", with no memory
    // that real wall-clock time passed while nothing was actually audible.
    // A single such gap early in a generation permanently shifts everything
    // queued after it later than it should be, with nothing in the existing
    // diagnostics ever able to show it. This tracks queue-empty transitions
    // directly against SDL_GetQueuedAudioSize() every tick (not just on the
    // throttled 500ms DIRK_DEBUG_TICK print further down, which is coarse
    // enough to miss a gap entirely) to find out, with real data instead of
    // more guessing, whether this is where that 208ms actually comes from.
    bool audioQueueWasEmpty = false;
    Uint32 audioUnderrunStartTicks = 0;
    // Audio-clock video pacing: confirmed on a real, sustained capture (no
    // seeks at all, just ~10 seconds of ordinary play) that avOffsetMs and
    // audioQueuedMs both climb STEADILY and never come back down -- +274ms
    // of drift over 10.1s of content, a consistent ~2.7% rate mismatch, not
    // a one-time event. That rules out every seek/cut-specific fix this
    // project has made (none of them could produce a continuous drift with
    // no seek involved) and points somewhere more fundamental: video here
    // has always been paced against SDL_GetTicks() (a CPU timer) via
    // frameAccumulator += dt*discFps, while the audio hardware drains the
    // queue at its OWN real rate -- and nothing has ever compared the two.
    // If that real rate is even slightly different from the nominal 48kHz
    // Dirk assumes (a DirectSound resampling quirk, a device not actually
    // running at 48kHz natively, anything on that machine's real audio
    // stack), audio content queued at the CPU-timer's rate piles up in the
    // queue faster than the device empties it, forever, which is exactly a
    // permanently growing backlog -- and a permanently growing REAL delay
    // between what's on screen and what's audible, regardless of how
    // correct every other part of this session's fixes are.
    //
    // The fix every real media player uses for exactly this: don't trust
    // two independent clocks to stay in step. Let the audio device's own
    // ACTUAL playback position -- audioClockSeconds below, the same
    // "content already queued minus content still waiting" math avOffsetMs
    // already computes -- drive how far video advances each tick, instead
    // of a CPU timer. Whatever the real audio drain rate turns out to be on
    // a given machine, video now tracks it directly, so it can't drift away
    // from it even if that real rate never matches the nominal 48kHz at
    // all. Falls back to plain wall-clock dt when there's no audio (or no
    // real reading yet) so silent movies/menus still pace correctly.
    double audioClockSeconds = 0.0;
    bool audioClockValid = false;
    double audioClockSecondsAtLastPacing = 0.0;
    double audioQueuedMsNow = -1.0; // current backlog, in ms -- computed alongside audioClockSeconds every tick; the DIRK_DEBUG_TICK print reads this instead of recomputing it
    // SDL3-specific closed-loop drift correction. Measured directly (not
    // guessed): comparing a DIRK_DEBUG_TICK log from this SDL3 build against
    // an equivalent SDL2 run of the same file/cut, both with zero underruns
    // and byte-identical VideoSource.cpp decode code, the SDL3 build's
    // audioQueuedMs backlog still grew roughly TWICE as fast post-cut
    // (~9.0ms/s vs SDL2's ~4.9ms/s). The audio-clock pacing above (see
    // audioClockSeconds) is designed to make a growing backlog harmless --
    // it's subtracted back out of audioClockSeconds every tick -- and that
    // held on SDL2, but the residual SDL3-only gap points at
    // SDL_GetAudioStreamQueued() not fully accounting for something SDL3's
    // SDL_AudioStream buffers internally (format/resample staging) the way
    // SDL2's raw SDL_GetQueuedAudioSize() didn't need to. Rather than guess
    // a fixed correction amount from this one measurement, actively drain
    // any backlog sitting above the lead target by nudging the stream's own
    // playback speed via SDL_SetAudioStreamFrequencyRatio -- proportional to
    // the CURRENT measured excess every tick, so it self-scales to whatever
    // the real drift rate is on a given machine/file instead of assuming
    // this one session's number generalizes.
    // Calibration history, each step a real measurement, not a re-guess:
    //   deadband=20/span=130/maxRatio=0.01 (first attempt) -- tested on a
    //   real ~5min UNINTERRUPTED capture (no cuts at all): ratio saturated
    //   at the +1% cap after ~2 minutes and STAYED saturated, yet the
    //   backlog kept climbing underneath it the whole time (~1-2ms/s
    //   residual growth even at the cap, avOffsetMs reaching ~580ms by
    //   5 minutes) -- proof the real steady-state drift on this machine is
    //   closer to ~11ms/s than the ~9ms/s a shorter/different capture had
    //   suggested, so the old 10ms/s max drain (1%) was barely short of
    //   covering it, permanently saturated with no margin. Also spent
    //   ~2 minutes ramping up to that saturated value before it mattered.
    constexpr double kBacklogCorrectionDeadbandMs = 20.0; // backlog within this much of the lead target: ratio stays 1.0 (ordinary decode-call sawtooth, not real drift -- avoid chattering SDL_SetAudioStreamFrequencyRatio every tick over noise)
    constexpr double kBacklogCorrectionSpanMs = 60.0;     // ramps linearly from the deadband edge up to the max ratio over this many additional ms of backlog -- shortened from 130 so full correction engages well before backlog reaches a few hundred ms, not after
    constexpr double kBacklogCorrectionMaxRatio = 0.02;   // cap: +2% playback speed at most (~34 cents of pitch -- a third of a semitone; very unlikely to be noticed on program material applied intermittently/proportionally, but a real, non-zero trade-off, not free) -- drains ~20ms/s, roughly double the ~11ms/s real need measured above, so it can actually catch up instead of merely slowing the climb
    float audioFrequencyRatioApplied = 1.0f; // last ratio actually handed to SDL -- only re-call when it moves enough to matter
    // Safety net for whatever the ratio correction above doesn't fully
    // cover (a machine with even worse real drift than measured here, or
    // a level that runs far longer than the 5 minutes this was tuned
    // against): if the backlog ever grows past this regardless, force the
    // same hard reset already used at scene cuts (trim + SDL_ClearAudioStream
    // + re-anchor) even mid-scene, so the worst case is a bounded, rare,
    // small audio skip instead of an unbounded, ever-growing lag. Set well
    // above the ~550ms peak seen in the uncorrected-cap capture above (this
    // should rarely if ever fire once the wider ratio margin is in place --
    // it exists purely as a backstop, not the primary fix).
    constexpr double kHardResyncBacklogMs = 500.0;
    // SDL3_image has no IMG_Init/IMG_Quit/IMG_INIT_* at all any more -- it
    // detects and loads whatever format a file actually is on demand, with
    // no explicit per-format init step.
    if (!TTF_Init()) {
        std::fprintf(stderr, "[dirk] warning: TTF_Init failed: %s (text rendering disabled)\n", SDL_GetError());
    }

    // SDL3's SDL_CreateWindow dropped the x/y position args entirely (4 args
    // now, not 6) -- centering is a separate SDL_SetWindowPosition call
    // after creation. SDL_WINDOW_SHOWN is also gone: windows are shown by
    // default now, there's no flag for it any more.
    //
    // SDL_WINDOW_RESIZABLE (2026-09-23, Cyril): windowed mode used to be a
    // fixed size (whatever disc.width()/height() was at launch) with no OS
    // resize border at all. Safe to turn on unconditionally -- it only
    // adds a resize border/grip when NOT fullscreen (SDL ignores it while
    // SDL_WINDOW_FULLSCREEN is active, ignoring the fixed size then already
    // being what real Singe's -w flag did on its own screen) -- and needs
    // no other change: computeRenderRects() below already recomputes the
    // aspect-correct content/bezel rects from SDL_GetCurrentRenderOutputSize()
    // fresh every frame rather than once (see its own comment -- that was
    // built to survive SDL_SetWindowFullscreen's async resize, and a live
    // drag-resize lands through the exact same path). Dragging the window
    // itself narrower/shorter than the picture just letterboxes further,
    // same as a mismatched-aspect fullscreen monitor already does.
    // Width at the picture's real shape (720x480 with 8:9 pixels -> 640x480,
    // see VideoSource::displayAspect()); square pixels: the movie's size.
    //
    // OpenGL ES on a bare screen (KMSDRM: Batocera/ArkOS consoles, Pi...),
    // 2026-10-04: SDL asks for desktop OpenGL there by default, and the
    // Mali drivers of the handhelds only do OpenGL ES -- an R36S under
    // ArkOS found no EGL config at all for any format ("Couldn't find
    // matching EGL config") and Dirk stopped on a black screen. OpenGL ES
    // 2.0 is what every such driver offers (Mesa too), and SDL's GLES2
    // renderer draws everything Dirk needs. If the window still can't be
    // made, one retry with SDL's defaults. DIRK_GLES=1 asks for the same
    // anywhere (testing, odd setups).
    const char* videoDriverName = SDL_GetCurrentVideoDriver();
    const bool preferGles = (videoDriverName && std::strcmp(videoDriverName, "kmsdrm") == 0) ||
                            (std::getenv("DIRK_GLES") && std::strcmp(std::getenv("DIRK_GLES"), "1") == 0);
    if (preferGles) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2,opengl,vulkan,software");
        std::fprintf(stderr, "[dirk] screen: %s -- using OpenGL ES 2\n", videoDriverName ? videoDriverName : "?");
    }
    SDL_Window* window = SDL_CreateWindow("Dirk", disc.displayWidth(), disc.height(), SDL_WINDOW_RESIZABLE);
    if (!window && preferGles) {
        std::fprintf(stderr, "[dirk] window with OpenGL ES failed (%s) -- retrying with SDL's defaults\n", SDL_GetError());
        SDL_GL_ResetAttributes();
        SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
        window = SDL_CreateWindow("Dirk", disc.displayWidth(), disc.height(), SDL_WINDOW_RESIZABLE);
    }
    if (!window) {
        std::fprintf(stderr, "[dirk] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    // Floor so the window can't be dragged down to something the picture
    // effectively disappears into (or, at the extreme, a 0-pixel edge) --
    // a quarter of the disc's own resolution is comfortably small while
    // still showing something recognizable, and scales with whatever this
    // particular game's own movie resolution is rather than a flat guess.
    SDL_SetWindowMinimumSize(window, std::max(160, disc.displayWidth() / 4), std::max(120, disc.height() / 4));
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    // Real Singe hides the OS mouse pointer entirely for the whole run --
    // confirmed by the user directly (moving the mouse never brings it
    // back on screen there either, unlike dirk before this). Matters for
    // more than polish: a game like Mad Dog McCree draws its OWN aiming
    // crosshair sprite every frame (see singeWantsCrosshairs() in
    // api_singe.cpp, which always tells the game "no native crosshair
    // layer here, draw your own") -- with the OS cursor also visible on
    // top of that, the player would see two overlapping pointers. The
    // mouse itself still moves and still generates the input events this
    // engine reads (onMouseMoved, click-mapped switches, etc.) -- only
    // its on-screen glyph is suppressed, same as real Singe. Done once
    // here, for the life of the process, rather than toggled per-screen:
    // every KimmyScript menu this engine has seen drives navigation
    // through mapped switches/keys, never by literally pointing-and-
    // clicking with a visible OS pointer, so there's no menu context
    // that would need it back.
    SDL_HideCursor(); // SDL3 split the old single-arg SDL_ShowCursor(SDL_DISABLE) into two no-arg calls
    // vsync matters more than it looks like it should: without it,
    // SDL_RenderPresent() returns immediately and the main loop below has
    // nothing else pacing it (the frame-accumulator logic further down only
    // throttles how often a NEW video frame is decoded, not how often the
    // current one gets redrawn), so it re-presents the same picture
    // thousands of times a second -- 100% of a CPU core plus the GPU
    // running flat out for zero visual benefit, which is exactly the kind
    // of load that spins a fan up. vsync makes RenderPresent block until
    // the next display refresh, which alone caps the loop to the monitor's
    // own rate (typically 60Hz) at essentially no cost.
    //
    // SDL3's SDL_CreateRenderer dropped the driver-index and flags args
    // (renderers are always accelerated-if-available now, no
    // SDL_RENDERER_ACCELERATED flag to ask for it, and no reliable software
    // fallback to opt out of either) -- vsync is a separate call afterward.
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer) {
        const char* rn = SDL_GetRendererName(renderer);
        std::fprintf(stderr, "[dirk] renderer: %s\n", rn ? rn : "?");
    }
    if (!renderer) {
        std::fprintf(stderr, "[dirk] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    // No more first-run settings screen: dirk boots straight into the game
    // every time, using LaunchSettings' own defaults (see LaunchSettings.h)
    // the very first time a given game is launched, and whatever that
    // game's own KimmyScript "UI Options" menu last saved after that.
    // `settingsPath`/`settings` were already loaded at the very top of
    // main(), before this window/renderer existed, purely so
    // applyConsoleVisibility() could read hideConsole before deciding
    // whether to attach to a console at all.
    applyFullscreen(window, settings.fullscreen, disc.displayWidth(), disc.height());
    // sound effects (soundLoad/soundPlay) -- SDL3_mixer gain is a 0.0-1.0
    // float, replacing SDL2_mixer's 0-128 Mix_Volume(channel, vol) scale;
    // -1 (old "every channel") has no equivalent because there are no
    // channels any more -- MIX_SetMixerGain applies to the whole mixer.
    if (soundMixer) MIX_SetMixerGain(soundMixer, settings.volumeNonVldp / 100.0f);

    // Loading screen for the first-launch index build (see needsIndexBuild
    // above) -- shown only when that build is actually about to happen, so
    // an ordinary launch (cache already on disk, VideoSource::finishOpening()
    // below returns almost instantly) never sees it at all. Drawn AFTER the
    // fullscreen toggle just above, not before, so it already appears in
    // the player's actual window mode instead of flashing windowed-then-
    // fullscreen once finishOpening() completes. Both assets are optional
    // loose files resolved via resolveSharedAsset() (JoystickProfile.h) --
    // <sharedEngineRoot>/KimmyScript/Cfg/ preferred, bare sharedEngineRoot
    // as a fallback for an install that still has them at the old spot --
    // so a Dirk+KimmyScript install can be re-skinned without touching the
    // .exe; either or both missing just means less to draw, never a
    // failure -- this is cosmetic, not load-bearing, so it never blocks a
    // launch on its own.
    bool indexBuildOk = true;
    if (needsIndexBuild) {
        SDL_Texture* loadingTex = IMG_LoadTexture(renderer, dirk::resolveSharedAsset(sharedEngineRoot, "dirk_loading.png").c_str());
        TTF_Font* loadingFont = TTF_OpenFont(dirk::resolveSharedAsset(sharedEngineRoot, "dirk_loading_font.ttf").c_str(), 32);

        int outW = 0, outH = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &outW, &outH);

        // Image size, scaled to fit -- same aspect-preserving reasoning as
        // computeRenderRects() uses for the movie itself, capped at 40% of
        // the shorter output dimension so a large source image can never
        // dominate a small window.
        int dw = 0, dh = 0;
        if (loadingTex) {
            // SDL3 dropped SDL_QueryTexture entirely -- SDL_Texture now
            // exposes its own w/h as plain public (read-only) int fields.
            int texW = loadingTex->w, texH = loadingTex->h;
            const double maxSide = 0.4 * std::min(outW, outH);
            const double scale = std::min(1.0, maxSide / static_cast<double>(std::max(texW, texH)));
            dw = static_cast<int>(texW * scale);
            dh = static_cast<int>(texH * scale);
        }

        // "INDEXING..." is the widest the animated label below ever gets
        // (1-3 trailing dots) -- measuring it up front (TTF_SizeUTF8, no
        // actual rendering yet) fixes the label's box for the whole
        // animation, so the word itself never jitters side to side as the
        // dot count changes; only the space to its right fills in.
        int textW = 0, textH = 0;
        if (loadingFont) TTF_GetStringSize(loadingFont, "INDEXING...", 0, &textW, &textH);

        // Image + gap + text centered as ONE block, vertically, rather than
        // the image alone at the vertical center with text hanging below it.
        constexpr int kGap = 16;
        const int blockH = dh + (dh > 0 && textH > 0 ? kGap : 0) + textH;
        const int imageY = (outH - blockH) / 2;
        const int textY = imageY + dh + (dh > 0 && textH > 0 ? kGap : 0);
        const int textX = (outW - textW) / 2;

        // The actual (potentially slow) work, on its own thread -- see
        // VideoSource::finishOpening()'s comment -- so the loop below can
        // keep presenting an animated frame and pumping the event queue
        // (some window managers/OSes flag a window "not responding" if it
        // stops doing both for even a few seconds) the whole time it runs.
        // Safe: `disc` is touched by this one background thread only until
        // it's joined below, and the main thread here never calls anything
        // on it before that join -- no shared state, no lock needed.
        std::atomic<bool> indexDone{false};
        std::thread indexWorker([&]() {
            indexBuildOk = disc.finishOpening();
            indexDone.store(true);
        });

        const Uint32 animStartTicks = SDL_GetTicks();
        SDL_Color white{255, 255, 255, 255};
        // Controller "device added/removed" events seen while draining the
        // queue below, handed back to the main loop once indexing is done.
        // Bug found by Cyril (2026-09-26): on a game's FIRST launch (the
        // one that builds the index), controllers didn't work at all and
        // the F9 wizard didn't list them; quitting and relaunching fixed
        // it. SDL announces the controllers already plugged in at startup
        // with one SDL_EVENT_JOYSTICK_ADDED / SDL_EVENT_GAMEPAD_ADDED each,
        // and the main loop only opens a controller when it sees that
        // event -- this loop used to throw them away with everything else.
        // Reproduced headless with a virtual joystick: first launch
        // never opened it, a cached launch did.
        std::vector<SDL_Event> deferredDeviceEvents;
        while (!indexDone.load()) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                // Nothing else is actionable yet (no game/Lua state exists
                // this early) -- drain the queue so the OS sees the window
                // as alive. SDL_EVENT_QUIT is intentionally NOT honored here:
                // finishOpening() is already running on indexWorker and
                // has to be joined either way before any cleanup can touch
                // `disc` safely, so a quit request during this one-time
                // wait is a few seconds' delay at worst, not a hang.
                switch (ev.type) {
                case SDL_EVENT_JOYSTICK_ADDED:
                case SDL_EVENT_JOYSTICK_REMOVED:
                case SDL_EVENT_GAMEPAD_ADDED:
                case SDL_EVENT_GAMEPAD_REMOVED:
                case SDL_EVENT_GAMEPAD_REMAPPED:
                    deferredDeviceEvents.push_back(ev);
                    break;
                default:
                    break;
                }
            }

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            if (loadingTex) {
                SDL_Rect dst{(outW - dw) / 2, imageY, dw, dh};
                SDL_FRect dstF; SDL_RectToFRect(&dst, &dstF);
                SDL_RenderTexture(renderer, loadingTex, nullptr, &dstF);
            }
            if (loadingFont) {
                // 1-3 dots, ~400ms per step -- a plain cycling ellipsis
                // rather than a spinner, since it reads fine even redrawn
                // at vsync rate off a single small text re-render like
                // this, no separate asset needed.
                const int dots = 1 + ((SDL_GetTicks() - animStartTicks) / 400) % 3;
                const std::string label = "INDEXING" + std::string(dots, '.');
                SDL_Surface* textSurf = TTF_RenderText_Blended(loadingFont, label.c_str(), 0, white);
                if (textSurf) {
                    SDL_Texture* textTex = SDL_CreateTextureFromSurface(renderer, textSurf);
                    if (textTex) {
                        SDL_Rect dst{textX, textY, textSurf->w, textSurf->h};
                        SDL_FRect dstF; SDL_RectToFRect(&dst, &dstF);
                        SDL_RenderTexture(renderer, textTex, nullptr, &dstF);
                        SDL_DestroyTexture(textTex);
                    }
                    SDL_DestroySurface(textSurf);
                }
            }
            SDL_RenderPresent(renderer); // vsync-paced, same as the real main loop -- no busy spin

            // Belt-and-suspenders, same reasoning as the main loop's own
            // ~75fps cap further down: some driver/compositor combinations
            // don't actually block RenderPresent on vsync, and this one-time
            // wait has no other pacing of its own.
            SDL_Delay(1);
        }
        indexWorker.join();
        for (SDL_Event& ev : deferredDeviceEvents) SDL_PushEvent(&ev); // same order, see above

        if (loadingTex) SDL_DestroyTexture(loadingTex);
        if (loadingFont) TTF_CloseFont(loadingFont);
    } else {
        indexBuildOk = disc.finishOpening();
    }

    if (!indexBuildOk) {
        std::fprintf(stderr, "[dirk] failed to open video: %s\n", videoPath.c_str());
        return 1;
    }
    if (dirkVerbose()) {
        std::fprintf(stderr, "[dirk] opened %s: %dx%d, %lld frames indexed\n",
                     videoPath.c_str(), disc.width(), disc.height(),
                     static_cast<long long>(disc.frameCount()));
    }

    // Optional per-game bezel(s) (see Bezel.h) -- empty Bezel/ or none at
    // all is the normal case and silently leaves `bezels` empty, falling
    // back to the plain aspect-correct letterbox computed below.
    std::vector<dirk::Bezel> bezels = dirk::loadBezels(renderer, gameDataPath, scriptStem);
    // A bezel whose window doesn't have the movie's shape would squeeze the
    // picture (16:9 movie in a window cut for 4:3): left out for this
    // movie (see BezelFit.h). Before the saved index is checked, so the
    // numbering is the same at every launch of this game.
    if (disc.displayAspect() != disc.width() / static_cast<double>(std::max(1, disc.height())) ||
        disc.pixelShape() != "1:1") {
        std::fprintf(stderr, "[dirk] video shape: %dx%d with %s pixels, shown as %dx%d (%.3f)\n",
                     disc.width(), disc.height(), disc.pixelShape().c_str(), disc.displayWidth(),
                     disc.height(), disc.displayAspect());
    }
    for (auto it = bezels.begin(); it != bezels.end();) {
        if (!dirk::bezelWindowFitsMovie(it->cutoutW, it->cutoutH, disc.displayAspect())) {
            std::fprintf(stderr, "[dirk] bezel %s left out: its screen window is %dx%d (%.2f), the movie is %.2f\n",
                         it->name.c_str(), it->cutoutW, it->cutoutH,
                         it->cutoutW / static_cast<double>(it->cutoutH), disc.displayAspect());
            dirk::destroyBezel(*it);
            it = bezels.erase(it);
        } else {
            ++it;
        }
    }
    // settings.bezelIndex already reflects this game's own per-game
    // override at this point -- see peekPerGameDirkSettings(), called
    // just after gameDataPath was resolved, well above.
    // A stale settings.bezelIndex from a game.cfg saved when this game had
    // more bezels than it does now (files removed/renamed since) would
    // otherwise point past the end of `bezels` -- clamp back to "none"
    // rather than silently drawing nothing or reading out of range.
    if (settings.bezelIndex < 0 || settings.bezelIndex > static_cast<int>(bezels.size())) {
        settings.bezelIndex = 0;
    }
    // 1-based (0 = none) index into `bezels` -- nullptr when nothing is
    // selected, e.g. because dirkSetBezelIndex(0) chose "off" or there are
    // no bezels at all. Small enough (and re-derived every call, not
    // cached) that recomputing it a few times a frame is not worth the
    // bookkeeping a cached pointer would need across F11/menu changes.
    // .srt subtitles next to the movie (see core/Subtitles.h): Movie/<name>.srt,
    // Movie/<name>.fr.srt... Loaded once; settings.subtitleIndex (0 = off,
    // N = track #N) picks one, from this game's game.cfg or the UI Options
    // menu. A saved index past the end (a file removed since) falls back to
    // the first track rather than silently showing nothing.
    std::vector<dirk::SubtitleTrack> subtitleTracks = dirk::findSubtitleTracks(videoPath);
    if (subtitleTracks.empty() && !gameDataPath.empty()) {
        // A Hypseus game plays the Movie/<name>.mkv Dirk made from
        // Video/<video>.m2v -- its subtitles naturally sit next to the
        // .m2v and its .ogg soundtracks (Video/<video>-fr.srt), look there too.
        // The framefile whose .mkv is playing (several: two versions);
        // a multi-piece one has no single .m2v to look next to.
        for (const auto& ff : dirk::findHypseusFrameFiles(gameDataPath)) {
            if (ff.baseName == std::filesystem::path(videoPath).stem().string() && ff.segment.pieces.empty()) {
                subtitleTracks = dirk::findSubtitleTracks(ff.segment.videoPath);
            }
        }
    }
    if (!subtitleTracks.empty()) {
        if (settings.subtitleIndex < 0) settings.subtitleIndex = 0;
        if (settings.subtitleIndex > static_cast<int>(subtitleTracks.size())) settings.subtitleIndex = 1;
        std::string names;
        for (const auto& t : subtitleTracks) {
            names += (names.empty() ? "" : ", ") + t.label + " (" + std::to_string(t.cues.size()) + " lines)";
        }
        std::fprintf(stderr, "[dirk] subtitles: %s -- %s\n", names.c_str(),
                     settings.subtitleIndex == 0
                         ? "off"
                         : ("showing " + subtitleTracks[settings.subtitleIndex - 1].label + ", size " +
                            std::to_string(settings.subtitleSize) + "%").c_str());
    }
    auto activeSubtitleTrack = [&]() -> const dirk::SubtitleTrack* {
        if (settings.subtitleIndex >= 1 && settings.subtitleIndex <= static_cast<int>(subtitleTracks.size())) {
            return &subtitleTracks[settings.subtitleIndex - 1];
        }
        return nullptr;
    };
    dirk::SubtitleRenderer subtitleRenderer(dirk::resolveSharedAsset(sharedEngineRoot, "dirk_loading_font.ttf"));

    // Room for a bezel on the screen the window is on (BezelFit.h, rule 2):
    // updated every frame by updateBezelRoom(); while false no bezel is
    // drawn or reachable, and settings.bezelIndex (the saved choice) is
    // left alone. Logged when it changes.
    bool bezelRoom = true;
    int bezelRoomLogged = -1;
    auto updateBezelRoom = [&](int fallbackW, int fallbackH) {
        int sw = fallbackW, sh = fallbackH;
        if (const SDL_DisplayID did = SDL_GetDisplayForWindow(window)) {
            SDL_Rect b{};
            if (SDL_GetDisplayBounds(did, &b) && b.w > 0 && b.h > 0) { sw = b.w; sh = b.h; }
        }
        bezelRoom = dirk::screenHasRoomForBezel(sw, sh, disc.displayAspect());
        if (!bezels.empty() && bezelRoomLogged != (bezelRoom ? 1 : 0)) {
            bezelRoomLogged = bezelRoom ? 1 : 0;
            std::fprintf(stderr, bezelRoom ? "[dirk] bezel: available (screen %dx%d, picture %.2f)\n"
                                           : "[dirk] bezel: not available, no room beside the picture (screen %dx%d, picture %.2f)\n",
                         sw, sh, disc.displayAspect());
        }
    };
    {
        int w0 = 0, h0 = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &w0, &h0);
        updateBezelRoom(w0, h0); // known before the script's first frame
    }
    auto activeBezel = [&]() -> const dirk::Bezel* {
        if (!bezelRoom) return nullptr;
        if (settings.bezelIndex >= 1 && settings.bezelIndex <= static_cast<int>(bezels.size())) {
            return &bezels[settings.bezelIndex - 1];
        }
        return nullptr;
    };

    // Optional CRT-style scanline overlay -- plain on/off per the launch
    // settings, no intensity slider by design. Built as a texture at the
    // *movie's own* resolution (alternating transparent/semi-black rows)
    // rather than at the eventual on-screen size, so it scales up together
    // with the picture (and the bezel cutout, if any) instead of needing to
    // track a resizable output. Deliberately created with nearest-neighbor
    // filtering -- unlike videoTex/overlayTex/the bezel, which all want
    // smooth (linear, SDL3's default -- see above) scaling, a linearly-
    // filtered 1px-on/N-off pattern would just blur into a flat grey wash
    // once scaled up, instead of reading as lines -- so this one texture
    // gets its scale mode set explicitly to nearest, once, right after it's
    // created (SDL3 has no more global hint to toggle around one call --
    // SDL_SetTextureScaleMode is per-texture state).
    //
    // Factored into a lambda (rather than inline, one-shot code) because a
    // game's own KimmyScript UI Options menu (dirkSetScanlines, see the
    // launchSettingsDirty block in the main loop) can toggle this setting
    // live and needs to rebuild the texture on the spot.
    SDL_Texture* scanlineTex = nullptr;
    auto rebuildScanlineTexture = [&]() {
        if (scanlineTex) {
            SDL_DestroyTexture(scanlineTex);
            scanlineTex = nullptr;
        }
        if (!settings.scanlines) return;
        const int sw = disc.width();
        const int sh = disc.height();
        std::vector<uint8_t> pixels(static_cast<size_t>(sw) * sh * 4, 0);
        // Tuned by eye against several side-by-side alpha/spacing
        // combinations (the initial 1-line-in-2 pattern read as too heavy):
        // one dark line every 3 rows, at a light ~18% darkening.
        constexpr uint8_t kScanlineAlpha = 45;
        constexpr int kScanlinePeriod = 3;
        for (int y = 1; y < sh; y += kScanlinePeriod) {
            uint8_t* row = pixels.data() + static_cast<size_t>(y) * sw * 4;
            for (int x = 0; x < sw; ++x) {
                row[x * 4 + 3] = kScanlineAlpha; // RGB already zeroed above
            }
        }
        scanlineTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, sw, sh);
        if (scanlineTex) {
            SDL_SetTextureScaleMode(scanlineTex, SDL_SCALEMODE_NEAREST);
            SDL_UpdateTexture(scanlineTex, nullptr, pixels.data(), sw * 4);
            SDL_SetTextureBlendMode(scanlineTex, SDL_BLENDMODE_BLEND);
        } else {
            std::fprintf(stderr, "[dirk] warning: failed to create scanline texture: %s\n", SDL_GetError());
        }
    };
    rebuildScanlineTexture();

    // What the window/mixer/scanline texture/console/log currently reflect:
    // every one of them has been applied from `settings` by this point
    // (window fullscreen and mixer gain above, generateLog and hideConsole
    // at startup, the scanline texture just now), and every later change
    // goes through launchSettingsDirty. See that block for why this exists.
    dirk::LaunchSettings appliedSettings = settings;

    // Fullscreen ("fullscreen window" -- see LaunchSettings) puts the window
    // at the DESKTOP's resolution/aspect ratio, which essentially never
    // matches the movie's own (DLe is 1440x1080, 4:3-ish; a 21:9 monitor is
    // nowhere close) -- without correcting for that, SDL_RenderCopy(...,
    // nullptr, nullptr) below stretches the video to fill the mismatched
    // rect, exactly the behavior real Singe's -w avoided.
    //
    // SDL_RenderSetLogicalSize looks like the built-in answer, but testing
    // it here (isolated from the rest of the engine, same fullscreen-desktop
    // setup) showed SDL_RenderGetViewport() reporting the correct centered
    // viewport (e.g. x=560 for a 1440-wide picture on a 2560-wide screen)
    // while what actually got drawn ignored it and sat flush at x=0 --
    // SDL_RenderCopy's nullptr-dst shortcut not respecting the logical
    // viewport on this renderer/driver combination. So this computes and
    // passes an explicit destination rect every frame instead of relying on
    // that: verified, with the same isolated test, to land exactly where
    // expected (centered, correctly scaled). Recomputed each frame (cheap)
    // rather than once, since SDL_SetWindowFullscreen's real resize can
    // land asynchronously and a size read right after this call isn't
    // trustworthy yet.
    //
    // When a bezel is loaded, the same centered-and-scaled treatment is
    // applied to the *bezel* image instead of the bare game picture, and the
    // game picture is then placed into the bezel's own screen cutout
    // (scaled by the same factor) rather than being separately
    // aspect-corrected -- exactly like a real cabinet, where the picture
    // fills whatever cutout the bezel artist cut, not a second independent
    // letterbox inside it.
    struct RenderRects {
        SDL_Rect content;  // where videoTex/overlayTex are drawn
        SDL_Rect bezelDst; // where the bezel texture is drawn (only if hasBezel)
        bool hasBezel;
        SDL_Texture* bezelTexture; // the active bezel's texture (only if hasBezel)
    };
    // Display geometry, logged to dirk_log / dirk.log every time the render
    // output size changes (2026-10-01: a 21:9 screen under Batocera showed
    // the 4:3 picture + bezel stretched to the whole width -- what SDL
    // reports here is the first thing to compare with the real screen).
    int geomLoggedW = -1, geomLoggedH = -1;
    auto logDisplayGeometry = [&](int outW, int outH) {
        if (outW == geomLoggedW && outH == geomLoggedH) return;
        geomLoggedW = outW;
        geomLoggedH = outH;
        int winW = 0, winH = 0, pixW = 0, pixH = 0;
        SDL_GetWindowSize(window, &winW, &winH);
        SDL_GetWindowSizeInPixels(window, &pixW, &pixH);
        const SDL_DisplayID did = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode* desk = did ? SDL_GetDesktopDisplayMode(did) : nullptr;
        const SDL_DisplayMode* cur = did ? SDL_GetCurrentDisplayMode(did) : nullptr;
        const char* drv = SDL_GetCurrentVideoDriver();
        std::fprintf(stderr,
            "[dirk] display: driver=%s desktop=%dx%d current=%dx%d (density %.2f) window=%dx%d pixels=%dx%d "
            "render output=%dx%d fullscreen=%s video=%dx%d\n",
            drv ? drv : "?", desk ? desk->w : 0, desk ? desk->h : 0, cur ? cur->w : 0, cur ? cur->h : 0,
            cur ? cur->pixel_density : 0.0f, winW, winH, pixW, pixH, outW, outH,
            (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) ? "yes" : "no", disc.width(), disc.height());
    };
    auto computeRenderRects = [&]() -> RenderRects {
        int outW = 0, outH = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &outW, &outH);
        logDisplayGeometry(outW, outH);
        updateBezelRoom(outW, outH);

        if (const dirk::Bezel* bez = activeBezel()) {
            double scale = std::min(static_cast<double>(outW) / bez->width,
                                     static_cast<double>(outH) / bez->height);
            int bw = static_cast<int>(bez->width * scale);
            int bh = static_cast<int>(bez->height * scale);
            SDL_Rect bezelDst{(outW - bw) / 2, (outH - bh) / 2, bw, bh};
            SDL_Rect content{
                bezelDst.x + static_cast<int>(bez->cutoutX * scale),
                bezelDst.y + static_cast<int>(bez->cutoutY * scale),
                static_cast<int>(bez->cutoutW * scale),
                static_cast<int>(bez->cutoutH * scale)};
            return RenderRects{content, bezelDst, true, bez->texture};
        }

        // At the picture's real shape (VideoSource::displayAspect()).
        double scale = std::min(static_cast<double>(outW) / disc.displayWidth(),
                                 static_cast<double>(outH) / disc.height());
        int w = static_cast<int>(disc.displayWidth() * scale);
        int h = static_cast<int>(disc.height() * scale);
        SDL_Rect content{(outW - w) / 2, (outH - h) / 2, w, h};
        return RenderRects{content, SDL_Rect{0, 0, 0, 0}, false, nullptr};
    };

    // In the colour matrix + range this movie was actually encoded with
    // (review point 8) -- see VideoTexture.h. Logged once, unconditionally:
    // one line per launch, and the first thing to check if a game's
    // colours ever look off.
    SDL_Texture* videoTex = dirk::createVideoTexture(renderer, disc, disc.width(), disc.height());
    std::fprintf(stderr, "[dirk] video colours: %s\n", dirk::describeVideoColors(disc).c_str());
    std::fprintf(stderr, "[dirk] video decoding: %d thread(s) (decode_threads=%s)\n", disc.decodeThreads(),
                 settings.decodeThreads == 0 ? "auto" : std::to_string(settings.decodeThreads).c_str());

    SDL_Texture* overlayTex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
        disc.width(), disc.height());
    SDL_SetTextureBlendMode(overlayTex, SDL_BLENDMODE_BLEND);
    // Tracks the pixel size overlayTex was actually CREATED at, so the main
    // loop below can tell when it's gone stale against ctx.overlayWidth/
    // overlayHeight (see l_overlaySetResolution in api_singe.cpp) and needs
    // recreating -- see that check for why this matters.
    int overlayTexWidth = disc.width();
    int overlayTexHeight = disc.height();

    dirk::EngineContext ctx;
    ctx.disc = &disc;
    ctx.renderer = renderer;
    ctx.window = window;
    ctx.overlayWidth = disc.width();
    ctx.overlayHeight = disc.height();
    // See EngineContext::movieAudioStream -- lets api_singe.cpp's
    // fontLoad/spriteLoad/soundLoad freeze this stream for the duration of
    // their own blocking disk I/O, instead of it draining in real time
    // while the whole main loop (video decode included) is stalled on that
    // same call. Set unconditionally, including nullptr when there's no
    // movie audio stream at all (disc.hasAudio() was false, or opening it
    // failed) -- ScopedAudioFreeze already treats nullptr as "nothing to
    // pause".
    ctx.movieAudioStream = movieAudioStream;
    ctx.soundMixer = soundMixer; // see the MIX_Init()/MIX_CreateMixerDevice() call above
    // See EngineContext::launchSettings -- lets KimmyScript's own dirkGet/Set*
    // settings functions read and edit this exact struct one field at a
    // time (a game's own "UI Options" menu screen, say), and the native F10/
    // F11 hotkeys below do the same.
    ctx.launchSettings = &settings;
    ctx.bezelCount = static_cast<int>(bezels.size());
    ctx.bezelRoom = &bezelRoom;
    // Soundtracks (languages) of the movie -- see VideoSource::audioTrackCount().
    // A saved choice past the end (a file removed since) falls back to the
    // default track.
    if (settings.audioTrack > disc.audioTrackCount()) settings.audioTrack = 0;
    // The saved label wins over the saved number (tracks renumbered since,
    // e.g. <name>-de.ogg added before <name>-fr.ogg). Nothing has played
    // yet, so switching here needs no re-seek.
    // A game.cfg written before 2026-09-28 saved the tag ("FR", "IT") where
    // Dirk now shows and saves the name ("French"): the tag still finds it.
    if (!settings.audioTrackName.empty()) {
        const std::string legacyName = dirk::languageName(settings.audioTrackName);
        for (int t = 0; t < disc.audioTrackCount(); ++t) {
            const std::string label = disc.audioTrackLabel(t);
            if (label == settings.audioTrackName || (legacyName != "Unknown" && label == legacyName)) {
                if (t != disc.currentAudioTrack()) disc.selectAudioTrack(t);
                settings.audioTrack = t + 1;
                break;
            }
        }
    }
    appliedSettings.audioTrack = settings.audioTrack; // resolved, not a change to save
    if (disc.audioTrackCount() > 1) {
        std::string names;
        for (int t = 0; t < disc.audioTrackCount(); ++t) {
            names += (t ? ", " : "") + disc.audioTrackLabel(t);
        }
        std::fprintf(stderr, "[dirk] audio tracks: %s -- playing %s\n", names.c_str(),
                     disc.audioTrackLabel(disc.currentAudioTrack()).c_str());
    }
    for (const auto& t : subtitleTracks) ctx.subtitleLabels.push_back(t.label);

    dirk::LuaEngine lua;
    lua.open();
    dirk::registerSingeApi(lua.state(), &ctx);
    // Must run before any game script or controls.cfg loads -- see
    // InputMap.h's registerRawJoystickTables() doc comment. Unconditional,
    // same as SCANCODE/GAMEPAD_0..3 being defined whether or not that
    // physical hardware is actually plugged in.
    dirk::registerRawJoystickTables(lua.state());

    std::unordered_map<int, int> switchMap; // physical id (SDL scancode OR GAMEPAD_N.* id) -> SWITCH_* id, see InputMap.h
    // 2026-10-08 (Cyril): forced bindings, for Create.dirk (its B3, L1, R1
    // whatever the game's own controls.cfg says) -- see InputMap.h's
    // buildForcedSwitchMap(). Checked FIRST at every input resolution site
    // below, before the joystick profiles and switchMap. Empty unless the
    // script set the forcedControls global, so every game is unaffected.
    std::unordered_map<int, int> forcedMap;
    auto forcedSwitch = [&forcedMap](int physicalId) -> int {
        auto f = forcedMap.find(physicalId);
        return f == forcedMap.end() ? -1 : f->second;
    };

    // SWITCH_PAUSE's numeric id (framework.singe:489, and InputMap.cpp's
    // kBindings {"INPUT_PAUSE", 19}). Used below to natively toggle
    // ctx.discPaused -- actually freezing VideoSource's frame advance --
    // for ANY physical input controls.cfg binds to INPUT_PAUSE, not just
    // the keyboard. First shipped hardcoded to SDL_SCANCODE_P only (see the
    // history below), which is why "P marche mais pas le bouton" happened:
    // this game's controls.cfg binds INPUT_PAUSE to BOTH SCANCODE.P and
    // GAMEPAD_0.BUTTON_Y, but only the keyboard half went through the
    // hardcoded check -- the gamepad button only ever reached moves.singe's
    // onInputReleased, which (as documented at the old P-hotkey site) only
    // flips the script-side bPause flag and never touches discPaused. Since
    // a real arcade cabinet has no keyboard at all, the native freeze has to
    // follow whatever controls.cfg actually wired to INPUT_PAUSE, so this is
    // now keyed off the resolved SWITCH_* id instead of one hardcoded
    // scancode -- see the SDL_EVENT_KEY_DOWN and SDL_EVENT_GAMEPAD_BUTTON_DOWN cases
    // below, both of which call this on any switchMap hit that resolves to
    // SWITCH_PAUSE.
    constexpr int kSwitchIdPause = 19;

    // Whether the disc is CURRENTLY frozen specifically because this native
    // Pause hotkey put it there -- as opposed to ctx.discPaused being true
    // for a completely unrelated, script-driven reason (KimmyScript calls
    // discPause() constantly on its own: waiting on a timer between two
    // clips, or -- the real bug this was added to fix -- sitting on a fixed
    // menu screen like Options or Quit, which is exactly
    // `discSkipToFrame(frameOptions); discPause()` with no discPlay() ever
    // following it while the menu is up -- confirmed by reading the real,
    // delivered service.kimmy's own Options/Quit handlers). Before this
    // flag existed, every SWITCH_PAUSE press did a blind
    // `ctx.discPaused = !ctx.discPaused`, with no memory of WHY it was
    // already true -- so pressing Pause while parked on the Quit screen
    // cleared the script's own freeze and the movie started playing behind
    // the still-showing menu, exactly what Cyril found on the real cabinet.
    // Now: pressing Pause only ever freezes the picture when it wasn't
    // already frozen for some other reason (and remembers that this hotkey
    // is the one holding it), and only ever un-freezes a freeze THIS hotkey
    // itself created -- a freeze the script put there for its own reasons
    // is left exactly as the script left it, since this native code has no
    // way to know when the script would actually want it lifted.
    //
    // 2026-09-26 (review point 4): the script keeps running while the
    // native pause holds the picture (inputs, onOverlayUpdate), so it can
    // change the pause state itself in the meantime -- e.g. a seek
    // (discSkipToFrame auto-resumes) followed later by parking on a menu
    // (discSkipToFrame + discPause). nativePauseFreeze then went stale, and
    // the next Pause press un-froze the SCRIPT's menu freeze, playing the
    // movie behind it -- the very bug this flag was added for, through a
    // side door. Now the freeze remembers the script's pause-write counter
    // (EngineContext::discPauseScriptWrites) and only counts as "ours" if
    // the script hasn't touched the pause state since. Normal pause/unpause
    // is unaffected: no game script calls disc* from its own pause handling
    // (checked: KimmyScript, Mad Dog 1/2, Johnny Rock, Space Pirates, Drug
    // Wars, Crime Patrol only flip their own bPause flag).
    bool nativePauseFreeze = false;
    uint64_t nativePauseScriptWritesAtFreeze = 0;
    auto toggleNativePause = [&]() {
        if (nativePauseFreeze && ctx.discPauseScriptWrites != nativePauseScriptWritesAtFreeze) {
            nativePauseFreeze = false; // the script took over the pause state meanwhile -- its state stands
        }
        if (nativePauseFreeze) {
            ctx.discPaused = false;
            nativePauseFreeze = false;
        } else if (!ctx.discPaused) {
            ctx.discPaused = true;
            nativePauseFreeze = true;
            nativePauseScriptWritesAtFreeze = ctx.discPauseScriptWrites;
        }
        // else: already paused for a script-driven reason (a fixed menu,
        // a timed pause between clips) -- this press does nothing to the
        // picture, on purpose.
    };

    // One entry per currently-open SDL game controller, keyed by SDL's own
    // per-device SDL_JoystickID (stable for as long as that specific
    // controller stays connected, even if others are unplugged/replugged in
    // between -- unlike a plain device index, which SDL can reassign).
    // playerIndex (0-3) selects which of framework.singe's GAMEPAD_0..3
    // numeric ranges (500-532, 600-632, 700-732, 800-832) this controller's
    // events map into -- see kGamepadBase/kGamepadPlayerStride below.
    // axisNegPressed/axisPosPressed track each of the 6 analog axes' two
    // digital "direction" ids (e.g. AXIS_LEFT_X_L / AXIS_LEFT_X_R) as
    // pressed/not-pressed, purely so an axis crossing back and forth over
    // the digital threshold fires onInputPressed/onInputReleased exactly
    // once per crossing -- the same edge-triggered semantics SDL_EVENT_KEY_DOWN's
    // ev.key.repeat check already gives the keyboard path.
    struct GamepadSlot {
        SDL_Gamepad* controller = nullptr;
        SDL_JoystickID instanceId = -1;
        int playerIndex = 0;
        std::string guid; // SDL_GUIDToString() -- same GUID-keyed profile lookup RawJoySlot::guid already has, see runtimeProfiles below
        bool axisNegPressed[6] = {false, false, false, false, false, false};
        bool axisPosPressed[6] = {false, false, false, false, false, false};
    };
    std::vector<GamepadSlot> gamepads;

    // One entry per currently-open raw SDL_Joystick that SDL does NOT
    // recognize as a standard game controller (SDL_IsGamepad() ==
    // false) -- the generic-arcade-encoder case (see InputMap.h's
    // RAWJOY_0..3 doc comment for the full design rationale). Unlike
    // GamepadSlot's SDL_Gamepad + SDL_GameControllerButton/Axis
    // enums, a raw SDL_Joystick only gives numeric indices with no fixed
    // meaning, so every button/axis/hat is exposed generically through
    // controls.cfg's RAWJOY_<playerIndex>.* tables instead of a fixed
    // per-button enum. playerIndex (0-3) selects which RAWJOY_0..3 numeric
    // range (kRawJoyBase + playerIndex*kRawJoyPlayerStride) this joystick's
    // events map into. axisNeg/PosPressed and lastHatValue give the same
    // edge-triggered press/release semantics GamepadSlot already has for
    // its axes (see kRawJoyAxisDigitalThreshold below) and extend it to
    // hats, whose SDL value is a bitmask (up/right/down/left) rather than a
    // single float axis.
    struct RawJoySlot {
        SDL_Joystick* joystick = nullptr;
        SDL_JoystickID instanceId = -1;
        int playerIndex = 0;
        std::string guid; // SDL_GUIDToString() -- see runtimeProfiles below, the GUID-keyed lookup checked before switchMap
        bool axisNegPressed[dirk::kRawJoyMaxAxes] = {};
        bool axisPosPressed[dirk::kRawJoyMaxAxes] = {};
        Uint8 lastHatValue[dirk::kRawJoyMaxHats] = {};
    };
    std::vector<RawJoySlot> rawJoysticks;
    // See framework.singe:350-413 -- GAMEPAD_0's own numeric scheme this
    // mirrors exactly: 18 axis-direction ids (500-517, 3 per axis: neutral/
    // negative/positive -- see kGamepadAxisStride below) then 15 button ids
    // (518-532, BUTTON_A..DPAD_RIGHT) in precisely SDL_GameControllerButton's
    // own enum order, which is why kGamepadButtonOffset + the raw SDL button
    // enum value needs no translation table at all. GAMEPAD_1/2/3 are the
    // same scheme +100/+200/+300 (framework.singe's utilDeepCopy loop).
    constexpr int kGamepadBase = 500;
    constexpr int kGamepadPlayerStride = 100;
    constexpr int kGamepadAxisStride = 3;   // per axis: base, negative-direction, positive-direction
    constexpr int kGamepadButtonOffset = 18; // where BUTTON_A (axes' 18 ids) ends and buttons begin
    // Stick tilt / trigger pull needed before it counts as a "digital"
    // direction press -- roughly halfway, same idea as any analog-stick-as-
    // D-pad deadzone. SDL reports axes as a signed 16-bit range
    // (-32768..32767 for sticks; triggers report 0..32767 in practice, so
    // their "negative" id -- AXIS_*_TRIGGER_N -- simply never fires, which
    // matches controls.cfg only ever binding the _P variant).
    constexpr Sint16 kGamepadAxisDigitalThreshold = 16000;

    // ---- Script error reporting (2026-09-26, review point 2) -----------
    // Every Lua error path below (the game script's own load, every input/
    // overlay callback) goes through reportScriptError(): it feeds
    // dirk_error.txt (scriptErrors, see ScriptErrorLog.h), prints the Lua
    // call chain to stderr next to the existing one-line message, and --
    // the FIRST time a given error is seen, never on its repeats -- arms
    // the on-screen banner (drawn at the very end of each frame, on top of
    // everything, see drawErrorBanner below). The banner's font and text
    // textures are only created once a banner actually has to be shown:
    // an error-free session never loads or renders anything for this.
    constexpr Uint32 kErrorBannerMs = 8000;
    //
    // 2026-09-27, Cyril: the banner showed the error itself (file:line:
    // message) on a second line -- too detailed for players, and redundant
    // with dirk_error.txt, which has all of it plus the call chain. Now a
    // single fixed sentence pointing to the log.
    static const char* const kErrorBannerText = "Script error detected - see dirk_error.txt";
    Uint64 errorBannerUntilTicks = 0;   // 0 = no banner showing
    TTF_Font* errorBannerFont = nullptr;
    bool errorBannerFontTried = false;
    SDL_Texture* errorBannerTitleTex = nullptr; // rendered once, the text never changes

    auto destroyErrorBannerTextures = [&]() {
        if (errorBannerTitleTex) { SDL_DestroyTexture(errorBannerTitleTex); errorBannerTitleTex = nullptr; }
    };

    auto reportScriptError = [&](const std::string& message, const std::string& traceback) {
        if (message.empty()) return;
        const bool isNew = scriptErrors.report(message, traceback, SDL_GetTicks());
        if (!isNew) return; // a repeat: counted in dirk_error.txt, nothing else
        if (!traceback.empty()) std::fprintf(stderr, "[dirk] %s\n", traceback.c_str());
        if (settings.errorBanner) {
            errorBannerUntilTicks = SDL_GetTicks() + kErrorBannerMs;
        }
    };

    // Plain ASCII title on purpose: the SDL_RenderDebugText() fallback
    // (no TTF font available) only renders ASCII -- same constraint as
    // JoystickWizard's on-screen text, and English like it (Cyril's rule
    // for Dirk's own on-screen text).
    auto drawErrorBanner = [&](int outW, int outH) {
        (void)outH;
        const std::string title = kErrorBannerText;

        if (!errorBannerFontTried) {
            errorBannerFontTried = true;
            errorBannerFont = TTF_OpenFont(
                dirk::resolveSharedAsset(sharedEngineRoot, "dirk_loading_font.ttf").c_str(), 24);
        }
        // (Re)built on demand: freed when a banner times out, see the
        // main loop, and needed again if another new error shows up later.
        if (errorBannerFont && !errorBannerTitleTex) {
            SDL_Surface* surf = TTF_RenderText_Blended(errorBannerFont, title.c_str(), 0, SDL_Color{255, 255, 255, 255});
            if (surf) {
                errorBannerTitleTex = SDL_CreateTextureFromSurface(renderer, surf);
                SDL_DestroySurface(surf);
            }
        }

        constexpr float kMargin = 12.0f;
        SDL_BlendMode prevBlend = SDL_BLENDMODE_NONE;
        SDL_GetRenderDrawBlendMode(renderer, &prevBlend); // restored below -- don't leak state into the next frame
        if (errorBannerTitleTex) {
            // Wider than the window (small window) -> shrunk to fit rather
            // than cut off, aspect kept.
            const float maxW = std::max(1.0f, static_cast<float>(outW) - kMargin * 2);
            const float s = std::min(1.0f, maxW / static_cast<float>(errorBannerTitleTex->w));
            const float titleH = errorBannerTitleTex->h * s;
            SDL_FRect bar{0.0f, 0.0f, static_cast<float>(outW), kMargin * 2 + titleH};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 120, 0, 0, 215);
            SDL_RenderFillRect(renderer, &bar);
            SDL_FRect dst{kMargin, kMargin, errorBannerTitleTex->w * s, titleH};
            SDL_RenderTexture(renderer, errorBannerTitleTex, nullptr, &dst);
        } else {
            // No TTF font at all -- SDL's built-in 8px debug font, scaled
            // up to stay readable from a cabinet's viewing distance.
            constexpr float kScale = 2.0f;
            const float lineH = static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) + 4.0f;
            SDL_FRect bar{0.0f, 0.0f, static_cast<float>(outW), (kMargin * 2 + lineH) * kScale};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 120, 0, 0, 215);
            SDL_RenderFillRect(renderer, &bar);
            // Fixed-width 8px glyphs: cut each line to what fits across
            // the window (at this scale) instead of letting it run off.
            const size_t fitChars = static_cast<size_t>(std::max(
                4.0f, (static_cast<float>(outW) / kScale - kMargin * 2) / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE));
            auto fit = [fitChars](const std::string& s) {
                return s.size() <= fitChars ? s : s.substr(0, fitChars - 3) + "...";
            };
            SDL_SetRenderScale(renderer, kScale, kScale);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugText(renderer, kMargin, kMargin, fit(title).c_str());
            SDL_SetRenderScale(renderer, 1.0f, 1.0f); // never leak the scale into the rest of the frame
        }
        SDL_SetRenderDrawBlendMode(renderer, prevBlend);
    };

    if (scriptPath) {
        ctx.scriptPath = dirOf(scriptPath);
        ctx.dataPath = ctx.scriptPath;

        // Load the shared KimmyScript engine bootstrap ourselves, before the
        // game's own top-level script runs -- every game's launcher used to
        // need its own dofile("./KimmyScript/Script/globals.singe") line to
        // do this (see the README's ".singe/.kimmy rename compatibility"
        // section), which is now Dirk's job instead. Safe regardless of load
        // order: framework.kimmy (dofiled by globals.kimmy first thing)
        // derives its DIR global from dirkGetScriptPath(), which just reads
        // ctx.scriptPath above -- already set natively, in C++, before any
        // Lua at all has run -- not from anything the game's own script sets
        // up first. A game whose launcher still has the old dofile(globals)
        // line keeps working unmodified: the dofile() compat shim recognizes
        // it as the same file already loaded here and skips it (see
        // LuaEngine::runSharedEngineBootstrap).
        if (!lua.runSharedEngineBootstrap("./KimmyScript/Script/globals.kimmy")) {
            std::fprintf(stderr,
                "[dirk] warning: couldn't load the shared engine bootstrap: %s -- "
                "the game script will need its own dofile(globals...) line\n",
                lua.lastError().c_str());
        }

        if (wizardMode ? !lua.runWizardChunk() : !lua.runFile(scriptPath)) {
            std::fprintf(stderr, "[dirk] script error: %s\n", lua.lastError().c_str());
            reportScriptError(lua.lastError(), lua.lastTraceback());
        }
        // controls.cfg (now under Cfg/, see InputMap.h) must load *after*
        // the game script chain, since it uses framework.kimmy's SCANCODE/
        // GAMEPAD_0 tables (only defined once framework.kimmy has dofile'd)
        // to describe physical bindings -- and, for a game that sets the
        // real KimmyScript altCfg global, that ordering is also exactly
        // what lets buildSwitchMap() see its final value (see InputMap.cpp).
        switchMap = dirk::buildSwitchMap(lua.state(), ctx.scriptPath, sharedEngineRoot);
        forcedMap = dirk::buildForcedSwitchMap(lua.state(), sharedEngineRoot);
    }

    // GUID-keyed joystick profiles (see JoystickProfile.h), written by the
    // in-engine setup wizard below (F9) -- loaded once here, and consulted
    // FIRST for a raw joystick's button/hat/axis events, before falling
    // back to switchMap's own RAWJOY_N.* entries (see the
    // SDL_EVENT_JOYSTICK_BUTTON_DOWN/HAT_MOTION/AXIS_MOTION handling
    // below). Passed to the wizard BY REFERENCE (not a separate copy it
    // owns) so a profile it saves takes effect on the very next button
    // press in this same run, with no reload step -- see JoystickWizard.h.
    //
    // 2026-09-24 (Cyril: "je veux que dans le premier menu tu demandes si
    // on fait la configuration pour CE jeu ... ou en general"): TWO stores
    // now, not one. runtimeProfilesGame is this one game's own profile
    // (Cfg/dirk_joystick_profiles.cfg next to its game.cfg -- empty/unused
    // for a game that's never had a per-game profile saved, and for the
    // legacy bare-video launch mode with no game folder at all, ctx.
    // scriptPath.empty()). runtimeProfiles is unchanged from before this
    // existed: the shared, install-wide file (same fixed point as
    // dirk_settings.cfg). Every raw/gamepad input-resolution site below
    // uses findJoystickProfile() instead of querying either store
    // directly, so the actual lookup order (per-game first, then general,
    // then switchMap/controls.cfg -- same final fallback as always) lives
    // in exactly one place.
    dirk::JoystickProfileStore runtimeProfilesGame;
    if (!ctx.scriptPath.empty()) {
        runtimeProfilesGame.load(lua.state(), dirk::JoystickProfileStore::defaultPathForGame(ctx.scriptPath));
    }
    dirk::JoystickProfileStore runtimeProfiles;
    runtimeProfiles.load(lua.state(), dirk::JoystickProfileStore::defaultPath(sharedEngineRoot));
    auto findJoystickProfile = [&](const std::string& guid) -> const dirk::JoystickBinding* {
        if (const dirk::JoystickBinding* p = runtimeProfilesGame.find(guid)) return p;
        return runtimeProfiles.find(guid);
    };
    dirk::JoystickWizard wizard(sharedEngineRoot, ctx.scriptPath, runtimeProfilesGame, runtimeProfiles);
    // Whether the wizard itself is the one holding ctx.discPaused true --
    // same "remember why, don't blindly toggle" reasoning as
    // nativePauseFreeze above (toggleNativePause's own comment): a game
    // that happened to already be paused for its own reason (a fixed
    // menu, service.kimmy's timed pause) when F9 was pressed must stay
    // exactly as it was once the wizard closes, not get force-resumed.
    bool wizardPausedDisc = false;
    uint64_t wizardScriptWritesAtFreeze = 0; // same stale-flag guard as toggleNativePause (review point 4)
    // Undoes ONLY the freeze wizardPausedDisc itself put in place, the
    // instant the wizard actually closes -- shared by every place the
    // wizard can close, not just the ones driven by an SDL event (see each
    // call site's own comment). Must be called right after ANYTHING that
    // could change wizard.active() from true to false. If the script
    // changed the pause state while the wizard was open (onOverlayUpdate
    // keeps running under it), the script's state stands.
    auto restoreWizardPauseIfClosed = [&]() {
        if (!wizard.active() && wizardPausedDisc) {
            if (ctx.discPauseScriptWrites == wizardScriptWritesAtFreeze) ctx.discPaused = false;
            wizardPausedDisc = false;
        }
    };

    // Prime the first video frame so there's something on screen even
    // before the first Lua-driven overlay update.
    //
    // Confirmed by the new AUDIO UNDERRUN diagnostic on a real capture: the
    // movie audio device goes silent for ~77ms starting right at this exact
    // moment, closely matching a real, OBS-measured ~83ms (2 frames) AV lag
    // at startup that avOffsetMs itself couldn't see (see that diagnostic's
    // own comment above for why). The cause: this one priming call is the
    // ONLY thing that decodes any audio before video's own advance gets
    // released (videoAdvanceUnlockTicks below) -- decodeOneFrame()'s
    // catch-up loop is the sole path audio is ever produced through (see
    // VideoSource.h), and nothing calls it again until that hold lifts AND
    // a further frame period's worth of real time accumulates on top of
    // that. This call's normal catch-up target (kAudioLeadMarginSeconds,
    // 60ms -- sized for steady-state playback, where a fresh stepForward()
    // tops it back up roughly once a frame) isn't enough runway to survive
    // that whole gap, so the device runs dry partway through it.
    //
    // Fix: for this ONE priming call only, ask for enough decode-ahead to
    // comfortably outlast the gap -- the hold itself, plus one more frame
    // period for frameAccumulator to reach 1.0 once the hold lifts, plus a
    // safety margin -- then drop back to the normal small margin
    // immediately after, same as every call for the rest of the session.
    // This doesn't touch what's displayed: the catch-up loop only ever
    // decodes MORE AUDIO and sets aside any video packets it reads past
    // (pendingVideoPackets_) without decoding them into a frame, so frame 0
    // is still exactly what ends up on screen first -- just backed by a
    // deeper initial audio buffer this one time.
    const double startupAudioPrimeSeconds =
        audioHoldbackSeconds + (1.0 / (ctx.discFps > 0.0 ? ctx.discFps : 24.0)) + 0.03;
    disc.setAudioLeadSeconds(startupAudioPrimeSeconds);
    disc.stepForward();
    disc.setAudioLeadSeconds(audioLeadSecondsCurrent);
    // Diagnostic only -- the previous attempt at this fix (raising the
    // catch-up target for just this call) had ZERO measured effect on the
    // real underrun, which means either this target isn't what's actually
    // limiting how much gets decoded here, or something else is consuming/
    // resetting the buffer between here and the loop's first queueing. This
    // prints the requested target alongside what actually landed in
    // pendingAudio_ right now, before anything else touches it, so the next
    // step is diagnosis from real numbers instead of another guess.
    if (SDL_getenv("DIRK_DEBUG_TICK")) {
        std::fprintf(stderr,
            "[dirk] startup audio prime: requested target=%.1fms, discFps=%.3f -> "
            "audioDecodedSeconds()=%.1fms, pendingAudioBytes()=%zu (%.1fms of content)\n",
            startupAudioPrimeSeconds * 1000.0, ctx.discFps, disc.audioDecodedSeconds() * 1000.0,
            disc.pendingAudioBytes(),
            disc.audioSampleRate() > 0 && disc.audioChannels() > 0
                ? disc.pendingAudioBytes() * 1000.0 / (disc.audioSampleRate() * disc.audioChannels() * 2.0)
                : -1.0);
    }

    Uint32 lastTicks = SDL_GetTicks();
    const Uint32 engineLoopStartTicks = lastTicks; // fixed reference point for AUDIO UNDERRUN timestamps below, unlike lastTicks which mutates every tick
    double frameAccumulator = 0.0;
    // See audioHoldbackSeconds above (measured, not guessed) -- armed once
    // here, at true startup, and never again anywhere else in this loop.
    Uint32 videoAdvanceUnlockTicks = SDL_GetTicks() + static_cast<Uint32>(audioHoldbackSeconds * 1000.0);
    bool quit = false;
    bool debugShotRequested = false; // F6: dump a mid-run screenshot without exiting
    // F7 (see below): a completely different kind of measurement from
    // everything else this session has tried. Every fix so far has been
    // about Dirk's own internal bookkeeping (decode positions, queue
    // backlog, silence gaps after a seek) -- all of it self-consistent, and
    // all of it now measurably clean, yet a real, OBS-confirmed ~7-frame
    // lag at an ordinary in-game scene cut hasn't moved. That combination
    // means the remaining gap likely isn't IN that bookkeeping at all -- it
    // may be real output latency, between "SDL_QueueAudio was called" and
    // "actually audible", that's simply bigger than the ~22ms this project
    // has been trusting from a raw WASAPI query (AudioLatency.cpp), which
    // was never actually verified end-to-end through the real playback path
    // (SDL_QueueAudio -> DirectSound, per "driver=directsound" logged at
    // startup -- a real compatibility layer with its own latency
    // characteristics, not necessarily what a direct WASAPI query reports).
    // F7 queues a short, sharp test tone straight into movieAudioStream and
    // flashes the screen white, bypassing every bit of video-decode/seek
    // logic -- letting the same OBS+Premiere waveform method already used
    // all session measure PURE output latency directly, as a real number to
    // compare against that 22ms assumption. Originally a single-frame flash
    // (one SDL_RenderPresent), but a real capture showed the flash didn't
    // always even show up in the recording -- one ~16ms frame is thinner
    // than some capture pipelines' own sampling, so it can just get missed
    // between two captured frames. Held for a fixed real-time WINDOW now
    // instead (see debugClickFlashUntilTicks below), long enough that no
    // reasonable capture frame rate can step over it entirely, while still
    // being a single sharp on/off edge for lining up against the tone's own
    // sharp onset in a waveform.
    Uint32 debugClickFlashUntilTicks = 0; // 0 = not currently flashing
    // Tracks which decoded frame is currently sitting in videoTex, so the
    // main loop can skip re-uploading it to the GPU on iterations where the
    // picture hasn't actually changed -- see the uploadFrameToTexture() call
    // below. -2 (rather than -1, which VideoSource itself uses for "nothing
    // decoded yet") just guarantees the very first frame is always uploaded.
    int64_t lastUploadedFrame = -2;

    // callOnOverlayUpdate() is checked for errors below (see the dedicated
    // block further down), but the other Lua entry points -- onInputPressed
    // above all -- were being called with their bool return discarded, so a
    // script error partway through handling a button press was completely
    // invisible: lua_pcall catches it (no crash), but nothing ever printed
    // it, and whatever game-state update that callback didn't get to finish
    // (e.g. leaving lvlState/discPaused mid-transition) looks to the player
    // like a plain freeze with "no error" -- exactly the New Game menu
    // symptom this was added to diagnose. One dedup slot per callback name
    // so a repeating error (e.g. held input, mouse motion) logs once, not
    // every event.
    std::unordered_map<std::string, std::string> lastLoggedLuaError;
    auto logLuaCallbackError = [&](bool ok, const char* label) {
        if (ok || lua.lastError().empty()) return;
        std::string& last = lastLoggedLuaError[label];
        if (lua.lastError() != last) {
            std::fprintf(stderr, "[dirk] %s error: %s\n", label, lua.lastError().c_str());
            last = lua.lastError();
        }
        // Every occurrence, not just the stderr-deduped ones above:
        // dirk_error.txt counts repeats itself (and only writes/prints
        // anything the first time), see reportScriptError.
        reportScriptError(lua.lastError(), lua.lastTraceback());
    };

    // One entry point for every physical input that maps to a Singe switch
    // (keyboard, mouse, raw joystick button/hat/axis, gamepad button/axis).
    // Review point 4 (2026-09-26): the native Pause freeze used to be
    // wired into only 4 of the 9 dispatch paths -- Pause bound to a hat or
    // a stick axis paused the game's logic (the script's own SWITCH_PAUSE
    // handling) but not the picture. Routing every path through here keeps
    // them identical by construction.
    // Joystick wizard driven by the game's own controls (2026-10-03, Cyril:
    // "si on a pas de touche F9 c'est mort" -- handhelds and cabinets have
    // no keyboard). While the wizard shows a menu-like screen (not while it
    // records presses), an input event is first resolved by the normal
    // code below (controls.cfg, gamepad/joystick profiles) and the logical
    // switch it gives goes to the wizard instead of the game: up/down
    // move, action 1 = Enter, action 2 / exit menu = Escape (see
    // JoystickWizard::handleGameSwitch). An event that gives no switch is
    // then handed to the wizard itself as before (Enter/Escape keys, "press
    // a button on the device you want").
    bool wizardNavRouting = false;
    bool wizardNavTaken = false;
    auto dispatchSwitch = [&](int switchId, bool pressed) {
        if (wizardNavRouting) {
            wizardNavTaken = true;
            wizard.handleGameSwitch(switchId, pressed);
            return;
        }
        if (pressed) {
            logLuaCallbackError(lua.callOnInputPressed(switchId), "onInputPressed");
            if (switchId == kSwitchIdPause) toggleNativePause();
        } else {
            logLuaCallbackError(lua.callOnInputReleased(switchId), "onInputReleased");
        }
    };

    // Synthesize one onMouseMoved() call using SDL's real current cursor
    // position, right after the game script (and controls.cfg) have loaded
    // but before the main loop starts. Without this, a game whose own
    // mousex/mousey fields only ever get updated inside its own
    // onMouseMoved() callback (confirmed: Mad Dog McCree's maddog.dirk --
    // mousex/mousey are plain Lua globals, declared once near the top of the
    // script and left untouched until the first real SDL_EVENT_MOUSE_MOTION event)
    // starts every run with those stuck at whatever sentinel value the
    // script declared them with -- (0,0) for Mad Dog -- until the player's
    // first real mouse/gun movement. Confirmed harmful there specifically:
    // its reload option is configured dip_ReloadTrigger = DOPT_BORDERS
    // (reload fires when aiming off-screen, at the image borders), and
    // (0,0) reads exactly like "aiming at the top-left border" -- so the
    // very first trigger pull, before any real mouse movement, fired the
    // shot sound AND the reload sound together; every pull after the first
    // real move fired correctly, since mousex/mousey were by then somewhere
    // sane on-screen. Matches the user's own report exactly: shot+reload on
    // the very first click, shot-only afterward. callOnMouseMoved() is a
    // silent no-op for a game with no onMouseMoved() defined, so this is
    // safe for every other game too.
    {
        // SDL3's SDL_GetMouseState takes float* now, not int* -- truncated
        // back to int right away, same as every other on-screen pixel
        // coordinate in this file.
        float mouseX = 0, mouseY = 0;
        SDL_GetMouseState(&mouseX, &mouseY);
        logLuaCallbackError(lua.callOnMouseMoved(static_cast<int>(mouseX), static_cast<int>(mouseY), 0, 0, 0), "onMouseMoved");
    }

    // Shared with the DIRK_SCREENSHOT-at-exit block below.
    //
    // SDL3's SDL_RenderReadPixels signature changed shape, not just name: it
    // now allocates and returns a fresh SDL_Surface of its own (in the
    // renderer's native format) instead of writing into a caller-provided
    // pixel buffer -- so there's no separate SDL_CreateRGBSurfaceWithFormat
    // pre-allocation step to do here any more, and no output-size query
    // needed just for that (SDL_GetCurrentRenderOutputSize is gone from this
    // lambda entirely).
    auto takeScreenshot = [&](const char* shotPath) {
        SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
        if (shot) {
            IMG_SavePNG(shot, shotPath);
            std::fprintf(stderr, "[dirk] wrote screenshot: %s\n", shotPath);
            SDL_DestroySurface(shot);
        } else {
            std::fprintf(stderr, "[dirk] warning: SDL_RenderReadPixels failed: %s\n", SDL_GetError());
        }
    };

    // Opens the joystick wizard (F9, or the game's Dirk Options menu via
    // dirkOpenJoystickWizard) and freezes the picture while it's open.
    auto openJoystickWizard = [&]() {
        std::vector<dirk::JoystickWizard::DeviceInfo> devices;
        devices.reserve(rawJoysticks.size() + gamepads.size());
        for (const RawJoySlot& r : rawJoysticks) {
            const char* name = SDL_GetJoystickName(r.joystick);
            devices.push_back(dirk::JoystickWizard::DeviceInfo{
                r.instanceId, name ? name : "?", r.guid,
                dirk::JoystickWizard::DeviceInfo::Kind::Raw});
        }
        // Recognized gamepads are listed too (2026-09-23,
        // Cyril) -- lets a pad's own factory-default
        // mapping be overridden when it doesn't suit a
        // given game, not just an unrecognized device's
        // otherwise-unmapped input. See JoystickWizard.h's
        // 2026-09-23 comment for the full design.
        for (const GamepadSlot& g : gamepads) {
            const char* name = SDL_GetGamepadName(g.controller);
            devices.push_back(dirk::JoystickWizard::DeviceInfo{
                g.instanceId, name ? name : "?", g.guid,
                dirk::JoystickWizard::DeviceInfo::Kind::Gamepad});
        }
        wizard.open(lua.state(), devices);
        if (!ctx.discPaused) {
            ctx.discPaused = true;
            wizardPausedDisc = true;
            wizardScriptWritesAtFreeze = ctx.discPauseScriptWrites;
        }
    };

    if (settings.swapFaceButtons) {
        std::fprintf(stderr, "[dirk] gamepad face buttons swapped, A<->B and X<->Y (swap_face_buttons=1)\n");
    }
    while (!quit && !ctx.quitRequested) {
        Uint32 loopStart = SDL_GetTicks(); // paced against at the bottom of this loop, see below
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // swap_face_buttons=1 (see LaunchSettings::swapFaceButtons):
            // done first, so everything below sees the printed letter.
            if (settings.swapFaceButtons &&
                (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP)) {
                switch (ev.gbutton.button) {
                    case SDL_GAMEPAD_BUTTON_SOUTH: ev.gbutton.button = SDL_GAMEPAD_BUTTON_EAST; break;
                    case SDL_GAMEPAD_BUTTON_EAST:  ev.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH; break;
                    case SDL_GAMEPAD_BUTTON_WEST:  ev.gbutton.button = SDL_GAMEPAD_BUTTON_NORTH; break;
                    case SDL_GAMEPAD_BUTTON_NORTH: ev.gbutton.button = SDL_GAMEPAD_BUTTON_WEST; break;
                    default: break;
                }
            }
            // Joystick setup wizard (F9): while open, it claims every
            // keyboard and joystick/gamepad event outright (see
            // JoystickWizard::handleEvent) so nothing leaks into the
            // paused game underneath while a device is being configured.
            // Events it has no interest in (window/quit, hot-plug, mouse,
            // ...) fall through untouched to the normal switch below,
            // wizard open or not.
            wizardNavRouting = false;
            wizardNavTaken = false;
            const bool wizardInputEvent =
                ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP ||
                ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || ev.type == SDL_EVENT_JOYSTICK_BUTTON_UP ||
                ev.type == SDL_EVENT_JOYSTICK_HAT_MOTION || ev.type == SDL_EVENT_JOYSTICK_AXIS_MOTION ||
                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
                ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
            if (wizard.active() && wizard.takesGameControls() && wizardInputEvent &&
                !(ev.type == SDL_EVENT_KEY_DOWN && ev.key.repeat)) {
                wizardNavRouting = true; // see dispatchSwitch; finished after the switch below
            } else if (wizard.active()) {
                bool consumed = wizard.handleEvent(ev);
                // The wizard just closed (saved, cancelled via Echap/F9, a
                // device removed mid-capture, ...) -- undo only the freeze
                // IT put in place, same nuance as toggleNativePause: a
                // script-driven pause already in effect before F9 was
                // pressed is left exactly as it was. See
                // restoreWizardPauseIfClosed's own comment for the OTHER
                // way the wizard can close (the empty-device-list auto-
                // close timeout, which isn't driven by an event at all).
                restoreWizardPauseIfClosed();
                if (consumed) continue;
            }
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    quit = true;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (!ev.key.repeat) {
                        // onInputPressed(what) expects a logical SWITCH_*
                        // id (SWITCH_UP=0, SWITCH_COIN1=9, ...), NOT a raw
                        // SDL scancode -- confirmed by reading moves.kimmy's
                        // real onInputPressed, which compares against those
                        // constants. See src/lua/InputMap.h for the full
                        // story. A key with no controls.cfg binding fires
                        // nothing, same as an unconfigured physical input
                        // would on real hardware.
                        if (std::getenv("DIRK_DEBUG_INPUT")) {
                            std::fprintf(stderr, "[dirk] keydown scancode=%d\n", static_cast<int>(ev.key.scancode));
                        }
                        int keySwitch = forcedSwitch(static_cast<int>(ev.key.scancode));
                        if (keySwitch < 0) {
                            auto it = switchMap.find(static_cast<int>(ev.key.scancode));
                            if (it != switchMap.end()) keySwitch = it->second;
                        }
                        if (keySwitch >= 0) {
                            if (std::getenv("DIRK_DEBUG_INPUT")) {
                                std::fprintf(stderr, "[dirk] -> onInputPressed(%d)\n", keySwitch);
                            }
                            dispatchSwitch(keySwitch, true);
                        }
                        // F6: dump a screenshot right now without exiting --
                        // DIRK_SCREENSHOT alone only fires at clean exit,
                        // which isn't always reachable (no on-screen quit
                        // control, headless test rigs, etc). Harmless when
                        // unused; gated behind DIRK_DEBUG_SHOT so it never
                        // fires by accident.
                        if (ev.key.scancode == SDL_SCANCODE_F6 && std::getenv("DIRK_DEBUG_SHOT")) {
                            debugShotRequested = true;
                        }
                        // F7: pure output-latency test tone -- see
                        // debugClickFlashThisFrame's declaration above for
                        // why. Gated behind DIRK_DEBUG_CLICK, same pattern
                        // as F6/DIRK_DEBUG_SHOT, so it can never fire for a
                        // real player. Queues the tone and prints its
                        // timestamp immediately, right here -- not deferred
                        // to later in the loop -- so there's no extra frame
                        // of Dirk's own latency between the keypress and the
                        // sound actually being handed to SDL.
                        if (ev.key.scancode == SDL_SCANCODE_F7 && std::getenv("DIRK_DEBUG_CLICK")) {
                            // 200ms: see debugClickFlashUntilTicks's
                            // declaration above -- comfortably longer than
                            // one frame at any plausible capture rate, short
                            // enough to still read as a single sharp event
                            // when lined up against the tone in Premiere.
                            debugClickFlashUntilTicks = SDL_GetTicks() + 200;
                            if (movieAudioStream != nullptr) {
                                const int sampleRate = disc.audioSampleRate() > 0 ? disc.audioSampleRate() : 48000;
                                const int channels = disc.audioChannels() > 0 ? disc.audioChannels() : 2;
                                constexpr double kClickFreqHz = 2000.0;
                                constexpr double kClickDurationSeconds = 0.03;
                                const int numSamples = static_cast<int>(sampleRate * kClickDurationSeconds);
                                std::vector<int16_t> click(static_cast<size_t>(numSamples) * channels);
                                for (int i = 0; i < numSamples; ++i) {
                                    double t = static_cast<double>(i) / sampleRate;
                                    int16_t sample = (std::fmod(t * kClickFreqHz, 1.0) < 0.5) ? 24000 : -24000; // sharp square wave, loud but not clipping
                                    for (int c = 0; c < channels; ++c) {
                                        click[static_cast<size_t>(i) * channels + c] = sample;
                                    }
                                }
                                Uint32 queuedBeforeMs = 0;
                                double bytesPerSecond = sampleRate * channels * 2.0;
                                if (bytesPerSecond > 0) {
                                    queuedBeforeMs = static_cast<Uint32>(
                                        SDL_GetAudioStreamQueued(movieAudioStream) * 1000.0 / bytesPerSecond);
                                }
                                SDL_PutAudioStreamData(movieAudioStream, click.data(), static_cast<int>(click.size() * sizeof(int16_t)));
                                std::fprintf(stderr,
                                    "[dirk] TEST CLICK queued at t=%ums (queue backlog BEFORE this click was %ums -- "
                                    "pause the movie before pressing F7 so this reads ~0)\n",
                                    // SDL_GetTicks() returns Uint64 in SDL3 (was Uint32 in SDL2) --
                                    // cast back down for this ms-since-start diagnostic, which never
                                    // needs more than 32 bits' worth of milliseconds.
                                    static_cast<Uint32>(SDL_GetTicks() - engineLoopStartTicks), queuedBeforeMs);
                            }
                        }
                        // F11: quick bezel cycle, live, without going through
                        // any menu -- None -> bezel #1 -> #2 -> ... -> #N ->
                        // None -> ... (2026-09-23: generalized from a plain
                        // on/off toggle once a game could have more than one
                        // bezel to cycle through; a game with exactly one
                        // still behaves like the old on/off, just phrased as
                        // a 2-position cycle). Chosen deliberately outside
                        // the range any real KimmyScript controls.cfg would
                        // bind -- arcade control panels have no function-key
                        // row, and F6/F7 above are already reserved for debug
                        // aids -- so this never collides with a physical
                        // input a game script is listening for. A no-op for
                        // a game with no Bezel/ files at all (ctx.bezelCount
                        // == 0 / bezels.empty()), same as the equivalent
                        // dirkSetBezelIndex() call a game's own UI Options
                        // menu makes. Goes through the same
                        // launchSettingsDirty path as every other native
                        // LaunchSettings change (see below) so it re-applies
                        // and persists straight onto the current game's own
                        // game.cfg (persistPerGameDirkSettings(), see above)
                        // -- same file and same effect as if the change had
                        // come from that game's own in-script menu
                        // (service.kimmy's writeConfig()), just without
                        // requiring a trip through it. Never touches the
                        // shared dirk_settings.cfg any more (2026-09-22,
                        // Cyril -- that file is a factory default only now).
                        // (F10/F11 swapped 2026-09-21, on request.)
                        // No room for a bezel on this screen (BezelFit.h): F11
                        // does nothing, the saved choice stays.
                        if (ev.key.scancode == SDL_SCANCODE_F11 && !bezels.empty() && bezelRoom && !wizard.active()) {
                            settings.bezelIndex = settings.bezelIndex + 1;
                            if (settings.bezelIndex > static_cast<int>(bezels.size())) {
                                settings.bezelIndex = 0;
                            }
                            ctx.launchSettingsDirty = true;
                        }
                        // Native picture-freeze pause (P key, and now also
                        // any gamepad button controls.cfg binds to
                        // INPUT_PAUSE) is handled generically above, right
                        // where SWITCH_PAUSE gets resolved from switchMap --
                        // see kSwitchIdPause's declaration for the full
                        // history. This used to be a hardcoded
                        // SDL_SCANCODE_P check living here, independent of
                        // switchMap/controls.cfg entirely -- which is
                        // exactly why the gamepad's INPUT_PAUSE button
                        // (BUTTON_Y in this game's controls.cfg) never
                        // froze the picture: only the keyboard scancode was
                        // wired up. A real arcade cabinet has no keyboard,
                        // so this now has to follow whatever controls.cfg
                        // actually maps to INPUT_PAUSE, keyboard or not.
                        // F10: instant fullscreen/windowed toggle while the
                        // game runs, same convenience real Singe/Hypseus
                        // offer and players expect from just about any PC
                        // game/browser -- doesn't require going through any
                        // menu for something this common. Replaces the old
                        // Alt+Enter binding outright (not in addition to it)
                        // -- picked for the same "arcade control panel has
                        // no function-key row" reason F11/F6/F7 already are.
                        // (F10/F11 swapped 2026-09-21, on request.)
                        if (ev.key.scancode == SDL_SCANCODE_F10 && !wizard.active()) {
                            settings.fullscreen = !settings.fullscreen;
                            ctx.launchSettingsDirty = true;
                        }
                        // F12: quick scanlines on/off toggle, live, same
                        // idea and same launchSettingsDirty path as F11's
                        // bezel toggle right above -- request was
                        // specifically for this to "stay in sync with the
                        // menu", which it does for free: dirkGetScanlines()
                        // (api_singe.cpp) reads settings.scanlines straight
                        // off this same struct, so a game's own UI Options
                        // screen already shows whatever F12 last set the
                        // next time it's opened, no separate wiring needed.
                        // Setting the dirty flag is also what makes the
                        // *visual* toggle actually happen: it's what drives
                        // rebuildScanlineTexture() further down in this same
                        // loop, exactly like a menu-driven dirkSetScanlines()
                        // call already does -- see that lambda's own comment
                        // for why a rebuild (not just flipping the bool) is
                        // needed to show/hide it live. Also persists onto
                        // the current game's own game.cfg the same way
                        // F10/F11 do (persistPerGameDirkSettings(), see
                        // above) -- never to the shared dirk_settings.cfg
                        // any more (2026-09-22, Cyril -- factory default
                        // only). SDL_SCANCODE_F12 has no
                        // native meaning elsewhere in dirk (F6/F7 debug aids,
                        // F10 fullscreen, F11 bezel) -- controls.cfg happens
                        // to list SCANCODE.F12 under INPUT_SCREENSHOT
                        // (SWITCH_SCREENSHOT), but that's a separate, purely
                        // Lua-side switch nothing currently reads (confirmed
                        // by grep), so there's no real collision, just the
                        // same physical key also being labelled "Screenshot"
                        // in that file.
                        if (ev.key.scancode == SDL_SCANCODE_F12 && !wizard.active()) {
                            settings.scanlines = !settings.scanlines;
                            ctx.launchSettingsDirty = true;
                        }
                        // F9: opens the native joystick setup wizard (see
                        // JoystickWizard.h) -- lets an installer bind an
                        // unrecognized raw joystick's buttons/axes/hats to
                        // a saved, GUID-keyed profile without touching
                        // controls.cfg by hand. Free like F6-F8 (no real
                        // KimmyScript controls.cfg binds a function key),
                        // and the exact hotkey JoystickProfile.cpp's own
                        // save() header comment already assumed before
                        // this wizard existed. wizard.active() guards
                        // against reopening it on top of itself if F9 is
                        // held/repeated (repeats are filtered above, but
                        // this is also reached on the very keydown that
                        // opens it) -- and, symmetrically, means a SECOND
                        // F9 press while the wizard is already open never
                        // reaches this block at all (it's consumed by
                        // wizard.handleEvent() above, same as every other
                        // keydown while active()); JoystickWizard::
                        // handleKeyDown's own 2026-09-24 comment is where
                        // that second press is actually handled (normalized
                        // to Escape, so F9 now closes the wizard exactly
                        // like Escape does at whatever step it's on).
                        if (ev.key.scancode == SDL_SCANCODE_F9 && !wizard.active()) {
                            openJoystickWizard();
                        }
                        // +/-: quick volume nudge, 10 points at a time,
                        // live while the game runs -- both volumes move
                        // together (Cyril: "make + and - raise/lower both
                        // volumes by 10%, obviously clamped to 0..100, even
                        // though the two aren't necessarily set the same"),
                        // each clamped independently against its OWN
                        // current value (dirk::nudgeVolumes(), see
                        // LaunchSettings.h/.cpp -- also unit-tested there,
                        // see tools/volume_nudge_test.cpp) rather than
                        // being forced to match each other. Two scancodes
                        // bound per direction -- the numpad +/-
                        // (SDL_SCANCODE_KP_PLUS/MINUS) and the main-row
                        // =/- key (SDL_SCANCODE_EQUALS/MINUS, the physical
                        // key '+' lives on with Shift on a standard
                        // layout) -- so this works regardless of which one
                        // a given keyboard/cabinet setup has or labels
                        // differently. Same launchSettingsDirty path as
                        // every other native settings hotkey (F10/F11/F12
                        // above): persists onto the CURRENT game's own
                        // game.cfg (persistPerGameDirkSettings(), never the
                        // shared dirk_settings.cfg) and applies live
                        // (MIX_SetMixerGain for volumeNonVldp; volumeVldp
                        // needs no extra apply call here -- see that
                        // field's own comment further down, read live from
                        // applyVolume() on every decoded audio packet).
                        if ((ev.key.scancode == SDL_SCANCODE_KP_PLUS || ev.key.scancode == SDL_SCANCODE_EQUALS) && !wizard.active()) {
                            dirk::nudgeVolumes(settings, 10);
                            ctx.launchSettingsDirty = true;
                        }
                        if ((ev.key.scancode == SDL_SCANCODE_KP_MINUS || ev.key.scancode == SDL_SCANCODE_MINUS) && !wizard.active()) {
                            dirk::nudgeVolumes(settings, -10);
                            ctx.launchSettingsDirty = true;
                        }
                    }
                    break;
                case SDL_EVENT_KEY_UP: {
                    int keySwitch = forcedSwitch(static_cast<int>(ev.key.scancode));
                    if (keySwitch < 0) {
                        auto it = switchMap.find(static_cast<int>(ev.key.scancode));
                        if (it != switchMap.end()) keySwitch = it->second;
                    }
                    if (keySwitch >= 0) {
                        dispatchSwitch(keySwitch, false);
                    }
                    break;
                }
                // SDL enumerates every controller already connected at
                // SDL_INIT_GAMECONTROLLER time as a DEVICEADDED event too
                // (not just true hot-plugs), so this alone covers "joystick
                // was plugged in before Dirk started" as well as plugging
                // one in mid-session -- no separate startup enumeration
                // needed. cdevice.which here is a DEVICE INDEX (not yet a
                // stable instance id -- that only exists once opened, below).
                // SDL fires this for EVERY joystick, including ones that are
                // ALSO recognized as a "game controller" (SDL_EVENT_GAMEPAD_ADDED
                // fires too for those -- handled below, this case then does
                // nothing for it). The ones that matter here are the ones
                // NOT recognized: SDL_IsGamepad() is true only for a
                // device SDL has a button/axis mapping for (its own built-in
                // community database, or one loaded at runtime) -- a
                // DirectInput arcade encoder Windows itself sees as a valid
                // gamepad can still fail this check if that specific
                // hardware isn't in that database, and SDL_EVENT_GAMEPAD_ADDED
                // simply never fires for it: no crash, no error, just
                // silence, which is indistinguishable from "nothing plugged
                // in" without this. Logged so that's visible immediately
                // instead of discovered as "the joystick does nothing" on
                // the real cabinet with no clue why -- see this session's
                // conversation about XInput vs DirectInput protocol mattering
                // here. Driven through the generic RAWJOY_0..3 raw-index
                // path (see InputMap.h) rather than a per-GUID
                // SDL_GameControllerAddMapping(), so this same code handles
                // any future unrecognized joystick, not just the specific
                // hardware this was built and verified against.
                case SDL_EVENT_JOYSTICK_ADDED: {
                    if (SDL_IsGamepad(ev.jdevice.which)) break; // handled by CONTROLLERDEVICEADDED below
                    SDL_Joystick* joy = SDL_OpenJoystick(ev.jdevice.which);
                    if (!joy) break;
                    char guidStr[64];
                    SDL_GUIDToString(SDL_GetJoystickGUID(joy), guidStr, sizeof(guidStr));
                    // Two identical raw joysticks (same model of cheap
                    // arcade encoder board -- the normal case for a 2-
                    // player cabinet, e.g. Cyril's) can report a BYTE-
                    // IDENTICAL GUID -- confirmed for real via
                    // SDL_AttachVirtualJoystick with matching vendor_id/
                    // product_id/name, not assumed (2026-09-24, after
                    // Cyril: "je ne suis pas sur que les deux joystick de
                    // l'arcade aient ete reconnus separement... pas tous
                    // les deux dans le fichier config apres un save" --
                    // exactly this: the second one's save silently
                    // overwrote the first's entry under the same guid
                    // key). SDL_GetJoystickPath() gives the OS-level
                    // device path instead, which two boards on two
                    // different USB ports do NOT share even when their
                    // GUID matches -- see buildProfileKey()'s own comment
                    // in JoystickProfile.h for the full design, including
                    // why this stays 100% backward compatible with every
                    // already-saved plain-GUID profile.
                    const char* path = SDL_GetJoystickPath(joy);
                    std::string profileKey = dirk::buildProfileKey(guidStr, path);
                    std::fprintf(stderr,
                        "[dirk] joystick connected but not recognized as a standard game controller: "
                        "\"%s\" (GUID %s, path %s, %d buttons, %d axes, %d hats) -- driving it through "
                        "the generic RAWJOY_N raw-index input path; set DIRK_DEBUG_JOYSTICK=1 to print "
                        "each raw button/axis/hat index as you press/move it (needed to write its "
                        "controls.cfg bindings)\n",
                        SDL_GetJoystickName(joy) ? SDL_GetJoystickName(joy) : "?", guidStr,
                        path ? path : "(aucun)",
                        SDL_GetNumJoystickButtons(joy), SDL_GetNumJoystickAxes(joy), SDL_GetNumJoystickHats(joy));
                    bool slotTaken[4] = {false, false, false, false};
                    for (const RawJoySlot& r : rawJoysticks) slotTaken[r.playerIndex] = true;
                    int playerIndex = 0;
                    while (playerIndex < 4 && slotTaken[playerIndex]) ++playerIndex;
                    if (playerIndex >= 4) {
                        std::fprintf(stderr,
                            "[dirk] all 4 RAWJOY_0..3 slots are already in use -- \"%s\" won't drive any input\n",
                            SDL_GetJoystickName(joy) ? SDL_GetJoystickName(joy) : "?");
                        SDL_CloseJoystick(joy);
                        break;
                    }
                    RawJoySlot slot;
                    slot.joystick = joy;
                    slot.instanceId = SDL_GetJoystickID(joy);
                    slot.playerIndex = playerIndex;
                    slot.guid = profileKey; // possibly "<guid>#<path>" -- see comment above
                    rawJoysticks.push_back(slot);
                    if (dirkVerbose()) {
                        std::fprintf(stderr, "[dirk] raw joystick connected: \"%s\" -> RAWJOY_%d\n",
                                     SDL_GetJoystickName(joy) ? SDL_GetJoystickName(joy) : "?", playerIndex);
                    }
                    if (std::getenv("DIRK_DEBUG_JOYSTICK")) {
                        std::fprintf(stderr,
                            "[dirk] DIRK_DEBUG_JOYSTICK: press each physical button/stick direction "
                            "one at a time and watch for the raw button/axis/hat index it fires below\n");
                    }
                    break;
                }
                case SDL_EVENT_JOYSTICK_REMOVED: {
                    // If the joystick wizard (F9) is mid-capture on exactly
                    // this device, unplugging it would otherwise leave the
                    // wizard stuck waiting forever on input that can never
                    // come -- cancel cleanly instead (nothing gets saved,
                    // same as any other Echap-cancel) rather than hanging.
                    if (wizard.isUsingDevice(ev.jdevice.which)) {
                        std::fprintf(stderr, "[dirk] joystick wizard: appareil deconnecte pendant la configuration, annulation\n");
                        wizard.cancel();
                        // A THIRD way the wizard can close, distinct from
                        // both the ones restoreWizardPauseIfClosed's own
                        // comment already covers (the caller of THIS
                        // switch statement already ran its own event-loop
                        // restore check earlier for THIS event, before the
                        // cancel() right above ever happened -- so without
                        // this, unplugging the device being configured
                        // would leave ctx.discPaused stuck true forever,
                        // same bug class as Cyril's 2026-09-24 report just
                        // above, just a different trigger).
                        restoreWizardPauseIfClosed();
                    }
                    // Instance id here (ev.jdevice.which), unlike ADDED
                    // above where it's a device index -- same ADDED-vs-
                    // REMOVED inconsistency SDL_CONTROLLERDEVICE* has (see
                    // CONTROLLERDEVICEREMOVED's own comment below). A
                    // joystick that WAS closed right away above (all 4 raw
                    // slots already taken) was never added to rawJoysticks,
                    // so this loop simply finds nothing and no-ops for it.
                    for (size_t i = 0; i < rawJoysticks.size(); ++i) {
                        if (rawJoysticks[i].instanceId == ev.jdevice.which) {
                            if (dirkVerbose()) {
                                std::fprintf(stderr, "[dirk] raw joystick disconnected: RAWJOY_%d\n",
                                             rawJoysticks[i].playerIndex);
                            }
                            SDL_CloseJoystick(rawJoysticks[i].joystick);
                            rawJoysticks.erase(rawJoysticks.begin() + static_cast<long>(i));
                            break;
                        }
                    }
                    break;
                }
                case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
                case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                    // DIRK_DEBUG_JOYSTICK printing stays available even once
                    // this path is "live" -- it's how any future unrecognized
                    // joystick gets its raw indices read off for
                    // controls.cfg, same as this hardware's were.
                    if (std::getenv("DIRK_DEBUG_JOYSTICK")) {
                        std::fprintf(stderr, "[dirk] DIRK_DEBUG_JOYSTICK: joystick %d button %d %s\n",
                                     ev.jbutton.which, ev.jbutton.button,
                                     ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ? "DOWN" : "up");
                    }
                    if (ev.jbutton.button >= dirk::kRawJoyMaxButtons) break;
                    for (const RawJoySlot& r : rawJoysticks) {
                        if (r.instanceId != ev.jbutton.which) continue;
                        // GUID-keyed profile (see JoystickProfile.h) checked
                        // FIRST -- only a device the wizard has actually
                        // configured has one, so this is a no-op fallthrough
                        // to the existing RAWJOY_N.*/switchMap path for
                        // every device it's never touched, exactly as
                        // before this existed.
                        int physicalId = dirk::kRawJoyBase + r.playerIndex * dirk::kRawJoyPlayerStride
                                         + dirk::kRawJoyButtonOffset + ev.jbutton.button;
                        int switchId = forcedSwitch(physicalId);
                        if (switchId < 0) {
                            if (const dirk::JoystickBinding* prof = findJoystickProfile(r.guid)) {
                                auto bit = prof->buttons.find(ev.jbutton.button);
                                if (bit != prof->buttons.end()) switchId = bit->second;
                            }
                        }
                        if (switchId < 0) {
                            auto it = switchMap.find(physicalId);
                            if (it != switchMap.end()) switchId = it->second;
                        }
                        if (switchId < 0) break;
                        dispatchSwitch(switchId, ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                        break;
                    }
                    break;
                }
                case SDL_EVENT_JOYSTICK_HAT_MOTION: {
                    if (std::getenv("DIRK_DEBUG_JOYSTICK") && ev.jhat.value != SDL_HAT_CENTERED) {
                        std::fprintf(stderr,
                            "[dirk] DIRK_DEBUG_JOYSTICK: joystick %d hat %d = %d "
                            "(bitmask: up=1 right=2 down=4 left=8, e.g. 9 = up+left)\n",
                            ev.jhat.which, ev.jhat.hat, ev.jhat.value);
                    }
                    if (ev.jhat.hat >= dirk::kRawJoyMaxHats) break;
                    for (RawJoySlot& r : rawJoysticks) {
                        if (r.instanceId != ev.jhat.which) continue;
                        int hat = ev.jhat.hat;
                        Uint8 oldVal = r.lastHatValue[hat];
                        Uint8 newVal = ev.jhat.value;
                        r.lastHatValue[hat] = newVal;
                        // Direction order must match InputMap.h's
                        // HAT_<hat>_UP/RIGHT/DOWN/LEFT field order (dir 0-3).
                        static constexpr Uint8 kHatBits[4] = {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT};
                        const dirk::JoystickBinding* prof = findJoystickProfile(r.guid);
                        for (int dir = 0; dir < 4; ++dir) {
                            bool wasOn = (oldVal & kHatBits[dir]) != 0;
                            bool isOn = (newVal & kHatBits[dir]) != 0;
                            if (wasOn == isOn) continue; // edge-triggered, same reasoning as axes below
                            // Profile checked first, same as the button
                            // case above -- see JoystickProfile.h.
                            int hatId = dirk::kRawJoyBase + r.playerIndex * dirk::kRawJoyPlayerStride
                                        + dirk::kRawJoyHatOffset + hat * 4 + dir;
                            int switchId = forcedSwitch(hatId);
                            if (prof && switchId < 0) {
                                auto hit = prof->hats.find(hat);
                                if (hit != prof->hats.end()) {
                                    switchId = (dir == 0) ? hit->second.up
                                             : (dir == 1) ? hit->second.right
                                             : (dir == 2) ? hit->second.down
                                                          : hit->second.left;
                                }
                            }
                            if (switchId < 0) {
                                auto it = switchMap.find(hatId);
                                if (it != switchMap.end()) switchId = it->second;
                            }
                            if (switchId < 0) continue;
                            dispatchSwitch(switchId, isOn);
                        }
                        break;
                    }
                    break;
                }
                case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
                    // Thresholded the same way the recognized-gamepad path
                    // is (kGamepadAxisDigitalThreshold) and only printed on
                    // a crossing, not every event -- an idle analog axis
                    // still sends a steady stream of near-zero motion
                    // events, which would otherwise flood the console and
                    // bury the one line that actually matters.
                    if (std::getenv("DIRK_DEBUG_JOYSTICK") &&
                        (ev.jaxis.value >= kGamepadAxisDigitalThreshold ||
                         ev.jaxis.value <= -kGamepadAxisDigitalThreshold)) {
                        std::fprintf(stderr, "[dirk] DIRK_DEBUG_JOYSTICK: joystick %d axis %d = %d\n",
                                     ev.jaxis.which, ev.jaxis.axis, ev.jaxis.value);
                    }
                    if (ev.jaxis.axis >= dirk::kRawJoyMaxAxes) break;
                    for (RawJoySlot& r : rawJoysticks) {
                        if (r.instanceId != ev.jaxis.which) continue;
                        int axis = ev.jaxis.axis;
                        int negId = dirk::kRawJoyBase + r.playerIndex * dirk::kRawJoyPlayerStride
                                    + axis * dirk::kRawJoyAxisStride + 1;
                        int posId = dirk::kRawJoyBase + r.playerIndex * dirk::kRawJoyPlayerStride
                                    + axis * dirk::kRawJoyAxisStride + 2;
                        const dirk::JoystickBinding* prof = findJoystickProfile(r.guid);
                        bool newNeg = ev.jaxis.value <= -kGamepadAxisDigitalThreshold;
                        bool newPos = ev.jaxis.value >= kGamepadAxisDigitalThreshold;
                        if (newNeg != r.axisNegPressed[axis]) {
                            r.axisNegPressed[axis] = newNeg;
                            // Profile checked first, same as button/hat above.
                            int switchId = forcedSwitch(negId);
                            if (prof && switchId < 0) {
                                auto ait = prof->axes.find(axis);
                                if (ait != prof->axes.end()) switchId = ait->second.neg;
                            }
                            if (switchId < 0) {
                                auto it = switchMap.find(negId);
                                if (it != switchMap.end()) switchId = it->second;
                            }
                            if (switchId >= 0) {
                                dispatchSwitch(switchId, newNeg);
                            }
                        }
                        if (newPos != r.axisPosPressed[axis]) {
                            r.axisPosPressed[axis] = newPos;
                            int switchId = forcedSwitch(posId);
                            if (prof && switchId < 0) {
                                auto ait = prof->axes.find(axis);
                                if (ait != prof->axes.end()) switchId = ait->second.pos;
                            }
                            if (switchId < 0) {
                                auto it = switchMap.find(posId);
                                if (it != switchMap.end()) switchId = it->second;
                            }
                            if (switchId >= 0) {
                                dispatchSwitch(switchId, newPos);
                            }
                        }
                        break;
                    }
                    break;
                }
                case SDL_EVENT_GAMEPAD_ADDED: {
                    if (gamepads.size() >= 4) break; // framework.singe only defines GAMEPAD_0..3
                    SDL_Gamepad* controller = SDL_OpenGamepad(ev.gdevice.which);
                    if (!controller) {
                        std::fprintf(stderr, "[dirk] SDL_OpenGamepad failed: %s\n", SDL_GetError());
                        break;
                    }
                    SDL_Joystick* joy = SDL_GetGamepadJoystick(controller);
                    SDL_JoystickID instanceId = SDL_GetJoystickID(joy);
                    // First unused player slot 0-3, not necessarily
                    // gamepads.size() -- a controller unplugged and
                    // replugged, or unplugged out of order, must not collide
                    // with a slot still held by another connected pad.
                    bool slotTaken[4] = {false, false, false, false};
                    for (const GamepadSlot& g : gamepads) slotTaken[g.playerIndex] = true;
                    int playerIndex = 0;
                    while (playerIndex < 4 && slotTaken[playerIndex]) ++playerIndex;
                    GamepadSlot slot;
                    slot.controller = controller;
                    slot.instanceId = instanceId;
                    slot.playerIndex = playerIndex;
                    // Same GUID computation as RawJoySlot's -- lets the
                    // joystick wizard (F9) and the profile-first checks in
                    // GAMEPAD_BUTTON_DOWN/UP/AXIS_MOTION below tell this
                    // exact physical pad apart from any other, the same way
                    // they already do for raw joysticks (2026-09-23, Cyril:
                    // "remapper ses touches si le mapping par defaut ne
                    // convient pas" -- a recognized gamepad can now also be
                    // walked through the wizard, see JoystickWizard.h).
                    char guidStr[64];
                    SDL_GUIDToString(SDL_GetJoystickGUID(joy), guidStr, sizeof(guidStr));
                    // Same per-port disambiguation as RawJoySlot's guid
                    // above -- two identical gamepad models can share a
                    // GUID just like two identical raw encoder boards can
                    // (confirmed the same way, see that comment).
                    slot.guid = dirk::buildProfileKey(guidStr, SDL_GetGamepadPath(controller));
                    gamepads.push_back(slot);
                    if (dirkVerbose()) {
                        std::fprintf(stderr, "[dirk] gamepad connected: \"%s\" -> GAMEPAD_%d\n",
                                     SDL_GetGamepadName(controller), playerIndex);
                    }
                    break;
                }
                case SDL_EVENT_GAMEPAD_REMOVED: {
                    // Here cdevice.which IS the stable instance id (unlike
                    // DEVICEADDED above) -- matches SDL2's own documented
                    // inconsistency between the two event types.
                    for (size_t i = 0; i < gamepads.size(); ++i) {
                        if (gamepads[i].instanceId == ev.gdevice.which) {
                            if (dirkVerbose()) {
                                std::fprintf(stderr, "[dirk] gamepad disconnected: GAMEPAD_%d\n", gamepads[i].playerIndex);
                            }
                            SDL_CloseGamepad(gamepads[i].controller);
                            gamepads.erase(gamepads.begin() + static_cast<long>(i));
                            break;
                        }
                    }
                    break;
                }
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                    // Only A..DPAD_RIGHT (0-14) exist in framework.singe's
                    // GAMEPAD_0 table -- newer SDL2 adds a few buttons past
                    // that (MISC1, paddles, touchpad-click) with no
                    // corresponding SWITCH_*, so those are silently ignored
                    // rather than mismapped onto some other button's id.
                    if (ev.gbutton.button > SDL_GAMEPAD_BUTTON_DPAD_RIGHT) break;
                    for (const GamepadSlot& g : gamepads) {
                        if (g.instanceId != ev.gbutton.which) continue;
                        // GUID-keyed profile (see JoystickProfile.h) checked
                        // FIRST, same as the raw-joystick button handler
                        // above -- only a pad the wizard has actually been
                        // used to remap has one (2026-09-23, Cyril: "remapper
                        // ses touches si le mapping par defaut ne convient
                        // pas"), so this is a no-op fallthrough to the
                        // existing GAMEPAD_N.*/switchMap default for every
                        // pad it's never touched, exactly as before this
                        // existed.
                        int physicalId = kGamepadBase + g.playerIndex * kGamepadPlayerStride
                                         + kGamepadButtonOffset + ev.gbutton.button;
                        int switchId = forcedSwitch(physicalId);
                        if (switchId < 0) {
                            if (const dirk::JoystickBinding* prof = findJoystickProfile(g.guid)) {
                                auto bit = prof->buttons.find(ev.gbutton.button);
                                if (bit != prof->buttons.end()) switchId = bit->second;
                            }
                        }
                        if (switchId < 0) {
                            auto it = switchMap.find(physicalId);
                            if (it != switchMap.end()) switchId = it->second;
                        }
                        if (switchId < 0) break;
                        dispatchSwitch(switchId, ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                        break;
                    }
                    break;
                }
                case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                    // Framework.singe only defines the 6 axes every
                    // SDL_Gamepad exposes (two sticks' X/Y + two
                    // triggers) -- SDL_CONTROLLER_AXIS_MAX is exactly 6, so
                    // this bound is really just documentation, not a real
                    // guard against anything SDL would actually send.
                    if (ev.gaxis.axis >= 6) break;
                    for (GamepadSlot& g : gamepads) {
                        if (g.instanceId != ev.gaxis.which) continue;
                        int axis = ev.gaxis.axis;
                        int negId = kGamepadBase + g.playerIndex * kGamepadPlayerStride
                                    + axis * kGamepadAxisStride + 1;
                        int posId = kGamepadBase + g.playerIndex * kGamepadPlayerStride
                                    + axis * kGamepadAxisStride + 2;
                        const dirk::JoystickBinding* prof = findJoystickProfile(g.guid);
                        bool newNeg = ev.gaxis.value <= -kGamepadAxisDigitalThreshold;
                        bool newPos = ev.gaxis.value >= kGamepadAxisDigitalThreshold;
                        // Independent edge-triggered fires for each
                        // direction, same reasoning as SDL_EVENT_KEY_DOWN/KEYUP --
                        // fires at most once per real threshold crossing,
                        // not once per raw SDL_EVENT_GAMEPAD_AXIS_MOTION event
                        // (SDL sends many of those per second while a stick
                        // is held at rest against the deadzone edge).
                        if (newNeg != g.axisNegPressed[axis]) {
                            g.axisNegPressed[axis] = newNeg;
                            // Profile checked first, same as the button case above.
                            int switchId = forcedSwitch(negId);
                            if (prof && switchId < 0) {
                                auto ait = prof->axes.find(axis);
                                if (ait != prof->axes.end()) switchId = ait->second.neg;
                            }
                            if (switchId < 0) {
                                auto it = switchMap.find(negId);
                                if (it != switchMap.end()) switchId = it->second;
                            }
                            if (switchId >= 0) {
                                dispatchSwitch(switchId, newNeg);
                            }
                        }
                        if (newPos != g.axisPosPressed[axis]) {
                            g.axisPosPressed[axis] = newPos;
                            int switchId = forcedSwitch(posId);
                            if (prof && switchId < 0) {
                                auto ait = prof->axes.find(axis);
                                if (ait != prof->axes.end()) switchId = ait->second.pos;
                            }
                            if (switchId < 0) {
                                auto it = switchMap.find(posId);
                                if (it != switchMap.end()) switchId = it->second;
                            }
                            if (switchId >= 0) {
                                dispatchSwitch(switchId, newPos);
                            }
                        }
                        break;
                    }
                    break;
                }
                case SDL_EVENT_MOUSE_MOTION:
                    // ev.motion.x/y/xrel/yrel are float in SDL3 (were int in
                    // SDL2) -- callOnMouseMoved still takes ints, same as
                    // every other on-screen pixel coordinate in this file.
                    logLuaCallbackError(lua.callOnMouseMoved(static_cast<int>(ev.motion.x), static_cast<int>(ev.motion.y),
                                                              static_cast<int>(ev.motion.xrel), static_cast<int>(ev.motion.yrel), 0),
                                        "onMouseMoved");
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP: {
                    // Physical ids taken directly from framework.singe's MOUSE_0
                    // table (BUTTON_LEFT=1000, BUTTON_RIGHT=1001,
                    // BUTTON_MIDDLE=1002, BUTTON_X1=1003, BUTTON_X2=1004 --
                    // note SDL's own SDL_BUTTON_* numbering has RIGHT/MIDDLE
                    // swapped relative to this, so this is a deliberate
                    // lookup, not an arithmetic offset). MOUSE_1..3 (the same
                    // table +100/+200/+300, for additional mice) are NOT
                    // wired here: SDL2's plain mouse events don't distinguish
                    // which physical mouse sent them (everything folds into
                    // one OS-composited pointer), so there is no reliable
                    // "second mouse" to assign those ids to without Windows-
                    // specific Raw Input -- every button press below is
                    // always treated as MOUSE_0, regardless of ev.button.which.
                    int physicalId = -1;
                    switch (ev.button.button) {
                        case SDL_BUTTON_LEFT:   physicalId = 1000; break;
                        case SDL_BUTTON_RIGHT:  physicalId = 1001; break;
                        case SDL_BUTTON_MIDDLE: physicalId = 1002; break;
                        case SDL_BUTTON_X1:     physicalId = 1003; break;
                        case SDL_BUTTON_X2:     physicalId = 1004; break;
                        default: break;
                    }
                    if (physicalId < 0) break;
                    int mouseSwitch = forcedSwitch(physicalId);
                    if (mouseSwitch < 0) {
                        auto it = switchMap.find(physicalId);
                        if (it == switchMap.end()) break;
                        mouseSwitch = it->second;
                    }
                    dispatchSwitch(mouseSwitch, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                    break;
                }
                default:
                    break;
            }
            if (wizardNavRouting) {
                wizardNavRouting = false;
                if (!wizardNavTaken) wizard.handleEvent(ev); // no game switch: the wizard's own keys / device buttons
                restoreWizardPauseIfClosed();
            }
        }

        // Opened from the game's Dirk Options menu (dirkOpenJoystickWizard,
        // see api_singe.cpp) -- same as pressing F9.
        if (ctx.joystickWizardRequested) {
            ctx.joystickWizardRequested = false;
            if (!wizard.active()) openJoystickWizard();
        }

        // Advances the joystick wizard's per-prompt timeout, if it's open
        // and waiting on one (see JoystickWizard.h/Cyril's requirement
        // that an absent button not block the flow forever). No-op
        // whenever the wizard isn't active.
        //
        // 2026-09-24 field report (Cyril, real cabinet): "quand aucun
        // joystick n'est detecte le wizard se ferme apres quelques
        // secondes, mais la video ne redemarre pas". Root cause, confirmed
        // by reading the code, not guessed: tick() is ALSO where the
        // empty-device-list screen auto-closes itself on its own timeout
        // (kEmptyListTimeoutMs, see JoystickWizard.h's 2026-09-23 field
        // report -- cancel() called from inside tick(), with no SDL event
        // involved at all). The event-loop's own restore, above, only ever
        // runs right after handling a real SDL event -- so a wizard that
        // closes here, via tick(), with no button ever pressed during the
        // whole wait, skipped that restore entirely: ctx.discPaused was
        // left true forever, looking exactly like a real freeze. Same fix
        // as the event loop's, just also reached on this timeout-driven
        // close.
        wizard.tick(SDL_GetTicks());
        restoreWizardPauseIfClosed();

        // Re-apply/persist logic for a single LaunchSettings field changed
        // either from KimmyScript (EngineContext::launchSettings and the
        // dirkGet/Set* functions in api_singe.cpp -- a game's own UI Options
        // menu) or from the native F11 (bezel)/F10 (fullscreen) hotkeys
        // above -- both routes just write the field and set this flag.
        // settings.volumeVldp needs no equivalent call here -- it's read
        // live from applyVolume(pcm, settings.volumeVldp) on every decoded
        // audio packet further below, so a plain field write already takes
        // effect on its own.
        //
        // Review point 6 (2026-09-26): applies only what actually CHANGED
        // since the last apply (appliedSettings). This used to redo
        // everything on any change -- a volume +/- press re-ran
        // SDL_SetWindowFullscreen, rebuilt the full-resolution scanline
        // texture (a movie-sized RGBA buffer filled and uploaded again) and
        // rewrote game.cfg, and service.kimmy's readConfig() re-sets all six
        // dirk settings to their unchanged values at startup, which did the
        // same. The end state is identical either way: each step below only
        // depends on its own field(s).
        if (ctx.launchSettingsDirty) {
            if (fullscreenForced()) settings.fullscreen = true; // see fullscreenForced()
            ctx.launchSettingsDirty = false;
            const dirk::LaunchSettings& was = appliedSettings;
            if (settings.fullscreen != was.fullscreen) {
                applyFullscreen(window, settings.fullscreen, disc.displayWidth(), disc.height());
            }
            if (settings.volumeNonVldp != was.volumeNonVldp && ctx.soundMixer) {
                MIX_SetMixerGain(ctx.soundMixer, settings.volumeNonVldp / 100.0f);
            }
            if (settings.scanlines != was.scanlines) {
                rebuildScanlineTexture();
            }
#ifdef _WIN32
            if (settings.hideConsole != was.hideConsole) {
                applyConsoleVisibility(settings.hideConsole);
            }
#endif
            if (settings.generateLog && !gameDataPath.empty()) {
                activateGenerateLog(logFileActive, gameDataPath); // returns at once when already active
            }
            // dirk_settings.cfg (settingsPath) is a factory-default fallback
            // only now -- loaded once above, never written again from live
            // play. Every live change is instead reported onto the CURRENT
            // game's own game.cfg -- see persistPerGameDirkSettings()'s own
            // comment for why.
            if (settings.audioTrack != was.audioTrack && settings.audioTrack >= 1 &&
                settings.audioTrack - 1 != disc.currentAudioTrack()) {
                // Other soundtrack (Dirk Options "Audio"): reopen the audio
                // decoder on it, then re-seek to the frame on screen -- the
                // same path as any discSkipToFrame() jump, so the new track
                // comes in aligned to the picture and the old one's queued
                // audio is dropped (audioGeneration() bump, see below).
                if (disc.selectAudioTrack(settings.audioTrack - 1) && disc.currentFrameNumber() >= 0) {
                    disc.seekToFrame(disc.currentFrameNumber());
                }
                settings.audioTrackName = disc.audioTrackLabel(disc.currentAudioTrack());
                std::fprintf(stderr, "[dirk] audio track: now playing %s\n",
                             disc.audioTrackLabel(disc.currentAudioTrack()).c_str());
            }
            if (!gameDataPath.empty() && perGameDirkSettingsDiffer(settings, was)) {
                persistPerGameDirkSettings(gameDataPath, settings);
            }
            if (settings.swapFaceButtons != was.swapFaceButtons) {
                // Dirk Options "Swap A/B X/Y": install-wide, so the one
                // line of the shared file (see setLaunchSettingLine), not
                // game.cfg. Already live: the event loop reads the field.
                dirk::setLaunchSettingLine(settingsPath, "swap_face_buttons", settings.swapFaceButtons ? "1" : "0");
                std::fprintf(stderr, "[dirk] gamepad face buttons: %s (saved in dirk_settings.cfg)\n",
                             settings.swapFaceButtons ? "swapped, A<->B and X<->Y" : "not swapped");
            }
            appliedSettings = settings;
        }

        Uint32 now = SDL_GetTicks();
        double dt = (now - lastTicks) / 1000.0;
        lastTicks = now;
        // Hoisted out of the video-advance block below (was declared
        // inline there) so the DIRK_DEBUG_TICK print further down can
        // report what pacing actually used this tick -- real dt vs. the
        // audio clock's own delta -- instead of just the end RESULT
        // (currentFrame/audioQueuedMs). Needed to tell apart "decode is
        // genuinely falling behind real time" from "pacing is computing a
        // too-small delta despite real time passing normally", which look
        // identical from currentFrame/avOffsetMs alone. Defaults to dt;
        // only changes if the video-advance block below actually runs and
        // trusts the audio clock instead.
        double pacingDeltaSecondsForDebug = dt;

        // Clamp frameAccumulator when a seek/generation change has happened
        // since the last time this block ran -- most commonly from a Lua
        // onInputPressed callback fired during event polling earlier in
        // THIS SAME tick, but this also catches one from an onOverlayUpdate
        // call elsewhere in the loop. Read-only against lastAudioGeneration
        // (the real handling -- SDL_ClearQueuedAudio, the cumulative/
        // pacing-baseline resets, and the actual lastAudioGeneration update
        // -- stays where it already was, further down): this just has to
        // run BEFORE the catch-up below gets a chance to use whatever
        // frameAccumulator happened to be carrying from before the seek.
        //
        // Why this matters: without it, frameAccumulator can carry
        // pre-seek "catch-up debt" straight into the freshly-landed
        // position, and the catch-up cap below (allows up to 5 frames in
        // one tick) lets it burn through several of those frames' own
        // audio catch-up decodes in a single burst. Each one can overshoot
        // its own target because of how this file interleaves audio and
        // video packets (see catchUpAudio()'s own comments) -- several
        // overshooting together compounds into a much bigger one-time
        // backlog than any single decode call would produce, AND lets the
        // picture jump forward several frames "for free", ahead of where a
        // smooth, real-time pace would have put it. A real capture this
        // session confirmed both halves of this at once, on the same cut:
        // a backlog that jumped to ~320ms and stayed elevated well past
        // it (instead of draining back to the normal ~100ms), and an F7
        // test-click delay that grew by exactly the same ~6 frames right
        // after the cut as the real, OBS-measured picture-to-audio lag at
        // that same cut.
        if (disc.audioGeneration() != lastAudioGeneration) {
            frameAccumulator = 0.0;
        }

        // Gate on videoAdvanceUnlockTicks (see audioHoldbackSeconds above):
        // while held, frameAccumulator simply doesn't grow -- lastTicks
        // above still advances every tick so dt never balloons into a big
        // catch-up burst once released, it just quietly doesn't count during
        // the hold. Only ever true during the first fraction of a second of
        // the whole session -- this is not re-armed anywhere else.
        if (!ctx.discPaused && ctx.discFps > 0.0 && now >= videoAdvanceUnlockTicks) {
            // See audioClockSeconds/audioClockValid above: prefer the real
            // audio-device clock's own delta since we last paced off it,
            // falling back to the CPU timer (dt) when there's no audio, or
            // when the delta is implausible -- negative, or bigger than half
            // a second -- which happens right after a seek/generation change
            // (cumulativeAudioContentSeconds just jumped to an unrelated
            // position) or right after unpausing from the settings panel.
            // One tick of plain-dt pacing in that case is unnoticeable; the
            // audio clock resumes driving pacing again the very next tick.
            //
            // Also fall back to dt when the device queue was actually
            // EMPTY at the moment audioClockSeconds was last sampled (a
            // real underrun, tracked by audioQueueWasEmpty/AUDIO UNDERRUN
            // below). During a genuine underrun the device is outputting
            // silence, so real time keeps passing, but our own bookkeeping
            // (total ever queued minus what's left unplayed) has no way to
            // reflect that silence -- it just sits frozen at whatever it
            // was before the queue ran dry. Trusting that frozen value here
            // would read as "zero time elapsed" and stall frameAccumulator
            // for the whole underrun, turning what used to be a purely
            // audio glitch into a visible video freeze/judder too -- worse
            // than before this audio-clock pacing existed at all, not
            // better. Falling back to dt for that one tick keeps video
            // advancing smoothly through an underrun exactly as it always
            // did; the audio clock resumes driving pacing again the very
            // next tick once the queue has something in it.
            //
            // The ">0.0" this used to read literally never saw exactly 0.0
            // in a real deadlock caught on LBH_HD (entering gameplay right
            // after the intro, via doLevelArmybase()'s own
            // discSkipToFrame(levelFrameStart) -- confirmed by a
            // DIRK_DEBUG_TICK capture): audioQueuedMsNow sat at a flat
            // 0.1ms for 20+ real seconds straight, currentFrame frozen the
            // entire time. 0.1ms is ~19 bytes at this file's sample rate --
            // a few unflushed sample bytes sitting in SDL's own resampler,
            // not real drainable content -- and it never moved because
            // nothing new was ever being fed to the device to move it. That
            // makes this a real deadlock, not just a missed one-tick
            // fallback: audioClockSeconds can only advance when new audio
            // is decoded, new audio only gets decoded when stepForward()
            // runs, and stepForward() only runs when frameAccumulator grows
            // -- which this block was refusing to do off dt, because
            // audioQueuedMsNow read as barely-nonzero instead of "empty".
            // Every side of that loop was waiting on the others, forever.
            // kMinAudioQueuedMsForClockPacing gives "empty enough to
            // distrust the audio clock" a real margin -- well under a
            // single video frame period (~33ms) or the ~100-120ms steady-
            // state buffer target, so it never affects normal playback,
            // only this stuck-near-zero case.
            constexpr double kMinAudioQueuedMsForClockPacing = 2.0;
            double pacingDeltaSeconds = dt;
            if (audioClockValid && audioQueuedMsNow > kMinAudioQueuedMsForClockPacing) {
                double audioDeltaSeconds = audioClockSeconds - audioClockSecondsAtLastPacing;
                if (audioDeltaSeconds >= 0.0 && audioDeltaSeconds <= 0.5) {
                    pacingDeltaSeconds = audioDeltaSeconds;
                }
            }
            // Found chasing Cyril's own catch: a real, uniform slow-motion
            // report on LBH_HD's Armybase practice target (frames ~3242-
            // 3706), confirmed via a DIRK_DEBUG_TICK capture to sawtooth
            // audioQueuedMsNow between ~1.5ms and ~43ms -- repeatedly
            // dipping under kMinAudioQueuedMsForClockPacing above and
            // falling back to dt for that one tick, exactly as intended.
            // The bug was here, not in that threshold itself: this used to
            // unconditionally snap the baseline to the LIVE audioClockSeconds
            // every tick, whichever branch actually ran. On a dt-fallback
            // tick, that silently discards the real gap between the old
            // baseline and the new one -- audio content that genuinely
            // finished decoding/playing during this tick, just never added
            // to frameAccumulator because dt was used instead. Each
            // fallback tick alone is a tiny loss (a couple of ms at most),
            // but this segment's sawtooth crosses the threshold often
            // enough, repeatedly, to add up to the reported ~16% slowdown
            // over its ~18 real seconds -- a slow, uniform drift rather
            // than any single dropped frame, matching exactly what was
            // described ("ça rame simplement, pas des a-coups"). Advancing
            // the baseline by pacingDeltaSeconds itself -- whatever this
            // tick actually fed frameAccumulator, from either source --
            // instead of jumping to the live value keeps the two always in
            // exact lockstep: nothing is ever double-counted (the audio-
            // clock branch already advances by exactly its own delta) and
            // nothing is ever silently dropped on a dt-fallback tick either.
            audioClockSecondsAtLastPacing += pacingDeltaSeconds;
            pacingDeltaSecondsForDebug = pacingDeltaSeconds; // see declaration above -- surfaced in the DIRK_DEBUG_TICK print further down

            frameAccumulator += pacingDeltaSeconds * ctx.discFps;
            // Cap catch-up so a stall (breakpoint, window drag) doesn't
            // burn through hundreds of frames at once.
            if (frameAccumulator > 5.0) frameAccumulator = 5.0;
            while (frameAccumulator >= 1.0) {
                if (!disc.stepForward()) break; // end of stream
                frameAccumulator -= 1.0;
            }
        }

        // The render loop below still runs every vsync tick (~60Hz) so the
        // overlay/scanlines/bezel composite stays smooth, but the actual
        // decoded picture only changes at the disc's own frame rate
        // (~24-30Hz) -- re-uploading the same YUV planes to the GPU on
        // every one of the ticks where it *hasn't* changed is pure waste
        // (a real CPU->GPU bandwidth + driver-overhead cost, not a no-op).
        // Skipping it when currentFrameNumber() hasn't moved cuts that
        // specific cost roughly in half without changing what's on screen.
        if (disc.currentFrameNumber() != lastUploadedFrame) {
            uploadFrameToTexture(videoTex, disc.currentPicture());
            lastUploadedFrame = disc.currentFrameNumber();
        }

        if (movieAudioStream != nullptr) {
            // A discSkipToFrame() jump bumps audioGeneration(). This used to
            // deliberately NOT flush this device's own queue for it, on the
            // theory that leaving the outgoing scene's already-queued tail
            // to drain naturally while the new scene's audio queued in right
            // behind it was a "brief, bounded bleed" -- cheaper than paying
            // hardware output latency again at every cut. That theory turned
            // out to be wrong in a way that only showed up on a real,
            // sustained capture: SDL_QueueAudio is a strict FIFO. Whatever
            // was still queued from the OLD scene at the moment of the jump
            // doesn't just "bleed a little" -- it sits in front of every
            // single byte of the NEW scene's audio, so the new scene's audio
            // only starts becoming audible once that entire backlog has
            // finished draining. That's not a brief tail, it's a fixed phase
            // shift for the rest of the new scene (confirmed: "le décalage
            // reste" -- it doesn't resolve on its own, because nothing about
            // normal playback ever un-shifts a FIFO). The one-shot startup
            // holdback (see audioHoldbackSeconds above) got this right by
            // clearing nothing and just giving real content a head start
            // before anything was queued at all; every discSkipToFrame()
            // since then had no equivalent, and was quietly accumulating
            // this same bug at every single scene cut in the game.
            //
            // Now that audioHoldbackSeconds is a real, measured number
            // (tens of ms on real hardware, not the 300-450ms guesses this
            // project tried and rejected earlier for being an audible
            // freeze) the fix that was too expensive to repeat at every cut
            // back then is cheap enough to repeat now: clear the device's
            // queue on every generation change, same as at startup. This
            // does NOT freeze video -- the picture keeps advancing exactly
            // as before, on schedule -- it only means the movie's audio
            // goes briefly silent (bounded by audioHoldbackSeconds, i.e. on
            // the order of tens of ms on this machine) right at the cut
            // while fresh, correctly-positioned audio for the new scene
            // catches up, instead of continuing to play the wrong scene's
            // audio for however long its backlog happened to be.
            int gen = disc.audioGeneration();
            if (gen != lastAudioGeneration) {
                // Diagnostic: does a seek/cut happen THIS early, before the
                // startup-primed audio (see above) ever reaches the device?
                // seekToFrame() clears pendingAudio_ as part of its own
                // (correct, already-fixed) logic -- if something calls
                // discSkipToFrame()/discSearch() before the main loop's
                // first takePendingAudio() call, it would silently wipe out
                // everything the priming step just decoded, which would
                // fully explain why a bigger prime target had zero effect.
                if (SDL_getenv("DIRK_DEBUG_TICK")) {
                    std::fprintf(stderr,
                        "[dirk] audio generation changed %d -> %d at t=%ums currentFrame=%lld "
                        "(pendingAudioBytes now=%zu)\n",
                        lastAudioGeneration, gen, now - engineLoopStartTicks,
                        static_cast<long long>(disc.currentFrameNumber()), disc.pendingAudioBytes());
                }
                lastAudioGeneration = gen;
                // Cap how much undelivered audio a cut can hand SDL in one
                // shot. A real capture this session showed pendingAudioBytes
                // landing around 300-370ms right after a cut -- confirmed
                // independently by the user's own F7 queue-backlog readings
                // (~85ms before a cut, ~307-326ms right after) -- roughly 3x
                // kAudioLeadMarginSeconds's intended ~120ms lead. Whatever
                // that excess is, it becomes a fixed phase shift for the
                // rest of the scene: SDL_QueueAudio is a strict FIFO, so the
                // very first byte queued after SDL_ClearQueuedAudio below
                // only becomes audible once everything queued ahead of it
                // has drained, and "everything ahead of it" is this whole
                // bloated buffer, not just the small lead this was supposed
                // to be. Trimming it down to a small, bounded cap here --
                // right before it gets handed to SDL below -- guarantees the
                // post-cut backlog can't balloon past that regardless of
                // which exact call produced the excess.
                //
                // Cap history, each step a real OBS/tick/ear measurement,
                // not a re-guess:
                //   ~150ms (kAudioLeadMarginSeconds + safety margin) -> ~3-4
                //   frames residual right after the cut.
                //   120ms (kAudioLeadMarginSeconds alone)            -> 2
                //   frames residual, clean.
                //   50ms  (~one video frame period + a little)       ->
                //   picture/audio gap became imperceptible, BUT a real,
                //   large audio underrun -- confirmed by the user directly
                //   ("underrun énorme"). Too aggressive: 50ms doesn't
                //   reliably survive to the next ordinary catch-up call
                //   (the very next video frame's own decodeOneFrame(), see
                //   catchUpAudio()) once any real-world scheduling jitter
                //   is added on top of the bare ~42ms frame period.
                // Landing back on kAudioLeadMarginSeconds (120ms): the last
                // value confirmed both clean (no underrun) AND a large,
                // measured improvement (2 frames, down from 6-10 before any
                // of this) -- ON THAT ONE MACHINE. Do not push the STARTING
                // point of this cap lower than 120ms again -- 50ms already
                // found the real floor there and it's above that. This now
                // tracks audioLeadSecondsCurrent (see its declaration above)
                // rather than the fixed constant directly, though: a
                // different machine that genuinely needs more margin (real,
                // continuous stutter reported on a tester's PC, unlike
                // anything seen on the machine this cap's history above was
                // measured on) grows this live, and the post-cut cap has to
                // grow right along with it or it would just recreate the
                // same starvation right at every single cut on that machine.
                disc.trimPendingAudioToSeconds(audioLeadSecondsCurrent);
                if (SDL_getenv("DIRK_DEBUG_TICK")) {
                    std::fprintf(stderr,
                        "[dirk] ... trimmed to pendingAudioBytes now=%zu (cap was %.1fms)\n",
                        disc.pendingAudioBytes(), audioLeadSecondsCurrent * 1000.0);
                }
                SDL_ClearAudioStream(movieAudioStream);
                // Safety net: the pause/resume calls below (line ~2798) only
                // fire when ctx.discPaused CHANGES across a tick. A script's
                // own discPause()/discSearch()/discPlay() triple (see e.g.
                // doIntro() in maddog.dirk) runs all three in one Lua call,
                // synchronously, before this native loop ever samples
                // ctx.discPaused in between -- so from this loop's point of
                // view discPaused never actually changes (false going in,
                // false coming out), and that gate never runs at all for
                // this cut. If the underlying device ever independently
                // drops to a stopped/starved state right at this clear (a
                // real WASAPI behavior: some drivers don't auto-resume
                // rendering on their own just because SDL_PutAudioStreamData
                // is called again after the queue briefly hit empty), the
                // device silently never restarts, even though ctx.discPaused
                // stays false and every later takePendingAudio()/PutAudioStreamData
                // call keeps queuing real content that just piles up unheard.
                // Confirmed as the exact mechanism behind Mad Dog's "black
                // screen, no input" report: a DIRK_DEBUG_TICK capture showed
                // currentFrame and audioQueuedMs both frozen solid, unmoving
                // for 10+ real seconds, immediately after this exact clear
                // (the seek from the attract loop back to offsetIntro),
                // while discPaused=0 the entire time -- video pacing is
                // driven off the audio device's own draining clock (see
                // audioClockSeconds below), so a device stuck not-draining
                // freezes the picture too, with no script-visible pause
                // state to explain why. Calling Resume here is a no-op if
                // the device is already running; it only matters on the
                // drivers/timings where it wasn't.
                //
                // Guarded on !ctx.discPaused: a cut CAN land already paused
                // -- a script bouncing between frozen menu sub-screens
                // (discSkipToFrame(newMenuFrame) with no discPlay() in
                // between, e.g. Options/Quit) also bumps audioGeneration()
                // here, with ctx.discPaused already true going in and staying
                // true. Without this guard, this call would force that
                // already-correctly-paused device back to running (and
                // nothing downstream would re-pause it this tick, since the
                // discPaused-changed check below only reacts to an actual
                // change) -- an audible regression on exactly the menu-freeze
                // case this engine's native Pause handling was built to get
                // right. Only forcing the resume when the script isn't
                // currently asking for a freeze keeps this a pure safety net
                // for the stuck-device bug, with no effect on genuine pauses.
                if (!ctx.discPaused) SDL_ResumeAudioStreamDevice(movieAudioStream);
                // Fresh backlog right after this trim sits at/near the lead
                // target by construction -- drop any in-flight correction so
                // the new scene doesn't inherit a sped-up stream it no
                // longer needs (see kBacklogCorrection* above).
                SDL_SetAudioStreamFrequencyRatio(movieAudioStream, 1.0f);
                audioFrequencyRatioApplied = 1.0f;
                // Keep the diagnostic (avOffsetMs below) honest across the
                // jump too -- currentFrameNumber() just moved to an
                // unrelated point in the file, so re-anchor the "audio
                // content played so far" running total to match the new
                // position instead of comparing it against the old scene's
                // now-irrelevant continuous count.
                cumulativeAudioContentSeconds =
                    disc.fps() > 0 ? disc.currentFrameNumber() / disc.fps() : 0.0;
                // Same re-anchoring, but for the audio-clock PACING baseline
                // (see audioClockSecondsAtLastPacing above): without this,
                // the very next video-advance tick after a cut would diff
                // the new scene's freshly-reset audioClockSeconds against
                // a stale baseline still sitting at the OLD scene's
                // position -- a jump of however far apart the two scenes
                // happen to be in file-time, not the real ~16ms that
                // actually elapsed. The existing >0.5s/negative guard in
                // the video-advance block only catches the large or
                // backward-jumping cases; a forward cut landing within half
                // a second of the old position (very plausible -- two
                // nearby splice points in the same file) would sail right
                // through it and burn through several frames' worth of
                // frameAccumulator in one tick right at the cut -- a real,
                // audible stutter, distinct from the debug-log-overhead
                // theory. Re-anchoring here to the NEW position means the
                // first post-cut audioDeltaSeconds comes out small and
                // correct (just the real handful of ms since the cut)
                // instead of "new position minus old position", so pacing
                // stays smooth right through the cut instead of spiking.
                // Same kAudioOutputHardwareLatencySeconds subtraction as the
                // real computation below -- has to match it exactly, or
                // this anchor and the next tick's fresh audioClockSeconds
                // would disagree by that whole constant and reintroduce the
                // very jump this re-anchoring exists to prevent.
                audioClockSecondsAtLastPacing =
                    cumulativeAudioContentSeconds - kAudioOutputHardwareLatencySeconds;
            }
            if (ctx.discPaused != lastAudioPaused) {
                if (ctx.discPaused) SDL_PauseAudioStreamDevice(movieAudioStream);
                else SDL_ResumeAudioStreamDevice(movieAudioStream);
                lastAudioPaused = ctx.discPaused;
            }
            std::vector<uint8_t> pcm = disc.takePendingAudio();
            if (!pcm.empty()) {
                applyVolume(pcm, settings.volumeVldp);
                SDL_PutAudioStreamData(movieAudioStream, pcm.data(), static_cast<int>(pcm.size()));
                double bytesPerSecond = disc.audioSampleRate() * disc.audioChannels() * 2.0; // S16
                if (bytesPerSecond > 0) {
                    cumulativeAudioContentSeconds += pcm.size() / bytesPerSecond;
                }
            }

            // See audioClockSeconds/audioClockValid above -- this is the
            // device's real played-content position (everything ever
            // queued, minus whatever's still sitting unplayed), computed
            // every tick now, not just under DIRK_DEBUG_TICK, since video
            // pacing depends on it. The DIRK_DEBUG_TICK block further down
            // just reads audioClockSeconds/audioQueuedMsNow rather than
            // recomputing this itself.
            //
            // audioQueuedMsNow (the backlog itself, still SDL-queue-only) is
            // deliberately NOT corrected by kAudioOutputHardwareLatencySeconds
            // -- it's used for the underrun check (queue bytes == 0), which
            // cares about SDL's own queue state, not true audibility.
            // audioClockSeconds IS corrected: everything that reads it
            // (video pacing, avOffsetMs) cares about the real, audible
            // position, and cumulativeAudioContentSeconds - backlogSeconds
            // alone only ever answered "how much has left SDL's queue" --
            // see kAudioOutputHardwareLatencySeconds's own comment for why
            // that's not the same thing, and by how much (see F7-testing
            // session note there).
            {
                // SDL_GetAudioStreamQueued returns int (-1 on failure), unlike
                // SDL2's SDL_GetQueuedAudioSize (always-valid Uint32) -- clamp
                // a failure to 0 rather than let it become a huge Uint32 via
                // a negative-to-unsigned cast.
                int streamQueued = SDL_GetAudioStreamQueued(movieAudioStream);
                Uint32 queuedBytes = streamQueued > 0 ? static_cast<Uint32>(streamQueued) : 0;
                double bytesPerSecond = disc.audioSampleRate() * disc.audioChannels() * 2.0; // S16
                double backlogSeconds = bytesPerSecond > 0 ? queuedBytes / bytesPerSecond : 0.0;
                audioQueuedMsNow = backlogSeconds * 1000.0;
                audioClockSeconds = cumulativeAudioContentSeconds - backlogSeconds
                                     - kAudioOutputHardwareLatencySeconds;
                audioClockValid = true;

                // Safety net -- see kHardResyncBacklogMs above. Mirrors the
                // scene-cut reset exactly (same trim/clear/re-anchor steps),
                // but triggered by the backlog itself instead of a
                // generation change, so a very long, uninterrupted scene
                // can't drift past this no matter how the ratio correction
                // below is tuned. Expected to be rare -- a real, audible
                // (but small, one-time) skip, traded for stopping an
                // otherwise ever-growing lag.
                if (audioQueuedMsNow > kHardResyncBacklogMs) {
                    if (SDL_getenv("DIRK_DEBUG_TICK")) {
                        std::fprintf(stderr,
                            "[dirk] HARD RESYNC: backlog was %.1fms (over %.1fms threshold) at t=%ums currentFrame=%lld -- forcing a mid-scene resync\n",
                            audioQueuedMsNow, kHardResyncBacklogMs, now - engineLoopStartTicks,
                            static_cast<long long>(disc.currentFrameNumber()));
                    }
                    disc.trimPendingAudioToSeconds(audioLeadSecondsCurrent);
                    SDL_ClearAudioStream(movieAudioStream);
                    // Same safety net as the generation-change clear above --
                    // see its comment for why this can't just rely on the
                    // ctx.discPaused-changed gate elsewhere in the loop, and
                    // why it's guarded on !ctx.discPaused (never force a
                    // genuinely-paused menu screen back to playing).
                    if (!ctx.discPaused) SDL_ResumeAudioStreamDevice(movieAudioStream);
                    SDL_SetAudioStreamFrequencyRatio(movieAudioStream, 1.0f);
                    audioFrequencyRatioApplied = 1.0f;
                    cumulativeAudioContentSeconds =
                        disc.fps() > 0 ? disc.currentFrameNumber() / disc.fps() : 0.0;
                    audioClockSecondsAtLastPacing =
                        cumulativeAudioContentSeconds - kAudioOutputHardwareLatencySeconds;
                    // Recompute this tick's own readings post-resync so the
                    // rest of this tick (pacing, the DIRK_DEBUG_TICK print
                    // below) reflects the new state instead of the stale
                    // pre-resync numbers.
                    int streamQueuedAfter = SDL_GetAudioStreamQueued(movieAudioStream);
                    Uint32 queuedBytesAfter = streamQueuedAfter > 0 ? static_cast<Uint32>(streamQueuedAfter) : 0;
                    double backlogSecondsAfter = bytesPerSecond > 0 ? queuedBytesAfter / bytesPerSecond : 0.0;
                    audioQueuedMsNow = backlogSecondsAfter * 1000.0;
                    audioClockSeconds = cumulativeAudioContentSeconds - backlogSecondsAfter
                                         - kAudioOutputHardwareLatencySeconds;
                }

                // Closed-loop backlog drain -- see kBacklogCorrection*
                // above. Proportional on the CURRENT measured excess above
                // the lead target every tick, not a fixed guessed ratio.
                double targetMs = audioLeadSecondsCurrent * 1000.0;
                double excessMs = audioQueuedMsNow - targetMs - kBacklogCorrectionDeadbandMs;
                double desiredRatio = 1.0;
                if (excessMs > 0.0) {
                    double t = std::min(excessMs / kBacklogCorrectionSpanMs, 1.0);
                    desiredRatio = 1.0 + t * kBacklogCorrectionMaxRatio;
                }
                // Only actually call SDL when the ratio moved enough to
                // matter -- avoids hammering SDL_SetAudioStreamFrequencyRatio
                // every single tick over sub-0.05%-level measurement noise.
                if (std::fabs(desiredRatio - static_cast<double>(audioFrequencyRatioApplied)) > 0.0005) {
                    SDL_SetAudioStreamFrequencyRatio(movieAudioStream, static_cast<float>(desiredRatio));
                    audioFrequencyRatioApplied = static_cast<float>(desiredRatio);
                }
            }

            // See audioQueueWasEmpty above: checked every tick (previously
            // gated behind DIRK_DEBUG_TICK -- moved out from under that so
            // the adaptive growth right below always runs, on every
            // player's machine, not just when someone remembers to set an
            // env var before launching. A real tester confirmed continuous
            // audio stutter on their own machine on an otherwise-working
            // build, with no way to get a debug-flagged run from them --
            // "non laisse tomber les variables, il y arrivera pas" -- so
            // this can't depend on that either. Checked AFTER this tick's
            // own SDL_QueueAudio call above, so "still empty here" means
            // this tick had nothing to give it either, not just "hadn't
            // gotten to it yet this tick".
            if (!ctx.discPaused) {
                bool emptyNow = SDL_GetAudioStreamQueued(movieAudioStream) <= 0;
                if (emptyNow && !audioQueueWasEmpty) {
                    audioUnderrunStartTicks = now;
                    if (SDL_getenv("DIRK_DEBUG_TICK")) {
                        std::fprintf(stderr, "[dirk] AUDIO UNDERRUN start at t=%ums currentFrame=%lld\n",
                                     now - engineLoopStartTicks, static_cast<long long>(disc.currentFrameNumber()));
                    }
                    // Adaptive lead margin: a real underrun on THIS machine,
                    // happening despite audioLeadSecondsCurrent's current
                    // value, means that value isn't enough headroom for
                    // whatever this specific machine's real playback timing
                    // looks like (slower CPU, different driver/scheduling,
                    // a sound card with more inherent jitter -- could be any
                    // of these, no way to know which from here, and no need
                    // to). Rather than the player ever having to report this
                    // and wait for a new build with a bigger guessed
                    // constant, grow it right now, live, and keep the actual
                    // decode-ahead margin AND the post-cut trim cap (see
                    // disc.trimPendingAudioToSeconds above, which reads this
                    // same variable) in sync with it immediately. Capped so
                    // a machine in genuine, sustained trouble (audio device
                    // fighting for CPU time with something else entirely)
                    // settles at a bounded, still-reasonable worst case
                    // instead of the margin growing without limit.
                    constexpr double kAudioLeadGrowStepSeconds = 0.03;
                    constexpr double kAudioLeadMaxSeconds = 0.30;
                    if (audioLeadSecondsCurrent < kAudioLeadMaxSeconds) {
                        audioLeadSecondsCurrent = std::min(audioLeadSecondsCurrent + kAudioLeadGrowStepSeconds,
                                                            kAudioLeadMaxSeconds);
                        disc.setAudioLeadSeconds(audioLeadSecondsCurrent);
                        if (dirkVerbose()) {
                            std::fprintf(stderr,
                                "[dirk] audio underrun detected -- increasing audio buffering to %.0fms "
                                "(this machine needs more headroom than the default; adjusting automatically)\n",
                                audioLeadSecondsCurrent * 1000.0);
                        }
                    }
                } else if (!emptyNow && audioQueueWasEmpty) {
                    if (SDL_getenv("DIRK_DEBUG_TICK")) {
                        std::fprintf(stderr, "[dirk] AUDIO UNDERRUN end at t=%ums, lasted %ums, currentFrame=%lld\n",
                                     now - engineLoopStartTicks, now - audioUnderrunStartTicks,
                                     static_cast<long long>(disc.currentFrameNumber()));
                    }
                }
                audioQueueWasEmpty = emptyNow;
            }
        }

        if (SDL_getenv("DIRK_DEBUG_TICK")) {
            static Uint32 lastDebug = 0;
            if (now - lastDebug > 500) {
                lastDebug = now;
                // queuedMs: how much audio is currently sitting in the SDL
                // device's own queue, waiting to be played -- if this ever
                // hits (or hovers near) 0 while the movie is playing and
                // unpaused, the device is starving (running out of queued
                // audio faster than stepForward()/takePendingAudio() is
                // refilling it) and briefly outputting silence, which is a
                // real, silent-until-you-look-for-it source of audio falling
                // behind picture -- a separate mechanism from both the
                // decode-catch-up fix and the kAudioLeadSeconds margin.
                //
                // avOffsetMs: the actual measured AV sync error, computed
                // with no external tool needed. audioPlayedSeconds is the
                // content position the device has ACTUALLY played out so
                // far -- everything ever queued, minus whatever's still
                // sitting unplayed in the device's own queue -- which is
                // exactly what a screen-recording + waveform comparison
                // would otherwise have to measure by hand. Positive means
                // video is ahead / audio is running behind (the symptom
                // described from the start -- lips move before the words);
                // negative means audio is ahead of picture.
                double queuedMs = -1.0;
                double avOffsetMs = 0.0;
                if (movieAudioStream != nullptr && audioClockValid) {
                    queuedMs = audioQueuedMsNow;
                    double videoSeconds = disc.fps() > 0 ? disc.currentFrameNumber() / disc.fps() : 0.0;
                    avOffsetMs = (videoSeconds - audioClockSeconds) * 1000.0;
                }
                // ctx.discFps vs disc.fps(): ctx.discFps is what actually
                // drives the main loop's frame-advance rate below
                // (frameAccumulator += dt * ctx.discFps) -- a script's
                // discSetFPS() call sets it, defaulting to 29.97 (see
                // EngineContext.h) if nothing ever calls it. disc.fps() is
                // this FILE's own real rate, read from the MP4 itself. If
                // these two don't match, the engine paces video decode at
                // the WRONG speed relative to real time while audio decode
                // (tied to the file's own real PTS, not ctx.discFps at all)
                // stays correctly paced -- logged here to settle it directly
                // rather than guessing at what the game's KimmyScript sets.
                // dtMs/pacingMs: added to chase a real, reproducible slow-
                // motion report on LBH_HD (Armybase's first practice-mode
                // target, frames ~3242-3706 -- confirmed by the reporter as
                // smooth, uniform slow motion with audio/video staying in
                // sync, not a stutter/underrun, and confirmed fine under
                // real Singe on the same machine, so not a raw decode-cost
                // explanation). dtMs is the real wall-clock tick length;
                // pacingMs is what frameAccumulator actually advanced by
                // this tick (dt, or the audio clock's own delta when that's
                // trusted -- see kMinAudioQueuedMsForClockPacing above). If
                // pacingMs runs consistently under dtMs while this plays,
                // that's this block silently computing less elapsed time
                // than really passed -- the audio clock itself reading
                // slow, not the CPU falling behind. If they match and both
                // ARE the slowdown, decode/CPU is the real suspect after
                // all despite Singe managing it fine.
                std::fprintf(stderr, "[dirk] tick: discPaused=%d currentFrame=%lld audioQueuedMs=%.1f avOffsetMs=%.1f discFps=%.3f fileFps=%.3f freqRatio=%.4f dtMs=%.1f pacingMs=%.1f\n",
                             ctx.discPaused, static_cast<long long>(disc.currentFrameNumber()), queuedMs, avOffsetMs,
                             ctx.discFps, disc.fps(), static_cast<double>(audioFrequencyRatioApplied),
                             dt * 1000.0, pacingDeltaSecondsForDebug * 1000.0);
            }
        }

        // A game switching the overlay resolution (menu.kimmy's Lo/UltraLo
        // options, via overlaySetResolution -> l_overlaySetResolution in
        // api_singe.cpp) only ever updated ctx.overlayWidth/overlayHeight --
        // nothing recreated overlayTex itself, which stays at whatever size
        // it was first created at (this movie's native resolution, e.g.
        // 1920x1080) for the entire run. Singe resizes the actual overlay
        // surface so a low-res overlay still fills the screen (just
        // blockier); here, the game's own drawing switches to the new,
        // SMALLER coordinate space (drawing e.g. within a 320x240 area) but
        // lands on a texture that's still 1920x1080 -- and since that whole
        // texture (not just its drawn corner) gets stretched to fill the
        // screen below via SDL_RenderCopy(..., overlayTex, nullptr, ...),
        // the actually-drawn content ends up as a tiny patch in one corner
        // instead of filling the screen. Confirmed directly by the user
        // switching to Lo/UltraLo in the settings menu. Fix: recreate
        // overlayTex at the game's requested resolution whenever it
        // changes, right before it's used as this frame's render target --
        // the existing full-texture-to-content-rect stretch below then
        // naturally fills the screen with whatever resolution the overlay
        // texture actually is, same as Singe.
        if (ctx.overlayWidth > 0 && ctx.overlayHeight > 0 &&
            (ctx.overlayWidth != overlayTexWidth || ctx.overlayHeight != overlayTexHeight)) {
            SDL_DestroyTexture(overlayTex);
            overlayTex = SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                ctx.overlayWidth, ctx.overlayHeight);
            SDL_SetTextureBlendMode(overlayTex, SDL_BLENDMODE_BLEND);
            overlayTexWidth = ctx.overlayWidth;
            overlayTexHeight = ctx.overlayHeight;
        }

        SDL_SetRenderTarget(renderer, overlayTex);
        bool updated = false;
        if (!lua.callOnOverlayUpdate(updated) && !lua.lastError().empty()) {
            // callOnOverlayUpdate() returns false both when the script never
            // defined onOverlayUpdate (fine, not an error) and when a
            // defined one raised a Lua runtime error (very much not fine --
            // and silently swallowing that would mean the overlay just
            // never draws again, every frame, with no visible symptom
            // beyond "nothing on screen"). Only the latter sets lastError().
            static std::string lastLogged;
            if (lua.lastError() != lastLogged) {
                std::fprintf(stderr, "[dirk] onOverlayUpdate error: %s\n", lua.lastError().c_str());
                lastLogged = lua.lastError();
            }
            reportScriptError(lua.lastError(), lua.lastTraceback());
        }
        dirk::endOverlayFrame(&ctx); // fontPrint texture cache housekeeping, see EngineContext::fontPrintCache
        SDL_SetRenderTarget(renderer, nullptr);

        // See computeRenderRects's definition above for why this is an
        // explicit rect every frame rather than SDL_RenderSetLogicalSize.
        // Paints black first for the letterbox/pillarbox bars outside it
        // (e.g. either side of a 4:3 movie on a 21:9 fullscreen monitor, or
        // outside the bezel image itself if its own aspect doesn't exactly
        // match the screen's) -- without this they'd show whatever garbage
        // was left in the backbuffer instead of a clean black.
        RenderRects rects = computeRenderRects();
        // SDL3's SDL_RenderTexture wants float SDL_FRect destinations, unlike
        // SDL2's SDL_RenderCopy (integer SDL_Rect) -- RenderRects itself stays
        // SDL_Rect (all the layout math above is integer pixel arithmetic),
        // converted once here rather than changing that math to floats.
        SDL_FRect contentDstF;
        SDL_RectToFRect(&rects.content, &contentDstF);
        SDL_FRect bezelDstF;
        SDL_RectToFRect(&rects.bezelDst, &bezelDstF);
        if (debugClickFlashUntilTicks != 0 && SDL_GetTicks() < debugClickFlashUntilTicks) {
            // F7's visual half: solid white, replacing the normal picture
            // entirely -- unambiguous in a recording (a real video frame
            // full of gradual content makes "exactly when did the picture
            // change" a judgment call; black-to-white covering the whole
            // screen doesn't). Held for a real-time window (see
            // debugClickFlashUntilTicks's declaration above), not just one
            // present, so it can't be missed by a capture pipeline sampling
            // at a different rate than this render loop.
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderClear(renderer);
            SDL_RenderPresent(renderer);
        } else {
            debugClickFlashUntilTicks = 0;
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        SDL_RenderTexture(renderer, videoTex, nullptr, &contentDstF);
        // Subtitles belong to the movie: drawn right on the picture, UNDER
        // the game's overlay -- so a menu, the score or any text the game
        // draws always stays on top of them (first tried above the overlay:
        // on a real KimmyScript menu the subtitle ran into the bottom menu
        // rows). Inside the picture's own rectangle; the cue is looked up
        // for the frame on screen, so jumps in the movie need nothing
        // special. See SubtitleRenderer.h.
        if (const dirk::SubtitleTrack* track = activeSubtitleTrack()) {
            subtitleRenderer.draw(renderer,
                                  dirk::activeSubtitleLines(*track, disc.frameTimeSeconds(disc.currentFrameNumber())),
                                  contentDstF, settings.subtitleSize);
        } else {
            subtitleRenderer.draw(renderer, {}, contentDstF); // turned off: frees the texture
        }
        SDL_RenderTexture(renderer, overlayTex, nullptr, &contentDstF);
        if (scanlineTex) {
            // Drawn over the picture but under the bezel -- scanlines are a
            // property of the simulated tube, not the cabinet artwork
            // around it.
            SDL_RenderTexture(renderer, scanlineTex, nullptr, &contentDstF);
        }
        if (rects.hasBezel) {
            SDL_RenderTexture(renderer, rects.bezelTexture, nullptr, &bezelDstF);
        }
        if (wizard.active()) {
            // Drawn last, on top of the fully composited game frame
            // (picture + overlay + scanlines + bezel) -- a dimmed panel
            // over whatever's already paused underneath, same visual
            // convention as any other pause overlay. outW/outH: same
            // query computeRenderRects() itself makes every frame (cheap,
            // and the only way to get the CURRENT output size -- see that
            // lambda's own comment on why this can't be cached).
            int outW = 0, outH = 0;
            SDL_GetCurrentRenderOutputSize(renderer, &outW, &outH);
            wizard.render(renderer, outW, outH);
        }
        if (errorBannerUntilTicks != 0) {
            // Drawn after everything, wizard included -- see
            // reportScriptError. Textures are freed as soon as it expires.
            if (SDL_GetTicks() < errorBannerUntilTicks) {
                int outW = 0, outH = 0;
                SDL_GetCurrentRenderOutputSize(renderer, &outW, &outH);
                drawErrorBanner(outW, outH);
            } else {
                errorBannerUntilTicks = 0;
                destroyErrorBannerTextures();
            }
        }
        SDL_RenderPresent(renderer);
        }

        if (debugShotRequested) {
            debugShotRequested = false;
            const char* shotPath = SDL_getenv("DIRK_SCREENSHOT");
            takeScreenshot(shotPath ? shotPath : "/tmp/dirk_debug_shot.png");
        }

        // Belt-and-suspenders on top of SDL_RENDERER_PRESENTVSYNC above:
        // vsync is what actually paces this loop in normal use, but it's
        // not something dirk controls end to end -- a GPU control panel
        // set to force vsync off overrides an application's request, and
        // some driver/compositor combinations (seen more on Windows) just
        // don't honor it for every window mode. Rather than let the loop
        // go back to spinning flat out on a CPU core and the GPU in that
        // case, cap it at ~75fps here regardless -- comfortably above any
        // real disc's frame rate, so it's never the bottleneck, but a
        // world away from "as fast as the hardware allows". A no-op most
        // of the time: vsync's own block already ate most of the budget,
        // so this typically has nothing left to wait for.
        constexpr Uint32 kMinFrameMs = 1000 / 75;
        Uint32 loopElapsed = SDL_GetTicks() - loopStart;
        if (loopElapsed < kMinFrameMs) {
            SDL_Delay(kMinFrameMs - loopElapsed);
        }
    }

    // Debug aid: DIRK_SCREENSHOT=<path.png> dumps the final composited
    // frame before exit, for visually confirming render output when running
    // headless (no way to just look at the window). Not part of the engine
    // proper -- harmless in normal use since the env var is normally unset.
    if (const char* shotPath = SDL_getenv("DIRK_SCREENSHOT")) {
        takeScreenshot(shotPath);
    }

    logLuaCallbackError(lua.callOnShutdown(), "onShutdown");
    // Repeat-count summary + close (no-op, and no file, if there was never
    // an error this run). Only reached on a normal quit -- a cabinet
    // switched off mid-game keeps every entry already written (each one
    // is flushed immediately), just not this final tally.
    scriptErrors.finish();
    destroyErrorBannerTextures();
    if (errorBannerFont) TTF_CloseFont(errorBannerFont);
    subtitleRenderer.shutdown(); // before SDL_DestroyRenderer()/TTF_Quit()

    for (const auto& g : gamepads) SDL_CloseGamepad(g.controller);
    for (const auto& r : rawJoysticks) SDL_CloseJoystick(r.joystick);

    dirk::destroyBezels(bezels);
    if (scanlineTex) SDL_DestroyTexture(scanlineTex);
    SDL_DestroyTexture(overlayTex);
    SDL_DestroyTexture(videoTex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    wizard.releaseFont(); // must precede TTF_Quit() -- see JoystickWizard::releaseFont()
    TTF_Quit();
    // No IMG_Quit() in SDL3_image -- see the IMG_Init note near startup.
    if (movieAudioStream != nullptr) SDL_DestroyAudioStream(movieAudioStream);
    if (soundMixer) MIX_DestroyMixer(soundMixer);
    MIX_Quit();
    SDL_Quit();
    return 0;
}
