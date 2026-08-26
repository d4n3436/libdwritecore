#!/usr/bin/env bash
#
# run_glyph_digest.sh - build glyph_digest.c and diff a run with the shim
# against a run without it.
#
#   tools/testing/run_glyph_digest.sh [font.ttf]
#
# SIZES and TEXT override the defaults. The shim is taken from build/; set
# SHIM to compare two builds of it against each other instead:
#
#   SHIM=/path/to/other/libcleartype.so tools/testing/run_glyph_digest.sh
#
# Prints one line per glyph, size and subpixel phase that differs. An empty
# diff means the two rasterize identically, which is the useful answer when
# checking that a change to the shim left the paths it was not meant to touch
# alone.
set -u

# An inherited LD_PRELOAD would make the control run not a control.
unset LD_PRELOAD

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SIZES="${SIZES:-11 12 13 13.33 14 15 15.5 16 18 20 24 32 48 96 110 140}"
TEXT="${TEXT:-}"
SHIM="${SHIM:-$ROOT/build/libcleartype.so}"

for tool in cc pkg-config fc-match; do
    command -v "$tool" >/dev/null || { echo "missing: $tool"; exit 1; }
done
[ -f "$SHIM" ] || { echo "no shim at $SHIM - build it first"; exit 1; }

FONT="${1:-}"
if [ -z "$FONT" ]; then
    # Ask fontconfig rather than guessing a path: the layout under
    # /usr/share/fonts differs between distributions. fc-match always answers,
    # so what it returns still has to be checked for existence.
    FONT="$(fc-match -f '%{file}' sans-serif)"
fi
[ -f "$FONT" ] || { echo "no such font file: ${FONT:-<none>}"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cc -O2 -o "$WORK/glyph_digest" "$ROOT/tools/testing/glyph_digest.c" \
   $(pkg-config --cflags --libs freetype2) || exit 1

ARGS=("$FONT")
for s in $SIZES; do ARGS+=("$s"); done
[ -n "$TEXT" ] && ARGS+=(--text "$TEXT")

echo "font   $FONT"
echo "shim   $SHIM"
echo "sizes  $SIZES"
for mode in lcd gray; do
    "$WORK/glyph_digest" "${ARGS[@]}" --mode "$mode" > "$WORK/plain.$mode" 2>/dev/null
    env LD_PRELOAD="$SHIM" "$WORK/glyph_digest" "${ARGS[@]}" --mode "$mode" \
        > "$WORK/shim.$mode" 2>/dev/null
    changed=$(diff "$WORK/plain.$mode" "$WORK/shim.$mode" | grep -c '^<' || true)
    total=$(wc -l < "$WORK/plain.$mode")
    echo "$mode: $changed of $total renders differ between FreeType and the shim"
done
