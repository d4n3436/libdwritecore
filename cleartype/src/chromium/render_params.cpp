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

    // kGenA8FromLCD says the surface cannot show subpixel text at all, and
    // Windows does not override it either. MakeRecAndEffects also sets it for
    // text too_big_for_lcd, which is how a large heading arrives as A8.
    const bool surface_refused_lcd =
        (flags_before_filter & skia_abi::kGenA8FromLCD) != 0;
    if (mask_format == skia_abi::kA8 && !surface_refused_lcd) {
        mask_format = skia_abi::kLCD16;
        flags &= static_cast<uint16_t>(~skia_abi::kLCD_BGROrder);
        flags &= static_cast<uint16_t>(~skia_abi::kLCD_Vertical);
        flags &= static_cast<uint16_t>(~skia_abi::kGenA8FromLCD);
    } else if (surface_refused_lcd) {
        // SkTypeface_Fontations::onFilterRec clears this flag on every rec.
        // DWriteFontTypeface leaves it alone, so Windows still fills the A8
        // mask from a ClearType texture and averages it.
        flags |= static_cast<uint16_t>(skia_abi::kGenA8FromLCD);
    }

    // Windows answers HINTING_MEDIUM, but Fontations would hint its outlines
    // from this field and Windows never does. The live rec says none, and
    // windows_path::WithWindowsHinting restores the Windows value where the
    // grid fit mode is chosen.
    flags &= static_cast<uint16_t>(~skia_abi::kHintingMask);
    flags |= static_cast<uint16_t>(skia_abi::kHintingNone << skia_abi::kHintingShift);

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
