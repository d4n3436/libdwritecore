//+--------------------------------------------------------------------------
//
//  fallback_order.cpp - choose the fallback font Windows would have chosen.
//
//  The two platforms pick a font for an unsupported character in completely
//  different ways. Windows tries a hardcoded list per script and takes the
//  first family that is installed, from
//  third_party/blink/renderer/platform/fonts/win/font_fallback_win.cc, and
//  only asks IDWriteFontFallback when none of them has the character
//  (font_cache_skia_win.cc, GetFallbackFamilyNameFromHardcodedChoices then
//  GetDWriteFallbackFamily). Linux has no such table.
//  ui/gfx/font_fallback_linux.cc sorts every installed font by fontconfig's
//  idea of closeness and walks the result looking for one whose charset covers
//  the character.
//
//  Reordering the sorted set is not enough, because one set is cached per
//  family and locale and then reused for every character. The loop in
//  GetFallbackFontForChar takes the first entry that covers the text, so a
//  single order cannot give Japanese to Yu Gothic and Han to Microsoft YaHei
//  at once.
//
//  The answer is given per character instead, where the decision actually is.
//  FcFontSort is watched only to learn which families exist and what each
//  one's charset is. FcCharSetHasChar then reports a miss for any family that
//  is not the one Windows' table would have reached for that character's
//  script, provided the family it would have reached really does cover it.
//  Everything else is passed through, so fontconfig still decides what is
//  installed and what covers what.
//
//----------------------------------------------------------------------------

// Style inspections left as they are: the shapes they suggest either read
// worse against the sources being mirrored, or would change which overload
// is chosen if one were ever added.
// ReSharper disable CppParameterMayBeConst
// ReSharper disable CppParameterMayBeConstPtrOrRef
// ReSharper disable CppUseDesignatedInitializers
// ReSharper disable CppUseStructuredBinding

#include <unistd.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <dlfcn.h>

#include "fallback_order.h"
#include "parity_gate.h"
#include "windows_fonts.h"

namespace {

// fontconfig, without its headers.
constexpr int kFcResultMatch = 0;

struct FcFontSet
{
    int nfont;
    void** fonts;
};

using PatternGetCharSetFn = int (*)(const void*, const char*, int, void**);
using PatternGetStringFn = int (*)(const void*, const char*, int, unsigned char**);

template <typename T>
T Sym(const char* name)
{
    return reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}

// The scripts that actually differ. Latin and the rest already agree, because
// fontconfig and the table reach the same family for them. The row is the
// header's own type, so the table can be handed out without a cast: two
// layout-identical types are still distinct to the aliasing rules, and the
// reads a caller makes through the wrong one are undefined.
using ScriptFonts = fallback_order::ScriptRow;

// A candidate only counts if a stock Windows 11 would have it. Chromium's
// lists name fonts that may or may not be present on any given machine, and
// kHangulFonts opens with "Noto Sans KR" and "Noto Sans CJK KR", which no
// Windows install ships but many Linux ones do. Taking the first installed
// candidate would then answer Noto here and Malgun Gothic there from the same
// table.
//
// windows_fonts::kBaseInstall is the families a stock Windows 11 ships,
// transcribed for the Firefox side of this repository and reused here.
bool ShipsWithWindows(const char* name)
{
    for (const char* known : windows_fonts::kBaseInstall) {
        if (strcasecmp(known, name) == 0) {
            return true;
        }
    }
    return false;
}

// The lists from font_fallback_win.cc, verbatim and in their order.
constexpr const char* kKatakanaOrHiragana[] = {"Noto Sans JP", "Noto Sans CJK JP", "Meiryo",
                                           "Yu Gothic", "MS PGothic", "Microsoft YaHei"};
constexpr const char* kTraditionalHan[] = {"Noto Sans TC", "Noto Sans CJK TC",
                                       "Microsoft JhengHei", "pmingli"};
constexpr const char* kSimplifiedHan[] = {"Noto Sans SC", "Noto Sans CJK SC", "Microsoft YaHei",
                                      "simsun"};
constexpr const char* kHangul[] = {"Noto Sans KR", "Noto Sans CJK KR", "Malgun Gothic", "Gulim"};
constexpr const char* kArabic[] = {"Tahoma", "Segoe UI"};
constexpr const char* kHebrew[] = {"David", "Segoe UI"};
constexpr const char* kArmenian[] = {"Segoe UI", "Sylfaen"};
constexpr const char* kGeorgian[] = {"Sylfaen", "Segoe UI"};
constexpr const char* kDevanagari[] = {"Nirmala UI", "Mangal"};
constexpr const char* kBengali[] = {"Nirmala UI", "Vrinda"};
constexpr const char* kGurmukhi[] = {"Nirmala UI", "Raavi"};
constexpr const char* kGujarati[] = {"Nirmala UI", "Shruti"};
constexpr const char* kOriya[] = {"Kalinga", "ori1Uni", "Lohit Oriya", "Nirmala UI"};
constexpr const char* kTamil[] = {"Nirmala UI", "Latha"};
constexpr const char* kTelugu[] = {"Nirmala UI", "Gautami"};
constexpr const char* kKannada[] = {"Tunga", "Nirmala UI"};
constexpr const char* kMalayalam[] = {"Nirmala UI", "Kartika"};
constexpr const char* kSinhala[] = {"Iskoola Pota", "AksharUnicode", "Nirmala UI"};
constexpr const char* kThai[] = {"Tahoma", "Leelawadee UI", "Leelawadee"};
constexpr const char* kLao[] = {"Leelawadee UI", "Lao UI"};
constexpr const char* kBuginese[] = {"Leelawadee UI"};
constexpr const char* kKhmer[] = {"Leelawadee UI", "Khmer UI", "Khmer OS", "MoolBoran", "DaunPenh"};

constexpr const char* kMyanmar[] = {"Myanmar Text", "Padauk", "Parabaik", "Myanmar3",
                                    "Code2000"};
constexpr const char* kEthiopic[] = {"Nyala", "Abyssinica SIL", "Ethiopia Jiret",
                                     "Visual Geez Unicode", "GF Zemen Unicode", "Ebrima"};
constexpr const char* kCherokee[] = {"Gadugi", "Plantagenet"};
constexpr const char* kCanadianAboriginal[] = {"Gadugi", "Euphemia"};
constexpr const char* kMongolian[] = {"Mongolian Baiti"};
constexpr const char* kTibetan[] = {"Microsoft Himalaya", "Jomolhari", "Tibetan Machine Uni"};
constexpr const char* kYi[] = {"Microsoft Yi Baiti", "Nuosu SIL", "Code2000"};
constexpr const char* kEbrima[] = {"Ebrima"};
constexpr const char* kNirmala[] = {"Nirmala UI"};
constexpr const char* kJavanese[] = {"Javanese Text"};
constexpr const char* kLisu[] = {"Segoe UI"};
constexpr const char* kTaiLe[] = {"Microsoft Tai Le"};
constexpr const char* kNewTaiLue[] = {"Microsoft New Tai Lue"};
constexpr const char* kPhagsPa[] = {"Microsoft PhagsPa"};
constexpr const char* kSegoeSymbol[] = {"Segoe UI Symbol"};

// font_fallback_win.cc has no list for the symbol and math blocks, so these
// are what DirectWrite's own MapCharacters settles on for the characters the
// requested font does not cover. Order matters the same way the script lists
// do: the first family that covers the character wins, which is how the
// arrows split between Cambria Math and Segoe UI Symbol and the block
// elements between Lucida Sans Unicode and MS PGothic.
constexpr const char* kMath[] = {"Cambria Math"};
constexpr const char* kArrowsSimple[] = {"Segoe UI Symbol", "Cambria Math"};
constexpr const char* kArrows[] = {"Cambria Math", "Segoe UI Symbol"};
constexpr const char* kBlockElements[] = {"Lucida Sans Unicode", "MS PGothic"};

// The characters Windows answers from the emoji font rather than the symbol
// font. These are the emoji-presentation ones, and the rows below are the
// runs DirectWrite was measured to split at.
constexpr const char* kEmoji[] = {"Segoe UI Emoji"};

// font_fallback_win.cc maps Latin, Greek and Cyrillic to one family each, and
// all three name the same one. GetFallbackFamily returns it and
// GetFallbackFamilyNameFromHardcodedChoices then checks that it covers the
// character, so it is a first choice and not a verdict; the set order says it
// that way.
constexpr const char* kTimesNewRoman[] = {"Times New Roman"};

// The supplementary Han blocks. Windows answers these from one family
// whatever the locale, unlike the unified block.
constexpr const char* kHanSupplementary[] = {"SimSun-ExtB"};
constexpr const char* kSpecials[] = {"Tahoma"};
constexpr const char* kSegoeHistoric[] = {"Segoe UI Historic"};
constexpr const char* kSegoeHistoricOrSymbol[] = {"Segoe UI Historic", "Segoe UI Symbol"};
constexpr const char* kSyriac[] = {"Estrangelo Edessa", "Estrangelo Nisibin", "Code2000"};
constexpr const char* kThaana[] = {"MV Boli"};

// Where each script lives. The unified Han block carries no list of its own,
// since Windows picks one from the locale; OrderForHan below answers it.
#define DWC_COUNT(table) (sizeof(table) / sizeof((table)[0]))
#define DWC_SCRIPT(first, last, table) {(first), (last), (table), \
                                        sizeof(table) / sizeof((table)[0])}

