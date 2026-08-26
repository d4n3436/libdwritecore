//+--------------------------------------------------------------------------
//
//  thread_test.cpp - concurrency stress for the bionic lock implementations.
//
//  The mutex, condition variable, rwlock and pthread_once in bionic-compat are
//  written from scratch on futexes, because bionic's opaque types are sized
//  differently from glibc's and cannot be forwarded. They are the least
//  battle-tested part of this project, so this hammers them through the paths
//  DWriteCore actually uses: the shared factory's internal caches, which are
//  guarded by exactly those primitives.
//
//  Every thread shares one factory, since a private factory per thread would
//  not exercise the contention this is meant to find.
//
//----------------------------------------------------------------------------

#include "dwrite_core.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "test_fonts.h"

namespace
{



std::u16string ToUtf16(const char* s)
{
    std::u16string out;
    for (; *s; ++s)
    {
        out.push_back(static_cast<char16_t>(*s));
    }
    return out;
}

std::atomic g_errors{0};
std::atomic<long> g_glyphs{0};

void Worker(IDWriteFactory* factory, const std::u16string& path, const int iterations, const int seed)
{
    for (int i = 0; i < iterations; ++i)
    {
        IDWriteFontFile* file = nullptr;
        if (FAILED(factory->CreateFontFileReference(path.c_str(), nullptr, &file)) || file == nullptr)
        {
            ++g_errors;
            return;
        }

        BOOL supported = FALSE;
        DWRITE_FONT_FILE_TYPE ft = DWRITE_FONT_FILE_TYPE_UNKNOWN;
        DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
        UINT32 faces = 0;
        (void)file->Analyze(&supported, &ft, &face_type, &faces);

        IDWriteFontFace* face = nullptr;
        if (FAILED(factory->CreateFontFace(face_type, 1, &file, 0, DWRITE_FONT_SIMULATIONS_NONE, &face)) || face == nullptr)
        {
            ++g_errors;
            file->Release();
            return;
        }

        // Vary the text per thread so the shaping and metric caches see
        // different keys under contention rather than one hot entry.
        char buf[16];
        (void)std::snprintf(buf, sizeof(buf), "Wg%d-Ax%d", seed, i % 7);
        std::vector<UINT32> cps;
        for (const char* p = buf; *p; ++p)
        {
            cps.push_back(static_cast<UINT32>(*p));
        }
        std::vector<UINT16> glyphs(cps.size());
        if (FAILED(face->GetGlyphIndices(cps.data(), static_cast<UINT32>(cps.size()), glyphs.data())))
        {
            ++g_errors;
        }

        DWRITE_FONT_METRICS fm = {};
        face->GetMetrics(&fm);

        const FLOAT em = 16.0f + static_cast<FLOAT>(seed % 5);
        std::vector adv(glyphs.size(), em * 0.5f);
        std::vector<DWRITE_GLYPH_OFFSET> offs(glyphs.size());

        DWRITE_GLYPH_RUN run = {};
        run.fontFace = face;
        run.fontEmSize = em;
        run.glyphCount = static_cast<UINT32>(glyphs.size());
        run.glyphIndices = glyphs.data();
        run.glyphAdvances = adv.data();
        run.glyphOffsets = offs.data();

        IDWriteGlyphRunAnalysis* analysis = nullptr;
        if (SUCCEEDED(factory->CreateGlyphRunAnalysis(&run, 1.0f, nullptr, DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL, DWRITE_MEASURING_MODE_NATURAL, 0.0f, 0.0f, &analysis)) &&
            analysis != nullptr)
        {
            RECT b = {};
            if (SUCCEEDED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &b)))
            {
                if (const int w = b.right - b.left, h = b.bottom - b.top; w > 0 && h > 0)
                {
                    std::vector<BYTE> tex(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0);
                    if (SUCCEEDED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &b, tex.data(), static_cast<UINT32>(tex.size()))))
                    {
                        g_glyphs += static_cast<long>(glyphs.size());
                    }
                    else
                    {
                        ++g_errors;
                    }
                }
            }
            analysis->Release();
        }
        else
        {
            ++g_errors;
        }

        face->Release();
        file->Release();
    }
}

} // namespace

int main(const int argc, char* const* argv)
{
    // Resolved through fontconfig, not from a list of paths: see test_fonts.h.
    // A machine with no outline font at all exits kSkipExit so ctest says
    // Skipped rather than counting this as a pass.
    std::string resolved;
    const char* font = argc > 1 ? argv[1] : nullptr;
    if (font == nullptr)
    {
        resolved = testfonts::AnyOutlineFont();
        if (resolved.empty())
        {
            std::printf("no outline font found through fontconfig; skipping\n");
            return kSkipExit;
        }
        font = resolved.c_str();
    }

    constexpr int threads = 8;
    constexpr int iterations = 40;
    std::printf("Concurrency stress: %d threads x %d iterations on one shared factory\n",
                threads, iterations);

    IUnknown* unknown = nullptr;
    const HRESULT hr = DWriteCoreCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, DWRITE_UUIDOF(IDWriteFactory), &unknown);
    if (FAILED(hr))
    {
        std::printf("FAIL  DWriteCoreCreateFactory hr=0x%08X\n", static_cast<unsigned>(hr));
        return 1;
    }
    auto* factory = reinterpret_cast<IDWriteFactory*>(unknown);

    const std::u16string path = ToUtf16(font);
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int i = 0; i < threads; ++i)
    {
        pool.emplace_back(Worker, factory, std::cref(path), iterations, i);
    }
    for (auto& t : pool)
    {
        t.join();
    }

    unknown->Release();

    std::printf("  rasterized %ld glyphs across %d threads, %d errors\n",
                g_glyphs.load(), threads, g_errors.load());
    if (g_errors.load() != 0)
    {
        std::printf("FAILURES PRESENT\n");
        return 1;
    }
    std::printf("Concurrency OK.\n");
    return 0;
}
