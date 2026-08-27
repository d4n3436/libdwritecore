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
//  FcFontSort is also exported by libcleartype.so, which substitutes a Windows
//  pattern there rather than watching, so preloading both libraries into one
//  process is unsupported. Whichever LD_PRELOAD names first takes the symbol
//  and the other's behavior is gone with no diagnostic.
//
//----------------------------------------------------------------------------

#include <cstdio>
#include <cstring>
#include <strings.h>

#include <dlfcn.h>

#include "parity_gate.h"
#include "windows_fonts.h"

namespace {

// fontconfig, without its headers.
constexpr int kFcResultMatch = 0;
constexpr int kFcCharSetMapSize = 8;

struct FcFontSet
{
    int nfont;
    int sfont;
    void** fonts;
};

using FontSortFn = FcFontSet* (*)(void*, void*, int, void**, int*);
using PatternGetCharSetFn = int (*)(const void*, const char*, int, void**);
using PatternGetStringFn = int (*)(const void*, const char*, int, unsigned char**);
using CharSetFirstPageFn = unsigned (*)(const void*, unsigned*, unsigned*);

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

// The lists from font_fallback_win.cc, verbatim and in their order. Only the
// scripts a page can actually reach are here; the historic and symbol ones
// all resolve to Segoe UI variants that fontconfig picks anyway.
const char* const kKatakanaOrHiragana[] = {"Noto Sans JP", "Noto Sans CJK JP", "Meiryo",
                                           "Yu Gothic", "MS PGothic", "Microsoft YaHei"};
const char* const kSimplifiedHan[] = {"Noto Sans SC", "Noto Sans CJK SC", "Microsoft YaHei",
                                      "simsun"};
const char* const kHangul[] = {"Noto Sans KR", "Noto Sans CJK KR", "Malgun Gothic", "Gulim"};
const char* const kArabic[] = {"Tahoma", "Segoe UI"};
const char* const kHebrew[] = {"David", "Segoe UI"};
const char* const kArmenian[] = {"Segoe UI", "Sylfaen"};
const char* const kGeorgian[] = {"Sylfaen", "Segoe UI"};
const char* const kDevanagari[] = {"Nirmala UI", "Mangal"};
const char* const kBengali[] = {"Nirmala UI", "Vrinda"};
const char* const kGurmukhi[] = {"Nirmala UI", "Raavi"};
const char* const kGujarati[] = {"Nirmala UI", "Shruti"};
const char* const kOriya[] = {"Kalinga", "ori1Uni", "Lohit Oriya", "Nirmala UI"};
const char* const kTamil[] = {"Nirmala UI", "Latha"};
const char* const kTelugu[] = {"Nirmala UI", "Gautami"};
const char* const kKannada[] = {"Tunga", "Nirmala UI"};
const char* const kMalayalam[] = {"Nirmala UI", "Kartika"};
const char* const kSinhala[] = {"Iskoola Pota", "AksharUnicode", "Nirmala UI"};
const char* const kThai[] = {"Tahoma", "Leelawadee UI", "Leelawadee"};
const char* const kLao[] = {"Leelawadee UI", "Lao UI"};
const char* const kKhmer[] = {"Leelawadee UI", "Khmer UI", "Khmer OS", "MoolBoran", "DaunPenh"};

// Where each script lives. USCRIPT_HAN resolves by locale on Windows; the
// simplified list is used, which is what a system whose UI locale is neither
// Japanese nor Korean resolves to.
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
    DWC_SCRIPT(0x1100, 0x11FF, kHangul),        // Hangul Jamo
    DWC_SCRIPT(0xAC00, 0xD7AF, kHangul),        // Hangul syllables
    DWC_SCRIPT(0x3040, 0x309F, kKatakanaOrHiragana),
    DWC_SCRIPT(0x30A0, 0x30FF, kKatakanaOrHiragana),
    DWC_SCRIPT(0x31F0, 0x31FF, kKatakanaOrHiragana),
    DWC_SCRIPT(0x3400, 0x4DBF, kSimplifiedHan),
    DWC_SCRIPT(0x4E00, 0x9FFF, kSimplifiedHan),
    DWC_SCRIPT(0xF900, 0xFAFF, kSimplifiedHan),
};

#undef DWC_SCRIPT

