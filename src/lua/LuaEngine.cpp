#include "lua/LuaEngine.h"
#include "core/GameArchive.h"
#include "lua/WizardChunk.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace dirk {

namespace {

// Stock Lua 5.1's os.clock() wraps C's clock(): CPU time consumed by the
// process, not real elapsed time. KimmyScript uses os.clock() as a wall-
// clock stopwatch for its own timers -- toolbox.singe's blinkTimer()/
// blinkTimer3() ("Function blinks every second", comparing
// `blinkSecs - lastBlinkSecs` against a threshold in seconds) and
// hscore.singe's joyDelayOn()/joyDelayDue() (the joystick-repeat delay for
// menu navigation) -- confirmed by reading both real files, not assumed.
// Under CPU-time accounting those timers run in slow motion: Dirk spends
// most of each frame asleep (vsync-capped, GPU does the video decode), so
// CPU time accumulates far slower than real time, and a "blink every
// second" ends up blinking every several seconds instead, with the exact
// ratio depending on system load. Scott Duensing hit the identical problem
// in real Singe and fixed it the same way for Singe 3.00 ("os.clock() in
// scripts now returns wall time rather than processor time... nearly every
// existing game times its input debounces and prompts with it"); KimmyScript
// is exactly one of those "existing games" his fix describes, so it gets the
// same override here. Done by replacing os.clock at the Lua level rather
// than touching any KimmyScript file -- scripts keep calling os.clock()
// exactly as written, they just get real elapsed seconds back now.
int l_osClockWallTime(lua_State* L) {
    using namespace std::chrono;
    // Measured from an arbitrary fixed point (this process's own start) --
    // KimmyScript only ever looks at the *difference* between two readings,
    // never the absolute value, so the epoch doesn't matter as long as it's
    // steady and consistent for the life of the process. steady_clock
    // (monotonic, immune to wall-clock adjustments) is the right clock for
    // that; system_clock is not.
    static const steady_clock::time_point kStart = steady_clock::now();
    double secs = duration<double>(steady_clock::now() - kStart).count();
    lua_pushnumber(L, secs);
    return 1;
}

void installWallClockOsClock(lua_State* L) {
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, l_osClockWallTime);
        lua_setfield(L, -2, "clock");
    }
    lua_pop(L, 1); // os (or whatever non-table junk was there instead)
}

// A KimmyScript install's shared engine files (KimmyScript/Script/*.singe)
// get renamed to .kimmy as part of setting a game up for Dirk (see the
// README's "Every other script file..." paragraph) -- but each game's own
// TOP-LEVEL launcher (the thing that becomes <Game>.dirk) is a pure, un-
// touched copy of the original file, and that original file's own content
// hardcodes the OLD extension when it pulls in the shared engine, e.g.
// (confirmed by reading a real one, DLe.singe line 27):
//   dofile("./KimmyScript/Script/globals.singe")
// Once globals.singe has been renamed to globals.kimmy on disk, that literal
// dofile() call 404s -- every single game hits this, not just one, since
// every top-level launcher was written against the pre-rename layout. Rather
// than requiring that line be hand-edited in every game (which would break
// the "runs the existing scripts unmodified" promise this whole project is
// built on), dofile() itself is wrapped here: if the exact path it's given
// doesn't exist but swapping its .singe/.kimmy extension resolves to a real
// file, that's silently substituted before delegating to the real dofile.
// Handles both directions (a .kimmy reference against an unrenamed .singe
// file falls back too) since nothing about KimmyScript's own content
// depends on which direction the fix goes, only that the fallback exists.
std::string singeKimmyExtensionFallback(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(path, ec)) return path;

    fs::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    std::string altExt;
    if (ext == ".singe") altExt = ".kimmy";
    else if (ext == ".kimmy") altExt = ".singe";
    else return path; // not a rename-affected extension, leave alone

    fs::path alt = p;
    alt.replace_extension(altExt);
    ec.clear();
    if (fs::exists(alt, ec)) {
        // Routine note, not a warning -- gated behind DIRK_VERBOSE like
        // every other informational print (see main.cpp's dirkVerbose()).
        if (std::getenv("DIRK_VERBOSE")) {
            std::fprintf(stderr,
                "[dirk] note: %s not found -- using %s instead "
                "(.singe/.kimmy rename compatibility)\n",
                path.c_str(), alt.string().c_str());
        }
        return alt.string();
    }
    return path; // neither exists -- let the real dofile() report it
}

