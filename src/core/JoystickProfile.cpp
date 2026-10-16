#include "core/JoystickProfile.h"
#include "lua/InputMap.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

namespace dirk {

namespace {

bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// Reads a {[rawIndex] = "INPUT_NAME", ...} table at the top of the stack
// into `out`, converting each name through inputNameToSwitchId(). Silently
// skips an entry whose name isn't recognized (rather than failing the
// whole profile) since a rebuilt engine dropping/renaming one switch
// should degrade a saved profile, not break it outright.
void readIndexToSwitchTable(lua_State* L, int idx, std::unordered_map<int, int>& out) {
    if (!lua_istable(L, idx)) return;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        // key at -2, value at -1
        if (lua_isnumber(L, -2) && lua_isstring(L, -1)) {
            int rawIndex = static_cast<int>(lua_tointeger(L, -2));
            int switchId = inputNameToSwitchId(lua_tostring(L, -1));
            if (switchId >= 0) out[rawIndex] = switchId;
        }
        lua_pop(L, 1); // keep key for lua_next
    }
}

int readSwitchField(lua_State* L, int tblIdx, const char* field) {
    lua_getfield(L, tblIdx, field);
    int result = -1;
    if (lua_isstring(L, -1)) {
        int switchId = inputNameToSwitchId(lua_tostring(L, -1));
        if (switchId >= 0) result = switchId;
    }
    lua_pop(L, 1);
    return result;
}

void readAxesTable(lua_State* L, int idx, std::unordered_map<int, JoystickBinding::AxisDir>& out) {
    if (!lua_istable(L, idx)) return;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_isnumber(L, -2) && lua_istable(L, -1)) {
            int axisIndex = static_cast<int>(lua_tointeger(L, -2));
            int entryIdx = lua_gettop(L);
            JoystickBinding::AxisDir dir;
            dir.neg = readSwitchField(L, entryIdx, "neg");
            dir.pos = readSwitchField(L, entryIdx, "pos");
            out[axisIndex] = dir;
        }
        lua_pop(L, 1);
    }
}

void readHatsTable(lua_State* L, int idx, std::unordered_map<int, JoystickBinding::HatDir>& out) {
    if (!lua_istable(L, idx)) return;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_isnumber(L, -2) && lua_istable(L, -1)) {
            int hatIndex = static_cast<int>(lua_tointeger(L, -2));
            int entryIdx = lua_gettop(L);
            JoystickBinding::HatDir dir;
            dir.up = readSwitchField(L, entryIdx, "up");
            dir.down = readSwitchField(L, entryIdx, "down");
            dir.left = readSwitchField(L, entryIdx, "left");
            dir.right = readSwitchField(L, entryIdx, "right");
            out[hatIndex] = dir;
        }
        lua_pop(L, 1);
    }
}

// Lua identifier quoting for `name`/`guid` -- both come from
// SDL_JoystickName()/GUIDString(), never from this engine's own fixed
// vocabulary, so unlike controls.cfg's INPUT_* names these must be treated
// as arbitrary text (a joystick can legitimately be named something with
// quotes or backslashes in it).
std::string luaQuote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

// The part of a possibly-composite "<guid>#<path>" key before "#" -- see
// buildProfileKey()'s own comment in JoystickProfile.h. Returns `key`
// unchanged if it has no "#".
std::string baseGuidOf(const std::string& key) {
    auto pos = key.find('#');
    return pos == std::string::npos ? key : key.substr(0, pos);
}

} // namespace

std::string buildProfileKey(const std::string& guid, const char* path) {
    if (!path || path[0] == '\0') return guid;
    return guid + "#" + path;
}

std::string resolveSharedAsset(const std::string& sharedEngineRoot, const std::string& filename) {
    const std::string preferred = sharedEngineRoot + "/KimmyScript/Cfg/" + filename;
    if (fileExists(preferred)) return preferred;
    return sharedEngineRoot + "/" + filename; // old location, kept as a fallback -- see header comment
}

std::string JoystickProfileStore::defaultPath(const std::string& sharedEngineRoot) {
    return sharedEngineRoot + "/KimmyScript/Cfg/dirk_joystick_profiles.cfg";
}

std::string JoystickProfileStore::defaultPathForGame(const std::string& gameFolder) {
    // Same filename, same Cfg/ subfolder convention as the per-game
    // game.cfg (see service.kimmy's writeConfig/readConfig) -- just under
    // the game's own folder instead of sharedEngineRoot.
    return gameFolder + "/Cfg/dirk_joystick_profiles.cfg";
}

void JoystickProfileStore::load(lua_State* L, const std::string& path) {
    bindings_.clear();
    if (!fileExists(path)) return; // no profile yet -- not an error, see header

    if (luaL_loadfile(L, path.c_str()) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "[dirk] warning: failed to load %s: %s -- raw joystick input falls back to "
                     "controls.cfg's RAWJOY_0..3 bindings, if any\n",
                     path.c_str(), err ? err : "unknown error");
        lua_pop(L, 1);
        return;
    }

    lua_getglobal(L, "DIRK_JOYSTICK_PROFILES");
    if (lua_istable(L, -1)) {
        int len = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= len; ++i) {
            lua_rawgeti(L, -1, i);
            int entryIdx = lua_gettop(L);
            if (lua_istable(L, entryIdx)) {
                JoystickBinding b;
                lua_getfield(L, entryIdx, "guid");
                if (lua_isstring(L, -1)) b.guid = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, entryIdx, "name");
                if (lua_isstring(L, -1)) b.name = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, entryIdx, "buttons");
                readIndexToSwitchTable(L, lua_gettop(L), b.buttons);
                lua_pop(L, 1);
                lua_getfield(L, entryIdx, "axes");
                readAxesTable(L, lua_gettop(L), b.axes);
                lua_pop(L, 1);
                lua_getfield(L, entryIdx, "hats");
                readHatsTable(L, lua_gettop(L), b.hats);
                lua_pop(L, 1);
                if (!b.guid.empty()) bindings_.push_back(std::move(b));
            }
            lua_pop(L, 1); // entry
        }
    }
    lua_pop(L, 1); // DIRK_JOYSTICK_PROFILES (or nil)

    // Don't leak this into whatever the game script chain sees next --
    // same reasoning as controls.cfg's own globals being plain script-
    // scope pollution nobody asked for.
    lua_pushnil(L);
    lua_setglobal(L, "DIRK_JOYSTICK_PROFILES");

    if (std::getenv("DIRK_VERBOSE")) {
        std::fprintf(stderr, "[dirk] joystick profiles: %zu device(s) loaded from %s\n",
                     bindings_.size(), path.c_str());
    }
}

