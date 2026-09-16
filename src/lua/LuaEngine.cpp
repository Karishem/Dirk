#include "lua/LuaEngine.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace dirk {

LuaEngine::~LuaEngine() {
    close();
}

bool LuaEngine::open() {
    close();
    L_ = luaL_newstate();
    if (!L_) {
        lastError_ = "luaL_newstate failed (out of memory)";
        return false;
    }
    luaL_openlibs(L_);
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
    if (luaL_loadfile(L_, path.c_str()) != 0) {
        lastError_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "unknown load error";
        lua_pop(L_, 1);
        return false;
    }
    return pcall(0, 0, path);
}

bool LuaEngine::pcall(int nargs, int nresults, const std::string& context) {
    if (lua_pcall(L_, nargs, nresults, 0) != 0) {
        const char* msg = lua_tostring(L_, -1);
        lastError_ = context + ": " + (msg ? msg : "unknown error");
        lua_pop(L_, 1);
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
