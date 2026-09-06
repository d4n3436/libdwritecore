#!/usr/bin/env python3
"""
What each pixel difference between two captures is actually made of.

    tools/testing/attribute_diff.py <shots-dir> <tag> <page> <url>
                                   [--a host:port] [--b host:port] [--url-b URL]

compare_viewport.py says where the differences are. This says what they are.
Every element's box is taken from both machines and painted into a label map,
deepest last, so each differing pixel belongs to the innermost element that
covers it. Nothing is sampled: the counts add up to the whole difference, and
the pixels no element covers are counted too.

Both machines' maps are used, and the deeper of the two labels wins. An
element that moved covers different pixels on each side, and its glyphs at the
place only one side drew them would otherwise be charged to its parent, or to
nothing at all.

  font        the two sides drew the element with different faces, which the
              PostScript names catch, and the families differ too
  bold-sub    the same families drawing the same glyph counts, split across
              different faces, and either the element asked for a bold weight
              or the face names read as a regular and bold pair. This is the
              bold fallback substitution seen from above Blink. Windows names
              the bold face because Chromium picked it, and Linux names the
              regular one because the swap happens in the scaler, below the
              DOM. The label says the substitution is in play on these pixels,
              not that it failed. A difference here is usually the shaping
              ceiling, since Blink shapes with the regular face's tables
              either way. DWC_BOLD_FALLBACK=0 is the control
  share       same faces, different numbers of glyphs from each
  metrics     the element's own box differs, or something inside it moved
  raster      same faces, same box, nothing inside moved, and no substituted
              element close enough for its marks to have reached these pixels,
              so only the pixels differ
  chrome      no element covers the pixel: background, image, border or rule

The two machines reach the page server at different addresses, so --url-b
gives the second one its own URL.

The captures and the browsers have to be the same ones, so run this straight
after a sweep, before either side navigates away.
"""

import argparse
import json
import math
import os
import re
import sys

import numpy as np

import compare_viewport as cv
import viewport_protocol as vp

# Every element, with the box it occupies and what it was asked to be drawn
# with. The index is its position in document order, which is how the node is
# asked for again when the platform fonts are wanted.
SNAPSHOT = """
  const out = [];
  const all = document.querySelectorAll('*');
  const shown = new Map();
  for (let i = 0; i < all.length; ++i) {
    const e = all[i];
    const parent = e.parentElement ? (shown.get(e.parentElement) ?? -1) : -1;
    const r = e.getBoundingClientRect();
    if (r.width < 0.01 || r.height < 0.01 ||
        r.bottom < 0 || r.top > innerHeight || r.right < 0 || r.left > innerWidth) {
      shown.set(e, parent);
      continue;
    }
    shown.set(e, i);
    let depth = 0;
    for (let p = e; p; p = p.parentElement) ++depth;
    const cs = getComputedStyle(e);
    out.push([i, e.tagName, depth,
              +r.x.toFixed(2), +r.y.toFixed(2), +r.width.toFixed(2), +r.height.toFixed(2),
              cs.fontFamily.slice(0, 38), cs.fontSize, cs.fontWeight,
              (e.textContent || '').trim().slice(0, 28), parent]);
  }
  return JSON.stringify(out);
"""


def device_scale(browser):
    """Device pixels per CSS pixel on this side.

    getBoundingClientRect answers in CSS pixels and a capture is device
    pixels. At any scale but 1 the boxes land away from the ink they cover,
    so every differing pixel misses every box and the whole difference reads
    as `chrome`.
    """
    return float(browser.script("return window.devicePixelRatio;"))


def to_device(rows, scale):
    """The snapshot's rects, in the units the capture is in."""
    if scale != 1.0:
        for r in rows.values():
            for i in (3, 4, 5, 6):
                r[i] = r[i] * scale
    return rows


