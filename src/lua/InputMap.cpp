#include "lua/InputMap.h"
#include "core/GameArchive.h"
#include "lua/LuaEngine.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/stat.h>

namespace dirk {

namespace {

bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 || dirk::gameArchive().archiveHasFile(path); // or in a .zdirk's zip
}

// The framework.singe SWITCH_* enumeration (framework.singe:470-491), and
// the controls.cfg INPUT_* global whose array of bindings maps to it.
// See InputMap.h for why this correspondence is inferred rather than
// documented.
//
// 2026-09-22: each entry now also carries an optional `alias` -- a second,
// friendlier global name (GO_UP, COIN_1P, BUTTON_1, ...) this same binding
// is ALSO recognized under, tried only if the primary INPUT_* name isn't
// defined. Added for the new simplified default controls.cfg (see
// findSharedEngineRoot()'s KimmyScript/Cfg/controls.cfg fallback in
// main.cpp) without touching any of Cyril's existing per-game controls.cfg
// files, all of which still use the original INPUT_* names exclusively and
// keep working completely unchanged.
struct SwitchBinding {
    const char* inputGlobal;
    int switchId;
    const char* alias = nullptr;
};

constexpr SwitchBinding kBindings[] = {
    {"INPUT_UP", 0, "GO_UP"},              // SWITCH_UP
    {"INPUT_LEFT", 1, "GO_LEFT"},          // SWITCH_LEFT
    {"INPUT_DOWN", 2, "GO_DOWN"},          // SWITCH_DOWN
    {"INPUT_RIGHT", 3, "GO_RIGHT"},        // SWITCH_RIGHT
    {"INPUT_1P_START", 4, "START_1P"},     // SWITCH_START1
    {"INPUT_2P_START", 5, "START_2P"},     // SWITCH_START2
    {"INPUT_ACTION_1", 6, "BUTTON_1"},     // SWITCH_BUTTON1
    {"INPUT_ACTION_2", 7, "BUTTON_2"},     // SWITCH_BUTTON2
    {"INPUT_ACTION_3", 8, "BUTTON_3"},     // SWITCH_BUTTON3
    {"INPUT_1P_COIN", 9, "COIN_1P"},       // SWITCH_COIN1
    {"INPUT_2P_COIN", 10, "COIN_2P"},      // SWITCH_COIN2 -- real usage confirmed
                                            // (moves.kimmy + several games' own
                                            // onInputPressed, e.g. drugwars.singe,
                                            // maddog2.singe): 2nd-player coin slot.
    {"INPUT_SKILL_EASY", 11, "SAVE_1"},    // SWITCH_SKILL1
    {"INPUT_SKILL_MEDIUM", 12, "SAVE_2"},  // SWITCH_SKILL2
    {"INPUT_SKILL_HARD", 13, "SAVE_3"},    // SWITCH_SKILL3
    {"INPUT_SERVICE", 14, "OPTION_MENU"},  // SWITCH_SERVICE
    {"INPUT_TEST_MODE", 15, "SAVE_MENU"},  // SWITCH_TEST
    {"INPUT_RESET_CPU", 16, "RESET_CPU"},  // SWITCH_RESET -- sets p1RESET_CPU in
                                            // moves.kimmy, confirmed (grep across
                                            // every uploaded script) never READ
                                            // anywhere else -- currently a no-op
                                            // in this engine, left out of the new
                                            // simplified default for that reason.
    {"INPUT_SCREENSHOT", 17, "SCREENSHOT"}, // SWITCH_SCREENSHOT -- no real usage
                                             // found outside its own definition;
                                             // left out of the simplified default.
    {"INPUT_QUIT", 18, "QUIT_PROGRAM"},    // SWITCH_QUIT -- real usage confirmed
                                            // (drugwars/johnnyrock/lastbounty-
                                            // hunter/crimepatrol/spacepirates/
                                            // maddog2/maddog: "if user pressed
                                            // ESC then quit program").
    {"INPUT_PAUSE", 19, "PAUSE_GAME"},     // SWITCH_PAUSE
    {"INPUT_CONSOLE", 20, "EXIT_MENU"},    // SWITCH_CONSOLE
    {"INPUT_ACTION_4", 21, "BUTTON_4"},    // SWITCH_BUTTON4 -- no real usage found;
                                            // left out of the simplified default.
    {"INPUT_TILT", 22, "TILT"},            // SWITCH_TILT -- no real usage found;
                                            // left out of the simplified default.
    {"INPUT_GRAB", 23, "GRAB"},            // SWITCH_GRAB -- confirmed present in
                                // both the shared KimmyScript framework.singe
                                // and Maddog_HD's own local one (previously
                                // thought missing; it isn't), but no real
                                // *usage* found outside framework's own
                                // definition; left out of the simplified
                                // default.
};

// True if the Lua value at `idx` is the exact same table object as
// SCANCODE[name] or one of GAMEPAD_0..GAMEPAD_3's [name] (bare comparison by
// identity, not by content) -- i.e. this binding entry really did come from
// one of those, not `MOUSE_0.something`, which shares the same
// {name=.., value=..} shape but isn't implemented as a switch source.
// GAMEPAD_1/2/3 are framework.singe's utilDeepCopy() of GAMEPAD_0 with
// +100/+200/+300 already baked into each entry's "value" (see
// framework.singe) -- so whichever of the four this matched, the caller can
// just read the entry's own "value" field and use it as-is; no separate
// bookkeeping needed for which pad it was.
bool isRecognizedPhysicalEntry(lua_State* L, int idx, const char* name) {
    // MOUSE_0 only, deliberately -- see main.cpp's SDL_MOUSEBUTTONDOWN/UP
    // handling for why MOUSE_1..3 (defined in framework.singe the same
    // way GAMEPAD_1..3 are, +100/+200/+300) are never actually reachable:
    // SDL2's plain mouse events don't distinguish which physical mouse
    // sent them, so there is no second/third mouse to assign those ids
    // to. Recognizing MOUSE_1..3 here would let a controls.cfg line
    // reference them without error while them never firing -- worse than
    // just not recognizing the table at all.
    static const char* const kTables[] = {"SCANCODE", "GAMEPAD_0", "GAMEPAD_1", "GAMEPAD_2", "GAMEPAD_3",
                                           "RAWJOY_0", "RAWJOY_1", "RAWJOY_2", "RAWJOY_3", "MOUSE_0"};
    for (const char* tableName : kTables) {
        lua_getglobal(L, tableName);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_getfield(L, -1, name);
        bool same = lua_rawequal(L, idx, -1);
        lua_pop(L, 2); // the <table>.name field, then <table> itself
        if (same) return true;
    }
    return false;
}

} // namespace

