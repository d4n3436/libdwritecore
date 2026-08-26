//+--------------------------------------------------------------------------
//
//  cleartype_test.cpp - end-to-end exercise of the ClearType pipeline.
//
//  This drives the real libdwritecore.so through the DirectWrite interfaces,
//  so a pass means three things at once: the bionic compatibility layer loads
//  an Android build on glibc, the calls reach the slots the headers predict,
//  and the rasterizer actually produces ClearType coverage.
//
//  Pass a font path as argv[1] to override the default.
//
//----------------------------------------------------------------------------

#include "dwrite_core.h"
#include "dwritecore_shim.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_fonts.h"



namespace
{


int g_failures = 0;


bool Check(const char* what, const HRESULT hr)
{
    if (FAILED(hr))
    {
        std::printf("FAIL  %-38s hr=0x%08X\n", what, static_cast<unsigned>(hr));
        ++g_failures;
        return false;
    }
    std::printf("ok    %-38s\n", what);
    return true;
}

std::u16string ToUtf16(const char* s)
{
    std::u16string out;
    for (; *s; ++s)
    {
        out.push_back(static_cast<char16_t>(*s));
    }
    return out;
}

} // namespace

int main(int argc, char* const* argv)
{
    // Resolved through fontconfig, not from a list of paths: see test_fonts.h.
    // A machine with no outline font at all exits kSkipExit so ctest says
    // Skipped rather than counting this as a pass.
    std::string resolved;
    const char* font_path = argc > 1 ? argv[1] : nullptr;
    if (font_path == nullptr)
    {
        resolved = testfonts::AnyOutlineFont();
        if (resolved.empty())
        {
            std::printf("no outline font found through fontconfig; skipping\n");
            return kSkipExit;
        }
        font_path = resolved.c_str();
    }
    std::printf("DWriteCore ClearType pipeline test\n  font: %s\n\n", font_path);

    // --- factory -----------------------------------------------------------
    IUnknown* unknown = nullptr;
    const HRESULT hr = DWriteCoreCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, DWRITE_UUIDOF(IDWriteFactory), &unknown);
    if (!Check("DWriteCoreCreateFactory", hr))
    {
        if (const char* err = DWriteCoreShimGetLastLoadError())
        {
            std::printf("%s\n", err);
        }
        return 1;
    }
    auto* factory = reinterpret_cast<IDWriteFactory*>(unknown);

    // --- font file and face ------------------------------------------------
    const std::u16string wpath = ToUtf16(font_path);
    IDWriteFontFile* file = nullptr;
    if (!Check("CreateFontFileReference",
               factory->CreateFontFileReference(wpath.c_str(), nullptr, &file)))
    {
        return 1;
    }

    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32 face_count = 0;
    Check("IDWriteFontFile::Analyze",
          file->Analyze(&supported, &file_type, &face_type, &face_count));
    std::printf("      supported=%d fileType=%d faceType=%d faces=%u\n",
                static_cast<int>(supported), static_cast<int>(file_type), static_cast<int>(face_type), face_count);
    if (!supported)
    {
        std::printf("FAIL  font not supported by DWriteCore\n");
        return 1;
    }

    IDWriteFontFace* face = nullptr;
    if (!Check("CreateFontFace",
               factory->CreateFontFace(face_type, 1, &file, 0, DWRITE_FONT_SIMULATIONS_NONE, &face)))
    {
        return 1;
    }

    DWRITE_FONT_METRICS metrics = {};
    face->GetMetrics(&metrics);
    std::printf("      unitsPerEm=%u ascent=%u descent=%u\n",
                metrics.designUnitsPerEm, metrics.ascent, metrics.descent);
    if (metrics.designUnitsPerEm == 0)
    {
        std::printf("FAIL  font metrics are empty\n");
        ++g_failures;
    }

    // --- glyphs ------------------------------------------------------------
    const auto* text = "Hamburgefonstiv";
    std::vector<UINT32> codepoints;
    for (const char* p = text; *p; ++p)
    {
        codepoints.push_back(static_cast<UINT32>(*p));
    }
    std::vector<UINT16> glyphs(codepoints.size());
    if (!Check("GetGlyphIndices",
               face->GetGlyphIndices(codepoints.data(), static_cast<UINT32>(codepoints.size()), glyphs.data())))
    {
        return 1;
    }

    UINT32 mapped = 0;
    for (const UINT16 g : glyphs)
    {
        mapped += g != 0;
    }
    std::printf("      %u/%zu characters mapped to glyphs\n", mapped, glyphs.size());
    if (mapped != glyphs.size())
    {
        std::printf("FAIL  some characters did not map\n");
        ++g_failures;
    }

    constexpr FLOAT em_size = 24.0f;
    std::vector advances(glyphs.size(), 0.0f);
    std::vector<DWRITE_GLYPH_OFFSET> offsets(glyphs.size());

    std::vector<DWRITE_GLYPH_METRICS> gm(glyphs.size());
    if (SUCCEEDED(face->GetDesignGlyphMetrics(glyphs.data(), static_cast<UINT32>(glyphs.size()), gm.data(), FALSE)))
    {
        for (size_t i = 0; i < glyphs.size(); ++i)
        {
            advances[i] = static_cast<FLOAT>(gm[i].advanceWidth) * em_size /
                      static_cast<FLOAT>(metrics.designUnitsPerEm);
        }
    }

    DWRITE_GLYPH_RUN run = {};
    run.fontFace = face;
    run.fontEmSize = em_size;
    run.glyphCount = static_cast<UINT32>(glyphs.size());
    run.glyphIndices = glyphs.data();
    run.glyphAdvances = advances.data();
    run.glyphOffsets = offsets.data();
    run.isSideways = FALSE;
    run.bidiLevel = 0;

    // --- ClearType rasterization ------------------------------------------
    IDWriteGlyphRunAnalysis* analysis = nullptr;
    if (!Check("CreateGlyphRunAnalysis",
               factory->CreateGlyphRunAnalysis(&run, 1.0f, nullptr, DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL, DWRITE_MEASURING_MODE_NATURAL, 0.0f, 0.0f, &analysis)))
    {
        return 1;
    }

    RECT bounds = {};
    if (!Check("GetAlphaTextureBounds",
               analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds)))
    {
        return 1;
    }

    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    std::printf("      texture bounds %ld,%ld..%ld,%ld  (%dx%d)\n",
                static_cast<long>(bounds.left), static_cast<long>(bounds.top),
                static_cast<long>(bounds.right), static_cast<long>(bounds.bottom), w, h);
    if (w <= 0 || h <= 0)
    {
        std::printf("FAIL  empty ClearType texture bounds\n");
        return 1;
    }

    // ClearType is 3 bytes per pixel: one coverage value per RGB subpixel.
    std::vector<BYTE> texture(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0);
    if (!Check("CreateAlphaTexture",
               analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, texture.data(), static_cast<UINT32>(texture.size()))))
    {
        return 1;
    }

    size_t nonzero = 0;
    unsigned peak = 0;
    for (const BYTE b : texture)
    {
        nonzero += b != 0;
        peak = b > peak ? b : peak;
    }
    std::printf("      coverage: %zu/%zu bytes set, peak %u\n",
                nonzero, texture.size(), peak);
    if (nonzero == 0)
    {
        std::printf("FAIL  texture is blank - nothing was rasterized\n");
        ++g_failures;
    }

    FLOAT gamma = 0, contrast = 0, level = 0;
    IDWriteRenderingParams* params = nullptr;
    if (SUCCEEDED(factory->CreateRenderingParams(&params)) && params)
    {
        Check("GetAlphaBlendParams",
              analysis->GetAlphaBlendParams(params, &gamma, &contrast, &level));
        std::printf("      gamma=%.3f enhancedContrast=%.3f clearTypeLevel=%.3f\n",
                    static_cast<double>(gamma), static_cast<double>(contrast),
            static_cast<double>(level));
        params->Release();
    }

    // --- show it ------------------------------------------------------------
    std::printf("\n  rendered (subpixels averaged per pixel):\n");
    for (int y = 0; y < h && y < 40; ++y)
    {
        std::printf("      ");
        for (int x = 0; x < w && x < 150; ++x)
        {
            const auto* ramp = " .:-=+*#%@";
            const BYTE* px = &texture[(static_cast<size_t>(y) * static_cast<size_t>(w) +
                                      static_cast<size_t>(x)) * 3];
            const unsigned avg = static_cast<unsigned>(px[0] + px[1] + px[2]) / 3;
            std::putchar(ramp[avg * 9 / 255]);
        }
        std::putchar('\n');
    }

    analysis->Release();
    face->Release();
    file->Release();
    unknown->Release();

    std::printf("\n%s\n", g_failures == 0 ? "ClearType pipeline OK." : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
