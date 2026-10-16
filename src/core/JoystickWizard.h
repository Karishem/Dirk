#pragma once
// JoystickWizard: native (not KimmyScript/Lua) in-engine flow for binding
// one specific joystick's buttons/axes(/hats) to Dirk's SWITCH_* logical
// inputs, and saving the result into a JoystickProfile (see
// JoystickProfile.h) keyed by that device's GUID -- so a non-technical
// installer never has to run DIRK_DEBUG_JOYSTICK, read console output, or
// hand-edit controls.cfg's RAWJOY_0..3/GAMEPAD_0..3 entries at all. This
// is the wizard JoystickProfile.h's own header comment already
// anticipated (it names this exact file) before any of it existed.
// Handles both an unrecognized raw joystick (the original case) and a
// recognized SDL gamepad whose factory-default mapping doesn't suit a
// given game (added 2026-09-23 -- see the field report further down) --
// DeviceInfo::kind picks which.
//
// Requirements set by Cyril when greenlighting this (2026-09-23):
//   - "on se reserve l'option de revenir en arriere si ce n'est pas
//     satisfaisant" -- satisfied two ways: (1) Retour arriere (Backspace)
//     during capture redoes the current or previous binding without
//     restarting the whole flow; (2) NOTHING is written to disk until the
//     final Confirm step is explicitly accepted -- Echap at any earlier
//     point cancels the whole run and leaves any existing saved profile
//     for that device completely untouched. On top of that, the device-
//     picker screen can also erase an already-saved profile outright
//     (Suppr), reverting that one device to exactly how it behaved before
//     the wizard ever touched it (plain RAWJOY_N.*/controls.cfg
//     resolution) -- the full undo, usable any time, not just mid-run.
//   - "il faut aussi prevoir un timeout si le bouton n'est pas present
//     (toutes les bornes d'arcade n'ont pas 12 boutons disponibles)" --
//     every binding prompt times out on its own (kPromptTimeoutMs) and
//     silently moves on, exactly like an unanswered prompt should; Entree
//     also skips a prompt immediately, for when the installer already
//     knows a given button doesn't exist on this cabinet and doesn't want
///    to wait out the timeout.
//
// Purely native: driven straight off SDL joystick/keyboard events and
// rendered with the same TTF_RenderText_Blended() approach main.cpp's own
// loading screen already uses (see JoystickWizard.cpp) -- no Lua/
// KimmyScript involvement, so the exact same wizard works identically for
// every game, and it freezes the picture (ctx.discPaused, main.cpp) while
// open, the same native pause mechanism the P/Pause button already uses.
//
// 2026-09-23 field report (Cyril, real cabinet, Hayate_HD): F9 with no
// unrecognized joystick connected appeared to hang the game completely.
// The real cause, confirmed from the log he sent: dirk_loading_font.ttf
// wasn't found at his sharedEngineRoot (a pre-existing install gap that
// had been silently harmless until now -- the loading screen's own
// "INDEXING..." label degrades the exact same way, with no warning
// printed for it at all, and nobody had ever noticed since nothing there
// waits on the player). This wizard was the first thing to both need
// on-screen text someone has to read AND block on a key they have no way
// to discover without reading it -- so a missing font silently turned
// "nothing to configure, press Echap" into what looks, to the player, like
// the game is frozen for good. Two independent fixes, so this can't
// recur even on another install with the same gap: (1) every screen now
// ALSO draws through SDL_RenderDebugText() -- a tiny built-in bitmap font
// SDL itself ships with no external file at all -- whenever the real TTF
// font failed to load, so there is always SOME readable text, never a
// silent dark screen; (2) the device-picker screen auto-closes on its own
// after kEmptyListTimeoutMs when there is nothing to configure at all
// (nothing is lost by that -- there was nothing to lose), so that
// specific dead end can no longer trap anyone even if some future issue
// broke text rendering entirely again. All wizard text is plain ASCII
// (accents removed 2026-09-23) specifically because SDL_RenderDebugText
// only renders ASCII -- keeping one text source for both rendering paths
// rather than a second, accent-free copy that could quietly drift out of
// sync with the real one.
//
// 2026-09-24, third change (Cyril, after confirming the two-controllers
// GUID-collision fix worked on the real cabinet): "il faudrait une option
// pour changer le calibrage... a placer dans cfg (par jeu) ou au general".
// Clarified the same day, since my first framing (a menu entry to relaunch
// the wizard) missed the actual ask: "Quand tu lances le wizard dans un jeu
// je veux que dans le premier menu tu demandes si on fait la configuration
// pour CE jeu (qu'on enregistrera dans jeu/cfg) ou en general (Kimmyscript
// ...)". So: the wizard's very first screen, right when F9 is pressed and
// BEFORE the existing device-picker (Step::PickScope, new), now asks
// exactly that -- "This game only" saves into a profile file living next
// to that one game's own Cfg/game.cfg (JoystickProfileStore::
// defaultPathForGame(), JoystickProfile.h), "General (all games)" keeps
// saving into the same shared install-wide file as before this existed.
// Whichever is picked becomes the ACTIVE store/path for the rest of that
// wizard run (device list, already-configured tag, Suppr, Confirm/save --
// see activeProfiles_/activePath_ below); nothing here changes the runtime
// RESOLUTION order (main.cpp: per-game checked first, then general, then
// switchMap/controls.cfg -- see main.cpp's findJoystickProfile()), only
// where THIS run's save goes. Skipped automatically (straight to
// PickDevice, general scope, exactly the old behavior) when there's no
// real per-game folder to offer "this game" against (gameFolder empty --
// the legacy bare-video launch mode) or nothing is even connected to
// configure (devices_ empty) -- see open()'s own comment.
//
// 2026-09-23, second change same day (Cyril): "l'idee est de remapper ses
// touches si le mapping par defaut ne convient pas" -- this wizard used to
// refuse every SDL-recognized ("standard") gamepad outright (see the old
// comment this replaced: "a real gamepad already has a fixed, working
// mapping"), on the theory that a device SDL already understands needs no
// help. Cyril's point: "already has a working default" isn't the same as
// "the player likes that default" -- a recognized pad's face buttons can
// still be labelled/laid out in a way that doesn't suit a given game, and
// there was no way to fix that without hand-editing controls.cfg's
// GAMEPAD_0..3 tables. So: DeviceInfo now carries a `kind`, and a
// recognized gamepad can be picked and walked through the exact same
// whole capture flow as an unrecognized one -- just captured/resolved through
// SDL's own named SDL_GamepadButton/SDL_GamepadAxis ids (handleGamepad*
// below) instead of raw indices (handleJoy* above), since that's the only
// id space SDL guarantees stays meaningful for a device it already has a
// mapping for. Both kinds save into the exact same JoystickBinding (see
// JoystickProfile.h) -- no format change, no migration needed for any
// profile file saved before this existed.
//
// This also added a second way into the wizard's capture flow, still from
// the PickDevice screen (opened with F9 as before -- Cyril: "en appuyant
// sur un bouton du joystick" was about SELECTING a device already listed
// on screen, not a whole new always-listening trigger): pressing a button
// on any device already shown in the list jumps straight to it (no more
// hunting for "my pad" with Up/Down in a long list) and asks "Remap
// this?" (the new ConfirmRemap step) rather than diving straight into the
// whole capture flow -- a stray/accidental button press this way costs one
// Escape, not a half-finished capture. IMPORTANT correctness note, found
// by writing a small standalone probe against a real SDL3 virtual
// gamepad rather than assumed: opening a device via SDL_OpenGamepad (see
// main.cpp's SDL_EVENT_GAMEPAD_ADDED handling) makes SDL deliver BOTH the
// semantic SDL_EVENT_GAMEPAD_BUTTON_DOWN/UP/AXIS_MOTION events AND the
// raw SDL_EVENT_JOYSTICK_BUTTON_DOWN/UP/AXIS_MOTION events for the exact
// same physical press (confirmed: both fire, same instance id, same
// press). Every handleJoy*/handleGamepad* method below is therefore kind-
// scoped -- it ignores any event for a device it doesn't recognize as its
// own kind -- so a single real button press can never double-capture
// (writing into binding_ from both a raw index and a semantic id at once)
// or double-advance a capture prompt.

