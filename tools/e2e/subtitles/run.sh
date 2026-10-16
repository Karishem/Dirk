#!/bin/bash
# Subtitles e2e (2026-09-27), see analyze.py. Real dirk, headless, driven
# by ../input_shim.cpp (LD_PRELOAD): a solid-colour 1440x1080 clip with
# clip.srt (untagged, "On") and clip-fr.srt ("FR") next to it; the script
# jumps around the movie on each Left Ctrl press, a screenshot after each.
# usage: run.sh <dirk> <input_shim.so> <subtitle_clip.mp4> <dirk_loading_font.ttf> [python3]
set -u
DIRK=$1; SHIM=$2; VIDEO=$3; FONT=$4; PY=${5:-python3}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/game" "$WORK/KimmyScript/Cfg"
cp -r "$HERE/game/." "$WORK/game/"
cp "$VIDEO" "$WORK/game/clip.mp4"
cp "$FONT" "$WORK/KimmyScript/Cfg/dirk_loading_font.ttf"
TL="800,0,p,0,0,s0_before"
t=1000
for i in 1 2 3 4 5 6 7 8 9; do
    TL+=";$t,0,k,224,1,press$i;$((t+100)),0,k,224,0,rel$i;$((t+500)),0,p,0,0,s$i"
    t=$((t+700))
done
TL+=";$((t+200)),0,h,0,0,END"
DIRK_TEST_TIMELINE="$TL" DIRK_TEST_SHOTDIR="$WORK" DIRK_DEBUG_SHOT=1 \
SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy LD_PRELOAD="$SHIM" \
timeout -s INT 14 "$DIRK" "$WORK/game/clip.mp4" "$WORK/game/subtitles_e2e.lua" > "$WORK/run.log" 2>&1
[ -n "${DIRK_TEST_KEEP:-}" ] && cp -r "$WORK" "$DIRK_TEST_KEEP"
"$PY" "$HERE/analyze.py" "$WORK/run.log" "$WORK"
