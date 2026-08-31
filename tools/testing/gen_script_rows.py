#!/usr/bin/env python3
"""
Emit the fallback script table from ICU's own data and Chromium's map.

    tools/testing/gen_script_rows.py <chromium-src> > rows.inc

font_fallback_win.cc answers a character by its script: GetScript asks ICU,
falls back to Character::GetScriptBasedOnUnicodeBlock for Common and
Inherited, and looks the result up in kScriptToFontFamilies. The families are
transcribed in fallback_order.cpp; the codepoint ranges each script covers are
ICU's, and are read here from the preparsed UCD that ships in the Chromium
tree rather than written out by hand.

The rows are appended to kScripts, so every row already in that table keeps
its answer. Those earlier rows are the ones measured against DirectWrite's own
fallback, which has no source to read.
"""
import collections
import sys

# kScriptToFontFamilies, by ICU script code, to the array names in
# fallback_order.cpp. Scripts whose Chromium entry names no font a stock
# Windows ships are left out; the last-resort walk answers them.
kScriptFamilies = {
    "Arab": "kArabic", "Armn": "kArmenian", "Beng": "kBengali",
    "Brah": "kSegoeHistoric", "Brai": "kSegoeSymbol", "Bugi": "kBuginese",
    "Cans": "kCanadianAboriginal", "Cari": "kSegoeHistoric",
    "Cher": "kCherokee", "Copt": "kSegoeSymbol", "Xsux": "kSegoeHistoric",
    "Cprt": "kSegoeHistoric", "Cyrl": "kTimesNewRoman",
    "Dsrt": "kSegoeSymbol", "Deva": "kDevanagari", "Egyp": "kSegoeHistoric",
    "Ethi": "kEthiopic", "Geor": "kGeorgian",
    "Glag": "kSegoeHistoricOrSymbol", "Goth": "kSegoeHistoricOrSymbol",
    "Grek": "kTimesNewRoman", "Gujr": "kGujarati", "Guru": "kGurmukhi",
    "Hang": "kHangul", "Hebr": "kHebrew", "Hira": "kKatakanaOrHiragana",
    "Armi": "kSegoeHistoric", "Phli": "kSegoeHistoric",
    "Prti": "kSegoeHistoric", "Java": "kJavanese", "Knda": "kKannada",
    "Kana": "kKatakanaOrHiragana", "Khar": "kSegoeHistoric",
    "Khmr": "kKhmer", "Laoo": "kLao", "Latn": "kTimesNewRoman",
    "Lisu": "kLisu", "Lyci": "kSegoeHistoric", "Lydi": "kSegoeHistoric",
    "Mlym": "kMalayalam", "Mtei": "kNirmala",
    "Merc": "kSegoeHistoricOrSymbol", "Mong": "kMongolian",
    "Mymr": "kMyanmar", "Talu": "kNewTaiLue", "Nkoo": "kEbrima",
    "Ogam": "kSegoeHistoricOrSymbol", "Olck": "kNirmala",
    "Ital": "kSegoeHistoricOrSymbol", "Xpeo": "kSegoeHistoric",
    "Sarb": "kSegoeHistoric", "Orya": "kOriya",
    "Orkh": "kSegoeHistoricOrSymbol", "Osma": "kEbrima", "Phag": "kPhagsPa",
    "Runr": "kSegoeHistoricOrSymbol", "Shaw": "kSegoeHistoric",
    "Sinh": "kSinhala", "Sora": "kNirmala", "Zsym": "kSegoeSymbol",
    "Syrc": "kSyriac", "Tale": "kTaiLe", "Taml": "kTamil", "Telu": "kTelugu",
    "Thaa": "kThaana", "Thai": "kThai", "Tibt": "kTibetan",
    "Tfng": "kEbrima", "Bopo": "kTraditionalHan", "Vaii": "kEbrima",
    "Yiii": "kYi",
    # Unified Han, which the sort's language settles. kSimplifiedHan is the
    # row's shape; OrderForHan answers it.
    "Hani": "kSimplifiedHan",
}

