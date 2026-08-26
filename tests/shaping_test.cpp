//+--------------------------------------------------------------------------
//
//  shaping_test.cpp - complex script shaping: Arabic, CJK, Hebrew, Devanagari.
//
//  This is the deepest path in the library. Unlike the Latin ClearType test,
//  which only does a cmap lookup, this drives the full pipeline:
//
//      AnalyzeScript / AnalyzeBidi  ->  GetGlyphs  ->  GetGlyphPlacements
//                                   ->  CreateGlyphRunAnalysis
//
//  It also exercises the COM ABI in the *reverse* direction. AnalyzeScript
//  takes an IDWriteTextAnalysisSource and an IDWriteTextAnalysisSink that the
//  caller implements, so DWriteCore calls into vtables built by this file
//  rather than the other way round. Those are the interfaces that carry no
//  QueryInterface registry entry in the binary precisely because they are
//  app-implemented, so this is the first check that they really work.
//
//  The load-bearing assertion for Arabic is that shaped glyph IDs differ from
//  the raw cmap lookup. Arabic letters take contextual forms - initial,
//  medial, final, isolated - which only appear if the OpenType GSUB tables
//  were actually applied. Identical IDs would mean the shaping engine never
//  ran, even though everything would still "succeed".
//
//----------------------------------------------------------------------------

#include "dwrite_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_fonts.h"

