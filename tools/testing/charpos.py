#!/usr/bin/env python3
"""Per-character layout positions on two browsers, diffed.

    charpos.py <sideA> <sideB> [--family F] [--size N] [--style S] [--weight W]
    charpos.py <sideA> <sideB> --page <url> [--settle S]

A side is `[driver:]host:port`, `cdp` for Chromium and Electron or
`marionette` for Firefox, and cdp when none is named.

compare_pages says which pixels differ; this says whether the glyphs were put
in the same place. Each character's box comes from a Range rect, which is the
layout position before any subpixel bucketing, so a difference here is
advances and metrics, not rasterization, and an exact match sends the search
to the rasterizer.

The first form styles one probe string. --page instead loads a whole page on
both sides and walks every text node character, which localizes a divergence
to the exact characters that moved.
"""
import argparse
import json
import sys
import time

PROBE_TEXT = "italic em i tag normally incorrect vertical not shown as small caps"

PROBE = r"""
const [text, family, size, style, weight] = arguments;
document.body.innerHTML = '';
document.body.style.margin = '0';
const p = document.createElement('p');
p.style.cssText = 'margin:0;font-family:' + family + ';font-size:' + size +
                  'px;font-style:' + style + ';font-weight:' + weight +
                  ';white-space:pre';
p.textContent = text;
document.body.appendChild(p);
const node = p.firstChild;
const out = [];
for (let i = 0; i < text.length; ++i) {
  const r = document.createRange();
  r.setStart(node, i); r.setEnd(node, i + 1);
  const b = r.getBoundingClientRect();
  out.push(['0#' + i, text[i], b.left, b.top, b.width, b.height]);
}
return JSON.stringify(out);
"""

WALK = r"""
const out = [];
const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
let node, ni = 0;
while ((node = walker.nextNode())) {
  const text = node.data;
  if (!text.trim()) { ni++; continue; }
  const r = document.createRange();
  for (let i = 0; i < text.length; i++) {
    r.setStart(node, i); r.setEnd(node, i + 1);
    const b = r.getBoundingClientRect();
    // A collapsed space has a zero box and says nothing; one carrying an
    // advance is a row like any other, and is where an advance divergence
    // often sits.
    if (b.width === 0 && b.height === 0) continue;
    out.push([ni + '#' + i, text[i], b.left, b.top, b.width, b.height]);
  }
  ni++;
}
return JSON.stringify(out);
"""


class Side:
    def __init__(self, spec):
        parts = spec.split(":")
        if len(parts) == 3:
            self.driver, host, port = parts
        elif len(parts) == 2:
            self.driver, (host, port) = "cdp", parts
        else:
            raise SystemExit("side is [driver:]host:port, got %r" % spec)
        if self.driver == "cdp":
            import viewport_protocol as vp
            self.browser = vp.CdpBrowser(host, int(port))
        elif self.driver == "marionette":
            from marionette import Marionette
            self.browser = Marionette(host, int(port))
            self.browser.start()
        else:
            sys.exit("driver is cdp or marionette, got %r" % self.driver)

    def navigate(self, url):
        if self.driver == "cdp":
            self.browser.call("Page.navigate", {"url": url})
        else:
            self.browser.call("WebDriver:Navigate", {"url": url})

    def script(self, body, args=()):
        return self.browser.script(body, args)


def rows(raw):
    return {r[0]: r for r in json.loads(raw)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a", metavar="sideA")
    ap.add_argument("b", metavar="sideB")
    ap.add_argument("--family", default="serif")
    ap.add_argument("--size", type=float, default=16)
    ap.add_argument("--style", default="normal")
    ap.add_argument("--weight", default="400")
    ap.add_argument("--page", help="load this URL on both sides and walk it "
                                   "instead of styling the probe string")
    ap.add_argument("--prefixes", nargs=2, metavar=("A", "B"),
                    help="per-side URL prefixes put before --page, for a "
                         "side that reaches the page server by another "
                         "address")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="seconds to wait after navigating, for fonts")
    ap.add_argument("--tolerance", type=float, default=0.001,
                    help="difference below this is agreement")
    args = ap.parse_args()

    sides = [Side(args.a), Side(args.b)]
    answers = []
    for n, side in enumerate(sides):
        if args.page:
            prefix = args.prefixes[n] if args.prefixes else ""
            side.navigate(prefix + args.page)
            time.sleep(args.settle)
            answers.append(rows(side.script(WALK)))
        else:
            answers.append(rows(side.script(
                PROBE, [PROBE_TEXT, args.family, args.size, args.style,
                        args.weight])))

    a, b = answers
    keys = [k for k in a if k in b]
    bad = 0
    worst = 0.0
    for k in keys:
        deltas = [abs(x - y) for x, y in zip(a[k][2:], b[k][2:])]
        if max(deltas) <= args.tolerance:
            continue
        bad += 1
        worst = max(worst, max(deltas))
        if bad <= 15:
            print("  %-8s %-4r left %.2f vs %.2f  top %.2f vs %.2f  "
                  "size %.2fx%.2f vs %.2fx%.2f"
                  % (k, a[k][1], a[k][2], b[k][2], a[k][3], b[k][3],
                     a[k][4], a[k][5], b[k][4], b[k][5]))
    only = (len(a) - len(keys)) + (len(b) - len(keys))
    print("%d of %d characters differ, worst %.4f px%s"
          % (bad, len(keys), worst,
             ", %d only on one side" % only if only else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
