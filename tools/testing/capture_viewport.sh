#!/usr/bin/env bash
#
# capture_viewport.sh - drive one browser and take the two screenshots.
#
#   capture_viewport.sh <backend> <out-prefix> <host> <port> <url> \
#                       <width> <height> [--wait S] [--scroll Y]
#
# backend is how to photograph the screen the browser is on:
#
#   x11:<display>       ImageMagick `import -window root` on that X display
#   libvirt:<domain>    `virsh screenshot` of that guest's display
#
# Writes <out-prefix>_marked.png and <out-prefix>_clean.png. The marked one
# locates the viewport, the clean one is what gets compared -
# compare_viewport.py takes all four files.
#
# Run it once per machine with the same width, height, url and scroll, then
# compare. The two runs are independent; neither side has to know about the
# other.
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

BACKEND="$1"; OUT="$2"; shift 2
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="$(mktemp -u "${TMPDIR:-/tmp}/dwc-capture-XXXXXX")"

# How long to keep retaking a shot that does not yet show the change. Generous,
# because it is a ceiling that is never reached and not a delay that is
# always paid.
ATTEMPTS="${CAPTURE_ATTEMPTS:-60}"

case "$BACKEND" in
    x11:*)     DISPLAY_NAME="${BACKEND#x11:}" ;;
    libvirt:*) DOMAIN="${BACKEND#libvirt:}" ;;
    *) echo "backend must be x11:<display> or libvirt:<domain>"; exit 2 ;;
esac

shoot() {                                  # shoot <path.png>
    case "$BACKEND" in
        x11:*)
            DISPLAY="$DISPLAY_NAME" import -window root "$1"
            ;;
        libvirt:*)
            virsh --connect "${LIBVIRT_URI:-qemu:///system}" screenshot \
                  "$DOMAIN" "$1.ppm" >/dev/null
            magick "$1.ppm" "$1"
            rm -f "$1.ppm"
            ;;
    esac
}

# Retake until the frame shows what this stage is waiting for, and has stopped
# changing. Two separate conditions, and both are needed:
#
#   the marker is present ("marker") or gone ("none") - which proves the frame
#   is on the far side of the DOM change;
#
#   two consecutive shots are byte-identical - which proves nothing else is
#   still arriving. Firefox fades its scrollbar out a moment after a scroll, so
#   without this the shot catches the thumb mid-fade and two machines disagree
#   about a hundred pixels of widget that is not what anyone is measuring.
#
# "uniform" is the blank-framebuffer failure - no window manager, or a browser
# on another display - and is never accepted, because it has no marker either
# and would otherwise read as a perfectly good clean shot.
capture_until() {                          # capture_until <path.png> <want>
    local path="$1" want="$2" state=""
    local prev="$1.prev"
    local i=0 steady=0
    rm -f "$prev"
    while [ "$i" -lt "$ATTEMPTS" ]; do
        shoot "$path"
        # Steadiness first, because it is a byte compare and free, and the
        # marker check is a Python process with an image decode in it. On a
        # settled screen the second shot is already steady, so this asks the
        # expensive question once per stage instead of once per shot.
        steady=0
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
# shots are of different layouts. Rare - the browser is warmed at startup - and
# retryable, because the reflow has landed by the time the retry starts.
RETRIES="${CAPTURE_RETRIES:-2}"

attempt() {
rm -f "$TAG.marked" "$TAG.clean"
python3 "$HERE/capture_viewport.py" "$1" "$2" "$3" "$4" "$5" "$TAG" "${@:6}" &
DRIVER=$!
trap 'kill $DRIVER 2>/dev/null; rm -f "$TAG.marked" "$TAG.clean"' EXIT

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
        if ! kill -0 "$DRIVER" 2>/dev/null; then
            wait "$DRIVER"; dstatus=$?
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
    want="none"; [ "$stage" = "marked" ] && want="marker"
    capture_until "${OUT}_${stage}.png" "$want" || return 1
    rm -f "$TAG.$stage"
done

wait $DRIVER
}

for try in $(seq 0 "$RETRIES"); do
    attempt "$@"
    status=$?
    [ "$status" -ne 3 ] && exit "$status"
    echo "retrying: the page reflowed under the capture" >&2
done
exit 3