def snapshot(browser, url, scroll=0):
    browser.navigate(url)
    vp.await_condition(browser, vp.PAGE_LOADED, 60, "the page never finished loading")
    if scroll:
        browser.script(vp.SETTLE, [scroll])
        vp.await_condition(browser, vp.VIEWPORT_READY, 60,
                           "the viewport never settled after scrolling", [scroll])
    if browser.driver == "cdp":
        browser.call("DOM.enable")
        browser.call("CSS.enable")
        browser.call("DOM.getDocument", {"depth": -1})
    return {r[0]: r for r in json.loads(browser.script(SNAPSHOT))}


# What CSS.getPlatformFontsForNode answers on the DevTools side, from
# InspectorUtils.getUsedFontFaces. Firefox names the face and reports no glyph
# count, so the count comes back zero and the comparison rests on the names.
USED_FACES = """
const el = document.querySelectorAll('*')[JSON.parse(arguments[0])];
if (!el) { return '[]'; }
const range = document.createRange();
range.selectNodeContents(el);
const out = [];
const faces = InspectorUtils.getUsedFontFaces(range, 0, true);
for (let i = 0; i < faces.length; i++) {
  out.push([faces[i].name, faces[i].CSSFamilyName || '']);
}
return JSON.stringify(out);
"""


class FontQueryFailed(Exception):
    """The browser could not be asked which faces an element drew with.

    Distinct from an element that drew with none: an empty answer and a failed
    query would otherwise both read as "the two sides agree", which turns a
    dead DevTools session into a page of raster verdicts.
    """


def platform_fonts(browser, index):
    """The families an element really drew with, sorted so order cannot lie."""
    if browser.driver == "marionette":
        try:
            rows = json.loads(browser.m.script(USED_FACES, [json.dumps(index)],
                                               sandbox="system"))
        except RuntimeError as err:
            raise FontQueryFailed(str(err)) from err
        return sorted((family or name, name, 0) for name, family in rows)
    try:
        browser.script("window.__a = document.querySelectorAll('*')[%d];" % index)
        obj = browser.call("Runtime.evaluate",
                           {"expression": "window.__a"})["result"]
        if "objectId" not in obj:
            return []
        node = browser.call("DOM.requestNode",
                            {"objectId": obj["objectId"]})["nodeId"]
        fonts = browser.call("CSS.getPlatformFontsForNode",
                             {"nodeId": node}).get("fonts", [])
    except RuntimeError as err:
        # A node that went away between the snapshot and the query is the one
        # failure meaning "no fonts"; every other one means "cannot ask".
        if "Could not find node" in str(err):
            return []
        raise FontQueryFailed(str(err)) from err
    merged = {}
    for f in fonts:
        face = (f["familyName"], f.get("postScriptName", ""))
        merged[face] = merged.get(face, 0) + f.get("glyphCount", 0)
    return sorted((fam, ps, n) for (fam, ps), n in merged.items())


def label_map(rows, width, height):
    """Element index and its depth per pixel, innermost wins.

    The box is taken outwards to whole pixels. A box starting at x=100.6 is
    drawn into the pixel at 100, and rounding inwards would hand that pixel,
    with the antialiased left edge of the first glyph in it, to the parent.
    """
    labels = np.full((height, width), -1, np.int32)
    depths = np.zeros((height, width), np.int32)
    for r in sorted(rows.values(), key=lambda rec: rec[2]):
        x0, y0 = max(0, int(math.floor(r[3]))), max(0, int(math.floor(r[4])))
        x1 = min(width, int(math.ceil(r[3] + r[5])))
        y1 = min(height, int(math.ceil(r[4] + r[6])))
        if x1 > x0 and y1 > y0:
            labels[y0:y1, x0:x1] = r[0]
            depths[y0:y1, x0:x1] = r[2]
    return labels, depths


def show(fonts):
    return ", ".join("%s %d" % (fam, n) for fam, _, n in fonts) or "-"


def show_faces(fonts):
    """The same listing by face, for when the family names agree."""
    return ", ".join("%s %d" % (ps or fam, n) for fam, ps, n in fonts) or "-"


def size(row):
    """The element's font size in pixels, which bounds how far its ink reaches."""
    try:
        return float(row[8].removesuffix("px"))
    except ValueError:
        return 16.0


