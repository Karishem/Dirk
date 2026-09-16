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

    const std::string& lastError() const { return lastError_; }

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
};

} // namespace dirk