const ScriptFonts kScripts[] = {
    DWC_SCRIPT(0x0590, 0x05FF, kHebrew),
    DWC_SCRIPT(0x0600, 0x06FF, kArabic),
    DWC_SCRIPT(0x0750, 0x077F, kArabic),        // Arabic supplement
    DWC_SCRIPT(0x08A0, 0x08FF, kArabic),        // Arabic extended-A
    DWC_SCRIPT(0xFB50, 0xFDFF, kArabic),        // presentation forms A
    DWC_SCRIPT(0xFE70, 0xFEFF, kArabic),        // presentation forms B
    DWC_SCRIPT(0x0530, 0x058F, kArmenian),
    DWC_SCRIPT(0x10A0, 0x10FF, kGeorgian),
    DWC_SCRIPT(0x1C90, 0x1CBF, kGeorgian),      // Georgian extended
    DWC_SCRIPT(0x0900, 0x097F, kDevanagari),
    DWC_SCRIPT(0x0980, 0x09FF, kBengali),
    DWC_SCRIPT(0x0A00, 0x0A7F, kGurmukhi),
    DWC_SCRIPT(0x0A80, 0x0AFF, kGujarati),
    DWC_SCRIPT(0x0B00, 0x0B7F, kOriya),
    DWC_SCRIPT(0x0B80, 0x0BFF, kTamil),
    DWC_SCRIPT(0x0C00, 0x0C7F, kTelugu),
    DWC_SCRIPT(0x0C80, 0x0CFF, kKannada),
    DWC_SCRIPT(0x0D00, 0x0D7F, kMalayalam),
    DWC_SCRIPT(0x0D80, 0x0DFF, kSinhala),
    DWC_SCRIPT(0x0E00, 0x0E7F, kThai),
    DWC_SCRIPT(0x0E80, 0x0EFF, kLao),
    DWC_SCRIPT(0x1780, 0x17FF, kKhmer),
    DWC_SCRIPT(0x0700, 0x074F, kSyriac),
    DWC_SCRIPT(0x0780, 0x07BF, kThaana),
    DWC_SCRIPT(0x07C0, 0x07FF, kEbrima),          // N'Ko
    DWC_SCRIPT(0x0F00, 0x0FFF, kTibetan),
    DWC_SCRIPT(0x1000, 0x109F, kMyanmar),
    DWC_SCRIPT(0xA9E0, 0xA9FF, kMyanmar),         // Myanmar extended-B
    DWC_SCRIPT(0xAA60, 0xAA7F, kMyanmar),         // Myanmar extended-A
    DWC_SCRIPT(0x1200, 0x139F, kEthiopic),
    DWC_SCRIPT(0x2D80, 0x2DDF, kEthiopic),        // Ethiopic extended
    DWC_SCRIPT(0x13A0, 0x13FF, kCherokee),
    DWC_SCRIPT(0xAB70, 0xABBF, kCherokee),        // Cherokee supplement
    DWC_SCRIPT(0x1400, 0x167F, kCanadianAboriginal),
    DWC_SCRIPT(0x18B0, 0x18FF, kCanadianAboriginal),
    DWC_SCRIPT(0x1800, 0x18AF, kMongolian),
    DWC_SCRIPT(0x1950, 0x197F, kTaiLe),
    DWC_SCRIPT(0x1980, 0x19DF, kNewTaiLue),
    DWC_SCRIPT(0x1C50, 0x1C7F, kNirmala),         // Ol Chiki
    DWC_SCRIPT(0x2800, 0x28FF, kSegoeSymbol),     // Braille
    DWC_SCRIPT(0x2C80, 0x2CFF, kSegoeSymbol),     // Coptic
    DWC_SCRIPT(0x2D30, 0x2D7F, kEbrima),          // Tifinagh
    DWC_SCRIPT(0xA000, 0xA4CF, kYi),
    DWC_SCRIPT(0xA4D0, 0xA4FF, kLisu),
    DWC_SCRIPT(0xA500, 0xA63F, kEbrima),          // Vai
    DWC_SCRIPT(0xA840, 0xA87F, kPhagsPa),
    DWC_SCRIPT(0xA980, 0xA9DF, kJavanese),
    DWC_SCRIPT(0x1680, 0x169F, kSegoeHistoricOrSymbol),    // Ogham
    DWC_SCRIPT(0x16A0, 0x16FF, kSegoeHistoricOrSymbol),    // Runic
    DWC_SCRIPT(0x2C00, 0x2C5F, kSegoeHistoricOrSymbol),    // Glagolitic
    DWC_SCRIPT(0x10300, 0x1032F, kSegoeHistoricOrSymbol),  // Old Italic
    DWC_SCRIPT(0x10330, 0x1034F, kSegoeHistoricOrSymbol),  // Gothic
    DWC_SCRIPT(0x10C00, 0x10C4F, kSegoeHistoricOrSymbol),  // Orkhon
    DWC_SCRIPT(0x109A0, 0x109FF, kSegoeHistoricOrSymbol),  // Meroitic cursive
    DWC_SCRIPT(0x1E000, 0x1E02F, kSegoeHistoricOrSymbol),  // Glagolitic supplement
    DWC_SCRIPT(0x102A0, 0x102DF, kSegoeHistoric),          // Carian
    DWC_SCRIPT(0x103A0, 0x103DF, kSegoeHistoric),          // Old Persian
    DWC_SCRIPT(0x10450, 0x1047F, kSegoeHistoric),          // Shavian
    DWC_SCRIPT(0x10800, 0x1083F, kSegoeHistoric),          // Cypriot
    DWC_SCRIPT(0x10840, 0x1085F, kSegoeHistoric),          // Imperial Aramaic
    DWC_SCRIPT(0x10A00, 0x10A5F, kSegoeHistoric),          // Kharoshthi
    DWC_SCRIPT(0x10A60, 0x10A7F, kSegoeHistoric),          // Old South Arabian
    DWC_SCRIPT(0x10B40, 0x10B5F, kSegoeHistoric),          // Inscriptional Parthian
    DWC_SCRIPT(0x10B60, 0x10B7F, kSegoeHistoric),          // Inscriptional Pahlavi
    DWC_SCRIPT(0x11000, 0x1107F, kSegoeHistoric),          // Brahmi
    DWC_SCRIPT(0x12000, 0x1254F, kSegoeHistoric),          // Cuneiform
    DWC_SCRIPT(0x13000, 0x1342F, kSegoeHistoric),          // Egyptian hieroglyphs
    DWC_SCRIPT(0x10400, 0x1044F, kSegoeSymbol),            // Deseret
    // Emoji first, as PlatformFallbackFontForCharacter does: any character
    // Unicode calls an emoji is asked for as emoji, and the block and script
    // below are never reached for it.
// emoji: 129 rows, generated by tools/testing/gen_script_rows.py.
    DWC_SCRIPT(0x0023, 0x0023, kSegoeSymbol),
    DWC_SCRIPT(0x002A, 0x002A, kSegoeSymbol),
    DWC_SCRIPT(0x0030, 0x0039, kSegoeSymbol),
    DWC_SCRIPT(0x00A9, 0x00A9, kSegoeSymbol),
    DWC_SCRIPT(0x00AE, 0x00AE, kSegoeSymbol),
    DWC_SCRIPT(0x203C, 0x203C, kSegoeSymbol),
    DWC_SCRIPT(0x2049, 0x2049, kSegoeSymbol),
    DWC_SCRIPT(0x2122, 0x2122, kSegoeSymbol),
    DWC_SCRIPT(0x2139, 0x2139, kSegoeSymbol),
    DWC_SCRIPT(0x2194, 0x2199, kSegoeSymbol),
    DWC_SCRIPT(0x21A9, 0x21AA, kSegoeSymbol),
    DWC_SCRIPT(0x231A, 0x231B, kEmoji),
    DWC_SCRIPT(0x2328, 0x2328, kSegoeSymbol),
    DWC_SCRIPT(0x23CF, 0x23CF, kSegoeSymbol),
    DWC_SCRIPT(0x23E9, 0x23EC, kEmoji),
    DWC_SCRIPT(0x23ED, 0x23EF, kSegoeSymbol),
    DWC_SCRIPT(0x23F0, 0x23F0, kEmoji),
    DWC_SCRIPT(0x23F1, 0x23F2, kSegoeSymbol),
    DWC_SCRIPT(0x23F3, 0x23F3, kEmoji),
    DWC_SCRIPT(0x23F8, 0x23FA, kSegoeSymbol),
    DWC_SCRIPT(0x24C2, 0x24C2, kSegoeSymbol),
    DWC_SCRIPT(0x25AA, 0x25AB, kSegoeSymbol),
    DWC_SCRIPT(0x25B6, 0x25B6, kSegoeSymbol),
    DWC_SCRIPT(0x25C0, 0x25C0, kSegoeSymbol),
    DWC_SCRIPT(0x25FB, 0x25FC, kSegoeSymbol),
    DWC_SCRIPT(0x25FD, 0x25FE, kEmoji),
    DWC_SCRIPT(0x2600, 0x2604, kSegoeSymbol),
    DWC_SCRIPT(0x260E, 0x260E, kSegoeSymbol),
    DWC_SCRIPT(0x2611, 0x2611, kSegoeSymbol),
    DWC_SCRIPT(0x2614, 0x2615, kEmoji),
    DWC_SCRIPT(0x2618, 0x2618, kSegoeSymbol),
    DWC_SCRIPT(0x261D, 0x261D, kSegoeSymbol),
    DWC_SCRIPT(0x2620, 0x2620, kSegoeSymbol),
    DWC_SCRIPT(0x2622, 0x2623, kSegoeSymbol),
    DWC_SCRIPT(0x2626, 0x2626, kSegoeSymbol),
    DWC_SCRIPT(0x262A, 0x262A, kSegoeSymbol),
    DWC_SCRIPT(0x262E, 0x262F, kSegoeSymbol),
    DWC_SCRIPT(0x2638, 0x263A, kSegoeSymbol),
    DWC_SCRIPT(0x2640, 0x2640, kSegoeSymbol),
    DWC_SCRIPT(0x2642, 0x2642, kSegoeSymbol),
    DWC_SCRIPT(0x2648, 0x2653, kEmoji),
    DWC_SCRIPT(0x265F, 0x2660, kSegoeSymbol),
    DWC_SCRIPT(0x2663, 0x2663, kSegoeSymbol),
    DWC_SCRIPT(0x2665, 0x2666, kSegoeSymbol),
    DWC_SCRIPT(0x2668, 0x2668, kSegoeSymbol),
    DWC_SCRIPT(0x267B, 0x267B, kSegoeSymbol),
    DWC_SCRIPT(0x267E, 0x267E, kSegoeSymbol),
    DWC_SCRIPT(0x267F, 0x267F, kEmoji),
    DWC_SCRIPT(0x2692, 0x2692, kSegoeSymbol),
    DWC_SCRIPT(0x2693, 0x2693, kEmoji),
    DWC_SCRIPT(0x2694, 0x2697, kSegoeSymbol),
    DWC_SCRIPT(0x2699, 0x2699, kSegoeSymbol),
    DWC_SCRIPT(0x269B, 0x269C, kSegoeSymbol),
    DWC_SCRIPT(0x26A0, 0x26A0, kSegoeSymbol),
    DWC_SCRIPT(0x26A1, 0x26A1, kEmoji),
    DWC_SCRIPT(0x26A7, 0x26A7, kSegoeSymbol),
    DWC_SCRIPT(0x26AA, 0x26AB, kEmoji),
    DWC_SCRIPT(0x26B0, 0x26B1, kSegoeSymbol),
    DWC_SCRIPT(0x26BD, 0x26BE, kEmoji),
    DWC_SCRIPT(0x26C4, 0x26C5, kEmoji),
    DWC_SCRIPT(0x26C8, 0x26C8, kSegoeSymbol),
    DWC_SCRIPT(0x26CE, 0x26CE, kEmoji),
    DWC_SCRIPT(0x26CF, 0x26CF, kSegoeSymbol),
    DWC_SCRIPT(0x26D1, 0x26D1, kSegoeSymbol),
    DWC_SCRIPT(0x26D3, 0x26D3, kSegoeSymbol),
    DWC_SCRIPT(0x26D4, 0x26D4, kEmoji),
    DWC_SCRIPT(0x26E9, 0x26E9, kSegoeSymbol),
    DWC_SCRIPT(0x26EA, 0x26EA, kEmoji),
    DWC_SCRIPT(0x26F0, 0x26F1, kSegoeSymbol),
    DWC_SCRIPT(0x26F2, 0x26F3, kEmoji),
    DWC_SCRIPT(0x26F4, 0x26F4, kSegoeSymbol),
    DWC_SCRIPT(0x26F5, 0x26F5, kEmoji),
    DWC_SCRIPT(0x26F7, 0x26F9, kSegoeSymbol),
    DWC_SCRIPT(0x26FA, 0x26FA, kEmoji),
    DWC_SCRIPT(0x26FD, 0x26FD, kEmoji),
    DWC_SCRIPT(0x2702, 0x2702, kSegoeSymbol),
    DWC_SCRIPT(0x2705, 0x2705, kEmoji),
    DWC_SCRIPT(0x2708, 0x2709, kSegoeSymbol),
    DWC_SCRIPT(0x270A, 0x270B, kEmoji),
    DWC_SCRIPT(0x270C, 0x270D, kSegoeSymbol),
    DWC_SCRIPT(0x270F, 0x270F, kSegoeSymbol),
    DWC_SCRIPT(0x2712, 0x2712, kSegoeSymbol),
    DWC_SCRIPT(0x2714, 0x2714, kSegoeSymbol),
    DWC_SCRIPT(0x2716, 0x2716, kSegoeSymbol),
    DWC_SCRIPT(0x271D, 0x271D, kSegoeSymbol),
    DWC_SCRIPT(0x2721, 0x2721, kSegoeSymbol),
    DWC_SCRIPT(0x2728, 0x2728, kEmoji),
    DWC_SCRIPT(0x2733, 0x2734, kSegoeSymbol),
    DWC_SCRIPT(0x2744, 0x2744, kSegoeSymbol),
    DWC_SCRIPT(0x2747, 0x2747, kSegoeSymbol),
    DWC_SCRIPT(0x274C, 0x274C, kEmoji),
    DWC_SCRIPT(0x274E, 0x274E, kEmoji),
    DWC_SCRIPT(0x2753, 0x2755, kEmoji),
    DWC_SCRIPT(0x2757, 0x2757, kEmoji),
    DWC_SCRIPT(0x2763, 0x2764, kSegoeSymbol),
    DWC_SCRIPT(0x2795, 0x2797, kEmoji),
    DWC_SCRIPT(0x27A1, 0x27A1, kSegoeSymbol),
    DWC_SCRIPT(0x27B0, 0x27B0, kEmoji),
    DWC_SCRIPT(0x27BF, 0x27BF, kEmoji),
    DWC_SCRIPT(0x2934, 0x2935, kSegoeSymbol),
    DWC_SCRIPT(0x2B05, 0x2B07, kSegoeSymbol),
    DWC_SCRIPT(0x2B1B, 0x2B1C, kEmoji),
    DWC_SCRIPT(0x2B50, 0x2B50, kEmoji),
    DWC_SCRIPT(0x2B55, 0x2B55, kEmoji),
    DWC_SCRIPT(0x3030, 0x3030, kSegoeSymbol),
    DWC_SCRIPT(0x303D, 0x303D, kSegoeSymbol),
    DWC_SCRIPT(0x3297, 0x3297, kSegoeSymbol),
    DWC_SCRIPT(0x3299, 0x3299, kSegoeSymbol),
    DWC_SCRIPT(0x1F004, 0x1F004, kEmoji),
    DWC_SCRIPT(0x1F0CF, 0x1F0CF, kEmoji),
    DWC_SCRIPT(0x1F170, 0x1F171, kSegoeSymbol),
    DWC_SCRIPT(0x1F17E, 0x1F17F, kSegoeSymbol),
    DWC_SCRIPT(0x1F18E, 0x1F18E, kEmoji),
    DWC_SCRIPT(0x1F191, 0x1F19A, kEmoji),
    DWC_SCRIPT(0x1F1E6, 0x1F1FF, kEmoji),
    DWC_SCRIPT(0x1F201, 0x1F201, kEmoji),
    DWC_SCRIPT(0x1F202, 0x1F202, kSegoeSymbol),
    DWC_SCRIPT(0x1F21A, 0x1F21A, kEmoji),
    DWC_SCRIPT(0x1F22F, 0x1F22F, kEmoji),
    DWC_SCRIPT(0x1F232, 0x1F236, kEmoji),
    DWC_SCRIPT(0x1F237, 0x1F237, kSegoeSymbol),
    DWC_SCRIPT(0x1F238, 0x1F23A, kEmoji),
    DWC_SCRIPT(0x1F250, 0x1F251, kEmoji),
    DWC_SCRIPT(0x1F300, 0x1F64F, kEmoji),
    DWC_SCRIPT(0x1F680, 0x1F6FF, kEmoji),
    DWC_SCRIPT(0x1F7E0, 0x1F7EB, kEmoji),
    DWC_SCRIPT(0x1F7F0, 0x1F7F0, kEmoji),
    DWC_SCRIPT(0x1F900, 0x1F9FF, kEmoji),
    DWC_SCRIPT(0x1FA70, 0x1FAFF, kEmoji),

    DWC_SCRIPT(0x2190, 0x219F, kArrowsSimple),
    DWC_SCRIPT(0x21A0, 0x21FF, kArrows),
    DWC_SCRIPT(0x2200, 0x22FF, kMath),                     // Mathematical operators
    DWC_SCRIPT(0x2300, 0x23FF, kMath),                     // Miscellaneous technical
    DWC_SCRIPT(0x27C0, 0x27EF, kMath),                     // Misc mathematical symbols-A
    DWC_SCRIPT(0x27F0, 0x27FF, kMath),                     // Supplemental arrows-A
    DWC_SCRIPT(0x2900, 0x297F, kMath),                     // Supplemental arrows-B
    DWC_SCRIPT(0x2980, 0x29FF, kMath),                     // Misc mathematical symbols-B
    DWC_SCRIPT(0x2A00, 0x2AFF, kMath),                     // Supplemental math operators
    DWC_SCRIPT(0x1D400, 0x1D7FF, kMath),                   // Mathematical alphanumeric
    DWC_SCRIPT(0x1EE00, 0x1EEFF, kMath),                   // Arabic mathematical
    DWC_SCRIPT(0x1F780, 0x1F7FF, kMath),                   // Geometric shapes extended
    DWC_SCRIPT(0x2B00, 0x2BFF, kSegoeSymbol),              // Misc symbols and arrows
    DWC_SCRIPT(0x1F0A0, 0x1F0FF, kSegoeSymbol),            // Playing cards
    DWC_SCRIPT(0x1F700, 0x1F77F, kSegoeSymbol),            // Alchemical symbols
    DWC_SCRIPT(0x2440, 0x245F, kSegoeSymbol),              // Optical character recognition
    DWC_SCRIPT(0x25FD, 0x25FE, kEmoji),
    DWC_SCRIPT(0x2614, 0x2615, kEmoji),
    DWC_SCRIPT(0x261D, 0x261D, kEmoji),
    DWC_SCRIPT(0x2648, 0x2653, kEmoji),
    DWC_SCRIPT(0x267F, 0x267F, kEmoji),
    DWC_SCRIPT(0x2693, 0x2693, kEmoji),
    DWC_SCRIPT(0x26A1, 0x26A1, kEmoji),
    DWC_SCRIPT(0x26AA, 0x26AB, kEmoji),
    DWC_SCRIPT(0x26BD, 0x26BE, kEmoji),
    DWC_SCRIPT(0x26C4, 0x26C5, kEmoji),
    DWC_SCRIPT(0x26CE, 0x26CE, kEmoji),
    DWC_SCRIPT(0x26D4, 0x26D4, kEmoji),
    DWC_SCRIPT(0x26EA, 0x26EA, kEmoji),
    DWC_SCRIPT(0x26F2, 0x26F3, kEmoji),
    DWC_SCRIPT(0x26F5, 0x26F5, kEmoji),
    DWC_SCRIPT(0x26F9, 0x26FA, kEmoji),
    DWC_SCRIPT(0x26FD, 0x26FD, kEmoji),
    DWC_SCRIPT(0x2705, 0x2705, kEmoji),
    DWC_SCRIPT(0x270A, 0x270D, kEmoji),
    DWC_SCRIPT(0x2728, 0x2728, kEmoji),
    DWC_SCRIPT(0x274C, 0x274C, kEmoji),
    DWC_SCRIPT(0x274E, 0x274E, kEmoji),
    DWC_SCRIPT(0x2753, 0x2755, kEmoji),
    DWC_SCRIPT(0x2757, 0x2757, kEmoji),
    DWC_SCRIPT(0x2795, 0x2797, kEmoji),
    DWC_SCRIPT(0x27B0, 0x27B0, kEmoji),
    DWC_SCRIPT(0x27BF, 0x27BF, kEmoji),
    // The pictograph blocks, which hold no text-presentation characters.
    DWC_SCRIPT(0x1F300, 0x1F5FF, kEmoji),
    DWC_SCRIPT(0x1F600, 0x1F64F, kEmoji),
    DWC_SCRIPT(0x1F680, 0x1F6FF, kEmoji),
    DWC_SCRIPT(0x1F900, 0x1F9FF, kEmoji),
    DWC_SCRIPT(0x25A0, 0x25FF, kMath),                     // Geometric shapes
    DWC_SCRIPT(0x2580, 0x259F, kBlockElements),
    DWC_SCRIPT(0x2600, 0x27BF, kSegoeSymbol),              // Symbols and dingbats
    DWC_SCRIPT(0x20000, 0x2FA1F, kHanSupplementary),
    DWC_SCRIPT(0xFFF0, 0xFFFF, kSpecials),
    DWC_SCRIPT(0x1100, 0x11FF, kHangul),        // Hangul Jamo
    DWC_SCRIPT(0xAC00, 0xD7AF, kHangul),        // Hangul syllables
    DWC_SCRIPT(0xFF65, 0xFF9F, kKatakanaOrHiragana),   // halfwidth katakana
    DWC_SCRIPT(0xFFA0, 0xFFDC, kHangul),               // halfwidth hangul
    DWC_SCRIPT(0x3100, 0x312F, kTraditionalHan),   // Bopomofo
    DWC_SCRIPT(0x3040, 0x309F, kKatakanaOrHiragana),
    DWC_SCRIPT(0x30A0, 0x30FF, kKatakanaOrHiragana),
    DWC_SCRIPT(0x31F0, 0x31FF, kKatakanaOrHiragana),
    // Unified Han, whose list is chosen from the locale. kSimplifiedHan is
    // named here only so the row is recognizable; OrderForHan answers it.
    DWC_SCRIPT(0x3400, 0x4DBF, kSimplifiedHan),
    DWC_SCRIPT(0x4E00, 0x9FFF, kSimplifiedHan),
    DWC_SCRIPT(0xF900, 0xFAFF, kSimplifiedHan),
    // GetFallbackFamily makes fullwidth ASCII Han outright.
    DWC_SCRIPT(0xFF01, 0xFF5E, kSimplifiedHan),

    // Every script kScriptToFontFamilies names, over the ranges ICU gives it.
    // These come last, so a row above answers wherever one exists; those were
    // measured against DirectWrite's own fallback, which has no source.
// 170 rows, generated by tools/testing/gen_script_rows.py.
    DWC_SCRIPT(0x0041, 0x005A, kTimesNewRoman),
    DWC_SCRIPT(0x0061, 0x007A, kTimesNewRoman),
    DWC_SCRIPT(0x00AA, 0x00AA, kTimesNewRoman),
    DWC_SCRIPT(0x00BA, 0x00BA, kTimesNewRoman),
    DWC_SCRIPT(0x00C0, 0x00D6, kTimesNewRoman),
    DWC_SCRIPT(0x00D8, 0x00F6, kTimesNewRoman),
    DWC_SCRIPT(0x00F8, 0x02B8, kTimesNewRoman),
    DWC_SCRIPT(0x02E0, 0x02E4, kTimesNewRoman),
    DWC_SCRIPT(0x02EA, 0x02EB, kTraditionalHan),
    DWC_SCRIPT(0x0370, 0x03E1, kTimesNewRoman),
    DWC_SCRIPT(0x03E2, 0x03EF, kSegoeSymbol),
    DWC_SCRIPT(0x03F0, 0x0484, kTimesNewRoman),
    DWC_SCRIPT(0x0487, 0x052F, kTimesNewRoman),
    DWC_SCRIPT(0x0530, 0x058F, kArmenian),
    DWC_SCRIPT(0x0590, 0x05FF, kHebrew),
    DWC_SCRIPT(0x0600, 0x06FF, kArabic),
    DWC_SCRIPT(0x0700, 0x074F, kSyriac),
    DWC_SCRIPT(0x0750, 0x077F, kArabic),
    DWC_SCRIPT(0x0780, 0x07BF, kThaana),
    DWC_SCRIPT(0x07C0, 0x07FF, kEbrima),
    DWC_SCRIPT(0x0860, 0x086F, kSyriac),
    DWC_SCRIPT(0x0870, 0x08FF, kArabic),
    DWC_SCRIPT(0x0900, 0x097F, kDevanagari),
    DWC_SCRIPT(0x0980, 0x09FF, kBengali),
    DWC_SCRIPT(0x0A00, 0x0A7F, kGurmukhi),
    DWC_SCRIPT(0x0A80, 0x0AFF, kGujarati),
    DWC_SCRIPT(0x0B00, 0x0B7F, kOriya),
    DWC_SCRIPT(0x0B80, 0x0BFF, kTamil),
    DWC_SCRIPT(0x0C00, 0x0C7F, kTelugu),
    DWC_SCRIPT(0x0C80, 0x0CFF, kKannada),
    DWC_SCRIPT(0x0D00, 0x0D7F, kMalayalam),
    DWC_SCRIPT(0x0D80, 0x0DFF, kSinhala),
    DWC_SCRIPT(0x0E00, 0x0E7F, kThai),
    DWC_SCRIPT(0x0E80, 0x0EFF, kLao),
    DWC_SCRIPT(0x0F00, 0x0FD4, kTibetan),
    DWC_SCRIPT(0x0FD9, 0x0FFF, kTibetan),
    DWC_SCRIPT(0x1000, 0x109F, kMyanmar),
    DWC_SCRIPT(0x10A0, 0x10FF, kGeorgian),
    DWC_SCRIPT(0x1100, 0x11FF, kHangul),
    DWC_SCRIPT(0x1200, 0x139F, kEthiopic),
    DWC_SCRIPT(0x13A0, 0x13FF, kCherokee),
    DWC_SCRIPT(0x1400, 0x167F, kCanadianAboriginal),
    DWC_SCRIPT(0x1680, 0x16EA, kSegoeHistoricOrSymbol),
    DWC_SCRIPT(0x16EE, 0x16FF, kSegoeHistoricOrSymbol),
    DWC_SCRIPT(0x1780, 0x17FF, kKhmer),
    DWC_SCRIPT(0x1800, 0x1801, kMongolian),
    DWC_SCRIPT(0x1804, 0x1804, kMongolian),
    DWC_SCRIPT(0x1806, 0x18AF, kMongolian),
    DWC_SCRIPT(0x18B0, 0x18FF, kCanadianAboriginal),
    DWC_SCRIPT(0x1950, 0x197F, kTaiLe),
    DWC_SCRIPT(0x1980, 0x19DF, kNewTaiLue),
    DWC_SCRIPT(0x19E0, 0x19FF, kKhmer),
    DWC_SCRIPT(0x1A00, 0x1A1F, kBuginese),
    DWC_SCRIPT(0x1C50, 0x1C7F, kNirmala),
    DWC_SCRIPT(0x1C80, 0x1C8F, kTimesNewRoman),
    DWC_SCRIPT(0x1C90, 0x1CBF, kGeorgian),
    DWC_SCRIPT(0x1D00, 0x1DBF, kTimesNewRoman),
    DWC_SCRIPT(0x1E00, 0x1FFF, kTimesNewRoman),
    DWC_SCRIPT(0x2071, 0x2071, kTimesNewRoman),
    DWC_SCRIPT(0x207F, 0x207F, kTimesNewRoman),
    DWC_SCRIPT(0x2090, 0x209C, kTimesNewRoman),
    DWC_SCRIPT(0x2126, 0x2126, kTimesNewRoman),
    DWC_SCRIPT(0x212A, 0x212B, kTimesNewRoman),
    DWC_SCRIPT(0x2132, 0x2132, kTimesNewRoman),
    DWC_SCRIPT(0x214E, 0x214E, kTimesNewRoman),
    DWC_SCRIPT(0x2160, 0x2188, kTimesNewRoman),
    DWC_SCRIPT(0x218C, 0x218F, kTimesNewRoman),
    DWC_SCRIPT(0x2800, 0x28FF, kSegoeSymbol),
    DWC_SCRIPT(0x2C00, 0x2C5F, kSegoeHistoricOrSymbol),
    DWC_SCRIPT(0x2C60, 0x2C7F, kTimesNewRoman),
    DWC_SCRIPT(0x2C80, 0x2CFF, kSegoeSymbol),
    DWC_SCRIPT(0x2D00, 0x2D2F, kGeorgian),
    DWC_SCRIPT(0x2D30, 0x2D7F, kEbrima),
    DWC_SCRIPT(0x2D80, 0x2DDF, kEthiopic),
    DWC_SCRIPT(0x2DE0, 0x2DFF, kTimesNewRoman),
    DWC_SCRIPT(0x2E80, 0x2FDF, kSimplifiedHan),
    DWC_SCRIPT(0x3000, 0x302D, kSimplifiedHan),
    DWC_SCRIPT(0x302E, 0x302F, kHangul),
    DWC_SCRIPT(0x3030, 0x303F, kSimplifiedHan),
    DWC_SCRIPT(0x3040, 0x30FF, kKatakanaOrHiragana),
    DWC_SCRIPT(0x3100, 0x312F, kTraditionalHan),
    DWC_SCRIPT(0x3130, 0x318F, kHangul),
    DWC_SCRIPT(0x31A0, 0x31BF, kTraditionalHan),
    DWC_SCRIPT(0x31F0, 0x31FF, kKatakanaOrHiragana),
    DWC_SCRIPT(0x3200, 0x321E, kHangul),
    DWC_SCRIPT(0x3260, 0x327E, kHangul),
    DWC_SCRIPT(0x32D0, 0x32FE, kKatakanaOrHiragana),
    DWC_SCRIPT(0x3300, 0x3357, kKatakanaOrHiragana),
    DWC_SCRIPT(0x3400, 0x4DBF, kSimplifiedHan),
    DWC_SCRIPT(0x4E00, 0x9FFF, kSimplifiedHan),
    DWC_SCRIPT(0xA000, 0xA4CF, kYi),
    DWC_SCRIPT(0xA4D0, 0xA4FF, kLisu),
    DWC_SCRIPT(0xA500, 0xA63F, kEbrima),
    DWC_SCRIPT(0xA640, 0xA69F, kTimesNewRoman),
    DWC_SCRIPT(0xA722, 0xA787, kTimesNewRoman),
    DWC_SCRIPT(0xA78B, 0xA7FF, kTimesNewRoman),
    DWC_SCRIPT(0xA840, 0xA87F, kPhagsPa),
    DWC_SCRIPT(0xA8E0, 0xA8FF, kDevanagari),
    DWC_SCRIPT(0xA960, 0xA97F, kHangul),
    DWC_SCRIPT(0xA980, 0xA9CE, kJavanese),
    DWC_SCRIPT(0xA9D0, 0xA9DF, kJavanese),
    DWC_SCRIPT(0xA9E0, 0xA9FF, kMyanmar),
    DWC_SCRIPT(0xAA60, 0xAA7F, kMyanmar),
    DWC_SCRIPT(0xAAE0, 0xAAFF, kNirmala),
    DWC_SCRIPT(0xAB00, 0xAB2F, kEthiopic),
    DWC_SCRIPT(0xAB30, 0xAB5A, kTimesNewRoman),
    DWC_SCRIPT(0xAB5C, 0xAB69, kTimesNewRoman),
    DWC_SCRIPT(0xAB6C, 0xAB6F, kTimesNewRoman),
    DWC_SCRIPT(0xAB70, 0xABBF, kCherokee),
    DWC_SCRIPT(0xABC0, 0xABFF, kNirmala),
    DWC_SCRIPT(0xAC00, 0xD7FF, kHangul),
    DWC_SCRIPT(0xF900, 0xFAFF, kSimplifiedHan),
    DWC_SCRIPT(0xFB00, 0xFB06, kTimesNewRoman),
    DWC_SCRIPT(0xFB07, 0xFB12, kHebrew),
    DWC_SCRIPT(0xFB13, 0xFB17, kArmenian),
    DWC_SCRIPT(0xFB18, 0xFB4F, kHebrew),
    DWC_SCRIPT(0xFB50, 0xFDFF, kArabic),
    DWC_SCRIPT(0xFE2E, 0xFE2F, kTimesNewRoman),
    DWC_SCRIPT(0xFE70, 0xFEFF, kArabic),
    DWC_SCRIPT(0xFF21, 0xFF3A, kTimesNewRoman),
    DWC_SCRIPT(0xFF41, 0xFF5A, kTimesNewRoman),
    DWC_SCRIPT(0xFF66, 0xFF6F, kKatakanaOrHiragana),
    DWC_SCRIPT(0xFF71, 0xFF9D, kKatakanaOrHiragana),
    DWC_SCRIPT(0xFFA0, 0xFFBE, kHangul),
    DWC_SCRIPT(0xFFC2, 0xFFC7, kHangul),
    DWC_SCRIPT(0xFFCA, 0xFFCF, kHangul),
    DWC_SCRIPT(0xFFD2, 0xFFD7, kHangul),
    DWC_SCRIPT(0xFFDA, 0xFFDC, kHangul),
    DWC_SCRIPT(0x10140, 0x1018F, kTimesNewRoman),
    DWC_SCRIPT(0x101A0, 0x101A0, kTimesNewRoman),
    DWC_SCRIPT(0x10280, 0x102DF, kSegoeHistoric),
    DWC_SCRIPT(0x10300, 0x1034F, kSegoeHistoricOrSymbol),
    DWC_SCRIPT(0x103A0, 0x103DF, kSegoeHistoric),
    DWC_SCRIPT(0x10400, 0x1044F, kSegoeSymbol),
    DWC_SCRIPT(0x10450, 0x1047F, kSegoeHistoric),
    DWC_SCRIPT(0x10480, 0x104AF, kEbrima),
    DWC_SCRIPT(0x10780, 0x107BF, kTimesNewRoman),
    DWC_SCRIPT(0x10800, 0x1085F, kSegoeHistoric),
    DWC_SCRIPT(0x10920, 0x1093F, kSegoeHistoric),
    DWC_SCRIPT(0x109A0, 0x109FF, kSegoeHistoricOrSymbol),
    DWC_SCRIPT(0x10A00, 0x10A7F, kSegoeHistoric),
    DWC_SCRIPT(0x10B40, 0x10B7F, kSegoeHistoric),
    DWC_SCRIPT(0x10C00, 0x10C4F, kSegoeHistoricOrSymbol),
    DWC_SCRIPT(0x10E60, 0x10E7F, kArabic),
    DWC_SCRIPT(0x10EC0, 0x10EFF, kArabic),
    DWC_SCRIPT(0x11000, 0x1107F, kSegoeHistoric),
    DWC_SCRIPT(0x110D0, 0x110FF, kNirmala),
    DWC_SCRIPT(0x111E0, 0x111FF, kSinhala),
    DWC_SCRIPT(0x11660, 0x1167F, kMongolian),
    DWC_SCRIPT(0x116D0, 0x116FF, kMyanmar),
    DWC_SCRIPT(0x11AB0, 0x11ABF, kCanadianAboriginal),
    DWC_SCRIPT(0x11B00, 0x11B5F, kDevanagari),
    DWC_SCRIPT(0x11FB0, 0x11FBF, kLisu),
    DWC_SCRIPT(0x11FC0, 0x11FFF, kTamil),
    DWC_SCRIPT(0x12000, 0x1254F, kSegoeHistoric),
    DWC_SCRIPT(0x13000, 0x143FF, kSegoeHistoric),
    DWC_SCRIPT(0x16FE2, 0x16FE3, kSimplifiedHan),
    DWC_SCRIPT(0x16FE5, 0x16FFF, kSimplifiedHan),
    DWC_SCRIPT(0x1AFF0, 0x1B16F, kKatakanaOrHiragana),
    DWC_SCRIPT(0x1D200, 0x1D24F, kTimesNewRoman),
    DWC_SCRIPT(0x1DF00, 0x1DFFF, kTimesNewRoman),
    DWC_SCRIPT(0x1E000, 0x1E02F, kSegoeHistoricOrSymbol),
    DWC_SCRIPT(0x1E030, 0x1E08F, kTimesNewRoman),
    DWC_SCRIPT(0x1E7E0, 0x1E7FF, kEthiopic),
    DWC_SCRIPT(0x1EE00, 0x1EEFF, kArabic),
    DWC_SCRIPT(0x1F200, 0x1F200, kKatakanaOrHiragana),
    DWC_SCRIPT(0x20000, 0x2A6DF, kSimplifiedHan),
    DWC_SCRIPT(0x2A700, 0x2EE5F, kSimplifiedHan),
    DWC_SCRIPT(0x2F800, 0x2FA1F, kSimplifiedHan),
    DWC_SCRIPT(0x30000, 0x3347F, kSimplifiedHan),
};

