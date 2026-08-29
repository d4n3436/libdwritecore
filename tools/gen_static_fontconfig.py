#!/usr/bin/env python3
"""
A fontconfig configuration that makes a statically linked Chromium fall back
the way Windows does.

    tools/gen_static_fontconfig.py <windows-font-dir> <cache-dir> > fonts.conf
    FONTCONFIG_FILE=$PWD/fonts.conf fc-cache -f

Build the cache first, so a browser does not scan the directory itself on
startup.

A build that compiles fontconfig in exports none of its symbols and keeps no
symbol table, so the interposers in cleartype/src/fontconfig.cpp never load and
the per-script fallback order they impose is lost. That fontconfig still reads
configuration files, which is how the order goes back in.

Three things are written. The font directory is the Windows set alone, which
does at the configuration level what HideFromChromium does at the pattern
level, leaving a family Windows does not ship no way to be a candidate. Then
one rule per language prepends the families
cleartype/src/chromium/fallback_order.cpp lists for that language's script, in
its order. Last comes a single global order, appended weakly so a pattern that
names a family keeps it, for the far commoner case where no language reaches
fontconfig at all.

Chromium builds one fallback set per locale in
CachedFontSet::CreateFcFontSetForLocale, from FC_LANG and FcConfigSubstitute
through FcFontSort, and GetFallbackFontForChar walks that set taking the first
family covering the character. The order of the set therefore decides the
answer. Blink asks with FontDescription::LocaleOrDefault(), which falls back to
the UI locale, so a run's own language rarely reaches the pattern and the
global order carries most of the work.

Nothing here is a new opinion about which font suits a script. The ranges and
the family lists come from fallback_order.cpp, which languages a range belongs
to comes from fontconfig's own FcLangGetCharSet, and which families cover a
range comes from the font files themselves.
"""

import ctypes
import ctypes.util
import os
import subprocess
import re
import sys
import xml.sax.saxutils as sax

SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "cleartype", "src", "chromium", "fallback_order.cpp")


def family_lists(text):
    """Every `constexpr const char* kName[] = {...}` in the file."""
    out = {}
    for name, body in re.findall(
            r'constexpr const char\* (k\w+)\[\]\s*=\s*\{(.*?)\};', text, re.S):
        out[name] = re.findall(r'"([^"]*)"', body)
    return out


def han_choices(text):
    """HanForLocale's kChoices table: the subtag that settles a Han locale."""
    body = re.search(r'static const HanChoice kChoices\[\]\s*=\s*\{(.*?)\};',
                     text, re.S)
    if body is None:
        return {}
    return {sub: table for sub, table in
            re.findall(r'\{\s*"([^"]+)"\s*,\s*(k\w+)\s*,', body.group(1))}


def han_table(lang, choices):
    """The Han list for a locale, the way LocaleForHan settles it.

    The first subtag that disambiguates wins. A `zh` that settles nothing takes
    simplified Han, which is what ComputeScriptForHan falls back to. Languages
    that are not Han return None and keep their range-derived answer.
    """
    parts = [p for p in re.split(r'[-_.@]', lang.lower()) if p]
    if not parts or parts[0] not in ("zh", "ja", "ko"):
        return None
    if parts[0] == "ja":
        return "kKatakanaOrHiragana"
    if parts[0] == "ko":
        return "kHangul"
    for part in parts[1:]:
        if part in choices:
            return choices[part]
    return "kSimplifiedHan"


def script_ranges(text):
    """Every DWC_SCRIPT(first, last, table) row, in the order written."""
    rows = []
    for first, last, table in re.findall(
            r'DWC_SCRIPT\(\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(k\w+)\s*\)', text):
        rows.append((int(first, 16), int(last, 16), table))
    return rows


