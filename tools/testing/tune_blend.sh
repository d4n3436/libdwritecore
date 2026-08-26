#!/usr/bin/env bash
#
# tune_blend.sh - find the WebRender blend settings that match Windows.
#
# Once the coverage textures agree, what is left between a Linux screenshot
# and a Windows one is the gamma-correct, contrast-enhanced blend WebRender
# applies when it composites one. DirectWrite does not put that in the texture
# - no CreateGlyphRunAnalysis overload takes an IDWriteRenderingParams - so it
# has to be matched on this side, and WebRender's prefs are integers on a scale
# that does not match the Windows cleartype_params ones.
#
# Renders tools/testing/pages/parity-reference.html under Xvfb for each candidate value,
# with the interposer active, and scores it against a Windows 11 capture.
#
#   tools/testing/tune_blend.sh windows.png "8 14 18 22"
#
# Xvfb, not firefox --headless: headless has no display geometry, so it
# renders grayscale and the whole comparison becomes meaningless.

set -u

# An inherited LD_PRELOAD would make the control run not a control.
unset LD_PRELOAD

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REFERENCE="${1:?usage: tune_blend.sh windows.png \"gamma values\"}"
VALUES="${2:-0 8 14 18 22}"
# The page this script scores against, which is the one in this repo. Set
# PAGE to serve it from somewhere else - over HTTP from another machine,
# say - but the default needs no server and no particular network.
PAGE="${PAGE:-file://$ROOT/tools/testing/pages/parity-reference.html}"
DISPLAY_NUM="${DISPLAY_NUM:-:99}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

command -v Xvfb >/dev/null || { echo "missing Xvfb"; exit 1; }

# Firefox, found and not assumed. The plain name is missing on more
# systems than one expects: Debian and Ubuntu ship the ESR package as
# firefox-esr, and the Flatpak and Snap builds put their launcher somewhere
# that is not always on PATH. Set FIREFOX to override any of this.
find_firefox() {
    local candidate
    for candidate in ${FIREFOX:+"$FIREFOX"} firefox firefox-esr firefox-bin; do
        if command -v "$candidate" >/dev/null 2>&1; then
            command -v "$candidate"
            return 0
        fi
    done
    for candidate in \
        "$HOME/.local/share/flatpak/exports/bin/org.mozilla.firefox" \
        /var/lib/flatpak/exports/bin/org.mozilla.firefox \
        /snap/bin/firefox; do
        [ -x "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    return 1
}
FIREFOX_BIN="$(find_firefox)" || { echo "no firefox found; set FIREFOX to its path"; exit 1; }

# ImageMagick 7 does not always install the legacy "import" command; where it
# does not, the same thing is "magick import". Resolved here instead of at
# the call site, because the screenshot is taken with stderr discarded and a
# missing command would otherwise produce no file and a meaningless score
# instead of an error.
if command -v import >/dev/null 2>&1; then
    SHOT=(import)
elif command -v magick >/dev/null 2>&1; then
    SHOT=(magick import)
else
    echo "missing ImageMagick (no 'import', no 'magick')"; exit 1
fi
[ -f "$REFERENCE" ] || { echo "no such reference: $REFERENCE"; exit 1; }

pgrep -u "$(id -u)" -f "Xvfb $DISPLAY_NUM" >/dev/null 2>&1 || {
    setsid Xvfb "$DISPLAY_NUM" -screen 0 1200x900x24 -nolisten tcp >/dev/null 2>&1 &
    sleep 2
}

for value in $VALUES; do
    profile="$WORK/p$value"
    mkdir -p "$profile"
    # Font selection only: the shim supplies gamma and contrast as *defaults*
    # through MOZ_DEFAULT_PREFS, and a user value beats a default, so writing
    # the value being swept into user.js is all it takes. Copying a whole prefs
    # file would pin the answer being looked for and every round would score
    # the same.
    : > "$profile/user.js"
    {
        echo 'user_pref("layout.css.devPixelsPerPx", "1.0");'
        echo "user_pref(\"${PREF:-gfx.font_rendering.freetype.gamma}\", $value);"
    } >> "$profile/user.js"

    DISPLAY="$DISPLAY_NUM" GDK_BACKEND=x11 MOZ_ENABLE_WAYLAND=0 WAYLAND_DISPLAY= \
    LD_PRELOAD="$ROOT/build/libcleartype.so" \
    setsid "$FIREFOX_BIN" --no-remote --profile "$profile" --kiosk "$PAGE" >/dev/null 2>&1 &
    firefox_pid=$!

    sleep 25
    DISPLAY="$DISPLAY_NUM" "${SHOT[@]}" -window root "$WORK/shot-$value.png" 2>/dev/null
    [ -s "$WORK/shot-$value.png" ] || { echo "screenshot failed for $value"; continue; }

    # setsid makes it a process group leader, so the whole tree goes with it.
    # Never pkill -f here: the pattern matches this script's own ancestry.
    kill -TERM -"$firefox_pid" 2>/dev/null
    wait "$firefox_pid" 2>/dev/null

    printf 'gamma %-4s ' "$value"
    python3 "$ROOT/tools/testing/score_blend.py" "$REFERENCE" "$WORK/shot-$value.png"
done
