/* raster_probe.cpp - rasterize glyphs through DirectWrite and digest the result.
 *
 *   raster_probe <font> <glyphs> [options]
 *
 * One source, two builds. Compiled against libdwritecore it asks the Linux
 * implementation; cross-compiled for Windows it asks the system dwrite.dll in
 * a guest. Run both with the same arguments and diff the output, and any
 * disagreement between the two libraries shows up as a changed line naming the
 * glyph, with no browser, window system or screenshot involved.
 *
 * <glyphs> is a comma-separated list of glyph indices, and a-b is the closed
 * range: "43", "43,44,45", "3-99".
 *
 *   --em N             em size in pixels, fractional allowed   (default 16)
 *   --matrix a,b,c,d   the DWRITE_MATRIX linear part           (default 1,0,0,1)
 *   --offset x,y       the matrix translation, device pixels   (default 0,0)
 *   --mode NAME        aliased, gdi-classic, gdi-natural, natural, symmetric
 *   --measure NAME     natural, gdi-classic, gdi-natural
 *   --sideways         set isSideways on the glyph run
 *   --grid-fit NAME    default, disabled, enabled, quality
 *   --factory N        which CreateGlyphRunAnalysis overload, 1, 2 or 3
 *   --texture          print the coverage rows as well as the digest
 *
 * The default is the seven-argument IDWriteFactory overload, which is what
 * WebRender and Skia both call. --grid-fit implies --factory 2, since the
 * plain overload takes no grid-fit mode; --factory 3 takes the same arguments
 * through IDWriteFactory3 and a DWRITE_RENDERING_MODE1. The three overloads do
 * not always agree with each other, which is the reason for the switch.
 *
 * The digest is one line per glyph: the alpha texture bounds, the summed
 * coverage, and a CRC-32 of the ClearType 3x1 texture. Two runs whose digests
 * match rasterized identically, byte for byte.
 *
 * Matching the em size, matrix, offset and mode against a `raster glyph` line
 * from CLEARTYPE_LOG reproduces exactly what a browser asked for, and the
 * printed ink is the same number the log prints, which is how a run is checked
 * to be asking the browser's question.
 *
 * Building, Linux, against the library this repository builds:
 *
 *   g++ -std=c++20 -O1 -I include -I src tools/testing/raster_probe.cpp \
 *       -o raster_probe -L <build-dir> -ldwritecore -ldl \
 *       -Wl,-rpath,<build-dir>
 *
 * Building for a Windows guest, statically, since a mingw runtime dependency
 * the guest does not have fails at load with STATUS_DLL_NOT_FOUND and no
 * message:
 *
 *   x86_64-w64-mingw32-g++ -std=c++17 -O1 tools/testing/raster_probe.cpp \
 *       -o raster_probe.exe -ldwrite -static -static-libgcc -static-libstdc++
 *
 * run_raster_probe.sh does both and diffs them.
 */

#ifdef _WIN32
#  include <windows.h>
#  include <dwrite_3.h>
#  define PROBE_UUIDOF(iface) __uuidof(iface)
#else
#  include "dwrite_core.h"
#  include "dwritecore_shim.h"
#  define PROBE_UUIDOF(iface) DWRITE_UUIDOF(iface)
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

/* The font path in whatever wide string the platform's headers expect. */
#ifdef _WIN32
using PathString = std::wstring;
#else
using PathString = std::u16string;
#endif

PathString WidenPath(const char* s)
{
    PathString out;
    for (; *s != '\0'; ++s)
    {
        /* Font paths in this harness are ASCII on both sides. A byte above
         * 0x7f would need a real conversion, so it is refused in ParseArgs
         * rather than silently mangled here. */
        out.push_back(static_cast<PathString::value_type>(*s));
    }
    return out;
}

