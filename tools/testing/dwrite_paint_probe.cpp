// dwrite_paint_probe.cpp - dump DWriteCore's COLRv1 paint tree for one glyph.
//
//   g++ -std=c++17 -O1 -o /tmp/paint_probe \
//       tools/testing/dwrite_paint_probe.cpp -Iinclude \
//       -Lbuild -ldwritecore -Wl,-rpath,build
//   /tmp/paint_probe <font.ttf> <glyph-id>
//
// Prints the layers, transforms, gradient stops and resolved colors that
// SkScalerContext_DW walks through IDWritePaintReader, for comparison against
// what the painter hook logs from skrifa's tree on the Linux side.
//
// Coordinates are in em here, so a radius reads as a fraction of 1. The Linux
// side reports font units, so multiply by units per em to compare.

#include "dwrite_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Only forward declared in dwrite_3.h, since it belongs to Direct2D.
struct D2D1_GRADIENT_STOP
{
    FLOAT position;
    D2D_COLOR_F color;
};

std::vector<unsigned char> ReadFile(const char* path)
{
    std::vector<unsigned char> out;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    out.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(out.data(), 1, out.size(), f) != out.size()) {
        out.clear();
    }
    std::fclose(f);
    return out;
}

const char* TypeName(const DWRITE_PAINT_TYPE t)
{
    switch (t) {
        case DWRITE_PAINT_TYPE_NONE: return "none";
        case DWRITE_PAINT_TYPE_LAYERS: return "layers";
        case DWRITE_PAINT_TYPE_SOLID_GLYPH: return "solid-glyph";
        case DWRITE_PAINT_TYPE_SOLID: return "solid";
        case DWRITE_PAINT_TYPE_LINEAR_GRADIENT: return "linear";
        case DWRITE_PAINT_TYPE_RADIAL_GRADIENT: return "radial";
        case DWRITE_PAINT_TYPE_SWEEP_GRADIENT: return "sweep";
        case DWRITE_PAINT_TYPE_GLYPH: return "glyph";
        case DWRITE_PAINT_TYPE_COLOR_GLYPH: return "color-glyph";
        case DWRITE_PAINT_TYPE_TRANSFORM: return "transform";
        case DWRITE_PAINT_TYPE_COMPOSITE: return "composite";
        default: return "?";
    }
}

// The stops of the gradient the reader is currently on. Colors come back
// already resolved through the palette, which is what Skia's Windows side
// hands the shader, so they compare against the Linux side's palette lookup.
void PrintStops(IDWritePaintReader* reader, const UINT32 count)
{
    if (count == 0) {
        return;
    }
    std::vector<D2D1_GRADIENT_STOP> stops(count);
    if (FAILED(reader->GetGradientStops(0, count,
                                        reinterpret_cast<::D2D1_GRADIENT_STOP*>(stops.data())))) {
        std::printf(" (no stops)");
        return;
    }
    for (const auto& stop : stops) {
        std::printf("  %.6f/rgba(%.6f,%.6f,%.6f,%.6f)", stop.position, stop.color.r, stop.color.g,
                    stop.color.b, stop.color.a);
    }
}

