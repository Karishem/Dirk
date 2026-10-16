#!/bin/bash
# Joystick wizard without a keyboard (2026-10-03), see analyze.py: opened
# from the script (Dirk Options menu item) and driven with a gamepad only,
# through the game's own controls.cfg (stick up/down, A = OK, B = back).
# usage: run.sh <dirk> <input_shim.so> <video.mp4> [python3]
set -u
DIRK=$(realpath "$1"); SHIM=$(realpath "$2"); VIDEO=$(realpath "$3"); PY=${4:-python3}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
cp -r "$HERE/game/." "$WORK/"; mkdir -p "$WORK/KimmyScript/Cfg"
# dev 1 = the virtual gamepad: b = button (0 A, 1 B, 2 X), a = axis (1 = left Y), k = key
TL="1000,1,b,0,1,A_to_game;1100,1,b,0,0,-;"
TL+="1500,1,b,1,1,B_open;1600,1,b,1,0,-;"
TL+="2200,1,a,1,32767,down_scope;2300,1,a,1,0,-;"
TL+="2800,1,b,0,1,A_scope_ok;2900,1,b,0,0,-;"
TL+="3400,1,a,1,32767,down_device;3500,1,a,1,0,-;"
TL+="4000,1,b,0,1,A_device_ok;4100,1,b,0,0,-;"
TL+="4600,1,b,2,1,X_captured;4700,1,b,2,0,-;"
TL+="5200,0,k,41,1,esc_cancel;5250,0,k,41,0,-;"
TL+="5800,1,b,1,1,B_open_again;5900,1,b,1,0,-;"
TL+="6400,1,b,1,1,B_back;6500,1,b,1,0,-;"
TL+="7000,1,b,0,1,A_to_game_again;7100,1,b,0,0,-;"
# R36S case (2026-10-03): D-pad for the 4 directions, then Start pressed on
# the "other input too?" question must NOT answer yes (it then made the
# stick-only pass ignore Start); B answers no, and Start is then recorded.
TL+="7600,1,b,1,1,B_open3;7700,1,b,1,0,-;8200,1,b,0,1,A_scope3;8300,1,b,0,0,-;"
TL+="8800,1,a,1,32767,down3;8900,1,a,1,0,-;9400,1,b,0,1,A_dev3;9500,1,b,0,0,-;"
TL+="10000,1,b,11,1,up;10100,1,b,11,0,-;10500,1,b,12,1,down;10600,1,b,12,0,-;"
TL+="11000,1,b,13,1,left;11100,1,b,13,0,-;11500,1,b,14,1,right;11600,1,b,14,0,-;"
TL+="12200,1,b,6,1,START_on_question;12300,1,b,6,0,-;12900,1,b,1,1,B_no;13000,1,b,1,0,-;"
TL+="13600,1,b,6,1,START_prompt;13700,1,b,6,0,-;14300,0,k,41,1,esc3;14350,0,k,41,0,-;14800,0,h,0,0,END"
( cd "$WORK" && DIRK_TEST_TIMELINE="$TL" DIRK_TEST_GAMEPAD=1 SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 \
  SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy LD_PRELOAD="$SHIM" \
  timeout -s INT 18 "$DIRK" "$VIDEO" "$WORK/wizard_nav_e2e.lua" > "$WORK/run.log" 2>&1 )
[ -n "${DIRK_TEST_KEEP:-}" ] && cp -r "$WORK" "$DIRK_TEST_KEEP"
"$PY" "$HERE/analyze.py" "$WORK/run.log"
