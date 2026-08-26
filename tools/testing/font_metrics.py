#!/usr/bin/env python3
"""
Report the font metrics a browser computed, through canvas TextMetrics.

    tools/testing/font_metrics.py <host> <port> [--against <host> <port>]
                                  [--font "MS Gothic"] [--sizes 13,24] [--layout|--underline]

TextMetrics is the only window onto gfxFont::Metrics that content has, and it
is enough for the numbers that decide where a line and its decorations land:

    emHeightAscent / emHeightDescent            emAscent / emDescent
    fontBoundingBoxAscent / fontBoundingBoxDescent   maxAscent / maxDescent

Which matters because gfxFont::SanitizeMetrics rebuilds the underline offset
out of them for the CJK families on Firefox's bad-underline list, instead of
believing the font's own post table:

    underlineOffset = min(underlineOffset, -2.0)
    underlineOffset = min(underlineOffset, -emDescent)                  ... or
    underlineOffset = min(underlineOffset, underlineSize - emDescent)

so a font whose emDescent is a fraction of a pixel off on one platform draws
its underline on a different row there, while every glyph above it is
identical. That is a difference no glyph comparison can see and this can.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from marionette import Marionette

# What *layout* uses, which is not what TextMetrics reports.
#
# nsFontMetrics rounds the font's metrics before layout ever sees them, and
# folds the underline into the descent so a decoration cannot be drawn outside
# the line box (gfx/src/nsFontMetrics.cpp, and the comment there that this must
# stay in step with nsCSSRendering::GetTextDecorationRectInternal):
#
#   ComputeMaxAscent  = floor(maxAscent + 0.5)
#   ComputeMaxDescent = floor(max(minDescent, maxDescent) + 0.5)
#     where minDescent = floor(-fontGroup->GetUnderlineOffset() + 0.5)
#                        + NS_round(underlineSize)
#
# so two fonts whose maxAscent differs by half a pixel can round to the same
# layout ascent, and a font whose underline sits lower gets a taller line box
# and not a clipped rule. Neither is visible in TextMetrics.
#
# With line-height:normal the line box is exactly MaxAscent + MaxDescent and
# the baseline is exactly MaxAscent, so an empty zero-size inline-block - which
# sits on the baseline by definition - reads both back out of layout.
LAYOUT = """
const out = {};
const host = document.createElement('div');
document.body.appendChild(host);
for (const fam of arguments[0]) {
  for (const size of arguments[1]) {
    host.style.cssText = 'font: ' + size + 'px "' + fam + '"; line-height: normal;'
                       + 'white-space: pre; margin: 0; padding: 0;';
    // An inline box's content area is exactly MaxAscent + MaxDescent - the
    // rounded pair above - while the block around it is subject to
    // line-height. So measure the inline, and take the baseline off a
    // zero-size inline-block, which sits on it by definition.
    host.innerHTML = '<span id="i">Hxg</span>'
                   + '<span id="b" style="display:inline-block;width:0;'
                   + 'height:0;vertical-align:baseline"></span>';
    const i = host.querySelector('#i').getBoundingClientRect();
    const b = host.querySelector('#b').getBoundingClientRect();
    out[fam + ' ' + size] = {ascent: b.top - i.top, height: i.height};
  }
}
host.remove();
return out;
"""

PROBE = """
const out = {};
const c = document.createElement('canvas');
const g = c.getContext('2d');
for (const fam of arguments[0]) {
  for (const size of arguments[1]) {
    g.font = size + 'px "' + fam + '"';
    const m = g.measureText('Hxg');
    out[fam + ' ' + size] = {
      emAsc: m.emHeightAscent, emDesc: m.emHeightDescent,
      boxAsc: m.fontBoundingBoxAscent, boxDesc: m.fontBoundingBoxDescent,
      alphaBase: m.alphabeticBaseline, width: m.width, font: g.font,
    };
  }
}
return out;
"""


# Where the underline actually lands, in device pixels, straight out of the
# browser's own screenshot.
#
# nsCSSRendering::GetTextDecorationRectInternal places it at
#
#   baseline = floor(bCoord + ascent + 0.5)
#   r.y      = baseline - floor(offset + 0.5)
#
# with offset = gfxFontGroup::GetUnderlineOffset(), the min of the first valid
# font's underlineOffset and any bad-underline font's, after
# gfxFont::SanitizeMetrics has had its way with it. None of that is reachable
# from content - but the painted row is, and the baseline beside it, so their
# difference is the only unknown and comes back exactly.
#
# Marionette takes the screenshot itself, so this needs no X server, no guest
# display and no marker: the same code measures the browser here and the one
# in the VM.
UNDERLINE = """
document.body.style.cssText = 'margin:0;background:#fff;color:#000';
document.body.innerHTML = '';
const row = document.createElement('div');
row.style.cssText = 'font: ' + arguments[1] + 'px "' + arguments[0] + '";'
                  + 'line-height: 3em; white-space: pre;'
                  + 'text-decoration: underline; margin: 0;';
