#!/usr/bin/env python3
"""
What a Windows Firefox actually *drew* per script, turned into Firefox
per-language font prefs so Linux falls back to the same families.

    tools/fallback_from_probe.py                            # inlined measurement
    tools/fallback_from_probe.py [probe.json [greprefs.js]]
    tools/fallback_from_probe.py --measure <host> <port> > measured.json
    tools/fallback_from_probe.py --measured measured.json

gen_firefox_parity.py transcribes Firefox's *defaults*, which is a different
answer: a default names a list, and only a browser running on a machine with
those fonts says which entry rendered. tools/testing/pages/probe-fallback.html
measures that, and this turns it into font.name-list lines a profile can use.

With no arguments both Windows-side inputs come from the measurement inlined
at the bottom of this file, so it needs nothing but a checkout and resolves
against whatever fonts are installed here now. Pass files to substitute a
different probe or a different Firefox's defaults; "-" as the first argument
keeps the inlined probe, and /dev/null as the second drops Firefox's own
defaults so only what the probe measured is emitted. Both fc-list calls read
the system fontconfig, which is what the browser reads too now that
cleartype/src/fontconfig.cpp answers the settings itself.

Two halves, because neither side can answer alone.

The probe runs on Windows and reports, per script, every installed family
whose metrics match what the browser actually drew. That set contains the
family DirectWrite chose - and also every family that does *not* have the
character, since those fell back to the same font and measure identically.
A browser cannot tell those apart: CSS has no way to disable fallback.

This side can. The same font files are installed here, so fontconfig's own
charset index says which families really contain the character, and the
decoys drop out. What is left is the answer.

Where two survivors are the same design at the same widths, though, even that
cannot separate them - Nirmala Text and Nirmala UI are two faces of one .ttc
and measure identically - and the tie was being broken by list order, which is
not evidence. --measure removes the guesswork: it drives a *running* Windows
Firefox over Marionette and asks the layout engine, through
InspectorUtils.getUsedFontFaces, which family it actually shaded the character
with. That is not a set of candidates, it is the answer, per generic. Feed the
result back with --measured and it leads each list.

    tools/testing/run_parity_firefox.sh guest <domain> --port 2929
    tools/fallback_from_probe.py --measure <windows-host> 2929 > measured.json
    tools/fallback_from_probe.py --measured measured.json

The output is font.name-list.<generic>.<langGroup>, deliberately, and not a
fontconfig rule. Firefox consults these before it asks fontconfig, and they
are keyed on its own language group and not on family matching, so unlike a
<match target="pattern"> rule they cannot fire on a font a page named
explicitly. Expressing it in fontconfig instead turns Consolas into
Courier New on every page that asks for it by name.
"""

import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "testing"))

# Scripts with no pref language group of their own. gfxPlatformFontList::
# GetFontPrefLangFor(uint32_t) switches on the Unicode block and ends in
# "default: return eFontPrefLang_Others", which is the x-unicode key - so all
# of these land in one list, and every one of them was a family that Windows
# and Linux disagreed about on a page of multi-script samples.
#
# They live here and not in PROBE, because PROBE is a *measurement*, with
# candidate lists this file cannot invent. These carry only the character to
# ask about; --measure supplies the answer.
OTHERS = {
    "Gothic":    ("U+10330", "x-unicode"),
    "Lao":       ("U+0E81",  "x-unicode"),
    "Mongolian": ("U+1820",  "x-unicode"),
    "Ogham":     ("U+1680",  "x-unicode"),
    "Runic":     ("U+16A0",  "x-unicode"),
}


def inline_probe():
    """The inlined measurement, in the shape json.load would have produced."""
    return {script: {"codepoint": codepoint, "lang": lang,
                     "candidates": {generic: [FAMILIES[i] for i in CANDIDATE_LISTS[k]]
                                    for generic, k in cands.items()}}
            for script, (codepoint, lang, cands) in PROBE.items()}


