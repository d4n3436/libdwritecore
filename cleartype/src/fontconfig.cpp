//+--------------------------------------------------------------------------
//
//  fontconfig.cpp - answering the rendering-settings queries directly.
//
//  Firefox on Linux asks fontconfig how to rasterize: whether to antialias,
//  whether to hint and how, the subpixel order, the LCD filter. Firefox on
//  Windows asks nobody - DirectWrite decides, and there is no fontconfig on the
//  machine to consult. So the Linux questions are given the answers that
//  describe what DirectWrite is about to do:
//
//    rgba = rgb, antialias = true
//      gfx/2d/ScaledFontFontconfig.cpp InstanceData(FcPattern*) makes
//      mAntialias SUBPIXEL from FC_RGBA rgb, which GetWRFontInstanceOptions
//      sends as FontRenderMode::Subpixel - the mode gfx/2d/ScaledFontDWrite.cpp
//      GetDefaultAAMode returns for a ClearType system.
//
//    hinting = false, hintstyle = hintnone
//      gfx/thebes/gfxFcPlatformFontList.cpp PrepareFontOptions then loads with
//      FT_LOAD_NO_HINTING, so gfxFT2FontBase::ShouldRoundXOffset is false and
//      GetFTGlyphExtents takes linearHoriAdvance - the unrounded design advance
//      gfxDWriteFont::MeasureGlyphWidth scales on Windows;
//      ScaledFontFontconfig::UseSubpixelPosition is true, as
//      ScaledFontDWrite::UseSubpixelPosition is; and FcPatternAllowsBitmaps
//      refuses embedded bitmaps for an outline font, leaving the
//      EMBEDDED_BITMAPS flag to this library's gfxDWriteFont::GetScaledFont.
//
//    lcdfilter = lcddefault
//      Only reaches glyphs the shim declines and FreeType renders itself.
//
//  Answering the queries here needs no configuration file to be pointed at,
//  and cannot be overridden by whatever the distribution ships.
//
//  fontconfig is a plain shared library and libxul imports 45 of its symbols,
//  so this is the same kind of interception as the FreeType side and fails the
//  same way: anything not recognized goes straight to the real fontconfig.
//
//  The first three answers are compiled in unconditionally. The two that turn
//  hinting off are a statement about Firefox's advance arithmetic and not
//  about rasterization, so they need a CLEARTYPE_FIREFOX_PARITY build, as do
//  the matching questions in the FcFontSort block below. Whether any of them
//  is answered at all is decided by Answering, below.
//
//  The Chromium half of this library needs the same FcFontSort, to learn which
//  family carries which charset. It only watches, so this one calls into
//  src/chromium/fallback_order.cpp with the sorted set.
//----------------------------------------------------------------------------

#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <algorithm>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <pthread.h>

#include "chromium/fallback_order.h"
#include "chromium/family_match.h"
#include "chromium/parity_gate.h"
#if CLEARTYPE_FIREFOX_PARITY
#include "firefox_parity_data.h"
#endif
#include "parity_mode.h"
#include "shim_exports.h"
#include "windows_fonts.h"

// Nothing declares the entry points below but their definitions. They replace
// fontconfig's, whose real declarations are in a header this file does not
// include - see the ABI block underneath.
// gcc and clang spell the same diagnostic differently.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#else
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#endif