class Fontconfig:
    """Just enough of libfontconfig to ask what characters a language uses."""

    def __init__(self):
        path = ctypes.util.find_library("fontconfig")
        if path is None:
            raise SystemExit("libfontconfig not found")
        self.lib = ctypes.CDLL(path)
        self.lib.FcInitLoadConfigAndFonts.restype = ctypes.c_void_p
        self.lib.FcGetLangs.restype = ctypes.c_void_p
        self.lib.FcLangGetCharSet.restype = ctypes.c_void_p
        self.lib.FcLangGetCharSet.argtypes = [ctypes.c_char_p]
        self.lib.FcCharSetHasChar.restype = ctypes.c_int
        self.lib.FcCharSetHasChar.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.lib.FcStrSetDestroy.argtypes = [ctypes.c_void_p]
        self.lib.FcInit()

    def langs(self):
        """The language tags fontconfig knows, read out of its FcStrSet."""
        strset = self.lib.FcGetLangs()
        if not strset:
            return []
        # FcStrSet is opaque; FcStrList walks it.
        self.lib.FcStrListCreate.restype = ctypes.c_void_p
        self.lib.FcStrListCreate.argtypes = [ctypes.c_void_p]
        self.lib.FcStrListNext.restype = ctypes.c_char_p
        self.lib.FcStrListNext.argtypes = [ctypes.c_void_p]
        self.lib.FcStrListDone.argtypes = [ctypes.c_void_p]
        walker = self.lib.FcStrListCreate(strset)
        out = []
        while True:
            item = self.lib.FcStrListNext(walker)
            if not item:
                break
            out.append(item.decode("utf-8"))
        self.lib.FcStrListDone(walker)
        self.lib.FcStrSetDestroy(strset)
        return out

    def directory_charsets(self, font_dir):
        """Every family in the directory and the code points it covers.

        fc-scan reads the files themselves, so the answer does not depend on
        which configuration is active while the file is generated.
        """
        try:
            out = subprocess.run(
                ["fc-scan", "--format=%{family}\t%{charset}\n", font_dir],
                capture_output=True, text=True, timeout=300)
        except Exception:
            return {}
        found = {}
        for line in out.stdout.split("\n"):
            if "\t" not in line:
                continue
            names, charset = line.split("\t", 1)
            points = set()
            for span in charset.split():
                if "-" in span:
                    lo, _, hi = span.partition("-")
                    try:
                        lo, hi = int(lo, 16), int(hi, 16)
                    except ValueError:
                        continue
                    if hi - lo > 0x20000:
                        continue
                    points.update(range(lo, hi + 1))
                elif span:
                    try:
                        points.add(int(span, 16))
                    except ValueError:
                        pass
            # fc-scan lists a face's names comma separated, localized ones too.
            for name in names.split(","):
                name = name.strip()
                if name:
                    found.setdefault(name, set()).update(points)
        return found

    def covers(self, lang, first, last):
        """Whether this language uses any character in the range."""
        charset = self.lib.FcLangGetCharSet(lang.encode("utf-8"))
        if not charset:
            return 0
        hits = 0
        for c in range(first, min(last, first + 0x2000) + 1):
            if self.lib.FcCharSetHasChar(charset, c):
                hits += 1
                if hits >= 4:       # more than an incidental borrowing
                    break
        return hits


# Weight added to an edge the table states outright, so it outranks every
# edge inferred from coverage no matter how large a range asks for that one.
WRITTEN_ORDER_WEIGHT = 1 << 20


def fallback_order(lists, ranges, fc, font_dir):
    """The families in an order that answers every script the way Windows does.

    Two kinds of edge. Within one script's list the order is already written
    down. Across scripts, the family named first has to come before any other
    listed family that covers the same range, or that one answers instead.
    """
    families = []
    for _, _, table in ranges:
        for f in lists.get(table, ()):
            if f not in families:
                families.append(f)

    known = fc.directory_charsets(font_dir)
    covers = {f: known.get(f, set()) for f in families}

    # Each edge carries the number of code points asking for it, so breaking a
    # cycle drops the edge that serves the fewer characters.
    edges = {}
    def want(a, b, weight):
        edges[(a, b)] = edges.get((a, b), 0) + weight

    for first, last, table in ranges:
        # ScriptToFontMap::FirstAvailableFont picks on presence, so a list
        # whose first names are not installed is answered by the first one
        # that is, and that is the family the others have to come after.
        row = [f for f in lists.get(table, ()) if covers.get(f)]
        if not row:
            continue
        weight = last - first + 1
        # An order written down in the table outranks one inferred from what a
        # family happens to cover, so a cycle gives up the inference first.
        for i in range(len(row)):
            for j in range(i + 1, len(row)):
                want(row[i], row[j], WRITTEN_ORDER_WEIGHT + weight)
        for other in families:
            if other in row or not covers.get(other):
                continue
            if any(c in covers[other] for c in sample_range(first, last)):
                want(row[0], other, weight)

    # A family with no file behind it only adds noise to the sort.
    return toposort([f for f in families if covers.get(f)], edges)