const ScriptFonts* ScriptFor(const unsigned codepoint)
{
    for (const ScriptFonts& s : kScripts) {
        if (codepoint >= s.first && codepoint <= s.last) {
            return &s;
        }
    }
    return nullptr;
}

// The character the fallback is being asked about. font_fallback_linux.cc
// puts exactly one into the pattern's charset.
bool FirstCodepoint(const void* pattern, unsigned* out)
{
    static const auto get_charset = Sym<PatternGetCharSetFn>("FcPatternGetCharSet");
    static const auto first_page = Sym<CharSetFirstPageFn>("FcCharSetFirstPage");
    if (get_charset == nullptr || first_page == nullptr) {
        return false;
    }
    void* charset = nullptr;
    if (get_charset(pattern, "charset", 0, &charset) != kFcResultMatch || charset == nullptr) {
        return false;
    }
    unsigned map[kFcCharSetMapSize] = {};
    unsigned next = 0;
    const unsigned base = first_page(charset, map, &next);
    if (base == static_cast<unsigned>(-1)) {
        return false;
    }
    for (int i = 0; i < kFcCharSetMapSize; ++i) {
        if (map[i] == 0) {
            continue;
        }
        for (int bit = 0; bit < 32; ++bit) {
            if ((map[i] & (1u << bit)) != 0) {
                *out = base + static_cast<unsigned>(i) * 32 + static_cast<unsigned>(bit);
                return true;
            }
        }
    }
    return false;
}

bool FamilyIs(const void* candidate, const char* want)
{
    static const auto get_string = Sym<PatternGetStringFn>("FcPatternGetString");
    if (get_string == nullptr) {
        return false;
    }
    unsigned char* name = nullptr;
    if (get_string(candidate, "family", 0, &name) != kFcResultMatch || name == nullptr) {
        return false;
    }
    return strcasecmp(reinterpret_cast<const char*>(name), want) == 0;
}

// Which families the last sorts turned up, and the charset of each. Small and
// bounded, since a fallback set is a few hundred fonts at most and only the
// families named in the table above are ever looked up.
struct Known
{
    const void* charset;
    char family[64];
};
constexpr unsigned kMaxKnown = 512;
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
    std::snprintf(g_known[g_known_count].family, sizeof(g_known[0].family), "%s", family);
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
    const ScriptFonts* script = ScriptFor(codepoint);
    if (script == nullptr) {
        return answer;
    }
    const char* mine = FamilyOf(charset);
    if (mine == nullptr) {
        return answer;          // not a font from a fallback sort
    }
    // The first candidate that exists and actually covers this character.
    for (unsigned i = 0; i < script->count; ++i) {
        if (!ShipsWithWindows(script->families[i])) {
            continue;
        }
        const void* candidate = CharSetOfFamily(script->families[i]);
        if (candidate == nullptr || real(candidate, codepoint) == 0) {
            continue;
        }
        return strcasecmp(mine, script->families[i]) == 0 ? 1 : 0;
    }
    return answer;              // none of them, so leave fontconfig's answer
}

extern "C" __attribute__((visibility("default")))
FcFontSet* FcFontSort(void* config, void* pattern, int trim, void** csp, int* result)
{
    static const auto real = Sym<FontSortFn>("FcFontSort");
    if (real == nullptr) {
        return nullptr;
    }
    FcFontSet* set = real(config, pattern, trim, csp, result);
    if (!chromium_patch::ParityWanted() || set == nullptr || set->nfont <= 1 || pattern == nullptr) {
        return set;
    }

    // Only learning here. The choice is made per character, above.
    static const auto get_charset = Sym<PatternGetCharSetFn>("FcPatternGetCharSet");
    static const auto get_string = Sym<PatternGetStringFn>("FcPatternGetString");
    if (get_charset == nullptr || get_string == nullptr) {
        return set;
    }
    for (int i = 0; i < set->nfont; ++i) {
        void* charset = nullptr;
        unsigned char* family = nullptr;
        if (get_charset(set->fonts[i], "charset", 0, &charset) == kFcResultMatch &&
            get_string(set->fonts[i], "family", 0, &family) == kFcResultMatch) {
            Remember(charset, reinterpret_cast<const char*>(family));
        }
    }
    return set;
}