#undef DWC_SCRIPT


// Which list unified Han resolves to, mirroring
// LayoutLocale::LocaleForHan()->GetScriptForHan(), which font_fallback_win.cc
// calls when it fills USCRIPT_HAN.
//
// layout_locale.cc's ComputeScriptForHan asks locale_to_script_mapping.cc's
// ScriptCodeForHanFromSubtags, which walks the subtags and takes the first
// that disambiguates: a two-letter region, or a four-letter script name.
struct HanChoice
{
    const char* subtag;
    const char* const* families;
    unsigned count;
};

// Null when the locale settles nothing, which is HasScriptForHan() answering
// false. ComputeScriptForHan falls back to simplified Han there but leaves
// has_script_for_han_ clear, and LocaleForHan tests that flag.
const char* const* HanForLocale(const char* locale, unsigned* count)
{
    // The script names ToSkFontMgrLocale answers, which layout_locale.cc
    // resolves before anything else:
    //
    //     USCRIPT_KATAKANA_OR_HIRAGANA -> "ja"   USCRIPT_HANGUL -> "ko"
    //     USCRIPT_SIMPLIFIED_HAN -> "zh-Hans"    USCRIPT_TRADITIONAL_HAN -> "zh-Hant"
    //
    // The Windows fallback is handed that string; Linux is handed the locale as
    // written, so ja-Hang has to resolve its script subtag here.
    static const HanChoice kScriptChoices[] = {
        {"hant", kTraditionalHan, DWC_COUNT(kTraditionalHan)},
        {"hans", kSimplifiedHan, DWC_COUNT(kSimplifiedHan)},
        {"jpan", kKatakanaOrHiragana, DWC_COUNT(kKatakanaOrHiragana)},
        {"kana", kKatakanaOrHiragana, DWC_COUNT(kKatakanaOrHiragana)},
        {"hira", kKatakanaOrHiragana, DWC_COUNT(kKatakanaOrHiragana)},
        {"kore", kHangul, DWC_COUNT(kHangul)},
        {"hang", kHangul, DWC_COUNT(kHangul)},
    };
    // ScriptCodeForHanFromRegion. A region names a script only when the
    // language did not, so it is consulted after the language.
    static const HanChoice kChoices[] = {
        {"hk", kTraditionalHan, DWC_COUNT(kTraditionalHan)},
        {"mo", kTraditionalHan, DWC_COUNT(kTraditionalHan)},
        {"tw", kTraditionalHan, DWC_COUNT(kTraditionalHan)},
        {"jp", kKatakanaOrHiragana, DWC_COUNT(kKatakanaOrHiragana)},
        {"kr", kHangul, DWC_COUNT(kHangul)},
    };
    if (locale == nullptr) {
        return nullptr;
    }

    // The language subtag itself decides only when it is already an
    // unambiguous Han script. ja and ko are, zh is not.
    char head[8] = {};
    unsigned n = 0;
    while (n + 1 < sizeof(head) && locale[n] != '\0' && locale[n] != '-' &&
           locale[n] != '_' && locale[n] != '.' && locale[n] != '@') {
        head[n] = static_cast<char>(tolower(static_cast<unsigned char>(locale[n])));
        ++n;
    }
    // The script subtag first, wherever it sits, because that is the order
    // layout_locale.cc reads them in.
    for (const char* p = locale; *p != '\0' && *p != '.' && *p != '@';) {
        if (*p != '-' && *p != '_') {
            ++p;
            continue;
        }
        ++p;
        char sub[8] = {};
        unsigned k = 0;
        while (k + 1 < sizeof(sub) && p[k] != '\0' && p[k] != '-' &&
               p[k] != '_' && p[k] != '.' && p[k] != '@') {
            sub[k] = static_cast<char>(tolower(static_cast<unsigned char>(p[k])));
            ++k;
        }
        for (const HanChoice& c : kScriptChoices) {
            if (std::strcmp(sub, c.subtag) == 0) {
                *count = c.count;
                return c.families;
            }
        }
        p += k;
    }
    if (std::strcmp(head, "ja") == 0) {
        *count = DWC_COUNT(kKatakanaOrHiragana);
        return kKatakanaOrHiragana;
    }
    if (std::strcmp(head, "ko") == 0) {
        *count = DWC_COUNT(kHangul);
        return kHangul;
    }

    // Then the subtags after it, first one that disambiguates.
    for (const char* p = locale; *p != '\0' && *p != '.' && *p != '@';) {
        if (*p != '-' && *p != '_') {
            ++p;
            continue;
        }
        ++p;
        char sub[8] = {};
        unsigned k = 0;
        while (k + 1 < sizeof(sub) && p[k] != '\0' && p[k] != '-' &&
               p[k] != '_' && p[k] != '.' && p[k] != '@') {
            sub[k] = static_cast<char>(tolower(static_cast<unsigned char>(p[k])));
            ++k;
        }
        for (const HanChoice& c : kChoices) {
            if (std::strcmp(sub, c.subtag) == 0) {
                *count = c.count;
                return c.families;
            }
        }
        p += k;
    }
    return nullptr;
}

