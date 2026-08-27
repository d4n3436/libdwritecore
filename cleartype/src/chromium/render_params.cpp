#include "render_params.h"

#include "skia_abi.h"

namespace render_params {
namespace {

// src/core/SkScalerContext.h
constexpr uint16_t kSubpixelPositioning = 0x0010;
constexpr uint16_t kForceAutohinting = 0x0020;

}  // namespace

void ApplyWindowsParams(void* rec, const uint16_t flags_before_filter)
{
    auto* bytes = static_cast<unsigned char*>(rec);
    auto mask_format = skia_abi::Read<uint8_t>(bytes, skia_abi::kRecMaskFormat);
    auto flags = skia_abi::Read<uint16_t>(bytes, skia_abi::kRecFlags);

    // kGenA8FromLCD says the surface cannot show subpixel text at all, which
    // Windows declines to override too.
    const bool surface_refused_lcd =
        (flags_before_filter & skia_abi::kGenA8FromLCD) != 0;
    if (mask_format == skia_abi::kA8 && !surface_refused_lcd) {
        mask_format = skia_abi::kLCD16;
        flags &= static_cast<uint16_t>(~skia_abi::kLCD_BGROrder);
        flags &= static_cast<uint16_t>(~skia_abi::kLCD_Vertical);
        flags &= static_cast<uint16_t>(~skia_abi::kGenA8FromLCD);
    }

    // HINTING_MEDIUM. Linux answers hintfull, the one value that stops
    // web_font_render_style.cc force-enabling subpixel positioning.
    flags &= static_cast<uint16_t>(~skia_abi::kHintingMask);
    flags |= static_cast<uint16_t>(skia_abi::kHintingNormal << skia_abi::kHintingShift);

    flags |= kSubpixelPositioning;

    // Linux derives linear metrics from subpixel positioning. Windows never
    // sets it, so advances stay rounded.
    flags &= static_cast<uint16_t>(~skia_abi::kLinearMetrics);

    flags &= static_cast<uint16_t>(~kForceAutohinting);
    flags &= static_cast<uint16_t>(~skia_abi::kEmbeddedBitmapText);

    // skia/BUILD.gn gives Linux SK_GAMMA_EXPONENT=1.2 and SK_GAMMA_CONTRAST=0.2
    // against Windows' SK_GAMMA_SRGB and 1.0. A contrast already at zero agrees,
    // since MakeRecAndEffects zeroes it on both platforms.
    constexpr uint8_t kGammaSrgb = 0;
    auto contrast = skia_abi::Read<uint8_t>(bytes, skia_abi::kRecContrast);
    if (contrast != 0) {
        contrast = 255;                         // 1.0 in 0.8 fixed point
    }
    std::memcpy(bytes + skia_abi::kRecDeviceGamma, &kGammaSrgb, sizeof(kGammaSrgb));
    std::memcpy(bytes + skia_abi::kRecContrast, &contrast, sizeof(contrast));

    std::memcpy(bytes + skia_abi::kRecMaskFormat, &mask_format, sizeof(mask_format));
    std::memcpy(bytes + skia_abi::kRecFlags, &flags, sizeof(flags));
}

}  // namespace render_params
