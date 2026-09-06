#!/usr/bin/env bash
#
# run_parity_firefox.sh - start a Firefox configured for Windows parity.
#
#   run_parity_firefox.sh start [options]          # here, under Xvfb
#   run_parity_firefox.sh stop [--port N]
#   run_parity_firefox.sh guest <domain> [options] # in a libvirt guest
#   run_parity_firefox.sh guest-stop <domain>
#
# Options:
#   --port N        Marionette port                     (default 2828). Each
#                   port keeps its own state directory, so two browsers on two
#                   ports run side by side and stop names one of them.
#                   `guest` takes a comma-separated list and starts one browser
#                   per port, each with its own profile, capture server and
#                   scheduled tasks; compare_pages.sh deals a side's cells out
#                   across the ports it names. Ports must be two apart, since
#                   each takes PORT+1 for its capture server.
#   --display :N    X display to create                 (default :99)
#   --size WxH      X screen size                       (default 2560x1440)
#   --shim PATH     the interceptor to preload          (default: the build)
#   --no-shim       run without LD_PRELOAD, the control run
#   --no-prefs      start on a stock profile, without the parity prefs. Use
#                   this when *measuring* a Windows Firefox: a browser already
#                   carrying the parity prefs answers with what those prefs
#                   say, which is circular if the answer is what they should
#                   say in the first place.
#   --scale S       device pixels per CSS pixel          (default: the
#                   display's own). Both sides must be given the same S: it is
#                   a Gecko pref on either platform, so the two reach the scale
#                   through the same code and a comparison at it is a
#                   comparison of one variable.
#   --scheme NAME   light or dark                        (default light). What
#                   a page's prefers-color-scheme reads, on both sides alike.
#                   The text color decides the gamma a mask is corrected with,
#                   so dark is a rasterizer decision of its own and not the
#                   light run inverted.
#   --url URL       first page to open                  (default about:blank)
#   --profile DIR   keep the profile here               (default: a temp dir)
#   --capture-port N  where the guest's capture server listens (default PORT+1)
#   --guest-firefox PATH  the guest's firefox.exe, when it should not be the
#                   one in Program Files. The measurement reference depends on
#                   this: a build whose xul.dll is patched to load dwcore.dll
#                   instead of dwrite.dll runs on DWriteCore, and the stock
#                   install runs on the system DirectWrite. Also DWC_GUEST_FIREFOX.
#   --rdp           run the guest's browser in its own RDP session, so the
#                   console session stays exactly as its user left it. Needs a
#                   FreeRDP client here and two lines in ~/.dwc-guest-rdp, the
#                   account name and its password.
#   --hide-console  disconnect the guest's console session once the capture
#                   server is up. It keeps compositing, so captures stay real,
#                   but the guest sits at its logon screen until guest-stop
#                   puts the session back. Off by default: that screen belongs
#                   to whoever is using the guest.
#
# The guest can also be driven with nothing on its console. Set STAGE_DIR and
# STAGE_URL  and the guest block builds wincap.exe, starts it in the browser's
# session, and disconnects that session. The desktop falls back to the logon
# screen and keeps compositing, so captures taken through `guest:<ip>:<capture-port>`
# are real paints of a window nobody is looking at. Without those two variables
# nothing changes and captures come off the console framebuffer as before.
#
# Both settings this starts Firefox with are load-bearing, and each was found
# by a comparison that failed without it:
#
#   prefs             supplied by the shim through MOZ_DEFAULT_PREFS - the
#                     rendering prefs, and ui.font.* for the UI font that
#                     `system-ui` resolves to.
#
# And two that are about the X server and not about fonts: without a window
# manager Firefox never maps its window and WebDriver:SetWindowRect is ignored,
# and without GDK_BACKEND=x11 a browser started from inside a Wayland session
# connects to that session instead of the display being photographed.
#
# The browser is *windowed*, never headless. A headless Firefox
# substitutes HeadlessLookAndFeel, which answers "sans-serif" for every system
# font, so system-ui cannot be measured at all that way.
set -u

# Xvfb, minwm and the Marionette helpers must run without it; the browser at
# the bottom is the only thing that sets it again.
unset LD_PRELOAD

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
STATE_BASE="${TMPDIR:-/tmp}/dwc-parity-firefox.$(id -u)"

