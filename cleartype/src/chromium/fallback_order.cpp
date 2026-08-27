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

// The lists from font_fallback_win.cc, verbatim and in their order. Only the
// scripts a page can actually reach are here; the historic and symbol ones
// all resolve to Segoe UI variants that fontconfig picks anyway.
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
    DWC_SCRIPT(0x1100, 0x11FF, kHangul),        // Hangul Jamo
    DWC_SCRIPT(0xAC00, 0xD7AF, kHangul),        // Hangul syllables
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
// LayoutLocale::GetSystem().GetScriptForHan(), which font_fallback_win.cc
// calls when it fills USCRIPT_HAN.
//
// layout_locale.cc takes the system locale from icu::Locale::getDefault() and
// ComputeScriptForHan asks locale_to_script_mapping.cc's
// ScriptCodeForHanFromSubtags, which walks the subtags and takes the first
// that disambiguates: a two-letter region, or a four-letter script name.
// Nothing conclusive leaves it at simplified Han.
//
// The environment is read as ICU reads it, LC_ALL before LC_CTYPE before LANG.
struct HanChoice
{
    const char* subtag;
    const char* const* families;
    unsigned count;
};

const char* const* HanFamilies(unsigned* count)
{
    static const char* const* chosen = nullptr;
    static unsigned chosen_count = 0;
    if (chosen != nullptr) {
        *count = chosen_count;
        return chosen;
    }

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

    const char* env = std::getenv("LC_ALL");
    if (env == nullptr || env[0] == '\0') {
        env = std::getenv("LC_CTYPE");
    }
    if (env == nullptr || env[0] == '\0') {
        env = std::getenv("LANG");
    }

    chosen = kSimplifiedHan;
    chosen_count = DWC_COUNT(kSimplifiedHan);
    if (env != nullptr) {
        // The language subtag itself decides only when it is already an
        // unambiguous Han script: ja and ko are, zh is not.
        char head[8] = {};
        unsigned n = 0;
        while (n + 1 < sizeof(head) && env[n] != '\0' && env[n] != '-' &&
               env[n] != '_' && env[n] != '.' && env[n] != '@') {
            head[n] = static_cast<char>(tolower(static_cast<unsigned char>(env[n])));
            ++n;
        }
        if (std::strcmp(head, "ja") == 0) {
            chosen = kKatakanaOrHiragana;
            chosen_count = DWC_COUNT(kKatakanaOrHiragana);
        } else if (std::strcmp(head, "ko") == 0) {
            chosen = kHangul;
            chosen_count = DWC_COUNT(kHangul);
        } else {
            // Then the subtags after it, first one that disambiguates.
            for (const char* p = env; *p != '\0' && *p != '.' && *p != '@';) {
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
                bool done = false;
                for (const HanChoice& c : kChoices) {
                    if (std::strcmp(sub, c.subtag) == 0) {
                        chosen = c.families;
                        chosen_count = c.count;
                        done = true;
                        break;
                    }
                }
                if (done) {
                    break;
                }
                p += k;
            }
        }
    }
    *count = chosen_count;
    return chosen;
}

const ScriptFonts* ScriptFor(const unsigned codepoint)
{
    for (const ScriptFonts& s : kScripts) {
        if (codepoint < s.first || codepoint > s.last) {
            continue;
        }
        if (s.families != kSimplifiedHan) {
            return &s;
        }
        static ScriptFonts han{};
        han = s;
        han.families = HanFamilies(&han.count);
        return &han;
    }
    return nullptr;
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
        if (const void* candidate = CharSetOfFamily(script->families[i]);
            candidate == nullptr || real(candidate, codepoint) == 0) {
            continue;
        }
        return strcasecmp(mine, script->families[i]) == 0 ? 1 : 0;
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