// Depth-first, the same order Skia's drawColorV1Paint walks it.
void Walk(IDWritePaintReader* reader, const DWRITE_PAINT_ELEMENT& element, const int depth,
          const float upem)
{
    const std::string pad(static_cast<size_t>(depth) * 2, ' ');
    std::printf("%s%s", pad.c_str(), TypeName(element.paintType));
    switch (element.paintType) {
        case DWRITE_PAINT_TYPE_GLYPH:
            std::printf(" glyph=%u", element.paint.glyph.glyphIndex);
            break;
        case DWRITE_PAINT_TYPE_SOLID_GLYPH:
            std::printf(" glyph=%u", element.paint.solidGlyph.glyphIndex);
            break;
        case DWRITE_PAINT_TYPE_RADIAL_GRADIENT: {
            const auto& r = element.paint.radialGradient;
            std::printf(" stops=%u c0=(%.6f,%.6f) r0=%.6f c1=(%.6f,%.6f) r1=%.6f",
                        r.gradientStopCount, r.x0 * upem, r.y0 * upem, r.radius0 * upem,
                        r.x1 * upem, r.y1 * upem, r.radius1 * upem);
            PrintStops(reader, r.gradientStopCount);
            break;
        }
        case DWRITE_PAINT_TYPE_LINEAR_GRADIENT: {
            const auto& l = element.paint.linearGradient;
            std::printf(" stops=%u", l.gradientStopCount);
            PrintStops(reader, l.gradientStopCount);
            break;
        }
        case DWRITE_PAINT_TYPE_SWEEP_GRADIENT: {
            const auto& w = element.paint.sweepGradient;
            std::printf(" stops=%u", w.gradientStopCount);
            PrintStops(reader, w.gradientStopCount);
            break;
        }
        case DWRITE_PAINT_TYPE_SOLID: {
            const auto& c = element.paint.solid.value;
            std::printf(" rgba=(%.6f,%.6f,%.6f,%.6f)", c.r, c.g, c.b, c.a);
            break;
        }
        case DWRITE_PAINT_TYPE_TRANSFORM: {
            const auto& t = element.paint.transform;
            std::printf(" [%.6f %.6f %.6f %.6f %.6f %.6f]", t.m11, t.m12, t.m21, t.m22,
                        t.dx * upem, t.dy * upem);
            break;
        }
        case DWRITE_PAINT_TYPE_LAYERS:
            std::printf(" children=%u", element.paint.layers.childCount);
            break;
        default:
            break;
    }
    std::printf("\n");

    // Only these carry children. Descending into a gradient or a solid walks
    // into the next glyph's tree and prints it as if it were nested.
    UINT32 children = 0;
    switch (element.paintType) {
        case DWRITE_PAINT_TYPE_LAYERS: children = element.paint.layers.childCount; break;
        case DWRITE_PAINT_TYPE_GLYPH:
        case DWRITE_PAINT_TYPE_COLOR_GLYPH:
        case DWRITE_PAINT_TYPE_TRANSFORM: children = 1; break;
        case DWRITE_PAINT_TYPE_COMPOSITE: children = 2; break;
        default: children = 0; break;
    }
    if (children == 0) {
        return;
    }
    DWRITE_PAINT_ELEMENT child;
    if (FAILED(reader->MoveToFirstChild(&child))) {
        return;
    }
    Walk(reader, child, depth + 1, upem);
    for (UINT32 i = 1; i < children; ++i) {
        if (FAILED(reader->MoveToNextSibling(&child))) {
            break;
        }
        Walk(reader, child, depth + 1, upem);
    }
    (void)reader->MoveToParent();
}

}  // namespace

int main(const int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: %s <font.ttf> <glyph-id>\n", argv[0]);
        return 2;
    }
    const std::vector<unsigned char> bytes = ReadFile(argv[1]);
    if (bytes.empty()) {
        std::printf("cannot read %s\n", argv[1]);
        return 1;
    }
    const auto glyph = static_cast<UINT32>(std::atoi(argv[2]));

    IUnknown* unk = nullptr;
    if (FAILED(DWriteCoreCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,
                                       DWRITE_UUIDOF(IDWriteFactory5), &unk)) ||
        unk == nullptr) {
        std::printf("no factory\n");
        return 1;
    }
    auto* factory = reinterpret_cast<IDWriteFactory5*>(unk);

    IDWriteFontFile* file = nullptr;
    IDWriteInMemoryFontFileLoader* loader = nullptr;
    if (FAILED(factory->CreateInMemoryFontFileLoader(&loader)) ||
        FAILED(factory->RegisterFontFileLoader(loader)) ||
        FAILED(loader->CreateInMemoryFontFileReference(factory, bytes.data(),
                                                       static_cast<UINT32>(bytes.size()), nullptr,
                                                       &file))) {
        std::printf("cannot load the font\n");
        return 1;
    }
    IDWriteFontFace* face = nullptr;
    if (FAILED(factory->CreateFontFace(DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &file, 0,
                                       DWRITE_FONT_SIMULATIONS_NONE, &face))) {
        std::printf("cannot make a face\n");
        return 1;
    }
    IDWriteFontFace7* face7 = nullptr;
    if (FAILED(face->QueryInterface(DWRITE_UUIDOF(IDWriteFontFace7),
                                    reinterpret_cast<void**>(&face7))) ||
        face7 == nullptr) {
        std::printf("no IDWriteFontFace7, so no paint reader\n");
        return 1;
    }
    DWRITE_FONT_METRICS fm{};
    face->GetMetrics(&fm);
    const auto upem = static_cast<float>(fm.designUnitsPerEm);

    IDWritePaintReader* reader = nullptr;
    if (FAILED(face7->CreatePaintReader(DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE,
                                        DWRITE_PAINT_FEATURE_LEVEL_COLR_V1, &reader)) ||
        reader == nullptr) {
        std::printf("no paint reader for this font\n");
        return 1;
    }
    DWRITE_PAINT_ELEMENT root{};
    D2D_RECT_F clip{};
    DWRITE_PAINT_ATTRIBUTES attrs{};
    if (FAILED(reader->SetCurrentGlyph(glyph, &root, &clip, &attrs))) {
        std::printf("cannot set glyph %u\n", glyph);
        return 1;
    }
    std::printf("glyph %u, upem %.0f, clip box em (%.6f %.6f %.6f %.6f)\n", glyph, upem, clip.left,
                clip.top, clip.right, clip.bottom);
    std::printf("clip box in font units (%.3f %.3f %.3f %.3f)\n", clip.left * upem,
                clip.top * upem, clip.right * upem, clip.bottom * upem);
    Walk(reader, root, 0, upem);
    return 0;
}