// The environment is read as ICU reads it, LC_ALL before LC_CTYPE before LANG.
const char* const* SystemHan(unsigned* count)
{
    const char* env = std::getenv("LC_ALL");
    if (env == nullptr || env[0] == '\0') {
        env = std::getenv("LC_CTYPE");
    }
    if (env == nullptr || env[0] == '\0') {
        env = std::getenv("LANG");
    }
    return HanForLocale(env, count);
}

// How initializeScriptFontMap seeds USCRIPT_HAN, from the system locale
// alone. GetFallbackFamily narrows that per call with the run's own language,
// which is not mirrored here. That language reaches fontconfig only as FC_LANG
// on the pattern Chromium sorts with, and the charset walk that reads the
// sorted set carries no language.
// Which list unified Han resolves to for one sort. The run's language reaches
// fontconfig as FC_LANG on the pattern being sorted; when it names no Han
// script the system locale answers, as initializeScriptFontMap seeds it.
// Null is GetFallbackFamily leaving USCRIPT_HAN unsettled.
const char* const* HanForSort(const char* locale, unsigned* count)
{
    if (locale != nullptr) {
        if (const char* const* families = HanForLocale(locale, count)) {
            return families;
        }
    }
    return SystemHan(count);
}

struct FamilyList
{
    const char* const* families;
    unsigned count;
    // Unified Han, whose family follows the run's language. The set was put in
    // that language's order at sort time, so the walk over it answers on its
    // own and this call has nothing to add.
    bool by_set_order;
};

