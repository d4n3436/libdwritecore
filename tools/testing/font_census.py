#!/usr/bin/env python3
"""
Enumerate font decisions on two browsers and diff them, layer by layer.

    tools/testing/font_census.py <mode> <sideA> <sideB>
                                 [--full] [--accept FILE]...

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
            Sound on a Firefox pair: the face is asked for by name through
            InspectorUtils.getUsedFontFaces, not inferred from an advance and
            ink box, which Firefox snaps.
  raster    every fallback-census codepoint drawn in a fixed grid and
            screenshotted on both sides, compared cell by cell. The one layer
            that needs pixels; everything the other modes cleared that still
            differs here is rasterization itself.
  all       every mode above, with one summary.

--full widens fallback and raster to every language the generics mode knows.
--accept names a file of cell keys (one per line, # comments) to report as
accepted rather than diverging; the exit status counts only the rest. It may
be given more than once, so a build whose own reference is the odd one out
adds its file beside the shared one instead of copying it, and the cells it
accepts stay flagged on every other build.

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
import os
import sys
import threading
import time
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

# Whole sizes alone hide a whole class: anything that rounds a size only shows
# where the size has a fraction to lose. A UA stylesheet reaches these
# constantly, since h5 is 0.83em, h6 0.67em and small/sub/sup are `smaller`.
METRIC_SIZES = [11, 12, 12.5, 13, 13.28, 13.33, 14.4, 16, 16.6, 17.28,
                18, 19.2, 20.8, 21, 24, 25.8064, 26.6, 28.8, 32, 33.12,
                36.8, 40, 48,
                # Above 256 Skia stops drawing from a mask and generates at a
                # canonical 64 px path strike that the draw scales, which is a
                # regime nothing below the threshold exercises.
                200, 260, 400]

# The weights a family is actually asked for on a page, plus the two that
# Windows maps differently inside a family.
METRIC_WEIGHTS = [300, 400, 500, 600, 700]

# Whether the face is asked for upright or slanted. A family with no italic
# face gets a synthetic oblique, and which faces exist differs by family, so
# this reaches both the selection and the synthesis.
METRIC_STYLES = ["normal", "italic"]

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


# Set from --lang: the content languages a run is restricted to, or None for
# all of them.
kOnlyLangs = None


# Set from --configs: (index, count), or None for the whole list.
kShard = None

# Set from --family: the families to sweep, or None for all of them.
kOnlyFamilies = None


def keep_family(name):
    return kOnlyFamilies is None or name in kOnlyFamilies


def shard(rows):
    """Every nth row, for splitting one run across several browser pairs.

    Applied after any filter so it composes with --lang and --family.
    """
    if kShard is None:
        return rows
    index, count = kShard
    return rows[index::count]


def keep_lang(lang):
    return kOnlyLangs is None or (lang or "-") in kOnlyLangs


def fallback_configs(full):
    if full:
        rows = [(lang, gen) for lang, _ in LANGS for gen in
                ("sans-serif", "serif", "monospace")]
    else:
        # The Han disambiguation set, plus the three generics with no language.
        rows = [("", "sans-serif"), ("", "serif"), ("", "monospace"),
                ("ja", "sans-serif"), ("zh-CN", "sans-serif"),
                ("zh-TW", "sans-serif"), ("ko", "sans-serif"),
                ("ar", "sans-serif"), ("th", "sans-serif"), ("hi", "sans-serif")]
    return shard([row for row in rows if keep_lang(row[0])])


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
        if kViewport is not None:
            # Raster mode compares screenshots, so the two sides have to hold
            # the same viewport. Where the app decides its own window size,
            # as Electron's does, they already do and this stays off.
            self.browser.call("Emulation.setDeviceMetricsOverride",
                              {"width": kViewport[0], "height": kViewport[1],
                               "deviceScaleFactor": 1, "mobile": False})
        self.browser.call("DOM.enable")
        self.browser.call("CSS.enable")
        self.warm()

    def warm(self):
        """Draw once before anything is measured.

        A build whose Skia is compiled in finds its rasterizer from a live
        scaler context, so the first glyphs of a fresh browser are drawn by
        Skia and measure a pixel narrower. Every cell after that is the
        rasterizer under test; this makes the first one so too.
        """
        self.browser.script(WARM, [])
        self.settle()

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
        # The face a node was drawn with is only reported once it has been
        # drawn; asking earlier answers with no faces at all.
        side.settle()
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


WARM = """
const host = document.createElement('div');
host.style.cssText = 'position:absolute;left:0;top:0;font:16px sans-serif';
// One run per script the corpus reaches, so every rasterizer the shim
// installs has been through a scaler context before a cell is measured.
host.textContent = 'Aa1 \u00c4\u00e9 \u0416 \u03b1 \u0627 \u05d0 \u0905 ' +
                   '\u0e01 \u4e00 \u3042 \uac00 \u2603 \u2500';