namespace {

// fontconfig's ABI, declared here instead of included so this library keeps
// its build dependencies. Names and values from fontconfig/fontconfig.h:
// FC_RGBA_RGB 1, FC_HINT_SLIGHT 1, FC_LCD_DEFAULT 1, FcResultMatch 0.
using FcPattern = struct _FcPattern;
using FcBool = int;
using FcResult = int;

constexpr FcResult kFcResultMatch = 0;

constexpr FcResult kFcResultNoMatch = 1;

// fontconfig.h: FcTypeInteger 1, FcTypeBool 4; FcValue is the tag then a union
// whose widest member is a double, so the union starts at offset 8.
constexpr int kFcTypeInteger = 1;
constexpr int kFcTypeBool = 4;

struct FcValue
{
    int type;
    // Every member is spelled out because this mirrors fontconfig's FcValue:
    // the union's size and alignment are what the layout depends on, not which
    // arms this file happens to read.
    // ReSharper disable CppDeclaratorNeverUsed
    union {
        const void* s;
        int i;
        int b;
        double d;
    } u;
    // ReSharper restore CppDeclaratorNeverUsed
};

constexpr int kFcRgbaRgb = 1;
constexpr int kFcHintSlight = 1;
constexpr int kFcLcdDefault = 1;
// FC_WEIGHT_LIGHT, FC_WEIGHT_DEMILIGHT and FC_WEIGHT_REGULAR.
constexpr int kFcWeightLight = 50;
constexpr int kFcWeightDemilight = 55;
constexpr int kFcWeightRegular = 80;
constexpr int kFcWeightBold = 200;

using FcPatternGetIntegerFn = FcResult (*)(const FcPattern*, const char*, int, int*);
using FcPatternGetBoolFn = FcResult (*)(const FcPattern*, const char*, int, FcBool*);
using FcPatternGetFn = FcResult (*)(const FcPattern*, const char*, int, FcValue*);

// The real fontconfig entry point behind ours.
//
// RTLD_NEXT alone is not enough, and a host that gets this wrong crashes
// outright. RTLD_NEXT searches the objects that follow this one
// in the *global* scope, and a library brought in by dlopen without
// RTLD_GLOBAL - the default - never joins it. JetBrains Rider is such a host:
// its JVM dlopens the GTK stack for the Swing look-and-feel, so libfontconfig
// is loaded and reachable while dlsym(RTLD_NEXT, "FcPatternGet") answers
// null. Interposing then means intercepting a call with nothing to forward
// it to, and returning FcResultNoMatch for a property the caller just set
// leaves it reading an FcValue nobody filled in - a segfault inside
// libpangoft2, not a missing setting.
//
// So the object is asked for directly when the scope does not have it.
// RTLD_NOLOAD first, which returns a handle only if it is already mapped and
// is therefore the same fontconfig the caller is using; a plain dlopen after
// that, for a host that has not loaded it yet.
// One handle for the whole file, and a failed open is retried rather than
// remembered: the library can arrive after the first ask, and a cached null
// would leave every later call with nothing to forward to.
void* FontconfigLibrary()
{
    static std::atomic<void*> library{nullptr};
    void* handle = library.load(std::memory_order_acquire);
    if (handle == nullptr) {
        handle = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
        if (handle == nullptr) {
            handle = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
        }
        if (handle != nullptr) {
            library.store(handle, std::memory_order_release);
        }
    }
    return handle;
}

template <typename Fn>
Fn Next(const char* name)
{
    if (void* fn = dlsym(RTLD_NEXT, name)) {
        return reinterpret_cast<Fn>(fn);
    }

    void* handle = FontconfigLibrary();
    return handle != nullptr ? reinterpret_cast<Fn>(dlsym(handle, name)) : nullptr;
}

bool IsOff(const char* v)
{
    return v != nullptr && (std::strcmp(v, "0") == 0 || strcasecmp(v, "off") == 0 ||
                            strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0);
}

// Whether this library answers these questions or passes them on.
//
// Answering is right for a parity build. The questions are about what the
// DirectWrite stack decides, and the host's own configuration is not the
// subject. It is wrong outside one, because there they are the user's settings
// - and "rgba" in particular is a statement about their panel.
//
// Answering rgb on a BGR panel does not change what this library rasterizes.
// An LCD bitmap carries no panel order; the caller reverses each triple when it
// reads one, as skia's copyFT2LCD16 does for lcdIsBGR, so the same coverage
// serves both orders. What it changes is whether the caller reverses at all,
// which lands the fringes on the wrong side of every stem.
//
// Hence the default differs by build, and CLEARTYPE_FONTCONFIG overrides
// it either way. The consequence of not answering is that subpixel rendering
// has to be on in the user's own fontconfig for FT_RENDER_MODE_LCD to be asked
// for at all, which is the ordinary way to turn it on and where the decision
// belongs.
bool Answering()
{
    // Decided once. A caller reads these settings and keeps them - Gecko
    // caches its render params per font, cairo per scaled font - so an answer
    // that changed partway through the process would leave two callers
    // disagreeing about the same font. That is also why ParityActive's
    // install-layout signal matters here: the first of these queries can
    // arrive before libxul is mapped.
    static const bool answering = [] {
        // CLEARTYPE=0 means this library does nothing, and that has to include
        // here: with the rasterization switched off, telling a caller the text
        // is subpixel-rendered when FreeType is about to render it its own way
        // is worse than saying nothing.
        if (!dwcft::Enabled()) {
            return false;
        }

        const char* v = std::getenv("CLEARTYPE_FONTCONFIG");
        if (v == nullptr) {
            return dwcft::ParityActive();
        }
        return !IsOff(v);
    }();
    return answering;
}

// Only the first value of a property is answered. fontconfig properties are
// lists, and a caller asking for element 1 of "rgba" is asking something this
// has no opinion about.
bool Answers(const char* object, const int n, const char* name)
{
    return n == 0 && object != nullptr && std::strcmp(object, name) == 0;
}

// The two settings tables, in one place: three accessors reach them.
// Gecko reads these with FcPatternGetBool and FcPatternGetInteger, and cairo
// reads the same properties off the same patterns with the generic
// FcPatternGet, so all three have to answer alike or the rasterizer and the
// layout disagree about the same font.
bool IntegerAnswer(const char* object, const int n, int* out)
{
    if (Answers(object, n, "rgba")) {
        *out = kFcRgbaRgb;
        return true;
    }
    if (Answers(object, n, "lcdfilter")) {
        *out = kFcLcdDefault;
        return true;
    }
#if CLEARTYPE_FIREFOX_PARITY
    // Only for Firefox. hintslight and hintnone both leave
    // gfxFT2FontBase::ShouldRoundXOffset false, so both read the unrounded
    // advance Windows measures with. They part over the glyph's ink bounds.
    // GetFTGlyphExtents floors the top and ceils the bottom to whole pixels
    // when the load flags carry FT_LOAD_NO_HINTING, and hintnone is the only
    // way to ask for that flag. DirectWrite scales the design bounds and
    // rounds nothing, so hintslight is the answer that matches. It costs no
    // hinting either, because UnhintedLoadFlags in src/freetype.cpp puts
    // FT_LOAD_NO_HINTING back before FreeType sees the flags.
    if (Answers(object, n, "hintstyle")) {
        *out = kFcHintSlight;
        return true;
    }
#endif
    return false;
}

bool BoolAnswer(const char* object, const int n, int* out)
{
    if (Answers(object, n, "antialias")) {
        *out = 1;
        return true;
    }
#if CLEARTYPE_FIREFOX_PARITY
    // PrepareFontOptions only reads hintstyle when this says yes; a no there
    // pins the style to hintnone whatever the integer answer is.
    if (Answers(object, n, "hinting")) {
        *out = 1;
        return true;
    }
    // With hinting on, FcPatternAllowsBitmaps lets a face's embedded strikes
    // through, which used to be refused for an outline font on the strength of
    // the hinting answer alone. Windows decides strikes per font in
    // gfxDWriteFont::GetScaledFont, so they are refused here as before.
    if (Answers(object, n, "embeddedbitmap")) {
        *out = 0;
        return true;
    }
#endif
    return false;
}


using FcChar8 = unsigned char;
using FcConfig = struct _FcConfig;
using FcFontSet = struct _FcFontSet;

using FcPatternGetStringFn = FcResult (*)(const FcPattern*, const char*, int, FcChar8**);
using FcConfigGetFontsFn = FcFontSet* (*)(FcConfig*, int);

// fontconfig.h: FcSetSystem is 0 and FcSetApplication is 1. Only the system
// set is filtered; the application set is what a caller added itself, and
// Firefox puts its own bundled faces there.
constexpr int kFcSetSystem = 0;

// fontconfig.h: struct _FcFontSet, whose header this file does not include.
// ReSharper disable once CppDeclaratorNeverUsed
// sfont is the set's capacity, which nothing here asks for. It stays because
// fonts sits after it.
struct FontSetLayout
{
    int nfont;
    int sfont;
    FcPattern** fonts;
};

#if CLEARTYPE_FIREFOX_PARITY

// ---------------------------------------------------------------------------
// Matching.
//
// gfx/thebes/gfxFcPlatformFontList.cpp FindGenericFamilies asks fontconfig
// which family to use for a CSS generic *in a language*, and
// GetDefaultFontForPlatform does the same under the name "-moz-default".
// fontconfig answers by coverage, so Tamil text gets a Tamil font and Punjabi
// text a Gurmukhi one. Windows has no such step: the generic resolves through
// font.name-list prefs, and gfx/thebes/gfxDWriteFontList.cpp
// GetDefaultFontForPlatform answers Arial whatever the language is. Whatever
// the requested family then lacks goes to font fallback, which is where the
// per-script Windows lists apply.
//
// So a request for a generic loses its language before fontconfig sees it, and
// the two names Windows resolves to Arial are asked for as Arial. These were
// the two <match target="pattern"> rules of
// cleartype/windows-parity-fontconfig.xml.
//
// The other half is what substitution is allowed to do to a request for a
// family by name. <alias><prefer> - which is how a symbol font ships itself as
// an addition to every monospace family - compiles to a family edit in
// prepend mode, so the aliased font ends up *ahead* of the family the page
// asked for. Gecko takes that list in order, so the first font of the group is
// the alias, and gfxFT2FontBase reads its metrics for the whole group: a line
// box sized by a font of arrows. Windows has no such mechanism at all - a page
// that asks for Consolas gets Consolas - so the family asked for is put back
// in front after substitution. No font is named here; the rule is that the
// request survives.
// ---------------------------------------------------------------------------

using FcCharSet = struct _FcCharSet;

using FcPatternDuplicateFn = FcPattern* (*)(const FcPattern*);
using FcPatternDelFn = FcBool (*)(FcPattern*, const char*);
using FcPatternAddStringFn = FcBool (*)(FcPattern*, const char*, const FcChar8*);
using FcPatternDestroyFn = void (*)(FcPattern*);
using FcFontSortFn = FcFontSet* (*)(FcConfig*, FcPattern*, FcBool, FcCharSet**, FcResult*);
using FcFontMatchFn = FcPattern* (*)(FcConfig*, FcPattern*, FcResult*);
using FcConfigSubstituteFn = FcBool (*)(FcConfig*, FcPattern*, int);
using FcFontSetCreateFn = FcFontSet* (*)();
using FcFontSetAddFn = FcBool (*)(FcFontSet*, FcPattern*);
using FcFontSetDestroyFn = void (*)(FcFontSet*);

// fontconfig.h: FcMatchPattern is 0, FcMatchFont 1, FcMatchScan 2. Only the
// request is edited here. The other two describe a font that has already been
// chosen, where deleting "lang" would delete the languages that font covers.
constexpr int kFcMatchPattern = 0;

// The least a machine can carry and still be filtered. Below this the
// Windows fonts are not really installed and hiding everything else would
// leave the caller with nothing to draw with, so the real set goes back
// untouched.
constexpr int kMinimumWindowsFamilies = 20;

bool ShipsWithWindows(const char* name)
{
    for (const char* known : windows_fonts::kBaseInstall) {
        if (strcasecmp(known, name) == 0) {
            return true;
        }
    }
    return false;
}

// The names gfxFcPlatformFontList hands to fontconfig for a generic: the CSS
// ones GetGenericName returns, plus the fake family GetDefaultFontForPlatform
// uses. Nothing else asks fontconfig a question Windows would not ask.
// Whether the pattern carries Gecko's substitution sentinel. Gecko tells an
// explicit fontconfig substitution from a suggested one by matching
// "<family>, -moz-sentinel" against "-moz-sentinel" alone
// (gfxFcPlatformFontList.cpp, kSentinelName). An unquoted generic never
// reaches that pattern, since FindFamilies resolves it through
// FindGenericFamilies and returns several branches earlier, so a generic
// keyword here is a quoted family name.
bool HasGeckoSentinel(const FcPattern* pattern)
{
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    if (get_string == nullptr || pattern == nullptr) {
        return false;
    }
    for (int i = 0;; ++i) {
        FcChar8* name = nullptr;
        if (get_string(pattern, "family", i, &name) != kFcResultMatch || name == nullptr) {
            return false;
        }
        if (std::strcmp(reinterpret_cast<const char*>(name), "-moz-sentinel") == 0) {
            return true;
        }
    }
}

bool IsGenericRequest(const char* family)
{
    static constexpr const char* kGenerics[] = {
        "serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui",
        "-moz-default",
    };
    for (const char* g : kGenerics) {
        if (std::strcmp(family, g) == 0) {
            return true;
        }
    }
    return false;
}

// The two Windows resolves to Arial rather than to a font of its own.
bool ResolvesToArial(const char* family)
{
    return std::strcmp(family, "-moz-default") == 0 ||
           std::strcmp(family, "fantasy") == 0;
}

// The edit itself, on a pattern that is about to be substituted. This is where
// the configuration file's <match target="pattern"> rules ran, and it has to
// be here and not at FcFontSort: by then the distribution's own rules have
// rewritten the generic into a list of real families, and nothing is left to
// recognize but the language.
bool EditInPlace(FcPattern* pattern)
{
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    static auto del = Next<FcPatternDelFn>("FcPatternDel");
    static auto add_string = Next<FcPatternAddStringFn>("FcPatternAddString");
    if (get_string == nullptr || del == nullptr || add_string == nullptr ||
        pattern == nullptr) {
        return false;
    }

    FcChar8* family = nullptr;
    if (get_string(pattern, "family", 0, &family) != kFcResultMatch || family == nullptr) {
        return false;
    }
    const auto* name = reinterpret_cast<const char*>(family);
    if (!IsGenericRequest(name)) {
        return false;
    }

    del(pattern, "lang");
    if (ResolvesToArial(name)) {
        del(pattern, "family");
        add_string(pattern, "family", reinterpret_cast<const FcChar8*>("Arial"));
    }
    return true;
}

// Put the family the caller asked for back at the head of the list, if
// substitution moved something in front of it. Everything else substitution
// did is left alone: an alias that renames a family this machine does not have
// is how fontconfig finds a replacement, and Windows skips a missing family
// the same way.
//
// Rebuilding the list with FcPatternAddString binds strongly, where the
// <alias><prefer> edits it replaces were weak, so the aliases that survive are
// promoted alongside the request. A machine with both the Windows family and a
// free one substituted in front of it then draws the Windows family, which is
// what the same page draws on Windows.
//
// This runs inside another process's fontconfig calls, so the null checks
// below cover arguments no caller in this library chose.
// ReSharper disable once CppDFAConstantConditions
// The family Windows resolves this name to, or null when it resolves to
// itself. gfxDWriteFontList::FindAndAddFamiliesLocked rewrites the key name
// through the substitute table before it looks a family up, so the answer is
// the same whatever language asked; gfxFcPlatformFontList has no such step and
// hands the name to fontconfig, which answers "Times" under lang=hy with
// Sylfaen where Windows has Times New Roman.
//
// AddSubstitute records a row only when the actual font is installed, and
// skips one whose own name is an installed family, so both tests are applied
// here against the families Windows ships.
const char* WindowsFontSubstitute(const char* family)
{
#if CLEARTYPE_FIREFOX_PARITY
    if (family == nullptr || !dwcft::ParityActive() || ShipsWithWindows(family)) {
        return nullptr;
    }
    for (const firefox_parity::FontSubstitute& sub : firefox_parity::kFontSubstitutes) {
        if (strcasecmp(sub.substitute, family) == 0 && ShipsWithWindows(sub.actual)) {
            return sub.actual;
        }
    }
#else
    (void)family;
#endif
    return nullptr;
}

void KeepRequestedFamilyFirst(FcPattern* pattern, const char* wanted)
{
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    static auto del = Next<FcPatternDelFn>("FcPatternDel");
    static auto add_string = Next<FcPatternAddStringFn>("FcPatternAddString");
    if (get_string == nullptr || del == nullptr || add_string == nullptr ||
        pattern == nullptr || wanted == nullptr) {
        return;
    }

    FcChar8* first = nullptr;
    if (get_string(pattern, "family", 0, &first) != kFcResultMatch || first == nullptr) {
        return;
    }
    if (std::strcmp(reinterpret_cast<const char*>(first), wanted) == 0) {
        return;                              // still first: nothing to undo
    }

    // The strings belong to the pattern, so they have to be copied out before
    // the list is deleted.
    std::vector<std::string> families;
    for (int i = 0;; ++i) {
        FcChar8* name = nullptr;
        if (get_string(pattern, "family", i, &name) != kFcResultMatch || name == nullptr) {
            break;
        }
        families.emplace_back(reinterpret_cast<const char*>(name));
    }

    del(pattern, "family");
    add_string(pattern, "family", reinterpret_cast<const FcChar8*>(wanted));
    for (const std::string& name : families) {
        if (name != wanted) {
            add_string(pattern, "family", reinterpret_cast<const FcChar8*>(name.c_str()));
        }
    }
}

// A copy of the pattern with the language dropped, and the family replaced
// where Windows would have replaced it. Returns nullptr when there is nothing
// to change, and the caller then uses the pattern it was given.
FcPattern* WindowsPattern(const FcPattern* pattern)
{
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    static auto duplicate = Next<FcPatternDuplicateFn>("FcPatternDuplicate");
    static auto del = Next<FcPatternDelFn>("FcPatternDel");
    static auto add_string = Next<FcPatternAddStringFn>("FcPatternAddString");
    if (get_string == nullptr || duplicate == nullptr || del == nullptr ||
        add_string == nullptr || pattern == nullptr) {
        return nullptr;
    }

    FcChar8* family = nullptr;
    if (get_string(pattern, "family", 0, &family) != kFcResultMatch || family == nullptr) {
        return nullptr;
    }
    const auto* name = reinterpret_cast<const char*>(family);
    if (!IsGenericRequest(name)) {
        return nullptr;
    }

    FcPattern* copy = duplicate(pattern);
    if (copy == nullptr) {
        return nullptr;
    }
    del(copy, "lang");
    if (ResolvesToArial(name)) {
        del(copy, "family");
        add_string(copy, "family", reinterpret_cast<const FcChar8*>("Arial"));
    }
    return copy;
}

}  // namespace