FamilyList ScriptFor(const unsigned codepoint)
{
    for (const ScriptFonts& s : kScripts) {
        if (codepoint < s.first || codepoint > s.last) {
            continue;
        }
        if (s.families == kSimplifiedHan || s.families == kTimesNewRoman) {
            return {nullptr, 0, true};
        }
        return {s.families, s.count, false};
    }
    return {nullptr, 0, false};
}

// What FontCache::GetFallbackFamilyNameFromHardcodedChoices walks when no
// script font covers the character, transcribed from font_cache_skia_win.cc.
// Both lists keep the fonts no Windows ships, which ShipsWithWindows drops,
// so the order stays comparable to the source.
constexpr const char* kCjkLastResort[] = {
    "arial unicode ms", "ms pgothic", "simsun",         "gulim",     "pmingliu",
    "wenquanyi zen hei", "ar pl shanheisun uni",        "ar pl zenkai uni",
    "han nom a",         "code2000"};
constexpr const char* kCommonLastResort[] = {
    "tahoma",       "arial unicode ms", "lucida sans unicode", "microsoft sans serif",
    "palatino linotype", "dejavu serif", "dejavu sasns",       "freeserif",
    "freesans",     "gentium",          "gentiumalt",          "ms pgothic",
    "simsun",       "gulim",            "pmingliu",            "code2000"};

