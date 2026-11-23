#!/bin/sh
# smoke.sh — headless launch check. Proves the binary answers --version and
# then keeps running in its event loop rather than quitting or crashing.
#
#   usage: sh tests/smoke.sh ./aldrem
#
# Exit code from `timeout` is the assertion: 124 means the loop was still
# running when the timer fired (pass). Any other non-zero is a crash or an
# early failure, and exit 0 is the app quitting on its own (fail).
set -eu

BIN=${1:-./aldrem}
if [ ! -x "$BIN" ]; then
    echo "smoke: $BIN is not executable" >&2
    exit 1
fi
case "$BIN" in
    /*) ;;
    *) BIN="$PWD/$BIN" ;;
esac

TMPDIR_ROOT=$(mktemp -d)
trap 'rm -rf "$TMPDIR_ROOT"' EXIT

# The app autosaves the playlist and reads HOME for the browser and the save
# dialog; point both at the throwaway dir so a real queue is never touched.
HOME="$TMPDIR_ROOT"
XDG_CONFIG_HOME="$TMPDIR_ROOT/config"
export HOME XDG_CONFIG_HOME

SDL_VIDEODRIVER=dummy
SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER SDL_AUDIODRIVER

if ! "$BIN" --version | grep -q "^aldrem "; then
    echo "smoke: --version did not print 'aldrem <version>'" >&2
    exit 1
fi

set +e
timeout 5 "$BIN" </dev/null >/dev/null 2>&1
status=$?
set -e

if [ "$status" -eq 124 ]; then
    echo "smoke: app stayed running for 5s (ok)"
    exit 0
fi

if [ "$status" -eq 0 ]; then
    echo "smoke: app exited 0 on its own; it should block in its loop" >&2
else
    echo "smoke: app exited $status before the timeout (crash?)" >&2
fi
exit 1