def sample_range(first, last):
    """A few code points from a range, enough to tell coverage apart."""
    span = last - first + 1
    step = max(1, span // 8)
    return range(first, last + 1, step)


def toposort(nodes, edges):
    """Kahn's algorithm, dropping the lightest edge that closes a cycle."""
    dropped = []
    edges = dict(edges)
    while True:
        # Kept in insertion order, so the same input always sorts the same
        # way; iterating a set here leaves the order up to string hashing.
        after = {n: [] for n in nodes}
        indeg = {n: 0 for n in nodes}
        for a, b in sorted(edges):
            if b not in after[a]:
                after[a].append(b)
                indeg[b] += 1
        ready = [n for n in nodes if indeg[n] == 0]
        out = []
        while ready:
            n = ready.pop(0)
            out.append(n)
            for m in after[n]:
                indeg[m] -= 1
                if indeg[m] == 0:
                    ready.append(m)
        if len(out) == len(nodes):
            return out, dropped
        # Something is left in a cycle; drop one of its edges and try again.
        stuck = [n for n in nodes if n not in out]
        inside = [e for e in edges if e[0] in stuck and e[1] in stuck]
        if not inside:
            return out + stuck, dropped
        weakest = min(inside, key=lambda e: (edges[e], e))
        dropped.append((weakest[0], weakest[1], edges[weakest]))
        del edges[weakest]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    font_dir, cache_dir = sys.argv[1], sys.argv[2]

    text = open(SOURCE, encoding="utf-8").read()
    lists = family_lists(text)
    ranges = script_ranges(text)

    fc = Fontconfig()
    choices = han_choices(text)

    # A language takes the families of the range it uses most characters from,
    # so a script that borrows a few letters from another does not take it over.
    best = {}
    for lang in fc.langs():
        top = None
        for first, last, table in ranges:
            if table not in lists:
                continue
            hits = fc.covers(lang, first, last)
            if hits and (top is None or hits > top[0]):
                top = (hits, table)
        # A Han locale is settled by its subtags, not by the block it shares
        # with the other two, which no range can tell apart.
        han = han_table(lang, choices)
        if han is not None and han in lists:
            best[lang] = han
        elif top is not None:
            best[lang] = top[1]

    out = ['<?xml version="1.0"?>',
           '<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">',
           '<fontconfig>',
           '  <!-- Generated by tools/gen_static_fontconfig.py. Do not edit. -->',
           '  <dir>%s</dir>' % sax.escape(font_dir),
           '  <cachedir>%s</cachedir>' % sax.escape(cache_dir)]

    for lang in sorted(best):
        families = lists[best[lang]]
        if not families:
            continue
        out.append('  <match target="pattern">')
        out.append('    <test name="lang" compare="contains"><string>%s</string></test>'
                   % sax.escape(lang))
        # Prepended last to first, so the list ends up in the order written.
        for family in reversed(families):
            out.append('    <edit name="family" mode="prepend" binding="strong">'
                       '<string>%s</string></edit>' % sax.escape(family))
        out.append('  </match>')

    # Blink asks with FontDescription::LocaleOrDefault(), which falls back to
    # the UI locale, so a run's own language almost never reaches fontconfig
    # and the rules above rarely fire. One set is built per locale and
    # GetFallbackFontForChar walks it taking the first family covering the
    # character, so the order of that one set decides nearly everything.
    #
    # The order has to satisfy, for every script, that the family
    # fallback_order.cpp names first precedes any other family that also
    # covers that script. Those are edges in a graph; the order is its
    # topological sort. An edge that would close a cycle is dropped, since two
    # scripts can genuinely disagree about two families that both cover both.
    order, dropped = fallback_order(lists, ranges, fc, font_dir)
    for a, b, weight in dropped:
        sys.stderr.write("cycle: %s before %s dropped (%d code points)\n"
                         % (a, b, weight))
    if order:
        out.append('  <match target="pattern">')
        for family in order:
            out.append('    <edit name="family" mode="append" binding="weak">'
                       '<string>%s</string></edit>' % sax.escape(family))
        out.append('  </match>')

    out.append('</fontconfig>')
    print("\n".join(out))


if __name__ == "__main__":
    main()