def families_covering(codepoint):
    """Families on this machine that really contain the character."""
    query = ":charset=%s" % codepoint.removeprefix("U+").lower()
    try:
        out = subprocess.run(["fc-list", query, "family"], capture_output=True,
                             text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return set()
    names = set()
    for line in out.splitlines():
        for name in line.split(","):
            if name.strip():
                names.add(name.strip())
    return names


def firefox_defaults(path):
    """font.name-list values from a Windows Firefox greprefs.js.

    These are what Firefox *asks* for on Windows, which is not the same as what
    it draws with: the lists name families that no stock Windows install has
    (Kokila, Latha, Shruti, Aboriginal Sans), and Firefox skips those and falls
    through to the OS. Keeping them is still right - Firefox on Linux skips
    them the same way - and it preserves the real ordering, which the probe
    cannot recover.
    """
    out = {}
    # font.name-list.<generic>.<langGroup>, and also the one that has no
    # language group at all: font.name-list.emoji.
    pattern = re.compile(r'pref\("font\.name-list\.([a-z-]+)(?:\.([\w-]+))?",\s*"([^"]*)"\)')
    with open(path, encoding="utf8") as handle:
        for line in handle:
            m = pattern.search(line)
            if m and m.group(3).strip():
                out[(m.group(1), m.group(2))] = [f.strip() for f in m.group(3).split(",")]
    return out


# A BCP-47 tag for each pref language group, so the measurement asks the
# question the pref key actually encodes. Without it the element has no
# language, and gfxPlatformFontList::AppendCJKPrefLangs orders the CJK groups
# by the page's language and the user's locale - so an English page measures
# Hiragana as SimSun, a Chinese font, and writing that into font.name-list.
# serif.ja would then break Japanese pages. x-unicode and x-math have no
# language by definition and are measured without one.
SAMPLE_LANG = {
    "ar": "ar", "el": "el", "he": "he", "ja": "ja", "ko": "ko", "th": "th",
    "x-armn": "hy", "x-beng": "bn", "x-cans": "iu", "x-cyrillic": "ru",
    "x-devanagari": "hi", "x-ethi": "am", "x-geor": "ka", "x-gujr": "gu",
    "x-guru": "pa", "x-khmr": "km", "x-knda": "kn", "x-mlym": "ml",
    "x-orya": "or", "x-sinh": "si", "x-tamil": "ta", "x-telu": "te",
    "x-tibt": "bo", "zh-CN": "zh-CN", "zh-TW": "zh-TW",
}

MEASURE_SCRIPT = r"""
var rows = arguments[0];
var out = [];
document.body.textContent = "";
for (var i = 0; i < rows.length; i++) {
  var div = document.createElement("div");
  if (rows[i][4]) { div.setAttribute("lang", rows[i][4]); }
  div.style.fontFamily = rows[i][2];
  div.textContent = String.fromCodePoint(rows[i][3]);
  document.body.appendChild(div);
  var range = document.createRange();
  range.selectNodeContents(div);
  var names = [];
  try {
    var faces = InspectorUtils.getUsedFontFaces(range, 0, true);
    for (var f = 0; f < faces.length; f++) { names.push(faces[f].name); }
  } catch (e) {
    return {error: String(e)};
  }
  out.push([rows[i][0], rows[i][1], rows[i][2], names]);
}
return {rows: out};
"""


def measure(host, port):
    """Ask a running Windows Firefox which family it draws each script with.

    One element per script and generic, holding nothing but the representative
    character, so the reported face list has exactly one entry: the family that
    claimed the character. No Latin text is included, because the generic's own
    Latin font would be reported first and say nothing.
    """
    from marionette import Marionette

    rows = []
    for script, (codepoint, lang, candidates) in sorted(PROBE.items()):
        for generic in sorted(candidates):
            rows.append([script, lang, generic,
                         int(codepoint.removeprefix("U+"), 16),
                         SAMPLE_LANG.get(lang, "")])
    for script, (codepoint, lang) in sorted(OTHERS.items()):
        for generic in ("monospace", "sans-serif", "serif"):
            rows.append([script, lang, generic,
                         int(codepoint.removeprefix("U+"), 16),
                         SAMPLE_LANG.get(lang, "")])

    client = Marionette(host, port)
    client.start("content")
    client.call("WebDriver:Navigate", {"url": "data:text/html;charset=utf-8,"})
    result = client.script(MEASURE_SCRIPT, [rows], sandbox="system")
    if not result or result.get("error"):
        sys.exit("measurement failed: %s"
                 % (result or {}).get("error", "no result"))

    measured = {}
    for script, lang, generic, names in result["rows"]:
        if not names:
            continue
        key = "%s|%s" % (generic, lang)
        have = measured.setdefault(key, [])
        for name in names:
            if name not in have:
                have.append(name)
    return measured


def strip_face(name, installed):
    """"Nirmala UI Bold" is a face; the pref wants the family it belongs to.

    getUsedFontFaces reports the face, style included. The pref list is matched
    against family names, so a trailing style word has to come off - but only
    when what is left is a family this machine really has, since plenty of
    families end in a word that looks like a style.
    """
    if name in installed:
        return name
    words = name.split()
    while len(words) > 1:
        words.pop()
        candidate = " ".join(words)
        if candidate in installed:
            return candidate
    return name


def installed_families():
    try:
        out = subprocess.run(["fc-list", ":", "family"], capture_output=True,
                             text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return set()
    return {name.strip()
            for line in out.splitlines()
            for name in line.split(",") if name.strip()}


def main():
    argv = sys.argv[1:]
    if argv[:1] == ["--measure"]:
        if len(argv) != 3:
            sys.exit("usage: fallback_from_probe.py --measure <host> <port>")
        json.dump(measure(argv[1], int(argv[2])), sys.stdout, indent=1,
                  sort_keys=True)
        print()
        return

    raw = dict(MEASURED)
    if argv[:1] == ["--measured"]:
        if len(argv) < 2:
            sys.exit("usage: fallback_from_probe.py --measured <file.json> ...")
        with open(argv[1], encoding="utf8") as handle:
            raw = json.load(handle)
        argv = argv[2:]

    # Only families this machine has. A name that is not installed here cannot
    # lead the list - Firefox would skip it and the ordering would be a lie.
    measured = {}
    if raw:
        here = installed_families()
        for key, names in raw.items():
            kept = []
            for name in (strip_face(n, here) for n in names):
                if name in here and name not in kept:
                    kept.append(name)
            if kept:
                measured[key] = kept

    if len(argv) > 2:
        sys.exit(__doc__.strip())
    probe_path = argv[0] if argv else "-"
    if probe_path == "-":
        probe = inline_probe()
    else:
        with open(probe_path, encoding="utf8") as handle:
            probe = json.load(handle)
    defaults = (firefox_defaults(argv[1]) if len(argv) > 1
                else dict(FIREFOX_WINDOWS_DEFAULTS))

    # Firefox's Windows defaults name families that no Windows machine has -
    # the Noto faces are there for other platforms' copies of the same list.
    # On Windows they never resolve; on Linux they do, so keeping them makes
    # Linux pick a font Windows would have skipped. Measured: serif.ko led with
    # Noto Serif CJK KR here and Malgun Gothic there, and the two rendered at
    # different widths. The probe's candidate list is the set of families the
    # Windows machine actually had, so it is also the filter.
    # noinspection PyUnboundLocalVariable
    installed = {family
                 for info in probe.values()
                 for names in info["candidates"].values()
                 for family in names}
    if installed:
        for key, names in list(defaults.items()):
            # Including when nothing survives. Windows resolved none of the
            # list there, so the family that actually drew is whatever the
            # probe observed, and that is appended below.
            #
            # Suppressing the key entirely instead - letting fontconfig
            # assemble the run the way Windows' OS fallback did - was tried and
            # is much worse: 35 of 54 script samples matched against 52, because
            # fontconfig picks different families from Windows for Indic,
            # Armenian, Georgian and Khmer.
            defaults[key] = [n for n in names if n in installed]

    print("// >>> BEGIN GENERATED - tools/fallback_from_probe.py")

    # Several scripts share one language group - Hiragana and Katakana are both
    # ja - so collect first and emit once, since writing the same key twice
    # would let the last one silently win.
    merged = {}   # (generic, lang) -> [survivors]
    notes = {}    # (generic, lang) -> [script names that contributed]
    skipped = []

    for script, info in probe.items():
        covering = families_covering(info["codepoint"])
        for generic, candidates in info["candidates"].items():
            real = [f for f in candidates if f in covering]
            key = (generic, info["lang"])
            if not real:
                skipped.append((script, info["codepoint"], generic, len(candidates)))
                continue
            notes.setdefault(key, []).append(script)
            # Union, in first-seen order: the pref is an ordered fallback list,
            # so two scripts sharing a group each contribute. Emoji and Symbol
            # are both x-unicode and need Segoe UI Emoji *and* Segoe UI Symbol.
            have = merged.setdefault(key, [])
            have.extend(f for f in real if f not in have)

    # The probe reports its matches alphabetically - it has no idea which one
    # Windows preferred, only that they measured the same. Usually that does
    # not matter, because only one distinct font survives the coverage filter
    # and the other names are aliases of it. When several do survive, lead
    # with the one Windows uses for that generic in Latin text, so `serif`
    # does not open with Arial.
    latin_default = {"serif": "Times New Roman", "sans-serif": "Arial",
                     "monospace": "Courier New"}
    ambiguous_above = 4

    for key in sorted(set(merged) | set(defaults), key=lambda k: (k[0], k[1] or "")):
        generic, lang = key
        real = merged.get(key, [])
        first = latin_default.get(generic)
        if first in real:
            real = [first] + [f for f in real if f != first]

        # Firefox's own Windows list first, in its own order, then whatever the
        # probe saw actually drawn. The first part reproduces what Firefox asks
        # for; the second catches the case where none of those are installed
        # and Windows fell through to the OS - without it, Linux would fall
        # through to fontconfig instead and pick something else.
        # Three layers, in this order:
        #
        #   what Firefox asks for on Windows, in its own order;
        #   what a Windows Firefox was *seen* to draw with (--measure), which
        #     is the only layer that can separate two faces of one file;
        #   whatever else the probe's metric match left standing.
        #
        # The middle layer is why Tamil is right: the probe cannot tell Nirmala
        # Text from Nirmala UI, and sans-serif Tamil resolves to Nirmala UI on
        # Windows while serif and monospace resolve to Nirmala Text - a
        # per-generic split no set of candidates encodes.
        names = list(defaults.get(key, []))
        unmeasured = list(names)
        seen = measured.get("%s|%s" % (generic, lang or ""), [])
        names += [f for f in seen if f not in names]
        # ...but only when the probe actually decided something. Advance width
        # cannot separate CJK fonts - ideographs are full-width in all of them
        # - so those cells come back with every covering family in them. That
        # is not an ordering, it is noise, and appending it would let a Chinese
        # font shadow the Japanese list if Yu Gothic were ever missing.
        if len(real) <= ambiguous_above:
            names += [f for f in real if f not in names]
            unmeasured += [f for f in real if f not in unmeasured]
        if not names:
            continue
        why = ", ".join(notes.get(key, [])) or "Firefox default"
        # Only where the measurement changed the answer. Marking every line it
        # merely confirmed would say nothing; this way the mark points at the
        # cells the metric match got wrong.
        if names != unmeasured:
            why += " (measured)"
        name = "font.name-list.%s.%s" % (generic, lang) if lang else \
               "font.name-list.%s" % generic
        print('user_pref("%s", "%s");  // %s' % (name, ", ".join(names), why))

    for script, codepoint, generic, n in skipped:
        print("// %-20s %-8s %-12s no candidate is installed here%s"
              % (script, codepoint, generic,
                 " (%d matched on Windows)" % n if n else ""))

    print("// <<< END GENERATED")


# ---------------------------------------------------------------------------
# The direct measurement, inlined.
#
# Produced by --measure against a *stock* Windows 11 Firefox - no parity prefs
# in the profile, or the answer would only be what those prefs already say.
# Each row is the family that Firefox reported having shaded the script's
# representative character with, for one CSS generic, with the element tagged
# in that script's language.
#
# This is the layer the metric-matching probe cannot produce: it names one
# family instead of a set, and it distinguishes generics. Regenerate it with
#
#     tools/testing/run_parity_firefox.sh guest <domain> --port 2929 --no-prefs
#     tools/fallback_from_probe.py --measure <guest-ip> 2929
#
# ---------------------------------------------------------------------------

MEASURED = {
    "monospace|ar": ["Arial"],
    "monospace|el": ["Consolas"],
    "monospace|he": ["Courier New"],
    "monospace|ja": ["MS Gothic"],
    "monospace|ko": ["Malgun Gothic"],
    "monospace|th": ["Tahoma"],
    "monospace|x-armn": ["Arial"],
    "monospace|x-beng": ["Nirmala Text"],
    "monospace|x-cans": ["Gadugi"],
    "monospace|x-cyrillic": ["Consolas"],
    "monospace|x-devanagari": ["Nirmala UI"],
    "monospace|x-ethi": ["Ebrima"],
    "monospace|x-geor": ["Segoe UI"],
    "monospace|x-gujr": ["Nirmala Text"],
    "monospace|x-guru": ["Nirmala Text"],
    "monospace|x-khmr": ["Leelawadee UI"],
    "monospace|x-knda": ["Nirmala Text"],
    "monospace|x-math": ["Consolas"],
    "monospace|x-mlym": ["Nirmala Text"],
    "monospace|x-orya": ["Nirmala Text"],
    "monospace|x-sinh": ["Nirmala Text"],
    "monospace|x-tamil": ["Nirmala Text"],
    "monospace|x-telu": ["Nirmala Text"],
    "monospace|x-tibt": ["Microsoft Himalaya"],
    "monospace|x-unicode": ["Segoe UI Emoji", "Segoe UI Symbol", "Segoe UI Historic", "Leelawadee UI", "Mongolian Baiti"],
    "monospace|zh-CN": ["NSimSun"],
    "monospace|zh-TW": ["NSimSun"],
    "sans-serif|ar": ["Segoe UI"],
    "sans-serif|el": ["Arial"],
    "sans-serif|he": ["Arial"],
    "sans-serif|ja": ["Yu Gothic Regular"],
    "sans-serif|ko": ["Malgun Gothic"],
    "sans-serif|th": ["Tahoma"],
    "sans-serif|x-armn": ["Arial"],
    "sans-serif|x-beng": ["Nirmala Text"],
    "sans-serif|x-cans": ["Gadugi"],
    "sans-serif|x-cyrillic": ["Arial"],
    "sans-serif|x-devanagari": ["Nirmala UI"],
    "sans-serif|x-ethi": ["Ebrima"],
    "sans-serif|x-geor": ["Segoe UI"],
    "sans-serif|x-gujr": ["Nirmala Text"],
    "sans-serif|x-guru": ["Nirmala Text"],
    "sans-serif|x-khmr": ["Leelawadee UI"],
    "sans-serif|x-knda": ["Nirmala Text"],
    "sans-serif|x-math": ["Arial"],
    "sans-serif|x-mlym": ["Nirmala Text"],
    "sans-serif|x-orya": ["Nirmala Text"],
    "sans-serif|x-sinh": ["Nirmala Text"],
    "sans-serif|x-tamil": ["Nirmala UI"],
    "sans-serif|x-telu": ["Nirmala Text"],
    "sans-serif|x-tibt": ["Microsoft Himalaya"],
    "sans-serif|x-unicode": ["Segoe UI Emoji", "Segoe UI Symbol", "Segoe UI Historic", "Leelawadee UI", "Mongolian Baiti"],
    "sans-serif|zh-CN": ["Microsoft YaHei"],
    "sans-serif|zh-TW": ["Microsoft JhengHei"],
    "serif|ar": ["Times New Roman"],
    "serif|el": ["Times New Roman"],
    "serif|he": ["Arial"],
    "serif|ja": ["Yu Gothic Regular"],
    "serif|ko": ["Malgun Gothic"],
    "serif|th": ["Tahoma"],
    "serif|x-armn": ["Sylfaen"],
    "serif|x-beng": ["Nirmala Text"],
    "serif|x-cans": ["Gadugi"],
    "serif|x-cyrillic": ["Times New Roman"],
    "serif|x-devanagari": ["Nirmala Text"],
    "serif|x-ethi": ["Ebrima"],
    "serif|x-geor": ["Sylfaen"],
    "serif|x-gujr": ["Nirmala Text"],
    "serif|x-guru": ["Nirmala Text"],
    "serif|x-khmr": ["Leelawadee UI"],
    "serif|x-knda": ["Nirmala Text"],
    "serif|x-math": ["Times New Roman"],
    "serif|x-mlym": ["Nirmala Text"],
    "serif|x-orya": ["Nirmala Text"],
    "serif|x-sinh": ["Nirmala Text"],
    "serif|x-tamil": ["Nirmala Text"],
    "serif|x-telu": ["Nirmala Text"],
    "serif|x-tibt": ["Microsoft Himalaya"],
    "serif|x-unicode": ["Segoe UI Emoji", "Segoe UI Symbol", "Segoe UI Historic", "Leelawadee UI", "Mongolian Baiti"],
    "serif|zh-CN": ["SimSun"],
    "serif|zh-TW": ["SimSun"],
}


# ---------------------------------------------------------------------------
# The Windows-side measurement, inlined.
#
# Inlined instead of read from files, so regenerating needs nothing but a
# checkout: 87 candidate lists over 29 scripts drawn from 125 family names, of
# which only 20 lists are distinct, kept as a name table plus indices.
#
# Provenance: tools/testing/pages/probe-fallback.html run on a Windows 11 install under
# that machine's own Firefox, plus the font.name-list lines from the same
# Firefox's greprefs.js inside omni.ja. Substitute a different measurement by
# passing files as arguments; see the module docstring.
#
# FIREFOX_WINDOWS_DEFAULTS is derived from Mozilla's greprefs.js, which ships
# under MPL-2.0.
# ---------------------------------------------------------------------------

# Every family the probe saw installed on the Windows machine.
FAMILIES = (
    'Arial', 'Arial Black', 'Bahnschrift', 'Bahnschrift Condensed',
    'Bahnschrift Light', 'Bahnschrift Light Condensed',
    'Bahnschrift Light SemiCondensed', 'Bahnschrift SemiBold',
    'Bahnschrift SemiBold Condensed', 'Bahnschrift SemiBold SemiConden',
    'Bahnschrift SemiCondensed', 'Bahnschrift SemiLight',
    'Bahnschrift SemiLight Condensed', 'Bahnschrift SemiLight SemiConde',
    'Calibri', 'Calibri Light', 'Cambria', 'Cambria Math', 'Candara',
    'Candara Light', 'Comic Sans MS', 'Consolas', 'Constantia', 'Corbel',
    'Corbel Light', 'Courier New', 'Ebrima', 'Franklin Gothic Medium',
    'Gabriola', 'Gadugi', 'Georgia', 'Impact', 'Ink Free', 'Javanese Text',
    'Leelawadee UI', 'Leelawadee UI Semilight', 'Lucida Console',
    'Lucida Sans Unicode', 'MS Gothic', 'MS PGothic', 'MS UI Gothic',
    'MV Boli', 'Malgun Gothic', 'Malgun Gothic Semilight', 'Marlett',
    'Microsoft Himalaya', 'Microsoft JhengHei', 'Microsoft JhengHei Light',
    'Microsoft JhengHei UI', 'Microsoft JhengHei UI Light',
    'Microsoft New Tai Lue', 'Microsoft PhagsPa', 'Microsoft Sans Serif',
    'Microsoft Tai Le', 'Microsoft YaHei', 'Microsoft YaHei Light',
    'Microsoft YaHei UI', 'Microsoft YaHei UI Light', 'Microsoft Yi Baiti',
    'MingLiU-ExtB', 'MingLiU_HKSCS-ExtB', 'MingLiU_MSCS-ExtB',
    'Mongolian Baiti', 'Myanmar Text', 'NSimSun', 'Nirmala Text',
    'Nirmala Text Semilight', 'Nirmala UI', 'Nirmala UI Semilight',
    'PMingLiU-ExtB', 'Palatino Linotype', 'Sans Serif Collection',
    'Segoe Fluent Icons', 'Segoe MDL2 Assets', 'Segoe Print', 'Segoe Script',
    'Segoe UI', 'Segoe UI Black', 'Segoe UI Emoji', 'Segoe UI Historic',
    'Segoe UI Light', 'Segoe UI Semibold', 'Segoe UI Semilight',
    'Segoe UI Symbol', 'Segoe UI Variable Display',
    'Segoe UI Variable Display Light', 'Segoe UI Variable Display Semib',
    'Segoe UI Variable Display Semil', 'Segoe UI Variable Small',
    'Segoe UI Variable Small Light', 'Segoe UI Variable Small Semibol',
    'Segoe UI Variable Small Semilig', 'Segoe UI Variable Text',
    'Segoe UI Variable Text Light', 'Segoe UI Variable Text Semibold',
    'Segoe UI Variable Text Semiligh', 'SimSun', 'SimSun-ExtB',
    'Sitka Banner', 'Sitka Banner Semibold', 'Sitka Display',
    'Sitka Display Semibold', 'Sitka Heading', 'Sitka Heading Semibold',
    'Sitka Small', 'Sitka Small Semibold', 'Sitka Subheading',
    'Sitka Subheading Semibold', 'Sitka Text', 'Sitka Text Semibold',
    'Sylfaen', 'Symbol', 'Tahoma', 'Times New Roman', 'Trebuchet MS',
    'Verdana', 'Webdings', 'Wingdings', 'Yu Gothic', 'Yu Gothic Light',
    'Yu Gothic Medium', 'Yu Gothic UI', 'Yu Gothic UI Light',
    'Yu Gothic UI Semibold', 'Yu Gothic UI Semilight',
)

# The distinct candidate lists, as indices into FAMILIES. A list holds every
# family whose metrics matched what the browser actually drew - which includes
# the ones that lack the character entirely and fell back to the same font.
# Telling those apart is what the fontconfig charset filter does.
CANDIDATE_LISTS = (
    # 0
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
     20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
     37, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
     58, 59, 60, 61, 62, 38, 39, 40, 41, 63, 65, 66, 67, 68, 64, 70, 69,
     71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87,
     88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103,
     104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117,
     118, 119, 120, 121, 122, 123, 124,),
    # 1
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
     20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
     37, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
     58, 59, 60, 61, 62, 38, 41, 63, 65, 66, 67, 68, 64, 70, 69, 71, 72,
     73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
     90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105,
     106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
     120,),
    # 2
    (0, 52,),
    # 3
    (3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 26, 29, 33, 34, 35, 44, 45, 50,
     51, 53, 58, 59, 60, 61, 62, 41, 63, 65, 66, 67, 68, 69, 72, 73, 78,
     79, 83, 85, 86, 87, 89, 90, 91, 93, 94, 95, 97, 99, 101, 103, 105,
     107, 109, 111, 113, 116, 117,),
    # 4
    (21,),
    # 5
    (3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 26, 29, 33, 34, 35, 44, 45, 50,
     51, 53, 58, 59, 60, 61, 62, 41, 63, 65, 66, 67, 68, 69, 72, 73, 78,
     79, 85, 86, 87, 89, 90, 91, 93, 94, 95, 97, 99, 101, 103, 105, 107,
     109, 111, 113, 116, 117,),
    # 6
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
     20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
     37, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
     58, 59, 60, 61, 62, 38, 39, 40, 41, 63, 65, 67, 64, 70, 69, 71, 72,
     73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
     90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105,
     106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
     120, 121, 122, 123, 124,),
    # 7
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
     20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 36, 37, 42,
     43, 44, 45, 46, 47, 48, 49, 50, 51, 53, 54, 55, 56, 57, 58, 59, 60,
     61, 62, 38, 39, 40, 41, 63, 65, 66, 67, 68, 64, 70, 69, 71, 72, 73,
     74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90,
     91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105,
     106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
     120, 121, 122, 123, 124,),
    # 8
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21,
     22, 23, 24, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 42, 43, 44,
     45, 46, 47, 48, 49, 50, 51, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62,
     38, 39, 40, 41, 63, 65, 66, 67, 68, 64, 70, 69, 71, 72, 73, 74, 75,
     77, 78, 79, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96,
     97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
     111, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124,),
    # 9
    (25, 52,),
    # 10
    (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21,
     22, 23, 24, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 42, 43,
     44, 45, 46, 47, 48, 49, 50, 51, 53, 54, 55, 56, 57, 58, 59, 60, 61,
     62, 38, 39, 40, 41, 63, 65, 66, 67, 68, 64, 70, 69, 72, 73, 74, 75,
     76, 77, 78, 79, 81, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94,
     95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
     110, 111, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124,),
    # 11
    (0, 113,),
    # 12
    (0,),
    # 13
    (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 18, 19, 22, 23, 24, 26,
     27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 42, 43, 44, 45, 46, 47, 48,
     49, 50, 51, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 38, 39, 40, 41,
     63, 65, 66, 67, 68, 64, 70, 69, 71, 72, 73, 74, 75, 77, 78, 79, 83,
     84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100,
     101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 116, 117, 118,
     119, 120, 121, 122, 123, 124,),
    # 14
    (76,),
    # 15
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21,
     22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 42,
     43, 44, 45, 46, 47, 48, 49, 50, 51, 53, 54, 55, 56, 57, 58, 59, 60,
     61, 62, 38, 39, 40, 41, 63, 65, 66, 67, 68, 64, 70, 69, 71, 72, 73,
     74, 75, 77, 78, 79, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94,
     95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
     110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123,
     124,),
    # 16
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
     20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
     37, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
     58, 59, 60, 61, 62, 38, 39, 40, 41, 63, 65, 66, 67, 68, 64, 70, 69,
     71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 84, 85, 86, 87, 88,
     89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104,
     105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118,
     119, 120, 121, 122, 123, 124,),
    # 17
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
     20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
     37, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
     58, 59, 60, 61, 62, 41, 63, 65, 66, 67, 68, 64, 70, 69, 71, 72, 73,
     74, 75, 76, 77, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91,
     92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106,
     107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117,),
    # 18
    (0, 1, 27, 52, 113,),
    # 19
    (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 31, 32, 33, 44, 45,
     50, 51, 53, 58, 59, 60, 61, 62, 41, 63, 65, 66, 67, 68, 69, 71, 72,
     73, 79, 83, 85, 86, 87, 89, 90, 91, 93, 94, 95, 97, 99, 101, 103,
     105, 107, 109, 111, 116, 117,),
)

