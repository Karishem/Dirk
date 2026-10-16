// Unit test for dirk::buildProfileKey() and JoystickProfileStore's
// composite-key fallback (see JoystickProfile.h) -- the fix for a real
// bug Cyril reported on his actual arcade cabinet (2026-09-24): "je ne
// suis pas sur que les deux joystick de l'arcade aient ete reconnus
// separement. Ils apparaissent bien tous les deux dans la liste mais pas
// tous les deux dans le fichier config apres un save".
//
// Root cause, confirmed for real (not guessed) via a standalone probe
// (SDL_AttachVirtualJoystick with two virtual devices given identical
// vendor_id/product_id/name/type -- exactly two identical cheap arcade
// encoder boards, the normal case for a 2-player cabinet): SDL's own
// SDL_GetJoystickGUID() produces a BYTE-IDENTICAL guid for both. Since
// JoystickProfileStore matched purely on `guid`, the second device's save
// silently overwrote the first's entry.
//
// Deliberately NOT an SDL/virtual-joystick end-to-end test like
// joystick_wizard_test.cpp: SDL_GetJoystickPathForID()/SDL_GetJoystickPath()
// return null for a virtual joystick (confirmed the same way, same probe --
// there is no OS-level device node behind one), so the collision itself
// can't be driven through the real wizard flow in an automated test. This
// instead tests buildProfileKey() and JoystickProfileStore directly against
// the exact key shapes real hardware produces (plain guid when no path is
// available -- unchanged, pre-existing behaviour; "<guid>#<path>" when a
// path is -- the new behaviour), which is the actual logic fixing the bug
// and is fully exercisable without any real duplicate hardware.
#include "core/JoystickProfile.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

static bool g_allPass = true;