static void readBindings(lua_State* L, std::unordered_map<int, int>& map);

std::unordered_map<int, int> buildSwitchMap(lua_State* L, const std::string& gameDataPath,
                                             const std::string& sharedEngineRoot) {
    std::unordered_map<int, int> map;

    // altCfg is a real KimmyScript global (defaults to false in
    // globals.kimmy; an individual game's own top-level script can set it
    // to true before this runs) -- reading it here, rather than inventing
    // a separate signal, is what lets controls.cfg follow the exact same
    // convention service.kimmy/hscore.kimmy already use for their own
    // per-game Cfg/ files. Absent/non-boolean is treated as false, same as
    // Lua's own truthiness would leave it if a script never touches it.
    bool altCfg = false;
    lua_getglobal(L, "altCfg");
    if (lua_isboolean(L, -1)) altCfg = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    // fileExists()/loadFileOrArchive(): a .zdirk game's files may be only in its zip (see core/GameArchive.h).
    std::string controlsCfgPath = gameDataPath + "Cfg/controls.cfg";
    if (altCfg) {
        std::string altPath = gameDataPath + "Cfg/controls2.cfg";
        if (fileExists(altPath)) {
            controlsCfgPath = altPath;
        }
        // else: altCfg is set but this game only has the one controls.cfg
        // (nothing to differ between its two configs input-wise) -- fall
        // back to the plain name rather than firing no switches at all.
    }

    // (2026-10-01, Cyril) No more controls.cfg read from the game's own root
    // folder: the KimmyScript convention is Cfg/controls.cfg, and a file at
    // the root is in practice a Singe game's own one, written with Singe's
    // input names that KimmyScript's base.kimmy no longer defines -- it
    // would load with no binding at all instead of falling through to the
    // shared default below.
    if (!fileExists(controlsCfgPath)) {
        const std::string rootPath = gameDataPath + "controls.cfg";
        if (fileExists(rootPath)) {
            std::fprintf(stderr,
                "[dirk] note: %s is ignored (a game's controls.cfg goes in its Cfg/ folder)\n",
                rootPath.c_str());
        }
    }

    if (!fileExists(controlsCfgPath)) {
        // Last resort: the shared engine's own default controls.cfg
        // (KimmyScript/Cfg/, same fixed point dirk_settings.cfg already
        // lives at -- see main.cpp). Specifically for Hypseus-style games:
        // Hypseus uses its own, unrelated config file and so never ships a
        // per-game controls.cfg at all, which otherwise means "no mapping,
        // keyboard/gamepad fire nothing" for every such game -- this gives
        // it a sane shared default instead.
        //
        // 2026-09-22: this step used to log nothing at all when it didn't
        // find a file, which made a real-world miss (empty sharedEngineRoot,
        // or a file genuinely not at the expected path -- a hidden ".txt"
        // double extension being the classic real-world cause) silently
        // indistinguishable from "this build doesn't have the shared
        // fallback at all". Now it always says what it checked, so the next
        // DIRK_VERBOSE log settles the question directly instead of needing
        // another round trip to guess at it.
        if (sharedEngineRoot.empty()) {
            if (std::getenv("DIRK_VERBOSE")) {
                std::fprintf(stderr,
                    "[dirk] controls.cfg: no shared-engine root known, skipping the "
                    "KimmyScript/Cfg/controls.cfg fallback\n");
            }
        } else {
            std::string sharedPath = sharedEngineRoot + "/KimmyScript/Cfg/controls.cfg";
            if (fileExists(sharedPath)) {
                std::fprintf(stderr,
                    "[dirk] note: no controls.cfg for this game (checked %s) -- "
                    "using the shared default at %s instead\n",
                    controlsCfgPath.c_str(), sharedPath.c_str());
                controlsCfgPath = sharedPath;
            } else if (std::getenv("DIRK_VERBOSE")) {
                std::fprintf(stderr,
                    "[dirk] controls.cfg: shared fallback also not found at %s\n",
                    sharedPath.c_str());
            }
        }
    }

    if (!fileExists(controlsCfgPath)) {
        std::fprintf(stderr, "[dirk] no controls.cfg at %s -- keyboard will fire no logical switches\n",
                     controlsCfgPath.c_str());
        return map;
    }
    if (loadFileOrArchive(L, controlsCfgPath) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "[dirk] warning: failed to load %s: %s\n",
                     controlsCfgPath.c_str(), err ? err : "unknown error");
        lua_pop(L, 1);
        return map;
    }

    readBindings(L, map);

    // Routine confirmation, not a warning -- gated behind DIRK_VERBOSE like
    // every other informational print (see main.cpp's dirkVerbose()).
    if (std::getenv("DIRK_VERBOSE")) {
        std::fprintf(stderr, "[dirk] controls.cfg: %zu keyboard/gamepad->switch bindings loaded from %s\n",
                     map.size(), controlsCfgPath.c_str());
    }
    return map;
}

