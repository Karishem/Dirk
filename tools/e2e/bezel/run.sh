#!/bin/bash
# Bezels e2e (2026-10-02), see analyze.py: real dirk, fullscreen on a
# virtual X screen (Xvfb) of a given size, the same 4:3 bezel offered to
# three movies (4:3, 16:9, 720x480 with 8:9 pixels = 4:3) -- is the bezel
# drawn, what does the script see, does F11 change anything.
# usage: run.sh <dirk> <input_shim.so> <assets dir> [python3]
set -u
DIRK=$(realpath "$1"); SHIM=$(realpath "$2"); ASSETS=$(realpath "$3"); PY=${4:-python3}
HERE=$(cd "$(dirname "$0")" && pwd)
command -v Xvfb >/dev/null || { echo "SKIP: Xvfb not installed"; exit 0; }
WORK=$(mktemp -d); trap 'rm -rf "$WORK"; [ -n "${XPID:-}" ] && kill $XPID 2>/dev/null' EXIT
# name  movie                               bezel  screen
CASES="m43_wide bezel_movie_43.mp4 yes 1920x1080
m43_square bezel_movie_43.mp4 yes 1024x768
m169_wide bezel_movie_169.mp4 yes 1920x1080
sar_wide_nobezel bezel_movie_720x480_sar8-9.mp4 no 1920x1080
sar_wide bezel_movie_720x480_sar8-9.mp4 yes 1920x1080"
disp=91
echo "$CASES" | while read -r name movie bezel screen; do
    G="$WORK/$name"; mkdir -p "$G"; cp -r "$HERE/game/." "$G/"
    mkdir -p "$G/KimmyScript/Cfg"; cp "$ASSETS/dirk_loading/dirk_loading_font.ttf" "$G/KimmyScript/Cfg/"
    [ "$bezel" = yes ] && { mkdir -p "$G/Bezel"; cp "$ASSETS/bezel_43_window.png" "$G/Bezel/bezel.png"; }
    cp "$ASSETS/$movie" "$G/movie.mp4"
    disp=$((disp + 1))
    Xvfb :$disp -screen 0 ${screen}x24 -nolisten tcp >/dev/null 2>&1 & XPID=$!
    sleep 1
    TL="1200,0,p,0,0,${name}_before;1400,0,k,68,1,f11;1500,0,k,68,0,f11up;1900,0,k,224,1,ctrl;2000,0,k,224,0,ctrlup;2300,0,p,0,0,${name}_after;2600,0,h,0,0,END"
    ( cd "$G" && DISPLAY=:$disp DIRK_FORCE_FULLSCREEN=1 DIRK_TEST_TIMELINE="$TL" DIRK_TEST_SHOTDIR="$WORK" DIRK_DEBUG_SHOT=1 \
      SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 SDL_VIDEO_DRIVER=x11 SDL_AUDIO_DRIVER=dummy LD_PRELOAD="$SHIM" \
      timeout -s INT 15 "$DIRK" "$G/movie.mp4" "$G/bezel_e2e.lua" > "$WORK/$name.log" 2>&1 )
    kill $XPID 2>/dev/null; wait $XPID 2>/dev/null; XPID=
done
[ -n "${DIRK_TEST_KEEP:-}" ] && cp -r "$WORK" "$DIRK_TEST_KEEP"
"$PY" "$HERE/analyze.py" "$WORK"