// The system font set, reduced to what a stock Windows 11 ships.
//
// A page that names a family gets it when the machine has it, and Firefox is
// right to give it, since gfxFcPlatformFontList built its list from every font
// fontconfig knows. A `font-family: "Open Sans", sans-serif` therefore draws
// in Open Sans on a machine that has the face and in Arial on a Windows that
// does not.
// gfxWindowsPlatform::GetCommonFallbackFonts names Noto faces for a dozen
// scripts with the comment that a user might have them, which holds here and
// not on a stock Windows.
//
// Neither happens if Gecko is never told about the fonts Windows does not
// have. The set is borrowed, not owned by the caller, so the filtered copy is
// built once and kept; the patterns in it are duplicates, so fontconfig's own
// set is left exactly as it was.
//
// Both answers are cached, and the copy is released on the path that decides
// this machine is not one with the fonts.
FcFontSet* WindowsOnlySet(FcFontSet* all)
{
    static auto create = Next<FcFontSetCreateFn>("FcFontSetCreate");
    static auto add = Next<FcFontSetAddFn>("FcFontSetAdd");
    static auto duplicate = Next<FcPatternDuplicateFn>("FcPatternDuplicate");
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    static auto destroy_set = Next<FcFontSetDestroyFn>("FcFontSetDestroy");
    if (create == nullptr || add == nullptr || duplicate == nullptr || get_string == nullptr ||
        destroy_set == nullptr) {
        return all;
    }

    // Reachable from any thread, since every toolkit in the process asks
    // fontconfig for the system set.
    static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&cache_mutex);

    // What was answered last, and for which set. The negative answer is cached
    // too, since reaching it costs a scan of every installed face.
    //
    // A set fontconfig destroyed and rebuilt at the same address matches here
    // and gets the older copy. The patterns in it are this function's own
    // duplicates, so nothing dangles and the list is only out of date.
    static FcFontSet* cached_for = nullptr;
    static FcFontSet* cached_answer = nullptr;
    if (cached_for == all) {
        FcFontSet* const answer = cached_answer;
        pthread_mutex_unlock(&cache_mutex);
        return answer;
    }

    const auto* view = reinterpret_cast<const FontSetLayout*>(all);
    if (view == nullptr || view->fonts == nullptr || view->nfont <= 0) {
        pthread_mutex_unlock(&cache_mutex);
        return all;
    }

    FcFontSet* kept = create();
    if (kept == nullptr) {
        pthread_mutex_unlock(&cache_mutex);
        return all;
    }
    std::vector<std::string> families;
    for (int i = 0; i < view->nfont; ++i) {
        const FcPattern* face = view->fonts[i];
        bool windows = false;
        std::string first;
        for (int n = 0; !windows; ++n) {
            FcChar8* family = nullptr;
            if (get_string(face, "family", n, &family) != kFcResultMatch || family == nullptr) {
                break;
            }
            const auto* name = reinterpret_cast<const char*>(family);
            if (n == 0) {
                first.assign(name);
            }
            windows = ShipsWithWindows(name);
        }
        if (!windows) {
            continue;
        }
        if (FcPattern* copy = duplicate(face); copy != nullptr && add(kept, copy) == 0) {
            // add() takes ownership on success only.
            static auto destroy = Next<FcPatternDestroyFn>("FcPatternDestroy");
            if (destroy != nullptr) {
                destroy(copy);
            }
        }
        if (std::ranges::find(families, first) == families.end()) {
            families.push_back(first);
        }
    }

    if (const auto* keptview = reinterpret_cast<const FontSetLayout*>(kept);
        static_cast<int>(families.size()) < kMinimumWindowsFamilies ||
        keptview == nullptr || keptview->nfont <= 0) {
        // Not a machine with the fonts. The scan built a full set of duplicated
        // patterns to decide that, and they are this function's to release.
        destroy_set(kept);
        cached_for = all;
        cached_answer = all;
        pthread_mutex_unlock(&cache_mutex);
        return all;
    }
    cached_for = all;
    cached_answer = kept;
    pthread_mutex_unlock(&cache_mutex);
    return kept;
}

