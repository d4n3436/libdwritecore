//+--------------------------------------------------------------------------
//
//  skia_abi.h - the Skia types this patch reads out of a live process.
//
//  Chromium's Skia is statically linked and stripped, so none of this can be
//  had from a header at build time. The offsets below are the release layout,
//  which is what Chromium ships:
//
//    sizeof(SkGlyph)            48
//    sizeof(SkScalerContextRec) 56
//    sizeof(SkScalerContext)    128
//
//  A debug build differs. SkGlyph carries a SkDEBUGCODE-only bool just before
//  fID, which puts fID at 48 and the struct at 56 bytes.
//
//  A build whose Skia moves these fields will read nonsense, so
//  LooksPlausible() below checks the values against what a glyph can actually
//  be before any of them is used.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_SKIA_ABI_H_INCLUDED
#define CHROMIUM_SKIA_ABI_H_INCLUDED

#include <cmath>
#include <cstdint>
#include <cstring>

namespace skia_abi {

// src/core/SkGlyph.h
constexpr size_t kGlyphWidth = 0;              // uint16_t
constexpr size_t kGlyphHeight = 2;             // uint16_t
constexpr size_t kGlyphTop = 4;                // int16_t
constexpr size_t kGlyphLeft = 6;               // int16_t
constexpr size_t kGlyphImage = 8;              // void*
constexpr size_t kGlyphMaskFormat = 40;        // SkMask::Format
constexpr size_t kGlyphScalerContextBits = 42; // uint16_t
constexpr size_t kGlyphID = 44;                // SkPackedGlyphID
constexpr size_t kGlyphSize = 48;

// src/core/SkScalerContext.h. fRec sits after the vtable pointer.
constexpr size_t kContextRec = 8;
constexpr size_t kContextTypeface = 64;  // SkTypeface&
constexpr size_t kContextPreBlend = 96;

// SkScalerContextRec, relative to kContextRec.
constexpr size_t kRecTypefaceID = 0;    // uint32_t
constexpr size_t kRecTextSize = 4;      // float
constexpr size_t kRecPreScaleX = 8;     // float
constexpr size_t kRecPreSkewX = 12;     // float
constexpr size_t kRecPost2x2 = 16;      // float[2][2]
constexpr size_t kRecFrameWidth = 32;   // float
constexpr size_t kRecLumBits = 44;      // uint32_t
constexpr size_t kRecDeviceGamma = 48;  // uint8_t, 2.6 fixed point
// fReservedAlign2, the other padding byte, holding the mark for a rec Skia
// built with SkFont::setupForAsPaths.
constexpr size_t kRecReservedPaths = 49;  // uint8_t
constexpr uint8_t kAsPathsMark = 0xAD;
constexpr size_t kRecContrast = 50;     // uint8_t, 0.8 fixed point
// fReservedAlign, a padding byte Skia declares const, initializes to zero and
// then only ever passes to sk_ignore_unused_variable. Nothing reads it, so a
// hook can leave a value of its own there.
constexpr size_t kRecReserved = 51;     // uint8_t
constexpr size_t kRecMaskFormat = 52;   // uint8_t
constexpr size_t kRecFlags = 54;        // uint16_t

// SkMask::Format
enum MaskFormat : uint8_t
{
    kBW = 0,
    kA8 = 1,
    kARGB32 = 3,
    kLCD16 = 4,
};

// SkScalerContext flags
enum Flags : uint16_t
{
    kFrameAndFill = 0x0001,
    kEmbolden = 0x0008,
    kEmbeddedBitmapText = 0x0004,
    kSubpixelPositioning = 0x0010,
    kForceAutohinting = 0x0020,
    kLCD_Vertical = 0x0200,
    kLCD_BGROrder = 0x0400,
    kGenA8FromLCD = 0x0800,
    kLinearMetrics = 0x1000,
    kHintingMask = 0x0180,
    kHintingShift = 7,
};

// SkFontationsScalerContext::ScalerContextBits, from
// src/ports/SkTypeface_fontations.cpp. These are the values the glyph handed
// to generateImage actually carries, because its metrics came from that
// scaler context. They are not SkScalerContext_DW's enum, which numbers the
// same idea differently.
enum FontationsBits : uint16_t
{
    kFontationsPath = 1,
    kFontationsColrV0 = 2,
    kFontationsColrV1 = 3,
    kFontationsBitmap = 4,
};

// SkFontHinting
enum Hinting : unsigned
{
    kHintingNone = 0,
    kHintingSlight = 1,
    kHintingNormal = 2,
    kHintingFull = 3,
};

template <typename T>
T Read(const void* base, const size_t offset)
{
    T v{};
    std::memcpy(static_cast<void*>(&v),
                static_cast<const unsigned char*>(base) + offset, sizeof(T));
    return v;
}

struct Glyph
{
    uint16_t width = 0;
    uint16_t height = 0;
    int16_t top = 0;
    int16_t left = 0;
    void* image = nullptr;
    uint8_t mask_format = 0;
    uint16_t scaler_bits = 0;
    uint32_t packed_id = 0;
    bool packed_id_known = false;

