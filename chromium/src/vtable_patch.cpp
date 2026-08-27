//+--------------------------------------------------------------------------
//
//  vtable_patch.cpp - replace Skia's glyph rasterization entry point in a
//  stripped, statically linked Chromium or Electron binary from outside the
//  process, the way cleartype/src/libxul_patch.cpp does it for libxul.
//
//  libxul's patch sites are found from a rodata string anchor. Chromium builds
//  carry no equivalent, being -fno-rtti and stripped with no symtab, so there
//  is no name and no typeinfo string to look for. What they do carry is Skia's
//  Fontations bridge, whose Rust entry points cxxbridge exports as GLOBAL
//  DEFINED symbols in .dynsym. Those calls are statically bound and cannot be
//  interposed, but their addresses are enough to recognize the code that calls
//  them.
//
//  What gets replaced is SkScalerContext::generateImage, the virtual that
//  turns one glyph into a mask. It is found through the ABI rather than by
//  guessing which function looks like a rasterizer. SkScalerContext declares
//  exactly seven virtuals in a fixed order (src/core/SkScalerContext.h), so
//  the search is for the vtable, and generateImage is slot 3 of whichever
//  table matches.
//
//  The replacement works out what Chromium on Windows would have asked
//  DirectWrite for (windows_path.cpp), rebuilds the font out of the typeface
//  (typeface_bridge.cpp), rasterizes it through DWriteCore and converts the
//  result into Skia's mask with Skia's own preblend (dwrite_raster.cpp).
//  Anything it declines, such as a color or bitmap glyph or a build whose font
//  cannot be read, falls through to Skia's generateImage untouched.
//
//  The target image is found by which loaded module carries fontations_ffi
//  symbols, never by a binary name, so nothing here is tied to a particular
//  Electron, Chrome or Discord build.
//
//----------------------------------------------------------------------------

#include "skia_abi.h"
#include "typeface_bridge.h"
#include "font_facts.h"
#include "dwrite_raster.h"
#include "render_params.h"
#include "parity_gate.h"
#include "render_params_patch.h"
#include "windows_path.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
void chromium_hook_thunk();
void chromium_filter_rec_thunk();
void chromium_metrics_thunk();
void chromium_font_metrics_thunk();
void OnChromiumFontMetrics(void* context, void* metrics);
void OnChromiumMetrics(void* result, void* context, const void* glyph);
size_t ChromiumGetTableData(const void* self, uint32_t tag, size_t offset, size_t length,
                            void* data);
bool OnChromiumFilterRec(void* self, void* rec);
bool OnChromiumGenerateImage(void* context, const void* glyph, void* image_buffer);

// Read by hook_thunk.S's tail jump. Set before the slot is patched.
void* g_original = nullptr;
void* g_original_filter_rec = nullptr;
void* g_original_metrics = nullptr;
void* g_original_font_metrics = nullptr;
}

namespace {

// SkTypeface::onGetTableData. The signature is known, so this needs no
// assembly and the vtable slot points straight at it.
using GetTableDataFn = size_t (*)(const void* self, uint32_t tag, size_t offset,
                                  size_t length, void* data);
GetTableDataFn g_original_get_table_data = nullptr;

// SkSetFourByteTag('V','D','M','X').
constexpr uint32_t kVdmxTag = 0x56444D58;

}  // namespace

// Hiding VDMX is what makes Blink take the Windows branch for a font's ascent
// and descent. FontMetrics::AscentDescentWithHacks in
// third_party/blink/renderer/platform/fonts/font_metrics.cc digs the table up
// on Linux and uses its hinted per-ppem numbers instead, which Windows never
// reaches.
//
// Hidden rather than lowering the hinting, since Blink gates on the SkFont's
// hinting and that would also change how the glyphs are rasterized.
extern "C" size_t ChromiumGetTableData(const void* self, const uint32_t tag,
                                       const size_t offset, const size_t length, void* data)
{
    if (tag == kVdmxTag) {
        return 0;
    }
    return g_original_get_table_data(self, tag, offset, length, data);
}

namespace {

// NOLINTNEXTLINE(cert-dcl50-cpp)
void Report(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// NOLINTNEXTLINE(cert-dcl50-cpp)
void Report(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    (void)std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)std::fprintf(stderr, "chromium-patch: %s\n", buf);
}

bool EnvDisables(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0);
}

bool EnvEnables(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && (std::strcmp(v, "1") == 0 || std::strcmp(v, "on") == 0);
}

// ---------------------------------------------------------------------------
// The image, meaning its executable segments and relocated read-only data.
// No rodata region and no eh_frame, since nothing here is found by a string
// and function bounds come from the call graph instead (see CodeMap).
// ---------------------------------------------------------------------------

struct Region
{
    const unsigned char* begin = nullptr;
    const unsigned char* end = nullptr;
};

struct Image
{
    uintptr_t base = 0;
    Region text[4];
    unsigned text_count = 0;
    Region relro;
};

bool DescribeImage(const uintptr_t base, const ElfW(Phdr)* phdr, const ElfW(Half) phnum, Image* out)
{
    out->base = base;
    for (ElfW(Half) i = 0; i < phnum; ++i) {
        const ElfW(Phdr)& p = phdr[i];
        const auto* begin = reinterpret_cast<const unsigned char*>(base + p.p_vaddr);
        if (p.p_type == PT_LOAD && (p.p_flags & PF_X) != 0) {
            if (out->text_count < 4) {
                out->text[out->text_count++] = {.begin = begin, .end = begin + p.p_filesz};
            }
        } else if (p.p_type == PT_GNU_RELRO) {
            out->relro = {.begin = begin, .end = begin + p.p_filesz};
        }
    }
    return out->text_count != 0 && out->relro.begin != nullptr;
}

bool InText(const Image& image, const uintptr_t addr)
{
    for (unsigned t = 0; t < image.text_count; ++t) {
        if (addr >= reinterpret_cast<uintptr_t>(image.text[t].begin) &&
            addr < reinterpret_cast<uintptr_t>(image.text[t].end)) {
            return true;
        }
    }
    return false;
}