# script -> (codepoint, Firefox language group, {generic: CANDIDATE_LISTS index})
PROBE = {
    'Han (Simplified)': ('U+4E00', 'zh-CN', {'sans-serif': 0, 'serif': 0, 'monospace': 0}),
    'Han (Traditional)': ('U+842C', 'zh-TW', {'sans-serif': 0, 'serif': 0, 'monospace': 0}),
    'Hiragana': ('U+3042', 'ja', {'sans-serif': 1, 'serif': 1, 'monospace': 1}),
    'Katakana': ('U+30A2', 'ja', {'sans-serif': 1, 'serif': 1, 'monospace': 1}),
    'Hangul': ('U+AC00', 'ko', {'sans-serif': 0, 'serif': 0, 'monospace': 0}),
    'Cyrillic': ('U+0410', 'x-cyrillic', {'sans-serif': 2, 'serif': 3, 'monospace': 4}),
    'Greek': ('U+0391', 'el', {'sans-serif': 2, 'serif': 5, 'monospace': 4}),
    'Devanagari': ('U+0915', 'x-devanagari', {'sans-serif': 6, 'serif': 6, 'monospace': 6}),
    'Bengali': ('U+0995', 'x-beng', {'sans-serif': 6, 'serif': 6, 'monospace': 6}),
    'Gurmukhi': ('U+0A15', 'x-guru', {'sans-serif': 6, 'serif': 6, 'monospace': 6}),
    'Gujarati': ('U+0A95', 'x-gujr', {'sans-serif': 6, 'serif': 6, 'monospace': 6}),
    'Oriya': ('U+0B15', 'x-orya', {'sans-serif': 6, 'serif': 6, 'monospace': 6}),
    'Tamil': ('U+0B95', 'x-tamil', {'sans-serif': 6, 'serif': 6, 'monospace': 6}),
    'Telugu': ('U+0C15', 'x-telu', {'sans-serif': 6, 'serif': 6, 'monospace': 6}),
    'Kannada': ('U+0C95', 'x-knda', {'sans-serif': 6, 'serif': 6, 'monospace': 6}),
    'Malayalam': ('U+0D15', 'x-mlym', {'sans-serif': 6, 'serif': 6, 'monospace': 6}),
    'Sinhala': ('U+0D9A', 'x-sinh', {'sans-serif': 6, 'serif': 6, 'monospace': 6}),
    'Thai': ('U+0E01', 'th', {'sans-serif': 7, 'serif': 7, 'monospace': 7}),
    'Khmer': ('U+1780', 'x-khmr', {'sans-serif': 0, 'serif': 0, 'monospace': 0}),
    'Hebrew': ('U+05D0', 'he', {'sans-serif': 8, 'serif': 8, 'monospace': 9}),
    'Arabic': ('U+0627', 'ar', {'sans-serif': 10, 'serif': 11, 'monospace': 11}),
    'Armenian': ('U+0531', 'x-armn', {'sans-serif': 12, 'serif': 13, 'monospace': 12}),
    'Georgian': ('U+10A0', 'x-geor', {'sans-serif': 14, 'serif': 15, 'monospace': 14}),
    'Tibetan': ('U+0F40', 'x-tibt', {'sans-serif': 0, 'serif': 0, 'monospace': 0}),
    'Ethiopic': ('U+1200', 'x-ethi', {'sans-serif': 0, 'serif': 0, 'monospace': 0}),
    'Canadian Syllabics': ('U+1401', 'x-cans', {'sans-serif': 0, 'serif': 0, 'monospace': 0}),
    'Emoji': ('U+1F600', 'x-unicode', {'sans-serif': 16, 'serif': 16, 'monospace': 16}),
    'Symbol': ('U+2603', 'x-unicode', {'sans-serif': 17, 'serif': 17, 'monospace': 17}),
    'Math': ('U+2211', 'x-math', {'sans-serif': 18, 'serif': 19, 'monospace': 4}),
}