bool JoystickProfileStore::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "[dirk] warning: couldn't write joystick profile to %s\n", path.c_str());
        return false;
    }

    out << "-- Auto-generated by Dirk's joystick setup wizard (F9 in-game). Safe to\n";
    out << "-- delete (or erase one device's entry from the wizard's own device\n";
    out << "-- list, Suppr key) to force that device back to controls.cfg's plain\n";
    out << "-- RAWJOY_0..3 bindings, or hand-edit using either these alias names\n";
    out << "-- (GO_UP, COIN_1P, EXIT_MENU, ...) or the primary INPUT_* names --\n";
    out << "-- both are accepted -- see README.\n";
    out << "DIRK_JOYSTICK_PROFILES = {\n";
    for (const JoystickBinding& b : bindings_) {
        out << "  {\n";
        out << "    guid = " << luaQuote(b.guid) << ",\n";
        out << "    name = " << luaQuote(b.name) << ",\n";
        out << "    buttons = {";
        bool first = true;
        for (const auto& [rawIndex, switchId] : b.buttons) {
            // switchIdToAliasName(), not switchIdToInputName(): Cyril's
            // explicit request (2026-09-24, "utilise mes alias... sur le
            // fichier de cfg") -- see InputMap.h's doc comment on it.
            const char* name = switchIdToAliasName(switchId);
            if (!name) continue;
            if (!first) out << ",";
            out << " [" << rawIndex << "] = " << luaQuote(name);
            first = false;
        }
        out << " },\n";
        out << "    axes = {";
        first = true;
        for (const auto& [axisIndex, dir] : b.axes) {
            if (!first) out << ",";
            out << " [" << axisIndex << "] = { ";
            if (dir.neg >= 0) { if (const char* n = switchIdToAliasName(dir.neg)) out << "neg = " << luaQuote(n) << ", "; }
            if (dir.pos >= 0) { if (const char* n = switchIdToAliasName(dir.pos)) out << "pos = " << luaQuote(n) << ", "; }
            out << "}";
            first = false;
        }
        out << " },\n";
        out << "    hats = {";
        first = true;
        for (const auto& [hatIndex, dir] : b.hats) {
            if (!first) out << ",";
            out << " [" << hatIndex << "] = { ";
            if (dir.up >= 0) { if (const char* n = switchIdToAliasName(dir.up)) out << "up = " << luaQuote(n) << ", "; }
            if (dir.down >= 0) { if (const char* n = switchIdToAliasName(dir.down)) out << "down = " << luaQuote(n) << ", "; }
            if (dir.left >= 0) { if (const char* n = switchIdToAliasName(dir.left)) out << "left = " << luaQuote(n) << ", "; }
            if (dir.right >= 0) { if (const char* n = switchIdToAliasName(dir.right)) out << "right = " << luaQuote(n) << ", "; }
            out << "}";
            first = false;
        }
        out << " },\n";
        out << "  },\n";
    }
    out << "}\n";
    return true;
}

const JoystickBinding* JoystickProfileStore::find(const std::string& guid) const {
    for (const JoystickBinding& b : bindings_) {
        if (b.guid == guid) return &b;
    }
    // Fallback for a composite "<guid>#<path>" key: a profile saved
    // before this device's path was known to collide with anything is
    // still stored under the bare guid -- see buildProfileKey()'s comment.
    const std::string base = baseGuidOf(guid);
    if (base != guid) {
        for (const JoystickBinding& b : bindings_) {
            if (b.guid == base) return &b;
        }
    }
    return nullptr;
}

void JoystickProfileStore::upsert(const JoystickBinding& binding) {
    bool replaced = false;
    for (JoystickBinding& b : bindings_) {
        if (b.guid == binding.guid) {
            b = binding;
            replaced = true;
            break;
        }
    }
    if (!replaced) bindings_.push_back(binding);

    // Composite key: drop any stale entry left over under the bare guid
    // for this same device (see upsert()'s own header comment).
    const std::string base = baseGuidOf(binding.guid);
    if (base != binding.guid) {
        for (size_t i = 0; i < bindings_.size();) {
            if (bindings_[i].guid == base) {
                bindings_.erase(bindings_.begin() + static_cast<long>(i));
            } else {
                ++i;
            }
        }
    }
}

bool JoystickProfileStore::remove(const std::string& guid) {
    const std::string base = baseGuidOf(guid);
    bool removedAny = false;
    for (size_t i = 0; i < bindings_.size();) {
        if (bindings_[i].guid == guid || bindings_[i].guid == base) {
            bindings_.erase(bindings_.begin() + static_cast<long>(i));
            removedAny = true;
        } else {
            ++i;
        }
    }
    return removedAny;
}

} // namespace dirk
