#!/usr/bin/env python3
"""
Enumerate font decisions on two browsers and diff them, layer by layer.

    tools/testing/font_census.py <mode> <sideA> <sideB>
                                 [--full] [--accept FILE]

A side is `[driver:]host:port`, the driver being `cdp` for Chromium and
Electron or `marionette` for Firefox, and DevTools when none is named. Firefox
answers the face question through InspectorUtils.getUsedFontFaces, which names
the face and not the family and reports no glyph count, so its cells hold bare
names. Both sides must be the same browser, which they always are: a census
compares one browser's two platforms.

Page comparison samples the rendering pipeline with whatever text the corpus
happens to contain, so a wrong decision hides until a page reaches it. Most of
the pipeline's decision layers take small, finite inputs, and both browsers
can be asked for every answer directly. Each mode enumerates one layer; a
divergence names its inputs, so what would have been a page-archaeology
session is one table row.

  generics  every generic family crossed with every content language, at both
            weights, asked twice per cell: once in the language's own script
            and once with Latin text, since the maps key on the element's
            locale and pick the CJK face for Latin text inside a CJK element.
  families  every named family crossed with the nine weights and both slants.
  fallback  which family draws every renderable codepoint, per content
            language and generic. Ranges that answer with one family are taken
            whole and mixed ones are split, so the full space costs little
            more than its transitions.
  metrics   per-character advance widths and the font box, measured on canvas
            for every family, size and weight. Canvas measures in the page, so
            a whole layer is one script call.
  shape     shaped widths of character pairs and clusters: joined Arabic
            pairs, Indic consonant-vowel and conjunct clusters, Thai stacking,
            emoji sequences, and every printable ASCII pair for the core Latin
            families, whose widths carry the kerning.
  raster    every fallback-census codepoint drawn in a fixed grid and
            screenshotted on both sides, compared cell by cell. The one layer
            that needs pixels; everything the other modes cleared that still
            differs here is rasterization itself.
  all       every mode above, with one summary.

--full widens fallback and raster to every language the generics mode knows.
--accept names a file of cell keys (one per line, # comments) to report as
accepted rather than diverging; the exit status counts only the rest.

Both browsers must reach the same font set and hold the same window size for
raster mode.

What this cannot enumerate: interactions between layers, run-level shaping
context beyond pairs and clusters, and paint effects. Those stay with
compare_pages.sh and the generated stress pages.
"""

import argparse
import base64
import io
import json
import sys
import unicodedata

import marionette
import viewport_protocol as vp

# ---------------------------------------------------------------------------
# The input spaces.
# ---------------------------------------------------------------------------

# One short sample per content language, in that language's own script. Zyyy
# stands for no language at all.
LANGS = [
    ("", "Wiki"), ("ar", "نص"), ("fa", "متن"),
    ("he", "אב"), ("ru", "тек"),
    ("el", "αβ"), ("ja", "あ漢"), ("ko", "한글"),
    ("zh-CN", "汉字"), ("zh-TW", "漢字"),
    ("nan", "Bân"), ("yue", "粵語"), ("th", "ไท"),
    ("hi", "हि"), ("ta", "தம"), ("te", "తె"),
    ("bn", "বা"), ("si", "සි"), ("km", "ខ្"),
    ("my", "မြ"), ("am", "አማ"), ("ka", "ქა"),
    ("hy", "հա"), ("vi", "tiếng"), ("ur", "ار"),
]
GENERICS = ["serif", "sans-serif", "monospace", "cursive", "fantasy", "math"]

FAMILIES = [
    "Arial", "Times New Roman", "Courier New", "Verdana", "Georgia", "Tahoma",
    "Segoe UI", "Calibri", "Cambria", "Consolas", "Impact", "Sylfaen",
    "Microsoft Sans Serif", "Lucida Console", "Ebrima", "Nirmala UI",
    "Malgun Gothic", "Meiryo", "Yu Gothic", "MS Gothic", "Microsoft YaHei",
    "SimSun", "NSimSun", "Microsoft JhengHei", "MingLiU", "Leelawadee UI",
    "Segoe UI Emoji", "Segoe UI Symbol", "Cambria Math",
]
ALL_WEIGHTS = [100, 200, 300, 400, 500, 600, 700, 800, 900]