// PT_GNU_RELRO is read-only by the time anything runs, so the slot has to be
// opened and closed around the write.
bool WriteSlot(void** slot, void* value)
{
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return false;
    }
    const uintptr_t first = reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(page - 1);
    const uintptr_t last = (reinterpret_cast<uintptr_t>(slot) + sizeof(void*) - 1) &
                           ~static_cast<uintptr_t>(page - 1);
    const size_t len = last - first + static_cast<size_t>(page);
    auto* at = reinterpret_cast<void*>(first);
    if (mprotect(at, len, PROT_READ | PROT_WRITE) != 0) {
        Report("the vtable page would not open for writing");
        return false;
    }
    *slot = value;
    if (mprotect(at, len, PROT_READ) != 0) {
        Report("the vtable page would not close again");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Finding the anchor symbols. Section headers exist only in the file, so this
// mmaps the module's own path. A preloaded library's constructors all run
// before ld.so reaches the executable's entry point, where Chromium's sandbox
// lockdown happens.
// ---------------------------------------------------------------------------

struct FfiSymbol
{
    uintptr_t value = 0;   // link-time, so the load base still has to be added
    char name[128] = {};
};

bool FindFfiSymbols(const char* path, const char* want_substr, std::vector<FfiSymbol>* out)
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(ElfW(Ehdr)))) {
        close(fd);
        return false;
    }
    const auto size = static_cast<size_t>(st.st_size);
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return false;
    }
    const auto* base = static_cast<const unsigned char*>(map);
    const auto* ehdr = reinterpret_cast<const ElfW(Ehdr)*>(base);
    const bool ok_header = std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) == 0 &&
                           ehdr->e_shoff != 0 && ehdr->e_shnum != 0 &&
                           ehdr->e_shstrndx < ehdr->e_shnum &&
                           static_cast<uint64_t>(ehdr->e_shoff) +
                                   static_cast<uint64_t>(ehdr->e_shnum) * ehdr->e_shentsize <=
                               size;
    if (!ok_header) {
        munmap(map, size);
        return false;
    }

    const auto* shdrs = reinterpret_cast<const ElfW(Shdr)*>(base + ehdr->e_shoff);
    const ElfW(Shdr)& shstrtab = shdrs[ehdr->e_shstrndx];
    if (shstrtab.sh_offset >= size) {
        munmap(map, size);
        return false;
    }

    const ElfW(Shdr)* dynsym = nullptr;
    const ElfW(Shdr)* dynstr = nullptr;
    for (ElfW(Half) i = 0; i < ehdr->e_shnum; ++i) {
        const ElfW(Shdr)& sh = shdrs[i];
        if (shstrtab.sh_offset + sh.sh_name >= size) {
            continue;
        }
        const auto* name = reinterpret_cast<const char*>(base + shstrtab.sh_offset + sh.sh_name);
        if (std::strcmp(name, ".dynsym") == 0) {
            dynsym = &sh;
        } else if (std::strcmp(name, ".dynstr") == 0) {
            dynstr = &sh;
        }
    }
    if (dynsym == nullptr || dynstr == nullptr || dynsym->sh_entsize == 0 ||
        dynsym->sh_offset + dynsym->sh_size > size || dynstr->sh_offset + dynstr->sh_size > size) {
        munmap(map, size);
        return false;
    }

    const size_t count = dynsym->sh_size / dynsym->sh_entsize;
    const auto* syms = reinterpret_cast<const ElfW(Sym)*>(base + dynsym->sh_offset);
    for (size_t i = 0; i < count; ++i) {
        const ElfW(Sym)& s = syms[i];
        if (s.st_shndx == SHN_UNDEF || ELF64_ST_TYPE(s.st_info) != STT_FUNC) {
            continue;
        }
        if (s.st_name >= dynstr->sh_size) {
            continue;
        }
        const auto* name = reinterpret_cast<const char*>(base + dynstr->sh_offset + s.st_name);
        if (std::strstr(name, want_substr) == nullptr) {
            continue;
        }
        FfiSymbol sym;
        sym.value = s.st_value;
        std::strncpy(sym.name, name, sizeof(sym.name) - 1);
        out->push_back(sym);
    }
    munmap(map, size);
    return true;
}

// Set once the vtable slot has actually been written, so the constructor
// knows whether it is worth loading DirectWrite at all.
std::atomic<bool> g_patched{false};

// fFlags as onFilterRec received them. Scaler contexts are built one at a
// time under Skia's lock, so one slot carries it across the call.
std::atomic<uint16_t> g_flags_before_filter{0};

// ---------------------------------------------------------------------------
// Finding generateImage.
//
// SkScalerContext declares exactly seven virtuals, and their order is fixed by
// third_party/skia/src/core/SkScalerContext.h:
//
//     [-0x10] offset-to-top      0
//     [-0x08] typeinfo           0, because Chromium builds -fno-rtti
//     slot 0  ~SkScalerContext   (complete)
//     slot 1  ~SkScalerContext   (deleting)
//     slot 2  generateMetrics
//     slot 3  generateImage       <-- what gets replaced
//     slot 4  generatePath
//     slot 5  generateDrawable
//     slot 6  generateFontMetrics
//
// So the search is for the table, not for the function. Whichever of its
// virtuals can be recognized, slot 3 is generateImage regardless.
//
// Two classes in the image have this shape, because SkTypeface_Fontations has
// seven virtuals too. They are told apart by which slots reach the Rust
// bridge: the scaler context answers per-glyph questions from slot 2
// (generateMetrics) and font-wide ones from slot 6 (generateFontMetrics),
// while the typeface's reaching slots are 3 and 4.
// ---------------------------------------------------------------------------

constexpr unsigned kScalerContextVirtuals = 7;
constexpr unsigned kGenerateImageSlot = 3;