usage() { sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

find_firefox() {
    if [ -n "${FIREFOX:-}" ]; then echo "$FIREFOX"; return; fi
    for candidate in firefox firefox-esr firefox-bin; do
        if command -v "$candidate" >/dev/null 2>&1; then
            command -v "$candidate"; return
        fi
    done
    echo ""
}

find_shim() {
    for candidate in "$REPO"/build/libcleartype.so \
                     "$REPO"/build/cleartype/libcleartype.so; do
        [ -f "$candidate" ] && { echo "$candidate"; return; }
    done
    echo ""
}

# Builds the font list before anything is measured.
#
# Firefox assembles the platform font list, and resolves fallback, partly off
# the main thread, so the first page a fresh browser loads can be laid out
# against an incomplete list and reflow by a few pixels when the rest arrives.
# It happens only on the first load, and nothing inside the content process can
# see that work finish, so it cannot be waited for from there.
#
# Paying it here instead costs a second and a half at startup and nothing after
# that. The warm-up page names every generic and several scripts, so the list,
# the per-language prefs and the fallback path are all exercised before the
# first real measurement.
warm_up() {                                # warm_up <host> <port>
    python3 - "$1" "$2" <<'PYWARM'
import sys, time
sys.path.insert(0, __import__("os").environ["DWC_TESTING_DIR"])
from marionette import Marionette

PAGE = ("data:text/html;charset=utf-8,<meta charset=utf-8>"
        "<div style='font-family:serif'>Handgloves</div>"
        "<div style='font-family:sans-serif'>Handgloves</div>"
        "<div style='font-family:monospace'>Handgloves</div>"
        "<div style='font-family:cursive'>Handgloves</div>"
        "<div style='font-family:fantasy'>Handgloves</div>"
        "<div style='font-family:system-ui'>Handgloves</div>"
        "<div>%D0%B4%D0%B0 %CE%B1%CE%B2 %D7%90%D7%91 %D8%A7%D8%A8 "
        "%E3%81%82%E3%81%84 %ED%95%9C%EA%B8%80 %E4%B8%AD%E6%96%87 "
        "%E0%A4%95%E0%A4%96 %E0%B8%81%E0%B8%82</div>")

try:
    m = Marionette(sys.argv[1], int(sys.argv[2]), timeout=60)
    m.start("content")
    m.call("WebDriver:Navigate", {"url": PAGE})
    # Hold until the layout has been unchanged for a stretch far longer than
    # the reflow this is here to absorb. A ceiling, not a duration: it exits as
    # soon as the page has been quiet for 0.6s.
    last, quiet, end = None, 0.0, time.time() + 20
    while time.time() < end:
        shape = m.script("return document.documentElement.scrollHeight + 'x' +"
                         " document.body.getBoundingClientRect().height;")
        now = time.time()
        quiet = 0.0 if shape != last else quiet + 0.05
        last = shape
        if quiet >= 0.6:
            break
        time.sleep(0.05)
    m.close()
except Exception as exc:                    # a warm-up is never fatal
    print("warm-up skipped: %s" % exc, file=sys.stderr)

# Firefox draws a status panel reading "Looking up <host>..." over the bottom
# left of the content area while a request is in flight, and it lingers after
# the load. It is chrome, so the marker handshake cannot see it and a capture is
# accepted with the panel in it, which reads as a font difference wherever the
# page has ink in those rows. Only the side whose page server is not loopback
# shows it, and any subresource brings it back, so disabling favicons covers
# only a page that fetches nothing else and a webfont undoes even that.
#
# StatusPanel.update() returns early on _frozen, so setting it silences the
# panel for the session. No pref does this, and the alternative that does,
# --kiosk, takes the window fullscreen and changes the viewport with it.
try:
    m = Marionette(sys.argv[1], int(sys.argv[2]), timeout=60)
    m.start("chrome")
    m.script("const w = Services.wm.getMostRecentWindow('navigator:browser');"
             " if (w && w.StatusPanel) { w.StatusPanel._frozen = true; }"
             " return 1;")
    m.close()
except Exception as exc:
    print("status panel not frozen: %s" % exc, file=sys.stderr)
PYWARM
}

stop_all() {
    if [ -f "$STATE/firefox.pid" ]; then
        # Only ever this browser, by pid. Never pkill firefox: the user's own
        # session is very likely running one.
        kill "$(cat "$STATE/firefox.pid")" 2>/dev/null
    fi
    for name in minwm xvfb; do
        [ -f "$STATE/$name.pid" ] && kill "$(cat "$STATE/$name.pid")" 2>/dev/null
    done
    sleep 1
    rm -rf "$STATE"
    echo "stopped"
}

[ $# -ge 1 ] || usage
COMMAND="$1"; shift

GUEST_DOMAIN=""
case "$COMMAND" in
    stop|start) ;;
    guest|guest-stop)
        [ $# -ge 1 ] || usage
        GUEST_DOMAIN="$1"; shift ;;
    *) usage ;;
esac

PORT=2828
SCALE=""
SCHEME="light"
DISPLAY_NAME=":99"
SIZE="2560x1440"
SHIM="$(find_shim)"
URL="about:blank"
PROFILE=""
USE_PREFS=1
CAPTURE_PORT=""
HIDE_CONSOLE=0
RDP_SESSION=0
GUEST_FIREFOX="${DWC_GUEST_FIREFOX:-}"
RDP_CREDS="${DWC_RDP_CREDS:-$HOME/.dwc-guest-rdp}"
RDP_DISPLAY="${DWC_RDP_DISPLAY:-:98}"
STAGE_DIR="${STAGE_DIR:-}"
STAGE_URL="${STAGE_URL:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --port)    PORT="$2"; shift 2 ;;
        --display) DISPLAY_NAME="$2"; shift 2 ;;
        --size)    SIZE="$2"; shift 2 ;;
        --shim)    SHIM="$2"; shift 2 ;;
        --no-shim) SHIM=""; shift ;;
        --no-prefs) USE_PREFS=0; export CLEARTYPE_PREFS=0; shift ;;
        --scale)   SCALE="$2"; shift 2 ;;
        --scheme)  SCHEME="$2"; shift 2 ;;
        --url)     URL="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --capture-port) CAPTURE_PORT="$2"; shift 2 ;;
        --hide-console) HIDE_CONSOLE=1; shift ;;
        --rdp)     RDP_SESSION=1; shift ;;
        --guest-firefox) GUEST_FIREFOX="$2"; shift 2 ;;
        *) usage ;;
    esac
