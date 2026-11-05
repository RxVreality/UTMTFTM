#!/bin/sh
# Installs the packed Frame download into a throwaway home folder, the way a person would, and runs the installed program
# against a simulated SteamVR (tests/fake_steamvr.cpp). It shows the program starts on this CPU, finds everything it
# needs, draws, and moves a hand end to end through the real driver. It can't show how the real SteamVR behaves.
# Usage: tests/test_frame_run.sh <UTMTFTM-*-linux-arm64.tar.gz> <folder for pictures and logs>
set -u

TAR=$(readlink -f "$1")
mkdir -p "$2"
OUT=$(readlink -f "$2")
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
pass() { echo "ok    $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }
expect() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }

export HOME="$WORK/home"
RUNTIME="$WORK/steamvr"
export UTMTFTM_STEAMVR="$RUNTIME"
export FAKE_STEAMVR_LOG="$OUT/steamvr.log"
export FAKE_STEAMVR_SHOTS="$OUT"
unset XDG_CONFIG_HOME XDG_DATA_HOME
rm -f "$FAKE_STEAMVR_LOG"
APP="$HOME/.local/share/UTMTFTM/app"
DATA="$HOME/.local/share/UTMTFTM"

mkdir -p "$RUNTIME/bin/linuxarm64" "$HOME/.config/openvr" "$WORK/unpack"
cp "$ROOT/dist/fake_steamvr/vrclient.so" "$RUNTIME/bin/linuxarm64/"
cat > "$RUNTIME/bin/linuxarm64/vrpathreg" <<'EOF'
#!/bin/sh
echo "vrpathreg $*" >> "$FAKE_STEAMVR_LOG"
EOF
chmod +x "$RUNTIME/bin/linuxarm64/vrpathreg"
cat > "$HOME/.config/openvr/openvrpaths.vrpath" <<EOF
{"config":["$WORK/config"],"external_drivers":null,"jsonid":"vrpathreg","log":["$WORK/logs"],"runtime":["$RUNTIME"],"version":1}
EOF
mkdir -p "$WORK/config" "$WORK/logs"
tar xzf "$TAR" -C "$WORK/unpack"

# On a Frame this uses the headset's own OpenVR loader, which reads the file above, the way the installed program will.
# Anywhere else there's only the SDK's copy, which ignores that file (it looks in /data/work) but listens to these.
if [ ! -e /opt/steamvr/bin/linuxarm64/libopenvr_api.so ]; then
  export LD_LIBRARY_PATH="$ROOT/dist"
  export VR_OVERRIDE="$RUNTIME" VR_CONFIG_PATH="$WORK/config" VR_LOG_PATH="$WORK/logs"
fi

echo "--- installing"
sh "$WORK/unpack/UTMTFTM/install.sh" --no-restart
expect "install.sh finishes without an error" "[ $? -eq 0 ]"
expect "the program is installed and can run" "[ -x '$APP/utmtftm' ]"
expect "the driver is installed where SteamVR looks" "[ -x '$APP/driver/bin/linuxarm64/driver_utmtftm.so' ]"
expect "the driver's manifest is beside its bin folder" "[ -f '$APP/driver/driver.vrdrivermanifest' ]"
expect "the fonts are installed" "[ -f '$APP/fonts/Barlow-Regular.ttf' ] && [ -f '$APP/fonts/Barlow-SemiBold.ttf' ]"
expect "the + menu entry is installed" "grep -q 'Exec=$APP/open-utmtftm.sh' '$HOME/.local/share/applications/utmtftm.desktop'"
expect "no placeholder is left in the manifest" "! grep -q '@APP@' '$APP/utmtftm.vrmanifest'"
expect "the driver was added with vrpathreg" "grep -q 'vrpathreg adddriver $APP/driver' '$FAKE_STEAMVR_LOG'"
expect "the manifest was added to SteamVR" "grep -q 'AddApplicationManifest $APP/utmtftm.vrmanifest' '$FAKE_STEAMVR_LOG'"
expect "it was set to start with SteamVR" "grep -q 'SetApplicationAutoLaunch utmtftm.overlay 1' '$FAKE_STEAMVR_LOG'"