// fontations_ffi entry points that appear in SkFontationsScalerContext and in
// no other class in src/ports/SkTypeface_fontations.cpp. The typeface shares
// only lookup_glyph_or_zero and units_per_em_or_zero with it, so neither of
// those is listed. Used to rank, never alone to decide.
constexpr const char* kRasterOnlyCallees[] = {
    "bitmap_glyph", "has_bitmap_glyph", "bitmap_metrics", "png_data",
    "alpha_mask_size", "alpha_mask_data", "has_alpha_mask", "draw_colr_glyph",
    "has_colrv0_glyph", "has_colrv1_glyph", "get_colrv1_clip_box",
    "get_path_verbs_points", "shrink_verbs_points_if_needed",
    "unhinted_advance_width_or_zero", "make_hinting_instance",
    "make_mono_hinting_instance", "no_hinting_instance", "hinting_reliant",
    "outline_format", "has_any_color_table", "get_skia_metrics",
};

enum : uint8_t
{
    kCallsFfi = 1u << 0,     // calls a cxxbridge fontations symbol directly
    kCallsRaster = 1u << 1,  // and one of the rasterization-only ones
    kCallsFfiCaller = 1u << 2,
};

// Function boundaries without eh_frame_hdr, which covers too few of these
// functions to bound anything, so a binary search over its FDEs lands in the
// wrong one. Every direct call target is a function entry by definition, and
// so is every text address a vtable holds. Together they are dense enough
// that a call site's enclosing function is the greatest start at or before
// it.
struct CodeMap
{
    std::vector<uintptr_t> starts;   // sorted, unique
    std::vector<uint8_t> flags;      // parallel to starts

    size_t IndexOf(const uintptr_t addr) const
    {
        const auto it = std::upper_bound(starts.begin(), starts.end(), addr);
        if (it == starts.begin()) {
            return static_cast<size_t>(-1);
        }
        return static_cast<size_t>(std::prev(it) - starts.begin());
    }

    bool Has(const uintptr_t fn, const uint8_t bit) const
    {
        const auto it = std::lower_bound(starts.begin(), starts.end(), fn);
        if (it == starts.end() || *it != fn) {
            return false;
        }
        return (flags[static_cast<size_t>(it - starts.begin())] & bit) != 0;
    }
};

// Every `call rel32` (E8) in the image, handed to `visit` as (site, target).
// Used to derive function starts, which is why it must not include jumps: a
// call lands on an entry point, a jump usually lands in the middle of one.
template <typename F>
void ForEachCall(const Image& image, F&& visit)
{
    for (unsigned t = 0; t < image.text_count; ++t) {
        const Region& r = image.text[t];
        for (const unsigned char* p = r.begin; p + 5 <= r.end; ++p) {
            if (*p != 0xE8) {
                continue;
            }
            int32_t rel;
            std::memcpy(&rel, p + 1, sizeof(rel));
            const uintptr_t site = reinterpret_cast<uintptr_t>(p);
            const uintptr_t target = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
            if (InText(image, target)) {
                visit(site, target);
            }
        }
    }
}

// Calls (E8) and tail jumps (E9) alike.
//
// A function whose last act is to return another's result compiles to a jump,
// not a call, and then reaching the Rust bridge leaves no E8 behind. The
// source does not say which it will be, and identical source compiles both
// ways across builds, so both have to be followed.
//
// This is only ever matched against the address of a known exported symbol,
// so an ordinary intra-function jump cannot be mistaken for a branch into
// one.
template <typename F>
void ForEachBranch(const Image& image, F&& visit)
{
    for (unsigned t = 0; t < image.text_count; ++t) {
        const Region& r = image.text[t];
        for (const unsigned char* p = r.begin; p + 5 <= r.end; ++p) {
            if (*p != 0xE8 && *p != 0xE9) {
                continue;
            }
            int32_t rel;
            std::memcpy(&rel, p + 1, sizeof(rel));
            const uintptr_t site = reinterpret_cast<uintptr_t>(p);
            const uintptr_t target = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
            if (InText(image, target)) {
                visit(site, target);
            }
        }
    }
}

// SkTypeface::onFilterRec is declaration index 8 in every Skia revision
// checked, the virtuals added later in the class coming after it, so it is
// vtable slot 9 throughout once the two header words are counted out.
constexpr unsigned kFilterRecSlot = 9;

// Walk from any known slot of a vtable back to its first virtual, by looking
// for the two zero words every -fno-rtti vtable begins with.
void** VtableBaseFrom(const Image& image, void** known_slot)
{
    const auto* relro_lo = reinterpret_cast<const uintptr_t*>(image.relro.begin);
    auto* p = reinterpret_cast<uintptr_t*>(known_slot);
    for (unsigned back = 0; back < 64; ++back, --p) {
        if (p - 2 < relro_lo) {
            return nullptr;
        }
        if (p[-2] == 0 && p[-1] == 0) {
            return reinterpret_cast<void**>(p);
        }
        if (!InText(image, *p)) {
            return nullptr;   // ran out of the table before finding its head
        }
    }
    return nullptr;
}