# Character::GetScriptBasedOnUnicodeBlock, for Common and Inherited.
kBlockScripts = {
    "CJK_Symbols": "Hani", "Hiragana": "Kana", "Katakana": "Kana",
    "Arabic": "Arab", "Thai": "Thai", "Greek": "Grek", "Devanagari": "Deva",
    "Armenian": "Armn", "Georgian": "Geor", "Kannada": "Knda",
    "Gothic": "Goth",
}


def parse(path):
    """Script and block per codepoint, with ppucd's own inheritance."""
    scripts = {}
    blocks = {}
    emoji = {}
    block_default = ("Zzzz", "NB", False, False)

    def props(fields):
        out = {}
        for field in fields:
            key, _, value = field.partition("=")
            # A binary property is written as its bare name when it is set.
            out[key] = value if value else "1"
        return out

    def span(text):
        first, _, last = text.partition("..")
        return int(first, 16), int(last or first, 16)

    for line in open(path, encoding="utf-8"):
        fields = line.rstrip("\n").split(";")
        if fields[0] == "block":
            low, high = span(fields[1])
            have = props(fields[2:])
            block_default = (have.get("sc", "Zzzz"), have.get("blk", "NB"),
                             "Emoji" in have, "EPres" in have)
            for cp in range(low, high + 1):
                scripts[cp], blocks[cp] = block_default[0], block_default[1]
                emoji[cp] = (block_default[2], block_default[3])
        elif fields[0] == "cp" and len(fields) > 1 and fields[1]:
            low, high = span(fields[1])
            have = props(fields[2:])
            sc = have.get("sc", block_default[0])
            blk = have.get("blk", block_default[1])
            is_emoji = "Emoji" in have or block_default[2]
            is_pres = "EPres" in have or block_default[3]
            for cp in range(low, high + 1):
                scripts[cp], blocks[cp] = sc, blk
                emoji[cp] = (is_emoji, is_pres)
    return scripts, blocks, emoji


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "/home/d4n/repos/electron/src"
    scripts, blocks, emoji = parse(
        root + "/third_party/icu/source/data/unidata/ppucd.txt")

    # PlatformFallbackFontForCharacter promotes any Character::IsEmoji to
    # kEmojiText, and GetFallbackFamily answers that from the mono emoji font
    # before it looks at the block or the script. A character whose default
    # presentation is emoji arrives as kEmojiEmoji instead and takes the color
    # one.
    emoji_family = {}
    for cp, (is_emoji, is_pres) in emoji.items():
        if is_emoji:
            emoji_family[cp] = "kEmoji" if is_pres else "kSegoeSymbol"

    family = {}
    for cp, sc in scripts.items():
        if sc in ("Zyyy", "Zinh", "Zzzz"):
            # GetScript infers one from the block for these.
            block = blocks.get(cp, "NB")
            sc = next((v for k, v in kBlockScripts.items()
                       if block.startswith(k)), None)
            if sc is None:
                continue
        name = kScriptFamilies.get(sc)
        if name is not None:
            family[cp] = name

    def emit(mapping, title):
        out = []
        for cp in sorted(mapping):
            name = mapping[cp]
            if out and out[-1][2] == name and cp == out[-1][1] + 1:
                out[-1][1] = cp
            else:
                out.append([cp, cp, name])
        print("// %s: %d rows, generated by tools/testing/gen_script_rows.py."
              % (title, len(out)))
        for low, high, name in out:
            print("    DWC_SCRIPT(0x%04X, 0x%04X, %s)," % (low, high, name))
        return out

    emit(emoji_family, "emoji")
    print("// ---- script rows ----")
    runs = []
    for cp in sorted(family):
        name = family[cp]
        if runs and runs[-1][2] == name and cp == runs[-1][1] + 1:
            runs[-1][1] = cp
        else:
            runs.append([cp, cp, name])
    by_name = collections.Counter(r[2] for r in runs)
    print("// %d rows, generated by tools/testing/gen_script_rows.py."
          % len(runs))
    for low, high, name in runs:
        print("    DWC_SCRIPT(0x%04X, 0x%04X, %s)," % (low, high, name))
    print("// families used: %s" % ", ".join(sorted(by_name)), file=sys.stderr)
    print("// %d rows" % len(runs), file=sys.stderr)


if __name__ == "__main__":
    main()
