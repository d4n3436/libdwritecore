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
// fontconfig and the table reach the same family for them.
struct ScriptFonts
{
    unsigned first;
    unsigned last;
    const char* const* families;
    unsigned count;
};

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
constexpr const char* kSpecials[] = {"Tahoma"};
constexpr const char* kSegoeHistoric[] = {"Segoe UI Historic"};
constexpr const char* kSegoeHistoricOrSymbol[] = {"Segoe UI Historic", "Segoe UI Symbol"};
constexpr const char* kSyriac[] = {"Estrangelo Edessa", "Estrangelo Nisibin", "Code2000"};
constexpr const char* kThaana[] = {"MV Boli"};

// Where each script lives. The unified Han block is filled in by HanFamilies()
// below, since Windows picks its list from the locale rather than fixing one.
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
    DWC_SCRIPT(0x2190, 0x219F, kArrowsSimple),
    DWC_SCRIPT(0x21A0, 0x21FF, kArrows),
    DWC_SCRIPT(0x2200, 0x22FF, kMath),                     // Mathematical operators
    DWC_SCRIPT(0x2300, 0x23FF, kMath),                     // Miscellaneous technical
    DWC_SCRIPT(0x2440, 0x245F, kSegoeSymbol),              // Optical character recognition
    DWC_SCRIPT(0x2580, 0x259F, kBlockElements),
    DWC_SCRIPT(0xFFF0, 0xFFFF, kSpecials),
    DWC_SCRIPT(0x1100, 0x11FF, kHangul),        // Hangul Jamo
    DWC_SCRIPT(0xAC00, 0xD7AF, kHangul),        // Hangul syllables
    DWC_SCRIPT(0x3100, 0x312F, kTraditionalHan),   // Bopomofo
    DWC_SCRIPT(0x3040, 0x309F, kKatakanaOrHiragana),
    DWC_SCRIPT(0x30A0, 0x30FF, kKatakanaOrHiragana),
    DWC_SCRIPT(0x31F0, 0x31FF, kKatakanaOrHiragana),
    // Unified Han, whose list is chosen from the locale. kSimplifiedHan is
    // named here only so the entry has a shape; HanFamilies() answers.
    DWC_SCRIPT(0x3400, 0x4DBF, kSimplifiedHan),
    DWC_SCRIPT(0x4E00, 0x9FFF, kSimplifiedHan),
    DWC_SCRIPT(0xF900, 0xFAFF, kSimplifiedHan),
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
    // ScriptCodeForHanFromRegion, plus the four-letter script names
    // IsUnambiguousHanScript accepts.
    static const HanChoice kChoices[] = {
        {"hk", kTraditionalHan, DWC_COUNT(kTraditionalHan)},
        {"mo", kTraditionalHan, DWC_COUNT(kTraditionalHan)},
        {"tw", kTraditionalHan, DWC_COUNT(kTraditionalHan)},
        {"jp", kKatakanaOrHiragana, DWC_COUNT(kKatakanaOrHiragana)},
        {"kr", kHangul, DWC_COUNT(kHangul)},
        {"hant", kTraditionalHan, DWC_COUNT(kTraditionalHan)},
        {"hans", kSimplifiedHan, DWC_COUNT(kSimplifiedHan)},
        {"jpan", kKatakanaOrHiragana, DWC_COUNT(kKatakanaOrHiragana)},
        {"kore", kHangul, DWC_COUNT(kHangul)},
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
const char* const* HanFamilies(unsigned* count)
{
    if (const char* const* families = SystemHan(count)) {
        return families;
    }
    *count = DWC_COUNT(kSimplifiedHan);
    return kSimplifiedHan;
}

struct FamilyList
{
    const char* const* families;
    unsigned count;
};

FamilyList ScriptFor(const unsigned codepoint)
{
    for (const ScriptFonts& s : kScripts) {
        if (codepoint < s.first || codepoint > s.last) {
            continue;
        }
        if (s.families != kSimplifiedHan) {
            return {s.families, s.count};
        }
        unsigned count = 0;
        const char* const* families = HanFamilies(&count);
        return {families, count};
    }
    return {nullptr, 0};
}

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
        if (g_known[i].charset == charset) {
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

const char* FamilyOf(const void* charset)
{
    for (unsigned i = 0; i < g_known_count; ++i) {
        if (g_known[i].charset == charset) {
            return g_known[i].family;
        }
    }
    return nullptr;
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
    if (!chromium_patch::ParityWanted() || answer == 0) {
        return answer;          // a miss stays a miss
    }
    const FamilyList script = ScriptFor(codepoint);
    if (script.families == nullptr) {
        return answer;
    }
    const char* mine = FamilyOf(charset);
    if (mine == nullptr) {
        return answer;          // not a font from a fallback sort
    }
    // The first candidate that exists and actually covers this character.
    for (unsigned i = 0; i < script.count; ++i) {
        if (!ShipsWithWindows(script.families[i])) {
            continue;
        }
        if (const void* candidate = CharSetOfFamily(script.families[i]);
            candidate == nullptr || real(candidate, codepoint) == 0) {
            continue;
        }
        return strcasecmp(mine, script.families[i]) == 0 ? 1 : 0;
    }
    return answer;              // none of them, so leave fontconfig's answer
}

namespace fallback_order {

// Called from cleartype/src/fontconfig.cpp's FcFontSort, on the set the real
// one returned. Both halves of this library wanted that symbol and only one
// can define it, so the Firefox side keeps the interposer and hands the
// result here.
void NoteFontSet(const void* pattern, void* sorted)
{
    const auto* set = static_cast<const FcFontSet*>(sorted);
    if (!chromium_patch::ParityWanted() || set == nullptr || set->nfont <= 1 ||
        pattern == nullptr) {
        return;
    }

    // Only learning here. The choice is made per character, above.
    static const auto get_charset = Sym<PatternGetCharSetFn>("FcPatternGetCharSet");
    static const auto get_string = Sym<PatternGetStringFn>("FcPatternGetString");
    if (get_charset == nullptr || get_string == nullptr) {
        return;
    }
    for (int i = 0; i < set->nfont; ++i) {
        void* charset = nullptr;
        unsigned char* family = nullptr;
        if (get_charset(set->fonts[i], "charset", 0, &charset) == kFcResultMatch &&
            get_string(set->fonts[i], "family", 0, &family) == kFcResultMatch) {
            Remember(charset, reinterpret_cast<const char*>(family));
        }
    }
}

}  // namespace fallback_order