def weight(row):
    """The weight the element asked for. Computed style, so always a number."""
    try:
        return int(row[9])
    except ValueError:
        return 400


def by_family(fonts):
    """The same listing with each family's faces merged.

    A run split across a regular and a bold face of one family reads the
    same here as one that was not split, which is what tells a face swap
    apart from a different family being picked.
    """
    merged = {}
    for fam, _, n in fonts:
        merged[fam] = merged.get(fam, 0) + n
    return sorted(merged.items())


def bolder_faces(fa, fb):
    """Every face only one side names is the other's name with a suffix.

    Ebrima against Ebrima-Bold, NirmalaUI against NirmalaUI-Bold. Reading
    the names catches a container whose own weight is 400 while the bold
    text inside it is what differs, and it holds in both directions since
    an ancestor's listing merges a direct bold match with a fallback one.
    """
    def paired(name, others):
        return any(o != name and (name.startswith(o) or o.startswith(name))
                   for o in others)
    a = {ps for _, ps, _ in fa}
    b = {ps for _, ps, _ in fb}
    return bool(a ^ b) and all(paired(ps, b) for ps in a - b) \
        and all(paired(ps, a) for ps in b - a)


def substituted(a, fa, fb):
    """Whether a face difference is the bold substitution.

    The families and their glyph counts have to agree, so only the split
    across faces differs, and the run has to be bold: by the element's own
    weight, or by the names reading as a regular and bold pair.
    """
    return (by_family(fa) == by_family(fb)
            and (weight(a) >= 600 or bolder_faces(fa, fb)))


def classify(a, b, fa, fb, moved):
    if [(fam, ps) for fam, ps, _ in fa] != [(fam, ps) for fam, ps, _ in fb]:
        if by_family(fa) == by_family(fb):
            bold = weight(a) >= 600 or bolder_faces(fa, fb)
            return "bold-sub" if bold else "font", \
                   "%s vs %s" % (show_faces(fa), show_faces(fb))
        return "font", "%s vs %s" % (show(fa), show(fb))
    if fa != fb:
        return "share", "%s vs %s" % (show(fa), show(fb))
    if a[3:7] != b[3:7]:
        return "metrics", "rect %s vs %s" % (a[3:7], b[3:7])
    if moved is not None:
        ma, mb = moved
        return "metrics", "<%s> %r inside sits at %s vs %s" % (
            ma[1], ma[10], ma[3:7], mb[3:7])
    return "raster", "same faces and box, nothing inside moved"


def viewport_origin(base):
    """Where the viewport starts in this side's clean shot.

    A shot taken over CDP is the viewport and nothing else, and has no marked
    companion; one photographed off a screen carries the marker's position in
    the marked shot beside it.
    """
    marked = base + "_marked.png"
    if not os.path.exists(marked):
        return 0, 0
    x, y, _ = cv.origin(marked)
    return x, y


def side_base(shots, tag, page):
    """Where one side's screenshots are, under either name they are written by.

    compare_pages.sh gives every cell its own directory and names the shots
    after the side alone; capture_viewport.sh puts a whole run in one directory
    and adds the page to tell them apart. The page is dropped when nothing is
    written under it.
    """
    with_page = "%s/%s_%s" % (shots, tag, page)
    if os.path.exists(with_page + "_marked.png") or \
            os.path.exists(with_page + "_clean.png"):
        return with_page
    return "%s/%s" % (shots, tag)


