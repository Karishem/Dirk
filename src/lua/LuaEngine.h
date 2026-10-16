#pragma once
// LuaEngine: thin wrapper around a Lua 5.1 state, scoped to exactly what
// KimmyScript needs (see the Dirk feasibility study, section 2 "Entrées"):
//   - run a script top-to-bottom (KimmyScript's dofile-style bootstrap,
//     where loading a file *is* running it -- defining globals, tables and
//     callback functions as a side effect);
//   - dispatch the confirmed callback set: onInputPressed(what),
//     onInputReleased(what), onMouseMoved(x,y,xrel,yrel,which),
//     onOverlayUpdate() [return value matters], onShutdown(),
//     onSoundCompleted(id) [menu.singe only].
// Callbacks KimmyScript never defines (onKeyPressed/Released,
// onControllerMoved, the singeMain() coroutine model) are deliberately not
// wired up yet -- see feasibility study section 8 phase plan.
//
// Every dispatch call is a no-op (returns true, does nothing) when the
// script hasn't defined that callback, mirroring how Singe treats optional
// hooks and how KimmyScript itself guards its own optional hooks
// (`if X ~= nil then X() end`).

#include <string>

struct lua_State;

namespace dirk {

class LuaEngine {
public:
    LuaEngine() = default;
    ~LuaEngine();

    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    // Opens a fresh Lua state and loads the standard library. Native Dirk
    // functions are NOT registered here -- that's registerSingeApi()'s job
    // (src/lua/api_singe.h), kept separate so LuaEngine stays a generic,
    // reusable Lua wrapper with no knowledge of the Singe-compat API.
    bool open();
    void close();

    lua_State* state() const { return L_; }
    bool isOpen() const { return L_ != nullptr; }

    // Loads and immediately executes a script file. For KimmyScript this is
    // literally what dofile(path) does at the top of a game script: run it
    // once, top to bottom.
    bool runFile(const std::string& path);

    // Loads the shared KimmyScript engine bootstrap (KimmyScript/Script/
    // globals.kimmy, or globals.singe -- whichever exists, resolved the same
    // way dofile() itself falls back between the two, see LuaEngine.cpp)
    // natively, once, before any game script runs -- so individual games no
    // longer need their own historical
    // dofile("./KimmyScript/Script/globals.singe") line at all. If a game's
    // top-level script still has that line (nothing requires deleting it),
    // the dofile() compat shim recognizes the exact same file being asked
    // for again and silently skips it rather than re-running the whole
    // shared-engine chain a second time. `relativePath` is resolved against
    // the process's current directory, same as the dofile() calls it
    // replaces -- call only after chdir'ing to the shared engine's root.
    bool runSharedEngineBootstrap(const std::string& relativePath);

    // dirk --wizard <folder> (2026-10-10): runs Wizard, built into dirk as
    // Lua bytecode (src/lua/WizardChunk.h, made by tools/embed_wizard.py
    // from wizard.kimmy), in place of a game script.
    bool runWizardChunk();

    // "context: file:line: message" of the last failed call, one line --
    // same text as before 2026-09-26. Cleared at the start of every call
    // made through pcall(), so it's empty after a successful call (it used
    // to linger forever, letting an old error resurface under a different
    // callback's label).
    const std::string& lastError() const { return lastError_; }

    // 2026-09-26 (review, point 2): the Lua call chain that led to
    // lastError() -- "stack traceback:\n\t..." as produced by
    // debug.traceback() at the moment of the error, e.g. showing
    // onInputReleased -> writeSave -> writeSavePlus -> io.output for the
    // DL2 s4p.cfg bug, instead of only the innermost addons.kimmy:314.
    // Empty on success, and for load/syntax errors (nothing ran yet, so
    // there's no chain to show). Only ever computed when an error actually
    // happens -- zero cost on the normal path.
    const std::string& lastTraceback() const { return lastTraceback_; }

    bool globalFunctionExists(const std::string& fnName);

    bool callOnInputPressed(int what);
    bool callOnInputReleased(int what);
    bool callOnMouseMoved(int x, int y, int xrel, int yrel, int which);
    bool callOnShutdown();
    bool callOnSoundCompleted(int id);

    // onOverlayUpdate() is the one callback whose return value drives
    // engine behavior (OVERLAY_UPDATED vs OVERLAY_NOT_UPDATED -- see
    // feasibility study section 6). Returns false if the callback is
    // undefined or errored; `updatedOut` is only meaningful on true.
    bool callOnOverlayUpdate(bool& updatedOut);

private:
    bool pushGlobalFunction(const std::string& fnName);
    bool pcall(int nargs, int nresults, const std::string& context);

    lua_State* L_ = nullptr;
    std::string lastError_;
    std::string lastTraceback_;
};

// luaL_loadfile(), or the same from a launched .zdirk's zip when the file
// is only there (see core/GameArchive.h). Same return value and stack
// effect as luaL_loadfile.
int loadFileOrArchive(lua_State* L, const std::string& path);

// The wizard.kimmy fingerprint WizardChunk.h was made from (8 hex digits).
const char* wizardChunkTag();

} // namespace dirk
