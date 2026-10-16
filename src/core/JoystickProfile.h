#pragma once
// JoystickProfile: a GUID-keyed, native (not controls.cfg/Lua-authored)
// record of which raw button/axis/hat index on a SPECIFIC physical
// joystick fires which SWITCH_* id -- written by the in-engine joystick
// setup wizard (src/core/JoystickWizard.h) so a non-technical installer
// never has to run DIRK_DEBUG_JOYSTICK, read console output, or hand-edit
// controls.cfg's RAWJOY_0..3 entries at all.
//
// Why GUID-keyed rather than reusing RAWJOY_0..3's playerIndex (connection-
// order-based) numbering: a real cabinet can have several similar
// unrecognized raw devices at once (e.g. two Sinden lightguns AND the
// actual arcade stick, all four showing up as "unrecognized joystick" --
// the exact case this was built and verified against). Which of those
// lands in RAWJOY_0 vs RAWJOY_2 depends on the order SDL/Windows happens
// to enumerate them at that boot, which is NOT guaranteed stable --
// confirmed by this engine's own real-world testing, not assumed. Keying
// this profile on the device's own GUID instead makes a saved profile
// keep working for "that exact physical joystick" no matter which
// RAWJOY_N slot it happens to enumerate into on a given run.
//
// This is a second, independent way for a raw joystick event to resolve
// to a SWITCH_* id, alongside (not replacing) the existing RAWJOY_0..3 /
// controls.cfg numeric-id path: main.cpp checks a connected raw
// joystick's GUID against this profile FIRST; anything not covered here
// (an entry the wizard's own timeout skipped, or a device with no profile
// at all) still falls back to whatever RAWJOY_N.* controls.cfg binds by
// hand, exactly as before this file existed. Nothing about the manual
// path changes or is required to keep working.
//
// One profile file covers every game a given Dirk+KimmyScript install
// runs (see defaultPath()) -- a real cabinet's physical buttons don't
// change per game, so configuring a device once should not need repeating
// per game folder.

#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace dirk {

struct JoystickBinding {
    std::string guid;  // SDL_JoystickGetGUIDString() -- the actual match key
    std::string name;   // display/debug only, never matched on (device names aren't reliably unique or stable)

    // Raw SDL button/axis/hat index -> SWITCH_* id. -1 (or absent) means
    // "not bound". Axis/hat directions mirror RAWJOY_0..3's own NEG/POS
    // and UP/DOWN/LEFT/RIGHT split (see InputMap.h).
    std::unordered_map<int, int> buttons;

    struct AxisDir {
        int neg = -1;
        int pos = -1;
    };
    std::unordered_map<int, AxisDir> axes;

    struct HatDir {
        int up = -1;
        int down = -1;
        int left = -1;
        int right = -1;
    };
    std::unordered_map<int, HatDir> hats;
};

// Builds the key a device's profile is actually stored/looked up under:
// `guid` alone, or "<guid>#<path>" when `path` is non-null/non-empty.
//
// Why: confirmed for real (2026-09-24, via SDL_AttachVirtualJoystick with
// two virtual devices given identical vendor_id/product_id/name/type --
// see this delivery's probe.cpp) that SDL_GetJoystickGUID() produces a
// BYTE-IDENTICAL guid for two devices that only differ by which physical
// USB port they're plugged into. This is exactly Cyril's real cabinet
// report: two of the same model of raw joystick/encoder both show up in
// the wizard's PickDevice list (each has its own SDL_JoystickID/instance),
// but only ONE ends up in the saved profile file -- because
// JoystickProfileStore's upsert()/find() match on `guid` alone, so the
// second device's save silently overwrites the first's entry under the
// exact same key. `path` (SDL_GetJoystickPathForID/SDL_GetJoystickPath,
// SDL_GetGamepadPathForID/SDL_GetGamepadPath) is the OS-level device path
// -- effectively identifies the physical port, which two identical
// boards plugged into two different USB ports do NOT share, unlike their
// GUID. Callers pass whichever of those path functions applies; nullptr
// is handled the same as an empty path (falls back to the plain guid --
// this engine's exact behavior before this existed, so an install with
// no colliding devices, or a platform/virtual-device case where no path
// is available at all, is completely unaffected).
std::string buildProfileKey(const std::string& guid, const char* path);

