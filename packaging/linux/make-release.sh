#!/bin/sh
# Packs dist/ into the Steam Frame download, and refuses if the program would need anything SteamOS doesn't have.
# Run build.sh first. Usage: packaging/linux/make-release.sh 0.2.0
set -eu

VERSION=$1
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
DIST=$ROOT/dist
OUT=$ROOT/build/release-frame
STAGE=$OUT/UTMTFTM
NAME=UTMTFTM-$VERSION-linux-arm64.tar.gz

rm -rf "$OUT"
mkdir -p "$STAGE/driver/bin/linuxarm64" "$STAGE/driver/resources/settings"

cp "$DIST/utmtftm" "$DIST/libopenvr_api.so" "$DIST/utmtftm.png" "$STAGE/"
cp "$DIST/driver/bin/linuxarm64/driver_utmtftm.so" "$STAGE/driver/bin/linuxarm64/"
cp "$DIST/driver/driver.vrdrivermanifest" "$STAGE/driver/"
cp "$DIST/driver/resources/settings/default.vrsettings" "$STAGE/driver/resources/settings/"
cp -R "$DIST/input" "$DIST/fonts" "$STAGE/"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/"
cp "$HERE/install.sh" "$HERE/uninstall.sh" "$HERE/open-utmtftm.sh" "$HERE/utmtftm.vrmanifest.in" "$HERE/utmtftm.desktop.in" "$STAGE/"
chmod 755 "$STAGE/install.sh" "$STAGE/uninstall.sh" "$STAGE/open-utmtftm.sh"
strip --strip-unneeded "$STAGE/utmtftm" "$STAGE/driver/bin/linuxarm64/driver_utmtftm.so"

# Only what's known to be on the headset. Frame Mic Tuner and Frame Perf Overlay need the same three libraries.
for f in "$STAGE/utmtftm" "$STAGE/driver/bin/linuxarm64/driver_utmtftm.so"; do
  file "$f" | grep -q 'ARM aarch64' || { echo "$f isn't an ARM64 program" >&2; exit 1; }
  needed=$(readelf -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
  for lib in $needed; do
    case $lib in
      libc.so.6 | libm.so.6 | libdl.so.2 | libpthread.so.0 | ld-linux-aarch64.so.1) ;;
      libopenvr_api.so | libcairo.so.2 | libfreetype.so.6 | libvulkan.so.1) ;;
      *)
        echo "$f needs $lib, which the Frame may not have" >&2
        exit 1
        ;;
    esac
  done
  echo "$(basename "$f") needs: $(echo $needed)"
  echo "$(basename "$f") newest glibc: $(objdump -T "$f" | sed -n 's/.*(GLIBC_\([0-9.]*\)).*/\1/p' | sort -Vu | tail -1)"
done

echo "libopenvr_api.so needs: $(readelf -d "$STAGE/libopenvr_api.so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"

tar --owner=0 --group=0 -C "$OUT" -czf "$OUT/$NAME" UTMTFTM
sed "s/@VERSION@/$VERSION/g" "$HERE/install-frame.sh" > "$OUT/install-frame.sh"
(cd "$OUT" && sha256sum "$NAME" > "$NAME.sha256")
ls -l "$OUT"
