#!/bin/bash
# Review point 6 e2e (2026-09-26), see analyze.py. Same driver as
# ../native_pause (input_shim.cpp via LD_PRELOAD, headless).
# usage: run.sh <dirk> <input_shim.so> <test_av_chirp.mp4> [python3]
set -u
DIRK=$1; SHIM=$2; VIDEO=$3; PY=${4:-python3}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
cp -r "$HERE/game/." "$WORK/"
TL="800,0,p,0,0,shot_off;1200,0,k,69,1,scan_on;1250,0,k,69,0,-;1600,0,p,0,0,shot_on;"
TL+="2000,0,k,87,1,vol_plus;2050,0,k,87,0,-;2400,0,h,0,0,after_plus;"
TL+="2500,0,b,3,1,same_values;2600,0,b,3,0,-;2900,0,p,0,0,after_same;3000,0,p,0,0,shot_same;"
TL+="3300,0,k,69,1,scan_off;3350,0,k,69,0,-;3700,0,p,0,0,shot_off2;4000,0,h,0,0,END"
DIRK_TEST_TIMELINE="$TL" DIRK_TEST_SHOTDIR="$WORK" DIRK_TEST_WATCH="$WORK/Cfg/game.cfg" DIRK_DEBUG_SHOT=1 \
SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy LD_PRELOAD="$SHIM" \
timeout -s INT 6 "$DIRK" "$VIDEO" "$WORK/settings_apply_e2e.lua" > "$WORK/run.log" 2>&1
"$PY" "$HERE/analyze.py" "$WORK/run.log" "$WORK" "$WORK/Cfg/game.cfg"