#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "core/JoystickProfile.h"

struct lua_State;

namespace dirk {

class JoystickWizard {
public:
    struct DeviceInfo {
        SDL_JoystickID instanceId = 0;
        std::string name;
        std::string guid;

        // Raw: an SDL_IsGamepad()==false joystick, captured/resolved
        // through its numeric SDL button/axis/hat indices
        // (JOYSTICK_BUTTON_DOWN/AXIS_MOTION/HAT_MOTION events) -- the
        // original "SDL doesn't know this device's layout at all" case
        // this wizard was built for.
        // Gamepad: an SDL_IsGamepad()==true device SDL already has a
        // working default mapping for (GAMEPAD_0..3/controls.cfg),
        // captured/resolved through SDL's own named SDL_GamepadButton/
        // SDL_GamepadAxis ids instead (GAMEPAD_BUTTON_DOWN/AXIS_MOTION
        // events -- no hats, SDL3 exposes a gamepad's d-pad as four
        // buttons) so a *recognized* pad's default layout can also be
        // overridden when it doesn't suit a given game (Cyril,
        // 2026-09-23: "l'idee est de remapper ses touches si le mapping
        // par defaut ne convient pas") -- see the top-of-file comment.
        enum class Kind { Raw, Gamepad };
        Kind kind = Kind::Raw;
    };