namespace
{

int g_failures = 0;
int g_skipped = 0;
int g_ran = 0;

void Fail(const char* what, const char* why)
{
    std::printf("    FAIL  %s: %s\n", what, why);
    ++g_failures;
}

std::u16string U16(const char* utf8)
{
    // Minimal UTF-8 -> UTF-16 for the literals in this file.
    std::u16string out;
    const auto* p = reinterpret_cast<const unsigned char*>(utf8);
    while (*p)
    {
        unsigned cp;
        if (*p < 0x80) { cp = *p++; }
        else if ((*p & 0xE0) == 0xC0) { cp = static_cast<UINT32>((*p & 0x1F) << 6); cp |= static_cast<UINT32>(*++p & 0x3F); ++p; }
        else if ((*p & 0xF0) == 0xE0)
        {
            cp = static_cast<UINT32>((*p & 0x0F) << 12);
            cp |= static_cast<UINT32>((*++p & 0x3F) << 6);
            cp |= static_cast<UINT32>(*++p & 0x3F); ++p;
        }
        else
        {
            cp = static_cast<UINT32>((*p & 0x07) << 18);
            cp |= static_cast<UINT32>((*++p & 0x3F) << 12);
            cp |= static_cast<UINT32>((*++p & 0x3F) << 6);
            cp |= static_cast<UINT32>(*++p & 0x3F); ++p;
        }
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        }
        else
        {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Caller-implemented analysis callbacks.
//
// Reference counting is trivial here: both objects live on the stack for the
// duration of the analysis call, so AddRef/Release just have to be valid.
// ---------------------------------------------------------------------------

// No virtual destructor: these implement COM interfaces, where a destructor
// slot would sit in the vtable and move every method after it. Both live on
// the stack for the duration of one call and are never deleted through a
// base pointer.
// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
struct AnalysisSource : IDWriteTextAnalysisSource
{
    const std::u16string& text;
    const WCHAR* locale;
    DWRITE_READING_DIRECTION direction;

    AnalysisSource(const std::u16string& t, const WCHAR* loc, const DWRITE_READING_DIRECTION dir)
        : text(t), locale(loc), direction(dir) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == DWRITE_UUIDOF(IDWriteTextAnalysisSource) ||
            riid == GUID{.Data1 = 0, .Data2 = 0, .Data3 = 0,
                         .Data4 = {0xC0, 0, 0, 0, 0, 0, 0, 0x46}})
        {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(
        const UINT32 pos, const WCHAR** str, UINT32* len) override
    {
        if (pos >= text.size()) { *str = nullptr; *len = 0; return S_OK; }
        *str = text.c_str() + pos;
        *len = static_cast<UINT32>(text.size() - pos);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(
        const UINT32 pos, const WCHAR** str, UINT32* len) override
    {
        if (pos == 0 || pos > text.size()) { *str = nullptr; *len = 0; return S_OK; }
        *str = text.c_str();
        *len = pos;
        return S_OK;
    }

    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override
    {
        return direction;
    }

    HRESULT STDMETHODCALLTYPE GetLocaleName(
        const UINT32 pos, UINT32* len, const WCHAR** name) override
    {
        (void)pos;
        *len = static_cast<UINT32>(text.size());
        *name = locale;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(
        const UINT32 pos, UINT32* len, IDWriteNumberSubstitution** sub) override
    {
        (void)pos;
        *len = static_cast<UINT32>(text.size());
        *sub = nullptr;
        return S_OK;
    }
};

struct ScriptRun
{
    DWRITE_SCRIPT_ANALYSIS analysis;
};

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
struct AnalysisSink : IDWriteTextAnalysisSink
{
    std::vector<ScriptRun> runs;
    UINT8 max_bidi_level = 0;
    int breakpoint_calls = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == DWRITE_UUIDOF(IDWriteTextAnalysisSink) ||
            riid == GUID{.Data1 = 0, .Data2 = 0, .Data3 = 0,
                         .Data4 = {0xC0, 0, 0, 0, 0, 0, 0, 0x46}})
        {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(
        UINT32 /*pos*/, UINT32 /*len*/, const DWRITE_SCRIPT_ANALYSIS* sa) override
    {
        runs.push_back(ScriptRun{*sa});
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(
        const UINT32 pos, const UINT32 len, const DWRITE_LINE_BREAKPOINT* bp) override
    {
        (void)pos; (void)len; (void)bp;
        ++breakpoint_calls;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetBidiLevel(
        const UINT32 pos, const UINT32 len, const UINT8 explicitLevel, const UINT8 resolvedLevel) override
    {
        (void)pos; (void)len; (void)explicitLevel;
        if (resolvedLevel > max_bidi_level) { max_bidi_level = resolvedLevel; }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(
        const UINT32 pos, const UINT32 len, IDWriteNumberSubstitution* sub) override
    {
        (void)pos; (void)len; (void)sub;
        return S_OK;
    }
};

// ---------------------------------------------------------------------------

struct Sample
{
    const char* name;
    const char* utf8;
    const char* locale;
    // How the font is found. `family` is a preference, kept so that the two
    // Arabic samples still exercise two different typefaces wherever both are
    // installed; `fclang` is the requirement, and is what makes the sample
    // meaningful on a machine that has neither. See test_fonts.h.
    const char* family;
    const char* fclang;
    bool rtl;
    bool expect_contextual; // shaped IDs must differ from the raw cmap lookup
};

IDWriteFontFace* LoadFace(IDWriteFactory* factory, const char* path)
{
    const std::u16string wpath = U16(path);
    IDWriteFontFile* file = nullptr;
    if (FAILED(factory->CreateFontFileReference(wpath.c_str(), nullptr, &file)) || !file)
    {
        return nullptr;
    }
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE ft = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32 faces = 0;
    (void)file->Analyze(&supported, &ft, &face_type, &faces);
    IDWriteFontFace* face = nullptr;
    if (!supported ||
        FAILED(factory->CreateFontFace(face_type, 1, &file, 0, DWRITE_FONT_SIMULATIONS_NONE, &face)))
    {
        face = nullptr;
    }
    file->Release();
    return face;
}

void RunSample(IDWriteFactory* factory, IDWriteTextAnalyzer* analyzer, const Sample& s)
{
    std::printf("\n== %s\n", s.name);
    const std::string font = testfonts::FontForLang(s.fclang, s.family);
    if (font.empty())
    {
        std::printf("    no installed font covers %s; skipping this sample\n", s.fclang);
        ++g_skipped;
        return;
    }
    ++g_ran;
    std::printf("    font: %s\n", font.c_str());

    IDWriteFontFace* face = LoadFace(factory, font.c_str());
    if (!face) { Fail(s.name, "could not load font face"); return; }

    const std::u16string text = U16(s.utf8);
    const std::u16string locale = U16(s.locale);
    std::printf("    %u UTF-16 code units\n", static_cast<unsigned>(text.size()));

    // --- analysis: DWriteCore calls back into our vtables here -------------
    AnalysisSource source(text, locale.c_str(),
                          s.rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                                : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
    AnalysisSink sink;

    if (FAILED(analyzer->AnalyzeScript(&source, 0, static_cast<UINT32>(text.size()), &sink)))
    {
        Fail(s.name, "AnalyzeScript failed");
        face->Release();
        return;
    }
    if (sink.runs.empty())
    {
        Fail(s.name, "AnalyzeScript produced no script runs");
        face->Release();
        return;
    }
    (void)analyzer->AnalyzeBidi(&source, 0, static_cast<UINT32>(text.size()), &sink);

    std::printf("    script runs: %zu (first: script=%u shapes=%u), max bidi level %u\n",
                sink.runs.size(), sink.runs[0].analysis.script,
                static_cast<unsigned>(sink.runs[0].analysis.shapes), sink.max_bidi_level);
    if (s.rtl && sink.max_bidi_level == 0)
    {
        Fail(s.name, "RTL text resolved to bidi level 0");
    }

    // --- shaping ----------------------------------------------------------
    const UINT32 max_glyphs = static_cast<UINT32>(text.size()) * 3 + 16;
    std::vector<UINT16> cluster_map(text.size());
    std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> text_props(text.size());
    std::vector<UINT16> glyphs(max_glyphs);
    std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyph_props(max_glyphs);
    UINT32 glyph_count = 0;

    if (const HRESULT hr = analyzer->GetGlyphs(text.c_str(), static_cast<UINT32>(text.size()), face, FALSE, s.rtl ? TRUE : FALSE, &sink.runs[0].analysis, locale.c_str(), nullptr, nullptr, nullptr, 0, max_glyphs, cluster_map.data(), text_props.data(), glyphs.data(), glyph_props.data(), &glyph_count); FAILED(hr) || glyph_count == 0)
    {
        Fail(s.name, "GetGlyphs produced no glyphs");
        face->Release();
        return;
    }
    glyphs.resize(glyph_count);
    std::printf("    shaped into %u glyphs\n", glyph_count);

    // --- did the shaping engine actually do anything? ---------------------
    std::vector<UINT32> cps;
    for (char16_t c : text) { cps.push_back(c); }
    std::vector<UINT16> raw(cps.size(), 0);
    (void)face->GetGlyphIndices(cps.data(), static_cast<UINT32>(cps.size()), raw.data());

    bool differs = glyph_count != text.size();
    if (!differs)
    {
        for (size_t i = 0; i < glyph_count; ++i)
        {
            if (glyphs[i] != raw[i]) { differs = true; break; }
        }
    }
    std::printf("    cmap lookup gives %zu glyphs; shaped output %s\n",
                raw.size(), differs ? "DIFFERS (GSUB applied)" : "is identical");
    if (s.expect_contextual && !differs)
    {
        Fail(s.name, "shaped glyphs identical to raw cmap - shaping engine did not run");
    }

    UINT32 notdef = 0;
    for (const UINT16 g : glyphs) { notdef += g == 0; }
    if (notdef * 2 > glyph_count)
    {
        Fail(s.name, "most glyphs are .notdef");
    }

    // --- placement and rasterization --------------------------------------
    constexpr FLOAT em = 28.0f;
    std::vector advances(glyph_count, 0.0f);
    std::vector<DWRITE_GLYPH_OFFSET> offsets(glyph_count);
    std::memset(offsets.data(), 0, offsets.size() * sizeof(DWRITE_GLYPH_OFFSET));

    if (FAILED(analyzer->GetGlyphPlacements(text.c_str(), cluster_map.data(), text_props.data(), static_cast<UINT32>(text.size()), glyphs.data(), glyph_props.data(), glyph_count, face, em, FALSE, s.rtl ? TRUE : FALSE, &sink.runs[0].analysis, locale.c_str(), nullptr, nullptr, 0, advances.data(), offsets.data())))
    {
        Fail(s.name, "GetGlyphPlacements failed");
        face->Release();
        return;
    }

    FLOAT total = 0;
    for (const FLOAT a : advances) { total += a; }
    std::printf("    total advance %.1f px at %.0f px/em\n",
                static_cast<double>(total), static_cast<double>(em));
    if (total <= 0.0f)
    {
        Fail(s.name, "zero total advance");
    }

    DWRITE_GLYPH_RUN run = {};
    run.fontFace = face;
    run.fontEmSize = em;
    run.glyphCount = glyph_count;
    run.glyphIndices = glyphs.data();
    run.glyphAdvances = advances.data();
    run.glyphOffsets = offsets.data();
    run.isSideways = FALSE;
    run.bidiLevel = s.rtl ? 1 : 0;

    IDWriteGlyphRunAnalysis* analysis = nullptr;
    if (FAILED(factory->CreateGlyphRunAnalysis(&run, 1.0f, nullptr, DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL, DWRITE_MEASURING_MODE_NATURAL, 0.0f, 0.0f, &analysis)) || !analysis)
    {
        Fail(s.name, "CreateGlyphRunAnalysis failed");
        face->Release();
        return;
    }

    RECT b = {};
    (void)analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &b);
    if (const int w = b.right - b.left, h = b.bottom - b.top; w <= 0 || h <= 0)
    {
        Fail(s.name, "empty texture bounds");
    }
    else
    {
        std::vector<BYTE> tex(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0);
        if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &b, tex.data(), static_cast<UINT32>(tex.size()))))
        {
            Fail(s.name, "CreateAlphaTexture failed");
        }
        else
        {
            size_t nonzero = 0;
            for (const BYTE v : tex) { nonzero += v != 0; }
            std::printf("    raster %dx%d, %zu/%zu bytes covered\n",
                        w, h, nonzero, tex.size());
            if (nonzero == 0) { Fail(s.name, "blank raster"); }

            for (int y = 0; y < h && y < 26; ++y)
            {
                std::printf("      ");
                for (int x = 0; x < w && x < 150; ++x)
                {
                    const auto* ramp = " .:-=+*#%@";
                    const BYTE* px = &tex[(static_cast<size_t>(y) * static_cast<size_t>(w) +
                                          static_cast<size_t>(x)) * 3];
                    std::putchar(ramp[(px[0] + px[1] + px[2]) / 3 * 9 / 255]);
                }
                std::putchar('\n');
            }
        }
    }

    analysis->Release();
    face->Release();
}

} // namespace

int main()
{
    IUnknown* unknown = nullptr;
    if (FAILED(DWriteCoreCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       DWRITE_UUIDOF(IDWriteFactory), &unknown)))
    {
        std::printf("FAIL  could not create factory\n");
        return 1;
    }
    auto* factory = reinterpret_cast<IDWriteFactory*>(unknown);

    IDWriteTextAnalyzer* analyzer = nullptr;
    if (FAILED(factory->CreateTextAnalyzer(&analyzer)) || !analyzer)
    {
        std::printf("FAIL  CreateTextAnalyzer\n");
        return 1;
    }
    std::printf("Complex script shaping test\n");

    const Sample samples[] = {
        // Arabic is cursive: every letter takes a contextual form, so the
        // shaped output must differ from a plain cmap lookup.
        {.name = "Arabic (Naskh)", .utf8 = "العربية مرحبا", .locale = "ar",
         .family = "Noto Naskh Arabic", .fclang = "ar", .rtl = true,
         .expect_contextual = true},
        {.name = "Arabic (Kufi)", .utf8 = "السلام عليكم", .locale = "ar",
         .family = "Noto Kufi Arabic", .fclang = "ar", .rtl = true,
         .expect_contextual = true},
        // Hebrew is RTL but not cursive; bidi must still resolve to level 1.
        {.name = "Hebrew", .utf8 = "שלום עולם", .locale = "he",
         .family = "Noto Sans Hebrew", .fclang = "he", .rtl = true,
         .expect_contextual = false},
        // CJK: no contextual substitution expected, but full-width metrics
        // and a large cmap exercise different code paths. fontconfig has no
        // zh-Hans orthography - its Simplified code is zh-cn.
        {.name = "Chinese", .utf8 = "你好世界文字", .locale = "zh-Hans",
         .family = "Noto Sans CJK SC", .fclang = "zh-cn", .rtl = false,
         .expect_contextual = false},
        {.name = "Japanese", .utf8 = "こんにちは漢字", .locale = "ja",
         .family = "Noto Sans CJK JP", .fclang = "ja", .rtl = false,
         .expect_contextual = false},
        // Devanagari reorders and forms conjuncts - heavy GSUB.
        {.name = "Devanagari", .utf8 = "नमस्ते हिन्दी", .locale = "hi",
         .family = "Noto Sans Devanagari", .fclang = "hi", .rtl = false,
         .expect_contextual = true},
    };

    for (const Sample& s : samples)
    {
        RunSample(factory, analyzer, s);
    }

    analyzer->Release();
    unknown->Release();

    if (g_ran == 0)
    {
        std::printf("\nno installed font covers any of these scripts; skipping\n");
        return kSkipExit;
    }
    if (g_skipped != 0)
    {
        // Loud, because a partly-skipped run that passes quietly reports
        // green for work it did not do.
        std::printf("\n%d of %d samples SKIPPED - no font on this machine covers them\n",
                    g_skipped, g_ran + g_skipped);
    }
    std::printf("\n%s\n", g_failures == 0 ? "Complex script shaping OK."
                                          : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
