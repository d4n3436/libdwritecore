#include "weight_style.h"

#include "code_patch.h"
#include "hb_abi.h"
#include "parity_gate.h"

#include "../parity_mode.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

namespace weight_style {
namespace {

constexpr char kBridgePrefix[] = "fontations_ffi$cxxbridge1$";
constexpr char kEntryTail[] = "get_font_style";

// The weight a regular face reports, and what it is reported as instead. One
// less is the whole change: it turns Blink's `>` into the `>=` that Windows
// uses, and leaves every comparison at another weight where it was.
constexpr int32_t kRegular = 400;
constexpr int32_t kReported = 399;

// fontations_ffi::BridgeFontStyle.
// ReSharper disable once CppDeclaratorNeverUsed
struct FontStyle
{
    int32_t weight;
    int32_t slant;
    int32_t width;
};

using StyleFn = bool (*)(const void*, const void*, FontStyle*);

StyleFn g_original = nullptr;

// The same question asked of a build whose fontconfig is compiled in, where
// Skia builds the style from an FcPattern instead of from the bridge. Nothing
// can be interposed there, so the calls are redirected in place.
constexpr int kFcResultMatch = 0;
constexpr int kFcWeightRegular = 80;
constexpr int kFcWeightBold = 200;
constexpr int kFcSetSystem = 0;

using FcGetIntegerFn = int (*)(const void*, const char*, int, int*);
using FcGetStringFn = int (*)(const void*, const char*, int, unsigned char**);
using FcGetFontsFn = void* (*)(void*, int);

FcGetIntegerFn g_fc_integer = nullptr;
FcGetStringFn g_fc_string = nullptr;
FcGetFontsFn g_fc_fonts = nullptr;

// FcFontSet's first two fields, which is all the walk needs.
// ReSharper disable once CppDeclaratorNeverUsed
struct FontSetHead
{
    int nfont;
    int sfont;
    void** fonts;
};

// Whether any face of this pattern's family is bold, answered from the real
// font set once and kept. A family that has one is left alone, since then that
// face is the one selected and its own weight is what Blink reads.
bool FamilyHasBold(const void* pattern)
{
    if (g_fc_string == nullptr || g_fc_fonts == nullptr) {
        return true;
    }
    unsigned char* family = nullptr;
    if (g_fc_string(pattern, "family", 0, &family) != kFcResultMatch || family == nullptr) {
        return true;
    }
    static std::vector<std::string>* bold = nullptr;
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    if (bold == nullptr) {
        bold = new std::vector<std::string>();
        if (const auto* all = static_cast<FontSetHead*>(g_fc_fonts(nullptr, kFcSetSystem));
            all != nullptr) {
            for (int i = 0; all->fonts != nullptr && i < all->nfont; ++i) {
                int weight = 0;
                if (g_fc_integer(all->fonts[i], "weight", 0, &weight) != kFcResultMatch ||
                    weight < kFcWeightBold) {
                    continue;
                }
                for (int f = 0;; ++f) {
                    unsigned char* other = nullptr;
                    if (g_fc_string(all->fonts[i], "family", f, &other) != kFcResultMatch ||
                        other == nullptr) {
                        break;
                    }
                    bold->emplace_back(reinterpret_cast<const char*>(other));
                }
            }
        }
    }
    const auto* name = reinterpret_cast<const char*>(family);
    return std::ranges::find(*bold, name) != bold->end();
}

int FcIntegerReplacement(const void* pattern, const char* object, const int n, int* value)
{
    const int result = g_fc_integer(pattern, object, n, value);
    if (result == kFcResultMatch && value != nullptr && *value == kFcWeightRegular && n == 0 &&
        object != nullptr && std::strcmp(object, "weight") == 0 && !FamilyHasBold(pattern)) {
        *value = kFcWeightRegular - 1;
    }
    return result;
}

bool Replacement(const void* font_ref, const void* coords, FontStyle* style)
{
    const bool ok = g_original(font_ref, coords, style);
    if (ok && style != nullptr && style->weight == kRegular) {
        style->weight = kReported;
    }
    return ok;
}

}  // namespace

void InstallAtLoad()
{
    if (!chromium_patch::ParityWanted()) {
        return;
    }
    void* entry = nullptr;
    for (const auto& [name, address] : hb_abi::SymbolsWithPrefix(kBridgePrefix)) {
        if (name.size() >= sizeof(kEntryTail) - 1 &&
            name.compare(name.size() - (sizeof(kEntryTail) - 1), sizeof(kEntryTail) - 1,
                         kEntryTail) == 0) {
            entry = address;
        }
    }
    if (entry == nullptr) {
        return;
    }
    const code_patch::TextSpan image = code_patch::TextHolding(entry);
    if (image.text == nullptr) {
        return;
    }
    g_original = reinterpret_cast<StyleFn>(entry);
    const unsigned moved = code_patch::RedirectCalls(
        image.text, image.size, static_cast<const unsigned char*>(entry),
        reinterpret_cast<void*>(&Replacement));
    if (moved == 0) {
        g_original = nullptr;
        return;
    }
}

void InstallFontconfig()
{
    if (!chromium_patch::ParityWanted()) {
        return;
    }
    const auto found = hb_abi::SymbolsWithPrefix("FcPattern");
    const auto integer = found.find("FcPatternGetInteger");
    const auto string = found.find("FcPatternGetString");
    if (integer == found.end() || string == found.end()) {
        return;
    }
    // A build that links fontconfig shared is already served by the
    // interposers, and redirecting here as well would answer twice.
    Dl_info where{};
    if (dladdr(integer->second, &where) != 0 && where.dli_fname != nullptr &&
        std::strstr(where.dli_fname, "fontconfig") != nullptr) {
        return;
    }
    const auto sets = hb_abi::SymbolsWithPrefix("FcConfigGetFonts");
    const auto fonts = sets.find("FcConfigGetFonts");
    if (fonts == sets.end()) {
        return;
    }
    g_fc_integer = reinterpret_cast<FcGetIntegerFn>(integer->second);
    g_fc_string = reinterpret_cast<FcGetStringFn>(string->second);
    g_fc_fonts = reinterpret_cast<FcGetFontsFn>(fonts->second);

    const code_patch::TextSpan image = code_patch::TextHolding(integer->second);
    if (image.text == nullptr) {
        return;
    }
    const unsigned moved = code_patch::RedirectCalls(
        image.text, image.size, static_cast<const unsigned char*>(integer->second),
        reinterpret_cast<void*>(&FcIntegerReplacement));
    if (moved == 0) {
        g_fc_integer = nullptr;
        return;
    }
}

}  // namespace weight_style
