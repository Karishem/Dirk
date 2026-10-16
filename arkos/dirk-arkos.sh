#!/bin/bash
# Dirk launcher for ArkOS (and other handheld systems with a "Ports" menu):
#   dirk-arkos.sh <game.zdirk | game.dirk>
# Called by the per-game scripts that "Dirk - Update games list.sh" writes
# into the Ports folder. Everything Dirk needs is in this folder: the dirk
# program, lib/ (SDL3), and KimmyScript/ (copied from the PC's Dirk folder).
# The run's messages go to dirk.log, next to this script (readable from
# Windows on the EASYROMS card).

DIRK_HOME="$(cd "$(dirname "$0")" && pwd)"
LOG="$DIRK_HOME/dirk.log"

{
    echo "== $(date '+%Y-%m-%d %H:%M:%S') dirk $*"
    [ -d "$DIRK_HOME/KimmyScript" ] || echo "KimmyScript folder missing: copy it to $DIRK_HOME/KimmyScript"
} > "$LOG"

# The handheld's own controls: PortMaster knows each device's layout
# (gamecontrollerdb). With it, SDL sees a standard gamepad and the JOY_*
# names of controls.cfg apply; without it, the controls are raw RAWJOY_0.
for d in /opt/system/Tools/PortMaster /opt/tools/PortMaster "${XDG_DATA_HOME:-$HOME/.local/share}/PortMaster" \
         "$DIRK_HOME/../PortMaster" /roms/ports/PortMaster /roms2/ports/PortMaster \
         /storage/roms/ports/PortMaster /mnt/mmc/MUOS/PortMaster /mnt/sdcard/MUOS/PortMaster; do
    if [ -f "$d/control.txt" ]; then
        # shellcheck disable=SC1091
        source "$d/control.txt" >> "$LOG" 2>&1
        get_controls >> "$LOG" 2>&1
        [ -n "${sdl_controllerconfig:-}" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
        echo "controller layout from $d (${DEVICE_NAME:-device})" >> "$LOG"
        pm_found=1
        break
    fi
done
[ -n "${pm_found:-}" ] || echo "PortMaster not found (no control.txt): the console's buttons are a raw joystick" >> "$LOG"

export DIRK_HOME   # where KimmyScript/ is (see findSharedEngineRoot)
export DIRK_FORCE_FULLSCREEN=1          # no desktop around it
# SDL's own messages about the screen (formats tried, EGL errors) go to
# dirk.log too: these consoles can't be tested here, the log is the only
# view of what happened.
export SDL_LOGGING="${SDL_LOGGING:-video=debug}"
export LD_LIBRARY_PATH="$DIRK_HOME/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$DIRK_HOME" || exit 1

# The EASYROMS card is exFAT: a program on it may not be runnable directly
# ("permission denied"). The system's loader can always run it.
if "$DIRK_HOME/dirk" >/dev/null 2>&1 || [ $? -ne 126 ]; then
    exec "$DIRK_HOME/dirk" "$@" >> "$LOG" 2>&1
fi
for ld in /lib/ld-linux-aarch64.so.1 /lib64/ld-linux-x86-64.so.2; do
    [ -e "$ld" ] && exec "$ld" "$DIRK_HOME/dirk" "$@" >> "$LOG" 2>&1
done
echo "cannot run $DIRK_HOME/dirk" >> "$LOG"
exit 1