# The Latin families whose kerning the shape mode sweeps pair by pair.
KERNED = ["Arial", "Times New Roman", "Segoe UI", "Calibri", "Georgia",
          "Verdana", "Tahoma"]

METRIC_SIZES = [12, 13, 16, 21, 24, 32]

# A face reports its name in the browser's own language, so the same file
# answers under two names on the two machines. Keyed lowercase.
CANONICAL = {
    "微軟正黑體": "Microsoft JhengHei",
    "微软雅黑": "Microsoft YaHei",
    "ｍｓ ゴシック": "MS Gothic",
    "ｍｓ 明朝": "MS Mincho",
    "ｍｓ ｐゴシック": "MS PGothic",
    "ｍｓ ｐ明朝": "MS PMincho",
    "新宋体": "NSimSun",
    "宋体": "SimSun",
    "맑은 고딕": "Malgun Gothic",
    "메이리오": "Meiryo",
    "メイリオ": "Meiryo",
    "游ゴシック": "Yu Gothic",
    "游明朝": "Yu Mincho",
    "굴림체": "Gulimche",
    "細明體": "MingLiU",
    "新細明體": "PMingLiU",
}

# The fallback and raster domain: every codepoint that renders as its own
# glyph. Letters, numbers, symbols and punctuation; marks and controls draw
# against dotted circles or not at all and answer for their base instead.
def renderable(limit):
    out = []
    for cp in range(0x21, limit):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        if unicodedata.category(chr(cp))[0] in "LNSP":
            out.append(cp)
    return out


def fallback_configs(full):
    if full:
        return [(lang, gen) for lang, _ in LANGS for gen in
                ("sans-serif", "serif", "monospace")]
    # The Han disambiguation set, plus the three generics with no language.
    return [("", "sans-serif"), ("", "serif"), ("", "monospace"),
            ("ja", "sans-serif"), ("zh-CN", "sans-serif"),
            ("zh-TW", "sans-serif"), ("ko", "sans-serif"),
            ("ar", "sans-serif"), ("th", "sans-serif"), ("hi", "sans-serif")]


# ---------------------------------------------------------------------------
# Driving one browser.
# ---------------------------------------------------------------------------

BUILD = """
const spec = JSON.parse(arguments[0]);
document.body.textContent = '';
document.body.style.cssText = 'margin:0;font-size:16px;background:#fff';
for (const [id, fam, lang, weight, slant, text] of spec) {
  const d = document.createElement('div');
  d.id = id;
  if (lang) d.lang = lang;
  d.style.cssText = 'font-family:' + fam + ';font-weight:' + weight +
                    ';font-style:' + slant;
  d.textContent = text;
  document.body.appendChild(d);
}
return document.body.childElementCount;
"""


# Firefox reports the faces a Range was drawn with through
# InspectorUtils.getUsedFontFaces, the call the DevTools font panel makes,
# which answers after every fallback step the layout engine took. It is a
# chrome-only global, so this runs in Marionette's system sandbox and the
# browser has to have been started with -remote-allow-system-access.
USED_FACES = """
const count = JSON.parse(arguments[0]);
const out = [];
for (let i = 0; i < count; i++) {
  const el = document.getElementById('c' + i);
  const names = [];
  if (el) {
    const range = document.createRange();
    range.selectNodeContents(el);
    const faces = InspectorUtils.getUsedFontFaces(range, 0, true);
    for (let f = 0; f < faces.length; f++) { names.push(faces[f].name); }
  }
  out.push(names);
}
return JSON.stringify(out);
"""