unsigned Crc32(const std::vector<BYTE>& v)
{
    unsigned c = 0xFFFFFFFFu;
    for (size_t i = 0; i < v.size(); ++i)
    {
        c ^= v[i];
        for (int k = 0; k < 8; ++k)
        {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

/* "43", "43,44,45" and "3-99" are all accepted; a-b is closed at both ends. */
bool ParseGlyphs(const char* spec, std::vector<UINT16>* out)
{
    const char* p = spec;
    while (*p != '\0')
    {
        char* end = nullptr;
        const unsigned long first = std::strtoul(p, &end, 10);
        if (end == p)
        {
            return false;
        }
        unsigned long last = first;
        if (*end == '-')
        {
            p = end + 1;
            last = std::strtoul(p, &end, 10);
            if (end == p || last < first)
            {
                return false;
            }
        }
        if (last > 0xFFFF)
        {
            return false;
        }
        for (unsigned long g = first; g <= last; ++g)
        {
            out->push_back(static_cast<UINT16>(g));
        }
        p = end;
        while (*p == ',')
        {
            ++p;
        }
    }
    return !out->empty();
}

bool ParseFloats(const char* spec, const int count, float* out)
{
    const char* p = spec;
    for (int i = 0; i < count; ++i)
    {
        char* end = nullptr;
        out[i] = std::strtof(p, &end);
        if (end == p)
        {
            return false;
        }
        p = end;
        if (i + 1 < count)
        {
            if (*p != ',')
            {
                return false;
            }
            ++p;
        }
    }
    return *p == '\0';
}

struct Named
{
    const char* name;
    int value;
};

bool Lookup(const Named* table, const char* name, int* out)
{
    for (const Named* n = table; n->name != nullptr; ++n)
    {
        if (std::strcmp(n->name, name) == 0)
        {
            *out = n->value;
            return true;
        }
    }
    return false;
}

const Named kRenderModes[] = {
    {"aliased", DWRITE_RENDERING_MODE_ALIASED},
    {"gdi-classic", DWRITE_RENDERING_MODE_GDI_CLASSIC},
    {"gdi-natural", DWRITE_RENDERING_MODE_GDI_NATURAL},
    {"natural", DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL},
    {"symmetric", DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC},
    {nullptr, 0},
};

const Named kMeasureModes[] = {
    {"natural", DWRITE_MEASURING_MODE_NATURAL},
    {"gdi-classic", DWRITE_MEASURING_MODE_GDI_CLASSIC},
    {"gdi-natural", DWRITE_MEASURING_MODE_GDI_NATURAL},
    {nullptr, 0},
};

const Named kGridFitModes[] = {
    {"default", DWRITE_GRID_FIT_MODE_DEFAULT},
    {"disabled", DWRITE_GRID_FIT_MODE_DISABLED},
    {"enabled", DWRITE_GRID_FIT_MODE_ENABLED},
    {nullptr, 0},
};

struct Options
{
    const char* font = nullptr;
    std::vector<UINT16> glyphs;
    float em = 16.0f;
    float matrix[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    float offset[2] = {0.0f, 0.0f};
    int render_mode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL;
    int measure_mode = DWRITE_MEASURING_MODE_NATURAL;
    int grid_fit = DWRITE_GRID_FIT_MODE_DEFAULT;
    int factory = 1;
    bool sideways = false;
    bool texture = false;
};

void Usage()
{
    std::printf(
        "usage: raster_probe <font> <glyphs> [options]\n"
        "  --em N  --matrix a,b,c,d  --offset x,y  --sideways  --texture\n"
        "  --mode aliased|gdi-classic|gdi-natural|natural|symmetric\n"
        "  --measure natural|gdi-classic|gdi-natural\n"
        "  --grid-fit default|disabled|enabled     --factory 1|2|3\n");
}

bool ParseArgs(const int argc, char* const* argv, Options* o)
{
    if (argc < 3)
    {
        return false;
    }
    o->font = argv[1];
    for (const char* p = o->font; *p != '\0'; ++p)
    {
        if ((static_cast<unsigned char>(*p) & 0x80u) != 0)
        {
            std::printf("font path must be ASCII\n");
            return false;
        }
    }
    if (!ParseGlyphs(argv[2], &o->glyphs))
    {
        std::printf("cannot read the glyph list \"%s\"\n", argv[2]);
        return false;
    }

    bool grid_fit_given = false;
    bool factory_given = false;
    for (int i = 3; i < argc; ++i)
    {
        const char* a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (std::strcmp(a, "--sideways") == 0)
        {
            o->sideways = true;
        }
        else if (std::strcmp(a, "--texture") == 0)
        {
            o->texture = true;
        }
        else if (v == nullptr)
        {
            std::printf("%s needs a value\n", a);
            return false;
        }
        else if (std::strcmp(a, "--em") == 0)
        {
            o->em = std::strtof(v, nullptr);
            ++i;
        }
        else if (std::strcmp(a, "--matrix") == 0)
        {
            if (!ParseFloats(v, 4, o->matrix))
            {
                std::printf("--matrix wants four comma-separated numbers\n");
                return false;
            }
            ++i;
        }
        else if (std::strcmp(a, "--offset") == 0)
        {
            if (!ParseFloats(v, 2, o->offset))
            {
                std::printf("--offset wants two comma-separated numbers\n");
                return false;
            }
            ++i;
        }
        else if (std::strcmp(a, "--mode") == 0)
        {
            if (!Lookup(kRenderModes, v, &o->render_mode))
            {
                std::printf("unknown rendering mode \"%s\"\n", v);
                return false;
            }
            ++i;
        }
        else if (std::strcmp(a, "--measure") == 0)
        {
            if (!Lookup(kMeasureModes, v, &o->measure_mode))
            {
                std::printf("unknown measuring mode \"%s\"\n", v);
                return false;
            }
            ++i;
        }
        else if (std::strcmp(a, "--grid-fit") == 0)
        {
            if (!Lookup(kGridFitModes, v, &o->grid_fit))
            {
                std::printf("unknown grid-fit mode \"%s\"\n", v);
                return false;
            }
            grid_fit_given = true;
            ++i;
        }
        else if (std::strcmp(a, "--factory") == 0)
        {
            o->factory = std::atoi(v);
            if (o->factory < 1 || o->factory > 3)
            {
                std::printf("--factory takes 1, 2 or 3\n");
                return false;
            }
            factory_given = true;
            ++i;
        }
        else
        {
            std::printf("unknown option \"%s\"\n", a);
            return false;
        }
    }
    /* The plain overload takes no grid-fit mode, so asking for one selects the
     * overload that does. An explicit --factory wins. */
    if (grid_fit_given && !factory_given)
    {
        o->factory = 2;
    }
    if (!(o->em > 0.0f))
    {
        std::printf("--em must be positive\n");
        return false;
    }
    return true;
}

IDWriteFactory* CreateFactory()
{
#ifdef _WIN32
    IDWriteFactory* factory = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   PROBE_UUIDOF(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&factory))))
    {
        return nullptr;
    }
    return factory;
#else
    IUnknown* unknown = nullptr;
    if (FAILED(DWriteCoreCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       PROBE_UUIDOF(IDWriteFactory), &unknown)) ||
        unknown == nullptr)
    {
        if (const char* why = DWriteCoreShimGetLastLoadError())
        {
            std::printf("%s\n", why);
        }
        return nullptr;
    }
    return reinterpret_cast<IDWriteFactory*>(unknown);
#endif
}