    // sharedEngineRoot: where the wizard's own font is loaded from (same
    // dirk_loading_font.ttf the loading screen already uses) and where
    // JoystickProfileStore::defaultPath() points the general/shared saved
    // profile file. gameFolder: the CURRENT game's own folder (ctx.
    // scriptPath in main.cpp) -- where JoystickProfileStore::
    // defaultPathForGame() points its per-game profile file; pass an empty
    // string for the legacy bare-video launch mode that has no game folder
    // at all (see open()'s comment on what that skips). gameProfiles/
    // generalProfiles: the SAME two stores main.cpp uses to resolve live
    // raw-joystick input during actual gameplay (see main.cpp's
    // runtimeProfilesGame/runtimeProfiles) -- taken by reference, not
    // owned here, so that a save this wizard makes takes effect on the
    // very next button press in the same run, with no separate reload
    // step and no risk of the two ever drifting apart. Which one is
    // actually read/written during a given run is decided by the new
    // PickScope screen (see the top-of-file 2026-09-24 comment) --
    // activeProfiles_/activePath_ below.
    JoystickWizard(std::string sharedEngineRoot, std::string gameFolder,
                    JoystickProfileStore& gameProfiles, JoystickProfileStore& generalProfiles);
    ~JoystickWizard();

    // Closes the TTF font now instead of in the destructor. main.cpp must
    // call this before TTF_Quit(): the wizard is a local of main(), so its
    // destructor only runs AFTER main's own TTF_Quit() -- and
    // TTF_CloseFont() on an already-shut-down SDL_ttf segfaults
    // (FT_Done_Face). Found 2026-09-26 while testing the script-error
    // banner: every normal quit crashed on exit whenever
    // dirk_loading_font.ttf was actually present (reproduced with the
    // unmodified build too, exit code 139). Safe to call more than once.
    void releaseFont();

    JoystickWizard(const JoystickWizard&) = delete;
    JoystickWizard& operator=(const JoystickWizard&) = delete;

    bool active() const { return step_ != Step::Inactive; }

    // Read-only introspection -- mainly for automated testing (see
    // tools/joystick_wizard_test.cpp), which needs to observe progress
    // without depending on stderr log text. Returns
    // static_cast<size_t>(-1) whenever the wizard isn't currently sitting
    // on a capture prompt (inactive, picking a device, or at Confirm).
    size_t currentPromptIndex() const {
        return step_ == Step::Capture ? promptIndex_ : static_cast<size_t>(-1);
    }

    // True only while `id` is the device currently being captured --
    // main.cpp calls this from its SDL_EVENT_JOYSTICK_REMOVED handling so
    // it can cancel the wizard cleanly (rather than leaving it stuck
    // waiting on a device that no longer exists) if that exact device is
    // unplugged mid-capture.
    bool isUsingDevice(SDL_JoystickID id) const;

    // True only while sitting on the "configure the other input too?"
    // question (see Step::ConfirmSecondStick). Same testing-only purpose
    // as currentPromptIndex() above -- currentPromptIndex() alone can't
    // tell this step apart from Confirm (both return -1), and a test
    // needs to know which one it actually landed on before pressing
    // Enter, since Enter means something completely different at each.
    bool isOfferingSecondStick() const { return step_ == Step::ConfirmSecondStick; }

