// Style inspections left as they are: the shapes they suggest either read
// worse against the sources being mirrored, or would change which overload
// is chosen if one were ever added.
// ReSharper disable RadGlobal

#include "dwrite_raster.h"

// Skia compares these to zero exactly. A tolerance check would take a
// different branch than Windows does on the same input.
#pragma GCC diagnostic ignored "-Wfloat-equal"

#include "compat.h"
#include "dwrite_3.h"
#include "dwrite_core.h"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include <dlfcn.h>

namespace dwrite_raster {
namespace {

void Say(const char* what)
{
    (void)std::fprintf(stderr, "chromium-patch: dwrite: %s\n", what);
}

using PfnCreateFactory = HRESULT (*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);

struct Dwrite
{
    IDWriteFactory5* factory5 = nullptr;
    IDWriteFactory2* factory2 = nullptr;
    IDWriteInMemoryFontFileLoader* loader = nullptr;
    bool tried = false;
    bool ok = false;
};

std::mutex g_mutex;
Dwrite g_dw;

// Loading the library and building a factory are deliberately separate, and
// happen on opposite sides of the fork.
//
// The dlopen has to be early: a renderer is forked from the zygote and can no
// longer open a file, so the mapping must already exist. The factory has to
// be late: DirectWrite starts threads and takes locks of its own while
// initializing, and only the forking thread survives a fork, so a factory
// built in the zygote leaves the child holding locks nobody will ever
// release, which hangs the renderer.
//
// Splitting them keeps both constraints: the file is mapped before the
// sandbox closes, and every DirectWrite object is created in the process that
// uses it. Nothing needs the filesystem after the dlopen, because the fonts
// come from memory.
PfnCreateFactory g_create = nullptr;

bool PreloadLibrary()
{
    if (g_create != nullptr) {
        return true;
    }
    void* handle = dlopen("libdwritecore.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* why = dlerror();
        (void)std::fprintf(stderr, "chromium-patch: dwrite: libdwritecore.so did not load (%s); "
                                   "glyphs stay with Fontations\n",
                           why != nullptr ? why : "no reason given");
        return false;
    }
    g_create = reinterpret_cast<PfnCreateFactory>(dlsym(handle, "DWriteCoreCreateFactory"));
    if (g_create == nullptr) {
        Say("libdwritecore.so has no DWriteCoreCreateFactory");
        return false;
    }
    return true;
}

bool EnsureFactory()
{
    if (g_dw.tried) {
        return g_dw.ok;
    }
    g_dw.tried = true;
    if (!PreloadLibrary()) {
        return false;
    }
    const PfnCreateFactory create = g_create;
    IUnknown* unknown = nullptr;
    if (FAILED(create(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory5), &unknown)) ||
        unknown == nullptr) {
        Say("no IDWriteFactory5");
        return false;
    }
    g_dw.factory5 = static_cast<IDWriteFactory5*>(unknown);
    if (FAILED(g_dw.factory5->QueryInterface(__uuidof(IDWriteFactory2),
                                             reinterpret_cast<void**>(&g_dw.factory2)))) {
        g_dw.factory2 = nullptr;
    }
    if (FAILED(g_dw.factory5->CreateInMemoryFontFileLoader(&g_dw.loader)) ||
        g_dw.loader == nullptr) {
        Say("no in-memory font file loader");
        return false;
    }
    if (FAILED(g_dw.factory5->RegisterFontFileLoader(g_dw.loader))) {
        Say("the in-memory loader would not register");
        return false;
    }
    g_dw.ok = true;
    Say("DWriteCore ready");
    return true;
}

// One font face per typeface, built from the bytes typeface_bridge rebuilt,
// so DirectWrite never touches the filesystem. That is what makes this work in
// a sandboxed renderer.
IDWriteFontFace* FaceFor(const void* typeface, const std::vector<uint8_t>& bytes,
                         const uint32_t face_index)
{
    static std::unordered_map<const void*, IDWriteFontFace*> faces;
    if (const auto it = faces.find(typeface); it != faces.end()) {
        return it->second;
    }
    // A collection carries 'ttcf' where a single face carries its SFNT
    // version, and DirectWrite has to be told which of the two it was given
    // before face_index means anything.
    const bool collection = bytes.size() >= 4 && bytes[0] == 't' && bytes[1] == 't' &&
                            bytes[2] == 'c' && bytes[3] == 'f';
    const DWRITE_FONT_FACE_TYPE type = collection ? DWRITE_FONT_FACE_TYPE_TRUETYPE_COLLECTION
                                                  : DWRITE_FONT_FACE_TYPE_TRUETYPE;
    IDWriteFontFace* face = nullptr;
    IDWriteFontFile* file = nullptr;
    if (!bytes.empty() &&
        SUCCEEDED(g_dw.loader->CreateInMemoryFontFileReference(
            g_dw.factory5, bytes.data(), static_cast<UINT32>(bytes.size()), nullptr, &file)) &&
        file != nullptr) {
        if (FAILED(g_dw.factory5->CreateFontFace(type, 1, &file, face_index,
                                                 DWRITE_FONT_SIMULATIONS_NONE, &face))) {
            face = nullptr;
        }
        file->Release();
    }
    faces[typeface] = face;
    if (face == nullptr) {
        Say("could not build a font face from the rebuilt bytes");
    }
    return face;
}

uint8_t ApplyLut(const uint8_t v, const uint8_t* table)
{
    return table != nullptr ? table[v] : v;
}

size_t RowBytes(const skia_abi::Glyph& g)
{
    switch (g.mask_format) {
        case skia_abi::kBW: return (static_cast<size_t>(g.width) + 7) >> 3;
        case skia_abi::kLCD16: return static_cast<size_t>(g.width) * 2;
        case skia_abi::kARGB32: return static_cast<size_t>(g.width) * 4;
        default: return g.width;
    }
}

// src/ports/SkScalerContext_win_dw.cpp, isLCD - the Rec's mask format, not
// the glyph's, is what chooses the conversion.
bool IsLcd(const skia_abi::Rec& rec)
{
    return rec.mask_format == skia_abi::kLCD16;
}

uint16_t Pack888ToRGB16(const uint8_t r, const uint8_t g, const uint8_t b)
{
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

}  // namespace

bool Preload()
{
    const std::lock_guard lock(g_mutex);
    return PreloadLibrary();
}

bool Available()
{
    const std::lock_guard lock(g_mutex);
    return EnsureFactory();
}

// SkScalerContext_DW::generateDWMetrics. The run is one glyph with a zero
// advance at the origin and the sub-pixel position in the transform, exactly
// as the raster path builds it, so the box asked for here is the box the
// glyph is later drawn into.
bool GlyphBounds(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const skia_abi::Glyph& glyph, const skia_abi::Rec& rec,
                 const windows_path::Decision& decision,
                 const windows_path::RenderingMode rendering_mode,
                 const windows_path::TextureType texture_type, int* left, int* top,
                 int* right, int* bottom, const uint32_t face_index)
{
    if (left == nullptr || top == nullptr || right == nullptr || bottom == nullptr) {
        return false;
    }
    const std::lock_guard lock(g_mutex);
    if (!EnsureFactory()) {
        return false;
    }
    IDWriteFontFace* face = FaceFor(typeface, font_bytes, face_index);
    if (face == nullptr) {
        return false;
    }

    DWRITE_MATRIX transform{};
    float scale_y = 0;
    windows_path::Matrix2x2 remaining;
    if (!windows_path::ComputeMatrices(rec, &scale_y, &remaining)) {
        return false;
    }
    transform.m11 = remaining.scale_x;
    transform.m12 = remaining.skew_y;
    transform.m21 = remaining.skew_x;
    transform.m22 = remaining.scale_y;
    transform.dx = static_cast<float>(glyph.SubX()) / 4.0f;
    transform.dy = static_cast<float>(glyph.SubY()) / 4.0f;

    FLOAT advance = 0.0f;
    UINT16 index = glyph.GlyphId();
    DWRITE_GLYPH_OFFSET offset{};
    DWRITE_GLYPH_RUN run{};
    run.glyphCount = 1;
    run.glyphAdvances = &advance;
    run.fontFace = face;
    run.fontEmSize = decision.text_size_render;
    run.bidiLevel = 0;
    run.glyphIndices = &index;
    run.isSideways = FALSE;
    run.glyphOffsets = &offset;

    IDWriteGlyphRunAnalysis* analysis = nullptr;
    HRESULT hr = E_FAIL;
    if (g_dw.factory2 != nullptr &&
        (decision.grid_fit_mode == windows_path::kGridFitDisabled ||
         decision.anti_alias_mode == windows_path::kAntiAliasGrayscale)) {
        hr = g_dw.factory2->CreateGlyphRunAnalysis(
            &run, &transform, static_cast<DWRITE_RENDERING_MODE>(rendering_mode),
            static_cast<DWRITE_MEASURING_MODE>(decision.measuring_mode),
            static_cast<DWRITE_GRID_FIT_MODE>(decision.grid_fit_mode),
            static_cast<DWRITE_TEXT_ANTIALIAS_MODE>(decision.anti_alias_mode), 0.0f, 0.0f,
            &analysis);
    } else if (g_dw.factory5 != nullptr) {
        hr = g_dw.factory5->CreateGlyphRunAnalysis(
            &run, 1.0f, &transform, static_cast<DWRITE_RENDERING_MODE>(rendering_mode),
            static_cast<DWRITE_MEASURING_MODE>(decision.measuring_mode), 0.0f, 0.0f,
            &analysis);
    }
    if (FAILED(hr) || analysis == nullptr) {
        return false;
    }

    RECT bbox{};
    hr = analysis->GetAlphaTextureBounds(static_cast<DWRITE_TEXTURE_TYPE>(texture_type),
                                         &bbox);
    analysis->Release();
    if (FAILED(hr)) {
        return false;
    }
    // GetAlphaTextureBounds succeeds but sometimes returns an empty rect for
    // small but not quite zero and large but not really large glyphs.
    if (bbox.left >= bbox.right || bbox.top >= bbox.bottom) {
        return false;
    }
    *left = bbox.left;
    *top = bbox.top;
    *right = bbox.right;
    *bottom = bbox.bottom;
    return true;
}

// SkScalerContext_DW::generateMetrics' advance, which is what decides where
// the next glyph goes. A GDI measuring mode takes it from
// GetGdiCompatibleGlyphMetrics and rounds the result; anything else takes it
// from GetDesignGlyphMetrics and leaves it fractional.
//
// The design-metrics branch is the one an ordinary page takes, and it is
// linear in the text size. That is why Windows advances step by exactly the
// same amount per pixel of size and a grid-fitted scaler's do not.
bool GlyphAdvance(const void* typeface, const std::vector<uint8_t>& font_bytes,
                  const uint16_t glyph_id, const skia_abi::Rec& rec,
                  const windows_path::Decision& decision, float* advance_x, float* advance_y,
                  const uint32_t face_index)
{
    if (advance_x == nullptr || advance_y == nullptr) {
        return false;
    }
    const std::lock_guard lock(g_mutex);
    if (!EnsureFactory()) {
        return false;
    }
    IDWriteFontFace* face = FaceFor(typeface, font_bytes, face_index);
    if (face == nullptr) {
        return false;
    }

    DWRITE_GLYPH_METRICS gm{};
    const UINT16 id = glyph_id;
    const bool gdi = decision.measuring_mode == windows_path::kMeasureGdiClassic ||
                     decision.measuring_mode == windows_path::kMeasureGdiNatural;
    HRESULT hr;
    if (gdi) {
        hr = face->GetGdiCompatibleGlyphMetrics(
            decision.text_size_measure, 1.0f, nullptr,
            decision.measuring_mode == windows_path::kMeasureGdiNatural ? TRUE : FALSE,
            &id, 1, &gm);
    } else {
        hr = face->GetDesignGlyphMetrics(&id, 1, &gm);
    }
    if (FAILED(hr)) {
        return false;
    }

    DWRITE_FONT_METRICS dwfm{};
    face->GetMetrics(&dwfm);
    if (dwfm.designUnitsPerEm == 0) {
        return false;
    }

    float x = decision.text_size_measure * static_cast<float>(gm.advanceWidth) /
              static_cast<float>(dwfm.designUnitsPerEm);
    if (gdi) {
        // DirectWrite produced 'compatible' metrics, but while close, the end
        // result is not always an integer as it would be with GDI.
        x = std::round(x);
    }

    // The advance is then mapped through sA, the same matrix the analysis
    // gets. That is the identity for ordinary axis-aligned text; for anything
    // stretched, skewed or rotated an unmapped advance puts every following
    // glyph in the wrong place.
    float scale_y = 0;
    windows_path::Matrix2x2 remaining;
    windows_path::ComputeMatrices(rec, &scale_y, &remaining);
    *advance_x = remaining.scale_x * x;
    *advance_y = remaining.skew_y * x;
    return true;
}

// SkScalerContext_DW::generateFontMetrics. Every field is the design value
// scaled by fTextSizeRender over the design units per em, with ascent
// negated.
//
// Blink then rounds ascent, descent and leading separately before adding them
// (SimpleFontData::PlatformInit, SetLineSpacing), so a fraction of a pixel of
// disagreement here becomes a whole pixel of line height.
bool FontMetrics(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const windows_path::Decision& decision, void* sk_font_metrics,
                 const uint32_t face_index)
{
    if (sk_font_metrics == nullptr) {
        return false;
    }
    const std::lock_guard lock(g_mutex);
    if (!EnsureFactory()) {
        return false;
    }
    IDWriteFontFace* face = FaceFor(typeface, font_bytes, face_index);
    if (face == nullptr) {
        return false;
    }

    DWRITE_FONT_METRICS dwfm{};
    const bool gdi = decision.measuring_mode == windows_path::kMeasureGdiClassic ||
                     decision.measuring_mode == windows_path::kMeasureGdiNatural;
    if (gdi) {
        DWRITE_MATRIX transform{};
        transform.m11 = 1.0f;
        transform.m22 = 1.0f;
        if (FAILED(face->GetGdiCompatibleMetrics(decision.text_size_render, 1.0f, &transform,
                                                 &dwfm))) {
            return false;
        }
    } else {
        face->GetMetrics(&dwfm);
    }
    if (dwfm.designUnitsPerEm == 0) {
        return false;
    }

    auto* m = static_cast<unsigned char*>(sk_font_metrics);
    const float size = decision.text_size_render;
    const auto upem = static_cast<float>(dwfm.designUnitsPerEm);
    const auto scaled = [&](const int design) { return size * static_cast<float>(design) / upem; };

    const float ascent = -scaled(dwfm.ascent);
    const float descent = scaled(dwfm.descent);
    const float leading = scaled(dwfm.lineGap);
    const float x_height = scaled(dwfm.xHeight);
    const float cap_height = scaled(dwfm.capHeight);
    const float underline_thickness = scaled(dwfm.underlineThickness);
    const float underline_position = -scaled(dwfm.underlinePosition);
    const float strikeout_thickness = scaled(dwfm.strikethroughThickness);
    const float strikeout_position = -scaled(dwfm.strikethroughPosition);

    std::memcpy(m + skia_abi::kFontMetricsAscent, &ascent, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsDescent, &descent, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsLeading, &leading, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsXHeight, &x_height, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsCapHeight, &cap_height, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsUnderlineThickness, &underline_thickness, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsUnderlinePosition, &underline_position, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsStrikeoutThickness, &strikeout_thickness, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsStrikeoutPosition, &strikeout_position, sizeof(float));

    auto flags = skia_abi::Read<uint32_t>(m, skia_abi::kFontMetricsFlags);
    flags |= skia_abi::kUnderlineThicknessValid | skia_abi::kUnderlinePositionValid |
             skia_abi::kStrikeoutThicknessValid | skia_abi::kStrikeoutPositionValid;
    std::memcpy(m + skia_abi::kFontMetricsFlags, &flags, sizeof(flags));

    // SkScalerContext_DW leaves fAvgCharWidth alone, so on Windows it reaches
    // Blink as zero and SimpleFontData::PlatformInit measures 'x' instead.
    // Fontations fills it from OS/2, which Blink then prefers, and an input
    // with no size attribute is laid out from it.
    constexpr float kNoAvgCharWidth = 0;
    std::memcpy(m + skia_abi::kFontMetricsAvgCharWidth, &kNoAvgCharWidth, sizeof(float));

    // fTop, fBottom, fXMin and fXMax bound the ink rather than the line, and
    // Skia's own values are already in the struct.
    return true;
}

bool RenderGlyph(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const skia_abi::Rec& rec, const skia_abi::Glyph& glyph,
                 const skia_abi::PreBlend& preblend, const windows_path::Decision& decision,
                 void* image_buffer, const uint32_t face_index)
{
    // Only a plain outline glyph. COLRv0, COLRv1 and embedded bitmaps are
    // drawn by Skia through paths an alpha texture cannot stand in for.
    if (glyph.scaler_bits != skia_abi::kFontationsPath) {
        return false;
    }
    if (glyph.mask_format == skia_abi::kARGB32 || glyph.width == 0 || glyph.height == 0 ||
        image_buffer == nullptr) {
        return false;
    }

    const std::lock_guard lock(g_mutex);
    if (!EnsureFactory()) {
        return false;
    }
    IDWriteFontFace* face = FaceFor(typeface, font_bytes, face_index);
    if (face == nullptr) {
        return false;
    }

    // getDWMaskBits: the transform carries the sub-pixel offset, and the run
    // is one glyph with a zero advance at the origin.
    //
    // The matrix is sA, the total matrix with the vertical scale taken out,
    // since Skia normalizes that scale into the em size and DirectWrite must
    // not apply it twice. DWRITE_MATRIX transposes the skews.
    DWRITE_MATRIX transform{};
    float scale_y = 0;
    windows_path::Matrix2x2 remaining;
    windows_path::ComputeMatrices(rec, &scale_y, &remaining);
    transform.m11 = remaining.scale_x;
    transform.m12 = remaining.skew_y;
    transform.m21 = remaining.skew_x;
    transform.m22 = remaining.scale_y;
    transform.dx = static_cast<float>(glyph.SubX()) / 4.0f;
    transform.dy = static_cast<float>(glyph.SubY()) / 4.0f;

    FLOAT advance = 0.0f;
    UINT16 index = glyph.GlyphId();
    DWRITE_GLYPH_OFFSET offset{};
    DWRITE_GLYPH_RUN run{};
    run.glyphCount = 1;
    run.glyphAdvances = &advance;
    run.fontFace = face;
    run.fontEmSize = decision.text_size_render;
    run.bidiLevel = 0;
    run.glyphIndices = &index;
    run.isSideways = FALSE;
    run.glyphOffsets = &offset;

    const auto rendering_mode = static_cast<DWRITE_RENDERING_MODE>(decision.rendering_mode);
    const auto measuring_mode = static_cast<DWRITE_MEASURING_MODE>(decision.measuring_mode);
    // windows_path names these the way SkScalerContext_DW does; DirectWrite has
    // its own enum with the same values, so the comparisons below are made in
    // DirectWrite's type rather than across the two.
    constexpr auto kDwriteAliased1x1 =
        static_cast<DWRITE_TEXTURE_TYPE>(windows_path::kTextureAliased1x1);
    constexpr auto kDwriteClearType3x1 =
        static_cast<DWRITE_TEXTURE_TYPE>(windows_path::kTextureClearType3x1);
    const auto texture_type = static_cast<DWRITE_TEXTURE_TYPE>(decision.texture_type);

    IDWriteGlyphRunAnalysis* analysis = nullptr;
    HRESULT hr = E_FAIL;
    // IDWriteFactory2::CreateGlyphRunAnalysis is very bad at aliased glyphs,
    // so Skia only uses it where it has to - grid fitting off, or grayscale.
    if (g_dw.factory2 != nullptr &&
        (decision.grid_fit_mode == windows_path::kGridFitDisabled ||
         decision.anti_alias_mode == windows_path::kAntiAliasGrayscale)) {
        hr = g_dw.factory2->CreateGlyphRunAnalysis(
            &run, &transform, rendering_mode, measuring_mode,
            static_cast<DWRITE_GRID_FIT_MODE>(decision.grid_fit_mode),
            static_cast<DWRITE_TEXT_ANTIALIAS_MODE>(decision.anti_alias_mode), 0.0f, 0.0f,
            &analysis);
    } else {
        hr = g_dw.factory5->CreateGlyphRunAnalysis(&run, 1.0f, &transform, rendering_mode,
                                                   measuring_mode, 0.0f, 0.0f, &analysis);
    }
    if (FAILED(hr) || analysis == nullptr) {
        return false;
    }

    const size_t pixels = static_cast<size_t>(glyph.width) * glyph.height;
    const size_t needed = texture_type == kDwriteClearType3x1 ? pixels * 3 : pixels;
    std::vector<uint8_t> bits(needed, 0);

    RECT bbox;
    bbox.left = glyph.left;
    bbox.top = glyph.top;
    bbox.right = glyph.left + glyph.width;
    bbox.bottom = glyph.top + glyph.height;

    hr = analysis->CreateAlphaTexture(texture_type, &bbox, bits.data(),
                                      static_cast<UINT32>(bits.size()));
    analysis->Release();
    if (FAILED(hr)) {
        return false;
    }

    // generateDWImage, with the conversions from the bottom of
    // SkScalerContext_win_dw.cpp. sk_apply_lut_if is the preblend, passing the
    // value through where the PreBlend is not applicable.
    const size_t row_bytes = RowBytes(glyph);
    const uint8_t* src = bits.data();
    auto* dst8 = static_cast<uint8_t*>(image_buffer);

    if (decision.rendering_mode == windows_path::kRenderAliased) {
        // BilevelToBW. The aliased texture is one byte per pixel holding 0 or
        // 0xFF, so masking each source byte with its own destination bit
        // packs eight pixels at a time.
        const int width = glyph.width;
        const size_t dst_rb = (static_cast<size_t>(width) + 7) >> 3;
        const int byte_count = width >> 3;
        const int bit_count = width & 7;
        uint8_t* dst = dst8;
        for (int row = 0; row < glyph.height; ++row) {
            for (int i = 0; i < byte_count; ++i) {
                unsigned byte = 0;
                byte |= src[0] & (1u << 7);
                byte |= src[1] & (1u << 6);
                byte |= src[2] & (1u << 5);
                byte |= src[3] & (1u << 4);
                byte |= src[4] & (1u << 3);
                byte |= src[5] & (1u << 2);
                byte |= src[6] & (1u << 1);
                byte |= src[7] & (1u << 0);
                dst[i] = static_cast<uint8_t>(byte);
                src += 8;
            }
            if (bit_count > 0) {
                unsigned byte = 0;
                unsigned mask = 0x80;
                for (int i = 0; i < bit_count; ++i) {
                    byte |= src[i] & mask;
                    mask >>= 1;
                }
                dst[byte_count] = static_cast<uint8_t>(byte);
            }
            src += bit_count;
            dst += dst_rb;
        }
        return true;
    }

    if (!IsLcd(rec)) {
        uint8_t* dst = dst8;
        if (texture_type == kDwriteAliased1x1) {
            // GrayscaleToA8
            for (int row = 0; row < glyph.height; ++row) {
                for (int i = 0; i < glyph.width; ++i) {
                    dst[i] = ApplyLut(*src++, preblend.g);
                }
                dst += row_bytes;
            }
        } else {
            // RGBToA8: the three subpixels averaged, then the green table.
            for (int row = 0; row < glyph.height; ++row) {
                for (int i = 0; i < glyph.width; ++i) {
                    const unsigned r = *src++;
                    const unsigned g = *src++;
                    const unsigned b = *src++;
                    dst[i] = ApplyLut(static_cast<uint8_t>((r + g + b) / 3), preblend.g);
                }
                dst += row_bytes;
            }
        }
        return true;
    }

    // RGBToLcd16, which needs the ClearType texture and a matching mask.
    if (texture_type != kDwriteClearType3x1 ||
        glyph.mask_format != skia_abi::kLCD16) {
        return false;
    }
    const bool rgb = (rec.flags & skia_abi::kLCD_BGROrder) == 0;
    for (int row = 0; row < glyph.height; ++row) {
        auto* dst = reinterpret_cast<uint16_t*>(dst8 + static_cast<size_t>(row) * row_bytes);
        for (int i = 0; i < glyph.width; ++i) {
            uint8_t r, g, b;
            if (rgb) {
                r = ApplyLut(*src++, preblend.r);
                g = ApplyLut(*src++, preblend.g);
                b = ApplyLut(*src++, preblend.b);
            } else {
                b = ApplyLut(*src++, preblend.b);
                g = ApplyLut(*src++, preblend.g);
                r = ApplyLut(*src++, preblend.r);
            }
            dst[i] = Pack888ToRGB16(r, g, b);
        }
    }
    return true;
}

}  // namespace dwrite_raster