extern "C" __attribute__((visibility("default")))
FcFontSet* FcConfigGetFonts(FcConfig* config, const int set)
{
    static auto real = Next<FcConfigGetFontsFn>("FcConfigGetFonts");
    if (real == nullptr) {
        return nullptr;
    }
    FcFontSet* all = real(config, set);
    if (!Answering() || set != kFcSetSystem || all == nullptr) {
        return all;
    }
    return WindowsOnlySet(all);
}

// A family this machine carries and Windows does not, asked for by name.
// SkFontConfigInterfaceDirect::matchFamilyName decides a match is acceptable
// by comparing the family it asked for against the family it got, so renaming
// the request to something nothing is called makes the match fail and Blink
// moves on to the next family in the CSS list, which is what Windows does.
constexpr const char* kNoSuchFamily = "DWriteCoreNoSuchFamily";

// The cursive row of font_defaults.cc goes to a script Linux has no row for,
// so cursive reaches here as the family the WebPreferences constructor holds.
// locale_settings_win.grd reads Comic Sans MS for it.
constexpr const char* kConstructorCursive = "Script";
constexpr const char* kWindowsCursive = "Comic Sans MS";

bool ReplaceFamily(FcPattern* pattern, const char* with)
{
    static auto del = Next<FcPatternDelFn>("FcPatternDel");
    static auto add_string = Next<FcPatternAddStringFn>("FcPatternAddString");
    if (del == nullptr || add_string == nullptr) {
        return false;
    }
    (void)del(pattern, "family");
    (void)add_string(pattern, "family", reinterpret_cast<const FcChar8*>(with));
    return true;
}