    // SkPackedGlyphID packs the sub-pixel position around the glyph id:
    // sub-x in bits 0..1, id in 2..17, sub-y in 18..19. Bits 20 and up are
    // unused, so a value with any of them set is not a packed glyph id.
    static bool PlausiblePackedID(const uint32_t v) { return v >> 20 == 0; }

    uint16_t GlyphId() const { return static_cast<uint16_t>((packed_id >> 2) & 0xFFFF); }
    unsigned SubX() const { return packed_id & 0x3; }
    unsigned SubY() const { return (packed_id >> 18) & 0x3; }

    static Glyph From(const void* p)
    {
        Glyph g;
        g.width = Read<uint16_t>(p, kGlyphWidth);
        g.height = Read<uint16_t>(p, kGlyphHeight);
        g.top = Read<int16_t>(p, kGlyphTop);
        g.left = Read<int16_t>(p, kGlyphLeft);
        g.image = Read<void*>(p, kGlyphImage);
        g.mask_format = Read<uint8_t>(p, kGlyphMaskFormat);
        g.scaler_bits = Read<uint16_t>(p, kGlyphScalerContextBits);
        g.packed_id = Read<uint32_t>(p, kGlyphID);
        g.packed_id_known = PlausiblePackedID(g.packed_id);
        return g;
    }

    // Cheap sanity check before anything is believed. Skia caps a mask at
    // kMaxGlyphWidth and will not ask for an image of nothing.
    bool LooksPlausible() const
    {
        constexpr uint16_t kMaxGlyphDimension = 1 << 13;
        return width <= kMaxGlyphDimension && height <= kMaxGlyphDimension &&
               (mask_format == kBW || mask_format == kA8 || mask_format == kARGB32 ||
                mask_format == kLCD16);
    }
};

// SkScalerContext::GlyphMetrics, the value generateMetrics returns. 64 bytes,
// so it comes back through a hidden pointer rather than in registers.
constexpr size_t kMetricsAdvanceX = 0;    // SkVector{float,float}
constexpr size_t kMetricsAdvanceY = 4;
constexpr size_t kMetricsBounds = 8;      // SkRect
constexpr size_t kMetricsMaskFormat = 24;
constexpr size_t kMetricsExtraBits = 26;  // uint16_t
constexpr size_t kMetricsNeverRequestPath = 28;  // bool
constexpr size_t kMetricsComputeFromPath = 29;   // bool
constexpr size_t kMetricsSize = 64;

// SkFontMetrics, filled by generateFontMetrics. All SkScalar but for the
// leading flags word.
constexpr size_t kFontMetricsFlags = 0;
constexpr size_t kFontMetricsTop = 4;
constexpr size_t kFontMetricsAscent = 8;
constexpr size_t kFontMetricsDescent = 12;
constexpr size_t kFontMetricsBottom = 16;
constexpr size_t kFontMetricsLeading = 20;
constexpr size_t kFontMetricsAvgCharWidth = 24;
constexpr size_t kFontMetricsMaxCharWidth = 28;
constexpr size_t kFontMetricsXMin = 32;
constexpr size_t kFontMetricsXMax = 36;
constexpr size_t kFontMetricsXHeight = 40;
constexpr size_t kFontMetricsCapHeight = 44;
constexpr size_t kFontMetricsUnderlineThickness = 48;
constexpr size_t kFontMetricsUnderlinePosition = 52;
constexpr size_t kFontMetricsStrikeoutThickness = 56;
constexpr size_t kFontMetricsStrikeoutPosition = 60;

enum FontMetricsFlags : uint32_t
{
    kUnderlineThicknessValid = 0x1,
    kUnderlinePositionValid = 0x2,
    kStrikeoutThicknessValid = 0x4,
    kStrikeoutPositionValid = 0x8,
};

// SkTMaskPreBlend, the gamma and contrast correction tables Skia builds for
// this scaler context. src/core/SkMaskGamma.h: fParent occupies the first
// eight bytes, then the three tables, and isApplicable() is simply whether
// the green one is there.
struct PreBlend
{
    const uint8_t* r = nullptr;
    const uint8_t* g = nullptr;
    const uint8_t* b = nullptr;