// The same list in the order the reference walks it, which takes Lucida Sans
// Unicode before Tahoma. Measured on the nine codepoints that reach the walk
// with both families covering them, under ja, ko, zh-CN and zh-TW; U+204A,
// which only Tahoma covers, answers Tahoma on both sides either way. Nothing
// in font_cache_skia_win.cc accounts for it, so the list above stays as the
// source writes it and only the walk order differs.
constexpr const char* kCommonWalkOrder[] = {
    "lucida sans unicode", "arial unicode ms", "tahoma", "microsoft sans serif",
    "palatino linotype", "dejavu serif", "dejavu sasns",       "freeserif",
    "freesans",     "gentium",          "gentiumalt",          "ms pgothic",
    "simsun",       "gulim",            "pmingliu",            "code2000"};
static_assert(sizeof(kCommonWalkOrder) == sizeof(kCommonLastResort),
              "the walk order has to be the same list");

// What IDWriteFontFallback answers once the script table and the pan-Unicode
// list have both missed, which is where font_cache_skia_win.cc calls
// GetDWriteFallbackFamily. DirectWrite resolves it internally, so there is no
// source to read, and DWriteCore cannot be asked either: its own fallback runs
// through fontconfig, so the question re-derives the Linux answer and re-enters
// the FcFontSort above. The rows are measured against the reference the way
// family_match.cpp's weight rule is, each read across its whole block rather
// than the codepoints a sweep samples.
//
// A row measured under one language only is not safe to add, because the
// renderer answers every language from whichever asked first. Each row here
// holds across all seven per-language sweeps.
struct DWriteFallback
{
    unsigned first;
    unsigned last;
    // The family walked after the pan-Unicode list. Null where the range needs
    // no answer past it.
    const char* family;
    // Set where the run's Han family is walked before that list. Its value is
    // the family an unsettled run takes; each route finds the run's own. Rows
    // without it are answered by the per-character walk alone.
    const char* front;
    // Set where DirectWrite answers with `family` even though the pan-Unicode
    // list also covers the character; the family is then walked ahead of the
    // list. The halfwidth katakana marks answer from MS PGothic, which Yu
    // Gothic in the list also covers.
    bool ahead = false;
};

