#!/usr/bin/env bash
#
# run_electron_side.sh - start one Electron for a parity capture, and stop it.
#
#   run_electron_side.sh start <app-dir> <url> <width> <height> [--port N]
#                              [--display :N] [--preload LIB] [--libdir DIR]
#                              [--no-sandbox]
#   run_electron_side.sh stop
#   run_electron_side.sh guest <domain> [--port N]
#   run_electron_side.sh guest-stop <domain>
#
# guest starts the Electron inside a libvirt guest, through the QEMU guest
# agent. The agent runs as the system account, so the browser lands in the
# services session, which has no visible desktop. That hides the window from
# whoever is at the guest's console, and nothing there can hover, focus or
# cover it. The capture must be the direct CDP route; there is no screen to
# photograph. The app is pushed from electron-app/ first, so the guest runs
# the same one, and a scheduled task restarts it at boot. DevTools binds to
# loopback, so a port proxy and a firewall rule expose it, on --port
# (default 9223).
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
            # kill only asks. A start that follows would find the port still
            # held and refuse, so wait for the process group to go.
            stopping="$(cat "$PIDFILE")"
            for _ in $(seq 1 100); do
                kill -0 "$stopping" 2>/dev/null || break
                sleep 0.1
            done
            rm -f "$PIDFILE"
        fi
        exit 0
        ;;
    start) shift ;;
    guest|guest-stop) COMMAND="$1"; GUEST_DOMAIN="${2:?guest needs a libvirt domain}"
        shift 2
        GUEST_PORT=9223
        # A second instance for census --mirror-b. One renderer stops scaling
        # past a few worker threads, so the extra guest cores are only reached
        # through another browser.
        MIRROR_PORT=9231
        while [ $# -gt 0 ]; do
            case "$1" in
                --port) GUEST_PORT="$2"; shift 2 ;;
                --mirror-port) MIRROR_PORT="$2"; shift 2 ;;
                *) echo "unknown option: $1" >&2; exit 2 ;;
            esac
        done
        ;;
    *) echo "usage: $0 start|stop|guest|guest-stop ..." >&2; exit 2 ;;
esac

# ---------------------------------------------------------------------------
# The guest side. Everything goes through the QEMU guest agent (vmexec.py).
# The guest layout is fixed: the Electron binary at C:\eparity\electron and
# the app at C:\eparity\app.
# ---------------------------------------------------------------------------

run_guest() {                             # run_guest <powershell text>
    printf '%s' "$1" | iconv -f UTF-8 -t UTF-16LE | base64 -w0 > "$STATE.ps1.b64"
    python3 "$HERE/vmexec.py" "$GUEST_DOMAIN" powershell -NoProfile \
            -EncodedCommand "$(cat "$STATE.ps1.b64")"
    local status=$?
    rm -f "$STATE.ps1.b64"
    return $status
}

if [ "${COMMAND:-}" = "guest-stop" ]; then
    run_guest "
Get-Process electron -ErrorAction SilentlyContinue | Stop-Process -Force
schtasks /delete /tn dwcel /f 2>&1 | Out-Null
schtasks /delete /tn dwcelb /f 2>&1 | Out-Null
netsh interface portproxy delete v4tov4 listenport=$GUEST_PORT listenaddress=0.0.0.0 | Out-Null
netsh interface portproxy delete v4tov4 listenport=$MIRROR_PORT listenaddress=0.0.0.0 | Out-Null
netsh advfirewall firewall delete rule name=dwc-devtools | Out-Null
netsh advfirewall firewall delete rule name=dwc-devtools-b | Out-Null
Write-Output 'guest stopped'
"
    exit $?
fi

if [ "${COMMAND:-}" = "guest" ]; then
    MAIN_B64="$(base64 -w0 "$HERE/electron-app/main.js")"
    PKG_B64="$(base64 -w0 "$HERE/electron-app/package.json")"
    run_guest "
