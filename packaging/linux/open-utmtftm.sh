#!/bin/sh
# What the + menu in the dashboard runs. Starts UTMTFTM detached from Steam, so Steam doesn't treat it as a running
# game, or just opens its dashboard tab if it's already running.
unset LD_PRELOAD
[ -n "${SYSTEM_LD_LIBRARY_PATH+x}" ] && export LD_LIBRARY_PATH="$SYSTEM_LD_LIBRARY_PATH"
setsid -f "$(dirname "$(readlink -f "$0")")/utmtftm" --open >/dev/null 2>&1 </dev/null
exit 0
