#!/usr/bin/env python3
"""
Compare two screenshots taken by capture_viewport.sh, whatever page they show.

    tools/testing/compare_viewport.py <width> <height> \
        <a_marked.png> <a_clean.png> <b_marked.png> <b_clean.png> \
        [--diff out.png] [--bands N] [--rows LO,HI] [--clusters N]
    tools/testing/compare_viewport.py <width> <height> <a.png> <b.png> [...]
    tools/testing/compare_viewport.py --marker <shot.png>
    tools/testing/compare_viewport.py --fonts <font-dir>

Whole-screen shots do not line up: different window chrome, different window
position, different screen. So this does not diff them directly. Each marked
shot carries an eight-pixel magenta square at the viewport origin; this finds
it on each side, crops width x height from there in both clean shots, and
compares those.

The four-argument form takes shots that are already the viewport, as
Page.captureScreenshot returns them, and compares the pair with no marker
and no crop.

capture_viewport.py draws that marker on whatever page it is given, so this
works on any page the browser can load.

--fonts is the precondition, not the comparison: a pixel difference means
nothing until both machines are shown to have the same font files. Run it on
each side and compare the two listings before trusting any number below.

What the numbers mean:

    identical pixels    Byte-for-byte agreement over the whole viewport. On a
                        page that is mostly background this flatters; read it
                        together with the differing rows and columns, which say
                        whether the disagreement is one line or the whole page.
    max |diff|          The largest single-channel difference. A handful of
                        pixels differing by 1 is a rounding tie; anything in the
                        tens is a different glyph, a different font, or a
                        layout shift.
    differing rows      A layout shift shows up as a large, contiguous count.
                        Rasterization differences scatter.

--rows narrows the comparison to a band of viewport rows, which is what to
reach for once --bands has found a layout offset: everything below it counts as
different for that reason alone, and the region above it is the part where the
rasterization can still be judged.

--clusters answers "what disagrees", once --bands has said it is not a layout
offset. Differing pixels are grouped into runs of adjacent rows, which is how
lines of text separate, and each run is reported with its bounding box, its
pixel count and its worst channel. A run a few rows tall and a few hundred
columns wide is one line of text; one that is two pixels tall and four wide is
a single antialiased edge and is not worth chasing. Feed the row numbers back
to linebox_sweep.py --probe to find out which element it is.

--bands answers the question that always comes next: is this a layout
difference or a rasterization one? It splits the viewport into horizontal bands
and, for each, reports the vertical shift that aligns it best. A band whose
best shift is zero disagrees about pixels; one that jumps to 100% at a shift of
one or two disagrees about where the content is. The row where the best shift
changes is where the layout diverged.

Needs pillow and numpy.
"""

import os
import struct
import sys

import numpy as np
from PIL import Image


# The marker's two colors, off the primaries on purpose. An exact match would
# fail wherever a capture path shifts a channel, and a wide tolerance is a
# range a page can paint inside, so the values are odd and the window is small.
MARKER_A = (253, 3, 251)
MARKER_B = (3, 251, 3)
MARKER_TOLERANCE = 2


def marker_colors(a):
    """Masks of the marker's two colors in an RGB array."""
    def near(rgb):
        return ((np.abs(a[:, :, 0] - rgb[0]) <= MARKER_TOLERANCE) &
                (np.abs(a[:, :, 1] - rgb[1]) <= MARKER_TOLERANCE) &
                (np.abs(a[:, :, 2] - rgb[2]) <= MARKER_TOLERANCE))
    return near(MARKER_A), near(MARKER_B)


def origin(path):
    """Top-left pixel of the viewport marker.

    The marker is a magenta and green checker rather than a flat fill, so a
    magenta pixel alone does not identify it: a page painting `background:
    fuchsia` would otherwise take the corner whenever its area sits further up
    or left. A magenta pixel counts only where green sits four pixels along,
    which is the checker's period.
    """
    a = np.asarray(Image.open(path).convert("RGB")).astype(int)
    magenta, green = marker_colors(a)
    # Green four columns right of a magenta pixel, and inside the image.
    paired = magenta.copy()
    paired[:, :-4] &= green[:, 4:]
    paired[:, -4:] = False
    ys, xs = np.nonzero(paired)
    if len(xs) == 0:
        sys.exit("no viewport marker in " + path)
    return int(xs.min()), int(ys.min()), int(magenta.sum())


