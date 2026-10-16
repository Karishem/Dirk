#pragma once
// InputMap: translates SDL scancodes into the logical switch IDs
// KimmyScript's onInputPressed(what)/onInputReleased(what) actually expect.
//
// IMPORTANT CORRECTION to an earlier assumption (see git history / design
// doc): `what` is NOT a raw SDL scancode. Reading moves.singe's real
// onInputPressed(intWhat) shows it compares intWhat against SWITCH_UP,
// SWITCH_START1, SWITCH_COIN1, etc. -- framework.singe's own small
// enumeration (SWITCH_UP=0 ... SWITCH_TILT=22), not SDL2 scancode values.
// The SCANCODE table (which DOES match SDL2 scancodes) is instead consumed
// by controls.cfg (`INPUT_UP = { SCANCODE.UP, GAMEPAD_0.DPAD_UP, ... }`) to
// describe *physical* bindings. Something has to translate physical input
// into a logical SWITCH_* id before calling onInputPressed -- in real
// Singe that's native engine code; here, it's this file.
//
// The INPUT_* -> SWITCH_* correspondence itself is NOT written down
// anywhere in the KimmyScript/Singe files or manual we have -- it's
// reconstructed by matching names (INPUT_1P_COIN inserts coin 1 ->
// SWITCH_COIN1, INPUT_ACTION_1..4 -> SWITCH_BUTTON1..4, etc.). 22 of
// controls.cfg's 23 entries match a SWITCH_* constant one-to-one this way
// (INPUT_GRAB is the one leftover -- no SWITCH_GRAB exists, so it's
// dropped). Treat this mapping as inferred, not confirmed.

struct lua_State;

#include <string>
#include <unordered_map>

