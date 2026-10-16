#!/bin/bash
# Files left open by a script error repeated every frame (see the .lua):
# Dirk's open file count must stay flat, not grow with the frames.
# usage: run.sh <dirk> <video.mp4> [python3]
set -u
DIRK=$(realpath "$1"); VIDEO=$(realpath "$2")
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
cp -r "$HERE/game/." "$WORK/"; mkdir -p "$WORK/KimmyScript/Cfg"
sed -i "s#DIR_FILES#\"$WORK\"#" "$WORK/files_e2e.lua"
( cd "$WORK" && SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy exec "$DIRK" "$VIDEO" "$WORK/files_e2e.lua" > "$WORK/run.log" 2>&1 ) &
PID=$!
counts=""
for i in 1 2 3 4 5 6 7 8; do sleep 1; c=$(ls /proc/$PID/fd 2>/dev/null | wc -l); counts="$counts $c"; done
kill -INT $PID 2>/dev/null; wait $PID 2>/dev/null
errs=$(grep -c "onOverlayUpdate error" "$WORK/run.log"); frames=$(grep -o 'FRAMES [0-9]*' "$WORK/run.log" | tail -1)
echo "open files each second:$counts ($frames, $errs error lines)"
first=$(echo $counts | cut -d' ' -f2); last=$(echo $counts | awk '{print $NF}')
if [ -n "$first" ] && [ "$last" -le $((first + 5)) ] && [ "${frames#FRAMES }" -ge 120 ] 2>/dev/null; then echo "PASS open files stay flat while the error repeats"; exit 0; fi
echo "FAIL open files grow (or the script didn't run long enough)"; exit 1