document.body.appendChild(host);
const ctx = document.createElement('canvas').getContext('2d');
ctx.font = '16px sans-serif';
ctx.measureText(host.textContent);
host.remove();
return '';
"""

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
           Math.round(m.actualBoundingBoxRight * 2) / 2,
           // The face's own ascent and descent, not the glyph's. Two faces
           // that happen to draw one glyph into the same box still differ
           // here, which is how a fullwidth form drawn by the wrong CJK font
           // is caught without a screenshot.
           Math.round(m.fontBoundingBoxAscent * 2) / 2,
           Math.round(m.fontBoundingBoxDescent * 2) / 2);
}
host.remove();
return JSON.stringify(out);
"""


# Values per codepoint that FINGERPRINT pushes.
kFingerprintWidth = 7


def fingerprints(side, cps, lang, generic):
    """One measurement tuple per codepoint, taken wholly in the page.

    The canvas cannot say which face it used, so the tuple stands in for it
    and the names are fetched afterwards for the codepoints that disagree.
    Glyph advance and ink box alone are not enough: Microsoft Tai Le and
    Microsoft YaHei draw U+FF06 into the same box, so the face's own
    ascent and descent are measured too.
    """
    out = []
    for start in range(0, len(cps), 20000):
        chunk = cps[start:start + 20000]
        row = json.loads(side.evaluate(
            FINGERPRINT, [json.dumps([chunk, lang, generic])]))
        out.extend(tuple(row[i:i + kFingerprintWidth])
                   for i in range(0, len(row), kFingerprintWidth))
    return out


def face_names(answer):
    """The names in one cell, whichever shape the driver reports.

    DevTools gives a name and a glyph count per face; Marionette gives the
    name alone.
    """
    return {f if isinstance(f, str) else f[0] for f in answer}


# Nodes per build. One build carries the whole batch, so the only reason to
# split is the page a large batch would otherwise become. Naming is round-trip
# bound and not layout bound, so a bigger batch is a straight win: measured
# over a 143,177 codepoint config, 500 costs 17.1s on Linux and 22.9s on the
# guest, and 4000 costs 11.2s and 13.4s. It flattens past 4000.
kNamesPerBuild = 4000


