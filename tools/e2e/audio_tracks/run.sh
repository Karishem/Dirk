#!/bin/bash
# Audio tracks e2e (2026-09-27), see analyze.py. Real dirk, headless:
# Hypseus-style game with three soundtracks (440 Hz original, 880 Hz -fr,
# 1320 Hz _it), switched live from Lua; Dirk's actual audio output is
# recorded with SDL's "disk" audio driver. Then relaunches to check the
# saved choice, and adds a soundtrack to check the cached .mkv is rebuilt.
# usage: run.sh <dirk> <input_shim.so> <assets/hypseus_multiaudio> [python3]
set -u
DIRK=$1; SHIM=$2; ASSETS=$3; PY=${4:-python3}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/game/Video"
cp -r "$HERE/game/." "$WORK/game/"
cp "$ASSETS/test.m2v" "$ASSETS/test.ogg" "$ASSETS/test-fr.ogg" "$ASSETS/test_it.ogg" "$WORK/game/Video/"
printf '1\n00:00:00,500 --> 00:00:07,000\nSous-titre\n' > "$WORK/game/Video/test-fr.srt"   # subtitles next to the .m2v too
run() { # $1 = name, $2 = timeline
    DIRK_TEST_TIMELINE="$2" SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 SDL_VIDEO_DRIVER=offscreen \
    SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE="$WORK/$1.raw" LD_PRELOAD="$SHIM" \
    timeout -s INT 12 "$DIRK" "$WORK/game/Game.dirk" > "$WORK/$1.log" 2>&1
}
# 1. first launch (builds the .mkv), switches at ~2.5 s and ~4.5 s, then an out-of-range request
run switch "2500,0,k,224,1,p1;2600,0,k,224,0,-;4500,0,k,224,1,p2;4600,0,k,224,0,-;5500,0,k,224,1,p3;5600,0,k,224,0,-;7000,0,h,0,0,END"
# 2. relaunch: the saved choice (IT) is used from the start, no rebuild
run relaunch "2500,0,h,0,0,END"
# 3. a new soundtrack appears: the cached .mkv is rebuilt with it
cp "$ASSETS/test-fr.ogg" "$WORK/game/Video/test-de.ogg"
cp "$ASSETS/test-fr.ogg" "$WORK/game/Video/test-xx.ogg"   # not an ISO 639 code -> "Unknown"
run added "2500,0,h,0,0,END"
cp "$WORK/game/Cfg/game.cfg" "$WORK/game/Cfg/game.cfg.after_added"
# 4. game.cfg as the previous Dirk wrote it (label "IT", 2026-09-27) with a
#    number that now points at German: the old label must still find Italian
sed -i -e 's/^dirk_AudioTrackName = .*/dirk_AudioTrackName = IT/' -e 's/^dirk_AudioTrack = .*/dirk_AudioTrack = 2/' "$WORK/game/Cfg/game.cfg"
run legacy "2500,0,h,0,0,END"
[ -n "${DIRK_TEST_KEEP:-}" ] && cp -r "$WORK" "$DIRK_TEST_KEEP"
"$PY" "$HERE/analyze.py" "$WORK"
