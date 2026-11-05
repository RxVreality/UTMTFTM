#!/bin/sh
# Installs UTMTFTM on a Steam Frame, in your home folder. No sudo. Run it from the unpacked folder.
# It finishes by restarting SteamVR, which is what loads the add-on, unless you add --no-restart.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP=$HOME/.local/share/UTMTFTM/app
LAUNCHER=$HOME/.local/share/applications/utmtftm.desktop
STEAMVR=${UTMTFTM_STEAMVR:-/opt/steamvr}
RESTART=1
[ "${1:-}" = --no-restart ] && RESTART=0

die() {
  echo "$1" >&2
  exit 1
}

[ "$(id -u)" -ne 0 ] || die "Run this as your normal user, without sudo."
[ "$(uname -m)" = aarch64 ] || die "This is the Steam Frame build (ARM64), and this machine is $(uname -m)."
[ -x "$STEAMVR/bin/linuxarm64/vrpathreg" ] || die "SteamVR wasn't found in $STEAMVR. Is this a Steam Frame?"

# Desktop Mode's terminal points this at a nested folder where systemctl --user can't find systemd.
export XDG_RUNTIME_DIR="/run/user/$(id -u)"

# Save and close a running copy before replacing it.
pkill -x utmtftm 2>/dev/null || true
tries=0
while pgrep -x utmtftm >/dev/null 2>&1 && [ "$tries" -lt 30 ]; do
  sleep 0.1
  tries=$((tries + 1))
done

mkdir -p "$APP/driver/bin/linuxarm64" "$APP/driver/resources/settings" "$(dirname "$LAUNCHER")"

# New files go in next to the old ones and get renamed over them, because SteamVR may have the old driver loaded.
put() {
  cp "$HERE/$1" "$APP/$1.new"
  chmod "$2" "$APP/$1.new"
  mv -f "$APP/$1.new" "$APP/$1"
}
put utmtftm 755
put libopenvr_api.so 755
put driver/bin/linuxarm64/driver_utmtftm.so 755
put driver/driver.vrdrivermanifest 644
put driver/resources/settings/default.vrsettings 644
put open-utmtftm.sh 755
put uninstall.sh 755
put utmtftm.png 644
put LICENSE 644
put THIRD_PARTY_NOTICES.md 644
rm -rf "$APP/input" "$APP/fonts"
cp -R "$HERE/input" "$HERE/fonts" "$APP/"

# Both of these name the install folder in full: SteamVR and Steam start the program from somewhere else.
sed "s|@APP@|$APP|g" "$HERE/utmtftm.vrmanifest.in" > "$APP/utmtftm.vrmanifest"
sed "s|@APP@|$APP|g" "$HERE/utmtftm.desktop.in" > "$LAUNCHER.new"
chmod 644 "$LAUNCHER.new"
mv -f "$LAUNCHER.new" "$LAUNCHER"

set +e
"$APP/utmtftm" --install 2>/dev/null
code=$?
set -e
case $code in
  0) ;;
  2) die "The add-on is in, but SteamVR couldn't be reached. Run this again with SteamVR running." ;;
  3) die "SteamVR's vrpathreg tool wouldn't add the driver. See ~/.local/share/UTMTFTM/utmtftm.log" ;;
  *) die "Something failed, code $code. See ~/.local/share/UTMTFTM/utmtftm.log" ;;
esac

echo "UTMTFTM is installed in $APP"
if [ "$RESTART" = 1 ] && systemctl --user is-active --quiet steamvr.service; then
  echo "SteamVR restarts in 5 seconds so it can load the add-on. Everything goes dark for a moment and this window closes."
  echo "After that, open the SteamVR dashboard and look for the yellow hand."
  sleep 5
  systemctl --user restart --no-block steamvr.service
else
  echo "Restart SteamVR or the headset to load the add-on, then open the dashboard and look for the yellow hand."
fi