// Install the rec filter on the Fontations typeface's own vtable. The proxy
// typeface Linux actually hands to a scaler context forwards onFilterRec to
// this one (SkTypeface_proxy::onFilterRec is
// `fRealTypeface->onFilterRec(rec)`), so patching here covers every scaler
// context without having to find the proxy's table too.
void InstallRecFilter(const Image& image, const std::vector<uintptr_t>& calls_tags)
{
    if (calls_tags.size() != 1) {
        Report("the Fontations typeface vtable is not identifiable, so the "
               "Windows render params cannot be applied");
        return;
    }
    const uintptr_t tags_fn = calls_tags.front();

    void** tags_slot = nullptr;
    auto* p = reinterpret_cast<uintptr_t*>(const_cast<unsigned char*>(image.relro.begin));
    auto* end = reinterpret_cast<uintptr_t*>(const_cast<unsigned char*>(image.relro.end));
    for (; p + 1 <= end; ++p) {
        if (*p != tags_fn) {
            continue;
        }
        if (tags_slot != nullptr) {
            Report("onGetTableTags is in more than one vtable; not applying the "
                   "Windows render params");
            return;
        }
        tags_slot = reinterpret_cast<void**>(p);
    }
    if (tags_slot == nullptr) {
        return;
    }

    void** base = VtableBaseFrom(image, tags_slot);
    if (base == nullptr) {
        Report("could not find the head of the Fontations typeface vtable");
        return;
    }
    void** slot = base + kFilterRecSlot;
    if (!InText(image, reinterpret_cast<uintptr_t>(*slot))) {
        Report("slot %u of that vtable is not a function", kFilterRecSlot);
        return;
    }
    g_original_filter_rec = *slot;
    if (WriteSlot(slot, reinterpret_cast<void*>(&chromium_filter_rec_thunk))) {
        Report("onFilterRec %p replaced through vtable slot %p; the Windows font "
               "render params will be applied to every scaler context",
               g_original_filter_rec, static_cast<void*>(slot));
    } else {
        g_original_filter_rec = nullptr;
    }

    // onGetTableData follows onGetTableTags by declaration order.
    void** data_slot = tags_slot + 1;
    if (InText(image, reinterpret_cast<uintptr_t>(*data_slot))) {
        g_original_get_table_data = reinterpret_cast<GetTableDataFn>(*data_slot);
        if (WriteSlot(data_slot, reinterpret_cast<void*>(&ChromiumGetTableData))) {
            Report("onGetTableData %p replaced through vtable slot %p; VDMX will "
                   "report as absent, as it does on Windows",
                   reinterpret_cast<void*>(g_original_get_table_data),
                   static_cast<void*>(data_slot));
        } else {
            g_original_get_table_data = nullptr;
        }
    }
}