// Whether HideFromChromium already renamed this request. The rename alone is
// not enough to make the match fail: SkFontConfigInterfaceDirect::MatchFont
// also accepts a result whose family equals the family originally requested,
// and the configuration's default answer for a name nothing matches can be
// exactly the hidden family that was asked for. A hidden request is answered with no
// fonts at all instead, which is what Windows says about a family it does
// not have.
bool HiddenRequest(FcPattern* pattern)
{
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    if (get_string == nullptr || pattern == nullptr) {
        return false;
    }
    FcChar8* first = nullptr;
    return get_string(pattern, "family", 0, &first) == kFcResultMatch &&
           first != nullptr &&
           std::strcmp(reinterpret_cast<const char*>(first), kNoSuchFamily) == 0;
}

bool HideFromChromium(FcPattern* pattern)
{
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    if (get_string == nullptr || pattern == nullptr) {
        return false;
    }
    FcChar8* first = nullptr;
    if (get_string(pattern, "family", 0, &first) != kFcResultMatch || first == nullptr) {
        return false;
    }
    const auto* name = reinterpret_cast<const char*>(first);
    if (std::strcmp(name, kConstructorCursive) == 0) {
        (void)ReplaceFamily(pattern, kWindowsCursive);
        return false;
    }
    // A generic keyword arriving as a family name is a quoted one. Blink
    // resolves a real generic through GenericFontFamilySettings before the
    // request reaches fontconfig, so "serif" is already Times New Roman here;
    // a literal "fantasy" is a family name no Windows font collection has,
    // and fontconfig's own aliases would otherwise answer it with Impact.
    if (ShipsWithWindows(name)) {
        return false;
    }
    return ReplaceFamily(pattern, kNoSuchFamily);
}

