#!/usr/bin/env bash
#
# run_electron_side.sh - start one Electron for a parity capture, and stop it.
#
#   run_electron_side.sh start <app-dir> <url> <width> <height> [--port N]
#                              [--display :N] [--preload LIB] [--libdir DIR]
#   run_electron_side.sh stop
#
# Started detached with a pidfile, because the process has to outlive the
# shell that launched it and pattern-matching for it does not work, since a
# pgrep for the binary path also matches the very command line that starts
# it.
#
# electron-app beside this script is the app to point it at, and the same one
# belongs on the other machine.
#
# The window is sized by the app itself, in electron-app/main.js. Electron does
# not implement Chromium's Browser domain, so the DevTools side verifies the
# size it was given and does not set it.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
STATE="${DWC_ELECTRON_STATE:-/var/tmp/electron-parity}"
PIDFILE="$STATE/electron.pid"

case "${1:-}" in
    stop)
        if [ -f "$PIDFILE" ]; then
            # The whole session, since Electron forks helper processes.
            kill -- "-$(cat "$PIDFILE")" 2>/dev/null || kill "$(cat "$PIDFILE")" 2>/dev/null
            rm -f "$PIDFILE"
        fi
        exit 0
        ;;
    start) shift ;;
    *) echo "usage: $0 start <app-dir> <url> <width> <height> [options]" >&2; exit 2 ;;
esac

APP="$1"; URL="$2"; WIDTH="$3"; HEIGHT="$4"; shift 4
PORT=9222
DISPLAY_NAME="${DISPLAY:-:99}"
PRELOAD=""
LIBDIR=""
BINARY="${DWC_ELECTRON_BIN:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --port)    PORT="$2"; shift 2 ;;
        --display) DISPLAY_NAME="$2"; shift 2 ;;
        --preload) PRELOAD="$2"; shift 2 ;;
        --libdir)  LIBDIR="$2"; shift 2 ;;
        --binary)  BINARY="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# There is no agreed place a system Electron lives, so there is nothing to
# default to. $PATH first, then the versioned install directories, and
# otherwise say what to set rather than failing later with a confusing error.
if [ -z "$BINARY" ]; then
    BINARY="$(command -v electron 2>/dev/null || true)"
fi
if [ -z "$BINARY" ]; then
    for candidate in /usr/lib/electron*/electron /usr/lib64/electron*/electron; do
        [ -x "$candidate" ] && BINARY="$candidate"
    done
fi
if [ -z "$BINARY" ] || [ ! -x "$BINARY" ]; then
    echo "no Electron found; pass --binary <path> or set DWC_ELECTRON_BIN" >&2
    exit 2
fi

mkdir -p "$STATE"
"$0" stop

# Only what the capture needs is set here, and no rendering switches. The
# two sides are supposed to run the defaults, so that what differs between
# them is the platform.
#
# WAYLAND_DISPLAY has to go, and the platform has to be named. Ozone prefers
# Wayland when the session offers it, so an Electron started from a desktop
# session ignores DISPLAY entirely, opens its window on the real desktop, and
# leaves the Xvfb screen empty, which reads downstream as "no window manager
# mapped the window". Unsetting it is not enough on its own, so the platform
# is stated outright.
# The --unset comes first because env takes its options before any
# assignments; after one, it would be read as the command to run.
env_args=(--unset=WAYLAND_DISPLAY
          DISPLAY="$DISPLAY_NAME" DWC_URL="$URL" DWC_W="$WIDTH" DWC_H="$HEIGHT")
[ -n "$LIBDIR" ]  && env_args+=(LD_LIBRARY_PATH="$LIBDIR")
[ -n "$PRELOAD" ] && env_args+=(LD_PRELOAD="$PRELOAD" CHROMIUM_PATCH_DWRITE=1)

# Occlusion throttling stays off because a sharded sweep runs two instances
# on one screen, one window fully covering the other, and a covered window's
# requestAnimationFrame is otherwise throttled to a stop, which stalls every
# capture from it.
setsid nohup env "${env_args[@]}" "$BINARY" \
    --ozone-platform=x11 --disable-backgrounding-occluded-windows \
    --remote-debugging-port="$PORT" "$APP" \
    > "$STATE/electron.log" 2>&1 < /dev/null &
echo $! > "$PIDFILE"

# Ready when DevTools answers, not after a fixed sleep.
for _ in $(seq 1 120); do
    if curl -s --max-time 2 "http://127.0.0.1:$PORT/json/version" > /dev/null 2>&1; then
        echo "electron up on port $PORT (pid $(cat "$PIDFILE"))"
        exit 0
    fi
    sleep 0.5
done
echo "electron never opened its DevTools port; see $STATE/electron.log" >&2
exit 1
