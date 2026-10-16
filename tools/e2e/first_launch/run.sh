#!/bin/bash
# First-launch e2e (2026-09-26): controllers plugged in at startup must work
# on a game's FIRST launch too -- the one that builds the .dirkindex behind
# the INDEXING screen. That screen's event loop used to swallow SDL's
# "controller added" events, so on first launch no controller was opened
# at all (not even listed by the F9 wizard); relaunching fixed it. Runs the
# real dirk twice on a copy of the clip with no index -- fresh, then cached
# -- with a virtual raw stick and a virtual gamepad (../input_shim.cpp),
# and checks each run opened both and received a press from each.
# usage: run.sh <dirk> <input_shim.so> <test_av_chirp.mp4>
set -u
DIRK=$1; SHIM=$2; VIDEO=$3
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
cp -r "$HERE/../native_pause/game/." "$WORK/"      # same script + controls.cfg
cp "$VIDEO" "$WORK/clip.mp4"                        # no .dirkindex next to it
TL="2500,0,b,3,1,raw_button;2600,0,b,3,0,-;3000,1,a,1,32767,pad_axis;3100,1,a,1,0,-;3800,0,h,0,0,END"
ok=1
for run in fresh cached; do
    DIRK_VERBOSE=1 DIRK_TEST_TIMELINE="$TL" DIRK_TEST_GAMEPAD=1 SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 \
    SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy LD_PRELOAD="$SHIM" \
    timeout -k 3 -s INT 6 "$DIRK" "$WORK/clip.mp4" "$WORK/native_pause_e2e.lua" > "$WORK/$run.log" 2>&1
    raw=$(grep -c "joystick connected" "$WORK/$run.log")
    pad=$(grep -c "gamepad connected" "$WORK/$run.log")
    p6=$(grep -c "^PRESSED 6$" "$WORK/$run.log")
    p19=$(grep -c "^PRESSED 19$" "$WORK/$run.log")
    idx=no; [ -f "$WORK/clip.mp4.dirkindex" ] && idx=yes
    if [ "$raw" -ge 1 ] && [ "$pad" -ge 1 ] && [ "$p6" -ge 1 ] && [ "$p19" -ge 1 ]; then r=PASS; else r=FAIL; ok=0; fi
    echo "$r $run launch: raw stick opened=$raw, gamepad opened=$pad, raw button press=$p6, gamepad axis press=$p19 (index now on disk: $idx)"
done
[ $ok = 1 ] && echo "ALL PASS" || echo "SOME FAILED"
[ $ok = 1 ]