echo "--- the add-on switched off"
: > "$FAKE_STEAMVR_LOG"
FAKE_STEAMVR_SCENARIO=addon_off timeout 30 "$APP/utmtftm" 2>/dev/null
expect "it leaves by itself" "[ $? -eq 0 ]"
expect "it shows nothing in the dashboard" "! grep -q CreateDashboardOverlay '$FAKE_STEAMVR_LOG'"

echo "--- placing a hand"
: > "$FAKE_STEAMVR_LOG"
FAKE_STEAMVR_SCENARIO=place timeout 60 "$APP/utmtftm" 2>"$OUT/stderr-place.txt"
code=$?
cat "$FAKE_STEAMVR_LOG"
cp "$FAKE_STEAMVR_LOG" "$OUT/place.log"
expect "the program ran and quit cleanly (exit $code)" "[ $code -eq 0 ]"
expect "every check inside the run passed" "grep -q 'RESULT PASS' '$FAKE_STEAMVR_LOG'"
expect "it says the add-on is running" "grep -q 'driver add-on is running' '$DATA/utmtftm.log'"
expect "it says it closed" "grep -q 'UTMTFTM closed' '$DATA/utmtftm.log'"
expect "the driver took the hook" "grep -q 'IVRServerDriverHost_006: hooked' '$DATA/driver.log'"
expect "the offset was saved for the game" "grep -q 'steam.app.12345' '$DATA/profiles.json' && grep -q '\"override_global\" : true' '$DATA/profiles.json'"
cp "$DATA/utmtftm.log" "$OUT/utmtftm-place.log"
cp "$DATA/driver.log" "$OUT/driver-place.log"
cp "$DATA/profiles.json" "$OUT/profiles.json"

echo "--- controllers the hook can't see"
: > "$FAKE_STEAMVR_LOG"
FAKE_STEAMVR_SCENARIO=bypass timeout 60 "$APP/utmtftm" 2>/dev/null
code=$?
cat "$FAKE_STEAMVR_LOG"
expect "the program ran and quit cleanly (exit $code)" "[ $code -eq 0 ]"
expect "it noticed, and didn't claim to be live" "grep -q 'RESULT PASS' '$FAKE_STEAMVR_LOG'"

echo "--- updating over the top"
: > "$FAKE_STEAMVR_LOG"
sh "$WORK/unpack/UTMTFTM/install.sh" --no-restart >/dev/null
expect "installing again works" "[ $? -eq 0 ]"
expect "installing again keeps saved offsets" "grep -q 'steam.app.12345' '$DATA/profiles.json'"

echo "--- removing"
: > "$FAKE_STEAMVR_LOG"
sh "$APP/uninstall.sh" --no-restart
expect "uninstall.sh finishes without an error" "[ $? -eq 0 ]"
expect "the driver was removed with vrpathreg" "grep -q 'vrpathreg removedriver $APP/driver' '$FAKE_STEAMVR_LOG'"
expect "the manifest was removed from SteamVR" "grep -q 'RemoveApplicationManifest $APP/utmtftm.vrmanifest' '$FAKE_STEAMVR_LOG'"
expect "the program's gone" "[ ! -e '$APP' ]"
expect "the + menu entry's gone" "[ ! -e '$HOME/.local/share/applications/utmtftm.desktop' ]"
expect "saved offsets stay by default" "[ -f '$DATA/profiles.json' ]"

sh "$WORK/unpack/UTMTFTM/install.sh" --no-restart >/dev/null
sh "$APP/uninstall.sh" --no-restart --purge >/dev/null
expect "--purge removes the saved offsets too" "[ ! -e '$DATA' ]"

echo "--- pictures"
ls "$OUT"/*.png 2>/dev/null | wc -l

if [ "$fails" -ne 0 ]; then
  echo "FAILED: $fails"
  exit 1
fi
echo "PASSED"