class CdpSide:
    """A Chromium or Electron, driven over DevTools."""

    driver = "cdp"

    def __init__(self, endpoint):
        host, _, port = endpoint.partition(":")
        self.name = endpoint
        self.browser = vp.CdpBrowser(host, int(port or 9222), timeout=120.0)
        self.browser.navigate("about:blank")
        vp.await_condition(self.browser, vp.PAGE_LOADED, 30,
                           "the page never finished loading")
        vp.hide_scrollbars(self.browser)
        self.browser.call("DOM.enable")
        self.browser.call("CSS.enable")

    def build(self, spec):
        self.browser.script(BUILD, [json.dumps(spec)])

    def fonts_of(self, count):
        """The faces behind elements c0..c<count-1>, canonically named."""
        doc = self.browser.call("DOM.getDocument", {"depth": -1})
        root = doc["root"]["nodeId"]
        out = []
        for i in range(count):
            node = self.browser.call("DOM.querySelector",
                                     {"nodeId": root, "selector": "#c%d" % i})
            fonts = self.browser.call("CSS.getPlatformFontsForNode",
                                      {"nodeId": node["nodeId"]})
            out.append(tuple(sorted(
                (CANONICAL.get(f["familyName"].lower(), f["familyName"]),
                 f["glyphCount"]) for f in fonts["fonts"])))
        return out

    def evaluate(self, body, args=()):
        return self.browser.script(body, args)

    def settle(self):
        self.browser.script_async(PAINTED)

    def shot(self):
        r = self.browser.call("Page.captureScreenshot",
                              {"format": "png", "fromSurface": True,
                               "optimizeForSpeed": True})
        return base64.b64decode(r["data"])

    def close(self):
        self.browser.close()


class MarionetteSide:
    """A Firefox, driven over Marionette."""

    driver = "marionette"

    def __init__(self, endpoint):
        host, _, port = endpoint.partition(":")
        self.name = endpoint
        self.m = marionette.Marionette(host, int(port or 2828), timeout=120)
        self.m.start("content")
        self.m.call("WebDriver:Navigate", {"url": "about:blank"})
        self.evaluate("document.documentElement.style.overflow = 'hidden';"
                      "return 1;")

    def build(self, spec):
        self.m.script(BUILD, [json.dumps(spec)])

    def fonts_of(self, count):
        """The faces behind elements c0..c<count-1>, canonically named.

        Firefox names the face, not the family, and reports no glyph count,
        so a cell here is a name tuple where the DevTools side is a name and
        count tuple. Both sides of a comparison run the same browser, so the
        two shapes never meet.
        """
        rows = json.loads(self.m.script(USED_FACES, [json.dumps(count)],
                                        sandbox="system"))
        return [tuple(sorted(CANONICAL.get(n.lower(), n) for n in names))
                for names in rows]

    def evaluate(self, body, args=()):
        return self.m.script(body, list(args))

    def settle(self):
        self.m.script_async(PAINTED)

    def shot(self):
        return self.m.screenshot()

    def close(self):
        self.m.close()


DRIVERS = {"cdp": CdpSide, "marionette": MarionetteSide}


def open_side(endpoint):
    """A side from `[driver:]host:port`, DevTools when no driver is named."""
    driver, _, rest = endpoint.partition(":")
    if driver in DRIVERS:
        return DRIVERS[driver](rest)
    return CdpSide(endpoint)


# ---------------------------------------------------------------------------
# The modes. Each returns {cell key: (answer A, answer B)} for its
# divergences.
# ---------------------------------------------------------------------------

def run_cells(sides, cells):
    """One element per cell on each side, and the faces each side used."""
    answers = []
    for side in sides:
        spec = [("c%d" % i,) + tuple(c[1:]) for i, c in enumerate(cells)]
        side.build(spec)
        answers.append(side.fonts_of(len(cells)))
    out = {}
    for i, cell in enumerate(cells):
        if answers[0][i] != answers[1][i]:
            out[cell[0]] = (answers[0][i], answers[1][i])
    return out


def mode_generics(sides, full):
    cells = []
    for gen in GENERICS:
        for lang, sample in LANGS:
            for weight in (400, 700):
                key = "%s|%s|%d" % (gen, lang or "-", weight)
                cells.append((key + "|native", gen, lang, weight, "normal", sample))
                cells.append((key + "|latin", gen, lang, weight, "normal", "Wiki"))
    return run_cells(sides, cells), len(cells)


def mode_families(sides, full):
    cells = []
    for fam in FAMILIES:
        for weight in ALL_WEIGHTS:
            for slant in ("normal", "italic"):
                key = "%s|%d|%s" % (fam, weight, slant)
                cells.append((key, '"%s"' % fam, "", weight, slant, "Wiki 09"))
    return run_cells(sides, cells), len(cells)