// The INPUT_* / alias globals a controls.cfg defines, read into `map`
// (physical id -> SWITCH_* id). Shared by buildSwitchMap() and
// buildForcedSwitchMap().
static void readBindings(lua_State* L, std::unordered_map<int, int>& map) {
    for (const SwitchBinding& b : kBindings) {
        lua_getglobal(L, b.inputGlobal);
        if (!lua_istable(L, -1) && b.alias) {
            // Primary INPUT_* name isn't defined in this controls.cfg --
            // try the friendlier alias (GO_UP, 1P_COIN, ...) before giving
            // up on this switch entirely. Tried in this order (primary
            // first) so an existing controls.cfg that happens to also
            // define a same-named alias global for some unrelated purpose
            // can't accidentally shadow the real binding.
            lua_pop(L, 1);
            lua_getglobal(L, b.alias);
        }
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        int len = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= len; ++i) {
            lua_rawgeti(L, -1, i);
            int entryIdx = lua_gettop(L);
            if (lua_istable(L, entryIdx)) {
                lua_getfield(L, entryIdx, "name");
                const char* entryName = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
                if (entryName && isRecognizedPhysicalEntry(L, entryIdx, entryName)) {
                    lua_getfield(L, entryIdx, "value");
                    if (lua_isnumber(L, -1)) {
                        int physicalId = static_cast<int>(lua_tointeger(L, -1));
                        map[physicalId] = b.switchId;
                    }
                    lua_pop(L, 1); // value
                }
                lua_pop(L, 1); // name
            }
            lua_pop(L, 1); // entry
        }
        lua_pop(L, 1); // INPUT_* table
    }
}

