#!/usr/bin/env python3
"""
Print the vertical metrics Firefox's line box arithmetic reads, per font.

    tools/testing/vmetrics.py /path/to/consola.ttf ...
    tools/testing/vmetrics.py /path/to/collection.ttc      # every face in it
    tools/testing/vmetrics.py /path/to/collection.ttc:1    # one face
    tools/testing/vmetrics.py --sweep            # the families in the parity sweep

gfxFT2FontBase::InitMetrics takes maxAscent/maxDescent from FreeType's
size->metrics - which the shim rounds - unless the OS/2 USE_TYPO_METRICS bit
is set, in which case it takes them from sTypoAscender/sTypoDescender and the
shim's rounding never reaches layout. It then overrides the line height with
sTypoAscender - sTypoDescender + sTypoLineGap, scaled. So which of the three
metric triples a font carries, and whether that bit is set, decides which
fields the shim has to write. This prints all of it.

Column notes:
  useTypo   fsSelection bit 7. True means the shim's size->metrics rounding is
            bypassed for ascent and descent.
  typo=hhea Whether the sTypo* triple equals the hhea triple. Fonts differ
            here far more often than not; Consolas is the exception.
"""

import subprocess
import sys

try:
    from fontTools.ttLib import TTCollection, TTFont
except ImportError:
    raise SystemExit("needs fonttools: pip install fonttools")

# The parity sweep's six families, named here in full: where a
# distribution keeps Windows fonts is its own business, and fontconfig already
# knows. Firefox resolves them the same way, so this asks the same question it
# does.
SWEEP = ["Segoe UI", "Malgun Gothic", "Microsoft YaHei", "Yu Gothic",
         "Consolas", "Arial"]


def resolve(family):
    """The file fontconfig would give Firefox for this family, or None.

    fc-match substitutes rather than failing - ask it for a family nothing on
    the machine has and it still answers, with some other font. So the family
    it reports back is checked against the one asked for; otherwise a missing
    font shows up as a row of plausible numbers belonging to a different
    typeface.
    """
    try:
        out = subprocess.run(["fc-match", "-f", "%{family}\t%{file}", family],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    got, _, path = out.partition("\t")
    names = [n.strip().lower() for n in got.split(",")]
    return path.strip() if family.lower() in names else None

HEADER = ("%-26s %5s | %6s %6s %5s | %6s %6s %5s | %5s %5s | %-7s %s" %
          ("family", "upm", "tAsc", "tDesc", "tGap", "hAsc", "hDesc", "hGap",
           "winA", "winD", "useTypo", "typo=hhea"))


def faces(path):
    """Every face in the file, as (path, index) pairs.

    A .ttc holds several, and they are not variations on one design: Nirmala
    UI and Nirmala Text share a file and disagree about their vertical
    metrics, so reading only face 0 hides the one that matters. Pass
    "file.ttc:1" to ask for a single face.
    """
    if ":" in path and path.rsplit(":", 1)[1].isdigit():
        base, index = path.rsplit(":", 1)
        return [(base, int(index))]
    if not path.lower().endswith(".ttc"):
        return [(path, -1)]
    collection = TTCollection(path, lazy=True)
    count = len(collection.fonts)
    collection.close()
    return [(path, i) for i in range(count)]


def row(path, index=-1):
    font = TTFont(path, fontNumber=index, lazy=True)
    os2, hhea, head, name = (font["OS/2"], font["hhea"], font["head"],
                             font["name"])
    typo = (os2.sTypoAscender, os2.sTypoDescender, os2.sTypoLineGap)
    hh = (hhea.ascender, hhea.descender, hhea.lineGap)
    family = name.getDebugName(1) or path.rsplit("/", 1)[-1]
    style = name.getDebugName(2)
    if style and style.lower() not in ("regular", ""):
        family = "%s %s" % (family, style)
    line = ("%-26s %5d | %6d %6d %5d | %6d %6d %5d | %5d %5d | %-7s %s" %
            (family[:26], head.unitsPerEm, typo[0], typo[1], typo[2],
             hh[0], hh[1], hh[2], os2.usWinAscent, os2.usWinDescent,
             bool(os2.fsSelection & (1 << 7)), typo == hh))
    font.close()
    return line


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__.strip())
    if args == ["--sweep"]:
        paths, missing = [], []
        for family in SWEEP:
            path = resolve(family)
            (paths if path else missing).append(path or family)
    else:
        paths, missing = args, []

    print(HEADER)
    for path in paths:
        try:
            for base, index in faces(path):
                print(row(base, index))
        except Exception as err:
            print("%-26s %s" % (path.rsplit("/", 1)[-1][:26], err))
    for family in missing:
        print("%-26s not installed" % family[:26])


if __name__ == "__main__":
    main()