    // True only while sitting on the new "this game / general" question
    // (Step::PickScope, see the top-of-file 2026-09-24 comment) -- same
    // testing-only purpose as currentPromptIndex()/isOfferingSecondStick()
    // above.
    bool isPickingScope() const { return step_ == Step::PickScope; }

    // Same testing-only purpose as currentPromptIndex() above -- what the
    // on-screen "Step X / Y" label (and the Confirm screen's "N/Y
    // assigned") actually show. totalPromptCount()/currentPromptNumber()
    // are private (draw() calls them directly); these just expose them to
    // tools/joystick_wizard_test.cpp without duplicating the logic there.
    // See the 2026-09-25 field report at those two's own declaration.
    int currentPromptNumberForTesting() const { return currentPromptNumber(); }
    int totalPromptCountForTesting() const { return totalPromptCount(); }

    // Opens the wizard at the device-picker step. `devices` should be
    // every currently-connected joystick/gamepad, both kinds (main.cpp's
    // own rawJoysticks AND gamepads lists, each converted to DeviceInfo
    // with the matching Kind -- see main.cpp's F9 handling). An empty list
    // is valid and shown as-is ("no device connected"), not treated as an
    // error -- silently doing nothing on F9 would look like a dead key,
    // which is worse.
    void open(lua_State* L, const std::vector<DeviceInfo>& devices);

    // Cancels immediately from any step, discarding any in-progress
    // capture. Never touches the saved profile file.
    void cancel();
    // Driving the wizard without a keyboard (2026-10-03, Cyril: handhelds
    // and cabinets have no F9/Enter/Escape). takesGameControls() is true
    // on every screen that is a choice (scope, device list, the yes/no
    // questions, the final save) -- never while presses are being
    // recorded, where every button is a binding. On those screens main.cpp
    // resolves each input through the game's own controls (controls.cfg,
    // saved profiles) and hands the logical switch here:
    //   SWITCH_UP / SWITCH_DOWN            -> Up / Down
    //   action 1 (SWITCH_BUTTON1)           -> Enter (not Start: see .cpp)
    //   action 2 (SWITCH_BUTTON2), exit menu (SWITCH_CONSOLE) -> Escape
    // Presses only; releases and other switches are ignored. During the
    // recording itself nothing changes: an absent button is skipped by the
    // prompt's own timeout.
    bool takesGameControls() const {
        return step_ == Step::PickScope || step_ == Step::PickDevice || step_ == Step::ConfirmDelete ||
               step_ == Step::ConfirmRemap || step_ == Step::ConfirmSecondStick || step_ == Step::Confirm;
    }
    void handleGameSwitch(int switchId, bool pressed);

    // Single entry point for every SDL event while active() -- feed it
    // every polled event before your own normal dispatch. Returns true if
    // the wizard consumed the event (caller must skip normal gameplay
    // dispatch for it -- keyboard and every joystick/gamepad input is
    // fully claimed by the wizard while it's open, so nothing leaks into
    // the paused game underneath). Events the wizard has no interest in
    // (window/quit, joystick hot-plug, mouse, ...) return false untouched
    // and should be handled exactly as if the wizard didn't exist.
    bool handleEvent(const SDL_Event& ev);

    // Advances the current prompt's timeout, if any. Call once per frame
    // with now = SDL_GetTicks().
    void tick(Uint32 nowTicks);