FINGERPRINT = """
const [cps, lang, generic] = JSON.parse(arguments[0]);
const host = document.createElement('div');
if (lang) host.lang = lang;
document.body.appendChild(host);
const canvas = document.createElement('canvas');
host.appendChild(canvas);
const ctx = canvas.getContext('2d');
ctx.font = '16px ' + generic;
const out = [];
for (const cp of cps) {
  const m = ctx.measureText(String.fromCodePoint(cp));
  // Rounded, since the two rasterizers disagree on ink-box hundredths for
  // the same face; a different face moves these by whole pixels.
  out.push(Math.round(m.width * 20) / 20,
           Math.round(m.actualBoundingBoxAscent * 2) / 2,
           Math.round(m.actualBoundingBoxDescent * 2) / 2,
           Math.round(m.actualBoundingBoxLeft * 2) / 2,
           Math.round(m.actualBoundingBoxRight * 2) / 2);
}
host.remove();
return JSON.stringify(out);
"""


def fingerprints(side, cps, lang, generic):
    """One measurement tuple per codepoint, taken wholly in the page.

    The canvas cannot say which face it used, but two different faces
    almost never agree on advance and ink box at once, so the tuple stands
    in for the face and the names are fetched afterwards for the few
    codepoints whose tuples disagree.
    """
    out = []
    for start in range(0, len(cps), 20000):
        chunk = cps[start:start + 20000]
        row = json.loads(side.evaluate(
            FINGERPRINT, [json.dumps([chunk, lang, generic])]))
        out.extend(tuple(row[i:i + 5]) for i in range(0, len(row), 5))
    return out


def face_names(answer):
    """The names in one cell, whichever shape the driver reports.

    DevTools gives a name and a glyph count per face; Marionette gives the
    name alone.
    """
    return {f if isinstance(f, str) else f[0] for f in answer}


def name_faces(side, cps, lang, generic):
    spec = [("c%d" % i, generic, lang, 400, "normal", chr(cp))
            for i, cp in enumerate(cps)]
    side.build(spec)
    answers = side.fonts_of(len(spec))
    return {cp: ",".join(sorted(face_names(a))) or "(nothing)"
            for cp, a in zip(cps, answers)}


# How many of a config's differing codepoints get their faces named.
kNamedPerConfig = 60


def mode_fallback(sides, full):
    # Fallback is decided in ranges, so a stride catches a range-level
    # divergence at a fraction of the cost; --full walks every codepoint.
    cps = renderable(0x30000) if full else renderable(0x10000)[::16]
    total = 0
    out = {}
    for lang, generic in fallback_configs(full):
        fa = fingerprints(sides[0], cps, lang, generic)
        fb = fingerprints(sides[1], cps, lang, generic)
        differ = [cp for cp, a, b in zip(cps, fa, fb) if a != b]
        total += len(cps)
        # Every differing codepoint is counted; only the first few are named,
        # since naming one costs a build and a face query per side. A cell the
        # names were not fetched for still reports, as the count it is.
        named = differ[:kNamedPerConfig]
        na = name_faces(sides[0], named, lang, generic)
        nb = name_faces(sides[1], named, lang, generic)
        for cp in differ:
            key = "U+%04X|%s|%s" % (cp, lang or "-", generic)
            out[key] = ((na[cp], nb[cp]) if cp in na
                        else ("(not named)", "(not named)"))
        print("  fallback %s/%s: %d codepoints, %d differ%s"
              % (lang or "-", generic, len(cps), len(differ),
                 "" if len(differ) <= kNamedPerConfig
                 else ", %d named" % kNamedPerConfig), file=sys.stderr)
    return out, total


MEASURE = """
const [configs, text] = JSON.parse(arguments[0]);
const canvas = document.createElement('canvas');
const ctx = canvas.getContext('2d');
const out = [];
for (const [fam, size, weight] of configs) {
  ctx.font = weight + ' ' + size + 'px ' + fam;
  const row = [];
  const box = ctx.measureText(text);
  row.push(box.fontBoundingBoxAscent, box.fontBoundingBoxDescent,
           box.actualBoundingBoxAscent, box.actualBoundingBoxDescent,
           box.width);
  for (const ch of text) row.push(ctx.measureText(ch).width);
  out.push(row);
}
return JSON.stringify(out);
"""