    bool Applicable() const { return g != nullptr; }

    static PreBlend From(const void* context)
    {
        const auto* at = static_cast<const unsigned char*>(context) + kContextPreBlend;
        PreBlend p;
        p.r = Read<const uint8_t*>(at, 8);
        p.g = Read<const uint8_t*>(at, 16);
        p.b = Read<const uint8_t*>(at, 24);
        return p;
    }
};

struct Rec
{
    float text_size = 0;
    float pre_scale_x = 0;
    float pre_skew_x = 0;
    float post2x2[2][2] = {{0, 0}, {0, 0}};
    uint8_t device_gamma = 0;
    uint8_t contrast = 0;
    uint8_t mask_format = 0;
    uint16_t flags = 0;
    bool as_paths = false;

    unsigned GetHinting() const
    {
        return (flags & kHintingMask) >> kHintingShift;
    }
    bool IsLinearMetrics() const { return (flags & kLinearMetrics) != 0; }
    bool IsSubpixel() const { return (flags & kSubpixelPositioning) != 0; }

    // SkScalerContextRec::getSingleMatrix without its translation, which is
    // always zero. Skia builds it as SkFontPriv::MakeTextMatrix, a scale by
    // (size * preScaleX, size) post-skewed by preSkewX, post-concatenated with
    // post2x2. Row major.
    void SingleMatrix2x2(float m[4]) const
    {
        const float a = text_size * pre_scale_x;
        const float b = text_size;
        const float l[4] = {a, pre_skew_x * b, 0.0f, b};
        m[0] = post2x2[0][0] * l[0] + post2x2[0][1] * l[2];
        m[1] = post2x2[0][0] * l[1] + post2x2[0][1] * l[3];
        m[2] = post2x2[1][0] * l[0] + post2x2[1][1] * l[2];
        m[3] = post2x2[1][0] * l[1] + post2x2[1][1] * l[3];
    }

    // A device offset carried back into the em space the color glyph's paint
    // tree is walked in. False when the matrix will not invert.
    bool DeviceOffsetToEm(const float dx, const float dy, float* out_x, float* out_y) const
    {
        float m[4];
        SingleMatrix2x2(m);
        const float det = m[0] * m[3] - m[1] * m[2];
        if (det == 0.0f || !std::isfinite(det)) {
            return false;
        }
        *out_x = (m[3] * dx - m[1] * dy) / det;
        *out_y = (m[0] * dy - m[2] * dx) / det;
        return true;
    }
    bool WantsEmbeddedBitmaps() const { return (flags & kEmbeddedBitmapText) != 0; }
    bool AsPaths() const { return as_paths; }

    // SkScalerContextRec's own fixed-point conversions.
    float DeviceGamma() const { return static_cast<float>(device_gamma) / (1 << 6); }
    float Contrast() const { return static_cast<float>(contrast) / ((1 << 8) - 1); }

    static Rec From(const void* context)
    {
        const auto* rec = static_cast<const unsigned char*>(context) + kContextRec;
        Rec r;
        r.text_size = Read<float>(rec, kRecTextSize);
        r.pre_scale_x = Read<float>(rec, kRecPreScaleX);
        r.pre_skew_x = Read<float>(rec, kRecPreSkewX);
        for (size_t i = 0; i < 2; ++i) {
            for (size_t j = 0; j < 2; ++j) {
                r.post2x2[i][j] = Read<float>(rec, kRecPost2x2 + (i * 2 + j) * sizeof(float));
            }
        }
        r.device_gamma = Read<uint8_t>(rec, kRecDeviceGamma);
        r.contrast = Read<uint8_t>(rec, kRecContrast);
        r.mask_format = Read<uint8_t>(rec, kRecMaskFormat);
        r.flags = Read<uint16_t>(rec, kRecFlags);
        r.as_paths = Read<uint8_t>(rec, kRecReservedPaths) == kAsPathsMark;
        return r;
    }
};

}  // namespace skia_abi

#endif  // CHROMIUM_SKIA_ABI_H_INCLUDED