    // Draws the wizard's overlay (a dimmed full-screen panel plus
    // centered text) on top of whatever is already composited into
    // `renderer` this frame. No-op if !active(). outW/outH: current
    // render output size -- main.cpp already queries this every frame
    // (SDL_GetCurrentRenderOutputSize), pass it through rather than
    // querying it again here.
    void render(SDL_Renderer* renderer, int outW, int outH);

private:
    // PickScope: the new first screen (see the top-of-file 2026-09-24
    // comment) -- "This game only" vs. "General (all games)". Reached
    // only from open() (never re-entered mid-run), resolved by
    // beginWithScope() once Enter confirms a choice, which is what
    // actually sets activeProfiles_/activePath_ and moves on to
    // PickDevice; Escape here cancels the whole wizard, same as anywhere
    // else. selectedScopeIndex_ (not a separate field for each option):
    // 0 = this game, 1 = general.
    //
    // ConfirmRemap: reached from PickDevice by pressing a button on a
    // device already shown in the list (see the top-of-file 2026-09-23
    // comment) -- asks "remap this one?" before committing to the full
    // whole capture flow, same spot in the flow ConfirmDelete already
    // occupies for Suppr. selectedDeviceIndex_ (not a separate field)
    // names which device: Enter here calls startCapture() on it, Escape
    // returns to PickDevice with the same device still highlighted.
    //
    // ConfirmSecondStick: reached from Capture, only for a Kind::Gamepad
    // device, right after the 4 directional prompts (Haut/Bas/Gauche/
    // Droite) finish -- 2026-09-24, Cyril: "pour les joystick de type
    // XBOX il serait bien de proposer de configurer un second stick apres
    // le premier (certains joueurs aiment jouer parfois sur le pad et
    // parfois sur le stick gauche)". Asks whether to ALSO bind the other
    // input (D-pad if the player just used the analog stick, or the
    // stick if they just used the D-pad) to the same 4 directions, on
    // top of (not instead of) what's already captured. Enter re-walks
    // promptIndex_ 0..3 a second time (secondStickActive_), restricted to
    // whichever input kind wasn't used the first time; Escape skips
    // straight to prompt 4 (Start), same as every other prompt's
    // "nothing to add" path. Either way this is only ever asked once per
    // run (secondStickOffered_), so finishing the second pass or
    // declining it both fall straight through to prompt 4 without asking
    // again.
    enum class Step { Inactive, PickScope, PickDevice, ConfirmDelete, ConfirmRemap, ConfirmSecondStick, Capture, Confirm };

    struct PromptDef {
        int switchId;
        const char* label; // display label shown on screen -- Cyril's own wording (see prompts()'s 2026-09-24 comment), not necessarily French
        // InputMap.cpp kBindings' own alias name for this exact switchId
        // (GO_UP, COIN_1P, EXIT_MENU, ...) -- added 2026-09-24 after a real
        // mislabeling was caught this way (Cyril: "l'action QUIT me semble
        // liee a Input_quit alors qu'en fait c'est INPUT_CONSOLE je crois").
        // Kept here as a source-level cross-check against InputMap.cpp's
        // own ground truth (each entry above was re-verified against it,
        // see prompts()'s comment) -- NOT shown on the Capture screen.
        // Cyril's follow-up the same day clarified his "pour la clarte"
        // request was about the SAVED PROFILE FILE, not this on-screen
        // text: JoystickProfile.cpp writes switch ids using
        // InputMap::switchIdToAliasName() (independent of this field) so
        // a profile he opens by hand reads in his own vocabulary; this UI
        // stays plain "Press: <label>", no alias appended.
        const char* alias;
    };
    // The curated subset of kBindings (InputMap.cpp) confirmed to have
    // real usage in the delivered games, AND that Cyril actually wants
    // offered here (17 of the 24 SWITCH_* ids as of 2026-09-24 -- see
    // prompts()'s own comment for the two, COIN2/CONSOLE, he dropped on
    // top of the five already-dead ones: RESET_CPU/SCREENSHOT/ACTION_4/
    // TILT/GRAB, excluded for the same reason the simplified shared
    // default controls.cfg already excludes them -- see InputMap.cpp's
    // own per-entry comments for why each one is dead).
    static const std::vector<PromptDef>& prompts();

    bool handleKeyDown(SDL_Scancode scancode);
    // Raw path -- kind-scoped to DeviceInfo::Kind::Raw devices only (see
    // the top-of-file 2026-09-23 comment on why: a Kind::Gamepad device's
    // raw events must never reach these, since SDL also fires them
    // alongside its semantic gamepad events for the very same press).
    bool handleJoyButton(SDL_JoystickID which, int button, bool down);
    bool handleJoyHat(SDL_JoystickID which, int hat, Uint8 value);
    bool handleJoyAxis(SDL_JoystickID which, int axis, Sint16 value);
    // Gamepad path -- kind-scoped to DeviceInfo::Kind::Gamepad devices
    // only, symmetric reasoning to the raw path above.
    bool handleGamepadButton(SDL_JoystickID which, int button, bool down);
    bool handleGamepadAxis(SDL_JoystickID which, int axis, Sint16 value);

