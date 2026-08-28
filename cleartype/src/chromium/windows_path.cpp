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

skia_abi::Rec WithWindowsHinting(skia_abi::Rec rec)
{
    rec.flags &= static_cast<uint16_t>(~skia_abi::kHintingMask);
    rec.flags |= static_cast<uint16_t>(skia_abi::kHintingNormal << skia_abi::kHintingShift);
    return rec;
}

bool IsAxisAligned(const skia_abi::Rec& rec)
{
    return rec.pre_skew_x == 0.0f &&
           (BothZero(rec.post2x2[0][1], rec.post2x2[1][0]) ||
            BothZero(rec.post2x2[0][0], rec.post2x2[1][1]));
}

namespace {

// SkMatrix22.cpp, SkComputeGivensRotation. G is Q^T for the QR of A, chosen
// so that GA[0][1] is zero. setSinCos(s, c) lays out as scaleX=c, skewX=-s,
// skewY=s, scaleY=c.
Matrix2x2 GivensRotation(const float a, const float b)
{
    float c;
    float s;
    if (b == 0.0f) {
        c = std::copysign(1.0f, a);
        s = 0.0f;
    } else if (a == 0.0f) {
        c = 0.0f;
        s = -std::copysign(1.0f, b);
    } else if (std::fabs(b) > std::fabs(a)) {
        const float t = a / b;
        const float u = std::copysign(std::sqrt(1.0f + t * t), b);
        s = -1.0f / u;
        c = -s * t;
    } else {
        const float t = b / a;
        const float u = std::copysign(std::sqrt(1.0f + t * t), a);
        c = 1.0f / u;
        s = -c * t;
    }
    return Matrix2x2{c, -s, s, c};
}

// Left-multiply, the 2x2 of SkMatrix::preConcat on a rotation.
Matrix2x2 Concat(const Matrix2x2& l, const Matrix2x2& r)
{
    return Matrix2x2{l.scale_x * r.scale_x + l.skew_x * r.skew_y,
                     l.scale_x * r.skew_x + l.skew_x * r.scale_y,
                     l.skew_y * r.scale_x + l.scale_y * r.skew_y,
                     l.skew_y * r.skew_x + l.scale_y * r.scale_y};
}

}  // namespace

bool ComputeMatrices(const skia_abi::Rec& rec, float* scale_y, Matrix2x2* remaining)
{
    // A is the total matrix getSingleMatrix builds: Scale(size * preScaleX,
    // size), post-skewed by preSkewX, then post-concatenated with fPost2x2.
    // Only the 2x2 is needed, and the translation is zero.
    const float size = rec.text_size;
    const float lx = size * rec.pre_scale_x;   // local scaleX
    const float lkx = rec.pre_skew_x * size;   // local skewX
    const float p00 = rec.post2x2[0][0];
    const float p01 = rec.post2x2[0][1];
    const float p10 = rec.post2x2[1][0];
    const float p11 = rec.post2x2[1][1];

    Matrix2x2 a;
    a.scale_x = p00 * lx;
    a.skew_x = p00 * lkx + p01 * size;
    a.skew_y = p10 * lx;
    a.scale_y = p10 * lkx + p11 * size;

    const bool skewed_or_flipped =
        a.skew_x != 0.0f || a.skew_y != 0.0f || a.scale_x < 0.0f || a.scale_y < 0.0f;

    // GA is A with the rotation removed. h is where A maps the horizontal
    // baseline, which for a vector is the first column.
    Matrix2x2 ga = a;
    if (skewed_or_flipped) {
        ga = Concat(GivensRotation(a.scale_x, a.skew_y), a);
    }

    // Singular, or so small an em-filling square could not touch a pixel.
    // Skia zeroes the matrices and renders nothing.
    constexpr float kNearlyZero = 1.0f / (1 << 12);
    if (std::fabs(ga.scale_x) <= kNearlyZero || std::fabs(ga.scale_y) <= kNearlyZero ||
        !std::isfinite(ga.scale_x) || !std::isfinite(ga.scale_y) ||
        !std::isfinite(ga.skew_x) || !std::isfinite(ga.skew_y)) {
        *scale_y = 1.0f;
        *remaining = Matrix2x2{0, 0, 0, 0};
        return false;
    }

    // kVertical puts the y-scale in both components.
    const float y = std::fabs(ga.scale_y);
    *scale_y = y;

    if (!skewed_or_flipped && a.scale_x == a.scale_y) {
        *remaining = Matrix2x2{};
    } else if (!skewed_or_flipped) {
        *remaining = Matrix2x2{a.scale_x / y, 0, 0, 1};
    } else {
        // sA = A with the scale taken out, preScale(1/s.fX, 1/s.fY), and
        // kVertical made both components y.
        *remaining = Matrix2x2{a.scale_x / y, a.skew_x / y, a.skew_y / y, a.scale_y / y};
    }
    return true;
}

float DeviceScaleY(const skia_abi::Rec& rec)
{
    float y = 0;
    Matrix2x2 ignored;
    ComputeMatrices(rec, &y, &ignored);
    return y;
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