std::unordered_map<int, int> buildForcedSwitchMap(lua_State* L, const std::string& sharedEngineRoot) {
    std::unordered_map<int, int> map;
    lua_getglobal(L, "forcedControls");
    std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    if (name.empty()) return map;
    // A plain file name in KimmyScript/Cfg/, nothing else: a script can't
    // point this at some other file of the computer.
    if (name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos) {
        std::fprintf(stderr, "[dirk] warning: forcedControls \"%s\" ignored (a file name in KimmyScript/Cfg/ only)\n", name.c_str());
        return map;
    }
    if (sharedEngineRoot.empty()) return map;
    const std::string path = sharedEngineRoot + "/KimmyScript/Cfg/" + name;
    if (!fileExists(path)) {
        std::fprintf(stderr, "[dirk] note: no %s: no forced controls, the game's ones only\n", path.c_str());
        return map;
    }
    // The game's controls.cfg globals (GO_UP, BUTTON_1...) are still defined
    // at this point: set aside while the forced file loads, so that only the
    // lines it holds itself are read, then put back as they were.
    std::vector<const char*> names;
    for (const SwitchBinding& b : kBindings) {
        names.push_back(b.inputGlobal);
        if (b.alias) names.push_back(b.alias);
    }
    lua_newtable(L);
    int saved = lua_gettop(L);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_getglobal(L, names[i]);
        lua_rawseti(L, saved, static_cast<int>(i) + 1);
        lua_pushnil(L);
        lua_setglobal(L, names[i]);
    }
    if (loadFileOrArchive(L, path) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "[dirk] warning: failed to load %s: %s\n", path.c_str(), err ? err : "unknown error");
        lua_pop(L, 1);
    } else {
        readBindings(L, map);
    }
    for (size_t i = 0; i < names.size(); ++i) {
        lua_rawgeti(L, saved, static_cast<int>(i) + 1);
        lua_setglobal(L, names[i]);
    }
    lua_pop(L, 1); // the saved table
    std::fprintf(stderr, "[dirk] %zu forced binding(s) from %s, before the game's own\n", map.size(), path.c_str());
    return map;
}

namespace {

// Pushes a {name=<name>, value=<value>} table onto the stack -- the exact
// shape SCANCODE.*/GAMEPAD_N.* entries have, which is what
// isRecognizedPhysicalEntry() and controls.cfg's INPUT_* arrays expect.
void pushBindingEntry(lua_State* L, const std::string& name, int value) {
    lua_newtable(L);
    lua_pushstring(L, name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, value);
    lua_setfield(L, -2, "value");
}

} // namespace

const char* switchIdToInputName(int switchId) {
    for (const SwitchBinding& b : kBindings) {
        if (b.switchId == switchId) return b.inputGlobal;
    }
    return nullptr;
}

int inputNameToSwitchId(const char* name) {
    if (!name) return -1;
    for (const SwitchBinding& b : kBindings) {
        if (std::strcmp(b.inputGlobal, name) == 0) return b.switchId;
        if (b.alias && std::strcmp(b.alias, name) == 0) return b.switchId;
    }
    return -1;
}

const char* switchIdToAliasName(int switchId) {
    for (const SwitchBinding& b : kBindings) {
        if (b.switchId == switchId) return b.alias ? b.alias : b.inputGlobal;
    }
    return nullptr;
}

void registerRawJoystickTables(lua_State* L) {
    static const char* const kHatDirNames[4] = {"UP", "RIGHT", "DOWN", "LEFT"};

    for (int player = 0; player < 4; ++player) {
        lua_newtable(L); // RAWJOY_<player>
        int base = kRawJoyBase + player * kRawJoyPlayerStride;

        for (int axis = 0; axis < kRawJoyMaxAxes; ++axis) {
            std::string negName = "AXIS_" + std::to_string(axis) + "_NEG";
            pushBindingEntry(L, negName, base + axis * kRawJoyAxisStride + 1);
            lua_setfield(L, -2, negName.c_str());

            std::string posName = "AXIS_" + std::to_string(axis) + "_POS";
            pushBindingEntry(L, posName, base + axis * kRawJoyAxisStride + 2);
            lua_setfield(L, -2, posName.c_str());
        }

        for (int button = 0; button < kRawJoyMaxButtons; ++button) {
            std::string name = "BUTTON_" + std::to_string(button);
            pushBindingEntry(L, name, base + kRawJoyButtonOffset + button);
            lua_setfield(L, -2, name.c_str());
        }

        for (int hat = 0; hat < kRawJoyMaxHats; ++hat) {
            for (int dir = 0; dir < 4; ++dir) {
                std::string name = "HAT_" + std::to_string(hat) + "_" + kHatDirNames[dir];
                pushBindingEntry(L, name, base + kRawJoyHatOffset + hat * 4 + dir);
                lua_setfield(L, -2, name.c_str());
            }
        }

        std::string globalName = "RAWJOY_" + std::to_string(player);
        lua_setglobal(L, globalName.c_str());
    }
}

} // namespace dirk