done

# The guest takes a list; everything else takes the first of it.
GUEST_PORTS="$(printf '%s' "$PORT" | tr ',' ' ')"
case "$SCHEME" in
    light) SCHEME_PREF=1 ;;
    dark)  SCHEME_PREF=0 ;;
    *) echo "--scheme takes light or dark, not $SCHEME" >&2; exit 2 ;;
esac
if [ -n "$SCALE" ]; then
    case "$SCALE" in
        ''|*[!0-9.]*|*.*.*) echo "--scale takes a number, not $SCALE" >&2; exit 2 ;;
    esac
fi

PORT="${GUEST_PORTS%% *}"
[ -n "$CAPTURE_PORT" ] || CAPTURE_PORT=$((PORT + 1))

# One state directory per Marionette port. The pid files live here, so sharing
# one directory means a second browser's start kills the first one.
STATE="$STATE_BASE.$PORT"

if [ "$COMMAND" = stop ]; then
    stop_all
    exit 0
fi

# Who the guest's scheduled tasks run as. The INTERACTIVE group means whoever
# is logged on, which is the console user; naming the harness account instead
# is what puts the browser in the RDP session and nowhere else.
RDP_USER=""
GUEST_PRINCIPAL="New-ScheduledTaskPrincipal -GroupId 'INTERACTIVE' -RunLevel Limited"
if [ "$RDP_SESSION" = 1 ]; then
    if [ ! -r "$RDP_CREDS" ]; then
        echo "no guest credentials at $RDP_CREDS: line 1 the account, line 2 its password" >&2
        exit 1
    fi
    RDP_USER="$(sed -n 1p "$RDP_CREDS")"
    GUEST_PRINCIPAL="New-ScheduledTaskPrincipal -UserId '$RDP_USER' -LogonType Interactive -RunLevel Limited"
fi

# ---------------------------------------------------------------------------
# The guest side
#
# Everything here goes through the QEMU guest agent (vmexec.py), which runs in
# session 0 and therefore cannot put a window on screen. So the browser is
# started by a scheduled task whose principal is the INTERACTIVE group: that is
# what lands it in the logged-on user's session, which is the session that gets
# photographed and the only one where the browser is not headless.
#
# Marionette binds to loopback, so it also needs a port proxy and a firewall
# hole before anything outside the guest can drive it.
# ---------------------------------------------------------------------------

run_guest() {                             # run_guest <powershell text>
    printf '%s' "$1" | iconv -f UTF-8 -t UTF-16LE | base64 -w0 > "$STATE.ps1.b64"
    python3 "$HERE/vmexec.py" "$GUEST_DOMAIN" powershell -NoProfile \
            -EncodedCommand "$(cat "$STATE.ps1.b64")"
    local status=$?
    rm -f "$STATE.ps1.b64"
    return $status
}

# A session of the harness's own, so the guest's console session is left
# alone. An RDP logon gets a composited desktop the same way the console does,
# and it is a different session, so nothing the browser draws reaches whoever
# is using the guest and nothing here has to disconnect them.
#
# The client draws into an Xvfb nobody reads. It exists because a *connected*
# session is the one Windows keeps composing; the pixels this end receives are
# thrown away, and the ones that matter are read inside the guest by wincap.
start_rdp_session() {                     # start_rdp_session <guest ip>
    local ip="$1" client pass
    client="$(command -v xfreerdp3 || command -v xfreerdp || true)"
    if [ -z "$client" ]; then
        echo "no FreeRDP client here; install freerdp for --rdp" >&2
        return 1
    fi
    pass="$(sed -n 2p "$RDP_CREDS")"

    if ! xdpyinfo -display "$RDP_DISPLAY" >/dev/null 2>&1; then
        Xvfb "$RDP_DISPLAY" -screen 0 "${SIZE}x24" -nolisten tcp \
            >"$STATE.rdp-xvfb.log" 2>&1 &
        local j
        for j in $(seq 1 40); do
            xdpyinfo -display "$RDP_DISPLAY" >/dev/null 2>&1 && break
            sleep 0.25
        done
    fi

    pkill -f "xfreerdp.*$ip" 2>/dev/null
    DISPLAY="$RDP_DISPLAY" "$client" "/v:$ip" "/u:$RDP_USER" "/p:$pass" \
        "/size:$SIZE" /cert:ignore /log-level:ERROR +auto-reconnect \
        >"$STATE.rdp.log" 2>&1 &

    # Wait for Windows to say the session is up, not for the client to say it
    # connected: the logon still has a profile to load, and a task started
    # before that lands nowhere.
    local i state=""
    for i in $(seq 1 60); do
        state="$(run_guest "
\$u = quser 2>\$null | Select-String '$RDP_USER'
if (\$u) { Write-Output (\$u -replace '.*(Active|Disc).*', '\$1') }
" 2>/dev/null | tr -d '\r' | grep -oE 'Active|Disc' | tail -1)"
        [ "$state" = "Active" ] && break
        sleep 2
    done
    if [ "$state" != "Active" ]; then
        echo "the $RDP_USER session never came up; see $STATE.rdp.log" >&2
        return 1
    fi
    echo "guest session for $RDP_USER is up"
}

