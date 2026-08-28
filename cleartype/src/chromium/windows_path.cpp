#include "windows_path.h"

#include <cmath>

// Skia compares these to zero exactly. A tolerance check would take a
// different branch than Windows does on the same input.
#pragma GCC diagnostic ignored "-Wfloat-equal"

namespace windows_path {

const char* RenderingModeName(const RenderingMode m)
{
    switch (m) {
        case kRenderAliased: return "ALIASED";
        case kRenderGdiClassic: return "GDI_CLASSIC";
        case kRenderGdiNatural: return "GDI_NATURAL";
        case kRenderNatural: return "NATURAL";
        case kRenderNaturalSymmetric: return "NATURAL_SYMMETRIC";
        case kRenderOutline: return "OUTLINE";
        default: return "DEFAULT";
    }
}

const char* TextureTypeName(const TextureType t)
{
    return t == kTextureAliased1x1 ? "ALIASED_1x1" : "CLEARTYPE_3x1";
}

const char* MeasuringModeName(const MeasuringMode m)
{
    switch (m) {
        case kMeasureGdiClassic: return "GDI_CLASSIC";
        case kMeasureGdiNatural: return "GDI_NATURAL";
        default: return "NATURAL";
    }
}

namespace {

bool BothZero(const float a, const float b)
{
    return a == 0.0f && b == 0.0f;
}

}  // namespace

bool IsAxisAligned(const skia_abi::Rec& rec)
{
    return rec.pre_skew_x == 0.0f &&
           (BothZero(rec.post2x2[0][1], rec.post2x2[1][0]) ||
            BothZero(rec.post2x2[0][0], rec.post2x2[1][1]));
}

float DeviceScaleY(const skia_abi::Rec& rec)
{
    // computeMatrices(PreMatrixScale::kVertical) puts the whole vertical
    // device scale into scale.fY, as a magnitude.
    const float m11 = rec.post2x2[0][0];
    const float m22 = rec.post2x2[1][1];
    const float m12 = rec.post2x2[0][1];
    const float m21 = rec.post2x2[1][0];
    const float y = std::sqrt(m21 * m21 + m22 * m22);
    const float fallback = std::sqrt(m11 * m11 + m12 * m12);
    const float scale = y != 0.0f ? y : fallback != 0.0f ? fallback : 1.0f;
    return rec.text_size * scale;
}

Decision Decide(const skia_abi::Rec& rec, const float scale_y, const FontFacts& facts)
{
    Decision d;

    // realTextSize is the actual device size we want (as opposed to the size
    // the user requested). gdiTextSize is the size we request when GDI
    // compatible. Due to floating point math, the lower bits are suspect, so
    // Skia rounds to 1/64 carefully.
    const float real_text_size = scale_y;
    float gdi_text_size = std::round(real_text_size * 64.0f) / 64.0f;
    if (gdi_text_size == 0.0f) {
        gdi_text_size = 1.0f;
    }
    d.real_text_size = real_text_size;
    d.gdi_text_size = gdi_text_size;

    // When embedded bitmaps are requested, treat the entire range like a
    // bitmap strike if the range is gridfit only and contains a bitmap.
    bool treat_like_bitmap = false;
    bool axis_aligned_bitmap = false;
    // Chromium asks for embedded bitmaps whatever fontconfig says, since
    // FontPlatformData::CreateSkFont sets the flag from the block list after
    // applying the render style, so the live Linux flag is not the answer.
    if (!facts.blocks_embedded_bitmaps) {
        treat_like_bitmap = facts.has_bitmap_strike;
        axis_aligned_bitmap = IsAxisAligned(rec);
    }

    if (rec.mask_format == skia_abi::kBW) {
        // If the user requested aliased, do so with aliased compatible metrics.
        d.text_size_render = gdi_text_size;
        d.rendering_mode = kRenderAliased;
        d.texture_type = kTextureAliased1x1;
        d.text_size_measure = gdi_text_size;
        d.measuring_mode = kMeasureGdiClassic;
        d.branch = "bw";
    } else if (treat_like_bitmap && axis_aligned_bitmap) {
        // If we can use a bitmap, use gdi classic rendering and measurement.
        d.text_size_render = gdi_text_size;
        d.rendering_mode = kRenderGdiClassic;
        d.texture_type = kTextureClearType3x1;
        d.text_size_measure = gdi_text_size;
        d.measuring_mode = kMeasureGdiClassic;
        d.branch = "bitmap-axis-aligned";
    } else if (treat_like_bitmap) {
        // If rotated but the horizontal text could have used a bitmap, render
        // high quality rotated glyphs but measure using bitmap metrics.
        d.text_size_render = gdi_text_size;
        d.rendering_mode = kRenderNaturalSymmetric;
        d.texture_type = kTextureClearType3x1;
        d.text_size_measure = gdi_text_size;
        d.measuring_mode = kMeasureGdiClassic;
        d.branch = "bitmap-rotated";
    } else if (facts.gasp_known && facts.gasp_version_1_or_later) {
        // If the font has a gasp table version 1, use it to determine
        // symmetric rendering.
        d.text_size_render = real_text_size;
        d.rendering_mode =
            facts.gasp_symmetric_smoothing ? kRenderNaturalSymmetric : kRenderNatural;
        d.texture_type = kTextureClearType3x1;
        d.text_size_measure = real_text_size;
        d.measuring_mode = kMeasureNatural;
        d.branch = "gasp-v1";
    } else if (real_text_size > 20.0f || !facts.is_hinted) {
        // If the requested size is above 20px or there are no bytecode hints,
        // use symmetric rendering.
        d.text_size_render = real_text_size;
        d.rendering_mode = kRenderNaturalSymmetric;
        d.texture_type = kTextureClearType3x1;
        d.text_size_measure = real_text_size;
        d.measuring_mode = kMeasureNatural;
        d.branch = "large-or-unhinted";
    } else {
        // Fonts with hints, no gasp or gasp version 0, and below 20px get
        // non-symmetric rendering. Often such fonts have hints which were only
        // tested with GDI ClearType classic, and some rely on drop out control
        // in the y direction to be legible. See https://crbug.com/385897
        d.text_size_render = gdi_text_size;
        d.rendering_mode = kRenderNatural;
        d.texture_type = kTextureClearType3x1;
        d.text_size_measure = real_text_size;
        d.measuring_mode = kMeasureNatural;
        d.branch = "hinted-small";
    }

    // DirectWrite2 allows for grayscale hinting.
    d.anti_alias_mode = kAntiAliasClearType;
    if (facts.factory2 && facts.fontface2 && rec.mask_format == skia_abi::kA8 &&
        (rec.flags & skia_abi::kGenA8FromLCD) == 0) {
        // DWRITE_TEXTURE_ALIASED_1x1 is now misnamed, it must also be used
        // with grayscale.
        d.texture_type = kTextureAliased1x1;
        d.anti_alias_mode = kAntiAliasGrayscale;
    }

    // DirectWrite2 allows hinting to be disabled.
    d.grid_fit_mode = kGridFitEnabled;
    if (rec.GetHinting() == skia_abi::kHintingNone) {
        d.grid_fit_mode = kGridFitDisabled;
        if (d.rendering_mode != kRenderAliased) {
            d.rendering_mode = kRenderNaturalSymmetric;
        }
    }

    if (rec.IsLinearMetrics()) {
        d.text_size_measure = real_text_size;
        d.measuring_mode = kMeasureNatural;
    }

    // The GDI measuring modes don't seem to work well with CBDT fonts.
    if (d.measuring_mode != kMeasureNatural && facts.has_cbdt) {
        d.measuring_mode = kMeasureNatural;
    }

    return d;
}

}  // namespace windows_path
