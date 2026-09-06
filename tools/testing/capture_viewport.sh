#!/usr/bin/env bash
#
# capture_viewport.sh - drive one browser and take the two screenshots.
#
#   capture_viewport.sh <backend> <out-prefix> <host> <port> <url> \
#                       <width> <height> [--wait S] [--scroll Y] [--css RULES]
#
# backend is how to photograph the screen the browser is on. screen_grab.py
# reads it, and a sweep reads a screen through the same three:
#
#   x11:<display>       the root window of that X display
#   libvirt:<domain>    that guest's display, through QEMU's screendump
#   guest:<host>[:port] a window inside a Windows guest, over TCP from wincap
#
# Writes <out-prefix>_marked.png and <out-prefix>_clean.png. The marked one
# locates the viewport, the clean one is what gets compared -
# compare_viewport.py takes all four files.
#
# Run it once per machine with the same width, height, url, scroll and --css,
# then compare. The two runs are independent; neither side has to know about
# the other. --css adds a rule sheet on top of the page, which is how a
# suspected cause is taken out of both sides at once.
#
# There is no fixed settle here. What a settle would be waiting for is visible
# in the photograph itself: the marked stage is done when the marker is in the
# frame, the clean stage when it is gone. So the shot is retaken until the
# frame answers, which is normally the first one, and a slow machine waits
# exactly as long as it needs to, and not as long as the fastest machine was
# guessed to need.
set -u

# An inherited LD_PRELOAD would make the control run not a control.
unset LD_PRELOAD

if [ $# -lt 7 ]; then
    sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

# Which browser driver to use. Marionette (Firefox) is the default, and
# --driver cdp drives Chromium and Electron over the DevTools protocol. Both
# put the page into the same state, the choreography being shared in
# viewport_protocol.py, so the screenshot half below does not care which.
DRIVER="marionette"
if [ "$1" = "--driver" ]; then DRIVER="$2"; shift 2; fi

BACKEND="$1"; OUT="$2"; shift 2
# The browser's own port. The guest backend reaches its capture server by it,
# and shoot() is called from too deep to see the arguments.
PORT="$2"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="$(mktemp -u "${TMPDIR:-/tmp}/dwc-capture-XXXXXX")"

# How long to keep retaking a shot that does not yet show the change. Generous,
# because it is a ceiling that is never reached and not a delay that is
# always paid.
ATTEMPTS="${CAPTURE_ATTEMPTS:-60}"


case "$BACKEND" in
    x11:*|libvirt:*|guest:*) ;;
    *) echo "backend must be x11:<display>, libvirt:<domain> or guest:<host>[:port]" >&2
       exit 2 ;;
esac

shoot() {                                  # shoot <path.png>
    python3 "$HERE/screen_grab.py" "$BACKEND" "$1" --port "$PORT"
}

# Retake until the frame shows what this stage is waiting for, and has stopped
# changing. Two conditions, and both are needed. The marker being present
# ("marker") or gone ("none") proves the frame is on the far side of the DOM
# change. Two consecutive shots being byte-identical proves nothing else is
# still arriving, since Firefox fades its scrollbar out a moment after a
# scroll and a shot taken mid-fade puts a hundred pixels of widget into the
# comparison.
#
# "uniform" is the blank-framebuffer failure, no window manager or a browser
# on another display, and is never accepted, because it has no marker either
# and would otherwise read as a perfectly good clean shot.
capture_until() {                 # capture_until <path.png> <want> [steady?]
    local path="$1" want="$2" need_steady="${3:-1}" state=""
    local prev="$1.prev"
    local i=0 steady=0
    rm -f "$prev"
    while [ "$i" -lt "$ATTEMPTS" ]; do
        shoot "$path"
        # Steadiness first, because it is a byte compare and free, and the
        # marker check is a Python process with an image decode in it. On a
        # settled screen the second shot is already steady, so this asks the
        # expensive question once per stage instead of once per shot.
        steady="$((1 - need_steady))"
        [ -f "$prev" ] && cmp -s "$path" "$prev" && steady=1
        if [ "$steady" = 1 ]; then
            state="$(python3 "$HERE/compare_viewport.py" --marker "$path" | cut -d' ' -f1)"
            case "$want:$state" in
                marker:marker|none:none) rm -f "$prev"; return 0 ;;
            esac
        fi
        cp "$path" "$prev"
        i=$((i + 1))
    done
    rm -f "$prev"
    echo "gave up waiting for the $want stage to settle on screen (last: $state)" >&2
    if [ "$state" = "uniform" ]; then
        echo "  a single-color screenshot means no window manager mapped the" >&2
        echo "  window, or the browser is on a different display - see minwm.py" >&2
    else
        echo "  the frame never stopped changing: something on the page animates," >&2
        echo "  or a caret is blinking in a focused field" >&2
    fi
    return 1
}

# Exit 3 from the driver means the page reflowed under the capture, so the two
# shots are of different layouts. Rare, since the browser is warmed at
# startup, and retryable, because the reflow has landed by the time the
# retry starts.
RETRIES="${CAPTURE_RETRIES:-2}"

case "$DRIVER" in
    marionette|cdp) ;;
    *) echo "unknown driver: $DRIVER" >&2; exit 2 ;;
esac

attempt() {
rm -f "$TAG.marked" "$TAG.clean"
# attempt runs again on a retry, so the pid needs a name of its own; holding it
# in $DRIVER left the second run reading a process id as a driver name.
python3 "$HERE/capture_viewport.py" "$1" "$2" "$3" "$4" "$5" "$TAG" \
        --driver "$DRIVER" "${@:6}" &
DRIVER_PID=$!
trap 'kill $DRIVER_PID 2>/dev/null; rm -f "$TAG.marked" "$TAG.clean"' EXIT

for stage in marked clean; do
    # The sentinel is a handshake between two processes on this machine, so
    # polling it is pure latency; the wait that matters is the one below.
    #
    # Watch the driver, not just the clock. A driver that fails - a browser
    # refusing a window size, a Marionette session that will not open - never
    # writes the sentinel, and a poll that only counts iterations then sits out
    # its whole deadline for a process it could see had already exited: five
    # minutes per stage, twice, for an error printed in the first second. The
    # driver's status is passed on, so a reflow (3) still retries.
    waited=0
    while [ ! -f "$TAG.$stage" ]; do
        if ! kill -0 "$DRIVER_PID" 2>/dev/null; then
            wait "$DRIVER_PID"; dstatus=$?
            echo "the driver exited (status $dstatus) before the $stage stage" >&2
            return "$dstatus"
        fi
        if [ "$waited" -ge 6000 ]; then
            echo "timed out waiting for the $stage stage" >&2
            return 1
        fi
        waited=$((waited + 1))
        sleep 0.05
    done
    # The marked shot is read for the marker's position and nothing else, so
    # it does not have to be of a settled screen. Only the clean shot is
    # compared pixel for pixel, and only it pays for the extra screenshot.
    want="none"; steady=1
    if [ "$stage" = "marked" ]; then want="marker"; steady=0; fi
    capture_until "${OUT}_${stage}.png" "$want" "$steady" || return 1
    rm -f "$TAG.$stage"
done

wait $DRIVER_PID
}

for try in $(seq 0 "$RETRIES"); do
    attempt "$@"
    status=$?
    [ "$status" -ne 3 ] && exit "$status"
    echo "retrying: the page reflowed under the capture" >&2
done
exit 3