Get-Process electron -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
New-Item -ItemType Directory -Force -Path C:\eparity\app | Out-Null
[IO.File]::WriteAllBytes('C:\eparity\app\main.js', [Convert]::FromBase64String('$MAIN_B64'))
[IO.File]::WriteAllBytes('C:\eparity\app\package.json', [Convert]::FromBase64String('$PKG_B64'))
Set-Content -Path C:\eparity\run.cmd -Value 'set DWC_URL=about:blank&& set DWC_W=1920&& set DWC_H=1080&& C:\eparity\electron\electron.exe --remote-debugging-port=9222 --disable-backgrounding-occluded-windows --disable-features=CalculateNativeWinOcclusion C:\eparity\app'
Set-Content -Path C:\eparity\run-b.cmd -Value 'set DWC_URL=about:blank&& set DWC_W=1920&& set DWC_H=1080&& C:\eparity\electron\electron.exe --remote-debugging-port=9230 --user-data-dir=C:\eparity\udb --disable-backgrounding-occluded-windows --disable-features=CalculateNativeWinOcclusion C:\eparity\app'
schtasks /create /tn dwcel /tr 'cmd.exe /c C:\eparity\run.cmd' /sc onstart /ru SYSTEM /rl highest /f | Out-Null
schtasks /create /tn dwcelb /tr 'cmd.exe /c C:\eparity\run-b.cmd' /sc onstart /ru SYSTEM /rl highest /f | Out-Null
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=$GUEST_PORT connectaddress=127.0.0.1 connectport=9222 | Out-Null
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=$MIRROR_PORT connectaddress=127.0.0.1 connectport=9230 | Out-Null
netsh advfirewall firewall add rule name=dwc-devtools dir=in action=allow protocol=TCP localport=$GUEST_PORT 2>&1 | Out-Null
netsh advfirewall firewall add rule name=dwc-devtools-b dir=in action=allow protocol=TCP localport=$MIRROR_PORT 2>&1 | Out-Null
schtasks /run /tn dwcel | Out-Null
schtasks /run /tn dwcelb | Out-Null
foreach (\$i in 1..60) {
    Start-Sleep -Milliseconds 500
    try {
        \$r = Invoke-WebRequest -UseBasicParsing http://127.0.0.1:9222/json/version -TimeoutSec 2
        if (\$r.StatusCode -eq 200) { Write-Output 'guest electron up on 9222'; exit 0 }
    } catch {}
}
Write-Output 'guest electron never opened its DevTools port'
exit 1
"
    exit $?
fi

APP="$1"; URL="$2"; WIDTH="$3"; HEIGHT="$4"; shift 4
PORT=9222
DISPLAY_NAME="${DISPLAY:-:99}"
PRELOAD=""
LIBDIR=""
BINARY="${DWC_ELECTRON_BIN:-}"
# An Electron unpacked from a release archive has no setuid chrome-sandbox,
# so its sandbox cannot start and the browser exits before DevTools opens.
SANDBOX=1

while [ $# -gt 0 ]; do
    case "$1" in
        --port)    PORT="$2"; shift 2 ;;
        --display) DISPLAY_NAME="$2"; shift 2 ;;
        --preload) PRELOAD="$2"; shift 2 ;;
        --libdir)  LIBDIR="$2"; shift 2 ;;
        --binary)  BINARY="$2"; shift 2 ;;
        --no-sandbox) SANDBOX=0; shift ;;
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
#
# LD_PRELOAD is stated either way. A shell that has the installed shim
# preloaded passes it to every browser it starts, so a run given no --preload
# is otherwise not the control it reads as: it measures whatever build is
# installed, silently, and only the mapped path in /proc says so.
env_args=(--unset=WAYLAND_DISPLAY)
if [ -z "$PRELOAD" ]; then
    env_args+=(--unset=LD_PRELOAD)
    if [ -n "${LD_PRELOAD:-}" ]; then
        echo "note: this shell preloads $LD_PRELOAD; starting without it, since no --preload was given" >&2
    fi
fi
env_args+=(DISPLAY="$DISPLAY_NAME" DWC_URL="$URL" DWC_W="$WIDTH" DWC_H="$HEIGHT")
[ -n "$LIBDIR" ]  && env_args+=(LD_LIBRARY_PATH="$LIBDIR")
[ -n "$PRELOAD" ] && env_args+=(LD_PRELOAD="$PRELOAD")

# Occlusion throttling stays off because a sharded sweep runs two instances
# on one screen, one window fully covering the other, and a covered window's
# requestAnimationFrame is otherwise throttled to a stop, which stalls every
# capture from it.
sandbox_args=()
[ "$SANDBOX" = 0 ] && sandbox_args+=(--no-sandbox)

# A browser still holding the port answers for the one about to start, which
# then fails to bind and is never reached. The pidfile and the mapped library
# both describe the new process, so nothing downstream catches it.
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
    echo "port $PORT is already in use; stop whatever holds it first" >&2
    exit 1
fi

setsid nohup env "${env_args[@]}" "$BINARY" \
    --ozone-platform=x11 --disable-backgrounding-occluded-windows \
    "${sandbox_args[@]}" \
    --remote-debugging-port="$PORT" "$APP" \
    > "$STATE/electron.log" 2>&1 < /dev/null &
echo $! > "$PIDFILE"

# Ready when DevTools answers, not after a fixed sleep.
for _ in $(seq 1 120); do
    if curl -s --max-time 2 "http://127.0.0.1:$PORT/json/version" > /dev/null 2>&1; then
        # setsid put the browser in its own process group led by the recorded
        # pid, so the listener has to be in that group to be the one started
        # here.
        started="$(cat "$PIDFILE")"
        listener="$(ss -ltnp 2>/dev/null | grep ":$PORT " |
                    grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)"
        if [ -n "$listener" ] &&
           [ "$(ps -o pgid= -p "$listener" 2>/dev/null | tr -d ' ')" != "$started" ]; then
            echo "port $PORT answers from pid $listener, not the browser just started" >&2
            exit 1
        fi
        echo "electron up on port $PORT (pid $started)"
        exit 0
    fi
    sleep 0.5
done
echo "electron never opened its DevTools port; see $STATE/electron.log" >&2
exit 1
