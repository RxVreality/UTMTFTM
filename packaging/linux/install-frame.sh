#!/bin/sh
# Downloads UTMTFTM @VERSION@ for the Steam Frame, checks the download and installs it. Needs no sudo.
#   curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
set -eu

VERSION=@VERSION@
BASE=https://github.com/RxVreality/UTMTFTM/releases/download/v$VERSION
FILE=UTMTFTM-$VERSION-linux-arm64.tar.gz

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"
curl -fsSLO "$BASE/$FILE"
curl -fsSLO "$BASE/$FILE.sha256"
sha256sum -c "$FILE.sha256" >/dev/null || {
  echo "The download doesn't match its checksum, so I'm not installing it. Try again." >&2
  exit 1
}
tar xzf "$FILE"
sh UTMTFTM/install.sh "$@"