void TryPatchModule(const uintptr_t base, const ElfW(Phdr)* phdr, const ElfW(Half) phnum,
                    const char* path, const std::vector<FfiSymbol>& syms)
{
    Image image;
    if (!DescribeImage(base, phdr, phnum, &image)) {
        Report("%s has no shape this can read", path);
        return;
    }

    // Which fontations symbols are rasterization-only, by runtime address.
    std::vector<uintptr_t> ffi;
    std::vector<uintptr_t> ffi_raster;
    ffi.reserve(syms.size());
    for (const FfiSymbol& s : syms) {
        const uintptr_t at = base + s.value;
        ffi.push_back(at);
        for (const char* r : kRasterOnlyCallees) {
            if (std::strstr(s.name, r) != nullptr) {
                ffi_raster.push_back(at);
                break;
            }
        }
    }
    std::sort(ffi.begin(), ffi.end());
    std::sort(ffi_raster.begin(), ffi_raster.end());

    const auto is_ffi = [&](const uintptr_t a) {
        return std::binary_search(ffi.begin(), ffi.end(), a);
    };
    const auto is_raster = [&](const uintptr_t a) {
        return std::binary_search(ffi_raster.begin(), ffi_raster.end(), a);
    };

    // RELRO is relocated by the time a constructor runs, so the two zero
    // header words really are zero rather than a pending relocation.
    CodeMap map;
    const auto* relro = reinterpret_cast<const uintptr_t*>(image.relro.begin);
    const auto* relro_end = reinterpret_cast<const uintptr_t*>(image.relro.end);
    for (const uintptr_t* p = relro; p + 1 <= relro_end; ++p) {
        if (InText(image, *p)) {
            map.starts.push_back(*p);
        }
    }

    ForEachCall(image, [&](uintptr_t, const uintptr_t target) {
        map.starts.push_back(target);
    });
    std::sort(map.starts.begin(), map.starts.end());
    map.starts.erase(std::unique(map.starts.begin(), map.starts.end()), map.starts.end());
    map.flags.assign(map.starts.size(), 0);

    ForEachBranch(image, [&](const uintptr_t site, const uintptr_t target) {
        if (!is_ffi(target)) {
            return;
        }
        const size_t i = map.IndexOf(site);
        if (i == static_cast<size_t>(-1)) {
            return;
        }
        map.flags[i] |= kCallsFfi;
        if (is_raster(target)) {
            map.flags[i] |= kCallsRaster;
        }
    });

    // One more hop, since a debug build leaves the C++ fontations_ffi
    // wrappers out of line.
    ForEachBranch(image, [&](const uintptr_t site, const uintptr_t target) {
        if (!map.Has(target, kCallsFfi)) {
            return;
        }
        const size_t i = map.IndexOf(site);
        if (i != static_cast<size_t>(-1)) {
            map.flags[i] |= kCallsFfiCaller;
        }
    });

    const auto reaches = [&](const uintptr_t fn) {
        return map.Has(fn, kCallsFfi) || map.Has(fn, kCallsFfiCaller);
    };

    unsigned found = 0;
    void** chosen_slot = nullptr;
    uintptr_t chosen_fn = 0;
    int best_raster = -1;

    for (const uintptr_t* p = relro; p + kScalerContextVirtuals + 1 <= relro_end; ++p) {
        // The two words before slot 0 are offset-to-top and typeinfo.
        if (p - 2 < relro || p[-2] != 0 || p[-1] != 0) {
            continue;
        }
        uintptr_t slots[kScalerContextVirtuals];
        bool shaped = true;
        for (unsigned i = 0; i < kScalerContextVirtuals; ++i) {
            slots[i] = p[i];
            if (!InText(image, slots[i])) {
                shaped = false;
                break;
            }
            for (unsigned j = 0; j < i; ++j) {
                if (slots[j] == slots[i]) {
                    shaped = false;   // a real class has seven distinct methods
                    break;
                }
            }
            if (!shaped) {
                break;
            }
        }
        if (!shaped) {
            continue;
        }
        // The eighth word belongs to whatever follows the table.
        if (InText(image, p[kScalerContextVirtuals])) {
            continue;
        }
        if (!reaches(slots[2]) || !reaches(slots[6])) {
            continue;
        }
        int raster = 0;
        for (unsigned i = 2; i < kScalerContextVirtuals; ++i) {
            if (map.Has(slots[i], kCallsRaster)) {
                ++raster;
            }
        }
        ++found;
        if (raster > best_raster) {
            best_raster = raster;
            chosen_fn = slots[kGenerateImageSlot];
            chosen_slot = const_cast<void**>(
                reinterpret_cast<void* const*>(p + kGenerateImageSlot));
        }
    }

    if (chosen_slot == nullptr) {
        Report("%s: no SkScalerContext vtable answering to the Fontations shape", path);
        return;
    }
    if (found > 1) {
        Report("%s: %u vtables matched; taking the one with the most "
               "rasterization-only calls (%d)", path, found, best_raster);
    }

    // The answer has to be a vtable entry rather than whatever function start
    // precedes the call site, since the dense start set has entries inside
    // large functions. Each vtable-referenced address is asked instead whether
    // an anchor call falls inside it.
    {
        uintptr_t tag_sym = 0;
        uintptr_t data_sym = 0;
        for (const FfiSymbol& s : syms) {
            if (std::strstr(s.name, "$table_tags") != nullptr) {
                tag_sym = base + s.value;
            } else if (std::strstr(s.name, "$table_data") != nullptr) {
                data_sym = base + s.value;
            }
        }
        // Some builds reach the bridge through a forwarding thunk, so nothing
        // branches to the symbol from a vtable entry. A function start that
        // jumps straight to an anchor counts as that anchor from here on.
        std::vector<uintptr_t> tag_targets{tag_sym};
        std::vector<uintptr_t> data_targets{data_sym};
        if (tag_sym != 0 && data_sym != 0) {
            ForEachBranch(image, [&](const uintptr_t site, const uintptr_t target) {
                if (*reinterpret_cast<const unsigned char*>(site) != 0xE9) {
                    return;   // a call is not a thunk, it is a use
                }
                if (target != tag_sym && target != data_sym) {
                    return;
                }
                // The jump is not the first instruction, these wrappers
                // opening with endbr64. The size cap is what keeps a large
                // function ending in a tail call from counting as one.
                const size_t i = map.IndexOf(site);
                if (i == static_cast<size_t>(-1)) {
                    return;
                }
                const uintptr_t start = map.starts[i];
                const uintptr_t end = i + 1 < map.starts.size()
                                          ? map.starts[i + 1]
                                          : reinterpret_cast<uintptr_t>(image.text[0].end);
                constexpr size_t kLargestThunk = 64;
                if (end - start > kLargestThunk) {
                    return;
                }
                (target == tag_sym ? tag_targets : data_targets).push_back(start);
            });
        }
        std::sort(tag_targets.begin(), tag_targets.end());
        std::sort(data_targets.begin(), data_targets.end());

        std::vector<uintptr_t> tag_sites;
        std::vector<uintptr_t> data_sites;
        if (tag_sym != 0 && data_sym != 0) {
            ForEachBranch(image, [&](const uintptr_t site, const uintptr_t target) {
                if (std::binary_search(tag_targets.begin(), tag_targets.end(), target)) {
                    tag_sites.push_back(site);
                } else if (std::binary_search(data_targets.begin(), data_targets.end(), target)) {
                    data_sites.push_back(site);
                }
            });
        }
        std::sort(tag_sites.begin(), tag_sites.end());
        std::sort(data_sites.begin(), data_sites.end());

        const auto has_site_in = [](const std::vector<uintptr_t>& sites, const uintptr_t lo,
                                    const uintptr_t hi) {
            const auto it = std::lower_bound(sites.begin(), sites.end(), lo);
            return it != sites.end() && *it < hi;
        };

        std::vector<uintptr_t> vtable_entries;
        for (const uintptr_t* p = relro; p + 1 <= relro_end; ++p) {
            if (InText(image, *p)) {
                vtable_entries.push_back(*p);
            }
        }
        std::sort(vtable_entries.begin(), vtable_entries.end());
        vtable_entries.erase(std::unique(vtable_entries.begin(), vtable_entries.end()),
                             vtable_entries.end());

        std::vector<uintptr_t> calls_tags;
        std::vector<uintptr_t> calls_data;
        for (const uintptr_t fn : vtable_entries) {
            const auto it = std::upper_bound(map.starts.begin(), map.starts.end(), fn);
            const uintptr_t end = it != map.starts.end()
                                      ? *it
                                      : reinterpret_cast<uintptr_t>(image.text[0].end);
            if (has_site_in(tag_sites, fn, end)) {
                calls_tags.push_back(fn);
            }
            if (has_site_in(data_sites, fn, end)) {
                calls_data.push_back(fn);
            }
        }
        // The bridge identifies onGetTableData by what the slot points at,
        // which after the VDMX hook is this library rather than Skia.
        calls_data.push_back(reinterpret_cast<uintptr_t>(&ChromiumGetTableData));
        typeface_bridge::SetAnchors(calls_tags, calls_data);
        if (chromium_patch::ParityWanted()) {
            InstallRecFilter(image, calls_tags);
        }
        Report("%s: table_tags reached by %zu vtable entries (%zu entry points), "
               "table_data by %zu", path, calls_tags.size(), tag_targets.size(),
               calls_data.size());
    }

    if (chromium_patch::ParityWanted()) {
        void** metrics_slot = chosen_slot - 1;
        if (InText(image, reinterpret_cast<uintptr_t>(*metrics_slot))) {
            g_original_metrics = *metrics_slot;
            if (WriteSlot(metrics_slot, reinterpret_cast<void*>(&chromium_metrics_thunk))) {
                Report("%s: generateMetrics %p replaced through vtable slot %p", path,
                       g_original_metrics, static_cast<void*>(metrics_slot));
            } else {
                g_original_metrics = nullptr;
            }
        }
    }

    if (chromium_patch::ParityWanted()) {
        void** font_metrics_slot = chosen_slot + 3;
        if (InText(image, reinterpret_cast<uintptr_t>(*font_metrics_slot))) {
            g_original_font_metrics = *font_metrics_slot;
            if (WriteSlot(font_metrics_slot,
                          reinterpret_cast<void*>(&chromium_font_metrics_thunk))) {
                Report("%s: generateFontMetrics %p replaced through vtable slot %p", path,
                       g_original_font_metrics, static_cast<void*>(font_metrics_slot));
            } else {
                g_original_font_metrics = nullptr;
            }
        }
    }

    g_original = reinterpret_cast<void*>(chosen_fn);
    if (WriteSlot(chosen_slot, reinterpret_cast<void*>(&chromium_hook_thunk))) {
        Report("%s: generateImage %#lx replaced through vtable slot %p", path, chosen_fn,
               static_cast<void*>(chosen_slot));
        g_patched.store(true, std::memory_order_release);
    } else {
        g_original = nullptr;
    }
}