// A sentinel pattern whose requested family is a generic keyword, meaning the
// author quoted it.
bool QuotedGenericSentinel(const FcPattern* pattern)
{
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    if (get_string == nullptr || pattern == nullptr || !HasGeckoSentinel(pattern)) {
        return false;
    }
    FcChar8* first = nullptr;
    return get_string(pattern, "family", 0, &first) == kFcResultMatch &&
           first != nullptr && IsGenericRequest(reinterpret_cast<const char*>(first));
}

extern "C" __attribute__((visibility("default")))
FcBool FcConfigSubstitute(FcConfig* config, FcPattern* pattern, const int kind)
{
    static auto real = Next<FcConfigSubstituteFn>("FcConfigSubstitute");
    if (real == nullptr) {
        return 0;
    }
    if (chromium_patch::ParityWanted() && kind == kFcMatchPattern &&
        HideFromChromium(pattern)) {
        return real(config, pattern, kind);
    }
    // Gecko reads the substitution list this call returns and collects the
    // families in it up to the terminator. Left unexpanded, the quoted word is
    // the only entry, no family answers to it, and Gecko moves on to the next
    // name in the author's list, which is what Windows does with a family it
    // does not ship. Expanded, fontconfig's own aliases answer "fantasy" with
    // Impact and "cursive" with Comic Sans MS, and Gecko takes that as a
    // match.
    if (Answering() && kind == kFcMatchPattern && QuotedGenericSentinel(pattern)) {
        return 1;
    }
    if (!Answering() || kind != kFcMatchPattern) {
        return real(config, pattern, kind);
    }

    EditInPlace(pattern);

    // What the caller asked for, before any rule has seen it.
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    std::string wanted;
    if (get_string != nullptr && pattern != nullptr) {
        FcChar8* first = nullptr;
        if (get_string(pattern, "family", 0, &first) == kFcResultMatch && first != nullptr) {
            wanted.assign(reinterpret_cast<const char*>(first));
        }
    }

    // Windows substitutes the name before it looks for a family, so the
    // substituted one is what fontconfig is asked about and what stays first.
    if (const char* actual = WindowsFontSubstitute(wanted.c_str())) {
        static auto del = Next<FcPatternDelFn>("FcPatternDel");
        static auto add_string = Next<FcPatternAddStringFn>("FcPatternAddString");
        if (del != nullptr && add_string != nullptr) {
            std::vector<std::string> rest;
            for (int i = 1;; ++i) {
                FcChar8* name = nullptr;
                if (get_string(pattern, "family", i, &name) != kFcResultMatch || name == nullptr) {
                    break;
                }
                rest.emplace_back(reinterpret_cast<const char*>(name));
            }
            del(pattern, "family");
            add_string(pattern, "family", reinterpret_cast<const FcChar8*>(actual));
            for (const std::string& name : rest) {
                add_string(pattern, "family", reinterpret_cast<const FcChar8*>(name.c_str()));
            }
            wanted.assign(actual);
        }
    }

    const FcBool ok = real(config, pattern, kind);
    if (!wanted.empty()) {
        KeepRequestedFamilyFirst(pattern, wanted.c_str());
    }
    return ok;
}

// Whether the running call was made by fontconfig itself. Its public entry
// points call one another through the PLT, so an interposed function is
// reached from inside the library too, and a result it is still reading must
// go back untouched.
thread_local unsigned g_inside_fontconfig = 0;

// A sort reduced in place to the faces Windows ships. The per-character
// fallback query carries a charset and no family, so fontconfig offers
// whatever installed face covers the character, and a face Windows does not
// have must not be offered. The set object itself goes back to the caller
// untouched apart from the shorter list, since its consumers hold pointers
// into it; the dropped patterns lose the reference the sort gave them. A
// character only the dropped faces covered draws the same missing-glyph box
// it draws on Windows.
void WindowsOnlyPrune(FcFontSet* sorted)
{
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    static auto destroy = Next<FcPatternDestroyFn>("FcPatternDestroy");
    if (get_string == nullptr || destroy == nullptr || sorted == nullptr) {
        return;
    }
    auto* view = reinterpret_cast<FontSetLayout*>(sorted);
    if (view->fonts == nullptr) {
        return;
    }
    int kept = 0;
    for (int i = 0; i < view->nfont; ++i) {
        FcPattern* face = view->fonts[i];
        bool windows = false;
        for (int n = 0; !windows; ++n) {
            FcChar8* family = nullptr;
            if (get_string(face, "family", n, &family) != kFcResultMatch ||
                family == nullptr) {
                break;
            }
            windows = ShipsWithWindows(reinterpret_cast<const char*>(family));
        }
        if (windows) {
            view->fonts[kept++] = face;
        } else {
            destroy(face);
        }
    }
    view->nfont = kept;
}