# Captures with nothing on the console. wincap.exe runs inside the browser's
# own session and asks the window to render itself, which works while that
# session is disconnected; QEMU's screendump cannot, because it photographs
# whatever the console is scanning out. The session is disconnected only once
# the capture server has answered, so a failure here leaves the guest usable.
# Builds wincap and puts it where the guest can fetch it, once for however
# many instances follow. Echoes the staged file's name.
stage_wincap() {
    command -v x86_64-w64-mingw32-g++ >/dev/null || {
        echo "no x86_64-w64-mingw32-g++; captures stay on the console" >&2
        return 1
    }
    [ -d "$STAGE_DIR" ] || { echo "no directory at STAGE_DIR=$STAGE_DIR" >&2; return 1; }

    local work="$STATE.wincap"
    mkdir -p "$work"
    x86_64-w64-mingw32-g++ -std=c++17 -O2 "$HERE/wincap.c" -o "$work/wincap.exe" \
        -lws2_32 -lgdi32 -luser32 -static 2>"$work/build.log" || {
        cat "$work/build.log" >&2
        return 1
    }
    local stamp staged
    stamp="$(sha256sum "$work/wincap.exe" | cut -c1-16)"
    staged="wincap.$stamp.exe"
    cp "$work/wincap.exe" "$STAGE_DIR/$staged"
    printf '%s' "$staged"
}

# start_capture <guest ip> <staged wincap> <marionette port> <capture port> <firefox pid>
#
# One server per browser. The window is named by the pid of the browser that
# owns it, because every window on the desktop is MozillaWindowClass with
# whatever title the sweep navigated to.
start_capture() {
    local ip="$1" staged="$2" mport="$3" cport="$4" pid="$5"

    # Under SystemDrive and not TEMP, for the reason the profile directory is:
    # the agent runs as SYSTEM, so its TEMP is one the interactive user cannot
    # read, and this has to be started by that user to land in their session.
    # wincap binds INADDR_ANY, so it needs the firewall hole but no port proxy.
    run_guest "
\$exe = Join-Path \$env:SystemDrive '$staged'
if (-not (Test-Path \$exe)) {
    Invoke-WebRequest -UseBasicParsing '$STAGE_URL/$staged' -OutFile \$exe
}
icacls \$exe /grant '*S-1-5-32-545:RX' | Out-Null
# Windows meets a program's first listen with a prompt in its session and an
# inbound block rule in the meantime, and a block rule outranks the port allow
# below. The staged name carries a build stamp, so every rebuild is a new
# program to it. The blocks it has made are cleared and the program allowed.
Get-NetFirewallRule -Direction Inbound -Action Block -ErrorAction SilentlyContinue |
    Where-Object { \$_.DisplayName -like 'wincap*' } | Remove-NetFirewallRule
netsh advfirewall firewall delete rule name=dwc-wincap 2>&1 | Out-Null
netsh advfirewall firewall add rule name=dwc-wincap dir=in action=allow program=\$exe | Out-Null
schtasks /delete /tn dwccap-$mport /f 2>&1 | Out-Null
\$act = New-ScheduledTaskAction -Execute \$exe -Argument '--serve $cport --class MozillaWindowClass --pid $pid'
\$pri = $GUEST_PRINCIPAL
Register-ScheduledTask -TaskName 'dwccap-$mport' -Action \$act -Principal \$pri | Out-Null
Start-ScheduledTask -TaskName 'dwccap-$mport'
netsh advfirewall firewall delete rule name=dwc-capture-$mport 2>&1 | Out-Null
netsh advfirewall firewall add rule name=dwc-capture-$mport dir=in action=allow protocol=TCP localport=$cport | Out-Null
Write-Output 'capture server started'
" || return 1

    # The browser has to paint a window before there is anything to answer
    # for, and a guest running several of them takes longer over the last
    # than the first.
    local i size=""
    for i in $(seq 1 90); do
        size="$(python3 "$HERE/wincap_size.py" "$ip" "$cport")"
        case "$size" in
            ""|"0 0") sleep 1 ;;
            *) break ;;
        esac
    done
    if [ -z "$size" ] || [ "$size" = "0 0" ]; then
        echo "no window answered on $ip:$cport; captures stay on the console" >&2
        return 1
    fi
    echo "captures at guest:$ip:$cport, window $size"
}

# Disconnecting the console is what takes the browser off the guest's screen,
# and it is also what puts the guest at its logon screen for as long as the
# sweep runs. That is somebody's desktop, so it is never done unasked.
hide_console() {
    run_guest "
\$s = (Get-Process firefox -ErrorAction SilentlyContinue | Select-Object -First 1).SessionId
if (-not \$s) { Write-Error 'no firefox to take the session id from'; exit 1 }
tsdiscon \$s
Write-Output ('disconnected session ' + \$s)
"
}

if [ "$COMMAND" = "guest-stop" ]; then
    pkill -f "xfreerdp.*/v:" 2>/dev/null
    run_guest "