static void check(bool cond, const char* what) {
    if (cond) {
        std::printf("PASS %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        g_allPass = false;
    }
}

static dirk::JoystickBinding makeBinding(const std::string& guid, const std::string& name) {
    dirk::JoystickBinding b;
    b.guid = guid;
    b.name = name;
    b.buttons[0] = 6; // SWITCH_BUTTON1 -- just needs to be a real switch id for save()/load() to round-trip it
    return b;
}

int main() {
    // -----------------------------------------------------------------
    // 1) buildProfileKey() itself: pure function, no store involved.
    // -----------------------------------------------------------------
    check(dirk::buildProfileKey("GUID-A", nullptr) == "GUID-A",
          "null path -> plain guid, unchanged from this engine's pre-fix behaviour");
    check(dirk::buildProfileKey("GUID-A", "") == "GUID-A",
          "empty path -> plain guid, same as null");
    check(dirk::buildProfileKey("GUID-A", "/dev/input/event7") == "GUID-A#/dev/input/event7",
          "real path -> composite \"guid#path\" key");

    // -----------------------------------------------------------------
    // 2) The actual collision Cyril hit: two devices, identical GUID
    //    (confirmed real via probe.cpp), disambiguated by path exactly
    //    like SDL_GetJoystickPath() would report for two boards on two
    //    different USB ports.
    // -----------------------------------------------------------------
    {
        const std::string sharedGuid = "030000005e0400008e02000010010000"; // same for both, like the real report
        const std::string keyA = dirk::buildProfileKey(sharedGuid, "/dev/input/event5");
        const std::string keyB = dirk::buildProfileKey(sharedGuid, "/dev/input/event6");
        check(keyA != keyB, "two devices sharing a GUID get DIFFERENT keys once their paths differ");

        dirk::JoystickProfileStore store;
        store.upsert(makeBinding(keyA, "Arcade Stick P1"));
        store.upsert(makeBinding(keyB, "Arcade Stick P2"));
        check(store.all().size() == 2, "both devices saved as SEPARATE entries, not one overwriting the other");

        const dirk::JoystickBinding* a = store.find(keyA);
        const dirk::JoystickBinding* b = store.find(keyB);
        check(a != nullptr && a->name == "Arcade Stick P1", "device A's own entry round-trips under its own key");
        check(b != nullptr && b->name == "Arcade Stick P2", "device B's own entry round-trips under its own key, not A's");

        // Full save()/load() round trip through a real Lua file, same as
        // the wizard actually does -- proves the fix survives the file
        // format too, not just the in-memory store.
        lua_State* L = luaL_newstate();
        const char* path = "/tmp/dirk_joystick_profile_test.cfg";
        check(store.save(path), "save() writes the file with both entries");
        dirk::JoystickProfileStore reloaded;
        reloaded.load(L, path);
        check(reloaded.all().size() == 2, "reload from disk: still two separate entries, not merged into one");
        const dirk::JoystickBinding* ra = reloaded.find(keyA);
        const dirk::JoystickBinding* rb = reloaded.find(keyB);
        check(ra != nullptr && ra->name == "Arcade Stick P1", "reload: device A's entry still resolves under its own key");
        check(rb != nullptr && rb->name == "Arcade Stick P2", "reload: device B's entry still resolves under its own key");
        lua_close(L);
    }

    // -----------------------------------------------------------------
    // 3) Backward compatibility: a profile saved under the OLD plain-
    //    GUID scheme (every already-configured device with no colliding
    //    sibling -- the overwhelming existing case) must keep resolving
    //    once this engine starts asking for it by a composite key,
    //    with zero migration step required from Cyril.
    // -----------------------------------------------------------------
    {
        dirk::JoystickProfileStore store;
        const std::string plainGuid = "030000005e0400008e02000010010000";
        store.upsert(makeBinding(plainGuid, "Old Stick, Saved Before This Fix"));

        const std::string composite = dirk::buildProfileKey(plainGuid, "/dev/input/event3");
        const dirk::JoystickBinding* found = store.find(composite);
        check(found != nullptr && found->name == "Old Stick, Saved Before This Fix",
              "find() with a composite key still resolves a pre-existing plain-guid entry (fallback)");

        check(store.remove(composite), "remove() with a composite key also removes a pre-existing plain-guid entry");
        check(store.find(composite) == nullptr, "...and it's actually gone after that");
    }

    // -----------------------------------------------------------------
    // 4) upsert() hygiene: re-running the wizard for a device that NOW
    //    gets a composite key (its sibling is connected this time) must
    //    not leave the old bare-guid entry behind as an orphan.
    // -----------------------------------------------------------------
    {
        dirk::JoystickProfileStore store;
        const std::string plainGuid = "030000005e0400008e02000010010000";
        store.upsert(makeBinding(plainGuid, "Stick, First Configured Alone"));
        check(store.all().size() == 1, "sanity: one entry so far");

        const std::string composite = dirk::buildProfileKey(plainGuid, "/dev/input/event3");
        store.upsert(makeBinding(composite, "Stick, Reconfigured With Sibling Connected"));
        check(store.all().size() == 1,
              "re-saving under a composite key replaces the old bare-guid entry, no orphan left behind");
        const dirk::JoystickBinding* found = store.find(composite);
        check(found != nullptr && found->name == "Stick, Reconfigured With Sibling Connected",
              "the surviving entry is the new one, under the composite key");
    }

    // -----------------------------------------------------------------
    // 5) resolveSharedAsset() -- the fix for Cyril's "pour pas pourrir
    //    la racine" request (2026-09-23): prefers
    //    <sharedEngineRoot>/KimmyScript/Cfg/<filename> when it exists,
    //    falls back to the bare <sharedEngineRoot>/<filename> spot the
    //    file used to have to live at, with no forced migration.
    // -----------------------------------------------------------------
    {
        namespace fs = std::filesystem;
        fs::path root = fs::temp_directory_path() / "dirk_resolve_shared_asset_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "KimmyScript" / "Cfg", ec);

        // Neither location has the file yet -- falls back to the bare
        // root spot (still returned even though nothing is there: callers
        // decide what a missing file means, same as the font/logo do today).
        const std::string sharedEngineRoot = root.string();
        check(dirk::resolveSharedAsset(sharedEngineRoot, "dirk_loading_font.ttf") ==
                  sharedEngineRoot + "/dirk_loading_font.ttf",
              "resolveSharedAsset(): falls back to the bare root when neither location has the file");

        // Old-style install: file only at the bare root (where it used to
        // have to be) -- must still resolve there, unchanged.
        {
            std::ofstream f((root / "dirk_loading_font.ttf").string());
            f << "not a real font, just needs to exist";
        }
        check(dirk::resolveSharedAsset(sharedEngineRoot, "dirk_loading_font.ttf") ==
                  sharedEngineRoot + "/dirk_loading_font.ttf",
              "resolveSharedAsset(): an old install with the file only at the bare root keeps resolving there");

        // Now also place it in KimmyScript/Cfg/ (the tidier spot Cyril
        // asked for) -- that one must now win.
        {
            std::ofstream f((root / "KimmyScript" / "Cfg" / "dirk_loading_font.ttf").string());
            f << "not a real font, just needs to exist";
        }
        check(dirk::resolveSharedAsset(sharedEngineRoot, "dirk_loading_font.ttf") ==
                  sharedEngineRoot + "/KimmyScript/Cfg/dirk_loading_font.ttf",
              "resolveSharedAsset(): KimmyScript/Cfg/ is preferred once the file is there too");

        fs::remove_all(root, ec);
    }

    // -----------------------------------------------------------------
    // 6) defaultPathForGame() and the per-game-then-general resolution
    //    order (2026-09-24, Cyril: "je veux que dans le premier menu tu
    //    demandes si on fait la configuration pour CE jeu ... ou en
    //    general"). The lookup-order half of this (checking the per-game
    //    store before the general one) is a 2-line lambda duplicated
    //    verbatim in main.cpp (findJoystickProfile) -- reproduced here so
    //    it's exercised against real JoystickProfileStore objects and real
    //    files, without needing SDL or the wizard at all (see
    //    joystick_wizard_test.cpp's own section 10 for the wizard/UI half
    //    of this feature: the new PickScope screen actually saving into
    //    the right file).
    // -----------------------------------------------------------------
    {
        namespace fs = std::filesystem;
        fs::path gameFolder = fs::temp_directory_path() / "dirk_joystick_profile_test_game";
        std::error_code ec;
        fs::remove_all(gameFolder, ec);
        fs::create_directories(gameFolder / "Cfg", ec);

        check(dirk::JoystickProfileStore::defaultPathForGame(gameFolder.string()) == gameFolder.string() + "/Cfg/dirk_joystick_profiles.cfg",
              "defaultPathForGame(): same Cfg/ convention as game.cfg, under the game's own folder");

        const std::string guidGameOnly = "guid-configured-per-game-only";
        const std::string guidGeneralOnly = "guid-configured-general-only";
        const std::string guidBoth = "guid-configured-in-both";

        dirk::JoystickProfileStore gameStore;
        gameStore.upsert(makeBinding(guidGameOnly, "Per-Game Only"));
        {
            dirk::JoystickBinding b = makeBinding(guidBoth, "In Both (per-game copy)");
            b.buttons[0] = 4; // a different switch than the general copy below, so the two are distinguishable
            gameStore.upsert(b);
        }

        dirk::JoystickProfileStore generalStore;
        generalStore.upsert(makeBinding(guidGeneralOnly, "General Only"));
        {
            dirk::JoystickBinding b = makeBinding(guidBoth, "In Both (general copy)");
            b.buttons[0] = 9; // deliberately different from the per-game copy's switch 4 above
            generalStore.upsert(b);
        }

        // Same 2-line resolution order as main.cpp's findJoystickProfile().
        auto findJoystickProfile = [&](const std::string& guid) -> const dirk::JoystickBinding* {
            if (const dirk::JoystickBinding* p = gameStore.find(guid)) return p;
            return generalStore.find(guid);
        };

        const dirk::JoystickBinding* onlyGame = findJoystickProfile(guidGameOnly);
        check(onlyGame != nullptr && onlyGame->name == "Per-Game Only",
              "a device configured only per-game resolves from the per-game store");

        const dirk::JoystickBinding* onlyGeneral = findJoystickProfile(guidGeneralOnly);
        check(onlyGeneral != nullptr && onlyGeneral->name == "General Only",
              "a device configured only generally resolves from the general store (fallback works)");

        const dirk::JoystickBinding* both = findJoystickProfile(guidBoth);
        check(both != nullptr && both->name == "In Both (per-game copy)",
              "a device configured in BOTH stores resolves the PER-GAME one -- it's checked first");
        if (both) {
            auto it = both->buttons.find(0);
            check(it != both->buttons.end() && it->second == 4,
                  "...specifically the per-game copy's own binding (switch 4), not the general copy's (switch 9)");
        }

        check(findJoystickProfile("guid-configured-nowhere") == nullptr,
              "a device configured in neither store resolves to nullptr (falls through to switchMap/controls.cfg in main.cpp)");

        // Round trip through real files too, same as section 2 above --
        // proves this isn't just an in-memory-object artifact.
        {
            const std::string gamePath = dirk::JoystickProfileStore::defaultPathForGame(gameFolder.string());
            check(gameStore.save(gamePath), "per-game store saves to its own real file");
            lua_State* L = luaL_newstate();
            dirk::JoystickProfileStore reloadedGame;
            reloadedGame.load(L, gamePath);
            const dirk::JoystickBinding* reloaded = reloadedGame.find(guidGameOnly);
            check(reloaded != nullptr && reloaded->name == "Per-Game Only",
                  "per-game file round-trips from disk, same as the general file already does");
            lua_close(L);
        }

        fs::remove_all(gameFolder, ec);
    }

    std::printf(g_allPass ? "\nALL TESTS PASSED\n" : "\nSOME TESTS FAILED\n");
    return g_allPass ? 0 : 1;
}