    // Index into devices_ whose instanceId == id, or -1. Used by the
    // PickDevice "press a button on a listed device to jump to it"
    // shortcut (see the top-of-file 2026-09-23 comment) -- handleJoyButton
    // and handleGamepadButton each call this and additionally check the
    // matched device's kind against their own before acting on it.
    int findDeviceIndex(SDL_JoystickID id) const;

    // Resolves the PickScope choice: picks gameProfiles_/gameProfilePath_
    // or generalProfiles_/generalProfilePath_ as the ACTIVE store/path for
    // the rest of this run (activeProfiles_/activePath_), loads it fresh
    // from disk (same defensive re-read open() always did, just deferred
    // to here now), and moves on to PickDevice -- or, if devices_ is
    // empty, arms the same auto-close timeout PickDevice always used in
    // that case. Called either from handleKeyDown's PickScope case (a
    // real player choice) or directly from open() when there's no real
    // choice to offer at all (see open()'s comment).
    void beginWithScope(lua_State* L, bool perGame);

    void startCapture(const DeviceInfo& device);
    void advancePrompt();                 // records nothing -- just moves on (timeout or manual skip already recorded what there was to record)
    void goBackPrompt();                  // steps promptIndex_ back one and clears whatever it had recorded, so it captures cleanly again
    // Removes recorded bindings pointing at this switch id. `axesOnly`
    // restricts this to the axes/hats maps only, leaving any button
    // mapping (e.g. a D-pad button from an earlier pass) untouched --
    // used during a second-stick pass's Retour arriere so undoing the
    // stick capture can't also wipe out the D-pad capture from the first
    // pass for the same direction (or vice versa, via `buttonsOnly`).
    // false/false (the default, used everywhere outside the second-stick
    // pass) clears everything, the original behaviour.
    void clearPromptBinding(int switchId, bool axesOnly = false, bool buttonsOnly = false);
    void armDeadline();
    void finishSave();

    // 2026-09-25 field report (Cyril): "le compteur du joystick wizard...
    // il compte sur 17, mais si tu rajoutes le second pad c'est sur 21
    // qu'il faut compter (et uniquement dans ce cas)". Root cause: the
    // on-screen "Step X / Y" label and the Confirm screen's "N/Y assigned"
    // both hardcoded Y to prompts().size() (17), even once the player
    // accepts the second-stick pass (see Step::ConfirmSecondStick), which
    // walks 4 more prompts (Up/Down/Left/Right a second time) for a real
    // total of 21. These two helpers compute the true total/position;
    // prompts().size()/promptIndex_ alone are only correct when the
    // second-stick pass was never accepted.
    int totalPromptCount() const;
    int currentPromptNumber() const;
    // True if switchId currently has a button mapping (D-pad, for a
    // Kind::Gamepad device) but no axis/hat mapping, or vice versa --
    // used right after the 4 directional prompts to figure out which
    // input kind the player just used, so the second-stick offer (see
    // Step::ConfirmSecondStick above) knows which OTHER kind to ask for.
    bool hasButtonBinding(int switchId) const;
    bool hasAxisOrHatBinding(int switchId) const;

    std::string sharedEngineRoot_;
    std::string gameFolder_; // ctx.scriptPath -- empty in the legacy bare-video launch mode (no game at all)
    std::string gameProfilePath_;    // empty when gameFolder_ is empty -- never dereferenced in that case, see open()
    std::string generalProfilePath_;
    JoystickProfileStore& gameProfiles_;
    JoystickProfileStore& generalProfiles_;

    // Which of the two stores/paths above THIS wizard run is actually
    // reading/writing -- set once by beginWithScope(), which always runs
    // before PickDevice/ConfirmDelete/Capture/Confirm can be reached, so
    // activeProfiles_ is never null by the time anything else touches it.
    // See the top-of-file 2026-09-24 comment.
    JoystickProfileStore* activeProfiles_ = nullptr;
    std::string activePath_;
    bool scopeIsPerGame_ = false; // for display only (PickDevice/Confirm) -- the actual routing is activeProfiles_/activePath_
    int selectedScopeIndex_ = 0;  // PickScope only: 0 = this game, 1 = general
    lua_State* pendingLuaState_ = nullptr; // stashed by open(), consumed by beginWithScope() once PickScope resolves

    TTF_Font* font_ = nullptr;