extern "C" __attribute__((visibility("default")))
FcFontSet* FcFontSort(FcConfig* config, FcPattern* pattern, const FcBool trim,
                      FcCharSet** csp, FcResult* result)
{
    static auto real = Next<FcFontSortFn>("FcFontSort");
    static auto destroy = Next<FcPatternDestroyFn>("FcPatternDestroy");
    static auto create_set = Next<FcFontSetCreateFn>("FcFontSetCreate");
    if (real == nullptr) {
        return nullptr;
    }
    if (chromium_patch::ParityWanted() && create_set != nullptr &&
        HiddenRequest(pattern)) {
        if (result != nullptr) {
            *result = kFcResultNoMatch;
        }
        return create_set();
    }
    FcPattern* copy = Answering() ? WindowsPattern(pattern) : nullptr;
    FcFontSet* set = real(config, copy != nullptr ? copy : pattern, trim, csp, result);
    if (copy != nullptr && destroy != nullptr) {
        destroy(copy);
    }
    if (chromium_patch::ParityWanted() && g_inside_fontconfig == 0) {
        WindowsOnlyPrune(set);
    }
    // The Chromium half watches the result to learn which family carries which
    // charset. It substitutes nothing, so it runs after the sort either way.
    fallback_order::NoteFontSet(pattern, set);
    // A named-family sort is then reordered so DirectWrite's pick sits first.
    family_match::ReorderForWindows(pattern, set);
    return set;
}

extern "C" __attribute__((visibility("default")))
FcPattern* FcFontMatch(FcConfig* config, FcPattern* pattern, FcResult* result)
{
    static auto real = Next<FcFontMatchFn>("FcFontMatch");
    static auto destroy = Next<FcPatternDestroyFn>("FcPatternDestroy");
    if (real == nullptr) {
        return nullptr;
    }
    if (chromium_patch::ParityWanted() && HiddenRequest(pattern)) {
        if (result != nullptr) {
            *result = kFcResultNoMatch;
        }
        return nullptr;
    }
    FcPattern* copy = Answering() ? WindowsPattern(pattern) : nullptr;
    // FcFontMatch reaches the public FcFontSort through the PLT, so the sort
    // this library filters must not be the one FcFontMatch is still reading.
    ++g_inside_fontconfig;
    FcPattern* matched = real(config, copy != nullptr ? copy : pattern, result);
    --g_inside_fontconfig;
    if (copy != nullptr && destroy != nullptr) {
        destroy(copy);
    }
    return matched;
}

namespace {

#endif  // CLEARTYPE_FIREFOX_PARITY

}  // namespace

// Whether any face of this pattern's family is bold. Blink on Linux starts
// synthetic bold at `weight + 200` of the face it selected, where Windows
// starts it at 600 flat, so a family with no bold face draws regular text at
// 600 on Linux and emboldened text on Windows. Reporting the regular face one
// fontconfig step lighter moves the Linux threshold onto Windows', and asking
// this first keeps every family that has a real bold face untouched.
bool FamilyHasBoldFace(const FcPattern* p)
{
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    static auto get_integer = Next<FcPatternGetIntegerFn>("FcPatternGetInteger");
    static auto get_fonts = Next<FcConfigGetFontsFn>("FcConfigGetFonts");
    if (p == nullptr || get_string == nullptr || get_integer == nullptr ||
        get_fonts == nullptr) {
        return true;
    }

    static pthread_mutex_t bold_mutex = PTHREAD_MUTEX_INITIALIZER;
    static std::vector<std::string>* bold = nullptr;

    FcChar8* family = nullptr;
    if (get_string(p, "family", 0, &family) != kFcResultMatch || family == nullptr) {
        return true;
    }
    const auto* name = reinterpret_cast<const char*>(family);

    pthread_mutex_lock(&bold_mutex);
    if (bold == nullptr) {
        bold = new std::vector<std::string>();
        // The real font set, since asking our own export here would re-enter
        // the call that got us here.
        if (FcFontSet* all = get_fonts(nullptr, kFcSetSystem); all != nullptr) {
            const auto* view = reinterpret_cast<const FontSetLayout*>(all);
            for (int i = 0; view->fonts != nullptr && i < view->nfont; ++i) {
                int weight = 0;
                if (get_integer(view->fonts[i], "weight", 0, &weight) != kFcResultMatch ||
                    weight < kFcWeightBold) {
                    continue;
                }
                for (int f = 0; ; ++f) {
                    FcChar8* other = nullptr;
                    if (get_string(view->fonts[i], "family", f, &other) != kFcResultMatch ||
                        other == nullptr) {
                        break;
                    }
                    bold->emplace_back(reinterpret_cast<const char*>(other));
                }
            }
        }
    }
    const bool found = std::ranges::find(*bold, name) != bold->end();
    pthread_mutex_unlock(&bold_mutex);
    return found;
}

