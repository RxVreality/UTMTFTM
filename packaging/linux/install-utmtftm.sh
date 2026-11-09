#!/bin/sh
# What FrameDrop's UTMTFTM entry runs when you press Play in the Steam library on the Frame. FrameDrop only copies this
# folder to the headset; this does the actual install, the same as the Konsole line. The first Play installs the add-on
# and restarts SteamVR. After that, Play just opens the yellow hand's page, until FrameDrop brings a newer version.
set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DATA=$HOME/.local/share/UTMTFTM
APP=$DATA/app
STAMP=$DATA/framedrop-installed
mkdir -p "$DATA"
LOG=$DATA/framedrop-install.log

# Steam starts titles with its overlay preloaded and its own library paths, which the system's tools complain about.
unset LD_PRELOAD
if [ -n "${SYSTEM_LD_LIBRARY_PATH+x}" ]; then export LD_LIBRARY_PATH="$SYSTEM_LD_LIBRARY_PATH"; else unset LD_LIBRARY_PATH; fi

# If Steam put this in a runtime container, SteamVR's tools aren't visible, so start again outside it.
if [ ! -x /opt/steamvr/bin/linuxarm64/vrpathreg ] && [ -z "${UTMTFTM_ON_HOST:-}" ] && command -v flatpak-spawn >/dev/null 2>&1; then
  exec flatpak-spawn --host env UTMTFTM_ON_HOST=1 sh "$HERE/install-utmtftm.sh" "$@"
fi

export XDG_RUNTIME_DIR="/run/user/$(id -u)"

TAR=$(ls "$HERE"/UTMTFTM-*-linux-arm64.tar.gz 2>/dev/null | head -n 1)
[ -n "$TAR" ] || { echo "$(date) The UTMTFTM download is missing from $HERE" >> "$LOG"; exit 1; }
SUM=$(sha256sum "$TAR" | cut -d' ' -f1)

# Already installed from this same download: open the page instead of installing and restarting SteamVR again.
if [ "${1:-}" != --reinstall ] && [ -x "$APP/open-utmtftm.sh" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$SUM" ]; then
  exec sh "$APP/open-utmtftm.sh"
fi

if [ "${1:-}" != --now ]; then
  # Run the install as its own background job: Steam ends a title's processes when it stops, and the SteamVR restart
  # at the end closes everything, Steam's view of this title included.
  if command -v systemd-run >/dev/null 2>&1 &&
    systemd-run --user --collect --quiet --unit="utmtftm-install-$(date +%s)" sh "$HERE/install-utmtftm.sh" --now; then
    exit 0
  fi
fi

{
  echo "$(date) Installing $(basename "$TAR")"
  if [ -f "$TAR.sha256" ] && ! (cd "$HERE" && sha256sum -c "$(basename "$TAR").sha256" >/dev/null); then
    echo "The download doesn't match its checksum, so I'm not installing it. Install it again from FrameDrop."
    exit 1
  fi
  WORK=$(mktemp -d)
  trap 'rm -rf "$WORK"' EXIT
  tar xzf "$TAR" -C "$WORK"
  # install.sh restarts SteamVR as its last step, so the stamp goes down first.
  echo "$SUM" > "$STAMP"
  sh "$WORK/UTMTFTM/install.sh" || { rm -f "$STAMP"; exit 1; }
} >> "$LOG" 2>&1
