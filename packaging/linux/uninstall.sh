#!/bin/sh
# Removes UTMTFTM from this Steam Frame and restarts SteamVR, so the add-on is gone straight away.
# Your saved offsets stay, unless you add --purge. Add --no-restart to leave SteamVR running until you next restart it.
set -eu

DATA=$HOME/.local/share/UTMTFTM
APP=$DATA/app
STEAMVR=${UTMTFTM_STEAMVR:-/opt/steamvr}
PURGE=0
RESTART=1
for arg in "$@"; do
  case $arg in
    --purge) PURGE=1 ;;
    --no-restart) RESTART=0 ;;
    *)
      echo "I don't know $arg. It takes --purge and --no-restart." >&2
      exit 2
      ;;
  esac
done

export XDG_RUNTIME_DIR="/run/user/$(id -u)"

# If the program can't run for some reason, still take the driver out of SteamVR.
if ! "$APP/utmtftm" --uninstall 2>/dev/null; then
  "$STEAMVR/bin/linuxarm64/vrpathreg" removedriver "$APP/driver" >/dev/null 2>&1 || true
fi
pkill -x utmtftm 2>/dev/null || true
rm -f "$HOME/.local/share/applications/utmtftm.desktop"
rm -rf "$APP"
if [ "$PURGE" = 1 ]; then
  rm -rf "$DATA"
  echo "UTMTFTM and your saved offsets are gone."
else
  rm -f "$DATA/live.shm"
  echo "UTMTFTM is gone. Your saved offsets are still in $DATA, in case you come back. Add --purge to delete them too."
fi

if [ "$RESTART" = 1 ] && systemctl --user is-active --quiet steamvr.service; then
  echo "SteamVR restarts in 5 seconds to unload the add-on."
  sleep 5
  systemctl --user restart --no-block steamvr.service
fi