// Resolves one of the optional loose assets shared across every game a
// given Dirk+KimmyScript install runs (dirk_loading.png, dirk_loading_font.ttf)
// against sharedEngineRoot. Not specific to joystick profiles at all --
// kept in this header purely because it's already the one common include
// between main.cpp and JoystickWizard.cpp for sharedEngineRoot-relative
// paths, same as buildProfileKey()/defaultPath() just above/below.
//
// 2026-09-23 (Cyril, after moving dirk_loading_font.ttf next to dirk.exe --
// the only place this used to look -- to fix the F9 hang): "je préférerai
// qu'on le mette ailleurs si c'est possible pour pas pourrir la racine".
// Checks <sharedEngineRoot>/KimmyScript/Cfg/<filename> FIRST -- the exact
// folder dirk_settings.cfg and dirk_joystick_profiles.cfg already live in
// (defaultPath() below), and where Cyril himself tried putting the font
// before asking -- then falls back to the bare <sharedEngineRoot>/<filename>
// spot these two files used before this existed, so an install that still
// has them loose at the root keeps working with zero forced migration.
std::string resolveSharedAsset(const std::string& sharedEngineRoot, const std::string& filename);

class JoystickProfileStore {
public:
    // sharedEngineRoot is the same fixed point dirk_settings.cfg already
    // uses (see main.cpp) -- shared by every game a given install runs.
    static std::string defaultPath(const std::string& sharedEngineRoot);

    // 2026-09-24 (Cyril: "je veux que dans le premier menu tu demandes si
    // on fait la configuration pour CE jeu ... ou en general"): a SECOND,
    // per-game profile file, checked before the shared one above -- lives
    // right next to that game's own Cfg/game.cfg (gameFolder is
    // ctx.scriptPath, the running game's own folder), so it's private to
    // that one game and never touches any other game's bindings or the
    // install-wide file. Only created if the player actually picks "this
    // game only" in the wizard's new first screen (see JoystickWizard.h) --
    // a game that's never had a per-game profile saved simply has no file
    // here, and JoystickProfileStore::load() already treats a missing file
    // as "empty store", same as it always has.
    static std::string defaultPathForGame(const std::string& gameFolder);

    // Loads `path` into this store (replacing anything already loaded).
    // Uses `L` only as a throwaway Lua interpreter to parse the file's
    // Lua-table syntax -- defines and then clears a temporary
    // DIRK_JOYSTICK_PROFILES global, same defensive load pattern as
    // buildSwitchMap(). Missing file or parse error is not fatal: logs a
    // warning and leaves the store empty, exactly like buildSwitchMap()
    // treats a missing/broken controls.cfg -- callers should treat "no
    // profile" as "fall back to controls.cfg", not a crash.
    void load(lua_State* L, const std::string& path);

    // Writes every currently-held binding back to `path` as a human-
    // readable Lua table (hand-formatted, not a general serializer -- the
    // schema is fixed and small). Returns false if the file couldn't be
    // opened for writing.
    bool save(const std::string& path) const;

    // nullptr if `guid` has no saved profile. `guid` may be a plain GUID
    // or a buildProfileKey()-style "<guid>#<path>" composite; if the exact
    // key isn't found and it contains "#", the part before "#" is also
    // tried -- so a profile saved under the old plain-GUID scheme (any
    // device with no path-based collision, i.e. almost everything already
    // configured before per-port disambiguation existed) keeps resolving
    // with no migration step.
    const JoystickBinding* find(const std::string& guid) const;

    // Replaces any existing entry with the same guid (by design: re-
    // running the wizard for an already-configured device is meant to
    // start that device's bindings fresh, not merge with stale ones).
    // When `binding.guid` is a composite "<guid>#<path>" key, also erases
    // any leftover entry saved under the bare guid for the same device
    // (from before this device's profile was ever disambiguated), so
    // re-running the wizard for it doesn't leave a stale orphaned entry
    // behind in the file.
    void upsert(const JoystickBinding& binding);

    // Removes any existing entry for `guid`, if one exists -- both the
    // exact key and, same fallback as find(), its bare-guid form if
    // `guid` is a composite key. Returns true if something was actually
    // removed. This is the wizard's "undo" at the device level (see
    // JoystickWizard.h): erasing a profile reverts that one device to
    // whatever it did before the wizard ever touched it -- plain
    // RAWJOY_N.* resolution through controls.cfg, exactly as if it had
    // never been configured -- without affecting any other device's
    // saved profile.
    bool remove(const std::string& guid);

    const std::vector<JoystickBinding>& all() const { return bindings_; }

private:
    std::vector<JoystickBinding> bindings_;
};

} // namespace dirk
