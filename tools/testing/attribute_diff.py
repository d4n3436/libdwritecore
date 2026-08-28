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

  font        the two sides drew the element with different families
  share       same families, different numbers of glyphs from each
  metrics     same families, but the element sits or measures differently
  raster      same families and the same box, so only the pixels differ
  chrome      no element covers the pixel: background, image, border or rule

The two machines reach the page server at different addresses, so --url-b
gives the second one its own URL.

The captures and the browsers have to be the same ones, so run this straight
after a sweep, before either side navigates away.
"""

import argparse
import json
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
  for (let i = 0; i < all.length; ++i) {
    const e = all[i];
    const r = e.getBoundingClientRect();
    if (r.width < 0.01 || r.height < 0.01) continue;
    if (r.bottom < 0 || r.top > innerHeight || r.right < 0 || r.left > innerWidth) continue;
    let depth = 0;
    for (let p = e; p; p = p.parentElement) ++depth;
    const cs = getComputedStyle(e);
    out.push([i, e.tagName, depth,
              +r.x.toFixed(2), +r.y.toFixed(2), +r.width.toFixed(2), +r.height.toFixed(2),
              cs.fontFamily.slice(0, 38), cs.fontSize, cs.fontWeight,
              (e.textContent || '').trim().slice(0, 28)]);
  }
  return JSON.stringify(out);
"""


def endpoint(text):
    host, _, port = text.partition(":")
    return host, int(port or 9222)


def snapshot(browser, url):
    browser.navigate(url)
    browser.call("DOM.enable")
    browser.call("CSS.enable")
    browser.call("DOM.getDocument", {"depth": -1})
    return {r[0]: r for r in json.loads(browser.script(SNAPSHOT))}


def platform_fonts(browser, index):
    """The families an element really drew with, sorted so order cannot lie."""
    browser.script("window.__a = document.querySelectorAll('*')[%d];" % index)
    obj = browser.call("Runtime.evaluate", {"expression": "window.__a"})["result"]
    if "objectId" not in obj:
        return []
    node = browser.call("DOM.requestNode", {"objectId": obj["objectId"]})["nodeId"]
    try:
        fonts = browser.call("CSS.getPlatformFontsForNode", {"nodeId": node}).get("fonts", [])
    except RuntimeError:
        return []
    return sorted((f["familyName"], f.get("glyphCount", 0)) for f in fonts)


def label_map(rows, width, height):
    """Element index per pixel, innermost wins."""
    labels = np.full((height, width), -1, np.int32)
    for r in sorted(rows.values(), key=lambda r: r[2]):
        x0, y0 = max(0, int(round(r[3]))), max(0, int(round(r[4])))
        x1 = min(width, int(round(r[3] + r[5])))
        y1 = min(height, int(round(r[4] + r[6])))
        if x1 > x0 and y1 > y0:
            labels[y0:y1, x0:x1] = r[0]
    return labels


def show(fonts):
    return ", ".join("%s %d" % p for p in fonts) or "-"


def classify(a, b, fa, fb):
    if [n for n, _ in fa] != [n for n, _ in fb]:
        return "font", "%s vs %s" % (show(fa), show(fb))
    if fa != fb:
        return "share", "%s vs %s" % (show(fa), show(fb))
    if a[3:7] != b[3:7]:
        return "metrics", "rect %s vs %s" % (a[3:7], b[3:7])
    return "raster", "same families and box"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shots")
    ap.add_argument("tag")
    ap.add_argument("page")
    ap.add_argument("url")
    ap.add_argument("--url-b", default=None)
    ap.add_argument("--a", default="127.0.0.1:9222")
    ap.add_argument("--b", default="192.168.122.206:9223")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--limit", type=int, default=14)
    args = ap.parse_args()

    base = "%s/%s_%s" % (args.shots, args.tag, args.page)
    other = "%s/%sW_%s" % (args.shots, args.tag, args.page)
    ax, ay, _ = cv.origin(base + "_marked.png")
    bx, by, _ = cv.origin(other + "_marked.png")
    a = cv.crop(base + "_clean.png", ax, ay, args.width, args.height).astype(int)
    b = cv.crop(other + "_clean.png", bx, by, args.width, args.height).astype(int)
    diff = (a != b).any(axis=2)
    total = int(diff.sum())
    print("%s: %d differing pixels, %.4f%% identical"
          % (args.page, total, 100 * (1 - total / (args.width * args.height))))
    if not total:
        return 0

    ba = vp.CdpBrowser(*endpoint(args.a))
    bb = vp.CdpBrowser(*endpoint(args.b))
    try:
        rows_a = snapshot(ba, args.url)
        rows_b = snapshot(bb, args.url_b or args.url)

        hit = label_map(rows_a, args.width, args.height)[diff]
        counts = {int(i): int(n) for i, n in zip(*np.unique(hit, return_counts=True))}
        chrome = counts.pop(-1, 0)

        kinds = {}
        detail = []
        # Only the elements a difference lands on are asked about, so the round
        # trips follow the damage and not the size of the page.
        for index in sorted(counts, key=lambda i: -counts[i]):
            ra, rb = rows_a.get(index), rows_b.get(index)
            if ra is None or rb is None:
                kinds["only-one-side"] = kinds.get("only-one-side", 0) + counts[index]
                continue
            kind, why = classify(ra, rb, platform_fonts(ba, index), platform_fonts(bb, index))
            kinds[kind] = kinds.get(kind, 0) + counts[index]
            detail.append((counts[index], kind, why, ra))
        if chrome:
            kinds["chrome"] = kinds.get("chrome", 0) + chrome

        for n, kind, why, ra in detail[:args.limit]:
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