def marker_state(path):
    """Whether a screenshot has the viewport marker in it, for the caller.

    The marker in the photograph proves which side of the DOM change the
    frame is on: if it is there, the frame is after the mark; if it is gone,
    the frame is after the removal. So the shot is simply retaken until the
    answer is right, which is normally the first one.

    "uniform" is the other thing worth knowing and comes free: a screenshot
    with one color in it is the blank-framebuffer failure - no window manager,
    or a browser that connected to a different display - and it has no marker
    either, so without this check it would read as a perfectly good clean shot.
    """
    a = np.asarray(Image.open(path).convert("RGB")).astype(int)
    # The checker, not one color; see origin() for why a flat test is wrong.
    magenta, green = marker_colors(a)
    found = magenta.copy()
    found[:, :-4] &= green[:, 4:]
    found[:, -4:] = False
    ys, xs = np.nonzero(found)
    if len(xs) == 0:
        corners = a[::max(1, a.shape[0] // 8), ::max(1, a.shape[1] // 8)]
        if len(np.unique(corners.reshape(-1, 3), axis=0)) <= 1:
            return "uniform"
        return "none"
    return "marker %d %d %d" % (int(xs.min()), int(ys.min()), len(xs))


def crop(path, x, y, w, h):
    a = np.asarray(Image.open(path).convert("RGB")).astype(int)
    out = a[y:y + h, x:x + w]
    if out.shape[0] != h or out.shape[1] != w:
        sys.exit("%s: viewport runs off the screenshot at (%d,%d)" % (path, x, y))
    return out


def bands(a, b, height, count=12, limit=3):
    """Best vertical alignment per horizontal band."""
    step = max(1, height // count)
    print("band    rows          best shift   at best     unshifted")
    for lo in range(0, height - step + 1, step):
        hi = lo + step
        best = None
        for dy in range(-limit, limit + 1):
            if lo + dy < 0 or hi + dy > height:
                continue
            score = (np.abs(a[lo:hi] - b[lo + dy:hi + dy]).max(axis=2) == 0).mean()
            if best is None or score > best[1]:
                best = (dy, score)
        flat = (np.abs(a[lo:hi] - b[lo:hi]).max(axis=2) == 0).mean()
        note = "" if best[0] == 0 else "   <- content is offset here"
        print("     %5d..%-5d  %+3d      %8.3f%%   %8.3f%%%s"
              % (lo, hi, best[0], 100 * best[1], 100 * flat, note))


def clusters(per_pixel, limit=12, gap=2):
    """The differing pixels, grouped into runs of adjacent rows."""
    rows = np.nonzero(per_pixel.max(axis=1))[0].tolist()
    if not rows:
        return
    runs = [[rows[0], rows[0]]]
    for r in rows[1:]:
        if r - runs[-1][1] <= gap:
            runs[-1][1] = r
        else:
            runs.append([r, r])
    scored = []
    for lo, hi in runs:
        band = per_pixel[lo:hi + 1]
        cols = np.nonzero(band.max(axis=0))[0]
        scored.append((int((band > 0).sum()), lo, hi,
                       int(cols.min()), int(cols.max()), int(band.max())))
    scored.sort(reverse=True)
    print("%d cluster(s), largest first:" % len(scored))
    for n, lo, hi, c0, c1, worst in scored[:limit]:
        print("   rows %4d..%-4d cols %4d..%-4d  %6d px  max %3d"
              % (lo, hi, c0, c1, n, worst))
    if len(scored) > limit:
        print("   ... %d more" % (len(scored) - limit))


def font_identity(root):
    """Print name/version and head.fontRevision for the fonts that matter, so
    the two machines can be compared before any rendering is."""
    interesting = ["arial.ttf", "times.ttf", "cour.ttf", "segoeui.ttf",
                   "SegUIVar.ttf", "Nirmala.ttc", "YuGothM.ttc", "malgun.ttf",
                   "msjh.ttc", "msyh.ttc", "LeelawUI.ttf", "seguiemj.ttf"]
    print("%-16s %-30s %s" % ("file", "version", "head.fontRevision"))
    for name in interesting:
        path = os.path.join(root, name)
        if not os.path.exists(path):
            print("%-16s %s" % (name, "(not installed)"))
            continue
        try:
            version, revision = read_font_version(path)
            print("%-16s %-30s %s" % (name, version, revision))
        except Exception as exc:
            print("%-16s error: %s" % (name, exc))


def read_font_version(path):
    with open(path, "rb") as handle:
        data = handle.read()
    base = struct.unpack_from(">I", data, 12)[0] if data[:4] == b"ttcf" else 0
    count = struct.unpack_from(">H", data, base + 4)[0]
    tables = {}
    for i in range(count):
        rec = base + 12 + 16 * i
        tables[data[rec:rec + 4].decode("latin1")] = struct.unpack_from(">II", data, rec + 8)

    version = "?"
    if "name" in tables:
        off = tables["name"][0]
        n, string_off = struct.unpack_from(">HH", data, off + 2)
        for i in range(n):
            rec = off + 6 + 12 * i
            pid, _, _, nid, length, str_at = struct.unpack_from(">HHHHHH", data, rec)
            if nid == 5:
                raw = data[off + string_off + str_at:off + string_off + str_at + length]
                # Platform 0 (Unicode) and 3 (Windows) are both UTF-16BE;
                # only platform 1 (Macintosh) is a byte encoding. Reading a
                # platform-0 record as latin1 is what produced "V e r s i o n".
                version = (raw.decode("utf-16-be", "replace") if pid in (0, 3)
                           else raw.decode("latin1")).strip()
                break
    revision = "?"
    if "head" in tables:
        revision = struct.unpack_from(">i", data, tables["head"][0] + 4)[0] / 65536.0
    return version, revision

def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "--fonts":
        font_identity(args[1])
        return 0
    if len(args) == 2 and args[0] == "--marker":
        state = marker_state(args[1])
        print(state)
        return 0 if state.startswith("marker") else 1
    diff_path = None
    row_range = None
    if "--rows" in args:
        i = args.index("--rows")
        lo, _, hi = args[i + 1].partition(",")
        row_range = (int(lo), int(hi))
        del args[i + 1]
        del args[i]
    want_bands = "--bands" in args
    band_count = 12
    if want_bands:
        i = args.index("--bands")
        if i + 1 < len(args) and args[i + 1].isdigit():
            band_count = int(args[i + 1])
            del args[i + 1]
        del args[i]
    want_clusters = "--clusters" in args
    cluster_count = 12
    if want_clusters:
        i = args.index("--clusters")
        if i + 1 < len(args) and args[i + 1].isdigit():
            cluster_count = int(args[i + 1])
            del args[i + 1]
        del args[i]
    if "--diff" in args:
        i = args.index("--diff")
        diff_path = args[i + 1]
        args = args[:i] + args[i + 2:]
    if len(args) not in (4, 6):
        sys.exit(__doc__.strip())

    w, h = int(args[0]), int(args[1])
    if len(args) == 4:
        # Both shots are already the viewport, so there is no marker to find
        # and nothing to crop away.
        ax = ay = bx = by = 0
        a_clean, b_clean = args[2], args[3]
        print("direct capture %dx%d" % (w, h))
    else:
        ax, ay, an = origin(args[2])
        bx, by, bn = origin(args[4])
        print("marker A (%d,%d) %d px   B (%d,%d) %d px" % (ax, ay, an, bx, by, bn))
        if an != bn:
            print("  note: the marker is a different size on the two sides, which "
                  "means display scaling differs and the comparison is not valid")
        a_clean, b_clean = args[3], args[5]

    a = crop(a_clean, ax, ay, w, h)
    b = crop(b_clean, bx, by, w, h)

    if row_range is not None:
        lo, hi = row_range
        print("comparing rows %d..%d only" % (lo, hi))
        a, b = a[lo:hi], b[lo:hi]

    d = np.abs(a - b)
    per_pixel = d.max(axis=2)
    total = per_pixel.size
    same = int((per_pixel == 0).sum())
    print("identical pixels %d/%d = %.4f%%" % (same, total, 100.0 * same / total))
    print("mean |diff| %.4f   max |diff| %d" % (d.mean(), int(d.max())))

    if same != total:
        rows = np.nonzero(per_pixel.max(axis=1))[0]
        cols = np.nonzero(per_pixel.max(axis=0))[0]
        print("differing rows %d (first %s)" % (len(rows), rows[:6].tolist()))
        print("differing cols %d" % len(cols))
        hist = np.bincount(per_pixel[per_pixel > 0].ravel())
        print("diff histogram:",
              [(i, int(c)) for i, c in enumerate(hist) if c][:12])
        if want_bands:
            bands(a, b, h, band_count)
        if want_clusters:
            clusters(per_pixel, cluster_count)
        if diff_path:
            Image.fromarray((per_pixel * 8).clip(0, 255).astype(np.uint8)).save(diff_path)
            print("wrote", diff_path)

    return 0 if same == total else 1


if __name__ == "__main__":
    sys.exit(main())