def check_cell(shots, url, scroll):
    """Refuse a cell directory and a URL that name different pages.

    The cell and the URL arrive as independent arguments and nothing else ties
    them together, so cell1 with cell2's URL reopens the page that was named,
    walks that DOM and charges the other image's pixels to it. The answer looks
    like a finding. compare_pages.sh writes `<index> <path> <scroll>` per line
    beside the cells, which says what each one actually holds.
    """
    match = re.match(r"cell(\d+)$", os.path.basename(shots.rstrip("/")))
    if match is None:
        return
    index = match.group(1)
    listing = os.path.join(os.path.dirname(shots.rstrip("/")), "cells")
    if not os.path.exists(listing):
        return
    with open(listing) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2 or parts[0] != index:
                continue
            if parts[1] not in url:
                sys.exit("cell%s holds %s, but the url given is %s"
                         % (index, parts[1], url))
            if len(parts) > 2 and int(parts[2]) != scroll:
                sys.exit("cell%s was captured at scroll %s, not %d"
                         % (index, parts[2], scroll))
            return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shots")
    ap.add_argument("tag")
    ap.add_argument("page")
    ap.add_argument("url")
    ap.add_argument("--url-b", default=None)
    ap.add_argument("--a", default="127.0.0.1:9222",
                    help="[driver:]host:port of the first side's driver")
    ap.add_argument("--b", required=True,
                    help="[driver:]host:port of the second side's driver")
    ap.add_argument("--tag-b", default=None,
                    help="the second side's tag; the first plus W by default")
    ap.add_argument("--scroll", type=int, default=0,
                    help="scroll offset of the compared cell")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--limit", type=int, default=14)
    args = ap.parse_args()
    check_cell(args.shots, args.url, args.scroll)

    base = side_base(args.shots, args.tag, args.page)
    other = side_base(args.shots, args.tag_b or args.tag + "W", args.page)
    ax, ay = viewport_origin(base)
    bx, by = viewport_origin(other)
    a = cv.crop(base + "_clean.png", ax, ay, args.width, args.height).astype(int)
    b = cv.crop(other + "_clean.png", bx, by, args.width, args.height).astype(int)
    diff = (a != b).any(axis=2)
    total = int(diff.sum())
    print("%s: %d differing pixels, %.4f%% identical"
          % (args.page, total, 100 * (1 - total / (args.width * args.height))))
    if not total:
        return 0

    ba = vp.open_browser(args.a)
    bb = vp.open_browser(args.b)
    try:
        scale_a, scale_b = device_scale(ba), device_scale(bb)
        if scale_a != scale_b:
            print("the two sides are at %g and %g device pixels per CSS pixel; "
                  "compare them at one scale" % (scale_a, scale_b), file=sys.stderr)
            return 2
        if scale_a != 1.0:
            print("boxes taken to device pixels at %g" % scale_a)
        rows_a = to_device(snapshot(ba, args.url, args.scroll), scale_a)
        rows_b = to_device(snapshot(bb, args.url_b or args.url, args.scroll), scale_b)

        # The tree, from the recorded-ancestor field, so a box whose own rect
        # agrees can still be caught holding something that moved.
        children = {}
        for r in rows_a.values():
            children.setdefault(r[11], []).append(r[0])

        def moved_inside(node, y_range):
            """A descendant that sits differently, in the rows that differ.

            Text after a run that changed width shifts along with it while
            its own container keeps its box, so the container's own rect
            proves nothing. Only descendants overlapping the differing rows
            count, since a block that moved at the other end of a long
            container did not put these pixels where they are.
            """
            lo_y, hi_y = y_range
            stack = list(children.get(node, []))
            while stack:
                j = stack.pop()
                stack.extend(children.get(j, []))
                ja, jb = rows_a.get(j), rows_b.get(j)
                if ja is None or jb is None or ja[3:7] == jb[3:7]:
                    continue
                if min(ja[4], jb[4]) <= hi_y and max(ja[4] + ja[6], jb[4] + jb[6]) >= lo_y:
                    return ja, jb
            return None

        labels_a, depths_a = label_map(rows_a, args.width, args.height)
        labels_b, depths_b = label_map(rows_b, args.width, args.height)
        inner = np.where(depths_b > depths_a, labels_b, labels_a)

        hit = inner[diff]
        rows_of, cols_of = np.nonzero(diff)
        counts = {int(i): int(n) for i, n in zip(*np.unique(hit, return_counts=True))}
        chrome = counts.pop(-1, 0)

        # The rows each element's own differing pixels fall in.
        uniq, index_of = np.unique(hit, return_inverse=True)
        lo = np.full(uniq.size, 1 << 30, np.int64)
        hi = np.full(uniq.size, -1, np.int64)
        cl = np.full(uniq.size, 1 << 30, np.int64)
        cr = np.full(uniq.size, -1, np.int64)
        np.minimum.at(lo, index_of, rows_of)
        np.maximum.at(hi, index_of, rows_of)
        np.minimum.at(cl, index_of, cols_of)
        np.maximum.at(cr, index_of, cols_of)
        band = {int(u): (int(l), int(h)) for u, l, h in zip(uniq, lo, hi)}
        span = {int(u): (int(l), int(r)) for u, l, r in zip(uniq, cl, cr)}

        kinds = {}
        detail = []
        # Only the elements a difference lands on are asked about, so the round
        # trips follow the damage and not the size of the page.
        for index in sorted(counts, key=lambda i: -counts[i]):
            ra, rb = rows_a.get(index), rows_b.get(index)
            if ra is None or rb is None:
                kinds["only-one-side"] = kinds.get("only-one-side", 0) + counts[index]
                continue
            try:
                fa, fb = platform_fonts(ba, index), platform_fonts(bb, index)
            except FontQueryFailed as err:
                raise SystemExit("the browsers could not be asked which faces "
                                 "drew element %d: %s\nEvery verdict below it "
                                 "would read as raster, so nothing is reported. "
                                 "Recapture against the browsers that are "
                                 "running now." % (index, err)) from err
            kind, why = classify(ra, rb, fa, fb, moved_inside(index, band[index]))
            detail.append([counts[index], kind, why, ra, index, fa, fb])

        # Devanagari and Thai marks sit above the inline box and reach past
        # its edges, so a substituted run's own differing pixels can land on
        # an element that is not its ancestor and whose faces agree. Once
        # everything is classified, a raster verdict is withdrawn if a
        # substituted element's box, grown by its font size, reaches the
        # pixels, that being how far a glyph's ink can go.
        def reaches(rec, lo_y, hi_y, x_left, x_right):
            reach = size(rec)
            return (rec[4] - reach <= hi_y and rec[4] + rec[6] + reach >= lo_y
                    and rec[3] - reach <= x_right and rec[3] + rec[5] + reach >= x_left)

        known = {row[4]: (row[3], row[5], row[6]) for row in detail}
        for row in detail:
            if row[1] != "raster":
                continue
            lo, hi = band[row[4]]
            left, right = span[row[4]]
            # Nearest first, so the element whose ink most plausibly reached
            # these pixels is the one asked about, and the search stops there.
            near = sorted((r for j, r in rows_a.items()
                           if j != row[4] and j in rows_b and reaches(r, lo, hi, left, right)),
                          key=lambda rec: rec[5] * rec[6])
            for ra_near in near[:8]:
                j = ra_near[0]
                if j in known:
                    _, fa, fb = known[j]
                else:
                    fa, fb = platform_fonts(ba, j), platform_fonts(bb, j)
                    known[j] = (ra_near, fa, fb)
                if fa != fb and substituted(ra_near, fa, fb):
                    row[1] = "bold-sub"
                    row[2] = ("<%s> %r beside it is substituted and its marks reach "
                              "past its box (%s vs %s)"
                              % (ra_near[1], ra_near[10], show_faces(fa), show_faces(fb)))
                    break

        for n, kind, _, _, _, _, _ in detail:
            kinds[kind] = kinds.get(kind, 0) + n
        if chrome:
            kinds["chrome"] = kinds.get("chrome", 0) + chrome

        for n, kind, why, ra, _, _, _ in detail[:args.limit]:
            print("  %6d px  <%s> %r  %s %s  %s" % (n, ra[1], ra[10], ra[8], ra[9], ra[7]))
            print("            %-8s %s" % (kind, why))
        if chrome:
            print("  %6d px  chrome   no element covers these" % chrome)
        seen = sum(kinds.values())
        print("  by kind: " + ", ".join("%s %d px" % kv for kv in sorted(kinds.items()))
              + "   (%d of %d px, %.2f%%)" % (seen, total, 100 * seen / total))
    finally:
        ba.close()
        bb.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
