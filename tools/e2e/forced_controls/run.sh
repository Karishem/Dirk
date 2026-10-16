#!/bin/bash
# Forced controls (2026-10-08, for Create.dirk), see analyze.py: the same
# presses three times -- with forcedControls naming a file, without it, and
# with a name that holds a path (refused).
# usage: run.sh <dirk> <input_shim.so> <video.mp4> [python3]
set -u
DIRK=$(realpath "$1"); SHIM=$(realpath "$2"); VIDEO=$(realpath "$3"); PY=${4:-python3}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
# dev 0 = raw stick (b0 forced, b3 = profile), dev 1 = gamepad (b0 A, b2 X), k 44 = Space
TL="1000,1,b,2,1,pad_X;1100,1,b,2,0,-;1500,0,b,0,1,raw_0;1600,0,b,0,0,-;"
TL+="2000,0,k,44,1,space;2100,0,k,44,0,-;2500,1,b,0,1,pad_A;2600,1,b,0,0,-;"
TL+="3000,0,b,3,1,raw_3;3100,0,b,3,0,-;3500,0,h,0,0,END"
for run in forced none path; do
  case $run in forced) FC=forced_e2e.cfg;; none) FC=;; path) FC=../KimmyScript/Cfg/forced_e2e.cfg;; esac
  rm -rf "$WORK/g"; mkdir -p "$WORK/g"; cp -r "$HERE/game/." "$WORK/g/"
  echo "RUN $run"
  ( cd "$WORK/g" && DIRK_HOME="$WORK/g" FC="$FC" DIRK_TEST_TIMELINE="$TL" DIRK_TEST_GAMEPAD=1 SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 \
    SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy LD_PRELOAD="$SHIM" \
    timeout -s INT 8 "$DIRK" "$VIDEO" "$WORK/g/forced_controls_e2e.lua" 2>&1 )
done > "$WORK/run.log"
[ -n "${DIRK_TEST_KEEP:-}" ] && cp -r "$WORK" "$DIRK_TEST_KEEP"
"$PY" "$HERE/analyze.py" "$WORK/run.log"
