#!/bin/bash
# Dirk for Batocera: puts each game's frontend media (its Media/ folder:
# clear.png, 3D.png, snap.png, laserdisc.png, background.png, a video...)
# in /userdata/roms/dirk/gamelist.xml, so the theme shows them.
# Lives in /userdata/roms/ports: run it from the Ports list after adding
# games or media. It only fills media tags (see dirk_gamelist.py): names,
# favourites, play counts and images you chose yourself are kept.

ROMS=/userdata/roms/dirk
DIRK=/userdata/system/dirk
LOG=/userdata/system/logs/dirk-gamelist.log
LIST="$(mktemp 2>/dev/null || echo /tmp/dirk-gamelist.tsv)"
mkdir -p "$(dirname "$LOG")" 2>/dev/null
: > "$LOG"
say() { echo "$*"; echo "$*" >> "$LOG"; }
pretty() { printf '%s' "$1" | tr '_' ' '; }

count=0
for game in "$ROMS"/*/; do
    [ -d "$game" ] || continue
    game="${game%/}"; name="$(basename "$game")"
    target=""
    # Same choice as Dirk: <Game>.zdirk, else the only .zdirk, else the
    # .dirk scripts (one entry each: games with several versions).
    if [ -f "$game/$name.zdirk" ]; then target="$game/$name.zdirk"
    else
        set -- "$game"/*.zdirk; [ -f "$1" ] && [ $# -eq 1 ] && target="$1"
    fi
    if [ -n "$target" ]; then
        entries=("$target")
        for s in "$game"/*.dirk; do
            [ -f "$s" ] && [ "$(stat -c %s "$s")" -lt 4096 ] && grep -q '^[[:space:]]*zdirk[[:space:]]*=' "$s" && entries+=("$s")
        done
    else
        entries=(); for s in "$game"/*.dirk; do [ -f "$s" ] && entries+=("$s"); done
    fi
    [ "${#entries[@]}" -eq 0 ] && continue
    printf 'folder\t./%s\t%s\t%s\n' "$name" "$(pretty "$name")" "$game" >> "$LIST"
    for e in "${entries[@]}"; do
        label="$(basename "${e%.*}")"
        [ "${#entries[@]}" -eq 1 ] && label="$name"
        printf 'game\t./%s/%s\t%s\t%s\n' "$name" "$(basename "$e")" "$(pretty "$label")" "$game" >> "$LIST"
        say "  $(pretty "$label")"
        count=$((count + 1))
    done
done
say "Dirk: $count game(s) in $ROMS."

# EmulationStation reads roms/dirk/gamelist.xml if it exists, else the one
# in its own folder: write to whichever it reads.
GL="$ROMS/gamelist.xml"
[ ! -f "$GL" ] && [ -f /userdata/system/.emulationstation/gamelists/dirk/gamelist.xml ] && GL=/userdata/system/.emulationstation/gamelists/dirk/gamelist.xml
if command -v python3 >/dev/null 2>&1; then
    python3 "$DIRK/dirk_gamelist.py" "$GL" "$LIST" "$ROMS" 2>&1 | tee -a "$LOG"
else
    say "python3 not found: media not added."
fi
rm -f "$LIST"

# The Wizard creator, one entry in this Ports folder for each game it can
# write: a movie in Movie/ or Video/ and no .zdirk (the sheet of a .zdirk is
# sealed in the zip). It opens the game's sheet, or makes one when there is
# none yet. Only the entries this script wrote (marked) are replaced.
PORTS="$(cd "$(dirname "$0")" && pwd)"
MARK="# dirk-wizard""-entry (written by Dirk - Update games list)"
for f in "$PORTS"/"Dirk - "*.sh; do
    [ -f "$f" ] && grep -qF "$MARK" "$f" && rm -f "$f"
done
wcount=0
for game in "$ROMS"/*/; do
    [ -d "$game" ] || continue
    game="${game%/}"; name="$(basename "$game")"
    set -- "$game"/*.zdirk
    [ -f "$1" ] && continue
    [ -d "$game/Movie" ] || [ -d "$game/Video" ] || continue
    label="$(printf '%s' "$name" | tr '/:*?"<>|\\' '_________')"
    out="$PORTS/Dirk - $label (Wizard).sh"
    { echo "#!/bin/bash"; echo "$MARK"
      printf 'exec bash %q --wizard %q\n' "$DIRK/dirk-launch.sh" "$game"; } > "$out"
    chmod +x "$out" 2>/dev/null
    say "  $(pretty "$name") (Wizard)"
    wcount=$((wcount + 1))
done
say "Dirk: $wcount Wizard(s) in the Ports list."

# EmulationStation is waiting for this script to end: ask it to re-read
# the game lists a moment after (Batocera's local API), in the background.
if command -v curl >/dev/null 2>&1; then
    setsid nohup sh -c 'sleep 3; curl -s -m 5 http://127.0.0.1:1234/reloadgames' >/dev/null 2>&1 &
fi
exit 0