row.innerHTML = '<span id="t">HHHHHHHH</span>'
              + '<span id="b" style="display:inline-block;width:0;height:0;'
              + 'vertical-align:baseline;text-decoration:none"></span>';
document.body.appendChild(row);
const t = row.querySelector('#t').getBoundingClientRect();
const b = row.querySelector('#b').getBoundingClientRect();
return {left: t.left, right: t.right, bottom: row.getBoundingClientRect().bottom,
        baseline: b.top};
"""


def measure(host, port, fonts, sizes, layout=False):
    m = Marionette(host, int(port), timeout=60)
    m.start("content")
    m.call("WebDriver:Navigate", {"url": "about:blank"})
    return m.script(LAYOUT if layout else PROBE, [fonts, sizes])


def underline_rows(host, port, fonts, sizes):
    """Return, per font and size, the painted underline row and the baseline.

    One size per screenshot, and the page is rebuilt each time. A sweep laid
    out as one tall page runs off the bottom of the viewport - the rows past
    it come back blank, which reads as "no underline" and not as "not
    photographed" - and the columns searched then depend on the window width,
    which is not the same on two machines. A single row is always in view and
    is measured across the text's own extent.
    """
    import base64
    import io

    from PIL import Image
    import numpy as np

    m = Marionette(host, int(port), timeout=60)
    m.start("content")
    m.call("WebDriver:Navigate", {"url": "about:blank"})

    out = {}
    for fam in fonts:
        for size in sizes:
            box = m.script(UNDERLINE, [fam, size])
            shot = m.call("WebDriver:TakeScreenshot",
                          {"full": False, "hash": False})
            if isinstance(shot, dict):
                shot = shot.get("value", shot)
            a = np.asarray(
                Image.open(io.BytesIO(base64.b64decode(shot))).convert("L"))
            x0, x1 = int(box["left"]), int(round(box["right"]))
            base = box["baseline"]
            # Below the baseline the underline is the only ink, except where
            # skip-ink has cut a notch around a descender, so the darkest row
            # is it. Ties go to the first, which is the top of a thick rule.
            best, best_dark = None, 0
            for y in range(int(base), min(int(box["bottom"]), a.shape[0])):
                dark = int((a[y, x0:x1] < 128).sum())
                if dark > best_dark:
                    best, best_dark = y, dark
            out["%s %g" % (fam, size)] = {
                "row": best, "baseline": base, "dark": best_dark,
                "span": x1 - x0,
                "offset": None if best is None else best - base,
            }
    return out


def line(key, v):
    if "row" in v:
        return ("%-18s underline row %5s  baseline %8.3f  below %5s  "
                "%d of %d px dark"
                % (key, v["row"], v["baseline"], v["offset"], v["dark"],
                   v["span"]))
    if "ascent" in v:
        return ("%-18s layout ascent %8.4f  descent %8.4f  line box %.4f"
                % (key, v["ascent"], v["height"] - v["ascent"], v["height"]))
    return ("%-18s em %8.4f / %-8.4f  box %8.4f / %-8.4f  width %.4f"
            % (key, v["emAsc"], v["emDesc"], v["boxAsc"], v["boxDesc"],
               v["width"]))


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__.strip())
    fonts = ["MS Gothic", "Arial"]
    sizes = [13, 24]
    if "--font" in args:
        fonts = args[args.index("--font") + 1].split(",")
    if "--sizes" in args:
        sizes = [float(s) for s in args[args.index("--sizes") + 1].split(",")]
    layout = "--layout" in args
    if "--underline" in args:
        got = underline_rows(args[0], args[1], fonts, sizes)
    else:
        got = measure(args[0], args[1], fonts, sizes, layout)

    if "--against" in args:
        at = args.index("--against")
        if "--underline" in args:
            other = underline_rows(args[at + 1], args[at + 2], fonts, sizes)
        else:
            other = measure(args[at + 1], args[at + 2], fonts, sizes, layout)
        differ = [k for k in got if got[k] != other.get(k)]
        for k in got:
            if k in differ:
                print("- " + line(k, got[k]))
                print("+ " + line(k, other[k]))
        print("\n%d of %d differ" % (len(differ), len(got)))
        sys.exit(1 if differ else 0)

    for k, v in got.items():
        print(line(k, v))


if __name__ == "__main__":
    main()