// Registry key holding the resolved, canonical path of whichever shared-
// engine bootstrap file LuaEngine::runSharedEngineBootstrap() just loaded
// natively (see below) -- read and consumed (cleared) by the dofile shim
// just below so a game script that still has its own historical
// dofile("./KimmyScript/Script/globals.singe") line doesn't re-run the
// entire shared-engine chain a second time (which would re-dofile every
// shared file again and re-run every soundLoad() at the bottom of
// globals.kimmy, leaking a duplicate sound handle each time it does).
// Absent/nil the rest of the time -- runSharedEngineBootstrap() sets it
// right before loading, the shim clears it the first time it matches (one-
// shot, not a permanent block), and nothing else ever touches this key.
constexpr const char* kNativeBootstrapGuardKey = "dirk_native_bootstrap_path";

// 2026-09-26 (review, point 2): registry copy of the stock
// debug.traceback, captured once in open() right after luaL_openlibs --
// kept in the registry (not looked up as the `debug` global at error
// time) so a script that happens to overwrite or nil out its own `debug`
// global can't silently disable error tracebacks.
constexpr const char* kTracebackKey = "dirk_debug_traceback";

// lua_pcall message handler: runs AT the moment of the error, while the
// failing call chain is still on the Lua stack (by the time lua_pcall
// returns, it's gone -- which is why this can't be done afterwards).
// Same approach as stock Lua 5.1's own lua.c `traceback` function: hand
// the message to debug.traceback(msg, 2) (level 2 skips this handler
// itself), which returns "msg\nstack traceback:\n\t...". Falls back to
// the bare message if debug.traceback isn't available. A non-string
// error value (error({}) and friends) gets a readable placeholder rather
// than a nil that would otherwise lose the error entirely.
int tracebackMessageHandler(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
        lua_replace(L, 1);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, kTracebackKey);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_settop(L, 1);
        return 1;
    }
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 2);
    lua_call(L, 2, 1);
    return 1;
}

// Dragon's Lair enhanced and its Hypseus-heritage siblings (the DL-clone,
// single-player LaserDisc genre, as opposed to the shooter games' own
// KimmyScript-native launchers) ship a "Hypseus Game Setup" header written
// for real Hypseus's own on-disk layout, confirmed directly by Cyril
// (2026-09-22): Hypseus/singe/<game>/ for each game, with the shared
// framework as a sibling at Hypseus/singe/FrameworkKimmy/ -- flat (globals.
// singe et al. directly inside, plus its own Cfg/ and Skin/, no Script/
// subfolder), never renamed to KimmyScript's own layout. Its one shared-
// bootstrap line looks like (real example):
//   dofile(BASEDIR .. "/FrameworkKimmy/globals.singe")   -- BASEDIR = "singe"
// This can never canonically match kNativeBootstrapGuardKey below --
// FrameworkKimmy/globals.singe and KimmyScript/Script/globals.kimmy are two
// different files on disk, laid out two different ways -- so the guard's
// path-equality check alone would never catch it, and Dirk has no reason to
// grow a second FrameworkKimmy-shaped loader just to read a file it has
// already loaded the equivalent of by the time any game script runs (see
// main.cpp's runSharedEngineBootstrap() call site, unconditional, before
// lua.runFile(scriptPath)). The explicit goal (Cyril, 2026-09-22): someone
// migrating an existing Hypseus install to Dirk shouldn't have to touch
// their game files at all. So this line is recognized by shape -- its
// filename is globals.singe/.kimmy AND one of its path segments is
// FrameworkKimmy (case-insensitive: hand-written third-party script text,
// not a Dirk-controlled path) -- and unconditionally skipped, the same
// spirit as the guard just below, just matched by pattern instead of exact
// path since the two can never be literally the same file.
bool isLegacyFrameworkKimmyBootstrap(const std::string& path) {
    namespace fs = std::filesystem;
    std::string filename = fs::path(path).filename().string();
    if (filename != "globals.singe" && filename != "globals.kimmy") return false;

    for (const auto& part : fs::path(path)) {
        std::string seg = part.string();
        std::transform(seg.begin(), seg.end(), seg.begin(),
                        [](unsigned char c) { return std::tolower(c); });
        if (seg == "frameworkkimmy") return true;
    }
    return false;
}

