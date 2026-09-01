// The shapes these suggest read worse against the sources being mirrored.
// ReSharper disable CppRedundantParentheses
// ReSharper disable CppUseDesignatedInitializers
// ReSharper disable CppUseRangeAlgorithm
// ReSharper disable CppUseStructuredBinding
// ReSharper disable CppVariableCanBeMadeConstexpr

#include "render_params.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "code_patch.h"
#include "skia_abi.h"

namespace render_params {

void ApplyWindowsParams(void* rec, const uint16_t flags_before_filter,
                        const bool plain_fontations)
{
    auto* bytes = static_cast<unsigned char*>(rec);
    auto mask_format = skia_abi::Read<uint8_t>(bytes, skia_abi::kRecMaskFormat);
    auto flags = skia_abi::Read<uint16_t>(bytes, skia_abi::kRecFlags);
    const uint8_t arrived_mask = mask_format;

    // kGenA8FromLCD says the surface cannot show subpixel text at all, and
    // Windows does not override it either. MakeRecAndEffects also sets it for
    // text too_big_for_lcd, which is how a large heading arrives as A8.
    const bool surface_refused_lcd =
        (flags_before_filter & skia_abi::kGenA8FromLCD) != 0;

    // SkFont::setupForAsPaths builds the strike Skia draws from above about
    // 256 px: hinting none, embedded bitmaps off, generated at a canonical
    // 64 px and scaled by the draw. It turns subpixel antialiasing into
    // grayscale, on both platforms, so Windows measures that glyph through
    // DWRITE_TEXTURE_ALIASED_1x1 and promoting it back to LCD16 here would add
    // the ClearType 3x1 texture's one pixel of padding on each side.
    static const bool paths_off = [] {
        const char* v = std::getenv("DWC_AS_PATHS");
        return v != nullptr && (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0);
    }();
    const bool as_paths =
        !paths_off &&
        ((flags_before_filter & skia_abi::kHintingMask) >> skia_abi::kHintingShift) ==
            skia_abi::kHintingNone &&
        (flags_before_filter & skia_abi::kEmbeddedBitmapText) == 0;

    // Windows quantizes an LCD request past SK_MAX_SIZE_FOR_LCDTEXT (48) back
    // to A8 and fills it from an averaged ClearType texture
    // (too_big_for_lcd, SkScalerContext.cpp). MakeRecAndEffects tests the
    // post-2x2 area when the device matrix carried one, which an identity
    // post-2x2 says it did not.
    const auto text_size = skia_abi::Read<float>(bytes, skia_abi::kRecTextSize);
    const auto p00 = skia_abi::Read<float>(bytes, skia_abi::kRecPost2x2);
    const auto p01 = skia_abi::Read<float>(bytes, skia_abi::kRecPost2x2 + 4);
    const auto p10 = skia_abi::Read<float>(bytes, skia_abi::kRecPost2x2 + 8);
    const auto p11 = skia_abi::Read<float>(bytes, skia_abi::kRecPost2x2 + 12);
    constexpr float kMaxSizeForLcd = 48.0f;
    const bool post_identity = p00 == 1.0f && p11 == 1.0f && p01 == 0.0f && p10 == 0.0f;
    const bool too_big_for_lcd =
        post_identity ? text_size > kMaxSizeForLcd
                      : (p00 * p11 - p10 * p01) * text_size * text_size >
                            kMaxSizeForLcd * kMaxSizeForLcd;

    if (mask_format == skia_abi::kA8 && !surface_refused_lcd && !as_paths &&
        too_big_for_lcd) {
        if (!plain_fontations) {
            flags |= static_cast<uint16_t>(skia_abi::kGenA8FromLCD);
        }
    } else if (mask_format == skia_abi::kA8 && !surface_refused_lcd && !as_paths) {
        mask_format = skia_abi::kLCD16;
        flags &= static_cast<uint16_t>(~skia_abi::kLCD_BGROrder);
        flags &= static_cast<uint16_t>(~skia_abi::kLCD_Vertical);
        flags &= static_cast<uint16_t>(~skia_abi::kGenA8FromLCD);
    } else if (surface_refused_lcd && !plain_fontations) {
        // SkTypeface_Fontations::onFilterRec clears this flag on every rec.
        // DWriteFontTypeface leaves it alone, so Windows still fills the A8
        // mask from a ClearType texture and averages it. A typeface Windows
        // renders through Fontations keeps the flag cleared there too, so the
        // big-text A8 comes straight from the path on both sides.
        flags |= static_cast<uint16_t>(skia_abi::kGenA8FromLCD);
    }

    // Windows answers HINTING_MEDIUM, but Fontations would hint its outlines
    // from this field and Windows never does. The live rec says none, and
    // windows_path::WithWindowsHinting restores the Windows value where the
    // grid fit mode is chosen.
    // A typeface Windows itself renders through Fontations keeps SkFont's
    // default hinting instead, which font_platform_data_win.cc's CreateSkFont
    // never changes, and under which the Fontations scaler autohints and
    // rounds advances. An as-paths strike arrives at hinting none from
    // setupForAsPaths on both platforms and stays there.
    flags &= static_cast<uint16_t>(~skia_abi::kHintingMask);
    if (plain_fontations && !as_paths) {
        flags |= static_cast<uint16_t>(skia_abi::kHintingNormal << skia_abi::kHintingShift);
    } else {
        flags |= static_cast<uint16_t>(skia_abi::kHintingNone << skia_abi::kHintingShift);
    }

    flags |= skia_abi::kSubpixelPositioning;

    // Linux derives linear metrics from subpixel positioning. Windows never
    // sets it, so advances stay rounded.
    flags &= static_cast<uint16_t>(~skia_abi::kLinearMetrics);

    flags &= static_cast<uint16_t>(~skia_abi::kForceAutohinting);
    // Windows keeps a web font's embedded bitmaps, so a stood-aside typeface
    // keeps the flag as it arrived.
    if (!plain_fontations) {
        flags &= static_cast<uint16_t>(~skia_abi::kEmbeddedBitmapText);
    }

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

    // The as-paths answer has to survive into the decision, and every flag it
    // could have been read from is normalized above, so it goes in the spare
    // byte beside the bold fallback's.
    const uint8_t mark = as_paths ? skia_abi::kAsPathsMark : 0;
    std::memcpy(bytes + skia_abi::kRecReservedPaths, &mark, sizeof(mark));

    std::memcpy(bytes + skia_abi::kRecMaskFormat, &mask_format, sizeof(mask_format));
    std::memcpy(bytes + skia_abi::kRecFlags, &flags, sizeof(flags));

    if (static const bool log = std::getenv("DWC_REC_LOG") != nullptr; log) {
        (void)std::fprintf(stderr,
                           "chromium-patch: rec: size %.2f mask %u flags %#x -> "
                           "mask %u flags %#x plain=%d\n",
                           static_cast<double>(text_size), arrived_mask,
                           flags_before_filter, mask_format, flags,
                           plain_fontations ? 1 : 0);
    }
}

namespace {

// Byte offsets into gfx::FontRenderParams, which is four bools, two enums
// and two floats.
constexpr size_t kFieldAntialiasing = 0;
constexpr size_t kFieldSubpixelPositioning = 1;
constexpr size_t kFieldAutohinter = 2;
constexpr size_t kFieldUseBitmaps = 3;
constexpr size_t kFieldHinting = 4;
constexpr size_t kFieldSubpixelRendering = 8;

constexpr int kHintingMedium = 2;         // FontRenderParams::HINTING_MEDIUM
constexpr int kSubpixelRenderingRgb = 1;  // SUBPIXEL_RENDERING_RGB

// The property names the target function passes to fontconfig.
constexpr const char* kProperties[] = {"antialias", "autohint", "embeddedbitmap",
                                       "hinting", "hintstyle", "rgba"};
constexpr unsigned kPropertyCount = 6;

void Say(const char* what)
{
    (void)std::fprintf(stderr, "chromium-patch: render params: %s\n", what);
}

struct Region
{
    const unsigned char* begin;
    const unsigned char* end;
};

struct Image
{
    Region text[4];
    unsigned text_count = 0;
    Region rodata[8];
    unsigned rodata_count = 0;
};

bool InText(const Image& image, const uintptr_t a)
{
    for (unsigned i = 0; i < image.text_count; ++i) {
        if (a >= reinterpret_cast<uintptr_t>(image.text[i].begin) &&
            a < reinterpret_cast<uintptr_t>(image.text[i].end)) {
            return true;
        }
    }
    return false;
}

// What font_render_params_win.cc states with font smoothing on and ClearType
// selected, which is what Windows ships.
//
// text_contrast and text_gamma are left as they arrive. Windows fills them
// from the ClearType tuner key, and the gamma that is actually used is set on
// the scaler context in render_params.cpp.
extern "C" void ChromiumFontRenderParams(void*, void* param_out)
{
    if (param_out == nullptr) {
        return;
    }
    auto* p = static_cast<unsigned char*>(param_out);
    p[kFieldAntialiasing] = 1;
    p[kFieldSubpixelPositioning] = 1;
    p[kFieldAutohinter] = 0;
    p[kFieldUseBitmaps] = 0;
    const int hinting = kHintingMedium;
    const int subpixel = kSubpixelRenderingRgb;
    std::memcpy(p + kFieldHinting, &hinting, sizeof(hinting));
    std::memcpy(p + kFieldSubpixelRendering, &subpixel, sizeof(subpixel));
}

// Every copy of a name, and every position it appears at.
//
// The linker merges string literals by tail, so a name that ends a longer one
// is stored inside it with no terminator in front. "hinting" is
// &"autohinting"[4], and that interior byte is what the lea points at.
//
// Interior matches cost nothing, since an address only counts when a lea
// names it exactly.
void FindStrings(const Image& image, const char* needle, const unsigned which,
                 std::vector<std::pair<uintptr_t, unsigned>>* out)
{
    const size_t len = std::strlen(needle) + 1;   // with its terminator
    for (unsigned i = 0; i < image.rodata_count; ++i) {
        const Region& r = image.rodata[i];
        const unsigned char* p = r.begin;
        size_t left = static_cast<size_t>(r.end - r.begin);
        while (left >= len) {
            const void* hit = memmem(p, left, needle, len);
            if (hit == nullptr) {
                break;
            }
            const auto* at = static_cast<const unsigned char*>(hit);
            out->emplace_back(reinterpret_cast<uintptr_t>(at), which);
            left = static_cast<size_t>(r.end - (at + 1));
            p = at + 1;
        }
    }
}

}  // namespace

void ApplyToImage(const uintptr_t base, const ElfW(Phdr)* phdr, const ElfW(Half) phnum)
{
    Image image;
    for (ElfW(Half) i = 0; i < phnum; ++i) {
        const ElfW(Phdr)& p = phdr[i];
        if (p.p_type != PT_LOAD) {
            continue;
        }
        const auto* begin = reinterpret_cast<const unsigned char*>(base + p.p_vaddr);
        const Region r{.begin = begin, .end = begin + p.p_filesz};
        if ((p.p_flags & PF_X) != 0) {
            if (image.text_count < 4) {
                image.text[image.text_count++] = r;
            }
        } else if ((p.p_flags & PF_W) == 0 && image.rodata_count < 8) {
            image.rodata[image.rodata_count++] = r;
        }
    }
    if (image.text_count == 0 || image.rodata_count == 0) {
        return;
    }

    std::vector<std::pair<uintptr_t, unsigned>> wanted;
    unsigned present = 0;
    for (unsigned i = 0; i < kPropertyCount; ++i) {
        const size_t before = wanted.size();
        FindStrings(image, kProperties[i], i, &wanted);
        if (wanted.size() != before) {
            ++present;
        }
    }
    if (present != kPropertyCount) {
        Say("not all of the fontconfig property names are present");
        return;
    }
    std::ranges::sort(wanted);

    // Every function start reached by a direct call, so a lea can be
    // attributed to the function it sits in.
    std::vector<uintptr_t> starts;
    for (unsigned t = 0; t < image.text_count; ++t) {
        const Region& r = image.text[t];
        for (const unsigned char* p = r.begin; p + 5 <= r.end; ++p) {
            if (*p != 0xE8) {
                continue;
            }
            int32_t rel;
            std::memcpy(&rel, p + 1, sizeof(rel));
            const auto site = reinterpret_cast<uintptr_t>(p);
            if (const uintptr_t target = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
                InText(image, target)) {
                starts.push_back(target);
            }
        }
    }
    std::ranges::sort(starts);
    starts.erase(std::ranges::unique(starts).begin(), starts.end());
    if (starts.empty()) {
        return;
    }

    // Which distinct property names each function reaches for.
    struct Hit
    {
        uintptr_t fn;
        unsigned mask;
    };
    std::vector<Hit> hits;
    for (unsigned t = 0; t < image.text_count; ++t) {
        const Region& r = image.text[t];
        for (const unsigned char* p = r.begin + 1; p + 7 <= r.end; ++p) {
            if (p[0] != 0x8D || p[-1] < 0x48 || p[-1] > 0x4F || (p[1] & 0xC7) != 0x05) {
                continue;
            }
            int32_t disp;
            std::memcpy(&disp, p + 2, sizeof(disp));
            const auto site = reinterpret_cast<uintptr_t>(p - 1);
            const uintptr_t target = site + 7 + static_cast<uintptr_t>(static_cast<intptr_t>(disp));
            const auto found_str = std::lower_bound(
                wanted.begin(), wanted.end(), std::make_pair(target, 0u));
            if (found_str == wanted.end() || found_str->first != target) {
                continue;
            }
            const unsigned which = found_str->second;
            const auto it = std::ranges::upper_bound(starts, site);
            if (it == starts.begin()) {
                continue;
            }
            const uintptr_t fn = *std::prev(it);
            auto found_fn = std::find_if(hits.begin(), hits.end(),
                                         [fn](const Hit& h) { return h.fn == fn; });
            if (found_fn == hits.end()) {
                hits.push_back({fn, 1u << which});
            } else {
                found_fn->mask |= 1u << which;
            }
        }
    }

    const auto popcount = [](const unsigned v) { return __builtin_popcount(v); };
    uintptr_t best = 0;
    int best_n = 0;
    int second_n = 0;
    for (const Hit& h : hits) {
        if (const int n = popcount(h.mask); n > best_n) {
            second_n = best_n;
            best = h.fn;
            best_n = n;
        } else if (n > second_n) {
            second_n = n;
        }
    }
    // All six, and clear of whatever came next.
    if (best_n != static_cast<int>(kPropertyCount) || second_n >= best_n) {
        Say("no single function reads all the fontconfig properties");
        std::sort(hits.begin(), hits.end(), [&](const Hit& a, const Hit& b) {
            return popcount(a.mask) > popcount(b.mask);
        });
        for (size_t i = 0; i < hits.size() && i < 4; ++i) {
            (void)std::fprintf(stderr, "  fn +%#lx mask %#x (%d)\n",
                               hits[i].fn - base, hits[i].mask, popcount(hits[i].mask));
        }
        (void)std::fprintf(stderr, "  %zu starts, %zu string copies, %u text, %u rodata\n",
                           starts.size(), wanted.size(), image.text_count,
                           image.rodata_count);
        return;
    }

    // Reported as an offset, to be checked against a disassembly on disk.
    if (std::getenv("CHROMIUM_PATCH_DRYRUN") != nullptr) {
        (void)std::fprintf(stderr,
                           "chromium-patch: render params: would replace +%#lx, "
                           "runner-up reads %d\n",
                           best - base, second_n);
        return;
    }
    const char* why = nullptr;
    if (code_patch::WriteDetour(reinterpret_cast<void*>(best),
                                reinterpret_cast<void*>(&ChromiumFontRenderParams), &why)) {
        (void)std::fprintf(stderr,
                           "chromium-patch: render params: "
                           "GetFontRenderParamsFromFcPattern +%#lx now answers what "
                           "Windows would; runner-up reads %d\n",
                           best - base, second_n);
    } else {
        Say(why);
    }
}

}  // namespace render_params