Get-Process firefox -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process | Where-Object { \$_.Name -like 'wincap*' } | Stop-Process -Force
# However many instances the last run left, found rather than named, so a stop
# does not need the port list the start was given.
schtasks /query /fo csv 2>\$null | ForEach-Object {
    if (\$_ -match '\"\\\\(dwc(ff|cap)-\d+)\"') { schtasks /delete /tn \$matches[1] /f 2>&1 | Out-Null }
}
schtasks /delete /tn dwcff /f 2>&1 | Out-Null
schtasks /delete /tn dwccap /f 2>&1 | Out-Null
netsh interface portproxy show v4tov4 | ForEach-Object {
    if (\$_ -match '^\s*0\.0\.0\.0\s+(\d+)') {
        netsh interface portproxy delete v4tov4 listenport=\$matches[1] listenaddress=0.0.0.0 | Out-Null
    }
}
netsh advfirewall firewall show rule name=all | Select-String '^Rule Name:\s+(dwc-\S+)' |
    ForEach-Object { netsh advfirewall firewall delete rule name=(\$_.Matches[0].Groups[1].Value) 2>&1 | Out-Null }
netsh advfirewall firewall delete rule name=dwc-marionette | Out-Null
netsh advfirewall firewall delete rule name=dwc-capture 2>&1 | Out-Null
Get-NetFirewallRule -Direction Inbound -Action Block -ErrorAction SilentlyContinue |
    Where-Object { \$_.DisplayName -like 'wincap*' } | Remove-NetFirewallRule
# The harness's own session, if there is one. Never the console user's.
\$h = quser 2>\$null | Select-String 'dwcparity'
if (\$h) { logoff (\$h -replace '.*?\s(\d+)\s+(Active|Disc).*', '\$1') 2>&1 | Out-Null }
# Back onto the console. Logging in again instead would open a second session
# and leave this one running with nobody attached to it.
\$d = (quser 2>\$null | Select-String 'Disc') -replace '.*\s(\d+)\s+Disc.*', '\$1'
if (\$d) { tscon \$d /dest:console 2>&1 | Out-Null }
Write-Output 'guest stopped'
"
    exit $?
fi

if [ "$COMMAND" = "guest" ] && [ "$USE_PREFS" = 1 ]; then
    echo "guest needs --no-prefs: the parity prefs live in the shim now, and a" >&2
    echo "Windows Firefox given them answers with what they say instead of with" >&2
    echo "what Windows does." >&2
    exit 2
fi

if [ "$COMMAND" = "guest" ] && [ "$RDP_SESSION" = 1 ]; then
    GUEST_IP="$(python3 "$HERE/vmexec.py" "$GUEST_DOMAIN" powershell -NoProfile -Command \
        "(Get-NetIPAddress -AddressFamily IPv4 | Where-Object { \$_.InterfaceAlias -notlike '*Loopback*' } | Select-Object -First 1).IPAddress" \
        2>/dev/null | tr -d '\r' | tail -1)"
    [ -n "$GUEST_IP" ] || { echo "no address for $GUEST_DOMAIN" >&2; exit 1; }
    start_rdp_session "$GUEST_IP" || exit 1
fi

# check_guest_dwrite_names <firefox.exe>
#
# Both of xul.dll's DirectWrite references, counted. A build meant to run on
# DWriteCore has to name it in the import descriptor, which is ASCII, and in
# dwrote's LoadLibraryW argument, which is UTF-16; a build meant to run on the
# system DirectWrite has to name that in both. Anything else is a mixture, and
# a mixture reads as a parity difference on some cells and crashes the GPU
# process on others.
check_guest_dwrite_names() {
    local exe="$1"
    local xul="${exe%\\*}\\xul.dll"
    local counts
    counts="$(run_guest "
\$src = @'
using System;
public class Scan {
  public static int Count(byte[] hay, byte[] needle) {
    int hits = 0;
    for (int i = 0; i <= hay.Length - needle.Length; i++) {
      int j = 0;
      while (j < needle.Length && hay[i+j] == needle[j]) j++;
      if (j == needle.Length) hits++;
    }
    return hits;
  }
}
'@
if (-not ('Scan' -as [type])) { Add-Type -TypeDefinition \$src }
\$b = [IO.File]::ReadAllBytes('$xul')
\$a = [Text.Encoding]::ASCII
\$u = [Text.Encoding]::Unicode
'core=' + ([Scan]::Count(\$b, \$a.GetBytes('dwcore.dll')) + [Scan]::Count(\$b, \$u.GetBytes('dwcore.dll'))) +
' write=' + ([Scan]::Count(\$b, \$a.GetBytes('dwrite.dll')) + [Scan]::Count(\$b, \$u.GetBytes('dwrite.dll')))
" | tr -d '\r' | tail -1)"
    local core write
    core="${counts#core=}"; core="${core%% *}"
    write="${counts##*write=}"
    case "$core:$write" in
        2:0) echo "guest xul.dll is on DWriteCore" ;;
        0:2) echo "guest xul.dll is on the system DirectWrite" ;;
        *)   echo "guest xul.dll names both DirectWrite builds ($counts): the import descriptor and dwrote's LoadLibraryW have to agree, or gfx/2d and WebRender run on different implementations" >&2
             return 1 ;;
    esac
}