IDWriteFontFace* CreateFace(IDWriteFactory* factory, const char* path)
{
    const PathString wide = WidenPath(path);
    IDWriteFontFile* file = nullptr;
    if (FAILED(factory->CreateFontFileReference(wide.c_str(), nullptr, &file)) ||
        file == nullptr)
    {
        std::printf("cannot open %s\n", path);
        return nullptr;
    }
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32 face_count = 0;
    file->Analyze(&supported, &file_type, &face_type, &face_count);
    if (!supported)
    {
        std::printf("%s is not a font DirectWrite supports\n", path);
        file->Release();
        return nullptr;
    }
    IDWriteFontFace* face = nullptr;
    /* Face zero. A collection's other faces are a separate question, and one
     * a probe that took an index would answer only for whoever passed it. */
    if (FAILED(factory->CreateFontFace(face_type, 1, &file, 0,
                                       DWRITE_FONT_SIMULATIONS_NONE, &face)))
    {
        std::printf("cannot create a font face from %s\n", path);
        file->Release();
        return nullptr;
    }
    file->Release();
    return face;
}

HRESULT CreateAnalysis(IDWriteFactory* factory, const Options& o,
                       const DWRITE_GLYPH_RUN& run, const DWRITE_MATRIX& m,
                       IDWriteGlyphRunAnalysis** out)
{
    const auto render_mode = static_cast<DWRITE_RENDERING_MODE>(o.render_mode);
    const auto measure_mode = static_cast<DWRITE_MEASURING_MODE>(o.measure_mode);
    const auto grid_fit = static_cast<DWRITE_GRID_FIT_MODE>(o.grid_fit);

    if (o.factory == 1)
    {
        return factory->CreateGlyphRunAnalysis(&run, 1.0f, &m, render_mode,
                                               measure_mode, 0.0f, 0.0f, out);
    }
    if (o.factory == 2)
    {
        IDWriteFactory2* factory2 = nullptr;
        if (FAILED(factory->QueryInterface(PROBE_UUIDOF(IDWriteFactory2),
                                           reinterpret_cast<void**>(&factory2))) ||
            factory2 == nullptr)
        {
            std::printf("this DirectWrite has no IDWriteFactory2\n");
            return E_NOINTERFACE;
        }
        const HRESULT hr = factory2->CreateGlyphRunAnalysis(
            &run, &m, render_mode, measure_mode, grid_fit,
            DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE, 0.0f, 0.0f, out);
        factory2->Release();
        return hr;
    }
    IDWriteFactory3* factory3 = nullptr;
    if (FAILED(factory->QueryInterface(PROBE_UUIDOF(IDWriteFactory3),
                                       reinterpret_cast<void**>(&factory3))) ||
        factory3 == nullptr)
    {
        std::printf("this DirectWrite has no IDWriteFactory3\n");
        return E_NOINTERFACE;
    }
    const HRESULT hr = factory3->CreateGlyphRunAnalysis(
        &run, &m, static_cast<DWRITE_RENDERING_MODE1>(o.render_mode), measure_mode,
        grid_fit, DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE, 0.0f, 0.0f, out);
    factory3->Release();
    return hr;
}