namespace dirk {

// Loads `<gameDataPath>Cfg/controls.cfg` (or, when the game's own script
// set the real KimmyScript `altCfg` global to true before this runs, tries
// `<gameDataPath>Cfg/controls2.cfg` first, falling back to the plain name if
// that specific file doesn't exist). A controls.cfg in the game's own
// root folder is NOT read (2026-10-01, Cyril: in practice that's a Singe
// game's own file, with Singe's input names; a note is logged instead).
//
// If neither Cfg/ candidate exists, and `sharedEngineRoot` is non-empty,
// tries once more at `<sharedEngineRoot>/KimmyScript/Cfg/controls.cfg` --
// the same shared-engine location dirk_settings.cfg already lives at (see
// main.cpp). Added specifically for Hypseus-style games (a real .m2v/.ogg
// pair remuxed by Dirk itself, see HypseusRemux.cpp): Hypseus has its own,
// different config file and so never ships a per-game controls.cfg at all,
// which used to mean such a game silently got no keyboard/gamepad bindings.
// altCfg/controls2.cfg don't apply here -- that distinction is meaningful
// only for a specific game's own two configs, not for one shared default
// meant to cover any game that has nothing more specific.
//
// Each INPUT_* global also has a friendlier alias (GO_UP for INPUT_UP,
// COIN_1P for INPUT_1P_COIN, BUTTON_1 for INPUT_ACTION_1, ... -- see
// kBindings in InputMap.cpp for the complete list), tried only when the
// primary INPUT_* name isn't defined in whichever file actually got
// loaded. Existing controls.cfg files that only use the original INPUT_*
// names are completely unaffected; the alias exists so a NEW, simplified
// controls.cfg -- such as the shared KimmyScript/Cfg/ default above -- can
// use shorter, clearer names without dirk needing two entirely separate
// parsers. Otherwise plain Lua, using the SCANCODE/
// GAMEPAD_0..3/RAWJOY_0..3 tables that must already be defined, which is
// also exactly why this must be called only after the game's own script
// chain has run: that's the same point at which altCfg, if the game sets
// it at all, is already known. altCfg/the "2" suffix aren't a dirk
// invention -- confirmed by reading the real service.kimmy/hscore.kimmy:
// that's precisely how they already pick between e.g. Cfg/game.cfg and
// Cfg/game2.cfg for a folder shared by two .dirk configs (Chantze Stone /
// Triad Stone is the concrete case this was added for) -- controls.cfg
// just follows the same, already-established convention instead of a new
// one. Returns a single map, keyed by "physical id", to logical SWITCH_*
// id. A physical id is a raw SDL scancode (from a SCANCODE.* entry), one of
// framework.singe's own GAMEPAD_N.* numeric ids (500-532 for pad 0, +100/
// +200/+300 for pads 1-3, see framework.singe -- e.g. GAMEPAD_0.BUTTON_A's
// "value" is 518), or one of Dirk's own native RAWJOY_N.* numeric ids
// (2000-2399, see kRawJoyBase/registerRawJoystickTables below). These
// numeric ranges never overlap (scancodes are 0-~283; gamepad ids are
// 500-832; raw-joystick ids are 2000-2399), so one unordered_map safely
// covers all three without a separate "kind" tag -- see main.cpp's
// SDL_CONTROLLERBUTTONDOWN/UP, SDL_CONTROLLERAXISMOTION, and
// SDL_JOYBUTTONDOWN/UP/AXISMOTION/HATMOTION handling for how a real
// controller/joystick event gets turned into the same numeric id space
// before this map is consulted.
// Only entries that are literally the same table object as one of
// SCANCODE's, GAMEPAD_0..3's, or RAWJOY_0..3's entries are taken --
// controls.cfg's arrays also contain MOUSE_0.* entries (same {name,value}
// shape, different table), which are deliberately skipped since Dirk
// doesn't implement mouse-as-switch input yet.
//
// Returns an empty map (not an error) if none of the candidate files exist
// or none defines any of the recognized INPUT_* globals -- callers should
// treat "no mapping" as "no switches fire", not a fatal condition.
// `sharedEngineRoot` is optional (defaults to empty, meaning "don't try the
// shared-engine fallback at all") so existing callers/tests that don't have
// it handy keep compiling unchanged.
std::unordered_map<int, int> buildSwitchMap(lua_State* L, const std::string& gameDataPath,
                                             const std::string& sharedEngineRoot = std::string());

// Forced bindings (2026-10-08, Cyril: Create needs its own "forced"
// buttons, B3 above all, which most of his games leave unbound): reads a
// controls.cfg-style file at `path` (same INPUT_* / alias names, same
// SCANCODE/GAMEPAD_N/RAWJOY_N entries) into its own map, without the game's
// own controls.cfg globals leaking in: only the lines the file itself holds
// count. main.cpp checks this map BEFORE the joystick profiles and the
// game's switchMap, so a physical input listed here always fires the
// switch it names. Loaded only when the running script sets the Lua
// global `forcedControls` to a plain file name, looked for in
// <sharedEngineRoot>/KimmyScript/Cfg/ (create.kimmy sets
// "create_controls.cfg"). No global, a name with a path in it, or no such
// file: empty map, nothing changes.
std::unordered_map<int, int> buildForcedSwitchMap(lua_State* L, const std::string& sharedEngineRoot);

// RAWJOY_0..3: a generic, hardware-agnostic fallback for any joystick SDL
// doesn't recognize as a standard game controller (SDL_IsGameController()
// == false) -- the common case for DIY/generic arcade encoders, which never
// match SDL's built-in controller mapping database and so never fire
// SDL_CONTROLLERDEVICEADDED/BUTTONDOWN/AXISMOTION at all, only the raw
// SDL_JOYDEVICEADDED/BUTTONDOWN/AXISMOTION/HATMOTION events. Rather than
// hardcoding a mapping for one specific GUID (which would only ever work
// for that exact hardware), Dirk exposes every such joystick's raw
// button/axis/hat indices as {name=,value=} entries -- exactly like
// SCANCODE/GAMEPAD_0..3 -- so any unrecognized joystick can be wired up
// from controls.cfg alone (after a DIRK_DEBUG_JOYSTICK=1 run to read off
// its raw indices), with no engine rebuild and no per-device C++ code.
//
// Numeric scheme (mirrors GAMEPAD_0..3's own base/player-stride/axis-stride
// layout in framework.singe -- see main.cpp's kGamepadBase et al.):
//   RAWJOY_<player> covers kRawJoyBase + player*kRawJoyPlayerStride ..
//   + kRawJoyPlayerStride - 1 (player 0-3, i.e. 2000-2099, 2100-2199,
//   2200-2299, 2300-2399).
//   Within a player's range:
//     - axis directions: for axis 0..kRawJoyMaxAxes-1, ids
//       (axis*kRawJoyAxisStride)+1 = negative direction,
//       (axis*kRawJoyAxisStride)+2 = positive direction (relative offset 0
//       is reserved/unused, matching GAMEPAD_N's own axis-id scheme).
//     - buttons: kRawJoyButtonOffset + raw SDL button index, for
//       0..kRawJoyMaxButtons-1.
//     - hats: kRawJoyHatOffset + hat*4 + direction (0=up,1=right,2=down,
//       3=left), for hat 0..kRawJoyMaxHats-1.
constexpr int kRawJoyBase = 2000;
constexpr int kRawJoyPlayerStride = 100;
constexpr int kRawJoyAxisStride = 3;
constexpr int kRawJoyMaxAxes = 8;
constexpr int kRawJoyButtonOffset = 24; // = kRawJoyMaxAxes * kRawJoyAxisStride
constexpr int kRawJoyMaxButtons = 32;
constexpr int kRawJoyHatOffset = 56;    // = kRawJoyButtonOffset + kRawJoyMaxButtons
constexpr int kRawJoyMaxHats = 4;

// Injects the native RAWJOY_0..3 globals described above into `L`. Call
// this once, right after the Lua state is opened and registerSingeApi()
// has run -- before any game script or controls.cfg loads, so those globals
// already exist by the time controls.cfg's INPUT_* arrays might reference
// RAWJOY_0.BUTTON_3 or similar (exactly like SCANCODE/GAMEPAD_0..3, which
// framework.kimmy defines just as early in the script chain). Unconditional
// and independent of whether any unrecognized joystick is actually plugged
// in -- same as SCANCODE/GAMEPAD_0..3 being defined whether or not a
// keyboard/gamepad is present.
void registerRawJoystickTables(lua_State* L);

// Human-readable INPUT_* name for a SWITCH_* id, and its reverse -- the
// exact correspondence buildSwitchMap()'s own kBindings table already
// holds, exposed here so other native code (src/core/JoystickProfile.*'s
// GUID-based wizard-generated profile format) can save/load switch ids as
// readable, portable text instead of bare numbers. Returns nullptr /-1 for
// an id or name kBindings doesn't cover.
const char* switchIdToInputName(int switchId);
int inputNameToSwitchId(const char* name);

// Same lookup as switchIdToInputName(), but returns kBindings' friendlier
// alias (GO_UP, COIN_1P, EXIT_MENU, ...) when that switchId has one,
// falling back to the primary INPUT_* name only for an entry with none.
// Added 2026-09-24 at Cyril's explicit request, clarified the same day:
// "je ne veux pas utiliser mes ALIAS dans les textes de F9 mais sur le
// fichier de cfg" -- he wants his aliases used when WRITING the saved
// joystick profile file (see JoystickProfile.cpp's save(), the only
// caller), not shown in the wizard's own on-screen text. Read-side is
// unaffected: inputNameToSwitchId() already accepts either form, so a
// profile written with this function's names, one written with the
// primary INPUT_* names (before this existed), and a hand-edited file
// mixing both all keep loading correctly with no migration step.
const char* switchIdToAliasName(int switchId);

} // namespace dirk
