#!/bin/bash
# .zdirk e2e (2026-09-28), see analyze.py. Real dirk, headless: game/ is
# zipped (with its top folder, like Explorer's "Compressed folder") into
# games/Zgame/Zgame.zdirk; next to it: Movie/clip.mp4 and Custom/.
# 1. launched as the .zdirk, 2. launched as the folder, 3. after the
# .zdirk is replaced by a new version.
# usage: run.sh <dirk> <input_shim.so> <video.mp4> <font.ttf> [python3]
set -u
DIRK=$1; SHIM=$2; VIDEO=$3; FONT=$4; PY=${5:-python3}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
G="$WORK/games/Zgame"
mkdir -p "$G/Movie" "$G/Custom" "$WORK/KimmyScript/Cfg" "$WORK/src"
cp -r "$HERE/game" "$WORK/src/Zgame"
mkdir -p "$WORK/src/Zgame/Fonts" "$WORK/src/Zgame/Sounds" "$WORK/src/Zgame/Overlay" "$WORK/tmp"
cp "$FONT" "$WORK/src/Zgame/Fonts/font.ttf"
cp "$VIDEO" "$G/Movie/clip.mp4"
printf 'CUSTOM' > "$G/Custom/override.txt"
printf 'x' > "$G/Blocker"   # a file where the script tries to write a folder
"$PY" - "$WORK/src" "$G/Zgame.zdirk" <<'PYEOF'
import sys, os, zlib, struct, zipfile
src, out = sys.argv[1], sys.argv[2]
def png(path, w, h):
    raw = b"".join(b"\0" + b"\xff\x00\x00\xff" * w for _ in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
png(os.path.join(src, "Zgame", "Overlay", "pic.png"), 3, 2)
png(os.path.join(src, "Zgame", "Overlay", "swap.png"), 5, 4)
import wave, math
w = wave.open(os.path.join(src, "Zgame", "Sounds", "beep.wav"), "wb")
w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050)
w.writeframes(b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * 660 * i / 22050))) for i in range(22050)))  # 1 s at 660 Hz
w.close()
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(os.path.join(src, "Zgame")):
        for f in files:
            full = os.path.join(root, f)
            z.write(full, os.path.relpath(full, src))
PYEOF
run() { # $1 = name, $2 = what to launch
    TMPDIR="$WORK/tmp" DIRK_TEST_TIMELINE="2500,0,h,0,0,END" SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 \
    SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE="$WORK/$1.raw" LD_PRELOAD="$SHIM" \
    timeout -s INT 12 "$DIRK" "$2" > "$WORK/$1.log" 2>&1
}
run first "$G/Zgame.zdirk"
run folder "$G"
sleep 1; touch "$G/Zgame.zdirk"   # "a new version of the game"
run updated "$G/Zgame.zdirk"
# 4. a shortcut Zalt.dirk next to the zip, pointing at its second game
printf -- '-- shortcut\nzdirk = "Zgame.zdirk"\n' > "$G/Zalt.dirk"
run shortcut "$G/Zalt.dirk"
rm "$G/Zalt.dirk"
[ -n "${DIRK_TEST_KEEP:-}" ] && cp -r "$WORK" "$DIRK_TEST_KEEP"
"$PY" "$HERE/analyze.py" "$WORK"