void PrintTexture(const std::vector<BYTE>& texture, const int width, const int height,
                  const int top)
{
    for (int y = 0; y < height; ++y)
    {
        std::printf("  row %4d:", y + top);
        for (int x = 0; x < width; ++x)
        {
            const size_t i =
                (static_cast<size_t>(y) * static_cast<size_t>(width) +
                 static_cast<size_t>(x)) * 3;
            std::printf(" %3u,%3u,%3u", texture[i], texture[i + 1], texture[i + 2]);
        }
        std::printf("\n");
    }
}

/* One glyph, zero advance, zero offset, which is how both WebRender and Skia
 * build the single-glyph run they hand to CreateGlyphRunAnalysis. */
void ProbeGlyph(IDWriteFactory* factory, IDWriteFontFace* face, const Options& o,
                const UINT16 glyph, const DWRITE_MATRIX& m)
{
    const FLOAT advance = 0.0f;
    const DWRITE_GLYPH_OFFSET offset = {};
    DWRITE_GLYPH_RUN run = {};
    run.fontFace = face;
    run.fontEmSize = o.em;
    run.glyphCount = 1;
    run.glyphIndices = &glyph;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;
    run.isSideways = o.sideways ? TRUE : FALSE;

    IDWriteGlyphRunAnalysis* analysis = nullptr;
    const HRESULT hr = CreateAnalysis(factory, o, run, m, &analysis);
    if (FAILED(hr) || analysis == nullptr)
    {
        std::printf("glyph %5u  analysis failed hr=0x%08X\n", glyph,
                    static_cast<unsigned>(hr));
        return;
    }

    RECT bounds = {};
    if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds)))
    {
        std::printf("glyph %5u  no texture bounds\n", glyph);
        analysis->Release();
        return;
    }
    const int width = static_cast<int>(bounds.right - bounds.left);
    const int height = static_cast<int>(bounds.bottom - bounds.top);
    if (width <= 0 || height <= 0)
    {
        /* Empty bounds are what a space rasterizes to, and what a glyph that
         * cannot be drawn with ClearType at this size rasterizes to as well. */
        std::printf("glyph %5u  empty\n", glyph);
        analysis->Release();
        return;
    }

    std::vector<BYTE> texture(static_cast<size_t>(width) * static_cast<size_t>(height) * 3,
                              0);
    if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds,
                                            texture.data(),
                                            static_cast<UINT32>(texture.size()))))
    {
        std::printf("glyph %5u  no alpha texture\n", glyph);
        analysis->Release();
        return;
    }

    unsigned long ink = 0;
    for (size_t i = 0; i < texture.size(); ++i)
    {
        ink += texture[i];
    }
    std::printf("glyph %5u  %3dx%-3d at %4d,%-4d  ink %8lu  crc %08X\n", glyph, width,
                height, static_cast<int>(bounds.left), static_cast<int>(bounds.top), ink,
                Crc32(texture));
    if (o.texture)
    {
        PrintTexture(texture, width, height, static_cast<int>(bounds.top));
    }
    analysis->Release();
}

}  // namespace

int main(int argc, char* const* argv)
{
    Options o;
    if (!ParseArgs(argc, argv, &o))
    {
        Usage();
        return 2;
    }

    IDWriteFactory* factory = CreateFactory();
    if (factory == nullptr)
    {
        std::printf("cannot create a DirectWrite factory\n");
        return 1;
    }
    IDWriteFontFace* face = CreateFace(factory, o.font);
    if (face == nullptr)
    {
        return 1;
    }

    DWRITE_MATRIX m = {};
    m.m11 = o.matrix[0];
    m.m12 = o.matrix[1];
    m.m21 = o.matrix[2];
    m.m22 = o.matrix[3];
    m.dx = o.offset[0];
    m.dy = o.offset[1];

    for (size_t i = 0; i < o.glyphs.size(); ++i)
    {
        ProbeGlyph(factory, face, o, o.glyphs[i], m);
    }

    face->Release();
    return 0;
}
