// joystick_wizard_test: standalone, fully automated end-to-end test of the
// native joystick setup wizard (src/core/JoystickWizard.*), driven through
// SDL's real virtual-joystick API (SDL_AttachVirtualJoystick) under
// SDL_VIDEODRIVER=dummy -- real SDL joystick/keyboard events flow through
// the exact same handleEvent()/tick() path main.cpp itself calls, and the
// result is verified by reading back the actual saved profile FILE (not by
// inspecting the wizard's private state), the same artifact a real cabinet
// would load at startup. Consistent with this project's standing rule:
// verify fixes/features against real, measured behavior, never guess.
//
// Exercises, in order:
//   1. Device picker with zero devices connected (shows the empty-list
//      message, only Echap works).
//   2. A full capture run covering every mechanism Cyril asked for:
//      - a button press captured normally;
//      - a prompt left to time out for real (the actual kPromptTimeoutMs
//        delay is waited out, not faked) -- must end up unbound;
//      - an axis captured in both directions across two separate prompts
//        (proves JoystickBinding::AxisDir accumulates rather than
//        overwriting);
//      - a hat direction captured;
//      - a manual instant-skip (Entree) -- must end up unbound;
//      - the SAME physical button reused for two different prompts
//        (last-write-wins, matching hand-edited controls.cfg semantics);
//      - Retour arriere (Backspace) re-doing a prompt mid-capture;
//      - Echap cancelling the whole run -- nothing must be written.
//   3. A second, complete run (skipping every remaining prompt) reaching
//      Confirm, using Retour arriere FROM Confirm to redo the last prompt,
//      then actually saving -- verified by loading the profile file back
//      with a brand new JoystickProfileStore/lua_State (a true round trip,
//      not the in-memory object the wizard itself just wrote).
//   4. The device list showing "deja configure" for that device, erasing
//      its profile (Suppr + confirm), and confirming the file no longer
//      has an entry for it afterward.
//   5. isUsingDevice()/cancel() -- the mechanism main.cpp's
//      SDL_EVENT_JOYSTICK_REMOVED handler uses to abort a capture cleanly
//      if the device being configured is unplugged mid-run.
//   10. PickScope (2026-09-24, Cyril: "je veux que dans le premier menu tu
//      demandes si on fait la configuration pour CE jeu ... ou en
//      general") -- a SEPARATE wizard instance built with a real
//      gameFolder: the new first screen appears, "this game" saves into a
//      profile file next to that game's own Cfg/ (never touching the
//      general file), "general" saves into the same shared file sections
//      1-9 above use (never touching the per-game file), and Suppr on the
//      device list only erases from whichever store is currently active.
//      Sections 1-9 themselves use a wizard built with an EMPTY
//      gameFolder throughout (the legacy bare-video launch mode's own
//      case) specifically so PickScope never appears there and every one
//      of their existing open()/pressKey sequences keeps working
//      unmodified -- proving the new screen is fully opt-in, not a
//      behavior change for every existing flow.
//   11. F9-while-open (2026-09-24 field report): a second F9 press while
//      the wizard is already open now closes it (or steps back, wherever
//      Escape itself would only step back) instead of silently doing
//      nothing and leaving the disc looking permanently frozen.

#include "core/JoystickWizard.h"
#include "lua/InputMap.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// See seek_test.cpp's own comment on why this include is needed even
// though nothing here calls SDL_main directly (SDL2main/SDL3main's link
// requirement on Windows).
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

bool g_allPass = true;

void check(bool cond, const std::string& label) {
    if (cond) {
        std::fprintf(stderr, "PASS %s\n", label.c_str());
    } else {
        std::fprintf(stderr, "FAIL %s\n", label.c_str());
        g_allPass = false;
    }
}

// Pumps the real SDL event queue for up to `maxMs`, handing every event to
// the wizard exactly like main.cpp's own loop does, and calling tick()
// once per iteration -- stops as soon as `until` returns true (checked
// after each event and after each tick).
template <typename Pred>
void pump(dirk::JoystickWizard& wizard, Uint32 maxMs, Pred until) {
    Uint32 start = SDL_GetTicks();
    while (SDL_GetTicks() - start < maxMs) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (wizard.active()) wizard.handleEvent(ev);
        }
        wizard.tick(SDL_GetTicks());
        if (until()) return;
        SDL_Delay(5);
    }
}

void pressKey(SDL_Scancode sc) {
    SDL_Event down{};
    down.type = SDL_EVENT_KEY_DOWN;
    down.key.scancode = sc;
    down.key.repeat = false;
    SDL_PushEvent(&down);
}

// Waits until the wizard's current capture-prompt index differs from
// `before` -- i.e. the wizard actually moved on, whatever the reason
// (a real capture, a manual skip, or a timeout). Returns true if it
// advanced within maxMs. This both drives the test off real, observed
// wizard state (rather than a guessed fixed delay) and doubles as an
// assertion that something actually happened.
bool waitForPromptAdvance(dirk::JoystickWizard& wizard, size_t before, Uint32 maxMs) {
    bool advanced = false;
    pump(wizard, maxMs, [&] {
        advanced = wizard.currentPromptIndex() != before;
        return advanced;
    });
    return advanced;
}

