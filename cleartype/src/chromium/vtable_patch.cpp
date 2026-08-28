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

// Everything below is entered from this library's constructor, which static
// analysis does not model, so it reads the whole file as unreachable.
// ReSharper disable CppDFAUnreachableFunctionCall
//
// OnChromiumFilterRec always returns true by design: hook_thunk.S reads the
// result out of al to decide whether to run the original, and the answer here
// is always yes.
// ReSharper disable CppDFAConstantFunctionResult

// Style inspections left as they are: the shapes they suggest either read
// worse against the sources being mirrored, or would change which overload
// is chosen if one were ever added.
// ReSharper disable CppLocalVariableMayBeConst
// ReSharper disable CppParameterMayBeConstPtrOrRef
// ReSharper disable CppRedundantParentheses
// ReSharper disable CppRedundantQualifierADL
// ReSharper disable CppTemplateArgumentsCanBeDeduced
// ReSharper disable CppUseStructuredBinding
// ReSharper disable CppVariableCanBeMadeConstexpr
// ReSharper disable RadGlobal

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
#include <setjmp.h>
#include <signal.h>
#include <sys/syscall.h>
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

// The image the patch settled on, kept for the runtime work that happens
// after the scan has finished.
Image g_image;

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
        if (const auto* name = reinterpret_cast<const char*>(base + shstrtab.sh_offset + sh.sh_name);
            std::strcmp(name, ".dynsym") == 0) {
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
std::atomic g_patched{false};

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
        const auto it = std::ranges::upper_bound(starts, addr);
        if (it == starts.begin()) {
            return static_cast<size_t>(-1);
        }
        return static_cast<size_t>(std::prev(it) - starts.begin());
    }

    bool Has(const uintptr_t fn, const uint8_t bit) const
    {
        const auto it = std::ranges::lower_bound(starts, fn);
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
            if (const uintptr_t target = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
                InText(image, target)) {
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
            if (const uintptr_t target = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
                InText(image, target)) {
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
// The two writes a typeface vtable gets when it is identified before Skia is
// dispatching through it: the rec filter that applies the Windows render
// params, and the table read that hides VDMX.
void PatchTypefaceSlots(void** base, void** data_slot)
{
    if (void** slot = base + kFilterRecSlot; InText(g_image, reinterpret_cast<uintptr_t>(*slot))) {
        g_original_filter_rec = *slot;
        if (WriteSlot(slot, reinterpret_cast<void*>(&chromium_filter_rec_thunk))) {
            Report("onFilterRec %p replaced through vtable slot %p; the Windows font "
                   "render params will be applied to every scaler context",
                   g_original_filter_rec, static_cast<void*>(slot));
        } else {
            g_original_filter_rec = nullptr;
        }
    } else {
        Report("slot %u of that vtable is not a function", kFilterRecSlot);
    }

    if (data_slot != nullptr && InText(g_image, reinterpret_cast<uintptr_t>(*data_slot))) {
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
    const auto* end =
        reinterpret_cast<const uintptr_t*>(image.relro.end);
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
    PatchTypefaceSlots(base, tags_slot + 1);
}

// The tag SkFontationsScalerContext::generateFontMetrics reads, from
// src/ports/SkTypeface_fontations.cpp. It is an immediate in that function, so
// it names the scaler context on a build that exports nothing. Other code
// holds the same constant, so the shape test below is required as well.
constexpr uint32_t kFvarTag = 0x66766172;

// The tag SkTypeface_Fontations::onGlyphMaskNeedsCurrentColor reads, from the
// same file. It names the typeface vtable the way fvar names the scaler
// context's.
constexpr uint32_t kColrTag = 0x434F4C52;

// How far apart two functions from one source file can sit.
constexpr uintptr_t kSameFileSpan = 0x80000;

// FT_GLYPH_FORMAT_OUTLINE. A build predating the Rust bridge rasterizes with
// a FreeType compiled into it, and SkScalerContext_FreeType::generateMetrics
// tests the loaded glyph against this format
// (src/ports/SkFontHost_FreeType.cpp).
constexpr uint32_t kOutlineFormat = 0x6F75746C;

// Where the two table virtuals sit relative to it. SkTypeface declares
// onGetTableTags and onGetTableData last but for onCopyTableData and
// onComputeBounds, which puts them fourteen and fifteen slots along.
constexpr unsigned kColrToTableTags = 14;

// onFilterRec relative to it. SkTypeface declares onFilterRec near the top and
// onGlyphMaskNeedsCurrentColor six virtuals later. The table calls have moved
// between revisions; this pair has not.
constexpr unsigned kColrToFilterRec = 6;

// Where that virtual sits in a Fontations typeface vtable. Unrelated tables
// hold the tag too, and the index tells them apart.
constexpr size_t kColrSlot = 15;

// Virtuals that reach the COLR tag, directly or through the SkOnce lambda.
std::vector<uintptr_t> g_colr_sites;
std::vector<uintptr_t> g_vt_starts;
std::vector<uintptr_t> g_colr_fns;

// Patch onFilterRec on the typeface vtable a COLR-holding virtual belongs to.
// This is what an older build gets: its scaler context cannot be named, but
// the render params still reach every scaler context Skia builds.
bool PatchFilterRecByColrTag(const Image& image, const CodeMap& map,
                             const std::vector<uintptr_t>& colr_sites,
                             const size_t want_index)
{
    if (colr_sites.empty()) {
        return false;
    }
    const auto holds = [&](const uintptr_t fn) {
        if (std::ranges::binary_search(g_colr_fns, fn)) {
            return true;
        }
        // A tag anywhere before the next function a vtable names counts. It
        // can sit past a branch, outside the site's own function.
        if (!g_vt_starts.empty()) {
            const auto next = std::ranges::upper_bound(g_vt_starts, fn);
            const uintptr_t stop = next != g_vt_starts.end()
                                       ? *next
                                       : reinterpret_cast<uintptr_t>(image.text[0].end);
            const auto at = std::ranges::lower_bound(g_colr_sites, fn);
            if (at != g_colr_sites.end() && *at < stop) {
                return true;
            }
        }
        const auto it = std::ranges::lower_bound(colr_sites, fn);
        for (auto s = it; s != colr_sites.end() && *s < fn + 0x20000; ++s) {
            if (const size_t i = map.IndexOf(*s);
                i != static_cast<size_t>(-1) && map.starts[i] == fn) {
                return true;
            }
        }
        return false;
    };
    auto* p = reinterpret_cast<uintptr_t*>(const_cast<unsigned char*>(image.relro.begin));
    auto* end = reinterpret_cast<uintptr_t*>(const_cast<unsigned char*>(image.relro.end));
    unsigned found = 0;
    void** chosen = nullptr;
    uintptr_t colr_fn = 0;
    bool mixed = false;
    std::vector<void**> slots_to_patch;
    for (; p + 1 <= end; ++p) {
        if (!InText(image, *p) || !holds(*p)) {
            continue;
        }
        auto* base = VtableBaseFrom(image, reinterpret_cast<void**>(p));
        if (base == nullptr) {
            continue;
        }
        const auto index = static_cast<size_t>(p - reinterpret_cast<uintptr_t*>(base));
        if (index < kColrToFilterRec || (want_index != 0 && index != want_index)) {
            continue;
        }
        void** slot = base + (index - kColrToFilterRec);
        if (!InText(image, reinterpret_cast<uintptr_t>(*slot))) {
            continue;
        }
        // A typeface vtable runs well past this virtual. A shorter table
        // holding the same tag is something else.
        const size_t tail = index + 12;
        if (reinterpret_cast<unsigned char*>(base + tail + 1) > image.relro.end) {
            continue;
        }
        bool long_enough = true;
        for (size_t k = index + 1; k <= tail && long_enough; ++k) {
            if (!InText(image, reinterpret_cast<uintptr_t>(base[k]))) {
                long_enough = false;
            }
        }
        if (!long_enough) {
            continue;
        }
        ++found;
        if (chosen == nullptr) {
            chosen = slot;
            colr_fn = *p;
        } else if (*p != colr_fn) {
            mixed = true;
        }
        slots_to_patch.push_back(slot);
    }
    // Sibling typefaces share one implementation, so several vtables name the
    // same virtual. That is expected; what is not is two different functions
    // answering to the tag, which would mean the wrong class is in the set.
    if (chosen == nullptr || mixed) {
        if (want_index == 0) {
            Report("%u typeface vtables hold the COLR tag%s; not applying the render "
                   "params through them", found, mixed ? ", naming different virtuals" : "");
        }
        return false;
    }
    g_original_filter_rec = *chosen;
    unsigned written = 0;
    for (void** s : slots_to_patch) {
        if (*s == g_original_filter_rec &&
            WriteSlot(s, reinterpret_cast<void*>(&chromium_filter_rec_thunk))) {
            ++written;
        }
    }
    if (written != 0) {
        Report("onFilterRec %p replaced through %u vtable slot(s), found by the COLR "
               "tag; the Windows font render params will be applied",
               g_original_filter_rec, written);
    } else {
        g_original_filter_rec = nullptr;
    }
    return written != 0;
}

// SkFontationsScalerContext answers per-glyph questions from generateMetrics
// and font-wide ones from generateFontMetrics, so those two slots are the ones
// that must reach the bridge.
constexpr unsigned kRecognizeFirst = 2;    // generateMetrics
constexpr unsigned kRecognizeSecond = 6;   // generateFontMetrics

// generateImage's place in that table, which is where chosen_slot points.
constexpr unsigned kScalerImageSlot = 3;

// head is 54 bytes in every font and carries a fixed magic number, so a slot
// that answers with both is onGetTableData and not something that happens to
// return 54.
constexpr uint32_t kHeadTag = 0x68656164;
constexpr size_t kHeadSize = 54;
constexpr uint32_t kHeadMagic = 0x5F0F3CF5;
constexpr size_t kHeadMagicAt = 12;

// Finding onGetTableData means calling a slot to see what it answers, and a
// slot that is not onGetTableData takes a pointer where the tag goes and
// writes through 0x68656164. The fault is caught so a wrong guess costs the
// resolve instead of the process.
sigjmp_buf g_probe_jmp;
std::atomic<long> g_probe_tid{0};
struct sigaction g_probe_old_segv;
struct sigaction g_probe_old_bus;
std::atomic g_probe_faulted{false};

void ProbeFaultHandler(int sig, siginfo_t* info, void* uc)
{
    // Only this thread faulting on the tag itself belongs to the guard.
    // Anything else is a real crash and goes to the handler already there,
    // which is the one that would have run had the guard not been installed.
    const bool mine =
        info != nullptr && syscall(SYS_gettid) == g_probe_tid.load(std::memory_order_acquire) &&
        reinterpret_cast<uintptr_t>(info->si_addr) == kHeadTag;
    if (!mine) {
        const struct sigaction& old = sig == SIGBUS ? g_probe_old_bus : g_probe_old_segv;
        if ((old.sa_flags & SA_SIGINFO) != 0 && old.sa_sigaction != nullptr) {
            old.sa_sigaction(sig, info, uc);
        } else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
            old.sa_handler(sig);
        } else {
            (void)sigaction(sig, &old, nullptr);
        }
        return;
    }
    siglongjmp(g_probe_jmp, 1);
}

// Reads head through the candidate slot. False means the call faulted or the
// bytes are not head, and either way the slot is not the one.
bool ReadsHeadTable(const GetTableDataFn probe, const void* typeface)
{
    // Only kHeadSize is asked for, but the callee is not known to be
    // onGetTableData yet, and one that ignores the length writes a whole
    // table. The slack keeps that inside the buffer.
    unsigned char head[512] = {};
    struct sigaction guard = {};
    guard.sa_sigaction = &ProbeFaultHandler;
    guard.sa_flags = SA_SIGINFO | SA_NODEFER;
    (void)sigemptyset(&guard.sa_mask);
    if (sigaction(SIGSEGV, &guard, &g_probe_old_segv) != 0) {
        return false;
    }
    if (sigaction(SIGBUS, &guard, &g_probe_old_bus) != 0) {
        (void)sigaction(SIGSEGV, &g_probe_old_segv, nullptr);
        return false;
    }
    g_probe_tid.store(syscall(SYS_gettid), std::memory_order_release);
    size_t got = 0;
    bool called = false;
    if (sigsetjmp(g_probe_jmp, 1) == 0) {
        got = probe(typeface, kHeadTag, 0, kHeadSize, head);
        called = true;
    } else {
        g_probe_faulted.store(true, std::memory_order_release);
    }
    g_probe_tid.store(0, std::memory_order_release);
    (void)sigaction(SIGSEGV, &g_probe_old_segv, nullptr);
    (void)sigaction(SIGBUS, &g_probe_old_bus, nullptr);
    if (!called || got != kHeadSize) {
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, head + kHeadMagicAt, sizeof(magic));
    return __builtin_bswap32(magic) == kHeadMagic;
}

std::atomic g_typeface_resolved{false};

// The typeface vtable, taken off a live scaler context rather than searched
// for. SkScalerContext holds fTypeface, so that object's vtable is the one
// Skia is really using and there is nothing to disambiguate.
//
// Its COLR slot is found by the tag, and the table calls sit a fixed distance
// along.
void ResolveTypefaceFromContext(const void* context, const bool may_patch)
{
    static std::atomic patched{false};
    if (g_typeface_resolved.load(std::memory_order_acquire) &&
        (!may_patch || patched.load(std::memory_order_acquire))) {
        return;
    }
    if (may_patch) {
        patched.store(true, std::memory_order_release);
    }
    g_typeface_resolved.store(true, std::memory_order_release);
    if (g_image.text_count == 0 || context == nullptr) {
        return;
    }
    const auto* typeface = skia_abi::Read<const void*>(context, skia_abi::kContextTypeface);
    if (typeface == nullptr) {
        return;
    }
    auto* vptr = *static_cast<uintptr_t* const*>(typeface);
    auto* base = VtableBaseFrom(g_image, reinterpret_cast<void**>(vptr));
    if (base == nullptr) {
        return;
    }
    auto* slots = reinterpret_cast<uintptr_t*>(base);

    const auto holds_colr = [&](const uintptr_t fn) {
        if (std::ranges::binary_search(g_colr_fns, fn)) {
            return true;
        }
        const auto next = std::ranges::upper_bound(g_vt_starts, fn);
        const uintptr_t stop = next != g_vt_starts.end()
                                   ? *next
                                   : reinterpret_cast<uintptr_t>(g_image.text[0].end);
        const auto it = std::ranges::lower_bound(g_colr_sites, fn);
        return it != g_colr_sites.end() && *it < stop;
    };

    for (unsigned i = 0; i < typeface_bridge::kMaxSlotSearched; ++i) {
        if (reinterpret_cast<unsigned char*>(slots + i + 1) > g_image.relro.end) {
            break;
        }
        if (!InText(g_image, slots[i]) || !holds_colr(slots[i])) {
            continue;
        }
        // How far the table calls sit past this virtual moves between Skia
        // revisions, so the slot answers for itself. Every distance is tried
        // and the guard absorbs the ones that land on a virtual taking a
        // pointer where the tag goes.
        // Only the distances Skia has actually used, nearest first. Calling a
        // slot that is not onGetTableData is destructive even when the fault
        // is caught, because unwinding out of a half-run Skia virtual leaves
        // its state broken, so a version matching neither refuses instead of
        // guessing. 12 leads because on the revisions where it is wrong it
        // returns harmlessly, while a wrong 14 writes through the tag.
        constexpr size_t kKnownDistances[] = {12, kColrToTableTags};
        size_t tags_at = 0;
        for (const size_t d : kKnownDistances) {
            const size_t at = i + d;
            if (at + 2 > typeface_bridge::kMaxSlotSearched ||
                reinterpret_cast<unsigned char*>(slots + at + 2) > g_image.relro.end) {
                continue;
            }
            if (!InText(g_image, slots[at]) || !InText(g_image, slots[at + 1])) {
                continue;
            }
            if (ReadsHeadTable(reinterpret_cast<GetTableDataFn>(slots[at + 1]), typeface)) {
                tags_at = at;
                break;
            }
        }
        if (g_probe_faulted.load(std::memory_order_acquire)) {
            Report("a vtable slot faulted while being probed for onGetTableData; "
                   "the search went on past it");
        }
        if (tags_at == 0) {
            return;
        }
        const std::vector<uintptr_t> tag_fns{slots[tags_at]};
        const std::vector<uintptr_t> data_fns{
            slots[tags_at + 1], reinterpret_cast<uintptr_t>(&ChromiumGetTableData)};
        typeface_bridge::SetAnchors(tag_fns, data_fns);
        typeface_bridge::SetSlotHint(vptr, static_cast<unsigned>(tags_at),
                                     static_cast<unsigned>(tags_at + 1));
        Report("typeface vtable %p resolved from a live scaler context: "
               "COLR slot %u, onGetTableTags slot %zu, onGetTableData %zu",
               static_cast<void*>(base), i, tags_at, tags_at + 1);
        // The table slots are left alone. Skia dispatches through them while
        // a scaler context is alive, and the bridge reads the tables through
        // the slots it was told about.
        //
        // onFilterRec is read when a scaler context is built, so writing it
        // reaches every context made after this.
        if (may_patch && g_original_filter_rec == nullptr) {
            if (void** slot = reinterpret_cast<void**>(base) + kFilterRecSlot;
                InText(g_image, reinterpret_cast<uintptr_t>(*slot))) {
                g_original_filter_rec = *slot;
                if (WriteSlot(slot, reinterpret_cast<void*>(&chromium_filter_rec_thunk))) {
                    Report("onFilterRec %p replaced through the live typeface vtable",
                           g_original_filter_rec);
                } else {
                    g_original_filter_rec = nullptr;
                }
            }
        }
        // onGetTableData hides VDMX, which Blink otherwise takes a font's
        // ascent and descent from. The write is one aligned pointer, so a call
        // in flight sees the old function or the new.
        if (may_patch && g_original_get_table_data == nullptr) {
            if (auto* slot = reinterpret_cast<void**>(slots + tags_at + 1);
                InText(g_image, reinterpret_cast<uintptr_t>(*slot))) {
                g_original_get_table_data = reinterpret_cast<GetTableDataFn>(*slot);
                if (WriteSlot(slot, reinterpret_cast<void*>(&ChromiumGetTableData))) {
                    Report("onGetTableData %p replaced through the live typeface "
                           "vtable; VDMX will report as absent, as it does on Windows",
                           reinterpret_cast<void*>(g_original_get_table_data));
                } else {
                    g_original_get_table_data = nullptr;
                }
            }
        }
        return;
    }
    Report("no slot of the live typeface vtable holds the COLR tag");
}

// The address a rip-relative lea reaches, which is how position-independent
// code names a vtable.
bool LeaTarget(const unsigned char* q, uintptr_t* target)
{
    if (q[0] != 0x48 || q[1] != 0x8D || (q[2] & 0xC7) != 0x05) {
        return false;
    }
    int32_t disp = 0;
    std::memcpy(&disp, q + 3, sizeof(disp));
    *target = reinterpret_cast<uintptr_t>(q + 7) + static_cast<uintptr_t>(disp);
    return true;
}

// Find the typeface through the scaler context it builds.
// SkTypeface_Fontations::onCreateScalerContext names that vtable's address, so
// the vtable entry holding that function is the typeface's.
bool PatchTypefaceByScalerRef(const Image& image, const CodeMap& map, const uintptr_t sc_vptr)
{
    std::vector<uintptr_t> makers;
    for (unsigned s = 0; s < image.text_count; ++s) {
        const Region& r = image.text[s];
        for (const unsigned char* q = r.begin; q + 7 <= r.end; ++q) {
            uintptr_t target = 0;
            if (!LeaTarget(q, &target) || target != sc_vptr) {
                continue;
            }
            if (const size_t i = map.IndexOf(reinterpret_cast<uintptr_t>(q));
                i != static_cast<size_t>(-1)) {
                makers.push_back(map.starts[i]);
            }
        }
    }
    std::ranges::sort(makers);
    makers.erase(std::ranges::unique(makers).begin(), makers.end());
    if (makers.empty()) {
        return false;
    }

    const auto holds_colr = [&](const uintptr_t fn) {
        if (std::ranges::binary_search(g_colr_fns, fn)) {
            return true;
        }
        const auto next = std::ranges::upper_bound(g_vt_starts, fn);
        const uintptr_t stop = next != g_vt_starts.end()
                                   ? *next
                                   : reinterpret_cast<uintptr_t>(image.text[0].end);
        const auto it = std::ranges::lower_bound(g_colr_sites, fn);
        return it != g_colr_sites.end() && *it < stop;
    };

    unsigned patched = 0;
    auto* p = reinterpret_cast<uintptr_t*>(const_cast<unsigned char*>(image.relro.begin));
    auto* end = reinterpret_cast<uintptr_t*>(const_cast<unsigned char*>(image.relro.end));
    for (; p + 1 <= end; ++p) {
        if (!InText(image, *p) || !std::ranges::binary_search(makers, *p)) {
            continue;
        }
        auto* base = VtableBaseFrom(image, reinterpret_cast<void**>(p));
        if (base == nullptr) {
            continue;
        }
        auto* slots = reinterpret_cast<uintptr_t*>(base);
        void** data_slot = nullptr;
        for (unsigned i = 0; i < typeface_bridge::kMaxSlotSearched; ++i) {
            if (reinterpret_cast<unsigned char*>(slots + i + 1) > image.relro.end) {
                break;
            }
            if (!InText(image, slots[i]) || !holds_colr(slots[i])) {
                continue;
            }
            const size_t tags_at = i + kColrToTableTags;
            if (reinterpret_cast<unsigned char*>(slots + tags_at + 2) <= image.relro.end &&
                InText(image, slots[tags_at + 1])) {
                data_slot = reinterpret_cast<void**>(slots + tags_at + 1);
            }
            break;
        }
        // A subclass that overrides nothing gets its own table, and the
        // typeface Skia hands out may be of that subclass.
        Report("typeface vtable %p found by the scaler context it builds",
               static_cast<void*>(base));
        PatchTypefaceSlots(base, data_slot);
        ++patched;
    }
    return patched != 0;
}

void TryPatchModule(const uintptr_t base, const ElfW(Phdr)* phdr, const ElfW(Half) phnum,
                    const char* path, const std::vector<FfiSymbol>& syms)
{
    Image image;
    if (!DescribeImage(base, phdr, phnum, &image)) {
        Report("%s has no shape this can read", path);
        return;
    }

    g_image = image;

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
    std::ranges::sort(ffi);
    std::ranges::sort(ffi_raster);

    const auto is_ffi = [&](const uintptr_t a) {
        return std::ranges::binary_search(ffi, a);
    };
    const auto is_raster = [&](const uintptr_t a) {
        return std::ranges::binary_search(ffi_raster, a);
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
    std::ranges::sort(map.starts);
    map.starts.erase(std::ranges::unique(map.starts).begin(), map.starts.end());
    map.flags.assign(map.starts.size(), 0);

    std::vector<uintptr_t> ffi_sites;
    std::vector<uintptr_t> raster_sites;
    ForEachBranch(image, [&](const uintptr_t site, const uintptr_t target) {
        if (!is_ffi(target)) {
            return;
        }
        ffi_sites.push_back(site);
        if (is_raster(target)) {
            raster_sites.push_back(site);
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

    // Hops outward from the anchors. One is enough where a debug build has
    // left the C++ wrapper out of line, but a virtual can reach the bridge
    // through more layers than that, so the propagation is repeated. The round
    // count is bounded so a mistake cannot spread across the whole image.
    constexpr unsigned kHops = 2;
    for (unsigned hop = 0; hop < kHops; ++hop) {
        std::vector<size_t> newly;
        ForEachBranch(image, [&](const uintptr_t site, const uintptr_t target) {
            if (!map.Has(target, kCallsFfi) && !map.Has(target, kCallsFfiCaller)) {
                return;
            }
            if (const size_t i = map.IndexOf(site);
                i != static_cast<size_t>(-1) && (map.flags[i] & kCallsFfiCaller) == 0) {
                newly.push_back(i);
            }
        });
        if (newly.empty()) {
            break;
        }
        for (const size_t i : newly) {
            map.flags[i] |= kCallsFfiCaller;
        }
    }

    // Where nothing is exported there are no anchor calls to find, so the
    // scaler context is recognized by the tag its generateFontMetrics loads.
    std::vector<uintptr_t> fvar_sites;
    if (syms.empty()) {
        std::vector<uintptr_t> outline_sites;
        std::vector<uintptr_t> starts_seed;
        ForEachCall(image, [&](uintptr_t, const uintptr_t target) {
            starts_seed.push_back(target);
        });
        std::ranges::sort(starts_seed);
        starts_seed.erase(std::ranges::unique(starts_seed).begin(), starts_seed.end());
        for (unsigned s = 0; s < image.text_count; ++s) {
            const Region& r = image.text[s];
            for (const unsigned char* q = r.begin; q + 4 <= r.end; ++q) {
                uint32_t v;
                std::memcpy(&v, q, sizeof(v));
                if (v == kOutlineFormat) {
                    outline_sites.push_back(reinterpret_cast<uintptr_t>(q));
                }
            }
        }
        std::ranges::sort(outline_sites);
        // The functions those constants sit in. A build old enough to use the
        // FreeType scaler reaches them from generateMetrics, sometimes through
        // a helper, so they serve as anchors and the hops below do the rest.
        for (const uintptr_t s2 : outline_sites) {
            if (const auto it = std::ranges::upper_bound(starts_seed, s2);
                it != starts_seed.begin()) {
                ffi.push_back(*std::prev(it));
            }
        }
        std::ranges::sort(ffi);
        ffi.erase(std::ranges::unique(ffi).begin(), ffi.end());
        for (unsigned s = 0; s < image.text_count; ++s) {
            const Region& r = image.text[s];
            for (const unsigned char* q = r.begin; q + 4 <= r.end; ++q) {
                uint32_t v;
                std::memcpy(&v, q, sizeof(v));
                if (v == kColrTag) {
                    g_colr_sites.push_back(reinterpret_cast<uintptr_t>(q));
                }
            }
        }
        std::ranges::sort(g_colr_sites);
        // The tag is loaded inside the SkOnce lambda
        // onGlyphMaskNeedsCurrentColor runs, which some builds keep as its own
        // function, so a virtual that only calls the holder counts as well.
        std::vector<uintptr_t> holders;
        for (const uintptr_t site : g_colr_sites) {
            if (const auto it = std::ranges::upper_bound(starts_seed, site);
                it != starts_seed.begin()) {
                holders.push_back(*std::prev(it));
            }
        }
        std::ranges::sort(holders);
        holders.erase(std::ranges::unique(holders).begin(), holders.end());
        g_colr_fns = holders;
        ForEachCall(image, [&](const uintptr_t site, const uintptr_t target) {
            if (!std::ranges::binary_search(holders, target)) {
                return;
            }
            if (const auto it = std::ranges::upper_bound(starts_seed, site);
                it != starts_seed.begin()) {
                g_colr_fns.push_back(*std::prev(it));
            }
        });
        std::ranges::sort(g_colr_fns);
        g_colr_fns.erase(std::ranges::unique(g_colr_fns).begin(), g_colr_fns.end());
        const auto* w = reinterpret_cast<const uintptr_t*>(image.relro.begin);
        for (const auto* w_end = reinterpret_cast<const uintptr_t*>(image.relro.end);
             w + 1 <= w_end; ++w) {
            if (InText(image, *w)) {
                g_vt_starts.push_back(*w);
            }
        }
        std::ranges::sort(g_vt_starts);
        g_vt_starts.erase(std::ranges::unique(g_vt_starts).begin(), g_vt_starts.end());
    }
    if (syms.empty()) {
        const auto tag = kFvarTag;
        for (unsigned s = 0; s < image.text_count; ++s) {
            const Region& r = image.text[s];
            for (const unsigned char* q = r.begin; q + 4 <= r.end; ++q) {
                uint32_t v;
                std::memcpy(&v, q, sizeof(v));
                if (v == tag) {
                    fvar_sites.push_back(reinterpret_cast<uintptr_t>(q));
                }
            }
        }
        std::ranges::sort(fvar_sites);
    }

    std::ranges::sort(ffi_sites);
    std::ranges::sort(raster_sites);

    // Every text address a vtable holds is a function entry, so consecutive
    // ones bound a function from above. The dense start set does not: it has
    // entries inside large functions, and asking only whether the enclosing
    // start carries the flag loses any call that sits past one of them. That
    // is what hides a scaler context whose generate* virtuals are large.
    std::vector<uintptr_t> vt_starts;
    for (const uintptr_t* q = relro; q + 1 <= relro_end; ++q) {
        if (InText(image, *q)) {
            vt_starts.push_back(*q);
        }
    }
    std::ranges::sort(vt_starts);
    vt_starts.erase(std::ranges::unique(vt_starts).begin(), vt_starts.end());

    const auto site_within = [&](const std::vector<uintptr_t>& sites, const uintptr_t fn) {
        const auto next = std::ranges::upper_bound(vt_starts, fn);
        const uintptr_t end = next != vt_starts.end()
                                  ? *next
                                  : reinterpret_cast<uintptr_t>(image.text[0].end);
        const auto it = std::ranges::lower_bound(sites, fn);
        return it != sites.end() && *it < end;
    };

    const auto reaches = [&](const uintptr_t fn) {
        return map.Has(fn, kCallsFfi) || map.Has(fn, kCallsFfiCaller) ||
               site_within(ffi_sites, fn);
    };

    unsigned found = 0;
    unsigned shaped_total = 0;
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
        ++shaped_total;
        if (syms.empty()) {
            // Either scaler: the Rust one names fvar in generateFontMetrics,
            // the compiled-in FreeType one names the outline format in
            // generateMetrics.
            if (!site_within(fvar_sites, slots[kRecognizeSecond]) &&
                !reaches(slots[kRecognizeFirst])) {
                continue;
            }
        } else if (!reaches(slots[kRecognizeFirst]) || !reaches(slots[kRecognizeSecond])) {
            continue;
        }
        int raster = 0;
        for (unsigned i = 2; i < kScalerContextVirtuals; ++i) {
            if (map.Has(slots[i], kCallsRaster) || site_within(raster_sites, slots[i])) {
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
        Report("%s: %u vtables have the SkScalerContext shape, none of them reaching "
               "the bridge", path, shaped_total);
        if (syms.empty() && chromium_patch::ParityWanted()) {
            std::vector<uintptr_t> colr;
            for (unsigned s = 0; s < image.text_count; ++s) {
                const Region& r = image.text[s];
                for (const unsigned char* q = r.begin; q + 4 <= r.end; ++q) {
                    uint32_t v;
                    std::memcpy(&v, q, sizeof(v));
                    if (v == kColrTag) {
                        colr.push_back(reinterpret_cast<uintptr_t>(q));
                    }
                }
            }
            std::ranges::sort(colr);
            if (!PatchFilterRecByColrTag(image, map, colr, kColrSlot)) {
                PatchFilterRecByColrTag(image, map, colr, 0);
            }
        }
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
    if (syms.empty()) {
        // Nothing is exported to anchor on, so the typeface is recognized by
        // the COLR tag its onGlyphMaskNeedsCurrentColor loads. Unrelated code
        // holds that tag, so only the sites near the scaler context the fvar
        // tag found are considered.
        std::vector<uintptr_t> near_fontations;
        for (const uintptr_t site : g_colr_sites) {
            const auto it = std::ranges::lower_bound(fvar_sites, site);
            const bool close =
                (it != fvar_sites.end() && *it - site < kSameFileSpan) ||
                (it != fvar_sites.begin() && site - *std::prev(it) < kSameFileSpan);
            if (close) {
                near_fontations.push_back(site);
            }
        }
        if (chromium_patch::ParityWanted()) {
            const auto sc_vptr = reinterpret_cast<uintptr_t>(chosen_slot - kScalerImageSlot);
            const std::vector<uintptr_t>& sites =
                near_fontations.empty() ? g_colr_sites : near_fontations;
            if (!PatchFilterRecByColrTag(image, map, sites, kColrSlot) &&
                !PatchTypefaceByScalerRef(image, map, sc_vptr)) {
                PatchFilterRecByColrTag(image, map, sites, 0);
            }
        }
    } else {
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
                if (constexpr size_t kLargestThunk = 64; end - start > kLargestThunk) {
                    return;
                }
                (target == tag_sym ? tag_targets : data_targets).push_back(start);
            });
        }
        std::ranges::sort(tag_targets);
        std::ranges::sort(data_targets);

        std::vector<uintptr_t> tag_sites;
        std::vector<uintptr_t> data_sites;
        if (tag_sym != 0 && data_sym != 0) {
            ForEachBranch(image, [&](const uintptr_t site, const uintptr_t target) {
                if (std::ranges::binary_search(tag_targets, target)) {
                    tag_sites.push_back(site);
                } else if (std::ranges::binary_search(data_targets, target)) {
                    data_sites.push_back(site);
                }
            });
        }

        // One more hop, the same allowance the anchors get above: the virtual
        // may call a helper that holds the table call rather than making it
        // itself. Such a helper starts no vtable entry, so a site inside it is
        // attributed to whoever calls it instead.
        const auto hop_to_callers = [&](std::vector<uintptr_t>* sites) {
            std::vector<uintptr_t> holders;
            for (const uintptr_t s : *sites) {
                if (const auto it = std::ranges::upper_bound(map.starts, s);
                    it != map.starts.begin()) {
                    holders.push_back(*std::prev(it));
                }
            }
            std::ranges::sort(holders);
            holders.erase(std::ranges::unique(holders).begin(), holders.end());
            if (holders.empty()) {
                return;
            }
            ForEachBranch(image, [&](const uintptr_t site, const uintptr_t target) {
                if (std::ranges::binary_search(holders, target)) {
                    sites->push_back(site);
                }
            });
        };
        hop_to_callers(&tag_sites);
        hop_to_callers(&data_sites);

        std::ranges::sort(tag_sites);
        std::ranges::sort(data_sites);

        const auto has_site_in = [](const std::vector<uintptr_t>& sites, const uintptr_t lo,
                                    const uintptr_t hi) {
            const auto it = std::ranges::lower_bound(sites, lo);
            return it != sites.end() && *it < hi;
        };

        std::vector<uintptr_t> vtable_entries;
        for (const uintptr_t* p = relro; p + 1 <= relro_end; ++p) {
            if (InText(image, *p)) {
                vtable_entries.push_back(*p);
            }
        }
        std::ranges::sort(vtable_entries);
        vtable_entries.erase(std::ranges::unique(vtable_entries).begin(), vtable_entries.end());

        std::vector<uintptr_t> calls_tags;
        std::vector<uintptr_t> calls_data;
        for (const uintptr_t fn : vtable_entries) {
            const auto it = std::ranges::upper_bound(map.starts, fn);
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

    // Every vtable that names the same three functions, not just the one the
    // search settled on. A subclass that overrides nothing gets its own table
    // holding the same pointers, and the object Skia hands out may be of that
    // subclass, so patching one table alone leaves the live one untouched.
    // The neighbours are checked as well, so a table that merely happens to
    // hold this function at some other index is left alone.
    void* const image_fn = *chosen_slot;
    void* const metrics_fn = *(chosen_slot - 1);
    void* const font_metrics_fn = *(chosen_slot + 3);

    std::vector<void**> tables;
    for (const uintptr_t* q = relro + 1; q + 4 <= relro_end; ++q) {
        if (auto* slot = const_cast<void**>(reinterpret_cast<void* const*>(q));
            *slot == image_fn && *(slot - 1) == metrics_fn && *(slot + 3) == font_metrics_fn) {
            tables.push_back(slot);
        }
    }
    if (tables.size() > 1) {
        Report("%s: %zu vtables name the same scaler context methods; patching each",
               path, tables.size());
    }

    g_original = reinterpret_cast<void*>(chosen_fn);
    g_original_metrics = metrics_fn;
    g_original_font_metrics = font_metrics_fn;
    bool any = false;
    for (void** slot : tables) {
        if (chromium_patch::ParityWanted() && InText(image, reinterpret_cast<uintptr_t>(metrics_fn))) {
            (void)WriteSlot(slot - 1, reinterpret_cast<void*>(&chromium_metrics_thunk));
        }
        if (chromium_patch::ParityWanted() &&
            InText(image, reinterpret_cast<uintptr_t>(font_metrics_fn))) {
            (void)WriteSlot(slot + 3, reinterpret_cast<void*>(&chromium_font_metrics_thunk));
        }
        if (WriteSlot(slot, reinterpret_cast<void*>(&chromium_hook_thunk))) {
            any = true;
        }
    }
    if (any) {
        Report("%s: generateImage %#lx replaced through %zu vtable slot(s)", path, chosen_fn,
               tables.size());
        g_patched.store(true, std::memory_order_release);
    } else {
        g_original = nullptr;
        g_original_metrics = nullptr;
        g_original_font_metrics = nullptr;
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
    if (auto* list = static_cast<ModuleList*>(out); list->count < ModuleList::kMax) {
        list->mods[list->count++] = {.name = info->dlpi_name,
                                     .base = info->dlpi_addr,
                                     .phdr = info->dlpi_phdr,
                                     .phnum = info->dlpi_phnum};
    }
    return 0;
}

std::atomic g_done{false};

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
        // The main executable only, which is where Chromium keeps this
        // function whichever scaler it was built with. Looking more widely
        // would reach fontconfig's own copies of the same property names.
        //
        // No Fontations symbols are required here. This patch names its target
        // by the properties it reads and refuses when no single function reads
        // them all, so it applies to any Chromium build.
        ModuleList browser;
        dl_iterate_phdr(CollectModule, &browser);
        for (unsigned i = 0; i < browser.count; ++i) {
            const LoadedModule& m = browser.mods[i];
            if (m.name != nullptr && m.name[0] != '\0') {
                continue;
            }
            render_params_patch::Apply(m.base, m.phdr, m.phnum);
            break;
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
        const char* path = m.name != nullptr && m.name[0] != '\0' ? m.name : "/proc/self/exe";
        std::vector<FfiSymbol> syms;
        // Well under what a real Fontations build exports, but enough to rule
        // out a few unrelated dynsym entries that contain the substring.
        if (!FindFfiSymbols(path, "fontations_ffi", &syms) || syms.size() < 10) {
            // A build can link the same Skia with nothing exported. Only the
            // executable itself is worth the scan, and TryPatchModule refuses
            // when the tag anchor finds no vtable of the right shape.
            if (m.name != nullptr && m.name[0] != '\0') {
                continue;
            }
            syms.clear();
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

// The caches below are keyed on the typeface pointer, which only identifies a
// font while that address still belongs to the same object. Skia frees
// typefaces and the allocator hands the address back, and the entry then
// answers for the wrong font. One reference is taken so a cached typeface
// outlives its cache entry. SkTypeface derives from SkRefCntBase, whose only
// field is the count, so it sits one pointer into the object.
void HoldTypeface(void* typeface)
{
    auto* count = reinterpret_cast<std::atomic<int32_t>*>(
        static_cast<unsigned char*>(typeface) + sizeof(void*));
    // A live typeface holds a small positive count. Anything else is not the
    // field this expects, and is left alone.
    if (const int32_t now = count->load(std::memory_order_relaxed);
        now > 0 && now < (1 << 20)) {
        (void)count->fetch_add(1, std::memory_order_relaxed);
    }
}

const std::vector<uint8_t>& FontBytesLocked(void* typeface)
{
    auto font = g_fonts.find(typeface);
    if (font == g_fonts.end()) {
        HoldTypeface(typeface);
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

windows_path::FontFacts FactsFor(void* typeface, const int gasp_ppem, const int bitmap_ppem)
{
    struct Cached
    {
        int gasp_ppem;
        int bitmap_ppem;
        windows_path::FontFacts facts;
    };
    static std::unordered_map<const void*, Cached> cache;

    const std::lock_guard lock(g_font_mutex);
    if (const auto cached = cache.find(typeface);
        cached != cache.end() && cached->second.gasp_ppem == gasp_ppem &&
        cached->second.bitmap_ppem == bitmap_ppem) {
        return cached->second.facts;
    }

    const windows_path::FontFacts facts =
        font_facts::Describe(FontBytesLocked(typeface), gasp_ppem, bitmap_ppem);
    cache[typeface] = {gasp_ppem, bitmap_ppem, facts};
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
        static std::atomic said{false};
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
    ResolveTypefaceFromContext(context, false);


    {
        static std::atomic said{false};
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
    auto* typeface = skia_abi::Read<void*>(context, skia_abi::kContextTypeface);
    {
        static std::atomic said{false};
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
    const float gdi_text_size = std::round(scale_y * 64.0f) / 64.0f;
    const int gasp_ppem = static_cast<int>(std::floor(gdi_text_size + 0.5f));
    const int bitmap_ppem = static_cast<int>(gdi_text_size);

    std::vector<uint8_t> font;
    {
        const std::lock_guard lock(g_font_mutex);
        font = FontBytesLocked(typeface);
    }
    if (font.empty()) {
        static std::atomic said{false};
        if (!said.exchange(true)) {
            Report("  metrics: no font bytes for typeface %p", typeface);
        }
        return;
    }
    const windows_path::Decision d =
        windows_path::Decide(rec, scale_y, font_facts::Describe(font, gasp_ppem, bitmap_ppem));

    float advance = 0;
    if (!dwrite_raster::GlyphAdvance(typeface, font, g.GlyphId(), d, &advance)) {
        static std::atomic said{false};
        if (!said.exchange(true)) {
            Report("DirectWrite would not measure glyph %u; advances stay Skia's",
                   g.GlyphId());
        }
        return;
    }
    std::memcpy(static_cast<unsigned char*>(result) + skia_abi::kMetricsAdvanceX,
                &advance, sizeof(advance));

    static std::atomic<uint64_t> count{0};
    if (const uint64_t n = count.fetch_add(1, std::memory_order_relaxed) + 1; n <= 3) {
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
    // Blink reads VDMX while it builds the font, so the patch has to land
    // here and not at the first glyph.
    ResolveTypefaceFromContext(context, true);


    if (context == nullptr || metrics == nullptr || !chromium_patch::ParityWanted()) {
        return;
    }
    auto* typeface = skia_abi::Read<void*>(context, skia_abi::kContextTypeface);
    if (typeface == nullptr) {
        return;
    }
    const skia_abi::Rec rec = skia_abi::Rec::From(context);
    const float scale_y = windows_path::DeviceScaleY(rec);
    const float gdi_text_size = std::round(scale_y * 64.0f) / 64.0f;
    const int gasp_ppem = static_cast<int>(std::floor(gdi_text_size + 0.5f));
    const int bitmap_ppem = static_cast<int>(gdi_text_size);

    std::vector<uint8_t> font;
    {
        const std::lock_guard lock(g_font_mutex);
        font = FontBytesLocked(typeface);
    }
    if (font.empty()) {
        return;
    }
    const windows_path::Decision d =
        windows_path::Decide(rec, scale_y, font_facts::Describe(font, gasp_ppem, bitmap_ppem));
    if (dwrite_raster::FontMetrics(typeface, font, d, metrics)) {
        static std::atomic said{false};
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
    ResolveTypefaceFromContext(context, true);

    static std::atomic<uint64_t> count{0};
    const uint64_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;

    if (context == nullptr || glyph == nullptr) {
        return false;
    }
    const skia_abi::Glyph g = skia_abi::Glyph::From(glyph);
    if (!g.LooksPlausible()) {
        // The offsets did not describe this build.
        static std::atomic said{false};
        if (!said.exchange(true)) {
            Report("SkGlyph at %p does not look like one (%ux%u, mask %u); "
                   "skia_abi.h does not match this build",
                   glyph, g.width, g.height, g.mask_format);
        }
        return false;
    }

    const skia_abi::Rec rec = skia_abi::Rec::From(context);
    const float scale_y = windows_path::DeviceScaleY(rec);
    // SkScalerContext_DW asks the gasp table at SkScalarRoundToInt(gdiTextSize)
    // and the bitmap strike at SkScalarTruncToInt(gdiTextSize), which differ
    // whenever the size has a fraction.
    const float gdi_text_size = std::round(scale_y * 64.0f) / 64.0f;
    const int gasp_ppem = static_cast<int>(std::floor(gdi_text_size + 0.5f));
    const int bitmap_ppem = static_cast<int>(gdi_text_size);
    auto* typeface = skia_abi::Read<void*>(context, skia_abi::kContextTypeface);
    // Without DirectWrite the tree takes the branch Skia takes for a font with
    // no gasp and no strike.
    const windows_path::FontFacts facts =
        chromium_patch::ParityWanted() ? FactsFor(typeface, gasp_ppem, bitmap_ppem)
                                       : windows_path::FontFacts{};
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
        if (const skia_abi::PreBlend pb = skia_abi::PreBlend::From(context); pb.Applicable()) {
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
        const std::lock_guard lock(g_font_mutex);
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
            const auto* px = static_cast<const uint16_t*>(image_buffer);
            for (int row = 0; row < g.height; ++row) {
                char line[200];
                int at = 0;
                for (int x = 0; x < g.width && at < 190; ++x) {
                    const uint16_t v = px[static_cast<size_t>(row) * g.width + static_cast<size_t>(x)];
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
                    const uint8_t v = px[static_cast<size_t>(row) * g.width + static_cast<size_t>(x)];
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