constexpr DWriteFallback kDWriteFallback[] = {
    // Vertical and halfwidth forms no language family claims. Under ja, zh-CN
    // and zh-TW the run's own family answers these; these rows are what the
    // walk reaches otherwise.
    {0xFE32, 0xFE32, "Microsoft JhengHei UI", nullptr},  // two em dash
    {0xFE47, 0xFE48, "Microsoft JhengHei UI", nullptr},  // vertical brackets
    {0xFF65, 0xFF65, "MS PGothic", nullptr, true},       // halfwidth middle dot
    {0xFF70, 0xFF70, "MS PGothic", nullptr, true},       // prolonged sound mark
    {0xFF9E, 0xFF9F, "MS PGothic", nullptr, true},       // sound marks
    // Mtavruli, which arrived in Unicode 11 and which Sylfaen never gained.
    {0x1C90, 0x1CBF, "Segoe UI", nullptr},
    {0x1CD0, 0x1CFF, "Nirmala UI", nullptr},    // Vedic extensions
    {0x3190, 0x319F, "Yu Gothic UI", nullptr},  // Kanbun
    {0xA640, 0xA69F, "Microsoft Sans Serif", nullptr},  // Cyrillic extended-B
    {0xA830, 0xA83F, "Nirmala UI", nullptr},    // common Indic number forms
    // Han ranges, which the sorted set answers on the dynamic route and a
    // static build has to spell out. The front is the family an unsettled
    // USCRIPT_HAN reaches; U+F900 needs both, since Windows answers it from
    // that family, then the list, then DirectWrite, in that order. Both rows
    // hold under every language, read with a different codepoint per locale.
    {0x2E80, 0x2FDF, "Yu Gothic UI", "Microsoft YaHei"},
    {0x3000, 0x302D, nullptr, "Microsoft YaHei"},
    {0xF900, 0xFAFF, "Microsoft JhengHei UI", "Microsoft YaHei"},
};

// Which families the last sorts turned up, and the charset of each. Small and
// bounded, since a fallback set is a few hundred fonts at most and only the
// families named in the table above are ever looked up.
struct Known
{
    const void* charset;
    char family[64];
};
// A fallback sort returns every scalable font on the machine, and a family
// this table names is unreachable unless its charset was remembered, so the
// bound has to cover the whole set rather than a prefix of it.
constexpr unsigned kMaxKnown = 4096;
Known g_known[kMaxKnown];
unsigned g_known_count = 0;

void Remember(const void* charset, const char* family)
{
    if (charset == nullptr || family == nullptr) {
        return;
    }
    for (unsigned i = 0; i < g_known_count; ++i) {
        if (g_known[i].charset == charset && strcasecmp(g_known[i].family, family) == 0) {
            return;
        }
    }
    if (g_known_count >= kMaxKnown) {
        return;
    }
    g_known[g_known_count].charset = charset;
    (void)std::snprintf(g_known[g_known_count].family, sizeof(g_known[0].family), "%s", family);
    ++g_known_count;
}

// Names a Han list for the log.
const char* HanName(const char* const* h)
{
    if (h == kKatakanaOrHiragana) { return "kana"; }
    if (h == kHangul) { return "hangul"; }
    if (h == kSimplifiedHan) { return "simplified"; }
    if (h == kTraditionalHan) { return "traditional"; }
    return h == nullptr ? "none" : "other";
}

// One line per decision in the CJK range, under DWC_FALLBACK_LOG.
void LogPick(unsigned codepoint, const char* stage, const char* family)
{
    if (std::getenv("DWC_FALLBACK_LOG") == nullptr || codepoint < 0x3100 ||
        codepoint > 0xFFFF) {
        return;
    }
    (void)std::fprintf(stderr, "chromium-patch: fallback: pid=%d U+%04X %s -> %s\n",
                       static_cast<int>(getpid()), codepoint, stage, family);
}

const char* FamilyOf(const void* charset)
{
    for (unsigned i = 0; i < g_known_count; ++i) {
        if (g_known[i].charset == charset) {
            return g_known[i].family;
        }
    }
    return nullptr;
}

// Whether this font answers to that family name. Asked instead of comparing
// against FamilyOf, which can only report one of the names.
bool HasFamily(const void* charset, const char* family)
{
    for (unsigned i = 0; i < g_known_count; ++i) {
        if (g_known[i].charset == charset && strcasecmp(g_known[i].family, family) == 0) {
            return true;
        }
    }
    return false;
}

// Whether this font answers to that family name, asked of the sorted pattern
// rather than of the learned table, which only holds fonts that carry a
// charset.
bool PatternHasFamily(const void* font, const char* family)
{
    static const auto get_string = Sym<PatternGetStringFn>("FcPatternGetString");
    if (get_string == nullptr) {
        return false;
    }
    for (int n = 0;; ++n) {
        unsigned char* name = nullptr;
        if (get_string(font, "family", n, &name) != kFcResultMatch) {
            return false;
        }
        if (strcasecmp(reinterpret_cast<const char*>(name), family) == 0) {
            return true;
        }
    }
}

// Move every font of one family to the front, keeping the order the sort gave
// them, and report how far the front now reaches.
int MoveToFront(FcFontSet* set, const char* family, int front)
{
    for (int i = front; i < set->nfont; ++i) {
        if (!PatternHasFamily(set->fonts[i], family)) {
            continue;
        }
        void* moved = set->fonts[i];
        std::memmove(static_cast<void*>(&set->fonts[front + 1]), static_cast<const void*>(&set->fonts[front]),
                     static_cast<size_t>(i - front) * sizeof(set->fonts[0]));
        set->fonts[front] = moved;
        ++front;
    }
    return front;
}

bool SetHasFamily(const FcFontSet* set, const char* family)
{
    for (int i = 0; i < set->nfont; ++i) {
        if (PatternHasFamily(set->fonts[i], family)) {
            return true;
        }
    }
    return false;
}

// Put the unified-Han answer where the walk finds it first.
//
// Which family draws Han depends on the run's language, and the walk in
// gfx::CachedFontSet::GetFallbackFontForChar carries no language: it takes the
// first font of the set that covers the character. The language is on the
// pattern being sorted, so the order is decided here, once per locale, and the
// walk then reproduces Windows on its own. Everything with a script of its own
// is answered per character above, so the order it sees does not matter.
void OrderForHan(const void* pattern, FcFontSet* set)
{
    static const auto get_string = Sym<PatternGetStringFn>("FcPatternGetString");
    if (get_string == nullptr) {
        return;
    }
    // Skia stamps the sorts it makes with the wrapper it wants. The fallback
    // sort in ui/gfx/font_fallback_linux.cc does not, and it is the only one
    // whose set is walked for coverage.
    unsigned char* wrapper = nullptr;
    if (get_string(pattern, "fontwrapper", 0, &wrapper) == kFcResultMatch) {
        return;
    }

    unsigned char* lang = nullptr;
    const char* locale = get_string(pattern, "lang", 0, &lang) == kFcResultMatch
                             ? reinterpret_cast<const char*>(lang)
                             : nullptr;
    unsigned count = 0;
    const char* const* han = HanForSort(locale, &count);
    const bool unsettled = han == nullptr;
    if (unsettled) {
        han = kSimplifiedHan;
        count = DWC_COUNT(kSimplifiedHan);
    }

    // Latin, Greek and Cyrillic first: their family is the same whatever the
    // language, and it covers no Han, so it never stands in front of one.
    int front = MoveToFront(set, kTimesNewRoman[0], 0);
    // FirstAvailableFont takes one installed candidate and memoizes it, so the
    // families after it never draw Han however much they cover.
    for (unsigned i = 0; i < count; ++i) {
        if (!ShipsWithWindows(han[i]) || !SetHasFamily(set, han[i])) {
            continue;
        }
        front = MoveToFront(set, han[i], front);
        break;
    }
    // Then the pan-Unicode list, for the characters that family lacks.
    const char* const* last = unsettled ? kCjkLastResort : kCommonLastResort;
    const unsigned last_count = unsettled ? DWC_COUNT(kCjkLastResort)
                                          : DWC_COUNT(kCommonLastResort);
    for (unsigned i = 0; i < last_count; ++i) {
        if (ShipsWithWindows(last[i])) {
            front = MoveToFront(set, last[i], front);
        }
    }
    // Last, what DirectWrite answers for the ranges the sorted set is what
    // decides. Those are answered by the walk over this set rather than per
    // character, so the family has to sit in it, behind everything above:
    // the run's own Han family covers the range in every language but Korean,
    // and only when it does not does this one get reached.
    for (const DWriteFallback& row : kDWriteFallback) {
        if (row.front == nullptr || row.family == nullptr ||
            !ShipsWithWindows(row.family)) {
            continue;
        }
        front = MoveToFront(set, row.family, front);
    }
    if (std::getenv("DWC_FALLBACK_LOG") != nullptr) {
        unsigned char* first = nullptr;
        (void)get_string(set->fonts[0], "family", 0, &first);
        (void)std::fprintf(stderr,
                           "chromium-patch: fallback: order lang=%s han=%s, %d of %d "
                           "moved, now %s\n",
                           locale != nullptr ? locale : "(none)", HanName(han), front,
                           set->nfont,
                           first != nullptr ? reinterpret_cast<const char*>(first)
                                            : "(unnamed)");
    }
}

const void* CharSetOfFamily(const char* family)
{
    for (unsigned i = 0; i < g_known_count; ++i) {
        if (strcasecmp(g_known[i].family, family) == 0) {
            return g_known[i].charset;
        }
    }
    return nullptr;
}

}  // namespace

