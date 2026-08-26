#!/usr/bin/env python3
"""
gen_webfont_stress.py - web fonts to churn a browser's font handling.

    tools/testing/gen_webfont_stress.py <outdir> [--fonts N] [--pages M]
                                        [--base FONT.ttf ...]

Every font a page downloads reaches the shim as FT_New_Memory_Face, which is
the path where a leak is unbounded: a browsing session loads a new set of them
per site and never stops. This writes N genuinely distinct fonts - distinct to
the sanitiser Firefox rewrites them with, not merely distinct files - and M
pages that use them, so that two questions can be asked separately:

  * the same page over and over. Nothing new arrives, so the shim's count of
    distinct fonts handed to DirectWrite must not move.
  * every page in turn. New fonts arrive, so it must move by exactly as many
    as arrived, and no more.

Distinctness is made by renaming the family in the `name` table rather than by
padding the file: Firefox rewrites every web font through OTS before FreeType
sees it, and padding does not survive that - two padded copies would arrive as
one font and the first question would answer itself.
"""

import argparse
import os
import subprocess
import sys

try:
    from fontTools.ttLib import TTFont, TTCollection
except ImportError:
    raise SystemExit("needs fonttools: pip install fonttools")

# What the bases are chosen for is variety of outline count and table set,
# since how much a font costs to hold is what is being measured - not any
# particular font. So they are resolved through fontconfig rather than named
# by path, and this works on a machine with no Windows fonts on it. Override
# with --base to pin exact files.
BASE_QUERIES = [
    "sans-serif:bold",
    "serif",
    "monospace",
    ":lang=ja",          # a CJK face: many more outlines, a larger table set
]


def resolve_bases(explicit):
    """Base font files, either the ones asked for or fontconfig's answers."""
    if explicit:
        missing = [f for f in explicit if not os.path.exists(f)]
        if missing:
            raise SystemExit("no such font: %s" % ", ".join(missing))
        return explicit

    found = []
    for query in BASE_QUERIES:
        try:
            path = subprocess.run(["fc-match", "-f", "%{file}", query],
                                  capture_output=True, text=True,
                                  check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            continue
        # fc-match always answers something, so the same file can come back
        # for two queries on a sparse system; distinctness of the *bases* is
        # not required, only variety.
        if path and os.path.exists(path) and path not in found:
            found.append(path)
    if not found:
        raise SystemExit("fc-match resolved none of: %s (pass --base instead)"
                         % ", ".join(BASE_QUERIES))
    return found

SAMPLE = ("Hamburgefonstiv 0123456789 &amp; the quick brown fox jumps over "
          "the lazy dog. ACCUMULATION, waffle, Illegible 1lI0O")

SIZES = [11, 13, 16, 19, 24, 31, 42, 67]


def distinct_font(base, index, out_path):
    """A copy of `base` whose family name nobody else has."""
    font = TTFont(base, fontNumber=0 if base.endswith(".ttc") else -1)
    family = "DwcStress%04d" % index
    for record in font["name"].names:
        # 1 family, 4 full name, 6 PostScript name, 16 typographic family.
        if record.nameID in (1, 4, 6, 16):
            value = family if record.nameID != 6 else family.replace(" ", "")
            record.string = value.encode("utf-16-be" if record.platformID == 3
                                         else "latin-1")
    font.save(out_path)
    font.close()
    return family


def page_html(title, families, url_for):
    faces = "\n".join(
        "@font-face{font-family:'%s';src:url('%s') format('truetype');"
        "font-display:block}" % (f, url_for(f)) for f in families)
    blocks = []
    for f in families:
        for size in SIZES:
            blocks.append(
                "<p style=\"font-family:'%s';font-size:%dpx\">%s</p>" % (f, size, SAMPLE))
        blocks.append(
            "<p style=\"font-family:'%s';font-size:22px;font-weight:bold\">%s</p>"
            % (f, SAMPLE))
        blocks.append(
            "<p style=\"font-family:'%s';font-size:22px;font-style:italic\">%s</p>"
            % (f, SAMPLE))
    return ("<!doctype html><meta charset=utf-8><title>%s</title>\n"
            "<style>body{background:#fff;color:#111;margin:12px}\n%s</style>\n"
            "<h1>%s</h1>\n%s\n" % (title, faces, title, "\n".join(blocks)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--fonts", type=int, default=40, help="distinct fonts (default 40)")
    ap.add_argument("--pages", type=int, default=8, help="pages to spread them over")
    ap.add_argument("--base", action="append", default=[], metavar="FONT",
                    help="base font to copy from; repeatable. Default: fontconfig's "
                         "answers for %s" % ", ".join(BASE_QUERIES))
    args = ap.parse_args()

    bases = resolve_bases(args.base)

    fontdir = os.path.join(args.outdir, "f")
    os.makedirs(fontdir, exist_ok=True)
    families = []
    for i in range(args.fonts):
        name = "s%04d.ttf" % i
        family = distinct_font(bases[i % len(bases)], i, os.path.join(fontdir, name))
        families.append((family, "f/" + name))

    per_page = max(1, len(families) // args.pages)
    pages = []
    for p in range(args.pages):
        chunk = families[p * per_page:(p + 1) * per_page] or families[-per_page:]
        names = [f for f, _ in chunk]
        urls = dict(chunk)
        html = page_html("page %d" % p, names, lambda f: urls[f])
        page = "p%02d.html" % p
        with open(os.path.join(args.outdir, page), "w", encoding="utf-8") as fh:
            fh.write(html)
        pages.append(page)

    with open(os.path.join(args.outdir, "index.html"), "w", encoding="utf-8") as fh:
        fh.write("<!doctype html><meta charset=utf-8><title>webfont stress</title>\n" +
                 "\n".join("<p><a href='%s'>%s</a></p>" % (p, p) for p in pages))

    print("%d fonts, %d pages in %s" % (len(families), len(pages), args.outdir))
    for p in pages:
        print("  " + p)


if __name__ == "__main__":
    main()