ASCII = "".join(chr(c) for c in range(0x21, 0x7F))


def mode_metrics(sides, full):
    configs = [['"%s"' % fam, size, weight]
               for fam in FAMILIES for size in METRIC_SIZES
               for weight in (400, 700)]
    rows = []
    for side in sides:
        rows.append(json.loads(side.evaluate(
            MEASURE, [json.dumps([configs, ASCII])])))
    labels = ["boxAscent", "boxDescent", "inkAscent", "inkDescent", "width"] \
        + list(ASCII)
    out = {}
    for c, (ra, rb) in enumerate(zip(rows[0], rows[1])):
        fam, size, weight = configs[c]
        for v, (va, vb) in enumerate(zip(ra, rb)):
            if abs(va - vb) > 0.01:
                out["%s|%d|%d|%s" % (fam.strip('"'), size, weight, labels[v])] = \
                    (round(va, 3), round(vb, 3))
    return out, len(configs) * len(labels)


def shape_sequences():
    seqs = []
    arabic = [chr(c) for c in range(0x0627, 0x064B)]
    for a in arabic:
        for b in arabic:
            seqs.append(("ar", '"Segoe UI"', a + b))
    deva_c = [chr(c) for c in range(0x0915, 0x093A)]
    deva_v = [chr(c) for c in range(0x093E, 0x094D)]
    for c in deva_c:
        for v in deva_v:
            seqs.append(("hi", '"Nirmala UI"', c + v))
    for c1 in deva_c[:12]:
        for c2 in deva_c[:12]:
            seqs.append(("hi", '"Nirmala UI"', c1 + "्" + c2))
    thai_c = [chr(c) for c in range(0x0E01, 0x0E2F)]
    thai_m = [chr(c) for c in (0x0E31, 0x0E34, 0x0E35, 0x0E38, 0x0E39,
                               0x0E48, 0x0E49, 0x0E4A, 0x0E4B)]
    for c in thai_c:
        for m in thai_m:
            seqs.append(("th", '"Leelawadee UI"', c + m))
    emoji = ["👍", "👍🏽", "🇺🇳", "👨‍👩‍👧", "🏳️‍🌈", "❤️", "1️⃣", "🧑‍🚀"]
    for e in emoji:
        seqs.append(("", '"Segoe UI Emoji"', e))
    for fam in KERNED:
        for a in ASCII:
            for b in ASCII:
                seqs.append(("", '"%s"' % fam, a + b))
    return seqs


SHAPE = """
const seqs = JSON.parse(arguments[0]);
const canvas = document.createElement('canvas');
const ctx = canvas.getContext('2d');
const out = [];
let last = '';
for (const [fam, text] of seqs) {
  if (fam !== last) { ctx.font = '16px ' + fam; last = fam; }
  out.push(Math.round(ctx.measureText(text).width * 1000) / 1000);
}
return JSON.stringify(out);
"""


def mode_shape(sides, full):
    seqs = shape_sequences()
    # Canvas ignores the element language, so what lang would pick is pinned
    # by naming the family outright above.
    payload = json.dumps([[fam, text] for _, fam, text in seqs])
    widths = []
    for side in sides:
        widths.append(json.loads(side.evaluate(SHAPE, [payload])))
    out = {}
    for i, (lang, fam, text) in enumerate(seqs):
        if abs(widths[0][i] - widths[1][i]) > 0.01:
            key = "%s|%s|%s" % (fam.strip('"'), lang or "-",
                                "+".join("%04X" % ord(c) for c in text))
            out[key] = (widths[0][i], widths[1][i])
    return out, len(seqs)


GRID = """
const [chars, lang, generic, size] = JSON.parse(arguments[0]);
document.body.textContent = '';
document.body.style.cssText = 'margin:0;background:#fff';
const d = document.createElement('div');
if (lang) d.lang = lang;
d.style.cssText = 'display:grid;grid-template-columns:repeat(40, 44px);' +
    'font-family:' + generic + ';font-size:' + size + 'px';
for (const ch of chars) {
  const c = document.createElement('span');
  c.style.cssText = 'width:44px;height:36px;overflow:hidden;' +
      'line-height:36px;display:block';
  c.textContent = ch;
  d.appendChild(c);
}
document.body.appendChild(d);
return chars.length;
"""