def both_sides(work, sides, *args):
    """Run `work(side, *args)` on both sides at once and return the two answers.

    Each side is its own socket to its own browser, so nothing is shared and
    the only ordering that matters is that both finish. A failure is re-raised
    from the calling thread rather than swallowed, so a browser that died still
    stops the run.
    """
    out = [None, None]
    error = [None, None]

    def run(i):
        try:
            out[i] = work(sides[i], *args)
        except BaseException as exc:                 # noqa: BLE001
            error[i] = exc

    threads = [threading.Thread(target=run, args=(i,)) for i in (0, 1)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for exc in error:
        if exc is not None:
            raise exc
    return out[0], out[1]


def name_faces(side, cps, lang, generic):
    out = {}
    for at in range(0, len(cps), kNamesPerBuild):
        batch = cps[at:at + kNamesPerBuild]
        spec = [("c%d" % i, generic, lang, 400, "normal", chr(cp))
                for i, cp in enumerate(batch)]
        side.build(spec)
        side.settle()
        answers = side.fonts_of(len(spec))
        for cp, a in zip(batch, answers):
            out[cp] = ",".join(sorted(face_names(a))) or "(nothing)"
    return out


# How many of a config's differing codepoints get their faces named. One build
# carries a batch of them, so naming every cell costs a handful of round trips
# per config. Lower it through the environment for a faster count-only run.
kNamedPerConfig = int(os.environ.get("DWC_CENSUS_NAMED", "0")) or None

# The viewport both sides are pinned to, for a browser whose window size the
# two platforms do not agree on. None leaves each side as it starts.
kViewport = None


def mode_fallback(sides, full):
    # A Chromium renderer caches the fallback family under the character
    # alone, so the first language to ask answers for every language after
    # it. Nothing in DevTools opens a fresh renderer on Electron, so the
    # browser has to be restarted between languages, which is what --lang is
    # for.
    if any(side.driver == "cdp" for side in sides) and len(fallback_configs(full)) > 1:
        print("  note: one browser answers every language from the first one's "
              "cache; run --lang once per language, restarting between them, "
              "for a reading that means anything", file=sys.stderr)
    # Fallback is decided in ranges, so a stride catches a range-level
    # divergence at a fraction of the cost; --full walks every codepoint.
    cps = renderable(0x30000) if full else renderable(0x10000)[::16]
    total = 0
    out = {}
    for lang, generic in fallback_configs(full):
        # The face is asked for by name rather than inferred from a canvas
        # measurement. Advance and ink box do not identify a face: Microsoft
        # Tai Le and Microsoft YaHei draw U+FF06 into the same box with the
        # same font box, so a stride of measurements called that cell equal
        # while the two sides were using different fonts. Naming all 3387
        # costs under a second, since one build carries 500 nodes.
        # The two sides share nothing, so asking them in turn costs their sum
        # while asking them at once costs the slower one. On a guest that is
        # about 1.2x slower than the host, that is most of the run.
        na, nb = both_sides(name_faces, sides, cps, lang, generic)
        differ = [cp for cp in cps if na[cp] != nb[cp]]
        total += len(cps)
        for cp in differ:
            out["U+%04X|%s|%s" % (cp, lang or "-", generic)] = (na[cp], nb[cp])
        print("  fallback %s/%s: %d codepoints, %d differ"
              % (lang or "-", generic, len(cps), len(differ)),
              file=sys.stderr)
    return out, total


MEASURE = """
const [configs, text] = JSON.parse(arguments[0]);
const canvas = document.createElement('canvas');
const ctx = canvas.getContext('2d');
const canary = '13px monospace';
const out = [];
for (const [fam, size, weight, style] of configs) {
  // A font shorthand the parser rejects leaves ctx.font as it was, so every
  // cell after a bad one would silently measure its neighbor and the census
  // would call that clean. Setting a canary first makes a rejected string
  // show up as a 0 in the row instead.
  ctx.font = canary;
  ctx.font = style + ' ' + weight + ' ' + size + 'px ' + fam;
  const row = [ctx.font === canary ? 0 : 1];
  const box = ctx.measureText(text);
  // Left and right as well as the advance: a shear, a stroke or a side
  // bearing moves ink sideways without moving the advance at all, and the
  // vertical pair cannot see any of it.
  row.push(box.fontBoundingBoxAscent, box.fontBoundingBoxDescent,
           box.actualBoundingBoxAscent, box.actualBoundingBoxDescent,
           box.actualBoundingBoxLeft, box.actualBoundingBoxRight,
           box.width);
  for (const ch of text) row.push(ctx.measureText(ch).width);
  out.push(row);
}
return JSON.stringify(out);
"""

ASCII = "".join(chr(c) for c in range(0x21, 0x7F))


def mode_metrics(sides, full):
    configs = shard([['"%s"' % fam, size, weight, style]
                     for fam in FAMILIES if keep_family(fam) for size in METRIC_SIZES
                     for weight in METRIC_WEIGHTS for style in METRIC_STYLES])
    if not configs:
        sys.exit("no families left to measure; check --family")
    rows = []
    for side in sides:
        rows.append(json.loads(side.evaluate(
            MEASURE, [json.dumps([configs, ASCII])])))
    labels = ["applied", "boxAscent", "boxDescent", "inkAscent", "inkDescent",
              "inkLeft", "inkRight", "width"] + list(ASCII)
    out = {}
    for c, (ra, rb) in enumerate(zip(rows[0], rows[1])):
        fam, size, weight, style = configs[c]
        for v, (va, vb) in enumerate(zip(ra, rb)):
            if abs(va - vb) > 0.01:
                # %s on the size, since %d collapsed 13.28 and 13.33 onto one
                # key and only the last of them was ever reported.
                out["%s|%s|%d|%s|%s" %
                    (fam.strip('"'), size, weight, style, labels[v])] = \
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
    seqs = [row for row in seqs if keep_family(row[1].strip('"'))]
    return shard(seqs)


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
                    # Color glyphs come from a different renderer on each
                    # side, Fontations against DirectWrite's paint tree, and
                    # they differ by edge coverage that no rasterizer setting
                    # reaches. Saying which cells those are keeps them from
                    # being read as a monochrome raster defect.
                    # Saturated fill, not edge fringes. ClearType gives every
                    # glyph colored edges, so a spread test alone calls all
                    # text color; a color glyph instead has many pixels that
                    # are both far from gray and bright.
                    box = a[y:y + cell_h, x:x + cell_w]
                    spread = box.max(axis=2) - box.min(axis=2)
                    lit = ((spread > 60) & (box.max(axis=2) > 128)).sum()
                    kind = "color" if lit > box.shape[0] * box.shape[1] * 0.02 else "mono"
                    out["U+%04X|%s|%s|%d" % (cp, lang or "-", generic, size)] = \
                        ("%d px differ (%s)" % (int((cell > 0).sum()), kind),
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


def elapsed(seconds):
    """A duration in the largest unit that keeps it readable."""
    if seconds < 60:
        return "%.1fs" % seconds
    minutes, rest = divmod(int(round(seconds)), 60)
    return "%dm %02ds" % (minutes, rest)


def describe(answer):
    """One side's answer as a line: a face tuple, or whatever else a mode put
    there. Firefox names faces without a glyph count, so a name stands alone
    where DevTools writes name(count)."""
    if not isinstance(answer, tuple):
        return answer
    return ", ".join(f if isinstance(f, str) else "%s(%d)" % f for f in answer)


def is_accepted(key, accepted):
    """Whether a cell key is covered by the accept file.

    An accept entry may name fewer fields than the cell key carries. raster
    keys end in the pixel size, so `U+FC9C|-|sans-serif` covers every size of
    that cell, while an entry that names the size stays exact. Without this
    the Arabic presentation forms read as seven fresh divergences on every
    raster run, since the file was written against the fallback key shape.
    """
    if key in accepted:
        return True
    return any(key.startswith(entry + "|") for entry in accepted)


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("mode", choices=sorted(MODES) + ["all"])
    ap.add_argument("a", metavar="sideA")
    ap.add_argument("b", metavar="sideB")
    ap.add_argument("--full", action="store_true",
                    help="widen fallback and raster to every language")
    ap.add_argument("--lang", default=None,
                    help="restrict fallback and raster to these content "
                         "languages, comma separated, '-' for no language. "
                         "One language per browser is the only way to read "
                         "this layer, since a Linux renderer answers a "
                         "character from whichever language asked first.")
    ap.add_argument("--family", action="append", default=None, metavar="NAME",
                    help="restrict metrics and shape to these families, exact "
                         "names from FAMILIES, repeatable. A whole metrics pass "
                         "is 7540 configs, so an A/B that only needs a few "
                         "families is minutes cheaper per sample")
    ap.add_argument("--configs", default=None, metavar="I/N",
                    help="take every Nth fallback or raster config, starting at "
                         "I (0-based), so one run can be split across several "
                         "browser pairs. Composes with --lang.")
    ap.add_argument("--viewport", default=None, metavar="WxH",
                    help="pin both sides to this viewport, for raster mode on a "
                         "browser whose window size the platforms disagree on")
    ap.add_argument("--accept", action="append", default=None,
                    help="file of cell keys to report as accepted; repeatable, "
                         "so a build with its own accepted cells adds a file "
                         "instead of copying the shared one")
    args = ap.parse_args()

    accepted = set()
    for path in args.accept or []:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line and not line.startswith("#"):
                    accepted.add(line)

    # Read before the sides are opened, since each applies it as it connects.
    if args.viewport:
        w, _, h = args.viewport.partition("x")
        globals()["kViewport"] = (int(w), int(h))
    sides = [open_side(args.a), open_side(args.b)]
    if sides[0].driver != sides[1].driver:
        for side in sides:
            side.close()
        sys.exit("both sides must be the same browser; %s answers over %s and "
                 "%s over %s" % (args.a, sides[0].driver, args.b,
                                 sides[1].driver))
    if args.lang is not None:
        globals()["kOnlyLangs"] = set(args.lang.split(","))
    if args.configs is not None:
        index, _, count = args.configs.partition("/")
        if not count or not index.isdigit() or not count.isdigit() or int(count) == 0:
            sys.exit("--configs takes I/N, both numbers, N above zero")
        if int(index) >= int(count):
            sys.exit("--configs index %s is not below %s" % (index, count))
        globals()["kShard"] = (int(index), int(count))
    if args.family:
        unknown = [f for f in args.family if f not in FAMILIES]
        if unknown:
            sys.exit("--family does not know %s" % ", ".join(unknown))
        globals()["kOnlyFamilies"] = set(args.family)
    names = sorted(MODES) if args.mode == "all" else [args.mode]
    failing = 0
    try:
        started = time.monotonic()
        for name in names:
            began = time.monotonic()
            diverging, total = MODES[name](sides, args.full)
            took = time.monotonic() - began
            known = sum(1 for k in diverging if is_accepted(k, accepted))
            new = len(diverging) - known
            failing += new
            print("== %s: %d of %d cells match on linux / windows "
                  "(%d diverge, %d accepted) in %s"
                  % (name, total - len(diverging), total, len(diverging), known,
                     elapsed(took)))
            for key in sorted(diverging):
                mark = "accepted " if is_accepted(key, accepted) else ""
                a, b = diverging[key]
                print("  %s%-44s A: %-34s B: %s"
                      % (mark, key, describe(a), describe(b)))
        if len(names) > 1:
            print("== %d modes in %s" % (len(names), elapsed(time.monotonic() - started)))
    finally:
        for side in sides:
            side.close()
    return 1 if failing else 0


if __name__ == "__main__":
    sys.exit(main())
