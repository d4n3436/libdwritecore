#!/usr/bin/env bash
#
# run_raster_probe.sh - rasterize the same glyphs through this library and
# through a Windows guest's DirectWrite, then diff the two.
#
#   tools/testing/run_raster_probe.sh <font> <glyphs> [probe options...]
#
# <font> is a file name, looked for under LINUX_FONTS here and under
# WINDOWS_FONTS in the guest, so the same argument names the same face on both
# sides. <glyphs> and the options are passed through to raster_probe; see the
# comment at the top of raster_probe.cpp for what it accepts.
#
#   tools/testing/run_raster_probe.sh arial.ttf 3-99 --em 16 \
#       --matrix 0,1,-1,0 --offset 1,-11
#
# Prints one line per glyph that rasterized differently, and a count. An empty
# diff means the two libraries agree byte for byte, which is the useful answer
# when deciding whether a difference on screen is the shim's to fix.
#
# The guest side needs three things, and without them only the local side runs:
#
#   DOMAIN        the libvirt domain, reached through vmexec.py
#   STAGE_DIR     a directory on this machine that an HTTP server already
#                 serves, used to hand the guest the cross-built probe
#   STAGE_URL     the URL prefix the guest reaches STAGE_DIR by, which is
#                 not the same string this machine would use
#
# The probe is copied into the guest's temp directory under a name carrying its
# hash, so a second run reuses it instead of fetching it again.
#
# Other knobs:
#
#   BUILD         the directory holding libdwritecore.so   (default build/)
#   LINUX_FONTS   where <font> lives here      (default /usr/share/fonts/windows)
#   WINDOWS_FONTS where <font> lives there     (default C:\Windows\Fonts)
#
# The cross build is static. A mingw build with a runtime dependency the guest
# does not have exits with 0xC0000135 and prints nothing at all, which reads
# exactly like a probe that ran and found nothing.
set -u

# An inherited LD_PRELOAD would put the shim in front of the probe's own
# FreeType and DirectWrite calls, which is not what is being measured here.
unset LD_PRELOAD

usage() { sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

[ $# -ge 2 ] || usage
case "$1" in -h|--help) usage ;; esac

FONT="$1"; GLYPHS="$2"; shift 2

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
LINUX_FONTS="${LINUX_FONTS:-/usr/share/fonts/windows}"
WINDOWS_FONTS="${WINDOWS_FONTS:-C:\\Windows\\Fonts}"
DOMAIN="${DOMAIN:-}"
STAGE_DIR="${STAGE_DIR:-}"
STAGE_URL="${STAGE_URL:-}"

SOURCE="$HERE/raster_probe.cpp"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/dwc-raster-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

command -v g++ >/dev/null || { echo "missing: g++"; exit 1; }
[ -f "$LINUX_FONTS/$FONT" ] || { echo "no font at $LINUX_FONTS/$FONT"; exit 1; }
[ -f "$BUILD/libdwritecore.so" ] || {
    echo "no libdwritecore.so in $BUILD - build it first, or set BUILD"; exit 1; }

# ---------------------------------------------------------------------------
# This machine
# ---------------------------------------------------------------------------
g++ -std=c++20 -O1 -I "$ROOT/include" -I "$ROOT/src" "$SOURCE" \
    -o "$WORK/raster_probe" -L "$BUILD" -ldwritecore -ldl -Wl,-rpath,"$BUILD" \
    2>"$WORK/build.log" || { cat "$WORK/build.log"; exit 1; }

"$WORK/raster_probe" "$LINUX_FONTS/$FONT" "$GLYPHS" "$@" > "$WORK/here.txt" || exit 1

if [ -z "$DOMAIN" ] || [ -z "$STAGE_DIR" ] || [ -z "$STAGE_URL" ]; then
    cat "$WORK/here.txt"
    echo
    echo "local side only: set DOMAIN, STAGE_DIR and STAGE_URL to compare" >&2
    exit 0
fi

# ---------------------------------------------------------------------------
# The guest
# ---------------------------------------------------------------------------
command -v x86_64-w64-mingw32-g++ >/dev/null || {
    echo "missing: x86_64-w64-mingw32-g++"; exit 1; }
[ -d "$STAGE_DIR" ] || { echo "no directory at STAGE_DIR=$STAGE_DIR"; exit 1; }

x86_64-w64-mingw32-g++ -std=c++17 -O1 "$SOURCE" -o "$WORK/raster_probe.exe" \
    -ldwrite -static -static-libgcc -static-libstdc++ \
    2>"$WORK/build-win.log" || { cat "$WORK/build-win.log"; exit 1; }

STAMP="$(sha256sum "$WORK/raster_probe.exe" | cut -c1-16)"
EXE="raster_probe.$STAMP.exe"
cp "$WORK/raster_probe.exe" "$STAGE_DIR/$EXE"

# Fetch only if this exact build is not there yet, then run it. Both halves are
# one guest command, because each round trip through the agent costs a second.
GUEST_ARGS="$WINDOWS_FONTS\\$FONT $GLYPHS $*"
python3 "$HERE/vmexec.py" "$DOMAIN" powershell -NoProfile -Command "
\$exe = Join-Path \$env:TEMP '$EXE'
if (-not (Test-Path \$exe)) {
    Invoke-WebRequest -UseBasicParsing '$STAGE_URL/$EXE' -OutFile \$exe
}
& \$exe $GUEST_ARGS
" 2>"$WORK/guest.log" | tr -d '\r' > "$WORK/there.txt"

if ! grep -q '^glyph' "$WORK/there.txt"; then
    echo "the guest produced no glyph lines:" >&2
    cat "$WORK/there.txt" "$WORK/guest.log" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# The comparison
# ---------------------------------------------------------------------------
grep '^glyph' "$WORK/here.txt"  > "$WORK/a.txt"
grep '^glyph' "$WORK/there.txt" > "$WORK/b.txt"

TOTAL="$(wc -l < "$WORK/a.txt")"
DIFFER="$(diff --unchanged-line-format= --old-line-format='%L' \
               --new-line-format= "$WORK/a.txt" "$WORK/b.txt" | wc -l)"

if [ "$DIFFER" -gt 0 ]; then
    echo "here                                                             there"
    diff --unchanged-line-format= --old-line-format='%L' --new-line-format= \
         "$WORK/a.txt" "$WORK/b.txt" | while read -r line; do
        glyph="$(printf '%s' "$line" | awk '{print $2}')"
        printf '%-64s %s\n' "$line" \
            "$(grep -m1 "^glyph *$glyph " "$WORK/b.txt" | sed 's/^glyph *[0-9]* *//')"
    done
fi
echo
echo "$((TOTAL - DIFFER)) of $TOTAL glyphs identical"