#if CLEARTYPE_FIREFOX_PARITY
// Whether this pattern's family also carries a LIGHT face.
//
// gfxFcPlatformFontList::MapFcWeight buckets a fontconfig weight into hundreds
// and DEMILIGHT lands in the same 300 bucket as LIGHT, so a family carrying
// both has two faces at one weight and the order decides between them.
// DirectWrite reports the Semilight face's real 350 and reaches it only where
// Regular does not cover the character. Answering REGULAR for that face gives
// the same result. LIGHT keeps everything below 400, and at 400 and above
// Semilight ties with the family's own Regular face and loses the tie, since
// FindAllFontsForStyle keeps the standard face ahead of it.
//
// Only where a LIGHT face exists to be separated from. A family with a
// Semilight and no Light, such as Leelawadee UI or Nirmala UI, has 350 as the
// closest face to every weight below 400, and moving it to the 400 bucket
// would hand those weights to Regular.

bool FamilyHasLightFace(const FcPattern* p)
{
    static auto get_string = Next<FcPatternGetStringFn>("FcPatternGetString");
    static auto get_integer = Next<FcPatternGetIntegerFn>("FcPatternGetInteger");
    static auto get_fonts = Next<FcConfigGetFontsFn>("FcConfigGetFonts");
    if (p == nullptr || get_string == nullptr || get_integer == nullptr ||
        get_fonts == nullptr) {
        return false;
    }

    // The real font set, since asking our own export here would re-enter the
    // cache that calls this.
    static pthread_mutex_t light_mutex = PTHREAD_MUTEX_INITIALIZER;
    static std::vector<std::string>* light = nullptr;

    FcChar8* family = nullptr;
    if (get_string(p, "family", 0, &family) != kFcResultMatch || family == nullptr) {
        return false;
    }
    const auto* name = reinterpret_cast<const char*>(family);

    pthread_mutex_lock(&light_mutex);
    if (light == nullptr) {
        light = new std::vector<std::string>();
        if (FcFontSet* all = get_fonts(nullptr, kFcSetSystem); all != nullptr) {
            const auto* view = reinterpret_cast<const FontSetLayout*>(all);
            for (int i = 0; view->fonts != nullptr && i < view->nfont; ++i) {
                int weight = 0;
                if (get_integer(view->fonts[i], "weight", 0, &weight) != kFcResultMatch ||
                    weight != kFcWeightLight) {
                    continue;
                }
                for (int f = 0; ; ++f) {
                    FcChar8* other = nullptr;
                    if (get_string(view->fonts[i], "family", f, &other) != kFcResultMatch ||
                        other == nullptr) {
                        break;
                    }
                    light->emplace_back(reinterpret_cast<const char*>(other));
                }
            }
        }
    }
    const bool found = std::ranges::find(*light, name) != light->end();
    pthread_mutex_unlock(&light_mutex);
    return found;
}
#endif

extern "C" __attribute__((visibility("default")))
FcResult FcPatternGetInteger(const FcPattern* p, const char* object, const int n, int* value)
{
    static auto real = Next<FcPatternGetIntegerFn>("FcPatternGetInteger");
    int answer = 0;
    if (Answering() && value != nullptr && IntegerAnswer(object, n, &answer)) {
        *value = answer;
        return kFcResultMatch;
    }
    if (real == nullptr) {
        // No real fontconfig to forward to. The out-parameter is written
        // anyway, since a caller that ignores the result and reads the value
        // is the crash this file's header describes.
        if (value != nullptr) {
            *value = 0;
        }
        return kFcResultNoMatch;
    }
    const FcResult result = real(p, object, n, value);
    // Skia builds a typeface's SkFontStyle from this, and Blink then compares
    // the run's weight against it. One step lighter maps to 396 instead of
    // 400, which is what puts the Linux synthetic-bold threshold on 600 where
    // Windows has it. Only for a family with no bold face; everywhere else the
    // bolder face is the one selected and its own weight is what gets read.
    if (result == kFcResultMatch && value != nullptr && *value == kFcWeightRegular &&
        Answers(object, n, "weight") && chromium_patch::ParityWanted() &&
        !dwcft::IsOffValue(std::getenv("DWC_WEIGHT_600")) && !FamilyHasBoldFace(p)) {
        *value = kFcWeightRegular - 1;
    }
#if CLEARTYPE_FIREFOX_PARITY
    if (result == kFcResultMatch && value != nullptr && *value == kFcWeightDemilight &&
        Answers(object, n, "weight") && dwcft::ParityActive() &&
        FamilyHasLightFace(p)) {
        *value = kFcWeightRegular;
    }
#endif
    return result;
}

extern "C" __attribute__((visibility("default")))
FcResult FcPatternGetBool(const FcPattern* p, const char* object, const int n, FcBool* value)
{
    static auto real = Next<FcPatternGetBoolFn>("FcPatternGetBool");
    int answer = 0;
    if (Answering() && value != nullptr && BoolAnswer(object, n, &answer)) {
        *value = answer;
        return kFcResultMatch;
    }
    if (real == nullptr) {
        if (value != nullptr) {
            *value = 0;
        }
        return kFcResultNoMatch;
    }
    return real(p, object, n, value);
}

// cairo asks for the same properties through this one. fontconfig.h: FcValue
// is a type tag and a union, FcTypeInteger 1 and FcTypeBool 4.
extern "C" __attribute__((visibility("default")))
FcResult FcPatternGet(const FcPattern* p, const char* object, const int n, FcValue* value)
{
    static auto real = Next<FcPatternGetFn>("FcPatternGet");
    int answer = 0;
    if (Answering() && value != nullptr) {
        if (IntegerAnswer(object, n, &answer)) {
            value->type = kFcTypeInteger;
            value->u.i = answer;
            return kFcResultMatch;
        }
        if (BoolAnswer(object, n, &answer)) {
            value->type = kFcTypeBool;
            value->u.b = answer;
            return kFcResultMatch;
        }
    }
    if (real == nullptr) {
        if (value != nullptr) {
            value->type = kFcTypeInteger;
            value->u.i = 0;
        }
        return kFcResultNoMatch;
    }
    return real(p, object, n, value);
}