# font.name-list.* as Firefox compiles them in for Windows: what it asks for
# before any fallback happens. Key is (generic, language group); the group is
# None for font.name-list.emoji, which has none.
FIREFOX_WINDOWS_DEFAULTS = {
    ('serif', 'x-math'): ['Latin Modern Math', 'STIX Two Math', 'XITS Math', 'Cambria Math', 'Libertinus Math', 'DejaVu Math TeX Gyre', 'TeX Gyre Bonum Math', 'TeX Gyre Pagella Math', 'TeX Gyre Schola', 'TeX Gyre Termes Math', 'STIX Math', 'Asana Math', 'STIXGeneral', 'DejaVu Serif', 'DejaVu Sans', 'Times New Roman'],
    ('sans-serif', 'x-math'): ['Arial'],
    ('monospace', 'x-math'): ['Consolas'],
    ('emoji', None): ['Segoe UI Emoji', 'Twemoji Mozilla'],
    ('serif', 'ar'): ['Times New Roman'],
    ('sans-serif', 'ar'): ['Segoe UI', 'Tahoma', 'Arial'],
    ('monospace', 'ar'): ['Consolas'],
    ('cursive', 'ar'): ['Comic Sans MS'],
    ('serif', 'el'): ['Times New Roman'],
    ('sans-serif', 'el'): ['Arial'],
    ('monospace', 'el'): ['Consolas'],
    ('cursive', 'el'): ['Comic Sans MS'],
    ('serif', 'he'): ['Narkisim', 'David'],
    ('sans-serif', 'he'): ['Arial'],
    ('monospace', 'he'): ['Fixed Miriam Transparent', 'Miriam Fixed', 'Rod', 'Consolas', 'Courier New'],
    ('cursive', 'he'): ['Guttman Yad', 'Ktav', 'Arial'],
    ('serif', 'ja'): ['Yu Mincho', 'MS PMincho', 'MS Mincho', 'Noto Serif CJK JP', 'Noto Serif JP', 'Meiryo', 'Yu Gothic', 'MS PGothic', 'MS Gothic'],
    ('sans-serif', 'ja'): ['Meiryo', 'Yu Gothic', 'MS PGothic', 'MS Gothic', 'Noto Sans CJK JP', 'Noto Sans JP'],
    ('monospace', 'ja'): ['BIZ UDGothic', 'MS Gothic', 'MS Mincho', 'Meiryo', 'Yu Gothic', 'Yu Mincho'],
    ('serif', 'ko'): ['Batang', 'Noto Serif CJK KR', 'Noto Serif KR', 'Gulim'],
    ('sans-serif', 'ko'): ['Malgun Gothic', 'Gulim', 'Noto Sans CJK KR', 'Noto Sans KR'],
    ('monospace', 'ko'): ['GulimChe'],
    ('cursive', 'ko'): ['Gungsuh'],
    ('serif', 'th'): ['Tahoma'],
    ('sans-serif', 'th'): ['Tahoma'],
    ('monospace', 'th'): ['Tahoma'],
    ('cursive', 'th'): ['Tahoma'],
    ('serif', 'x-cyrillic'): ['Times New Roman'],
    ('sans-serif', 'x-cyrillic'): ['Arial'],
    ('monospace', 'x-cyrillic'): ['Consolas'],
    ('cursive', 'x-cyrillic'): ['Comic Sans MS'],
    ('serif', 'x-unicode'): ['Times New Roman'],
    ('sans-serif', 'x-unicode'): ['Arial'],
    ('monospace', 'x-unicode'): ['Consolas'],
    ('cursive', 'x-unicode'): ['Comic Sans MS'],
    ('serif', 'x-western'): ['Times New Roman'],
    ('sans-serif', 'x-western'): ['Arial'],
    ('monospace', 'x-western'): ['Consolas'],
    ('cursive', 'x-western'): ['Comic Sans MS'],
    ('serif', 'zh-CN'): ['SimSun', 'MS Song', 'SimSun-ExtB', 'Noto Serif CJK SC', 'Noto Serif SC'],
    ('sans-serif', 'zh-CN'): ['Microsoft YaHei', 'SimHei', 'Noto Sans CJK SC', 'Noto Sans SC'],
    ('monospace', 'zh-CN'): ['NSimSun', 'SimSun', 'MS Song', 'SimSun-ExtB'],
    ('cursive', 'zh-CN'): ['KaiTi', 'KaiTi_GB2312'],
    ('serif', 'zh-TW'): ['Times New Roman', 'PMingLiu', 'MingLiU', 'MingLiU-ExtB', 'Noto Serif CJK TC', 'Noto Serif TC'],
    ('sans-serif', 'zh-TW'): ['Arial', 'Microsoft JhengHei', 'PMingLiU', 'MingLiU', 'MingLiU-ExtB', 'Noto Sans CJK TC', 'Noto Sans TC'],
    ('monospace', 'zh-TW'): ['MingLiU', 'MingLiU-ExtB'],
    ('cursive', 'zh-TW'): ['DFKai-SB'],
    ('serif', 'zh-HK'): ['Times New Roman', 'MingLiu_HKSCS', 'Ming(for ISO10646)', 'MingLiU', 'MingLiU_HKSCS-ExtB', 'Noto Serif CJK HK', 'Noto Serif HK', 'Microsoft JhengHei'],
    ('sans-serif', 'zh-HK'): ['Arial', 'MingLiU_HKSCS', 'Ming(for ISO10646)', 'MingLiU', 'MingLiU_HKSCS-ExtB', 'Microsoft JhengHei', 'Noto Sans CJK HK', 'Noto Sans HK'],
    ('monospace', 'zh-HK'): ['MingLiU_HKSCS', 'Ming(for ISO10646)', 'MingLiU', 'MingLiU_HKSCS-ExtB', 'Microsoft JhengHei'],
    ('cursive', 'zh-HK'): ['DFKai-SB'],
    ('serif', 'x-devanagari'): ['Kokila', 'Raghindi'],
    ('sans-serif', 'x-devanagari'): ['Nirmala UI', 'Mangal'],
    ('monospace', 'x-devanagari'): ['Mangal', 'Nirmala UI'],
    ('serif', 'x-tamil'): ['Latha'],
    ('monospace', 'x-tamil'): ['Latha'],
    ('serif', 'x-armn'): ['Sylfaen'],
    ('sans-serif', 'x-armn'): ['Arial AMU'],
    ('monospace', 'x-armn'): ['Arial AMU'],
    ('serif', 'x-beng'): ['Vrinda', 'Akaash', 'Likhan', 'Ekushey Punarbhaba'],
    ('sans-serif', 'x-beng'): ['Nirmala Text', 'Vrinda', 'Akaash', 'Likhan', 'Ekushey Punarbhaba'],
    ('monospace', 'x-beng'): ['Mitra Mono', 'Likhan', 'Mukti Narrow'],
    ('serif', 'x-cans'): ['Aboriginal Serif', 'BJCree Uni'],
    ('sans-serif', 'x-cans'): ['Aboriginal Sans'],
    ('monospace', 'x-cans'): ['Aboriginal Sans', 'OskiDakelh', 'Pigiarniq', 'Uqammaq'],
    ('serif', 'x-ethi'): ['Visual Geez Unicode', 'Visual Geez Unicode Agazian'],
    ('sans-serif', 'x-ethi'): ['GF Zemen Unicode'],
    ('monospace', 'x-ethi'): ['Ethiopia Jiret'],
    ('cursive', 'x-ethi'): ['Visual Geez Unicode Title'],
    ('serif', 'x-geor'): ['Sylfaen', 'BPG Paata Khutsuri U', 'TITUS Cyberbit Basic'],
    ('sans-serif', 'x-geor'): ['BPG Classic 99U'],
    ('monospace', 'x-geor'): ['BPG Classic 99U'],
    ('serif', 'x-gujr'): ['Shruti'],
    ('sans-serif', 'x-gujr'): ['Shruti'],
    ('monospace', 'x-gujr'): ['Shruti'],
    ('serif', 'x-guru'): ['Raavi', 'Saab'],
    ('monospace', 'x-guru'): ['Raavi', 'Saab'],
    ('serif', 'x-khmr'): ['PhnomPenh OT', '.Mondulkiri U GR 1.5', 'Khmer OS'],
    ('sans-serif', 'x-khmr'): ['Khmer OS'],
    ('monospace', 'x-khmr'): ['Khmer OS', 'Khmer OS System'],
    ('serif', 'x-mlym'): ['Rachana_w01', 'AnjaliOldLipi', 'Kartika', 'ThoolikaUnicode'],
    ('sans-serif', 'x-mlym'): ['Rachana_w01', 'AnjaliOldLipi', 'Kartika', 'ThoolikaUnicode'],
    ('monospace', 'x-mlym'): ['Rachana_w01', 'AnjaliOldLipi', 'Kartika', 'ThoolikaUnicode'],
    ('serif', 'x-orya'): ['ori1Uni', 'Kalinga'],
    ('sans-serif', 'x-orya'): ['ori1Uni', 'Kalinga'],
    ('monospace', 'x-orya'): ['ori1Uni', 'Kalinga'],
    ('serif', 'x-telu'): ['Gautami', 'Akshar Unicode'],
    ('sans-serif', 'x-telu'): ['Gautami', 'Akshar Unicode'],
    ('monospace', 'x-telu'): ['Gautami', 'Akshar Unicode'],
    ('serif', 'x-knda'): ['Tunga', 'AksharUnicode'],
    ('sans-serif', 'x-knda'): ['Tunga', 'AksharUnicode'],
    ('monospace', 'x-knda'): ['Tunga', 'AksharUnicode'],
    ('serif', 'x-sinh'): ['Iskoola Pota', 'AksharUnicode'],
    ('sans-serif', 'x-sinh'): ['Iskoola Pota', 'AksharUnicode'],
    ('monospace', 'x-sinh'): ['Iskoola Pota', 'AksharUnicode'],
    ('serif', 'x-tibt'): ['Tibetan Machine Uni', 'Jomolhari', 'Microsoft Himalaya'],
    ('sans-serif', 'x-tibt'): ['Tibetan Machine Uni', 'Jomolhari', 'Microsoft Himalaya'],
    ('monospace', 'x-tibt'): ['Tibetan Machine Uni', 'Jomolhari', 'Microsoft Himalaya'],
    ('cursive', 'x-math'): ['Comic Sans MS'],
}


if __name__ == "__main__":
    main()