# start_guest_instance <marionette port>
#
# Everything one browser needs, named after its port so several can stand side
# by side. Each has its own profile, Marionette port, pair of scheduled tasks,
# port proxy and firewall holes.
start_guest_instance() {
    local gport="$1"
    local gcap=$((gport + 1))
    # Marionette binds inside the guest and the port proxy binds in front of
    # it, so the two cannot share a number. Whichever starts first takes it
    # and the other silently does without. The proxy keeps the port the sweep
    # names and Firefox listens a hundred above it.
    local ginner=$((gport + 100))

    # The prefs go over in pieces. guest-exec passes its argument vector
    # through a helper with a bounded command line, and a whole prefs file
    # base64-encoded inside a UTF-16 -EncodedCommand blows past it - the agent
    # answers "Failed to execute helper program (Invalid argument)", which
    # names nothing. So: append a chunk at a time, then decode in the guest.
    GUEST_PREFS="$STATE.guest-user.js.$gport"
    {
        echo ""
        echo "// Added by tools/testing/run_parity_firefox.sh"
        echo "user_pref(\"marionette.port\", $ginner);"
        echo 'user_pref("browser.shell.checkDefaultBrowser", false);'
        # The sweep asks the page whether anything painted after its capture
        # landed; this is what lets content see its own paints.
        echo 'user_pref("dom.send_after_paint_to_content", true);'
        # A caret that blinks is on in one capture and off in the next, and a
        # page that focuses an editable element carries one. nsCaret stops the
        # timer with the caret drawn when the blink time is not positive, so
        # both sides show it in every capture instead of a coin toss each.
        echo 'user_pref("ui.caretBlinkTime", 0);'
        # One page is open at a time and the sweep drives it through one tab,
        # so the default pool of content processes is idle memory. Eight
        # browsers of it starves the guest, and a capture that misses its
        # frame is reported as a difference. Both sides are held to the same
        # number so neither is measuring a different process layout.
        echo 'user_pref("dom.ipc.processCount", 2);'
        # Site isolation pools processes per origin and ignores the count
        # above, and a preallocated one sits there whether or not it is used.
        echo 'user_pref("dom.ipc.processCount.webIsolated", 1);'
        echo 'user_pref("dom.ipc.processPrelaunch.enabled", false);'
        # No caching, on either side. A page under measurement is usually one
        # being regenerated between runs, and a browser that revalidates on a
        # heuristic keeps the old copy - so the two sides compare different
        # pages and the number is wrong instead of slow.
        echo 'user_pref("browser.cache.disk.enable", false);'
        echo 'user_pref("browser.cache.memory.enable", false);'
        echo 'user_pref("browser.cache.check_doc_frequency", 1);'
        # No favicons. A page that names none still costs a request for
        # /favicon.ico after it has loaded, and Firefox reports that request in
        # the status panel across the bottom left corner of the window. The
        # marker handshake cannot see it, being chrome and not page, so a
        # capture taken while it is up carries 21 rows of "Looking up
        # <host>..." and reads as a font difference wherever the page has ink
        # there. Only the side whose page server is not loopback shows it,
        # which is this one.
        # Nothing off the page server, on either side. A page that fetches a
        # script from the public internet renders as a function of how much of
        # that download the capture waited through, which differs per machine
        # and per run: css-writing-modes/test-plan/index.html pulls ReSpec from
        # www.w3.org and builds a table-of-contents sidebar from it, and it
        # swept at four different percentages in four runs because of it. A
        # proxy that answers nothing is the Marionette equivalent of the
        # Network.setBlockedURLs the DevTools sides use. Private ranges stay
        # direct, since the guest reaches this machine's page server by its
        # LAN address and only the public side has to be cut.
        echo 'user_pref("network.proxy.type", 1);'
        echo 'user_pref("network.proxy.http", "127.0.0.1");'
        echo 'user_pref("network.proxy.http_port", 1);'
        echo 'user_pref("network.proxy.ssl", "127.0.0.1");'
        echo 'user_pref("network.proxy.ssl_port", 1);'
        echo 'user_pref("network.proxy.no_proxies_on", "localhost,127.0.0.1,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16");'
        echo 'user_pref("browser.chrome.site_icons", false);'
        # The same scheme on both sides, whatever theme each machine is set
        # to. A page with a prefers-color-scheme rule otherwise reads the
        # guest's Windows theme on one side and the GTK theme on the other,
        # and clagnut-pangrams came back 0.0000% identical for that alone.
        # 1 is Light and 0 is Dark, confirmed against a running browser
        # rather than read off the enum.
        echo "user_pref(\"layout.css.prefers-color-scheme.content-override\", $SCHEME_PREF);"
        # A scale states itself as a Gecko pref on either platform, so both
        # sides reach it through the same code. Written whether or not one was
        # asked for: user.js is appended to, and a profile left from an earlier
        # run otherwise keeps that run's scale and answers with it. -1.0 is the
        # default, which is the browser following the display it is on.
        echo "user_pref(\"layout.css.devPixelsPerPx\", \"${SCALE:--1.0}\");"
        echo 'user_pref("datareporting.policy.dataSubmissionEnabled", false);'
    } > "$GUEST_PREFS"
    CHUNKS="$(base64 -w0 < "$GUEST_PREFS" | fold -w1200)"
    FIRST=1
    while IFS= read -r chunk; do
        if [ "$FIRST" = 1 ]; then
            run_guest "Set-Content -Path \$env:TEMP\dwc-prefs-$gport.b64 -Value '$chunk' -NoNewline" >/dev/null || return 1
            FIRST=0
        else
            run_guest "Add-Content -Path \$env:TEMP\dwc-prefs-$gport.b64 -Value '$chunk' -NoNewline" >/dev/null || return 1
        fi
    done <<< "$CHUNKS"

    run_guest "
\$ErrorActionPreference = 'Continue'
# Not under LOCALAPPDATA: the guest agent runs as SYSTEM, so that expands to
# the *service* profile, which the interactive user the browser runs as cannot
# read. Firefox then starts on some other profile with no user.js and no
# Marionette, and the only symptom is a port that never opens. A fixed path
# with the Users group granted access is reachable from both sides. S-1-5-32-545
# is that group by SID, which is the same on a guest in any language.
\$profileDir = Join-Path \$env:SystemDrive 'dwc-parity-profile-$gport'
New-Item -ItemType Directory -Force -Path \$profileDir | Out-Null
icacls \$profileDir /grant '*S-1-5-32-545:(OI)(CI)F' | Out-Null
\$prefs = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String((Get-Content \$env:TEMP\dwc-prefs-$gport.b64 -Raw)))
Set-Content -Path (Join-Path \$profileDir 'user.js') -Value \$prefs -Encoding UTF8
Write-Output ('user.js ' + (Get-Item (Join-Path \$profileDir 'user.js')).Length + ' bytes')
" || return 1

    # The launch goes over in three calls. guest-exec passes its argument
    # vector through a helper with a bounded command line, and the whole
    # launch as a single UTF-16 -EncodedCommand goes past it, with a failure
    # that names nothing.
    run_guest "