struct LoadedModule
{
    const char* name = nullptr;
    uintptr_t base = 0;
    const ElfW(Phdr)* phdr = nullptr;
    ElfW(Half) phnum = 0;
};

struct ModuleList
{
    static constexpr unsigned kMax = 128;
    LoadedModule mods[kMax];
    unsigned count = 0;
};

int CollectModule(dl_phdr_info* info, size_t, void* out)
{
    auto* list = static_cast<ModuleList*>(out);
    if (list->count < ModuleList::kMax) {
        list->mods[list->count++] = {info->dlpi_name, info->dlpi_addr, info->dlpi_phdr,
                                      info->dlpi_phnum};
    }
    return 0;
}

std::atomic<bool> g_done{false};

// Chromium and Electron relaunch the same executable for every process role
// and this constructor runs in each one, but only a renderer rasterizes a
// glyph. The search costs enough startup time that running it in the GPU and
// network-service processes can push Chromium's own crash-loop detection into
// killing the browser, so it is spent only where it can matter.
//
// That is the zygote rather than the renderer. A renderer is normally forked
// from the zygote and never execed, so its own copy of this library never runs
// its constructor again, and what it inherits at fork time is whatever the
// zygote already wrote to its memory, patched vtable slot included. A
// renderer's own cmdline is checked as well, for the --no-zygote case.
bool ShouldScanThisProcess()
{
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char buf[4096];
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    // A zygote-forked renderer's cmdline reads back as a single argv[0]
    // holding the whole original command line, so NULs become spaces and one
    // substring search finds either flag either way.
    for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') {
            buf[i] = ' ';
        }
    }
    return std::strstr(buf, "--type=zygote") != nullptr ||
          std::strstr(buf, "--type=renderer") != nullptr;
}

// The browser is the only process that asks fontconfig what the host wants,
// a renderer being told over Mojo. It is identified by having no --type= at
// all, and gets only the render-params replacement, never the glyph
// machinery.
bool IsBrowserProcess()
{
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char buf[4096];
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') {
            buf[i] = ' ';
        }
    }
    return std::strstr(buf, "--type=") == nullptr;
}

void ScanLoadedImages()
{
    if (g_done.exchange(true)) {
        return;
    }
    if (chromium_patch::ParityWanted() && IsBrowserProcess()) {
        ModuleList browser;
        dl_iterate_phdr(CollectModule, &browser);
        for (unsigned i = 0; i < browser.count; ++i) {
            const LoadedModule& m = browser.mods[i];
            const char* path =
                (m.name != nullptr && m.name[0] != '\0') ? m.name : "/proc/self/exe";
            std::vector<FfiSymbol> syms;
            if (FindFfiSymbols(path, "fontations_ffi", &syms) && syms.size() >= 10) {
                render_params_patch::Apply(m.base, m.phdr, m.phnum);
                break;
            }
        }
        return;
    }
    if (!ShouldScanThisProcess()) {
        return;
    }
    if (EnvDisables("CHROMIUM_PATCH")) {
        return;
    }

    ModuleList list;
    dl_iterate_phdr(CollectModule, &list);

    // Out of the callback, since dl_iterate_phdr holds the loader's list lock
    // and the open()/mmap() below must not run under it.
    for (unsigned i = 0; i < list.count; ++i) {
        const LoadedModule& m = list.mods[i];
        const char* path = (m.name != nullptr && m.name[0] != '\0') ? m.name : "/proc/self/exe";
        std::vector<FfiSymbol> syms;
        // Well under what a real Fontations build exports, but enough to rule
        // out a few unrelated dynsym entries that contain the substring.
        if (!FindFfiSymbols(path, "fontations_ffi", &syms) || syms.size() < 10) {
            continue;
        }
        TryPatchModule(m.base, m.phdr, m.phnum, path, syms);
        return;
    }
}

// The rebuilt font and the answers it gives, kept per typeface and ppem.
// Rebuilding walks every table, so it happens once per typeface and the
// answers are then only re-derived when the size changes.
std::mutex g_font_mutex;
std::unordered_map<const void*, std::vector<uint8_t>> g_fonts;

const std::vector<uint8_t>& FontBytesLocked(void* typeface)
{
    auto font = g_fonts.find(typeface);
    if (font == g_fonts.end()) {
        std::vector<uint8_t> bytes = typeface_bridge::ReadFontFile(typeface);
        if (bytes.empty()) {
            Report("typeface %p: no font (its onGetTableTags/onGetTableData could not be "
                   "identified, or the tables would not read)", typeface);
        } else {
            Report("typeface %p: rebuilt %zu bytes from its tables", typeface, bytes.size());
        }
        font = g_fonts.emplace(typeface, std::move(bytes)).first;
    }
    return font->second;
}

windows_path::FontFacts FactsFor(void* typeface, const int ppem)
{
    static std::unordered_map<const void*, std::pair<int, windows_path::FontFacts>> cache;

    const std::lock_guard<std::mutex> lock(g_font_mutex);
    const auto cached = cache.find(typeface);
    if (cached != cache.end() && cached->second.first == ppem) {
        return cached->second.second;
    }

    const windows_path::FontFacts facts = font_facts::Describe(FontBytesLocked(typeface), ppem);
    cache[typeface] = {ppem, facts};
    return facts;
}

}  // namespace