    Step step_ = Step::Inactive;
    std::vector<DeviceInfo> devices_;
    int selectedDeviceIndex_ = 0;
    int deleteCandidateIndex_ = -1; // device index pending confirmation on ConfirmDelete

    SDL_JoystickID captureInstanceId_ = 0;
    DeviceInfo::Kind captureKind_ = DeviceInfo::Kind::Raw; // which of handleJoy*/handleGamepad* the current capture belongs to
    // Edge-triggered state for handleGamepadAxis, reset in startCapture()
    // and never touched anywhere else -- found necessary by testing
    // against a real SDL3 virtual gamepad, not assumed: unlike gamepad
    // BUTTON events, SDL's gamepad AXIS events are delivered in
    // duplicate for the exact same value (confirmed with a standalone
    // probe: one SDL_SetJoystickVirtualAxis call produced the matching
    // SDL_EVENT_GAMEPAD_AXIS_MOTION twice, back to back, same value).
    // Without this, handleGamepadAxis's threshold check would fire twice
    // per real stick push, silently binding the WRONG prompt (whichever
    // one advancePrompt() had just moved to) to that same still-held
    // direction. Mirrors main.cpp's own GamepadSlot::axisNegPressed/
    // axisPosPressed, which already avoids this exact issue for live
    // gameplay input -- only ever acts on a false->true transition, never
    // on a repeated already-true report. handleJoyAxis (raw path) does
    // NOT need this: tested against the same kind of duplicate-delivery
    // risk and found clean -- SDL's raw joystick axis events don't repeat
    // this way, only the ones that go through the gamepad mapping layer
    // do.
    bool captureAxisNegPressed_[6] = {};
    bool captureAxisPosPressed_[6] = {};
    JoystickBinding binding_;
    size_t promptIndex_ = 0;

    // Second-stick pass state (Step::ConfirmSecondStick, see its own
    // comment above). secondStickOffered_ latches true the moment the
    // question is asked (whichever answer), so it's never asked twice in
    // one run. secondStickActive_ is true only while promptIndex_ 0..3
    // is being walked a SECOND time. secondStickFilter_ says which input
    // kind that second walk accepts: AxisOnly/ButtonOnly when pass one
    // clearly used the other kind (handleGamepadButton/handleGamepadAxis
    // both check this and ignore the kind not being asked for, so a
    // stray press of the input already bound in pass one can't corrupt
    // pass two); None when pass one bound nothing at all for these 4
    // switches (every directional prompt timed out/was skipped) -- there
    // is then nothing to infer the "other" kind from, so pass two simply
    // accepts whichever kind shows up first, same as pass one did.
    enum class SecondStickFilter { None, AxisOnly, ButtonOnly };
    bool secondStickOffered_ = false;
    bool secondStickActive_ = false;
    // 2026-09-25: distinct from secondStickActive_ (true only DURING the
    // second walk of prompts 0..3) -- this one latches true the moment the
    // player accepts the offer and stays true for the rest of the run
    // (through the second pass AND the ordinary prompts afterwards), so
    // totalPromptCount()/currentPromptNumber() stay correct (21, not 17)
    // all the way to Confirm. secondStickOffered_ doesn't work for this:
    // it also latches true on a DECLINE.
    bool secondStickAccepted_ = false;
    SecondStickFilter secondStickFilter_ = SecondStickFilter::None;
    Uint32 deadlineTicks_ = 0;

    // How long a single binding prompt waits for a press before silently
    // skipping it (Cyril's explicit requirement -- not every cabinet has
    // all of these buttons). Long enough that a person still reading
    // the prompt has time to find and press the right physical button;
    // short enough that a walkthrough with several genuinely
    // absent buttons doesn't drag on for minutes. Entree skips instantly
    // for anyone who already knows a button isn't there and doesn't want
    // to wait this out.
    static constexpr Uint32 kPromptTimeoutMs = 8000;

    // How long the device-picker screen waits, with nothing to configure
    // (devices_.empty()), before closing itself. See the 2026-09-23 field
    // report above -- there is nothing to lose by auto-closing here (there
    // was nothing to configure), so this is purely a safety net against
    // ever trapping the player on a screen they can't read and don't know
    // how to dismiss. Not applied to any other step: those all have real,
    // in-progress work that must never be silently discarded.
    static constexpr Uint32 kEmptyListTimeoutMs = 6000;
};

} // namespace dirk
