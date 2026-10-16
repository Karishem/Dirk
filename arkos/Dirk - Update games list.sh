#!/bin/bash
# Dirk for ArkOS: (re)writes one script per Dirk game in this Ports folder,
# so each game appears in the Ports list. Run it from the Ports list after
# adding or removing games in roms/dirk (or roms2/dirk).
#   roms/dirk/<Game>/<Game>.zdirk (+ Movie/)   or   roms/dirk/<Game>/<Game>.dirk
# Every script it writes starts with "Dirk - " and is marked, so it never
# touches anything else in the Ports folder.
# Each game's frontend media (its Media/ folder: clear.png, 3D.png,
# snap.png, laserdisc.png, background.png, a video) is added to the Ports
# gamelist.xml for those entries (dirk/dirk_gamelist.py); other entries,
# names and images chosen in EmulationStation are kept.

PORTS="$(cd "$(dirname "$0")" && pwd)"
MARK="# dirk-game""-entry (written by Dirk - Update games list)"   # split: this file itself must not match
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
REPORT="$PORTS/dirk/last-update.txt"
say() { echo "$*"; echo "$*" >> "$REPORT"; [ -w /dev/tty1 ] && echo "$*" > /dev/tty1; }
: > "$REPORT"
LIST="$(mktemp 2>/dev/null || echo /tmp/dirk-gamelist.tsv)"; : > "$LIST"

# Old game scripts out (only the ones this tool wrote).
for f in "$PORTS"/"Dirk - "*.sh; do
    [ "$f" = "$SELF" ] && continue
    [ -f "$f" ] && grep -qF "$MARK" "$f" && rm -f "$f"
done

count=0
wcount=0
for root in /roms/dirk /roms2/dirk "$PORTS/../dirk"; do
    [ -d "$root" ] || continue
    root="$(cd "$root" && pwd)"
    case " $done_roots " in *" $root "*) continue;; esac
    done_roots="$done_roots $root"
    for game in "$root"/*/; do
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
            # Versions inside the .zdirk get a small shortcut .dirk next to it
            # (Dirk Downloader writes them): one more entry each.
            for s in "$game"/*.dirk; do
                [ -f "$s" ] && [ "$(stat -c %s "$s")" -lt 4096 ] && grep -q '^[[:space:]]*zdirk[[:space:]]*=' "$s" && entries+=("$s")
            done
        else
            entries=(); for s in "$game"/*.dirk; do [ -f "$s" ] && entries+=("$s"); done
        fi
        for e in "${entries[@]}"; do
            label="$(basename "${e%.*}")"
            [ "${#entries[@]}" -eq 1 ] && label="$name"
            label="$(printf '%s' "$label" | tr '/:*?"<>|\\' '_________')"
            out="$PORTS/Dirk - $label.sh"
            { echo "#!/bin/bash"; echo "$MARK"
              printf 'exec %q %q\n' "$PORTS/dirk/dirk-arkos.sh" "$e"; } > "$out"
            chmod +x "$out" 2>/dev/null
            printf 'game\t./%s\t%s\t%s\n' "Dirk - $label.sh" "Dirk - $(printf '%s' "$label" | tr '_' ' ')" "$game" >> "$LIST"
            say "  $label"
            count=$((count + 1))
        done
        # The Wizard creator, for a game it can write: a movie in Movie/ or
        # Video/ and no .zdirk (the sheet of a .zdirk is sealed in the zip).
        # It opens the game's sheet, or makes one when there is none yet.
        set -- "$game"/*.zdirk
        if [ ! -f "$1" ] && { [ -d "$game/Movie" ] || [ -d "$game/Video" ]; }; then
            label="$(printf '%s' "$name" | tr '/:*?"<>|\\' '_________')"
            out="$PORTS/Dirk - $label (Wizard).sh"
            { echo "#!/bin/bash"; echo "$MARK"
              printf 'exec %q --wizard %q\n' "$PORTS/dirk/dirk-arkos.sh" "$game"; } > "$out"
            chmod +x "$out" 2>/dev/null
            printf 'game\t./%s\t%s\t%s\n' "Dirk - $label (Wizard).sh" "Dirk - $(printf '%s' "$label" | tr '_' ' ') (Wizard)" "$game" >> "$LIST"
            say "  $label (Wizard)"
            wcount=$((wcount + 1))
        fi
    done
done
say "Dirk: $count game(s) and $wcount Wizard(s) in the Ports list."
[ "$count" -eq 0 ] && say "No game found: put them in roms/dirk/<Game>/ (on the EASYROMS card)."

# Media. EmulationStation reads ports/gamelist.xml if it exists, else the
# one in its home folder: write to whichever it reads.
GL="$PORTS/gamelist.xml"
if [ ! -f "$GL" ]; then
    for h in "$HOME" /home/ark; do
        [ -n "$h" ] && [ -f "$h/.emulationstation/gamelists/ports/gamelist.xml" ] && { GL="$h/.emulationstation/gamelists/ports/gamelist.xml"; break; }
    done
fi
if [ $((count + wcount)) -gt 0 ]; then
    if command -v python3 >/dev/null 2>&1; then
        python3 "$PORTS/dirk/dirk_gamelist.py" "$GL" "$LIST" "$PORTS" 2>&1 | while IFS= read -r l; do say "$l"; done
    else
        say "python3 not found: game media not added."
    fi
fi
rm -f "$LIST"

# Show the new list: restart EmulationStation when allowed, else say how.
sleep 2
if sudo -n true 2>/dev/null; then
    sudo -n systemctl restart emulationstation 2>/dev/null || say "Restart EmulationStation to see the list (Start > Quit > Restart)."
else
    say "Restart EmulationStation to see the list (Start > Quit > Restart)."
fi