// Runs before the typeface's own onFilterRec. Returns whether the caller
// should go on to run that, which it always should. This only needs to see
// the flags first, because Fontations clears kGenA8FromLCD_Flag and that flag
// is the only sign the surface cannot show subpixel text.
bool OnChromiumFilterRec(void*, void* rec)
{
    if (rec == nullptr) {
        return true;
    }
    const uint16_t arrived = skia_abi::Read<uint16_t>(rec, skia_abi::kRecFlags);
    g_flags_before_filter.store(arrived, std::memory_order_relaxed);
    {
        // This flag, not anything in the Rec, is what decides in
        // skia_text_metrics.cc whether advances keep their fraction.
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            Report("SkFont arrived with subpixelPositioning=%d hinting=%u "
                   "(flags %#x)",
                   (arrived & 0x0010) != 0 ? 1 : 0,
                   (arrived & skia_abi::kHintingMask) >> skia_abi::kHintingShift,
                   arrived);
        }
    }
    return true;
}

// Runs after it, with the flags as they arrived.
extern "C" void OnChromiumFilterRecDone(void* rec);
void OnChromiumFilterRecDone(void* rec)
{
    if (rec == nullptr) {
        return;
    }
    render_params::ApplyWindowsParams(
        rec, g_flags_before_filter.load(std::memory_order_relaxed));
}

// Runs after Skia's own generateMetrics, on the metrics it produced.
//
// Only the advance is replaced. Windows measures it through DirectWrite at
// fTextSizeMeasure in fMeasuringMode, linear in the text size on the branch an
// ordinary page takes, while a Fontations scaler returns a grid-fitted one.
// That decides where the glyphs end up, not merely how they are shaded.
//
// The bounds are left as Skia computed them. They are the box generateImage
// is then asked to fill, and the two have to agree with each other more than
// either has to agree with Windows.
void OnChromiumMetrics(void* result, void* context, const void* glyph)
{
    {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            Report("generateMetrics hook reached (result=%p context=%p glyph=%p dwrite=%d)",
                   result, context, glyph, chromium_patch::ParityWanted() ? 1 : 0);
        }
    }
    if (result == nullptr || context == nullptr || glyph == nullptr ||
        !chromium_patch::ParityWanted()) {
        return;
    }
    const skia_abi::Glyph g = skia_abi::Glyph::From(glyph);
    void* typeface = skia_abi::Read<void*>(context, skia_abi::kContextTypeface);
    {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            Report("  metrics: id_known=%d id=%u typeface=%p", g.packed_id_known ? 1 : 0,
                   g.GlyphId(), typeface);
        }
    }
    if (!g.packed_id_known || typeface == nullptr) {
        return;
    }

    const skia_abi::Rec rec = skia_abi::Rec::From(context);
    const float scale_y = windows_path::DeviceScaleY(rec);
    const int ppem = static_cast<int>(std::round(scale_y * 64.0f) / 64.0f);

    std::vector<uint8_t> font;
    {
        const std::lock_guard<std::mutex> lock(g_font_mutex);
        font = FontBytesLocked(typeface);
    }
    if (font.empty()) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            Report("  metrics: no font bytes for typeface %p", typeface);
        }
        return;
    }
    const windows_path::Decision d =
        windows_path::Decide(rec, scale_y, font_facts::Describe(font, ppem));

    float advance = 0;
    if (!dwrite_raster::GlyphAdvance(typeface, font, g.GlyphId(), d, &advance)) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            Report("DirectWrite would not measure glyph %u; advances stay Skia's",
                   g.GlyphId());
        }
        return;
    }
    std::memcpy(static_cast<unsigned char*>(result) + skia_abi::kMetricsAdvanceX,
                &advance, sizeof(advance));

    static std::atomic<uint64_t> count{0};
    const uint64_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3) {
        Report("advance for glyph %u replaced with DirectWrite's %.3f", g.GlyphId(),
               static_cast<double>(advance));
    }
}

// Runs after Skia's own generateFontMetrics, on what it produced.
//
// These are the font-wide numbers a line box is built from, and Blink rounds
// ascent, descent and leading separately before adding them, so a fraction of
// a pixel here lands as a whole pixel of line height.
void OnChromiumFontMetrics(void* context, void* metrics)
{
    if (context == nullptr || metrics == nullptr || !chromium_patch::ParityWanted()) {
        return;
    }
    void* typeface = skia_abi::Read<void*>(context, skia_abi::kContextTypeface);
    if (typeface == nullptr) {
        return;
    }
    const skia_abi::Rec rec = skia_abi::Rec::From(context);
    const float scale_y = windows_path::DeviceScaleY(rec);
    const int ppem = static_cast<int>(std::round(scale_y * 64.0f) / 64.0f);

    std::vector<uint8_t> font;
    {
        const std::lock_guard<std::mutex> lock(g_font_mutex);
        font = FontBytesLocked(typeface);
    }
    if (font.empty()) {
        return;
    }
    const windows_path::Decision d =
        windows_path::Decide(rec, scale_y, font_facts::Describe(font, ppem));
    if (dwrite_raster::FontMetrics(typeface, font, d, metrics)) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            Report("font metrics now DirectWrite's");
        }
    }
}