extern "C" __attribute__((visibility("default")))
int FcCharSetHasChar(const void* charset, unsigned codepoint)
{
    using HasCharFn = int (*)(const void*, unsigned);
    static const auto real = Sym<HasCharFn>("FcCharSetHasChar");
    if (real == nullptr) {
        return 0;
    }
    const int answer = real(charset, codepoint);
    // Bopomofo. Windows answers Microsoft JhengHei, and the walk lands on
    // Microsoft JhengHei UI instead. The two are separate faces of msjh.ttc
    // with disjoint family lists, but fontconfig interns one charset per
    // coverage and they have the same coverage, so both entries reach this
    // hook with the same pointer and FamilyOf can only report the name that
    // was registered first. Occurrence order is what is left: the UI face
    // precedes its sibling in the sort, so declining the first occurrence of
    // each charset walks on to the other face. Only these ranges are gated,
    // and they are wrong today, so a sort that ordered the pair the other way
    // would leave the cell as it already is rather than break a right one.
    if (answer != 0 && chromium_patch::ParityWanted() &&
        ((codepoint >= 0x3105 && codepoint <= 0x312F) ||
         (codepoint >= 0x31A0 && codepoint <= 0x31BF)) &&
        HasFamily(charset, "Microsoft JhengHei") &&
        HasFamily(charset, "Microsoft JhengHei UI")) {
        thread_local unsigned seen_cp = 0;
        thread_local const void* seen[8] = {};
        thread_local unsigned seen_count = 0;
        if (seen_cp != codepoint) {
            seen_cp = codepoint;
            seen_count = 0;
        }
        bool first = true;
        for (unsigned i = 0; i < seen_count; ++i) {
            if (seen[i] == charset) {
                first = false;
                break;
            }
        }
        if (first) {
            if (seen_count < 8) {
                seen[seen_count++] = charset;
            }
            return 0;
        }
    }
    if (!chromium_patch::ParityWanted() || answer == 0) {
        return answer;          // a miss stays a miss
    }
    if (FamilyOf(charset) == nullptr) {
        return answer;          // not a font from a fallback sort
    }
    const FamilyList script = ScriptFor(codepoint);
    if (script.by_set_order) {
        // OrderForHan puts every by-set-order row's DirectWrite family in the
        // set, and one of them can cover another row's range and answer for it
        // from further forward. Each range keeps only its own, which the run's
        // Han family still outranks, since that is a different family from the
        // UI variant named here.
        bool mine = false;
        for (const DWriteFallback& row : kDWriteFallback) {
            if (codepoint >= row.first && codepoint <= row.last) {
                mine = true;
                break;
            }
        }
        if (mine) {
            const char* family = FamilyOf(charset);
            for (const DWriteFallback& row : kDWriteFallback) {
                const bool is_mine = codepoint >= row.first && codepoint <= row.last;
                if (row.front == nullptr || row.family == nullptr || is_mine) {
                    continue;   // its own row's family is the one that may answer
                }
                if (family != nullptr && strcasecmp(family, row.family) == 0) {
                    return 0;
                }
            }
        }
        return answer;          // the sort already put the right family first
    }
    // ScriptToFontMap::FirstAvailableFont takes the first installed
    // candidate without asking whether it covers anything, and memoizes it for
    // the script. A character the chosen family lacks therefore falls to the
    // last-resort walk below instead of to the next candidate.
    for (unsigned i = 0; i < script.count; ++i) {
        if (!ShipsWithWindows(script.families[i])) {
            continue;
        }
        const void* candidate = CharSetOfFamily(script.families[i]);
        if (candidate == nullptr) {
            continue;           // not installed, so not what Windows picked
        }
        if (real(candidate, codepoint) == 0) {
            break;              // installed but does not cover it
        }
        LogPick(codepoint, "script", script.families[i]);
        return HasFamily(charset, script.families[i]) ? 1 : 0;
    }
    // No script font covers it, so Windows walks its last-resort list, which is
    // also how characters with no script row are answered.
    // GetFallbackFamilyNameFromHardcodedChoices picks the list from the script
    // GetFallbackFamily reported, and everything reaching here has a script
    // other than Han, which is the common list. The CJK list belongs to
    // unsettled Han and is walked by OrderForHan instead.
    constexpr unsigned last_count = DWC_COUNT(kCommonWalkOrder);
    const char* const* last = kCommonWalkOrder;
    for (unsigned i = 0; i < last_count; ++i) {
        const char* name = last[i];
        if (!ShipsWithWindows(name)) {
            continue;
        }
        if (const void* candidate = CharSetOfFamily(name);
            candidate == nullptr || real(candidate, codepoint) == 0) {
            continue;
        }
        LogPick(codepoint, "last", name);
        return HasFamily(charset, name) ? 1 : 0;
    }
    // Nothing hardcoded covers it, which is where Windows falls through to
    // IDWriteFontFallback. The rows below are what it answers.
    for (const DWriteFallback& row : kDWriteFallback) {
        if (codepoint < row.first || codepoint > row.last || row.family == nullptr) {
            continue;           // nothing to answer past the list
        }
        if (!ShipsWithWindows(row.family)) {
            break;
        }
        const void* candidate = CharSetOfFamily(row.family);
        if (candidate == nullptr || real(candidate, codepoint) == 0) {
            break;              // not installed here, or does not cover it
        }
        LogPick(codepoint, "dwrite", row.family);
        return HasFamily(charset, row.family) ? 1 : 0;
    }
    // No row for it, so fontconfig keeps the answer.
    LogPick(codepoint, "none", "(fontconfig)");
    return answer;              // none of them, so leave fontconfig's answer
}

namespace fallback_order {

// The script table, for a caller that has to state the order up front.
// The table's own rows, which are the header's type already.
const ScriptRow* Scripts(unsigned* count)
{
    if (count != nullptr) {
        *count = static_cast<unsigned>(sizeof(kScripts) / sizeof(kScripts[0]));
    }
    return kScripts;
}

// The Han candidates one language names, for a caller that has to state the
// order up front and cannot watch the sorts.
const char* const* HanCandidates(const char* locale, unsigned* count)
{
    unsigned n = 0;
    const char* const* families = HanForSort(locale, &n);
    if (count != nullptr) {
        *count = families != nullptr ? n : 0;
    }
    return families;
}

const char* const* FamiliesFor(const unsigned codepoint, unsigned* count)
{
    const FamilyList list = ScriptFor(codepoint);
    if (count != nullptr) {
        *count = list.by_set_order ? 0 : list.count;
    }
    return list.by_set_order ? nullptr : list.families;
}

bool SetOrderAnswers(const unsigned codepoint)
{
    return ScriptFor(codepoint).by_set_order;
}

int DWriteRowFor(const unsigned codepoint)
{
    for (unsigned i = 0; i < DWC_COUNT(kDWriteFallback); ++i) {
        if (codepoint >= kDWriteFallback[i].first && codepoint <= kDWriteFallback[i].last) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const char* DWriteRowFamily(const int row)
{
    if (row < 0 || static_cast<unsigned>(row) >= DWC_COUNT(kDWriteFallback)) {
        return nullptr;
    }
    return kDWriteFallback[row].family;
}

unsigned DWriteRowCount() { return DWC_COUNT(kDWriteFallback); }

bool DWriteRowAhead(const int row)
{
    if (row < 0 || static_cast<unsigned>(row) >= DWC_COUNT(kDWriteFallback)) {
        return false;
    }
    return kDWriteFallback[row].ahead;
}

const char* DWriteRowFront(const int row)
{
    if (row < 0 || static_cast<unsigned>(row) >= DWC_COUNT(kDWriteFallback)) {
        return nullptr;
    }
    return kDWriteFallback[row].front;
}

unsigned DWriteRowFirst(const int row)
{
    if (row < 0 || static_cast<unsigned>(row) >= DWC_COUNT(kDWriteFallback)) {
        return 0;
    }
    return kDWriteFallback[row].first;
}

// The family the sort puts in front for a by-set-order range, or null when the
// run's language decides it. FcFontSort is what does the fronting, so a build
// with no sort to watch has to name this family itself.
const char* SetOrderFront(const unsigned codepoint)
{
    for (const ScriptFonts& s : kScripts) {
        if (codepoint < s.first || codepoint > s.last) {
            continue;
        }
        return s.families == kTimesNewRoman ? kTimesNewRoman[0] : nullptr;
    }
    return nullptr;
}

// The pan-Unicode list, for a caller that has to state the order up front.
const char* const* PanUnicode(const bool cjk, unsigned* count)
{
    if (count != nullptr) {
        *count = cjk ? DWC_COUNT(kCjkLastResort) : DWC_COUNT(kCommonWalkOrder);
    }
    return cjk ? kCjkLastResort : kCommonWalkOrder;
}

// Called from cleartype/src/fontconfig.cpp's FcFontSort, on the set the real
// one returned. Both halves of this library wanted that symbol and only one
// can define it, so the Firefox side keeps the interposer and hands the
// result here.
void NoteFontSet(const void* pattern, void* sorted)
{
    auto* set = static_cast<FcFontSet*>(sorted);
    if (!chromium_patch::ParityWanted() || set == nullptr || set->nfont <= 1 ||
        pattern == nullptr) {
        return;
    }

    static const auto get_charset = Sym<PatternGetCharSetFn>("FcPatternGetCharSet");
    static const auto get_string = Sym<PatternGetStringFn>("FcPatternGetString");
    if (get_charset == nullptr || get_string == nullptr) {
        return;
    }

    for (int i = 0; i < set->nfont; ++i) {
        void* charset = nullptr;
        if (get_charset(set->fonts[i], "charset", 0, &charset) != kFcResultMatch) {
            continue;
        }
        // A font answers to every name in its family list, and the Windows CJK
        // families lead with the UI variant. Learning only the first files
        // Microsoft YaHei under Microsoft YaHei UI, and the table, which asks
        // for the name Chromium uses, never finds it.
        for (int n = 0;; ++n) {
            unsigned char* family = nullptr;
            if (get_string(set->fonts[i], "family", n, &family) != kFcResultMatch) {
                break;
            }
            Remember(charset, reinterpret_cast<const char*>(family));
        }
    }

    OrderForHan(pattern, set);
}

}  // namespace fallback_order
