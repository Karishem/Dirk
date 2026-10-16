#!/bin/bash
# Review point 4 e2e (2026-09-26): drives a real dirk binary, headless, with
# SDL virtual joysticks (raw stick + recognised gamepad) and synthetic keys
# injected by input_shim (LD_PRELOAD), then checks from the frame numbers
# the script prints which scenes played and which stayed frozen:
#   A/B/C  Pause bound to a raw hat / raw axis / gamepad axis freezes and
#          unfreezes the picture (before: only buttons/keys/mouse did)
#   D      native Pause, then the script parks on a menu (seek + discPause),
#          then Pause again: the menu must stay frozen (before: it played)
#   E      same with the F9 joystick wizard instead of Pause
# usage: run.sh <dirk> <input_shim.so> <test_av_chirp.mp4> [python3]
set -u
DIRK=$1; SHIM=$2; VIDEO=$3; PY=${4:-python3}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
cp -r "$HERE/game/." "$WORK/"
TL="1500,0,h,0,1,A_hat_press1;1600,0,h,0,0,-;2300,0,h,0,1,A_hat_press2;2400,0,h,0,0,-;"
TL+="3000,0,a,1,32767,B_axis_press1;3100,0,a,1,0,-;3800,0,a,1,32767,B_axis_press2;3900,0,a,1,0,-;"
TL+="4500,1,a,1,32767,C_gpaxis_press1;4600,1,a,1,0,-;5300,1,a,1,32767,C_gpaxis_press2;5400,1,a,1,0,-;"
TL+="6000,0,b,5,1,D_native_pause;6100,0,b,5,0,-;6500,0,b,3,1,D_menu;6600,0,b,3,0,-;7300,0,b,5,1,D_pause_again;7400,0,b,5,0,-;"
TL+="7800,0,b,4,1,E_script_play;7900,0,b,4,0,-;8600,0,k,66,1,E_F9_wizard;8650,0,k,66,0,-;10000,0,k,41,1,E_esc_close;10050,0,k,41,0,-;11000,0,h,0,0,END"
DIRK_TEST_TIMELINE="$TL" DIRK_TEST_GAMEPAD=1 SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 \
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy LD_PRELOAD="$SHIM" \
timeout -s INT 13 "$DIRK" "$VIDEO" "$WORK/native_pause_e2e.lua" > "$WORK/run.log" 2>&1
"$PY" "$HERE/analyze.py" "$WORK/run.log"
