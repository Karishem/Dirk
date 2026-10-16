#!/bin/bash
# Dirk launcher for Batocera, called by EmulationStation (see
# es_systems_dirk.cfg): dirk-launch.sh <game.zdirk | game.dirk>
#
# Everything Dirk needs is in this folder: the dirk program, its lib/
# (SDL3), and KimmyScript/ (copied from the PC's Dirk folder).
# The run's messages go to /userdata/system/logs/dirk.log.

DIRK_HOME="$(cd "$(dirname "$0")" && pwd)"
LOG_DIR=/userdata/system/logs
mkdir -p "$LOG_DIR" 2>/dev/null || LOG_DIR=/tmp
LOG="$LOG_DIR/dirk.log"

# Files copied over the network share may lose their "executable" mark.
chmod +x "$DIRK_HOME/dirk" 2>/dev/null

{
    echo "== $(date '+%Y-%m-%d %H:%M:%S') dirk $*"
    if [ ! -d "$DIRK_HOME/KimmyScript" ]; then
        echo "KimmyScript folder missing: copy it to $DIRK_HOME/KimmyScript"
    fi
} > "$LOG"

cd "$DIRK_HOME" || exit 1
# Always fullscreen here (no desktop around it), whatever a game copied
# from a Windows PC had saved.
export DIRK_HOME   # where KimmyScript/ is (see findSharedEngineRoot)
export DIRK_FORCE_FULLSCREEN=1
# lib/ also holds libraries SDL loads on demand (PNG, Ogg Vorbis).
export LD_LIBRARY_PATH="$DIRK_HOME/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$DIRK_HOME/dirk" "$@" >> "$LOG" 2>&1