// Real dofile is stashed as this closure's sole upvalue by
// installDofileRenameCompat() below, so this only ever intercepts the
// filename argument -- everything else (multiple return values, stdin
// dofile(), error propagation on a genuinely broken script) is however
// Lua's own dofile already behaves.
int l_dofileRenameCompat(lua_State* L) {
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        std::string given = lua_tostring(L, 1);

        if (isLegacyFrameworkKimmyBootstrap(given)) {
            // Hypseus-heritage line (see comment above) -- dirk already ran
            // the equivalent shared bootstrap itself before this game's
            // script started; nothing to load, nothing for the user to
            // change in their own file. Always skip, unconditionally.
            if (std::getenv("DIRK_VERBOSE")) {
                std::fprintf(stderr,
                    "[dirk] note: %s is a Hypseus-style shared-framework bootstrap "
                    "line -- dirk already loaded the equivalent KimmyScript chain "
                    "itself, skipping (nothing to change in this game's script)\n",
                    given.c_str());
            }
            return 0;
        }

        std::string resolved = singeKimmyExtensionFallback(given);
        if (resolved != given) {
            lua_pushstring(L, resolved.c_str());
            lua_replace(L, 1);
        }

        lua_getfield(L, LUA_REGISTRYINDEX, kNativeBootstrapGuardKey);
        if (lua_isstring(L, -1)) {
            std::string guardPath = lua_tostring(L, -1);
            lua_pop(L, 1);
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(resolved, ec);
            if (!ec && canon.string() == guardPath) {
                // Dirk already loaded exactly this file natively, before the
                // game's own script started -- this call is the game's own
                // (possibly not-yet-deleted) dofile(globals...) line seeing
                // it a second time. Consume the guard (one-shot) and skip.
                lua_pushnil(L);
                lua_setfield(L, LUA_REGISTRYINDEX, kNativeBootstrapGuardKey);
                if (std::getenv("DIRK_VERBOSE")) {
                    std::fprintf(stderr,
                        "[dirk] note: %s already loaded by dirk itself -- skipping "
                        "(safe to delete this dofile() line from the game script)\n",
                        resolved.c_str());
                }
                return 0;
            }
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pushvalue(L, lua_upvalueindex(1)); // the real dofile
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

void installDofileRenameCompat(lua_State* L) {
    lua_getglobal(L, "dofile");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushcclosure(L, l_dofileRenameCompat, 1); // consumes the real dofile
    lua_setglobal(L, "dofile");
}

} // namespace

LuaEngine::~LuaEngine() {
    close();
}

// .zdirk games (2026-09-28, see core/GameArchive.h): the game's folder is
// the .zdirk's folder with the zip's content laid over it. Scripts are
// loaded straight from the zip (no copy on disk); Lua's io functions, which
// only take file names, get a temporary copy for a file that is only in
// the zip (never needed by KimmyScript in practice: its Cfg/ files are on
// disk). Installed only when a .zdirk was launched: other games run
// exactly as before. Written in Lua, not C, with a non-tail call through a
// local named like the original: a mistake in a game's script still reads
// "bad argument #1 to 'output'" (a C wrapper would turn the name into '?').
int l_archiveResolveRead(lua_State* L) {
    lua_pushstring(L, dirk::gameArchive().resolveRead(luaL_checkstring(L, 1)).c_str());
    return 1;
}
int l_archiveResolveWrite(lua_State* L) {
    lua_pushstring(L, dirk::gameArchive().resolveWrite(luaL_checkstring(L, 1)).c_str());
    return 1;
}
// ZL(path): the script as a function when it is in the zip (also under the
// other of .singe/.kimmy, like the dofile shim does on disk); nil + message
// for a syntax error; false when the zip doesn't have it (not a .zdirk
// file: the original function handles it).
int l_archiveLoad(lua_State* L) {
    const std::string path = luaL_checkstring(L, 1);
    std::vector<std::string> candidates{path};
    const std::filesystem::path p(path);
    const std::string ext = p.extension().string();
    if (ext == ".singe") candidates.push_back(std::filesystem::path(p).replace_extension(".kimmy").string());
    if (ext == ".kimmy") candidates.push_back(std::filesystem::path(p).replace_extension(".singe").string());
    for (const auto& c : candidates) {
        std::vector<unsigned char> bytes;
        if (!dirk::gameArchive().readArchiveFile(c, bytes)) continue;
        const std::string chunkName = "@" + c;
        if (luaL_loadbuffer(L, reinterpret_cast<const char*>(bytes.data()), bytes.size(), chunkName.c_str()) != 0) {
            lua_pushnil(L);
            lua_insert(L, -2); // nil, message
            return 2;
        }
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}
const char* kArchiveRedirectChunk = R"LUA(
local R, W, ZL = ...
local function pack(...) return { n = select("#", ...), ... } end
local function rd(p) if type(p) == "string" then return R(p) end return p end
local function wr(p) if type(p) == "string" then return W(p) end return p end
do local open = io.open
  io.open = function(p, m, ...)
    local w = type(m) == "string" and m:find("[wa+]") ~= nil
    local r = pack(open(w and wr(p) or rd(p), m, ...)); return unpack(r, 1, r.n) end end
do local input = io.input
  io.input = function(p, ...) local r = pack(input(rd(p), ...)); return unpack(r, 1, r.n) end end
do local output = io.output
  io.output = function(p, ...) local r = pack(output(wr(p), ...)); return unpack(r, 1, r.n) end end
do local lines = io.lines
  io.lines = function(p, ...) local r = pack(lines(rd(p), ...)); return unpack(r, 1, r.n) end end
do local loadfile = loadfile
  _G.loadfile = function(p, ...)
    if type(p) == "string" then local f, e = ZL(p); if f ~= false then return f, e end end
    local r = pack(loadfile(p, ...)); return unpack(r, 1, r.n) end end
do local dofile = dofile
  _G.dofile = function(p, ...)
    if type(p) == "string" then
      local f, e = ZL(p)
      if f == nil then error(e, 0) end
      if f ~= false then local r = pack(f()); return unpack(r, 1, r.n) end
    end
    local r = pack(dofile(p, ...)); return unpack(r, 1, r.n) end end
)LUA";

void installArchiveRedirects(lua_State* L) {
    if (luaL_loadbuffer(L, kArchiveRedirectChunk, std::strlen(kArchiveRedirectChunk), "=dirk .zdirk") != 0) {
        std::fprintf(stderr, "[dirk] internal error (.zdirk redirects): %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_pushcfunction(L, l_archiveResolveRead);
    lua_pushcfunction(L, l_archiveResolveWrite);
    lua_pushcfunction(L, l_archiveLoad);
    if (lua_pcall(L, 3, 0, 0) != 0) {
        std::fprintf(stderr, "[dirk] internal error (.zdirk redirects): %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

// luaL_loadfile(), or the same from a .zdirk's zip (see above).
int loadFileOrArchive(lua_State* L, const std::string& path) {
    std::vector<unsigned char> bytes;
    if (dirk::gameArchive().readArchiveFile(path, bytes)) {
        const std::string chunkName = "@" + path;
        return luaL_loadbuffer(L, reinterpret_cast<const char*>(bytes.data()), bytes.size(), chunkName.c_str());
    }
    return luaL_loadfile(L, path.c_str());
}

bool LuaEngine::open() {
    close();
    L_ = luaL_newstate();
    if (!L_) {
        lastError_ = "luaL_newstate failed (out of memory)";
        return false;
    }
    luaL_openlibs(L_);
    // See kTracebackKey -- grabbed before any script can touch `debug`.
    lua_getglobal(L_, "debug");
    if (lua_istable(L_, -1)) {
        lua_getfield(L_, -1, "traceback");
        lua_setfield(L_, LUA_REGISTRYINDEX, kTracebackKey);
    }
    lua_pop(L_, 1);
    installWallClockOsClock(L_);
    installDofileRenameCompat(L_);
    if (dirk::gameArchive().active()) installArchiveRedirects(L_); // outermost, around the dofile shim
    return true;
}

void LuaEngine::close() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
}

bool LuaEngine::runFile(const std::string& path) {
    if (!L_) {
        lastError_ = "runFile called before open()";
        return false;
    }
    if (loadFileOrArchive(L_, path) != 0) { // a .zdirk's script comes from the zip
        lastError_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "unknown load error";
        lastTraceback_.clear(); // load/syntax error: nothing ran, no call chain to show
        lua_pop(L_, 1);
        return false;
    }
    return pcall(0, 0, path);
}

// 2026-10-10: Wizard (KimmyScript's create mode) ships inside dirk as Lua
// bytecode instead of as KimmyScript/Script/wizard.kimmy (Cyril's request:
// the sheet editor is not handed out as editable source). The bytes in
// WizardChunk.h are scrambled by tools/embed_wizard.py so its strings do not
// show in the binary; this undoes it with the same generator.
bool LuaEngine::runWizardChunk() {
    if (!L_) {
        lastError_ = "runWizardChunk called before open()";
        return false;
    }
    std::string code(kWizardChunkSize, '\0');
    uint32_t x = 0x2545F491u;
    for (std::size_t i = 0; i < kWizardChunkSize; ++i) {
        x = x * 1103515245u + 12345u;
        code[i] = static_cast<char>(kWizardChunk[i] ^ ((x >> 16) & 0xFFu));
    }
    // No string.dump while Wizard runs: KimmyScript's own files are plain
    // text anyone can edit, and one line in them could otherwise write
    // Wizard's functions back out as bytecode.
    lua_getglobal(L_, "string");
    if (lua_istable(L_, -1)) {
        lua_pushnil(L_);
        lua_setfield(L_, -2, "dump");
    }
    lua_pop(L_, 1);
    if (luaL_loadbuffer(L_, code.data(), code.size(), "=Wizard") != 0) {
        lastError_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "unknown load error";
        lastTraceback_.clear();
        lua_pop(L_, 1);
        return false;
    }
    return pcall(0, 0, "Wizard");
}

const char* wizardChunkTag() { return kWizardTag; }

bool LuaEngine::runSharedEngineBootstrap(const std::string& relativePath) {
    if (!L_) {
        lastError_ = "runSharedEngineBootstrap called before open()";
        return false;
    }
    // Same .singe/.kimmy fallback dofile() itself uses, so this loads
    // whichever spelling actually exists on disk regardless of which one
    // was asked for.
    std::string resolved = singeKimmyExtensionFallback(relativePath);

    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(resolved, ec);
    std::string guardPath = ec ? resolved : canon.string();
    lua_pushstring(L_, guardPath.c_str());
    lua_setfield(L_, LUA_REGISTRYINDEX, kNativeBootstrapGuardKey);

    if (!runFile(resolved)) {
        // Never actually loaded -- clear the guard so it can't spuriously
        // suppress a later, genuine dofile() call on that same path.
        lua_pushnil(L_);
        lua_setfield(L_, LUA_REGISTRYINDEX, kNativeBootstrapGuardKey);
        return false;
    }
    return true;
}

bool LuaEngine::pcall(int nargs, int nresults, const std::string& context) {
    // Cleared up front (2026-09-26): previously lastError_ was only ever
    // overwritten on the NEXT failure, never reset on success, so a caller
    // checking lastError() after an unrelated successful call could pick
    // up a stale message (and log it under the wrong callback's name).
    lastError_.clear();
    lastTraceback_.clear();

    // Slot the message handler in just below the function being called
    // (stack: ... fn arg1..argN  ->  ... handler fn arg1..argN), then take
    // it back out afterwards whatever happened -- on success the results
    // sit above it, on failure the error string does, and lua_remove
    // shifts either down into place, so callers see exactly the stack
    // layout they always did (callOnOverlayUpdate() reads its return
    // value at -1 right after this).
    const int handlerIndex = lua_gettop(L_) - nargs;
    lua_pushcfunction(L_, tracebackMessageHandler);
    lua_insert(L_, handlerIndex);
    const int rc = lua_pcall(L_, nargs, nresults, handlerIndex);
    lua_remove(L_, handlerIndex);

    if (rc != 0) {
        const char* raw = lua_tostring(L_, -1);
        std::string full = raw ? raw : "unknown error";
        lua_pop(L_, 1);
        // debug.traceback's output is "msg\nstack traceback:\n\t..." --
        // split it so lastError() keeps its historical one-line form
        // (what every existing log line prints) and the chain lives in
        // lastTraceback() on its own.
        const std::string marker = "\nstack traceback:";
        const size_t cut = full.find(marker);
        if (cut != std::string::npos) {
            lastTraceback_ = full.substr(cut + 1);
            full.erase(cut);
        }
        lastError_ = context + ": " + full;
        // Files the failed call had opened and not closed yet (KimmyScript's
        // `h = io.input(f) ... io.close(h)` with an error in between) are
        // only closed when Lua's garbage collector frees them. A script
        // error repeated every frame then opens one more file per frame and
        // the collector, which runs on memory use, not on open files, lets
        // them pile up: on an R36S (2026-10-03) a few seconds of one
        // repeating error ended in "Too many open files", and every later
        // read failed. A full collection after each error closes them.
        lua_gc(L_, LUA_GCCOLLECT, 0);
        return false;
    }
    return true;
}

bool LuaEngine::globalFunctionExists(const std::string& fnName) {
    if (!L_) return false;
    lua_getglobal(L_, fnName.c_str());
    bool isFn = lua_isfunction(L_, -1);
    lua_pop(L_, 1);
    return isFn;
}

bool LuaEngine::pushGlobalFunction(const std::string& fnName) {
    lua_getglobal(L_, fnName.c_str());
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        // Optional hook not defined: not an error, and it must not look
        // like one -- callOnOverlayUpdate() returns false in this case and
        // main.cpp tells "undefined" from "errored" by lastError() being
        // empty, so a leftover message from some earlier failed callback
        // would otherwise be re-reported here under the wrong name.
        lastError_.clear();
        lastTraceback_.clear();
        return false;
    }
    return true;
}

bool LuaEngine::callOnInputPressed(int what) {
    if (!L_ || !pushGlobalFunction("onInputPressed")) return true; // optional hook
    lua_pushinteger(L_, what);
    return pcall(1, 0, "onInputPressed");
}

bool LuaEngine::callOnInputReleased(int what) {
    if (!L_ || !pushGlobalFunction("onInputReleased")) return true;
    lua_pushinteger(L_, what);
    return pcall(1, 0, "onInputReleased");
}

bool LuaEngine::callOnMouseMoved(int x, int y, int xrel, int yrel, int which) {
    if (!L_ || !pushGlobalFunction("onMouseMoved")) return true;
    lua_pushinteger(L_, x);
    lua_pushinteger(L_, y);
    lua_pushinteger(L_, xrel);
    lua_pushinteger(L_, yrel);
    lua_pushinteger(L_, which);
    return pcall(5, 0, "onMouseMoved");
}

bool LuaEngine::callOnShutdown() {
    if (!L_ || !pushGlobalFunction("onShutdown")) return true;
    return pcall(0, 0, "onShutdown");
}

bool LuaEngine::callOnSoundCompleted(int id) {
    if (!L_ || !pushGlobalFunction("onSoundCompleted")) return true;
    lua_pushinteger(L_, id);
    return pcall(1, 0, "onSoundCompleted");
}

bool LuaEngine::callOnOverlayUpdate(bool& updatedOut) {
    if (!L_ || !pushGlobalFunction("onOverlayUpdate")) return false;
    if (!pcall(0, 1, "onOverlayUpdate")) return false;

    // KimmyScript returns the OVERLAY_UPDATED / OVERLAY_NOT_UPDATED globals
    // defined by the framework layer (integer constants, per the Singe
    // manual). We don't hardcode their values here: whatever the script
    // itself considers OVERLAY_UPDATED is compared against the returned
    // value, so this works regardless of the exact constant framework.singe
    // picks.
    lua_getglobal(L_, "OVERLAY_UPDATED");
    bool haveConst = lua_isnumber(L_, -1);
    lua_Number updatedConst = haveConst ? lua_tonumber(L_, -1) : 1.0;
    lua_pop(L_, 1);

    if (lua_isnumber(L_, -1)) {
        updatedOut = (lua_tonumber(L_, -1) == updatedConst);
    } else {
        // Defensive fallback if a script returns something unexpected.
        updatedOut = false;
    }
    lua_pop(L_, 1);
    return true;
}

} // namespace dirk
