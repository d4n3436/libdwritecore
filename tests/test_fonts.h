// test_fonts.h - find fonts for the tests the way the rest of the system does.
//
// Absolute paths under /usr/share/fonts are a packaging decision and differ on
// every distribution, and a test that cannot find its font is worse than one
// that fails: it prints "skipping", returns 0, and leaves ctest reporting a
// pass for work it did not do. fontconfig knows where fonts are on any
// distribution, and it is what Firefox asks too.
//
// Two rules here, both about not lying:
//
//   * fontconfig substitutes. Ask FcFontMatch for a font covering a language
//     nothing on the machine covers and it still answers - with a font that
//     does not cover it. Every result is checked against what was asked for
//     before it is handed back.
//   * When nothing suitable exists the caller exits kSkipExit, which CMake
//     maps to ctest's Skipped state through SKIP_RETURN_CODE. A test that
//     could not run then reads as skipped instead of hiding inside a pass.

#ifndef DWC_TEST_FONTS_H
#define DWC_TEST_FONTS_H

#include <string>

// ctest's conventional skip code; see SKIP_RETURN_CODE in CMakeLists.txt.
constexpr int kSkipExit = 77;

#ifdef HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

namespace testfonts
{

#ifdef HAVE_FONTCONFIG

inline bool FileOf(const FcPattern* pattern, std::string* out)
{
    FcChar8* file = nullptr;
    if (FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch ||
        file == nullptr)
    {
        return false;
    }
    *out = reinterpret_cast<const char*>(file);
    return !out->empty();
}

// One named family, exactly. FcFontList filters the font set instead of
// matching against it, so it never substitutes: an empty result means the
// family genuinely is not installed.
inline bool ByFamily(const char* family, std::string* out)
{
    FcPattern* pattern = FcPatternCreate();
    if (pattern == nullptr) { return false; }
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(family));
    FcPatternAddBool(pattern, FC_OUTLINE, FcTrue);

    FcObjectSet* objects = FcObjectSetBuild(FC_FILE, FC_WEIGHT, FC_SLANT, nullptr);
    FcFontSet* set = FcFontList(nullptr, pattern, objects);

    // FcFontList does not order its result, so a family with several faces
    // would otherwise hand back whichever one the scan reached first - Bold on
    // one machine, Medium on the next. Prefer the upright regular face so a
    // rerun picks the same file.
    bool found = false;
    if (set != nullptr && set->nfont > 0)
    {
        const FcPattern* pick = set->fonts[0];
        for (int i = 0; i < set->nfont; ++i)
        {
            int weight = 0;
            int slant = 0;
            if (FcPatternGetInteger(set->fonts[i], FC_WEIGHT, 0, &weight) == FcResultMatch &&
                FcPatternGetInteger(set->fonts[i], FC_SLANT, 0, &slant) == FcResultMatch &&
                weight == FC_WEIGHT_REGULAR && slant == FC_SLANT_ROMAN)
            {
                pick = set->fonts[i];
                break;
            }
        }
        found = FileOf(pick, out);
    }

    if (set != nullptr) { FcFontSetDestroy(set); }
    if (objects != nullptr) { FcObjectSetDestroy(objects); }
    FcPatternDestroy(pattern);
    return found;
}

// Any outline font that really covers `lang`. FcFontMatch is used for its
// ordering - it returns the font the system would prefer - and then the answer
// is verified against the language set it declares. That check is the whole
// point: without it, a machine with no Arabic font at all yields a Latin font
// and the test goes on to assert things about Arabic shaping with it.
inline bool ByLang(const char* lang, std::string* out)
{
    FcPattern* pattern = FcPatternCreate();
    if (pattern == nullptr) { return false; }
    FcPatternAddString(pattern, FC_LANG, reinterpret_cast<const FcChar8*>(lang));
    FcPatternAddBool(pattern, FC_OUTLINE, FcTrue);
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result = FcResultNoMatch;
    FcPattern* matched = FcFontMatch(nullptr, pattern, &result);

    bool found = false;
    if (matched != nullptr)
    {
        FcLangSet* langs = nullptr;
        if (FcPatternGetLangSet(matched, FC_LANG, 0, &langs) == FcResultMatch &&
            langs != nullptr &&
            FcLangSetHasLang(langs, reinterpret_cast<const FcChar8*>(lang)) !=
                FcLangDifferentLang)
        {
            found = FileOf(matched, out);
        }
        FcPatternDestroy(matched);
    }
    FcPatternDestroy(pattern);
    return found;
}

#else  // !HAVE_FONTCONFIG

inline bool ByFamily(const char*, std::string*) { return false; }
inline bool ByLang(const char*, std::string*) { return false; }

#endif

// A plain Latin outline font. The named families are preferences, not
// requirements - whichever is present is fine, and if none is, any font
// declaring Latin coverage does.
inline std::string AnyOutlineFont()
{
    static constexpr const char* kPreferred[] = {
        "DejaVu Sans", "Liberation Sans", "Noto Sans", "Arial", "FreeSans",
    };
    std::string path;
    for (const char* family : kPreferred)
    {
        if (ByFamily(family, &path)) { return path; }
    }
    if (ByLang("en", &path)) { return path; }
    return std::string();
}

// A font for one script. `preferred_family` keeps a test that distinguishes
// two typefaces of the same script - Naskh against Kufi, say - distinguishing
// them wherever both are installed, while `lang` keeps the test meaningful
// where neither is. Empty when the machine has nothing covering the script.
inline std::string FontForLang(const char* lang, const char* preferred_family)
{
    std::string path;
    if (preferred_family != nullptr && ByFamily(preferred_family, &path))
    {
        return path;
    }
    if (ByLang(lang, &path)) { return path; }
    return std::string();
}

}  // namespace testfonts

#endif  // DWC_TEST_FONTS_H