// Runs for every glyph Skia rasterizes, ahead of the real generateImage.
// Reads the live SkGlyph and SkScalerContextRec through the offsets in
// skia_abi.h, works out what Chromium on Windows would have asked DirectWrite
// for, and fills the mask from DWriteCore. Returns whether Skia should go on
// to rasterize the glyph itself, which it does for anything declined here.
bool OnChromiumGenerateImage(void* context, const void* glyph, void* image_buffer)
{
    static std::atomic<uint64_t> count{0};
    const uint64_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;

    if (context == nullptr || glyph == nullptr) {
        return false;
    }
    const skia_abi::Glyph g = skia_abi::Glyph::From(glyph);
    if (!g.LooksPlausible()) {
        // The offsets did not describe this build.
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            Report("SkGlyph at %p does not look like one (%ux%u, mask %u); "
                   "skia_abi.h does not match this build",
                   glyph, g.width, g.height, g.mask_format);
        }
        return false;
    }

    const skia_abi::Rec rec = skia_abi::Rec::From(context);
    const float scale_y = windows_path::DeviceScaleY(rec);
    // gdiTextSize rounded to whole pixels is the ppem the gasp and bitmap
    // strike lookups are made at, the same as SkScalerContext_DW does.
    const int ppem = static_cast<int>(std::round(scale_y * 64.0f) / 64.0f);
    void* typeface = skia_abi::Read<void*>(context, skia_abi::kContextTypeface);
    // Without DirectWrite the tree takes the branch Skia takes for a font with
    // no gasp and no strike.
    const windows_path::FontFacts facts =
        chromium_patch::ParityWanted() ? FactsFor(typeface, ppem) : windows_path::FontFacts{};
    const windows_path::Decision d = windows_path::Decide(rec, scale_y, facts);

    // Enough to see what the tree decides, without a line per glyph forever.
    if (n <= 20 || (n & 0x3ff) == 1) {
        char id[48];
        if (g.packed_id_known) {
            (void)std::snprintf(id, sizeof(id), "id=%u sub=(%u,%u)", g.GlyphId(), g.SubX(),
                                g.SubY());
        } else {
            (void)std::snprintf(id, sizeof(id), "id=?");
        }
        Report("glyph #%llu %s %ux%u mask=%u | linux: gamma=%.2f "
               "contrast=%.2f hinting=%u linearMetrics=%d | windows would ask: "
               "%s %s measure=%s render=%.3f measure=%.3f gridfit=%d aa=%d [%s]",
               static_cast<unsigned long long>(n), id, g.width, g.height,
               g.mask_format, static_cast<double>(rec.DeviceGamma()),
               static_cast<double>(rec.Contrast()), rec.GetHinting(),
               rec.IsLinearMetrics() ? 1 : 0,
               windows_path::RenderingModeName(d.rendering_mode),
               windows_path::TextureTypeName(d.texture_type),
               windows_path::MeasuringModeName(d.measuring_mode),
               static_cast<double>(d.text_size_render),
               static_cast<double>(d.text_size_measure), static_cast<int>(d.grid_fit_mode),
               static_cast<int>(d.anti_alias_mode), d.branch);
        const skia_abi::PreBlend pb = skia_abi::PreBlend::From(context);
        if (pb.Applicable()) {
            Report("    preblend applicable, green ramp: 0->%u 64->%u 128->%u 192->%u 255->%u",
                   pb.g[0], pb.g[64], pb.g[128], pb.g[192], pb.g[255]);
        } else {
            Report("    preblend not applicable, coverage passes through");
        }
        Report("    font facts: hinted=%d gasp=%d gaspV1=%d symSmooth=%d strike=%d cbdt=%d",
               facts.is_hinted ? 1 : 0, facts.gasp_known ? 1 : 0,
               facts.gasp_version_1_or_later ? 1 : 0, facts.gasp_symmetric_smoothing ? 1 : 0,
               facts.has_bitmap_strike ? 1 : 0, facts.has_cbdt ? 1 : 0);
    }

    if (!chromium_patch::ParityWanted()) {
        return false;
    }

    std::vector<uint8_t> font;
    {
        const std::lock_guard<std::mutex> lock(g_font_mutex);
        font = FontBytesLocked(typeface);
    }
    if (font.empty()) {
        return false;
    }

    const skia_abi::PreBlend preblend = skia_abi::PreBlend::From(context);
    const bool drawn =
        dwrite_raster::RenderGlyph(typeface, font, rec, g, preblend, d, image_buffer);
    static std::atomic<uint64_t> drawn_count{0};
    static std::atomic<uint64_t> declined_count{0};
    const uint64_t seen =
        drawn ? drawn_count.fetch_add(1, std::memory_order_relaxed) + 1
              : declined_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen <= 3 || (seen & 0x3ff) == 1) {
        Report("%s glyph %u (%ux%u): %llu so far", drawn ? "DirectWrite drew" : "declined",
               g.GlyphId(), g.width, g.height, static_cast<unsigned long long>(seen));
    }
    // A picture of the first few masks, to show the bytes are a glyph and not
    // a misaligned buffer.
    if (drawn && std::getenv("CHROMIUM_PATCH_SHOW_MASK") != nullptr) {
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1, std::memory_order_relaxed) < 2 &&
            g.mask_format == skia_abi::kLCD16) {
            Report("LCD mask for glyph %u (%ux%u), R/G/B per pixel:", g.GlyphId(), g.width,
                   g.height);
            const auto* px = reinterpret_cast<const uint16_t*>(image_buffer);
            for (int row = 0; row < g.height; ++row) {
                char line[200];
                int at = 0;
                for (int x = 0; x < g.width && at < 190; ++x) {
                    const uint16_t v = px[static_cast<size_t>(row) * g.width + x];
                    const unsigned r = (v >> 11) & 0x1f;
                    const unsigned gg = (v >> 5) & 0x3f;
                    const unsigned b = v & 0x1f;
                    // One hex digit each, so a color fringe shows as differing
                    // digits within a pixel.
                    at += std::snprintf(line + at, sizeof(line) - static_cast<size_t>(at),
                                        "%x%x%x ", r >> 1, gg >> 2, b >> 1);
                }
                line[at] = '\0';
                Report("  |%s|", line);
            }
        } else if (shown.load(std::memory_order_relaxed) <= 2 &&
                   g.mask_format == skia_abi::kA8) {
            Report("mask for glyph %u (%ux%u):", g.GlyphId(), g.width, g.height);
            const auto* px = static_cast<const uint8_t*>(image_buffer);
            for (int row = 0; row < g.height; ++row) {
                char line[160];
                int at = 0;
                for (int x = 0; x < g.width && at < 150; ++x) {
                    const uint8_t v = px[static_cast<size_t>(row) * g.width + x];
                    line[at++] = " .:-=+*#%@"[v * 9 / 255];
                }
                line[at] = '\0';
                Report("  |%s|", line);
            }
        }
    }
    return drawn;
}

__attribute__((constructor)) static void ChromiumPatchInit()
{
    ScanLoadedImages();
    // Creating the factory needs the filesystem, so a renderer that waits
    // until it is sandboxed gets nothing. One forked from here inherits a built
    // factory, and its fonts come from memory.
    if (g_patched.load(std::memory_order_acquire) && chromium_patch::ParityWanted()) {
        (void)dwrite_raster::Available();
    }
}
