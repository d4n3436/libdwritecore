// dwrite_oblique_probe.cpp - which families DWriteCore answers with a
// simulated oblique face, and which it leaves upright.
//
//   g++ -std=c++17 -O1 -o /tmp/oblique_probe \
//       tools/testing/dwrite_oblique_probe.cpp -Iinclude \
//       -Lbuild -ldwritecore -Wl,-rpath,build
//   /tmp/oblique_probe "MS Gothic" "Impact"
//
// Blink asks Skia for an italic face. Where the family has none, Skia on
// Windows gets back whatever DWriteCore matched: a face marked
// DWRITE_FONT_SIMULATIONS_OBLIQUE reports itself as italic, so Blink adds no
// skew of its own and the slant is DWriteCore's 20 degrees. An upright face
// instead gets Skia's 0.25 skew, which is 14 degrees. This says which families
// fall on which side.

#include "dwrite_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

const char* StyleName(const DWRITE_FONT_STYLE s)
{
    switch (s) {
        case DWRITE_FONT_STYLE_NORMAL: return "normal";
        case DWRITE_FONT_STYLE_OBLIQUE: return "oblique";
        case DWRITE_FONT_STYLE_ITALIC: return "italic";
        default: return "?";
    }
}

std::u16string Wide(const char* s)
{
    std::u16string out;
    while (*s != '\0') {
        out.push_back(static_cast<char16_t>(static_cast<unsigned char>(*s++)));
    }
    return out;
}

}  // namespace

int main(const int argc, char** argv)
{
    IUnknown* unk = nullptr;
    if (FAILED(DWriteCoreCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       DWRITE_UUIDOF(IDWriteFactory3), &unk)) ||
        unk == nullptr) {
        std::printf("no factory\n");
        return 1;
    }
    auto* factory = reinterpret_cast<IDWriteFactory3*>(unk);
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(factory->GetSystemFontCollection(&collection, FALSE)) || collection == nullptr) {
        std::printf("no system collection\n");
        return 1;
    }

    // The ink box of one glyph under a simulation, so the slant DirectWrite
    // bakes into a simulated face can be put beside the one the browser draws.
    auto InkBox = [&](IDWriteFontFace* face, const UINT16 glyph, const float size,
                      RECT* out) -> bool {
        FLOAT advance = 0.0f;
        DWRITE_GLYPH_OFFSET offset{};
        DWRITE_GLYPH_RUN run{};
        run.glyphCount = 1;
        run.glyphAdvances = &advance;
        run.fontFace = face;
        run.fontEmSize = size;
        run.glyphIndices = &glyph;
        run.glyphOffsets = &offset;
        IDWriteGlyphRunAnalysis* analysis = nullptr;
        if (FAILED(factory->CreateGlyphRunAnalysis(&run, 1.0f, nullptr,
                                                   DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                                   DWRITE_MEASURING_MODE_NATURAL, 0.0f, 0.0f,
                                                   &analysis)) ||
            analysis == nullptr) {
            return false;
        }
        const HRESULT hr = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, out);
        analysis->Release();
        return SUCCEEDED(hr);
    };

    for (int i = 1; i < argc; ++i) {
        const std::u16string name = Wide(argv[i]);
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(collection->FindFamilyName(name.c_str(), &index, &exists)) || !exists) {
            std::printf("%-16s not in the collection\n", argv[i]);
            continue;
        }
        IDWriteFontFamily* family = nullptr;
        if (FAILED(collection->GetFontFamily(index, &family)) || family == nullptr) {
            continue;
        }
        IDWriteFont* font = nullptr;
        if (SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
                                                   DWRITE_FONT_STRETCH_NORMAL,
                                                   DWRITE_FONT_STYLE_ITALIC, &font)) &&
            font != nullptr) {
            const DWRITE_FONT_SIMULATIONS sims = font->GetSimulations();
            std::printf("%-16s italic asked -> style %-7s sims %s%s  faces %u\n", argv[i],
                        StyleName(font->GetStyle()),
                        (sims & DWRITE_FONT_SIMULATIONS_BOLD) != 0 ? "BOLD " : "",
                        (sims & DWRITE_FONT_SIMULATIONS_OBLIQUE) != 0 ? "OBLIQUE" : "none",
                        family->GetFontCount());
            IDWriteFontFace* face = nullptr;
            if (SUCCEEDED(font->CreateFontFace(&face)) && face != nullptr) {
                const UINT32 cp = 'H';
                UINT16 glyph = 0;
                RECT plain{};
                RECT slanted{};
                if (SUCCEEDED(face->GetGlyphIndices(&cp, 1, &glyph)) && glyph != 0) {
                    IDWriteFont* upright = nullptr;
                    IDWriteFontFace* upface = nullptr;
                    if (SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
                                                               DWRITE_FONT_STRETCH_NORMAL,
                                                               DWRITE_FONT_STYLE_NORMAL,
                                                               &upright)) &&
                        upright != nullptr && SUCCEEDED(upright->CreateFontFace(&upface)) &&
                        upface != nullptr && InkBox(upface, glyph, 400.0f, &plain) &&
                        InkBox(face, glyph, 400.0f, &slanted)) {
                        const double dw = static_cast<double>(slanted.right - slanted.left) -
                                          static_cast<double>(plain.right - plain.left);
                        const double h = static_cast<double>(plain.bottom - plain.top);
                        std::printf("%-16s   simulated slant %.2f deg (ink %ld -> %ld over %ld)\n",
                                    argv[i], h > 0 ? std::atan(dw / h) * 57.29577951 : 0.0,
                                    static_cast<long>(plain.right - plain.left),
                                    static_cast<long>(slanted.right - slanted.left),
                                    static_cast<long>(h));
                    }
                    if (upface != nullptr) { upface->Release(); }
                    if (upright != nullptr) { upright->Release(); }
                }
                face->Release();
            }
            font->Release();
        }
        family->Release();
    }
    return 0;
}