PAINTED = """
const done = arguments[arguments.length - 1];
requestAnimationFrame(() => requestAnimationFrame(() => done(1)));
"""


def mode_raster(sides, full):
    import numpy as np
    from PIL import Image

    cps = renderable(0x30000) if full else renderable(0x10000)[::16]
    configs = [("", "sans-serif", 16)]
    if full:
        configs += [("ja", "sans-serif", 16), ("zh-CN", "sans-serif", 16),
                    ("", "serif", 16), ("", "monospace", 16)]
    cols, cell_w, cell_h = 40, 44, 36
    rows_per_page = 1080 // cell_h
    per_page = cols * rows_per_page

    out = {}
    total = 0
    for lang, generic, size in configs:
        for start in range(0, len(cps), per_page):
            chunk = cps[start:start + per_page]
            grids = []
            for side in sides:
                side.evaluate(GRID, [json.dumps(
                    [[chr(cp) for cp in chunk], lang, generic, size])])
                side.settle()
                png = side.shot()
                grids.append(np.asarray(
                    Image.open(io.BytesIO(png)).convert("RGB")).astype(np.int16))
            a, b = grids
            if a.shape != b.shape:
                out["page@%d|%s|%s" % (start, lang or "-", generic)] = \
                    ("shape %s" % (a.shape,), "shape %s" % (b.shape,))
                continue
            diff = np.abs(a - b).max(axis=2)
            for i, cp in enumerate(chunk):
                y = (i // cols) * cell_h
                x = (i % cols) * cell_w
                cell = diff[y:y + cell_h, x:x + cell_w]
                if cell.size and cell.max() > 0:
                    out["U+%04X|%s|%s|%d" % (cp, lang or "-", generic, size)] = \
                        ("%d px differ" % int((cell > 0).sum()),
                         "max %d" % int(cell.max()))
            total += len(chunk)
        print("  raster %s/%s: %d codepoints" % (lang or "-", generic, total),
              file=sys.stderr)
    return out, total * 1


MODES = {
    "generics": mode_generics,
    "families": mode_families,
    "fallback": mode_fallback,
    "metrics": mode_metrics,
    "shape": mode_shape,
    "raster": mode_raster,
}


def describe(answer):
    """One side's answer as a line: a face tuple, or whatever else a mode put
    there. Firefox names faces without a glyph count, so a name stands alone
    where DevTools writes name(count)."""
    if not isinstance(answer, tuple):
        return answer
    return ", ".join(f if isinstance(f, str) else "%s(%d)" % f for f in answer)


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("mode", choices=sorted(MODES) + ["all"])
    ap.add_argument("a", metavar="sideA")
    ap.add_argument("b", metavar="sideB")
    ap.add_argument("--full", action="store_true",
                    help="widen fallback and raster to every language")
    ap.add_argument("--accept", default=None,
                    help="file of cell keys to report as accepted")
    args = ap.parse_args()

    accepted = set()
    if args.accept:
        with open(args.accept, encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line and not line.startswith("#"):
                    accepted.add(line)

    sides = [open_side(args.a), open_side(args.b)]
    if sides[0].driver != sides[1].driver:
        for side in sides:
            side.close()
        sys.exit("both sides must be the same browser; %s answers over %s and "
                 "%s over %s" % (args.a, sides[0].driver, args.b,
                                 sides[1].driver))
    names = sorted(MODES) if args.mode == "all" else [args.mode]
    failing = 0
    try:
        for name in names:
            diverging, total = MODES[name](sides, args.full)
            known = sum(1 for k in diverging if k in accepted)
            new = len(diverging) - known
            failing += new
            print("== %s: %d of %d cells match on linux / windows "
                  "(%d diverge, %d accepted)"
                  % (name, total - len(diverging), total, len(diverging), known))
            for key in sorted(diverging):
                mark = "accepted " if key in accepted else ""
                a, b = diverging[key]
                print("  %s%-44s A: %-34s B: %s"
                      % (mark, key, describe(a), describe(b)))
    finally:
        for side in sides:
            side.close()
    return 1 if failing else 0


if __name__ == "__main__":
    sys.exit(main())