\$exe = @('$GUEST_FIREFOX',
          (Join-Path \$env:ProgramFiles 'Mozilla Firefox\firefox.exe'),
          (Join-Path \${env:ProgramFiles(x86)} 'Mozilla Firefox\firefox.exe')) |
        Where-Object { \$_ -and (Test-Path \$_) } | Select-Object -First 1
if (-not \$exe) { Write-Error 'no firefox.exe in the guest'; exit 1 }
schtasks /delete /tn dwcff-$gport /f 2>&1 | Out-Null
\$d = Join-Path \$env:SystemDrive 'dwc-parity-profile-$gport'
\$a = '-marionette -remote-allow-system-access -no-remote -profile \"' + \$d + '\" \"$URL\"'
\$act = New-ScheduledTaskAction -Execute \$exe -Argument \$a
Register-ScheduledTask -TaskName 'dwcff-$gport' -Action \$act -Principal ($GUEST_PRINCIPAL) | Out-Null
Start-ScheduledTask -TaskName 'dwcff-$gport'
Write-Output 'started'
" >/dev/null || return 1

    # The window's owner, which is what the capture server is told to answer
    # for. Found by the Marionette port it listens on, which is this
    # instance's alone and belongs to the parent process, the one with the
    # window. The guest agent runs in session 0 and can read neither another
    # session's command lines nor its window handles; wincap runs in the
    # browser's session and can.
    GUEST_PID="$(run_guest "
foreach (\$i in 1..60) {
  \$c = Get-NetTCPConnection -LocalPort $ginner -State Listen -EA SilentlyContinue |
        Select-Object -First 1
  if (\$c) { Write-Output \$c.OwningProcess; exit 0 }
  Start-Sleep -Milliseconds 500
}
Write-Output 0
" | tr -d '\r' | tail -1)"

    run_guest "
netsh interface portproxy delete v4tov4 listenport=$gport listenaddress=0.0.0.0 2>&1 | Out-Null
netsh interface portproxy add v4tov4 listenport=$gport listenaddress=0.0.0.0 connectport=$ginner connectaddress=127.0.0.1 | Out-Null
netsh advfirewall firewall delete rule name=dwc-marionette-$gport 2>&1 | Out-Null
netsh advfirewall firewall add rule name=dwc-marionette-$gport dir=in action=allow protocol=TCP localport=$gport | Out-Null
Write-Output 'routed'
" >/dev/null || return 1

    case "$GUEST_PID" in
        ''|0|*[!0-9]*) echo "no browser on port $gport in the guest" >&2; return 1 ;;
    esac
    return 0
}

if [ "$COMMAND" = "guest" ]; then
    # Once for the whole set, before any of them starts. A browser left from a
    # previous run holds a profile and a port, and a wincap left listening
    # answers for a window that is no longer anybody's.
    run_guest "
Get-Process firefox -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process | Where-Object { \$_.Name -like 'wincap*' } | Stop-Process -Force
Write-Output 'cleared'
" >/dev/null || exit 1

    STAGED=""
    if [ -n "$STAGE_DIR" ] && [ -n "$STAGE_URL" ]; then
        STAGED="$(stage_wincap)" || STAGED=""
    fi

    GUEST_IP="$(python3 "$HERE/vmexec.py" "$GUEST_DOMAIN" powershell -NoProfile -Command \
        "(Get-NetIPAddress -AddressFamily IPv4 | Where-Object { \$_.InterfaceAlias -notlike '*Loopback*' } | Select-Object -First 1).IPAddress" \
        2>/dev/null | tr -d '\r' | tail -1)"
    [ -n "$GUEST_IP" ] || { echo "no address for $GUEST_DOMAIN" >&2; exit 1; }

    if [ -n "$GUEST_FIREFOX" ]; then
        check_guest_dwrite_names "$GUEST_FIREFOX" || exit 1
    fi

    STARTED=""
    for gp in $GUEST_PORTS; do
        start_guest_instance "$gp" || exit 1
        DWC_TESTING_DIR="$HERE" warm_up "$GUEST_IP" "$gp"
        echo "marionette at $GUEST_IP:$gp"
        if [ -n "$STAGED" ]; then
            start_capture "$GUEST_IP" "$STAGED" "$gp" "$((gp + 1))" "$GUEST_PID" || exit 1
        fi
        STARTED="$STARTED$gp,"
    done
    echo "guest ports ${STARTED%,}"
    [ "$HIDE_CONSOLE" = 1 ] && hide_console
    exit 0
