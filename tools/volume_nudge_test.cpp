// Unit test for dirk::nudgeVolumes() (see LaunchSettings.h/.cpp) -- the
// small free function behind main.cpp's +/- volume hotkeys (Cyril,
// 2026-09-23: "make + and - raise/lower both volumes by 10%, clamped to
// 0..100, even though the two aren't necessarily set the same").
//
// Deliberately NOT an SDL/virtual-joystick style end-to-end test like
// joystick_wizard_test.cpp: nudgeVolumes() is a pure function over a plain
// struct, so this just calls it directly and checks the resulting fields --
// no window, no renderer, no event loop, runs in a few milliseconds.
//
// Covers: a plain step in each direction, clamping at the 100 ceiling and
// the 0 floor, clamping each field INDEPENDENTLY against its own value
// (this is the whole point of the feature -- the two volumes are not
// forced to match each other), and the asymmetric real defaults documented
// in LaunchSettings.h (volumeVldp=100, volumeNonVldp=60) so a fresh
// LaunchSettings behaves the way an actual first launch would.

#include "core/LaunchSettings.h"

#include <cstdio>

static bool g_allPass = true;

static void check(bool cond, const char* what) {
    if (cond) {
        std::printf("PASS %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        g_allPass = false;
    }
}

int main() {
    // 1) Plain raise from the real defaults (vldp=100, nonvldp=60): vldp is
    //    already at the ceiling and must clamp there instead of overshooting
    //    to 110; nonvldp has room and simply moves by 10.
    {
        dirk::LaunchSettings s; // defaults: volumeVldp=100, volumeNonVldp=60
        dirk::nudgeVolumes(s, 10);
        check(s.volumeVldp == 100, "raise from defaults: vldp clamps at ceiling (100)");
        check(s.volumeNonVldp == 70, "raise from defaults: nonvldp moves 60 -> 70");
    }

    // 2) Plain lower from the real defaults: both fields have room, both
    //    just move by -10, independently.
    {
        dirk::LaunchSettings s;
        dirk::nudgeVolumes(s, -10);
        check(s.volumeVldp == 90, "lower from defaults: vldp moves 100 -> 90");
        check(s.volumeNonVldp == 50, "lower from defaults: nonvldp moves 60 -> 50");
    }

    // 3) Repeated lowers walk both fields down to exactly 0 and then stop
    //    there -- never negative -- each field clamping on its own schedule
    //    since they started at different values (vldp needs 10 nudges from
    //    100, nonvldp only needs 6 from 60).
    {
        dirk::LaunchSettings s;
        for (int i = 0; i < 12; ++i) dirk::nudgeVolumes(s, -10);
        check(s.volumeVldp == 0, "12x lower: vldp floors at 0, never negative");
        check(s.volumeNonVldp == 0, "12x lower: nonvldp floors at 0, never negative");
    }

    // 4) Repeated raises walk both fields up to exactly 100 and stop there --
    //    never past it -- again each on its own schedule.
    {
        dirk::LaunchSettings s;
        for (int i = 0; i < 12; ++i) dirk::nudgeVolumes(s, 10);
        check(s.volumeVldp == 100, "12x raise: vldp ceilings at 100, never past");
        check(s.volumeNonVldp == 100, "12x raise: nonvldp ceilings at 100, never past");
    }

    // 5) Independence check with a custom starting point where only ONE of
    //    the two fields is at a boundary: vldp pinned at 100 (must stay put
    //    on a raise) while nonvldp sits mid-range (must still move freely).
    //    This is the actual behaviour Cyril asked for -- "clamped to
    //    0..100, even though the two aren't necessarily set the same" --
    //    so it's worth checking on its own, not just inferred from the
    //    all-defaults cases above.
    {
        dirk::LaunchSettings s;
        s.volumeVldp = 100;
        s.volumeNonVldp = 35;
        dirk::nudgeVolumes(s, 10);
        check(s.volumeVldp == 100, "independent clamp: vldp already at ceiling stays put");
        check(s.volumeNonVldp == 45, "independent clamp: nonvldp still free to move (35 -> 45)");
    }

    // 6) Same independence check on the floor side: nonvldp pinned at 0
    //    (must stay put on a lower) while vldp sits mid-range (must still
    //    move freely).
    {
        dirk::LaunchSettings s;
        s.volumeVldp = 45;
        s.volumeNonVldp = 0;
        dirk::nudgeVolumes(s, -10);
        check(s.volumeVldp == 35, "independent clamp: vldp still free to move (45 -> 35)");
        check(s.volumeNonVldp == 0, "independent clamp: nonvldp already at floor stays put");
    }

    // 7) A delta that isn't a clean multiple of the field's distance to the
    //    boundary still clamps exactly at the boundary, not past it (e.g. a
    //    field at 95 raised by 10 lands at 100, not 105).
    {
        dirk::LaunchSettings s;
        s.volumeVldp = 95;
        s.volumeNonVldp = 5;
        dirk::nudgeVolumes(s, 10);
        check(s.volumeVldp == 100, "uneven step clamps exactly at ceiling (95 + 10 -> 100)");
        dirk::nudgeVolumes(s, -20); // nonvldp: 5 +10 -> 15, then -20 -> clamp 0
        check(s.volumeNonVldp == 0, "uneven step clamps exactly at floor (15 - 20 -> 0)");
    }

    std::printf(g_allPass ? "\nALL PASS\n" : "\nSOME FAILED\n");
    return g_allPass ? 0 : 1;
}
