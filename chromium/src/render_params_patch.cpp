//+--------------------------------------------------------------------------
//
//  render_params_patch.cpp - replace GetFontRenderParamsFromFcPattern with
//  the answers Windows would have given.
//
//  Mirrors ui/gfx/font_render_params_win.cc over
//  ui/gfx/linux/fontconfig_util.cc, which reads antialias, autohint,
//  embeddedbitmap, hinting, hintstyle and rgba off an FcPattern.
//
//  Chrome, Electron and Discord link fontconfig statically, so interposing it
//  reaches nothing there. The function is replaced where it stands instead,
//  which works whether fontconfig is linked statically or dynamically.
//
//  It carries no symbol, so it is found by what it names: the six property
//  strings are loaded with rip-relative leas, and one function in the image
//  reaches for all six.
//
//  Runs in the browser process, the only one that asks. A renderer is told
//  the answer over Mojo.
//
//----------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "render_params_patch.h"

namespace render_params_patch {
namespace {

// gfx::FontRenderParams. Four bools, two enums, two floats.
constexpr size_t kAntialiasing = 0;
constexpr size_t kSubpixelPositioning = 1;
constexpr size_t kAutohinter = 2;
constexpr size_t kUseBitmaps = 3;
constexpr size_t kHinting = 4;
constexpr size_t kSubpixelRendering = 8;

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
    p[kAntialiasing] = 1;
    p[kSubpixelPositioning] = 1;
    p[kAutohinter] = 0;
    p[kUseBitmaps] = 0;
    const int hinting = kHintingMedium;
    const int subpixel = kSubpixelRenderingRgb;
    std::memcpy(p + kHinting, &hinting, sizeof(hinting));
    std::memcpy(p + kSubpixelRendering, &subpixel, sizeof(subpixel));
}

// movabs rax, imm64 ; jmp rax
bool WriteDetour(unsigned char* at, void* to)
{
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return false;
    }
    constexpr size_t kPatch = 12;
    auto start = reinterpret_cast<uintptr_t>(at) & ~static_cast<uintptr_t>(page - 1);
    const uintptr_t last =
        (reinterpret_cast<uintptr_t>(at) + kPatch - 1) & ~static_cast<uintptr_t>(page - 1);
    const size_t len = last - start + static_cast<size_t>(page);
    auto* base = reinterpret_cast<void*>(start);
    if (mprotect(base, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        Say("the page would not open for writing; leaving the function alone");
        return false;
    }
    unsigned char code[kPatch] = {0x48, 0xB8};
    const auto target = reinterpret_cast<uint64_t>(to);
    std::memcpy(code + 2, &target, sizeof(target));
    code[10] = 0xFF;
    code[11] = 0xE0;
    std::memcpy(at, code, kPatch);
    if (mprotect(base, len, PROT_READ | PROT_EXEC) != 0) {
        Say("the page would not close again");
    }
    __builtin___clear_cache(reinterpret_cast<char*>(at),
                            reinterpret_cast<char*>(at + kPatch));
    return true;
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

void Apply(const uintptr_t base, const ElfW(Phdr)* phdr, const ElfW(Half) phnum)
{
    Image image;
    for (ElfW(Half) i = 0; i < phnum; ++i) {
        const ElfW(Phdr)& p = phdr[i];
        if (p.p_type != PT_LOAD) {
            continue;
        }
        const auto* begin = reinterpret_cast<const unsigned char*>(base + p.p_vaddr);
        const Region r{begin, begin + p.p_filesz};
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
    std::sort(wanted.begin(), wanted.end());

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
            const uintptr_t target = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
            if (InText(image, target)) {
                starts.push_back(target);
            }
        }
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
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
            const auto it = std::upper_bound(starts.begin(), starts.end(), site);
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
        const int n = popcount(h.mask);
        if (n > best_n) {
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
    if (WriteDetour(reinterpret_cast<unsigned char*>(best),
                    reinterpret_cast<void*>(&ChromiumFontRenderParams))) {
        (void)std::fprintf(stderr,
                           "chromium-patch: render params: "
                           "GetFontRenderParamsFromFcPattern +%#lx now answers what "
                           "Windows would; runner-up reads %d\n",
                           best - base, second_n);
    }
}

}  // namespace render_params_patch