fi

BROWSER="$(find_firefox)"
[ -n "$BROWSER" ] || { echo "no firefox found; set FIREFOX" >&2; exit 1; }

mkdir -p "$STATE"
[ -n "$PROFILE" ] || PROFILE="$STATE/profile"
mkdir -p "$PROFILE"

# The prefs, plus the Marionette port. Firefox reads user.js on every start, so
# the parity prefs are re-applied even if a previous run wrote prefs.js.
: > "$PROFILE/user.js"
{
    echo ""
    echo "// Added by tools/testing/run_parity_firefox.sh"
    echo "user_pref(\"marionette.port\", $PORT);"
    echo "user_pref(\"browser.shell.checkDefaultBrowser\", false);"
    echo "user_pref(\"dom.send_after_paint_to_content\", true);"
    echo "user_pref(\"ui.caretBlinkTime\", 0);"
    echo "user_pref(\"dom.ipc.processCount\", 2);"
    echo "user_pref(\"dom.ipc.processCount.webIsolated\", 1);"
    echo "user_pref(\"dom.ipc.processPrelaunch.enabled\", false);"
    echo "user_pref(\"browser.cache.disk.enable\", false);"
    echo "user_pref(\"browser.cache.memory.enable\", false);"
    echo "user_pref(\"browser.cache.check_doc_frequency\", 1);"
    # Both sides alike; see the guest block above for what each is for.
    echo "user_pref(\"network.proxy.type\", 1);"
    echo "user_pref(\"network.proxy.http\", \"127.0.0.1\");"
    echo "user_pref(\"network.proxy.http_port\", 1);"
    echo "user_pref(\"network.proxy.ssl\", \"127.0.0.1\");"
    echo "user_pref(\"network.proxy.ssl_port\", 1);"
    echo "user_pref(\"network.proxy.no_proxies_on\", \"localhost,127.0.0.1,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16\");"
    echo "user_pref(\"browser.chrome.site_icons\", false);"
    echo "user_pref(\"layout.css.prefers-color-scheme.content-override\", $SCHEME_PREF);"
    echo "user_pref(\"layout.css.devPixelsPerPx\", \"${SCALE:--1.0}\");"
    echo "user_pref(\"browser.startup.homepage_override.mstone\", \"ignore\");"
    echo "user_pref(\"datareporting.policy.dataSubmissionEnabled\", false);"
    echo "user_pref(\"toolkit.telemetry.reportingpolicy.firstRun\", false);"
} >> "$PROFILE/user.js"

# A browser whose Marionette cannot bind still starts, and every later command
# then reaches whoever already holds the port. That reads as a shim that did
# nothing, so refuse instead. It catches a browser this script has lost track
# of, which is what happens when TMPDIR or the port changes under a running one.
if (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then
    exec 3<&- 2>/dev/null
    echo "port $PORT is already in use; stop that browser first" >&2
    ss -ltnp 2>/dev/null | grep ":$PORT " >&2
    exit 1
fi

if command -v Xvfb >/dev/null 2>&1; then
    Xvfb "$DISPLAY_NAME" -screen 0 "${SIZE}x24" >/dev/null 2>&1 &
    echo $! > "$STATE/xvfb.pid"
    sleep 2
else
    echo "Xvfb not found; using the display already at $DISPLAY_NAME" >&2
fi

DISPLAY="$DISPLAY_NAME" python3 "$HERE/minwm.py" >"$STATE/minwm.log" 2>&1 &
echo $! > "$STATE/minwm.pid"
sleep 1

export DISPLAY="$DISPLAY_NAME"
export GDK_BACKEND=x11
export MOZ_ENABLE_WAYLAND=0
unset WAYLAND_DISPLAY
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8
if [ -n "$SHIM" ]; then
    export LD_PRELOAD="$SHIM"
fi

"$BROWSER" -marionette -remote-allow-system-access \
           -profile "$PROFILE" -no-remote "$URL" \
           >"$STATE/firefox.log" 2>&1 &
echo $! > "$STATE/firefox.pid"

for _ in $(seq 1 60); do
    if (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then
        # The guest gets this at the end of its own start; this side needs it
        # for the same reason. A loopback page server skips the "Looking up"
        # panel but not the one a subresource puts up, and a panel that appears
        # for one cell of a sweep and not the next is a constant-size
        # difference on whichever page it lands on.
        DWC_TESTING_DIR="$HERE" warm_up 127.0.0.1 "$PORT"
        echo "firefox on $DISPLAY_NAME, marionette 127.0.0.1:$PORT${SHIM:+, shim $SHIM}"
        echo "profile $PROFILE"
        exit 0
    fi
    sleep 1
done

echo "marionette never came up on $PORT; see $STATE/firefox.log" >&2
exit 1