// SDL auto-generates a gamepad mapping the first time it sees a virtual,
// gamepad-typed device in a process -- and, confirmed via a standalone
// probe (SDL_AttachVirtualJoystick, outside this test binary), can then
// WRONGLY reuse an earlier device's cached mapping (wrong name, wrong
// button/axis count) for a later-attached virtual gamepad-shaped device
// in that same process. Without this, a device attached second-or-later
// in this file can silently end up wired to the WRONG button/axis
// indices -- a real device's own D-pad, say, mapped onto whatever index
// an earlier, differently-shaped device happened to expose there. This
// registers an explicit, correct flat mapping for `id`'s own GUID, which
// always takes priority over SDL's cached guess, so every gamepad-shaped
// device this file attaches is verified against ITS real shape, not
// coincidentally against a previous one's.
void forceCorrectGamepadMapping(SDL_JoystickID id, const std::string& name, int nbuttons, int naxes) {
    static const char* kButtonNames[] = {
        "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
        "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"
    };
    static const char* kAxisNames[] = {
        "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"
    };
    char guidStr[64];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guidStr, sizeof(guidStr));

    std::string mapping = std::string(guidStr) + "," + name + ",";
    int nb = nbuttons < 15 ? nbuttons : 15;
    for (int i = 0; i < nb; ++i) {
        mapping += std::string(kButtonNames[i]) + ":b" + std::to_string(i) + ",";
    }
    int na = naxes < 6 ? naxes : 6;
    for (int i = 0; i < na; ++i) {
        mapping += std::string(kAxisNames[i]) + ":a" + std::to_string(i) + ",";
    }
    mapping += "platform:Linux,";

    int rc = SDL_AddGamepadMapping(mapping.c_str());
    check(rc >= 0, "SDL_AddGamepadMapping registered an explicit, correct mapping for \"" + name + "\"");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path to dirk_loading_font.ttf>\n", argv[0]);
        return 1;
    }
    const std::string fontSourcePath = argv[1];

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!TTF_Init()) {
        std::fprintf(stderr, "FAIL: TTF_Init: %s\n", SDL_GetError());
        return 1;
    }

    // A real (dummy-driver) window+renderer so render() can be exercised
    // for real, same as main.cpp's own -- not strictly needed for the
    // pass/fail checks below (those all come from the saved file), but a
    // crash inside render() while the wizard is on screen would be exactly
    // the kind of bug this test should catch.
    SDL_Window* window = SDL_CreateWindow("joystick_wizard_test", 640, 480, 0);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    check(window != nullptr && renderer != nullptr, "SDL_CreateWindow/CreateRenderer (dummy driver)");

    // sharedEngineRoot: a scratch directory with the real loading font
    // copied in (so render() actually draws text, not just a dimmed
    // rectangle) and the Cfg/ subfolder the profile file lives under.
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "dirk_joystick_wizard_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "KimmyScript" / "Cfg", ec);
    fs::copy_file(fontSourcePath, root / "dirk_loading_font.ttf", ec);
    check(!ec, "font asset staged into scratch sharedEngineRoot");

    const std::string sharedEngineRoot = root.string();
    const std::string profilePath = dirk::JoystickProfileStore::defaultPath(sharedEngineRoot);

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    dirk::JoystickProfileStore profiles;
    // Empty gameFolder -- sections 1-9 below (unchanged since before the
    // PickScope screen existed, 2026-09-24) test the capture flow itself,
    // not the new scope question, so this wizard is built the way the
    // legacy bare-video launch mode uses it (see JoystickWizard::open()'s
    // comment): PickScope is skipped and every open() below lands
    // straight on PickDevice, exactly as it always did. unusedGameProfiles
    // is genuinely never touched by this instance (perGame is never true
    // here) -- section 10 further down is the dedicated PickScope/
    // per-game coverage, with its own wizard instance that DOES get a
    // real gameFolder.
    dirk::JoystickProfileStore unusedGameProfiles;
    dirk::JoystickWizard wizard(sharedEngineRoot, /*gameFolder=*/"", unusedGameProfiles, profiles);

    // -----------------------------------------------------------------
    // 1) Empty device list.
    // -----------------------------------------------------------------
    wizard.open(L, {});
    check(wizard.active(), "open() with no devices still opens (shows the empty-list message)");
    check(!wizard.isPickingScope(),
          "wizard built with an empty gameFolder skips PickScope entirely, even for the empty-device-list case");
    pressKey(SDL_SCANCODE_ESCAPE);
    pump(wizard, 500, [&] { return !wizard.active(); });
    check(!wizard.active(), "Echap closes the empty-device-list screen");

    // -----------------------------------------------------------------
    // Attach one virtual joystick -- 8 buttons, 2 axes, 1 hat, comfortably
    // covering everything the wizard's prompts might ask for in this
    // test.
    // -----------------------------------------------------------------
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = 2;
    desc.nbuttons = 8;
    desc.nhats = 1;
    desc.name = "Dirk Test Cabinet Stick";
    SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    check(vid != 0, "SDL_AttachVirtualJoystick");

    SDL_JoystickID capturedId = 0;
    std::string capturedGuid;
    {
        Uint32 start = SDL_GetTicks();
        while (SDL_GetTicks() - start < 1000 && capturedId == 0) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_JOYSTICK_ADDED) {
                    capturedId = ev.jdevice.which;
                }
            }
        }
    }
    check(capturedId != 0, "SDL_EVENT_JOYSTICK_ADDED observed for the virtual device");
    SDL_Joystick* vjoy = SDL_OpenJoystick(capturedId);
    check(vjoy != nullptr, "SDL_OpenJoystick on the virtual device");
    {
        char guidStr[64];
        SDL_GUIDToString(SDL_GetJoystickGUID(vjoy), guidStr, sizeof(guidStr));
        capturedGuid = guidStr;
    }

    std::vector<dirk::JoystickWizard::DeviceInfo> devices = {
        {capturedId, "Dirk Test Cabinet Stick", capturedGuid},
    };

    // -----------------------------------------------------------------
    // 2) First run: exercise every capture mechanism, then cancel. All
    //    waits below are driven by currentPromptIndex() actually changing
    //    (see waitForPromptAdvance), not a guessed fixed delay -- each one
    //    doubles as an assertion that the step really happened.
    // -----------------------------------------------------------------
    wizard.open(L, devices);
    pressKey(SDL_SCANCODE_RETURN); // select the only device -> starts capture at prompt 0 ("Haut")
    check(waitForPromptAdvance(wizard, static_cast<size_t>(-1), 2000), "selecting the device starts capture at prompt 0");

    // Prompt 0 ("Haut", switch 0): real button press.
    size_t p = wizard.currentPromptIndex();
    SDL_SetJoystickVirtualButton(vjoy, 3, true);
    check(waitForPromptAdvance(wizard, p, 2000), "prompt 0 (Haut) advanced after a real button press");
    SDL_SetJoystickVirtualButton(vjoy, 3, false);

    // Prompt 1 ("Bas", switch 2): let it genuinely time out. Waits the
    // real kPromptTimeoutMs (8s) -- deliberately not faked, so this proves
    // Cyril's timeout requirement end-to-end, not just on paper.
    p = wizard.currentPromptIndex();
    std::fprintf(stderr, "(waiting out the real prompt timeout -- ~8s...)\n");
    Uint32 t0 = SDL_GetTicks();
    bool timedOut = waitForPromptAdvance(wizard, p, 11000);
    Uint32 elapsedMs = SDL_GetTicks() - t0;
    check(timedOut, "prompt 1 (Bas) eventually advanced with no input at all (timeout)");
    check(elapsedMs >= 7000 && elapsedMs < 10500,
          "prompt 1's timeout took roughly kPromptTimeoutMs (~8s), not instantly and not never");

    // Prompt 2 ("Gauche", switch 1): axis 0, negative direction.
    p = wizard.currentPromptIndex();
    SDL_SetJoystickVirtualAxis(vjoy, 0, -32000);
    check(waitForPromptAdvance(wizard, p, 2000), "prompt 2 (Gauche) advanced after axis 0 went negative");
    SDL_SetJoystickVirtualAxis(vjoy, 0, 0);

    // Prompt 3 ("Droite", switch 3): SAME axis 0, positive direction --
    // must accumulate into the same AxisDir entry rather than replacing it.
    p = wizard.currentPromptIndex();
    SDL_SetJoystickVirtualAxis(vjoy, 0, 32000);
    check(waitForPromptAdvance(wizard, p, 2000), "prompt 3 (Droite) advanced after axis 0 went positive");
    SDL_SetJoystickVirtualAxis(vjoy, 0, 0);

    // Prompt 4 ("Start", switch 4): hat up.
    p = wizard.currentPromptIndex();
    SDL_SetJoystickVirtualHat(vjoy, 0, SDL_HAT_UP);
    check(waitForPromptAdvance(wizard, p, 2000), "prompt 4 (Start) advanced after hat 0 up");
    SDL_SetJoystickVirtualHat(vjoy, 0, SDL_HAT_CENTERED);

    // Prompt 5 ("Coin", switch 9): manual instant skip.
    p = wizard.currentPromptIndex();
    pressKey(SDL_SCANCODE_RETURN);
    check(waitForPromptAdvance(wizard, p, 2000), "prompt 5 (Coin) advanced after Entree (manual skip)");

    // Prompt 6 ("Bouton 1", switch 6): reuse button 3 -- must
    // overwrite prompt 0's earlier mapping (buttons[3] goes from switch 0
    // to switch 6).
    p = wizard.currentPromptIndex();
    SDL_SetJoystickVirtualButton(vjoy, 3, true);
    check(waitForPromptAdvance(wizard, p, 2000), "prompt 6 (Bouton 1) advanced, reusing button 3");
    SDL_SetJoystickVirtualButton(vjoy, 3, false);

    // Now at prompt 7 ("Bouton 2"). Test Retour arriere: go back
    // to prompt 6, which must clear its button-3 mapping again, then
    // rebind it to button 5 instead.
    p = wizard.currentPromptIndex();
    pressKey(SDL_SCANCODE_BACKSPACE);
    check(waitForPromptAdvance(wizard, p, 2000), "Retour arriere steps back to prompt 6");
    check(wizard.currentPromptIndex() == p - 1, "Retour arriere lands exactly one prompt earlier, not further");
    p = wizard.currentPromptIndex();
    SDL_SetJoystickVirtualButton(vjoy, 5, true);
    check(waitForPromptAdvance(wizard, p, 2000), "prompt 6 re-captured with button 5 after going back");
    SDL_SetJoystickVirtualButton(vjoy, 5, false);

    // Now at prompt 7 again. Cancel the whole run -- nothing must be saved.
    pressKey(SDL_SCANCODE_ESCAPE);
    pump(wizard, 1000, [&] { return !wizard.active(); });
    check(!wizard.active(), "Echap during capture cancels the wizard");

    {
        dirk::JoystickProfileStore reload;
        reload.load(L, profilePath);
        check(reload.find(capturedGuid) == nullptr,
              "cancelling mid-capture wrote nothing to disk (no profile file / no entry for this GUID)");
    }

    // -----------------------------------------------------------------
    // 3) Second run: capture the axis/hat/button-reuse cases for real
    //    (this is the run that actually gets saved), skip everything
    //    else, then use Retour arriere from Confirm to redo the very
    //    last prompt before saving.
    // -----------------------------------------------------------------
    wizard.open(L, devices);
    pressKey(SDL_SCANCODE_RETURN); // select device, starts fresh (binding_ reset -- run 1's captures must not leak in)
    check(waitForPromptAdvance(wizard, static_cast<size_t>(-1), 2000), "second run starts fresh at prompt 0");

    // Prompt 0 ("Haut"): skip.
    p = wizard.currentPromptIndex();
    pressKey(SDL_SCANCODE_RETURN);
    check(waitForPromptAdvance(wizard, p, 2000), "run 2 prompt 0 skipped");
    // Prompt 1 ("Bas"): skip (manual, not the timeout this time -- that
    // mechanism was already proven above; no need to pay the 8s twice).
    p = wizard.currentPromptIndex();
    pressKey(SDL_SCANCODE_RETURN);
    check(waitForPromptAdvance(wizard, p, 2000), "run 2 prompt 1 skipped");
    // Prompt 2 ("Gauche"): axis 0 negative, for real this time.
    p = wizard.currentPromptIndex();
    SDL_SetJoystickVirtualAxis(vjoy, 0, -32000);
    check(waitForPromptAdvance(wizard, p, 2000), "run 2 prompt 2 (Gauche) captured via axis 0 negative");
    SDL_SetJoystickVirtualAxis(vjoy, 0, 0);
    // Prompt 3 ("Droite"): axis 0 positive, same axis.
    p = wizard.currentPromptIndex();
    SDL_SetJoystickVirtualAxis(vjoy, 0, 32000);
    check(waitForPromptAdvance(wizard, p, 2000), "run 2 prompt 3 (Droite) captured via axis 0 positive");
    SDL_SetJoystickVirtualAxis(vjoy, 0, 0);
    // Prompt 4 ("Start"): hat 0 up.
    p = wizard.currentPromptIndex();
    SDL_SetJoystickVirtualHat(vjoy, 0, SDL_HAT_UP);
    check(waitForPromptAdvance(wizard, p, 2000), "run 2 prompt 4 (Start) captured via hat 0 up");
    SDL_SetJoystickVirtualHat(vjoy, 0, SDL_HAT_CENTERED);
    // Prompts 5..15 ("Coin" through "Save slot 2"): skip the rest with
    // Entree. (List trimmed to 17 entries 2026-09-24, Cyril: "on propose
    // trop de trucs", then reordered 2026-09-24 to Cyril's own literal
    // order -- entry count and this sweep's bounds are unaffected either
    // time, only which label sits at which index changed.)
    for (int i = 5; i <= 15; ++i) {
        p = wizard.currentPromptIndex();
        pressKey(SDL_SCANCODE_RETURN);
        check(waitForPromptAdvance(wizard, p, 2000), "run 2 prompt skipped in the 5..15 sweep");
    }

    // Should now be at prompt 16 ("Save slot 3", switch 13), the last one
    // in Cyril's 17-entry list. Skip it too, reaching Confirm...
    p = wizard.currentPromptIndex();
    check(p == 16, "run 2 reached the final prompt (index 16) after skipping 0-4 and 5-15");
    pressKey(SDL_SCANCODE_RETURN);
    pump(wizard, 2000, [&] { return wizard.currentPromptIndex() == static_cast<size_t>(-1); });

    // ...then use Retour arriere from the Confirm screen to redo that same
    // last prompt with a real button, proving Retour arriere also works
    // from Confirm, not just mid-capture.
    pressKey(SDL_SCANCODE_BACKSPACE);
    check(waitForPromptAdvance(wizard, static_cast<size_t>(-1), 2000), "Retour arriere from Confirm re-opens the last prompt");
    check(wizard.currentPromptIndex() == 16, "Retour arriere from Confirm lands back on prompt 16 specifically");
    p = wizard.currentPromptIndex();
    SDL_SetJoystickVirtualButton(vjoy, 7, true);
    check(waitForPromptAdvance(wizard, p, 2000), "prompt 16 re-captured with button 7 after Confirm-screen redo");
    SDL_SetJoystickVirtualButton(vjoy, 7, false);

    // Back at Confirm -- save for real.
    pressKey(SDL_SCANCODE_RETURN);
    pump(wizard, 1000, [&] { return !wizard.active(); });
    check(!wizard.active(), "Entree at Confirm closes the wizard (saved)");

    {
        dirk::JoystickProfileStore reload;
        reload.load(L, profilePath);
        const dirk::JoystickBinding* b = reload.find(capturedGuid);
        check(b != nullptr, "profile file round-trips: entry for this GUID exists after reload");
        if (b != nullptr) {
            check(b->name == "Dirk Test Cabinet Stick", "round-tripped name matches");
            // switch 1 ("Gauche") -> axis 0 negative.
            auto axIt = b->axes.find(0);
            check(axIt != b->axes.end() && axIt->second.neg == 1,
                  "axis 0 negative direction round-trips to switch 1 (Gauche)");
            // switch 3 ("Droite") -> axis 0 positive -- same entry as above,
            // proving both directions of one axis survive the save/load
            // round trip together.
            check(axIt != b->axes.end() && axIt->second.pos == 3,
                  "axis 0 positive direction round-trips to switch 3 (Droite), same AxisDir entry as neg");
            // switch 4 ("Start") -> hat 0 up.
            auto hatIt = b->hats.find(0);
            check(hatIt != b->hats.end() && hatIt->second.up == 4,
                  "hat 0 up direction round-trips to switch 4 (Start)");
            // switch 13 ("Save slot 3") -> button 7, captured via the
            // Confirm-screen Retour arriere redo.
            auto btnIt = b->buttons.find(7);
            check(btnIt != b->buttons.end() && btnIt->second == 13,
                  "button 7 round-trips to switch 13 (Save slot 3), captured via Confirm-screen redo");
            // Nothing from run 1 (button 3/5 on "Bouton 1", which
            // was cancelled) should have leaked in -- button 7 must be the
            // ONLY button bound.
            check(b->buttons.size() == 1,
                  "no leftover bindings from the earlier cancelled run (exactly 1 button bound, not more)");
        }
    }

    // -----------------------------------------------------------------
    // 4) Device list shows "deja configure", erase the profile via Suppr.
    // -----------------------------------------------------------------
    wizard.open(L, devices); // reload from disk happens inside open()
    check(profiles.find(capturedGuid) != nullptr,
          "runtime store (the SAME one main.cpp would use for live gameplay) sees the just-saved profile");
    pressKey(SDL_SCANCODE_DELETE);
    pump(wizard, 500, [&] { return false; });
    pressKey(SDL_SCANCODE_RETURN); // confirm delete
    pump(wizard, 500, [&] { return false; });
    check(profiles.find(capturedGuid) == nullptr, "profile removed from the runtime store immediately after confirming delete");
    pressKey(SDL_SCANCODE_ESCAPE);
    pump(wizard, 500, [&] { return !wizard.active(); });

    {
        dirk::JoystickProfileStore reload;
        reload.load(L, profilePath);
        check(reload.find(capturedGuid) == nullptr,
              "deleted profile is also gone from disk after reload (device reverted to plain RAWJOY_N/controls.cfg)");
    }

    // -----------------------------------------------------------------
    // 5) isUsingDevice()/cancel() -- what main.cpp's own
    //    SDL_EVENT_JOYSTICK_REMOVED handler calls when the device being
    //    configured is unplugged mid-capture.
    // -----------------------------------------------------------------
    wizard.open(L, devices);
    pressKey(SDL_SCANCODE_RETURN);
    pump(wizard, 500, [&] { return false; });
    check(wizard.isUsingDevice(capturedId), "isUsingDevice() is true for the device currently being captured");
    check(!wizard.isUsingDevice(capturedId + 999), "isUsingDevice() is false for an unrelated instance id");
    wizard.cancel(); // exactly what main.cpp's JOYSTICK_REMOVED case does on a match
    check(!wizard.active(), "cancel() (as called from a simulated device-removal) closes the wizard cleanly");

    // -----------------------------------------------------------------
    // 6) Field-report regression (Cyril, 2026-09-23, real cabinet): F9
    //    with no unrecognized joystick connected AND the TTF font missing
    //    from sharedEngineRoot appeared to hang the game -- a dark screen
    //    with literally nothing on it, no way to know Echap would close
    //    it. Reproduced here for real: a SEPARATE wizard instance pointed
    //    at a scratch sharedEngineRoot that has no font file at all (the
    //    exact condition from his log), opened with zero devices (also
    //    exactly his log). Must now (a) still render something via the
    //    SDL_RenderDebugText fallback without crashing, and (b) close
    //    itself on its own well within a few seconds, with no input at
    //    all -- so this exact situation can never look like a hang again.
    // -----------------------------------------------------------------
    {
        fs::path noFontRoot = fs::temp_directory_path() / "dirk_joystick_wizard_test_nofont";
        fs::remove_all(noFontRoot, ec);
        fs::create_directories(noFontRoot / "KimmyScript" / "Cfg", ec);
        // Deliberately NOT copying dirk_loading_font.ttf here.

        dirk::JoystickProfileStore noFontProfiles;
        dirk::JoystickProfileStore noFontUnusedGameProfiles; // same reasoning as unusedGameProfiles above -- empty gameFolder below
        dirk::JoystickWizard noFontWizard(noFontRoot.string(), /*gameFolder=*/"", noFontUnusedGameProfiles, noFontProfiles);

        noFontWizard.open(L, {}); // exactly Cyril's log: 0 devices detected
        check(noFontWizard.active(), "field-repro: opens even with a missing font and zero devices");

        // Exercise the no-font render path for real, on the real dummy
        // renderer -- must not crash.
        noFontWizard.render(renderer, 640, 480);
        check(true, "field-repro: render() with no TTF font does not crash (SDL_RenderDebugText fallback)");

        Uint32 t0 = SDL_GetTicks();
        bool closedOnItsOwn = false;
        Uint32 start = SDL_GetTicks();
        while (SDL_GetTicks() - start < 9000) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (noFontWizard.active()) noFontWizard.handleEvent(ev);
            }
            noFontWizard.tick(SDL_GetTicks());
            noFontWizard.render(renderer, 640, 480); // keep exercising the fallback path throughout the wait
            if (!noFontWizard.active()) { closedOnItsOwn = true; break; }
            SDL_Delay(5);
        }
        Uint32 elapsedMs = SDL_GetTicks() - t0;
        check(closedOnItsOwn, "field-repro: the empty-list screen closes itself with NO input at all (no more apparent hang)");
        check(elapsedMs >= 5000 && elapsedMs < 8500,
              "field-repro: auto-close took roughly kEmptyListTimeoutMs (~6s), not instantly and not never");
    }

    // -----------------------------------------------------------------
    // 7) Recognized-gamepad remap support (Cyril, 2026-09-23: "l'idee est
    //    de remapper ses touches si le mapping par defaut ne convient
    //    pas") -- a SDL_IsGamepad()==true device can now also be picked
    //    and walked through the wizard, captured/resolved through SDL's
    //    named SDL_GamepadButton/SDL_GamepadAxis ids rather than raw
    //    indices. Attaches a SECOND virtual device (naxes=6, nbuttons=15,
    //    no hats -- the exact shape a real probe against this SDL build
    //    confirmed SDL auto-recognizes as a standard gamepad, unlike the
    //    section-1 device above, which has a hat and is deliberately kept
    //    as the Raw-kind case throughout this file) and lists it
    //    ALONGSIDE the still-attached Raw device, specifically to prove
    //    the two kinds don't cross-contaminate each other -- see the
    //    IMPORTANT correctness note in JoystickWizard.h's 2026-09-23
    //    comment: opening a device via SDL_OpenGamepad makes SDL deliver
    //    BOTH the semantic GAMEPAD_* events AND the raw JOYSTICK_* events
    //    for the very same physical press (confirmed against a real SDL3
    //    virtual gamepad, not assumed), so a naive implementation could
    //    silently double-capture or double-advance a prompt.
    // -----------------------------------------------------------------
    {
        SDL_VirtualJoystickDesc gpDesc;
        SDL_INIT_INTERFACE(&gpDesc);
        gpDesc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        gpDesc.naxes = 6;
        gpDesc.nbuttons = 15;
        gpDesc.nhats = 0;
        gpDesc.name = "Dirk Test Recognized Pad";
        SDL_JoystickID gpJoyId = SDL_AttachVirtualJoystick(&gpDesc);
        check(gpJoyId != 0, "SDL_AttachVirtualJoystick (recognized-gamepad-shaped device)");

        SDL_JoystickID capturedGpId = 0;
        {
            Uint32 start = SDL_GetTicks();
            while (SDL_GetTicks() - start < 1000 && capturedGpId == 0) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_EVENT_JOYSTICK_ADDED && ev.jdevice.which == gpJoyId) {
                        capturedGpId = ev.jdevice.which;
                    }
                }
            }
        }
        check(capturedGpId != 0, "SDL_EVENT_JOYSTICK_ADDED observed for the gamepad-shaped virtual device");
        forceCorrectGamepadMapping(capturedGpId, "Dirk Test Recognized Pad", 15, 6);
        check(SDL_IsGamepad(capturedGpId), "SDL recognizes the gamepad-shaped virtual device as a standard gamepad");

        SDL_Gamepad* gp = SDL_OpenGamepad(capturedGpId);
        check(gp != nullptr, "SDL_OpenGamepad on the virtual gamepad");
        SDL_Joystick* gpJoy = SDL_GetGamepadJoystick(gp);
        std::string gpGuid;
        {
            char guidStr[64];
            SDL_GUIDToString(SDL_GetJoystickGUID(gpJoy), guidStr, sizeof(guidStr));
            gpGuid = guidStr;
        }

        std::vector<dirk::JoystickWizard::DeviceInfo> mixedDevices = {
            {capturedId, "Dirk Test Cabinet Stick", capturedGuid, dirk::JoystickWizard::DeviceInfo::Kind::Raw},
            {capturedGpId, "Dirk Test Recognized Pad", gpGuid, dirk::JoystickWizard::DeviceInfo::Kind::Gamepad},
        };

        // 7a) Pressing a button on the RAW device, with a gamepad ALSO in
        // the list, must select the RAW one -- proves handleJoyButton's
        // PickDevice scan doesn't get confused by the gamepad entry.
        wizard.open(L, mixedDevices);
        SDL_SetJoystickVirtualButton(vjoy, 0, true);
        pump(wizard, 1000, [&] { return false; }); // let the PickDevice-select -> ConfirmRemap transition settle
        SDL_SetJoystickVirtualButton(vjoy, 0, false);
        pressKey(SDL_SCANCODE_RETURN); // ConfirmRemap -> startCapture on whichever device got selected
        check(waitForPromptAdvance(wizard, static_cast<size_t>(-1), 2000),
              "pressing a button on the RAW device (gamepad also listed) starts a capture");
        {
            size_t pp = wizard.currentPromptIndex();
            SDL_SetJoystickVirtualButton(vjoy, 1, true);
            check(waitForPromptAdvance(wizard, pp, 2000),
                  "...and that capture responds to RAW button presses (confirms the RAW device, not the gamepad, was selected)");
            SDL_SetJoystickVirtualButton(vjoy, 1, false);
        }
        pressKey(SDL_SCANCODE_ESCAPE); // cancel -- this sub-test only cares which device got selected, nothing here should be saved
        pump(wizard, 1000, [&] { return !wizard.active(); });
        check(!wizard.active(), "7a run cancelled cleanly");

        // 7b) Pressing a button on the GAMEPAD device, RAW device also
        // listed, must select the GAMEPAD one and capture through the
        // semantic GAMEPAD_* path -- and, critically, must advance
        // EXACTLY one prompt per real press despite SDL firing both
        // GAMEPAD_BUTTON_DOWN and JOYSTICK_BUTTON_DOWN for it (see the
        // section comment above).
        wizard.open(L, mixedDevices);
        SDL_SetJoystickVirtualButton(gpJoy, SDL_GAMEPAD_BUTTON_SOUTH, true);
        pump(wizard, 1000, [&] { return false; });
        SDL_SetJoystickVirtualButton(gpJoy, SDL_GAMEPAD_BUTTON_SOUTH, false);
        pressKey(SDL_SCANCODE_RETURN); // ConfirmRemap -> startCapture(gamepad)
        check(waitForPromptAdvance(wizard, static_cast<size_t>(-1), 2000),
              "pressing a button on the GAMEPAD device (raw also listed) starts a capture");
        check(wizard.currentPromptIndex() == 0, "capture restarted fresh at prompt 0 on the gamepad device");

        // Prompt 0 ("Haut", switch 0): one real gamepad button press.
        size_t pp = wizard.currentPromptIndex();
        SDL_SetJoystickVirtualButton(gpJoy, SDL_GAMEPAD_BUTTON_SOUTH, true);
        check(waitForPromptAdvance(wizard, pp, 2000), "prompt 0 (Haut) advanced after one real gamepad button press");
        check(wizard.currentPromptIndex() == pp + 1,
              "...and advanced by EXACTLY one prompt, not two -- no double-capture from the dual GAMEPAD_*/JOYSTICK_* event delivery");
        SDL_SetJoystickVirtualButton(gpJoy, SDL_GAMEPAD_BUTTON_SOUTH, false);

        // Prompt 1 ("Bas", switch 2): gamepad axis, negative direction.
        pp = wizard.currentPromptIndex();
        SDL_SetJoystickVirtualAxis(gpJoy, SDL_GAMEPAD_AXIS_LEFTX, -32000);
        check(waitForPromptAdvance(wizard, pp, 2000), "prompt 1 (Bas) advanced after gamepad axis LEFTX went negative");
        SDL_SetJoystickVirtualAxis(gpJoy, SDL_GAMEPAD_AXIS_LEFTX, 0);

        // Prompt 2 ("Gauche", switch 1): SAME gamepad axis, positive
        // direction -- must accumulate into the same AxisDir entry, same
        // check as the raw-axis case in section 2 above.
        pp = wizard.currentPromptIndex();
        SDL_SetJoystickVirtualAxis(gpJoy, SDL_GAMEPAD_AXIS_LEFTX, 32000);
        check(waitForPromptAdvance(wizard, pp, 2000), "prompt 2 (Gauche) advanced after gamepad axis LEFTX went positive");
        SDL_SetJoystickVirtualAxis(gpJoy, SDL_GAMEPAD_AXIS_LEFTX, 0);

        // Prompt 3 ("Droite", switch 3): manual skip. Reaching prompt 4
        // right after this, on a gamepad, is also exactly what triggers
        // the second-stick offer (2026-09-24, Cyril: "il serait bien de
        // proposer de configurer un second stick apres le premier" --
        // see Step::ConfirmSecondStick and section 8 below for that
        // feature's own dedicated coverage). This sub-test isn't about
        // that -- it declines it (Escape), same net effect as before that
        // feature existed -- and moves on.
        pp = wizard.currentPromptIndex();
        pressKey(SDL_SCANCODE_RETURN);
        check(waitForPromptAdvance(wizard, pp, 2000), "prompt 3 (Droite) skipped manually");
        check(wizard.isOfferingSecondStick(),
              "...and reaching prompt 4 on a gamepad lands on the second-stick offer, not straight on prompt 4");
        pressKey(SDL_SCANCODE_ESCAPE); // decline -- not this sub-test's concern
        check(waitForPromptAdvance(wizard, static_cast<size_t>(-1), 2000), "declining the offer moves on");
        check(wizard.currentPromptIndex() == 4, "...landing specifically on prompt 4 (Start), not restarted from 0");

        // Skip every remaining prompt (4..16, against Cyril's 17-entry
        // list) with Entree, reach Confirm, save.
        while (wizard.currentPromptIndex() != static_cast<size_t>(-1)) {
            pp = wizard.currentPromptIndex();
            pressKey(SDL_SCANCODE_RETURN);
            if (!waitForPromptAdvance(wizard, pp, 2000)) break; // reached Confirm (currentPromptIndex() -> -1)
        }
        pressKey(SDL_SCANCODE_RETURN); // Confirm -> save
        pump(wizard, 1000, [&] { return !wizard.active(); });
        check(!wizard.active(), "gamepad run saved and closed");

        {
            dirk::JoystickProfileStore reload;
            reload.load(L, profilePath);
            const dirk::JoystickBinding* prof = reload.find(gpGuid);
            check(prof != nullptr, "gamepad profile round-trips: entry for this GUID exists after reload");
            if (prof) {
                check(prof->name == "Dirk Test Recognized Pad", "round-tripped gamepad name matches");
                auto bit = prof->buttons.find(static_cast<int>(SDL_GAMEPAD_BUTTON_SOUTH));
                check(bit != prof->buttons.end() && bit->second == 0,
                      "gamepad button SOUTH round-trips to switch 0 (Haut), keyed by SDL_GamepadButton id (not a raw index)");
                auto ait = prof->axes.find(static_cast<int>(SDL_GAMEPAD_AXIS_LEFTX));
                check(ait != prof->axes.end() && ait->second.neg == 2 && ait->second.pos == 1,
                      "gamepad axis LEFTX round-trips both directions (neg->switch 2 Bas, pos->switch 1 Gauche) in the same AxisDir entry");
            }
            check(reload.find(capturedGuid) == nullptr,
                  "the RAW device's profile is still untouched by the gamepad run (no cross-kind contamination)");
        }

        SDL_CloseGamepad(gp);
        SDL_DetachVirtualJoystick(gpJoyId);
    }

    // -----------------------------------------------------------------
    // 8) Second-stick pass (2026-09-24, Cyril: "pour les joystick de type
    //    XBOX il serait bien de proposer de configurer un second stick
    //    apres le premier (certains joueurs aiment jouer parfois sur le
    //    pad et parfois sur le stick gauche)"). Captures Haut/Bas/Gauche/
    //    Droite via the D-pad BUTTONS first, accepts the offer, proves
    //    the wrong input kind (a button) is ignored during the second
    //    pass, then captures the same 4 directions again via the LEFT
    //    STICK axes -- and verifies BOTH end up saved for every one of
    //    the 4 switches (merged, not one replacing the other).
    // -----------------------------------------------------------------
    {
        SDL_VirtualJoystickDesc gpDesc;
        SDL_INIT_INTERFACE(&gpDesc);
        gpDesc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        gpDesc.naxes = 6;
        gpDesc.nbuttons = 15;
        gpDesc.nhats = 0;
        gpDesc.name = "Dirk Test Second Stick Pad";
        SDL_JoystickID gpJoyId = SDL_AttachVirtualJoystick(&gpDesc);
        check(gpJoyId != 0, "SDL_AttachVirtualJoystick (second-stick test device)");

        SDL_JoystickID capturedGpId = 0;
        {
            Uint32 start = SDL_GetTicks();
            while (SDL_GetTicks() - start < 1000 && capturedGpId == 0) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_EVENT_JOYSTICK_ADDED && ev.jdevice.which == gpJoyId) {
                        capturedGpId = ev.jdevice.which;
                    }
                }
            }
        }
        check(capturedGpId != 0, "SDL_EVENT_JOYSTICK_ADDED observed for the second-stick test device");
        forceCorrectGamepadMapping(capturedGpId, "Dirk Test Second Stick Pad", 15, 6);

        SDL_Gamepad* gp = SDL_OpenGamepad(capturedGpId);
        check(gp != nullptr, "SDL_OpenGamepad on the second-stick test device");
        SDL_Joystick* gpJoy = SDL_GetGamepadJoystick(gp);
        std::string gpGuid;
        {
            char guidStr[64];
            SDL_GUIDToString(SDL_GetJoystickGUID(gpJoy), guidStr, sizeof(guidStr));
            gpGuid = guidStr;
        }

        std::vector<dirk::JoystickWizard::DeviceInfo> devices = {
            {capturedGpId, "Dirk Test Second Stick Pad", gpGuid, dirk::JoystickWizard::DeviceInfo::Kind::Gamepad},
        };
        wizard.open(L, devices);
        pressKey(SDL_SCANCODE_RETURN); // select the only device -> starts capture at prompt 0
        check(waitForPromptAdvance(wizard, static_cast<size_t>(-1), 2000), "second-stick test: capture started");

        // Pass one: D-pad BUTTONS for all 4 directions (Haut/Bas/Gauche/
        // Droite -> switch 0/2/1/3, same order as every other section).
        auto pressButton = [&](Uint8 button) {
            size_t pp = wizard.currentPromptIndex();
            SDL_SetJoystickVirtualButton(gpJoy, button, true);
            check(waitForPromptAdvance(wizard, pp, 2000), "pass one: D-pad button advanced the prompt");
            SDL_SetJoystickVirtualButton(gpJoy, button, false);
        };
        pressButton(SDL_GAMEPAD_BUTTON_DPAD_UP);    // Haut (switch 0)
        pressButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN);  // Bas (switch 2)
        pressButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT);  // Gauche (switch 1)
        pressButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT); // Droite (switch 3)

        check(wizard.isOfferingSecondStick(),
              "pass one used only buttons -> reaching prompt 4 offers the second-stick question");

        // 2026-09-25 field report (Cyril): "le compteur... il compte sur
        // 17, mais si tu rajoutes le second pad c'est sur 21 qu'il faut
        // compter". Right here, at prompt index 4, the second-stick
        // question has been ASKED but not yet ANSWERED -- nothing decided
        // yet, so the total is still the ordinary 17 (identical to any
        // single-pass run reaching this same point).
        check(wizard.totalPromptCountForTesting() == 17,
              "before answering the second-stick offer: total is still the ordinary 17");
        check(wizard.currentPromptNumberForTesting() == 5,
              "...and current position is a plain 5 (prompt index 4 + 1), no +4 yet");

        // Accept it.
        pressKey(SDL_SCANCODE_RETURN);
        check(waitForPromptAdvance(wizard, static_cast<size_t>(-1), 2000), "accepting restarts capture at prompt 0");
        check(wizard.currentPromptIndex() == 0, "...specifically back at prompt 0, for the second pass");

        // The bug itself: promptIndex_ is back at 0 (second pass, same
        // internal index space as pass one), but this is really step 5 of
        // a now-21-step run, not step 1 of 17 again.
        check(wizard.totalPromptCountForTesting() == 21,
              "accepting the second-stick offer bumps the total from 17 to 21");
        check(wizard.currentPromptNumberForTesting() == 5,
              "...and the FIRST prompt of the second pass reads as step 5 (not step 1 again)");

        // Wrong-kind input during the second pass (a button -- pass one
        // already used buttons, so this pass wants the stick) must be
        // ignored entirely: the prompt must NOT advance.
        {
            size_t pp = wizard.currentPromptIndex();
            SDL_SetJoystickVirtualButton(gpJoy, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
            bool advanced = waitForPromptAdvance(wizard, pp, 500); // short wait -- expecting NO advance
            SDL_SetJoystickVirtualButton(gpJoy, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
            check(!advanced, "second pass ignores a D-pad button press (pass one's kind), doesn't advance");
        }

        // Pass two: LEFT STICK axes for the same 4 directions. LEFTY
        // negative/positive for Haut/Bas, LEFTX negative/positive for
        // Gauche/Droite -- natural stick-to-direction mapping.
        auto pressAxis = [&](SDL_GamepadAxis axis, Sint16 value) {
            size_t pp = wizard.currentPromptIndex();
            SDL_SetJoystickVirtualAxis(gpJoy, axis, value);
            check(waitForPromptAdvance(wizard, pp, 2000), "pass two: stick axis advanced the prompt");
            SDL_SetJoystickVirtualAxis(gpJoy, axis, 0);
        };
        pressAxis(SDL_GAMEPAD_AXIS_LEFTY, -32000); // Haut (switch 0)
        pressAxis(SDL_GAMEPAD_AXIS_LEFTY, 32000);  // Bas (switch 2)
        pressAxis(SDL_GAMEPAD_AXIS_LEFTX, -32000); // Gauche (switch 1)
        pressAxis(SDL_GAMEPAD_AXIS_LEFTX, 32000);  // Droite (switch 3)

        check(!wizard.isOfferingSecondStick(),
              "second pass finishing does NOT ask the second-stick question again");
        check(wizard.currentPromptIndex() == 4, "...and lands directly on prompt 4 (Start), same as declining would");

        // Tail (Start, Coin, ...): promptIndex_ resumes its ORIGINAL count
        // (4, 5, 6, ...) from prompts(), but the true position is now
        // offset by the second pass's 4 extra steps -- step 9, not 5.
        check(wizard.totalPromptCountForTesting() == 21,
              "total stays 21 for the rest of the run (Start, Coin, ...)");
        check(wizard.currentPromptNumberForTesting() == 9,
              "...and prompt 4 (Start) now reads as step 9 (4 first pass + 4 second pass + 1), not step 5");

        // Skip everything else, save.
        while (wizard.currentPromptIndex() != static_cast<size_t>(-1)) {
            size_t pp = wizard.currentPromptIndex();
            pressKey(SDL_SCANCODE_RETURN);
            if (!waitForPromptAdvance(wizard, pp, 2000)) break;
        }
        // Landed on Confirm -- its "N/21 assigned" summary uses the exact
        // same totalPromptCountForTesting() the Capture screen's step
        // label does, so this also covers that second draw() call site.
        check(wizard.totalPromptCountForTesting() == 21,
              "Confirm screen's assigned-count total is also 21, not 17");
        pressKey(SDL_SCANCODE_RETURN); // Confirm -> save
        pump(wizard, 1000, [&] { return !wizard.active(); });
        check(!wizard.active(), "second-stick run saved and closed");

        {
            dirk::JoystickProfileStore reload;
            reload.load(L, profilePath);
            const dirk::JoystickBinding* prof = reload.find(gpGuid);
            check(prof != nullptr, "second-stick profile round-trips after reload");
            if (prof) {
                // Every one of the 4 directions must have BOTH its D-pad
                // button (pass one) AND its stick axis (pass two) --
                // merged, neither overwriting the other.
                auto btnUp = prof->buttons.find(static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_UP));
                check(btnUp != prof->buttons.end() && btnUp->second == 0, "D-pad UP -> switch 0 (Haut) still there");
                auto btnDown = prof->buttons.find(static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_DOWN));
                check(btnDown != prof->buttons.end() && btnDown->second == 2, "D-pad DOWN -> switch 2 (Bas) still there");
                auto btnLeft = prof->buttons.find(static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_LEFT));
                check(btnLeft != prof->buttons.end() && btnLeft->second == 1, "D-pad LEFT -> switch 1 (Gauche) still there");
                auto btnRight = prof->buttons.find(static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
                check(btnRight != prof->buttons.end() && btnRight->second == 3, "D-pad RIGHT -> switch 3 (Droite) still there");

                auto axisY = prof->axes.find(static_cast<int>(SDL_GAMEPAD_AXIS_LEFTY));
                check(axisY != prof->axes.end() && axisY->second.neg == 0 && axisY->second.pos == 2,
                      "LEFTY axis ALSO bound: neg -> switch 0 (Haut), pos -> switch 2 (Bas)");
                auto axisX = prof->axes.find(static_cast<int>(SDL_GAMEPAD_AXIS_LEFTX));
                check(axisX != prof->axes.end() && axisX->second.neg == 1 && axisX->second.pos == 3,
                      "LEFTX axis ALSO bound: neg -> switch 1 (Gauche), pos -> switch 3 (Droite)");
            }
        }

        SDL_CloseGamepad(gp);
        SDL_DetachVirtualJoystick(gpJoyId);
    }

    // -----------------------------------------------------------------
    // 9) A RAW device never gets offered a second stick (the feature is
    //    gamepad-only -- see JoystickWizard.h's Step::ConfirmSecondStick
    //    comment). Reuses the section-1 raw virtual joystick, still
    //    attached (vjoy/vid).
    // -----------------------------------------------------------------
    {
        std::vector<dirk::JoystickWizard::DeviceInfo> rawOnly = {
            {capturedId, "Dirk Test Cabinet Stick", capturedGuid, dirk::JoystickWizard::DeviceInfo::Kind::Raw},
        };
        wizard.open(L, rawOnly);
        pressKey(SDL_SCANCODE_RETURN); // select the only device
        check(waitForPromptAdvance(wizard, static_cast<size_t>(-1), 2000), "raw-device re-check: capture started");

        for (int i = 0; i < 4; ++i) {
            size_t pp = wizard.currentPromptIndex();
            pressKey(SDL_SCANCODE_RETURN); // skip all 4 directional prompts
            check(waitForPromptAdvance(wizard, pp, 2000), "raw-device re-check: directional prompt skipped");
        }
        check(!wizard.isOfferingSecondStick(),
              "a RAW device reaching prompt 4 is NOT offered a second stick (gamepad-only feature)");
        check(wizard.currentPromptIndex() == 4, "...lands directly on prompt 4 (Start)");
        check(wizard.totalPromptCountForTesting() == 17,
              "...and the total stays the ordinary 17 -- a RAW device never sees 21");
        check(wizard.currentPromptNumberForTesting() == 5,
              "...current position is a plain 5, no leftover +4 from the earlier gamepad run in this same test binary");

        pressKey(SDL_SCANCODE_ESCAPE); // cancel -- this sub-test only cares about the offer not appearing
        pump(wizard, 1000, [&] { return !wizard.active(); });
        check(!wizard.active(), "raw-device re-check run cancelled cleanly");
    }

    // -----------------------------------------------------------------
    // 10) PickScope: a separate wizard instance built with a REAL
    //     gameFolder this time, so the new first screen actually appears.
    //     Reuses the section-1 raw device (still attached: vjoy/
    //     capturedId/capturedGuid).
    // -----------------------------------------------------------------
    {
        namespace fs = std::filesystem;
        fs::path gameFolder = fs::temp_directory_path() / "dirk_joystick_wizard_test_game";
        fs::remove_all(gameFolder, ec);
        fs::create_directories(gameFolder / "Cfg", ec);
        check(!ec, "scratch gameFolder/Cfg/ created for the PickScope test");

        const std::string gameProfilePath = dirk::JoystickProfileStore::defaultPathForGame(gameFolder.string());
        // Same general/shared file sections 1-9 above use -- by this point
        // in the test it holds no entry for capturedGuid (run 2 saved one,
        // section 4 erased it again, section 9 only ever cancelled) --
        // confirmed below rather than assumed, so this section's own
        // "general still untouched" checks aren't vacuous.
        {
            dirk::JoystickProfileStore sanity;
            sanity.load(L, profilePath);
            check(sanity.find(capturedGuid) == nullptr,
                  "sanity: general/shared file has no entry for capturedGuid yet at the start of the PickScope test");
        }

        dirk::JoystickProfileStore gameProfiles;
        dirk::JoystickProfileStore generalProfiles;
        dirk::JoystickWizard scopedWizard(sharedEngineRoot, gameFolder.string(), gameProfiles, generalProfiles);

        std::vector<dirk::JoystickWizard::DeviceInfo> scopedDevices = {
            {capturedId, "Dirk Test Cabinet Stick", capturedGuid, dirk::JoystickWizard::DeviceInfo::Kind::Raw},
        };

        // Small helper: drives a run from PickDevice through to a saved
        // Confirm, capturing exactly one button (`firstButton`) for
        // prompt 0 and skipping every remaining prompt -- everything this
        // sub-test needs to check lives in prompt 0's binding.
        auto runOneCapture = [&](Uint8 firstButton) {
            pressKey(SDL_SCANCODE_RETURN); // select the only device
            check(waitForPromptAdvance(scopedWizard, static_cast<size_t>(-1), 2000),
                  "PickScope test: selecting the device starts capture at prompt 0");
            size_t pp = scopedWizard.currentPromptIndex();
            SDL_SetJoystickVirtualButton(vjoy, firstButton, true);
            check(waitForPromptAdvance(scopedWizard, pp, 2000), "PickScope test: prompt 0 captured");
            SDL_SetJoystickVirtualButton(vjoy, firstButton, false);
            while (scopedWizard.currentPromptIndex() != static_cast<size_t>(-1)) {
                pp = scopedWizard.currentPromptIndex();
                pressKey(SDL_SCANCODE_RETURN);
                if (!waitForPromptAdvance(scopedWizard, pp, 2000)) break;
            }
            pressKey(SDL_SCANCODE_RETURN); // Confirm -> save
            pump(scopedWizard, 1000, [&] { return !scopedWizard.active(); });
            check(!scopedWizard.active(), "PickScope test: run saved and closed");
        };

        // --- 10a: "This game only" (the default selection) -----------
        scopedWizard.open(L, scopedDevices);
        check(scopedWizard.isPickingScope(), "PickScope test: a real gameFolder + a real device makes PickScope appear");
        pressKey(SDL_SCANCODE_RETURN); // confirm the default choice (This game only)
        pump(scopedWizard, 500, [&] { return !scopedWizard.isPickingScope(); });
        check(!scopedWizard.isPickingScope() && scopedWizard.active(),
              "Entree on the default choice (This game only) moves on to PickDevice");
        runOneCapture(3);

        {
            dirk::JoystickProfileStore reload;
            reload.load(L, gameProfilePath);
            const dirk::JoystickBinding* b = reload.find(capturedGuid);
            check(b != nullptr, "\"This game only\": entry exists in the PER-GAME file");
            if (b) {
                auto it = b->buttons.find(3);
                check(it != b->buttons.end() && it->second == 0, "...button 3 -> switch 0 (Haut), as captured");
            }
            dirk::JoystickProfileStore generalReload;
            generalReload.load(L, profilePath);
            check(generalReload.find(capturedGuid) == nullptr,
                  "\"This game only\": general/shared file still has NO entry for this device (no cross-write)");
        }

        // --- 10b: "General (all games)" -------------------------------
        scopedWizard.open(L, scopedDevices);
        check(scopedWizard.isPickingScope(), "PickScope test: reappears on a fresh open()");
        pressKey(SDL_SCANCODE_DOWN); // move selection to "General (all games)"
        pressKey(SDL_SCANCODE_RETURN);
        pump(scopedWizard, 500, [&] { return !scopedWizard.isPickingScope(); });
        check(!scopedWizard.isPickingScope() && scopedWizard.active(), "Down+Entree selects General and moves on");
        runOneCapture(5); // deliberately a DIFFERENT button than 10a, to tell the two saves apart

        {
            dirk::JoystickProfileStore generalReload;
            generalReload.load(L, profilePath);
            const dirk::JoystickBinding* gb = generalReload.find(capturedGuid);
            check(gb != nullptr, "\"General\": entry now exists in the general/shared file");
            if (gb) {
                auto it = gb->buttons.find(5);
                check(it != gb->buttons.end() && it->second == 0, "...button 5 -> switch 0 (Haut), as captured this run");
            }
            dirk::JoystickProfileStore gameReload;
            gameReload.load(L, gameProfilePath);
            const dirk::JoystickBinding* pb = gameReload.find(capturedGuid);
            check(pb != nullptr, "\"General\": the PER-GAME file from 10a is still there, untouched");
            if (pb) {
                auto it = pb->buttons.find(3);
                check(it != pb->buttons.end() && it->second == 0,
                      "...still button 3 from 10a, not overwritten by this run's button 5");
            }
        }

        // --- 10c: Suppr only erases from the currently ACTIVE (scoped)
        //     store -- reopen with the default choice (This game only)
        //     again and erase there; the general entry from 10b must
        //     survive untouched.
        scopedWizard.open(L, scopedDevices);
        pressKey(SDL_SCANCODE_RETURN); // This game only (default)
        pump(scopedWizard, 500, [&] { return !scopedWizard.isPickingScope(); });
        check(gameProfiles.find(capturedGuid) != nullptr,
              "10c: the per-game runtime store sees the entry from 10a right after re-picking \"this game\" scope");
        pressKey(SDL_SCANCODE_DELETE);
        pump(scopedWizard, 500, [&] { return false; });
        pressKey(SDL_SCANCODE_RETURN); // confirm delete
        pump(scopedWizard, 500, [&] { return false; });
        check(gameProfiles.find(capturedGuid) == nullptr, "10c: entry removed from the per-game runtime store");
        pressKey(SDL_SCANCODE_ESCAPE);
        pump(scopedWizard, 500, [&] { return !scopedWizard.active(); });

        {
            dirk::JoystickProfileStore gameReload;
            gameReload.load(L, gameProfilePath);
            check(gameReload.find(capturedGuid) == nullptr, "10c: per-game file on disk no longer has this device");
            dirk::JoystickProfileStore generalReload;
            generalReload.load(L, profilePath);
            const dirk::JoystickBinding* gb = generalReload.find(capturedGuid);
            check(gb != nullptr,
                  "10c: general/shared file (from 10b) is completely untouched by erasing the per-game entry");
            if (gb) {
                auto it = gb->buttons.find(5);
                check(it != gb->buttons.end() && it->second == 0, "...still button 5 from 10b");
            }
        }

        fs::remove_all(gameFolder, ec);
    }

    // -----------------------------------------------------------------
    // 11) F9-while-open regression (2026-09-24, Cyril, real cabinet: "F9
    //     met la video en pause mais si on rappuie sur F9 (au lieu de
    //     escape) la video reste figee"). main.cpp's own F9 handler only
    //     OPENS the wizard (guarded on !wizard.active()), so a second F9
    //     press while it's already open is instead routed into
    //     handleKeyDown just like any other key -- before the fix, F9
    //     matched no case there and did nothing at all, leaving the
    //     wizard stuck open (and the disc paused) with no visible
    //     difference from an actual hang. Fixed by normalizing F9 to
    //     Escape right at the top of handleKeyDown, so it now does
    //     whatever Escape already does at the CURRENT step -- checked
    //     both for a step where Escape fully closes the wizard
    //     (PickDevice) and one where Escape only steps back one level
    //     (ConfirmDelete), proving this isn't a blanket hard-close that
    //     could discard more than Escape itself would from that same
    //     spot.
    // -----------------------------------------------------------------
    {
        std::vector<dirk::JoystickWizard::DeviceInfo> f9Devices = {
            {capturedId, "Dirk Test Cabinet Stick", capturedGuid, dirk::JoystickWizard::DeviceInfo::Kind::Raw},
        };

        // 11a) F9 pressed at PickDevice (same step Escape fully closes
        // from) must close the wizard, same as Escape would.
        wizard.open(L, f9Devices);
        check(wizard.active(), "F9 regression: wizard opens normally");
        {
            SDL_Event f9{};
            f9.type = SDL_EVENT_KEY_DOWN;
            f9.key.scancode = SDL_SCANCODE_F9;
            f9.key.repeat = false;
            check(wizard.handleEvent(f9), "F9 regression: a second F9 while open is consumed (swallowed), not leaked to gameplay");
        }
        check(!wizard.active(), "F9 regression: that second F9 (at PickDevice) closed the wizard -- no more stuck-open/frozen-video case");

        // 11b) F9 pressed at ConfirmDelete (a step where Escape only steps
        // back to PickDevice, not a full close) must do the SAME partial
        // step-back, not force the whole wizard shut -- proving the fix
        // is "F9 = whatever Escape does here", not a blanket override.
        // Whatever this device's profile state happens to be at this point
        // in the file (irrelevant here) -- capture and save a fresh one,
        // purely so ConfirmDelete below has something to offer (PickDevice's
        // Delete key only transitions to ConfirmDelete when the active
        // store's find() actually returns something for this device).
        wizard.open(L, f9Devices);
        pressKey(SDL_SCANCODE_RETURN);
        pump(wizard, 2000, [&] { return wizard.currentPromptIndex() != static_cast<size_t>(-1); });
        check(wizard.currentPromptIndex() == 0, "F9 regression setup: capture started fresh at prompt 0");
        // Skip everything to reach Confirm, then save, so this device HAS
        // a profile again for the DELETE step below to actually operate on.
        while (wizard.currentPromptIndex() != static_cast<size_t>(-1)) {
            size_t pp = wizard.currentPromptIndex();
            pressKey(SDL_SCANCODE_RETURN);
            if (!waitForPromptAdvance(wizard, pp, 2000)) break;
        }
        pressKey(SDL_SCANCODE_RETURN); // Confirm -> save
        pump(wizard, 1000, [&] { return !wizard.active(); });
        check(!wizard.active(), "F9 regression setup: profile saved so ConfirmDelete has something to offer");

        wizard.open(L, f9Devices);
        pressKey(SDL_SCANCODE_DELETE); // PickDevice -> ConfirmDelete (profile exists from just above)
        pump(wizard, 500, [&] { return false; });
        {
            SDL_Event f9{};
            f9.type = SDL_EVENT_KEY_DOWN;
            f9.key.scancode = SDL_SCANCODE_F9;
            f9.key.repeat = false;
            wizard.handleEvent(f9);
        }
        check(wizard.active(), "F9 regression: F9 at ConfirmDelete does NOT close the whole wizard...");
        check(!wizard.isPickingScope(), "...sanity: not somehow back at PickScope either");
        pressKey(SDL_SCANCODE_ESCAPE); // clean up: close for real
        pump(wizard, 500, [&] { return !wizard.active(); });
        check(!wizard.active(), "F9 regression: cleanup close succeeded");
    }

    SDL_DetachVirtualJoystick(vid);
    lua_close(L);

    std::fprintf(stderr, g_allPass ? "[joystick_wizard_test] ALL PASS\n" : "[joystick_wizard_test] SOME FAILED\n");
    return g_allPass ? 0 : 1;
}
