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
#include "path_abi.h"
#include "typeface_bridge.h"
#include "font_facts.h"
#include "bold_fallback.h"
#include "bold_shaping.h"
#include "bold_weight.h"
#include "colr_outline.h"
#include "weight_style.h"
#include "dwrite_raster.h"
#include "render_params.h"
#include "parity_gate.h"
#include "windows_path.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
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
void chromium_path_thunk();
void OnChromiumFontMetrics(void* context, void* metrics);
void OnChromiumMetrics(void* result, void* context, const void* glyph);
void OnChromiumMetricsPre(void* context, const void* glyph);
void OnChromiumPath(void* result, void* context, const void* glyph);
size_t ChromiumGetTableData(const void* self, uint32_t tag, size_t offset, size_t length,
                            void* data);
bool OnChromiumFilterRec(void* self, void* rec);
bool OnChromiumGenerateImage(void* context, const void* glyph, void* image_buffer);

// Read by hook_thunk.S's tail jump. Set before the slot is patched.
void* g_original = nullptr;
void* g_original_filter_rec = nullptr;
// Where it was written, so a vtable the image scan picked by shape can be
// given its function back once the one Skia dispatches through is known.
std::vector<void**> g_filter_rec_slots;
void* g_original_metrics = nullptr;
void* g_original_font_metrics = nullptr;
void* g_original_path = nullptr;
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

// The module holding Chromium, for a symbol table read after the walk.
char g_image_path[512] = {};

// The address a build that kept its symbol table gives a class's vtable
// outright. Returns the link-time value, so the load base still has to be
// added. Chromium's own builds are stripped and answer nothing; CEF ships a
// full .symtab, and there the shape search has nothing to add.
uintptr_t NamedVtable(const char* path, const char* want)
{
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(ElfW(Ehdr)))) {
        close(fd);
        return 0;
    }
    const auto size = static_cast<size_t>(st.st_size);
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return 0;
    }
    uintptr_t found = 0;
    const auto* base = static_cast<const unsigned char*>(map);
    const auto* ehdr = reinterpret_cast<const ElfW(Ehdr)*>(base);
    if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) == 0 && ehdr->e_shoff != 0 &&
        ehdr->e_shnum != 0 && ehdr->e_shstrndx < ehdr->e_shnum &&
        static_cast<uint64_t>(ehdr->e_shoff) +
                static_cast<uint64_t>(ehdr->e_shnum) * ehdr->e_shentsize <= size) {
        const auto* shdrs = reinterpret_cast<const ElfW(Shdr)*>(base + ehdr->e_shoff);
        for (ElfW(Half) i = 0; i < ehdr->e_shnum && found == 0; ++i) {
            const ElfW(Shdr)& sh = shdrs[i];
            if ((sh.sh_type != SHT_SYMTAB && sh.sh_type != SHT_DYNSYM) ||
                sh.sh_entsize == 0 || sh.sh_link >= ehdr->e_shnum ||
                sh.sh_offset + sh.sh_size > size) {
                continue;
            }
            const ElfW(Shdr)& str = shdrs[sh.sh_link];
            if (str.sh_offset + str.sh_size > size) {
                continue;
            }
            const size_t count = sh.sh_size / sh.sh_entsize;
            const auto* syms = reinterpret_cast<const ElfW(Sym)*>(base + sh.sh_offset);
            for (size_t j = 0; j < count; ++j) {
                if (syms[j].st_shndx == SHN_UNDEF || syms[j].st_value == 0 ||
                    syms[j].st_name >= str.sh_size) {
                    continue;
                }
                const auto* name =
                    reinterpret_cast<const char*>(base + str.sh_offset + syms[j].st_name);
                if (std::strcmp(name, want) == 0) {
                    found = syms[j].st_value;
                    break;
                }
            }
        }
    }
    munmap(map, size);
    return found;
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

// How far ahead of the runner-up the winning vtable must be for the shape
// test to identify the typeface instead of merely breaking a tie. A build
// that compiles Fontations in holds several tables of the right shape and
// only one of them rasterizes.
constexpr int kRasterCallsAhead = 2;

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

void FindVariationSlot(void** base, void** data_slot);
void FindVariationSlotFromColr(void** base, size_t colr_slot);
bool PatchTypefaceByName(const Image& image);
bool ConfirmVariationSlot(void** base, size_t slot, const void* bridge);
bool HoldsColrTag(uintptr_t fn);
unsigned ChromiumMajor();

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
    FindVariationSlot(base, data_slot);
    // The axis position is the virtual after the COLR query in every Skia
    // revision, so finding that one names it without counting. The symbol
    // route never looks for it, and the count back from the table calls
    // depends on the version.
    for (unsigned i = 0; i < typeface_bridge::kMaxSlotSearched; ++i) {
        if (reinterpret_cast<unsigned char*>(base + i + 1) > g_image.relro.end) {
            break;
        }
        const auto fn = reinterpret_cast<uintptr_t>(base[i]);
        if (!InText(g_image, fn) || !HoldsColrTag(fn)) {
            continue;
        }
        FindVariationSlotFromColr(base, i);
        break;
    }
    if (void** slot = base + kFilterRecSlot; InText(g_image, reinterpret_cast<uintptr_t>(*slot))) {
        g_original_filter_rec = *slot;
        if (WriteSlot(slot, reinterpret_cast<void*>(&chromium_filter_rec_thunk))) {
            g_filter_rec_slots.push_back(slot);
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

// The typeface vtable a build that kept its symbol table names outright. The
// searches below read the table off whatever holds onGetTableTags or builds a
// scaler context, and on CEF neither is the one Skia dispatches through, which
// leaves the rec unfiltered until a scaler context turns up. By then the recs
// a page opened with are cached and keep Skia's own antialiasing rather than
// the Windows render params.
bool PatchTypefaceByName(const Image& image)
{
    for (const char* named : {"_ZTV21SkTypeface_Fontations", "_ZTV19SkTypeface_FreeType"}) {
        const uintptr_t value = NamedVtable(g_image_path, named);
        if (value == 0) {
            continue;
        }
        // A vtable symbol names the object, which opens with the offset to top
        // and the typeinfo pointer; the virtuals follow.
        auto* base = reinterpret_cast<void**>(image.base + value + 2 * sizeof(void*));
        if (!InText(image, reinterpret_cast<uintptr_t>(base[kFilterRecSlot]))) {
            continue;
        }
        Report("%s names the typeface vtable at %p", named, static_cast<void*>(base));
        PatchTypefaceSlots(base, nullptr);
        return true;
    }
    return false;
}

void InstallRecFilter(const Image& image, const std::vector<uintptr_t>& calls_tags)
{
    if (PatchTypefaceByName(image)) {
        return;
    }

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

// Whether this virtual reads the COLR tag, from the tables the module scan
// built. SkTypeface_Fontations::onGlyphMaskNeedsCurrentColor is the one that
// does, and the axis position is declared immediately after it.
// The Chromium version the patched image was built from, read from the user
// agent string it carries. The SkTypeface layout moves with it, and a count
// that was right when it was written stops describing the object otherwise.
unsigned ChromiumMajor()
{
    static const unsigned major = [] {
        struct Ask
        {
            uintptr_t base;
            unsigned found;
        } ask{g_image.base, 0};
        dl_iterate_phdr(
            [](dl_phdr_info* info, size_t, void* data) {
                auto* a = static_cast<Ask*>(data);
                if (info->dlpi_addr != a->base) {
                    return 0;
                }
                for (int i = 0; i < info->dlpi_phnum; ++i) {
                    const ElfW(Phdr)& p = info->dlpi_phdr[i];
                    if (p.p_type != PT_LOAD || (p.p_flags & (PF_X | PF_W)) != 0) {
                        continue;
                    }
                    const auto* begin =
                        reinterpret_cast<const unsigned char*>(info->dlpi_addr + p.p_vaddr);
                    const unsigned char* end = begin + p.p_filesz;
                    for (const unsigned char* q = begin; q + 12 <= end; ++q) {
                        if (std::memcmp(q, "Chrome/", 7) != 0) {
                            continue;
                        }
                        unsigned v = 0;
                        const unsigned char* d = q + 7;
                        for (; d < end && *d >= '0' && *d <= '9'; ++d) {
                            v = v * 10 + static_cast<unsigned>(*d - '0');
                        }
                        if (d < end && *d == '.' && v >= 100 && v < 1000) {
                            a->found = v;
                            return 1;
                        }
                    }
                }
                return 1;
            },
            &ask);
        if (ask.found != 0) {
            Report("the image reports Chrome/%u", ask.found);
        }
        return ask.found;
    }();
    return major;
}

bool HoldsColrTag(const uintptr_t fn)
{
    if (std::ranges::binary_search(g_colr_fns, fn)) {
        return true;
    }
    if (g_vt_starts.empty() || g_image.text_count == 0) {
        return false;
    }
    // A tag anywhere before the next function a vtable names counts, since it
    // can sit past a branch and outside the site's own function.
    const auto next = std::ranges::upper_bound(g_vt_starts, fn);
    const uintptr_t stop = next != g_vt_starts.end()
                               ? *next
                               : reinterpret_cast<uintptr_t>(g_image.text[0].end);
    const auto at = std::ranges::lower_bound(g_colr_sites, fn);
    return at != g_colr_sites.end() && *at < stop;
}

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
void ResolveTypefaceVtable(const void* typeface, bool may_patch, bool through_proxy);

// Set once the slots the patch writes have been written, which is a step past
// resolving where they are.
std::atomic g_typeface_patched{false};

void ResolveTypefaceFromContext(const void* context, const bool may_patch)
{
    if (g_typeface_resolved.load(std::memory_order_acquire) &&
        (!may_patch || g_typeface_patched.load(std::memory_order_acquire))) {
        return;
    }
    // Marked only once the search below succeeds. The first glyph can arrive
    // with a typeface this cannot read, and settling the answer on that would
    // stand the search down for the life of the process on one bad sample.
    // Every call at first, since the typefaces that reach here early are
    // mostly proxies and only a few carry the vtable being looked for. After
    // that on calls 1, 2, 4, 8 and so on, so a process that never resolves
    // settles into doing almost nothing while one that later paints through a
    // readable typeface still finds it.
    constexpr unsigned kEveryCall = 256;
    static std::atomic<unsigned> attempts{0};
    if (const unsigned n = attempts.fetch_add(1, std::memory_order_relaxed);
        n >= kEveryCall && (n & (n + 1)) != 0) {
        return;
    }
    if (g_image.text_count == 0 || context == nullptr) {
        return;
    }
    if (const auto* typeface = skia_abi::Read<const void*>(context, skia_abi::kContextTypeface);
        typeface != nullptr) {
        ResolveTypefaceVtable(typeface, may_patch, false);
    }
}

// `through_proxy` says this is the typeface behind a proxy, so the walk stops
// here rather than following another.
void ResolveTypefaceVtable(const void* typeface, const bool may_patch,
                           const bool through_proxy)
{
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
        g_typeface_resolved.store(true, std::memory_order_release);
        if (may_patch) {
            g_typeface_patched.store(true, std::memory_order_release);
        }
        Report("typeface vtable %p resolved from a live scaler context: "
               "COLR slot %u, onGetTableTags slot %zu, onGetTableData %zu",
               static_cast<void*>(base), i, tags_at, tags_at + 1);
        // The COLR query names it, since the axis position is the virtual
        // immediately after. The distance back from the table calls is not
        // usable, having moved when Chromium 146 added the two synthetic
        // style queries.
        FindVariationSlotFromColr(base, i);
        // The table slots are left alone. Skia dispatches through them while
        // a scaler context is alive, and the bridge reads the tables through
        // the slots it was told about.
        //
        // onFilterRec is read when a scaler context is built, so writing it
        // reaches every context made after this.
        // The vtable the image scan settled on is not always the one Skia
        // dispatches through, and where it is not, the rec filter never runs.
        // Patching this one as well is only sound while both hold the same
        // function, since the thunk calls through one pointer.
        if (may_patch && g_original_filter_rec != nullptr) {
            void** slot = reinterpret_cast<void**>(base) + kFilterRecSlot;
            if (*slot != reinterpret_cast<void*>(&chromium_filter_rec_thunk) &&
                InText(g_image, reinterpret_cast<uintptr_t>(*slot))) {
                if (*slot != g_original_filter_rec) {
                    // The thunk calls through one pointer, so the tables the
                    // scan guessed at are given theirs back.
                    for (void** guessed : g_filter_rec_slots) {
                        (void)WriteSlot(guessed, g_original_filter_rec);
                    }
                    g_filter_rec_slots.clear();
                    g_original_filter_rec = *slot;
                }
                if (WriteSlot(slot, reinterpret_cast<void*>(&chromium_filter_rec_thunk))) {
                    g_filter_rec_slots.push_back(slot);
                    Report("onFilterRec %p replaced on the live typeface vtable",
                           g_original_filter_rec);
                }
            }
        }
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
    // SkTypeface_FCI derives from SkTypeface_proxy and forwards the table
    // calls, so its own vtable reaches none of the bridge. The typeface it
    // proxies for does, and it is the one Skia reads the font through.
    if (const auto* inner = skia_abi::Read<const void*>(
            typeface, typeface_bridge::kProxyRealTypeface);
        inner != nullptr && inner != typeface &&
        (reinterpret_cast<uintptr_t>(inner) & 7) == 0 && !through_proxy) {
        if (const auto* inner_vptr = skia_abi::Read<const void*>(inner, 0);
            inner_vptr != nullptr && (reinterpret_cast<uintptr_t>(inner_vptr) & 7) == 0) {
            ResolveTypefaceVtable(inner, may_patch, true);
            return;
        }
    }
    static std::atomic said{false};
    if (!said.exchange(true)) {
        Report("no slot of the live typeface vtable holds the COLR tag");
    }
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
    if (chromium_patch::ParityWanted() && PatchTypefaceByName(image)) {
        return true;
    }
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
        size_t colr_at = SIZE_MAX;
        for (unsigned i = 0; i < typeface_bridge::kMaxSlotSearched; ++i) {
            if (reinterpret_cast<unsigned char*>(slots + i + 1) > image.relro.end) {
                break;
            }
            if (!InText(image, slots[i]) || !holds_colr(slots[i])) {
                continue;
            }
            colr_at = i;
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
        if (colr_at != SIZE_MAX) {
            FindVariationSlotFromColr(base, colr_at);
        }
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
    (void)std::snprintf(g_image_path, sizeof(g_image_path), "%s", path);

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
    int second_raster = -1;

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
            second_raster = best_raster;
            best_raster = raster;
            chosen_fn = slots[kGenerateImageSlot];
            chosen_slot = const_cast<void**>(
                reinterpret_cast<void* const*>(p + kGenerateImageSlot));
        } else if (raster > second_raster) {
            second_raster = raster;
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
    // More than one candidate means the shape test did not identify the
    // typeface, and patching the wrong vtable corrupts a table the browser
    // dispatches through. A tie is refused, the same way InstallRecFilter
    // refuses when onGetTableTags is in more than one table.
    if (found > 1 && best_raster - second_raster < kRasterCallsAhead) {
        Report("%s: %u vtables matched and none is clearly the scaler "
               "context's (best %d, runner-up %d rasterization-only calls); "
               "leaving them alone", path, found, best_raster, second_raster);
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
    // The neighbors are checked as well, so a table that merely happens to
    // hold this function at some other index is left alone.
    void* const image_fn = *chosen_slot;
    void* const metrics_fn = *(chosen_slot - 1);
    void* const path_fn = *(chosen_slot + 1);
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
    g_original_path = path_fn;
    bool any = false;
    for (void** slot : tables) {
        if (chromium_patch::ParityWanted() && InText(image, reinterpret_cast<uintptr_t>(metrics_fn))) {
            (void)WriteSlot(slot - 1, reinterpret_cast<void*>(&chromium_metrics_thunk));
        }
        if (chromium_patch::ParityWanted() &&
            InText(image, reinterpret_cast<uintptr_t>(font_metrics_fn))) {
            (void)WriteSlot(slot + 3, reinterpret_cast<void*>(&chromium_font_metrics_thunk));
        }
        // Only where the table names the same generatePath, since the thunk
        // tail-calls that one function and a subclass that overrides it would
        // otherwise be sent to its sibling's implementation.
        if (chromium_patch::ParityWanted() && *(slot + 1) == path_fn &&
            InText(image, reinterpret_cast<uintptr_t>(path_fn))) {
            (void)WriteSlot(slot + 1, reinterpret_cast<void*>(&chromium_path_thunk));
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
        g_original_path = nullptr;
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

// Whether a module's read-only data holds this file name. A __FILE__ from one
// translation unit names the module that was built from it, which is how a
// build that withholds its symbols is still recognized: CEF exports its cef_*
// surface and nothing else, and keeps no symbol table at all.
bool CarriesAnchor(const uintptr_t base, const ElfW(Phdr)* phdr, const ElfW(Half) phnum,
                   const char* anchor)
{
    // This library holds every anchor it looks for, since they are written
    // down in it. Its own image is never the host's.
    static const uintptr_t self = [] {
        Dl_info info{};
        return dladdr(reinterpret_cast<const void*>(&CarriesAnchor), &info) != 0
                   ? reinterpret_cast<uintptr_t>(info.dli_fbase)
                   : 0;
    }();
    if (self != 0 && base == self) {
        return false;
    }
    const size_t length = std::strlen(anchor);
    for (ElfW(Half) i = 0; i < phnum; ++i) {
        const ElfW(Phdr)& p = phdr[i];
        if (p.p_type != PT_LOAD || (p.p_flags & PF_X) != 0 || (p.p_flags & PF_W) != 0) {
            continue;
        }
        const auto* begin = reinterpret_cast<const unsigned char*>(base + p.p_vaddr);
        if (memmem(begin, p.p_filesz, anchor, length) != nullptr) {
            return true;
        }
    }
    return false;
}

// Skia's Fontations typeface, which is the class the raster patch replaces.
constexpr char kFontationsAnchor[] = "skia/src/ports/SkTypeface_fontations.cpp";

// The translation unit that defines GetFontRenderParamsFromFcPattern. It
// names the module to look in, which fontconfig itself never is: a build that
// links fontconfig shared has the same six property names in it, and the
// function reading all six there is fontconfig's own.
constexpr char kRenderParamsAnchor[] = "ui/gfx/linux/fontconfig_util.cc";

void ScanLoadedImages()
{
    if (g_done.exchange(true)) {
        return;
    }
    if (chromium_patch::ParityWanted() && IsBrowserProcess()) {
        // The module built from fontconfig_util.cc, which is the one holding
        // the function. Looking everywhere would reach fontconfig's own copies
        // of the same property names, and the function reading all six there
        // is fontconfig's; the anchor is what tells the two apart.
        //
        // A build that compiles fontconfig in holds those copies in the same
        // module. The scan still identifies the target, because it requires
        // one function to read all six property names and the runner-up to
        // read fewer.
        //
        // No Fontations symbols are required here. This patch names its target
        // by the properties it reads and refuses when no single function reads
        // them all, so it applies to any Chromium build.
        ModuleList browser;
        dl_iterate_phdr(CollectModule, &browser);
        const LoadedModule* carrying = nullptr;
        const LoadedModule* executable = nullptr;
        for (unsigned i = 0; i < browser.count; ++i) {
            const LoadedModule& m = browser.mods[i];
            if (m.name == nullptr || m.name[0] == '\0') {
                executable = &m;
            }
            if (carrying == nullptr &&
                CarriesAnchor(m.base, m.phdr, m.phnum, kRenderParamsAnchor)) {
                carrying = &m;
            }
        }
        if (const LoadedModule* m = carrying != nullptr ? carrying : executable; m != nullptr) {
            render_params::ApplyToImage(m->base, m->phdr, m->phnum);
        }
        return;
    }
    if (!ShouldScanThisProcess()) {
        return;
    }
    // A slot replaced at load cannot be put back, so the answer is asked
    // before anything is written and not only when a glyph arrives.
    // bold_fallback and bold_shaping gate their own installers the same way.
    if (!chromium_patch::ParityWanted()) {
        return;
    }
    if (EnvDisables("CLEARTYPE_CHROMIUM_PATCH")) {
        return;
    }

    ModuleList list;
    dl_iterate_phdr(CollectModule, &list);

    // Out of the callback, since dl_iterate_phdr holds the loader's list lock
    // and the open()/mmap() below must not run under it.
    //
    // The module that names the bridge is taken first, since its symbols rank
    // the candidate vtables. Failing that, the one carrying Skia's Fontations
    // typeface, which is the same code with its names withheld. The executable
    // is the last resort, and TryPatchModule refuses there when no vtable of
    // the right shape reaches anything.
    for (unsigned i = 0; i < list.count; ++i) {
        const LoadedModule& m = list.mods[i];
        const char* path = m.name != nullptr && m.name[0] != '\0' ? m.name : "/proc/self/exe";
        std::vector<FfiSymbol> syms;
        // Well under what a real Fontations build exports, but enough to rule
        // out a few unrelated dynsym entries that contain the substring.
        if (FindFfiSymbols(path, "fontations_ffi", &syms) && syms.size() >= 10) {
            TryPatchModule(m.base, m.phdr, m.phnum, path, syms);
            return;
        }
    }
    for (unsigned i = 0; i < list.count; ++i) {
        const LoadedModule& m = list.mods[i];
        if (!CarriesAnchor(m.base, m.phdr, m.phnum, kFontationsAnchor)) {
            continue;
        }
        const char* path = m.name != nullptr && m.name[0] != '\0' ? m.name : "/proc/self/exe";
        Report("%s carries the Fontations typeface and names none of it", path);
        TryPatchModule(m.base, m.phdr, m.phnum, path, {});
        return;
    }
    for (unsigned i = 0; i < list.count; ++i) {
        const LoadedModule& m = list.mods[i];
        if (m.name == nullptr || m.name[0] == '\0') {
            TryPatchModule(m.base, m.phdr, m.phnum, "/proc/self/exe", {});
            return;
        }
    }
}

// The rebuilt font and the answers it gives, kept per typeface and ppem.
// Rebuilding walks every table, so it happens once per typeface and the
// answers are then only re-derived when the size changes.
std::mutex g_font_mutex;
// Shared, since a caller needs the bytes outside the lock and these fonts run
// to tens of megabytes; copying one per glyph is what the cache exists to
// avoid.
using FontBytes = std::shared_ptr<const std::vector<uint8_t>>;
std::unordered_map<const void*, FontBytes> g_fonts;
std::unordered_map<const void*, std::vector<dwrite_raster::VariationCoord>> g_var_coords;

// onGetVariationDesignPosition, called through each typeface's own vtable so
// the proxy typeface Linux hands out forwards to the face behind it. The
// SkSpan parameter is a pointer and a count in registers, and an empty span
// asks for the count alone.
using VariationPositionFn = int (*)(const void*, dwrite_raster::VariationCoord*, size_t);

// Where it sits relative to onGetTableTags. SkTypeface declares thirteen
// virtuals between the two: onGetVariationDesignParameters, the two synthetic
// style queries, onGetFontDescriptor, onCharsToGlyphs, onCountGlyphs,
// onGetUPEM, onGetKerningPairAdjustments, onGetFamilyName,
// onGetPostScriptName, onGetResourceName and onCreateFamilyNameIterator.
constexpr unsigned kTableTagsToVariationPosition = 13;

// The slot to dispatch on. Slots here are counted the way an object's vptr
// indexes them, so the same number reaches it through any typeface.
std::atomic<unsigned> g_variation_index{0};

// Set when the slot was taken from the class layout alone. A build that keeps
// no symbols has nothing to disassemble against, so the answer is confirmed
// against the font's own fvar the first time it is read instead.
std::atomic<bool> g_variation_unconfirmed{false};

void FindVariationSlot(void** base, void** data_slot)
{
    if (g_variation_index.load(std::memory_order_relaxed) != 0 || data_slot == nullptr) {
        return;
    }
    // cxxbridge numbers the export and the number moves between Skia
    // revisions, so both the numbered and unnumbered spellings are asked for
    // before the class layout is used instead.
    void* bridge = dlsym(RTLD_DEFAULT,
                         "fontations_ffi$cxxbridge1$194$variation_position");
    if (bridge == nullptr) {
        bridge = dlsym(RTLD_DEFAULT, "fontations_ffi$cxxbridge1$variation_position");
    }
    const auto tags = static_cast<size_t>(data_slot - 1 - base);
    if (tags <= kTableTagsToVariationPosition) {
        return;
    }
    // One candidate, not a search. From Chromium 146 on, SkTypeface declares
    // onIsSyntheticBold and onIsSyntheticOblique between the axis position and
    // the table calls, so the distance is 11 before that version and 13 after.
    // A window wide enough to cover both would take an earlier slot that also
    // reaches the bridge.
    const size_t back = ChromiumMajor() >= 146 ? kTableTagsToVariationPosition
                                               : kTableTagsToVariationPosition - 2;
    if (back < tags) {
        ConfirmVariationSlot(base, tags - back, bridge);
    }
}

// Confirms a candidate against the bridge export and takes it if it matches.
bool ConfirmVariationSlot(void** base, const size_t slot, const void* bridge)
{
    const auto fn = reinterpret_cast<uintptr_t>(base[slot]);
    if (!InText(g_image, fn) || bridge == nullptr) {
        return false;
    }
    // The wrapper reaches the one bridge export that answers the axis
    // position, within its own prologue. onMakeClone reaches the same export,
    // so a scan that took the first slot matching would answer with that.
    // Whether this code reaches the bridge by direct call or tail jump within
    // `span` bytes of its start.
    const auto reaches = [&](const uintptr_t code, const unsigned span) {
        if (!InText(g_image, code)) {
            return false;
        }
        const auto* p = reinterpret_cast<const unsigned char*>(code);
        for (unsigned off = 0; off + 5 <= span; ++off) {
            if (p[off] != 0xE8 && p[off] != 0xE9) {
                continue;
            }
            int32_t rel = 0;
            std::memcpy(&rel, p + off + 1, sizeof(rel));
            if (code + off + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel)) ==
                reinterpret_cast<uintptr_t>(bridge)) {
                return true;
            }
        }
        return false;
    };

    const auto* at = reinterpret_cast<const unsigned char*>(fn);
    // Wide enough to reach a call placed past a prologue that sets up several
    // arguments, which is over 0x60 bytes into the virtual on some builds.
    for (unsigned off = 0; off + 5 <= 0x100; ++off) {
        // A call or a tail jump; a wrapper this thin ends in the latter.
        if (at[off] != 0xE8 && at[off] != 0xE9) {
            continue;
        }
        int32_t rel = 0;
        std::memcpy(&rel, at + off + 1, sizeof(rel));
        const uintptr_t target =
            fn + off + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        // The virtual may call a six-byte shim that tail jumps to the bridge
        // rather than the bridge itself, which is what Electron 39 builds.
        if (target == reinterpret_cast<uintptr_t>(bridge) || reaches(target, 0x20)) {
            g_variation_index.store(static_cast<unsigned>(slot), std::memory_order_relaxed);
            Report("onGetVariationDesignPosition is slot %zu", slot);
            return true;
        }
    }
    return false;
}

// SkTypeface declares the axis position immediately after the COLR query, in
// every revision Chromium 142 through 150 ships. The COLR slot is found by the
// tag its function reads, so this needs no count and no version. A build with
// no symbols has nothing to confirm the answer against, and ReadVariationCoords
// checks it against the font's own fvar before it is trusted.
void FindVariationSlotFromColr(void** base, const size_t colr_slot)
{
    if (g_variation_index.load(std::memory_order_relaxed) != 0) {
        return;
    }
    const size_t slot = colr_slot + 1;
    if (slot >= typeface_bridge::kMaxSlotSearched ||
        !InText(g_image, reinterpret_cast<uintptr_t>(base[slot]))) {
        return;
    }
    void* bridge = dlsym(RTLD_DEFAULT, "fontations_ffi$cxxbridge1$194$variation_position");
    if (bridge == nullptr) {
        bridge = dlsym(RTLD_DEFAULT, "fontations_ffi$cxxbridge1$variation_position");
    }
    if (ConfirmVariationSlot(base, slot, bridge)) {
        return;
    }
    g_variation_unconfirmed.store(true, std::memory_order_relaxed);
    g_variation_index.store(static_cast<unsigned>(slot), std::memory_order_relaxed);
    Report("onGetVariationDesignPosition is slot %zu, the virtual after the COLR "
           "query; the font's own axes confirm it", slot);
}

// The axes the font itself declares, which is what an answer from the vtable
// has to agree with.
std::vector<uint32_t> FvarAxes(const std::vector<uint8_t>& font)
{
    const font_facts::Span fvar = font_facts::FindTable(font, kFvarTag);
    if (fvar.data == nullptr || fvar.size < 16) {
        return {};
    }
    const auto be16 = [&](const size_t at) {
        return static_cast<size_t>(fvar.data[at]) << 8 | fvar.data[at + 1];
    };
    const size_t axes_at = be16(4);
    const size_t count = be16(8);
    const size_t size = be16(10);
    if (count == 0 || count > 64 || size < 4 || axes_at + count * size > fvar.size) {
        return {};
    }
    std::vector<uint32_t> tags;
    tags.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = fvar.data + axes_at + i * size;
        tags.push_back(static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
                       static_cast<uint32_t>(p[2]) << 8 | p[3]);
    }
    return tags;
}

// Read under g_font_mutex, beside the bytes, so the coords and the font they
// vary always answer together.
void ReadVariationCoords(void* typeface, const std::vector<uint8_t>& font)
{
    const unsigned index = g_variation_index.load(std::memory_order_relaxed);
    if (index == 0 || g_var_coords.contains(typeface)) {
        return;
    }
    // Only a variable font has anything to ask for, and on a build where the
    // slot is unconfirmed this keeps the call off every other font.
    const std::vector<uint32_t> axes = FvarAxes(font);
    if (axes.empty()) {
        g_var_coords.emplace(typeface, std::vector<dwrite_raster::VariationCoord>{});
        return;
    }
    const auto fn = reinterpret_cast<VariationPositionFn>(
        (*reinterpret_cast<void***>(typeface))[index]);
    const int count = fn(typeface, nullptr, 0);
    const bool unconfirmed = g_variation_unconfirmed.load(std::memory_order_relaxed);
    const auto give_up = [&](const char* why) {
        g_variation_index.store(0, std::memory_order_relaxed);
        Report("slot %u %s, so it is not onGetVariationDesignPosition; variable fonts "
               "will draw at their default instance", index, why);
        g_var_coords.emplace(typeface, std::vector<dwrite_raster::VariationCoord>{});
    };
    if (unconfirmed && count != static_cast<int>(axes.size())) {
        give_up("does not answer the font's axis count");
        return;
    }
    if (count <= 0 || count > 64) {
        g_var_coords.emplace(typeface, std::vector<dwrite_raster::VariationCoord>{});
        return;
    }
    std::vector<dwrite_raster::VariationCoord> coords(static_cast<size_t>(count));
    if (fn(typeface, coords.data(), coords.size()) != count) {
        coords.clear();
    }
    if (unconfirmed) {
        // The count alone is weak, since a neighboring slot could return the
        // same small number. The tags come from the font, so a slot answering
        // all of them is the one that reads fvar.
        for (const dwrite_raster::VariationCoord& c : coords) {
            if (std::ranges::find(axes, c.axis) == axes.end()) {
                give_up("answers an axis the font does not declare");
                return;
            }
        }
        g_variation_unconfirmed.store(false, std::memory_order_relaxed);
        Report("slot %u answers the font's axes, so it is "
               "onGetVariationDesignPosition", index);
    }
    g_var_coords.emplace(typeface, std::move(coords));
}

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

FontBytes FontBytesLocked(void* typeface)
{
    auto font = g_fonts.find(typeface);
    if (font == g_fonts.end()) {
        HoldTypeface(typeface);
        auto bytes = std::make_shared<std::vector<uint8_t>>(
            typeface_bridge::ReadFontFile(typeface));
        if (bytes->empty()) {
            Report("typeface %p: no font (its onGetTableTags/onGetTableData could not be "
                   "identified, or the tables would not read)", typeface);
        } else {
            Report("typeface %p: rebuilt %zu bytes from its tables", typeface,
                   bytes->size());
        }
        ReadVariationCoords(typeface, *bytes);
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
        font_facts::Describe(*FontBytesLocked(typeface), gasp_ppem, bitmap_ppem);
    cache[typeface] = {gasp_ppem, bitmap_ppem, facts};
    return facts;
}

}  // namespace

// The font behind a typeface, for bold_shaping.cpp. It answers only for a
// typeface the scaler hooks have already read, which is what makes it safe to
// call on a pointer that is only believed to be one: it looks the address up
// and never follows it. A typeface's virtuals cannot be probed on a guess,
// since a wrong guess is a segmentation fault rather than a wrong answer.
//
// Answering out of the same cache also means shaping and drawing cannot
// disagree about which runs carry a substituted face.
//
// The pointer stays good: entries are never erased, and an unordered_map
// keeps element addresses across a rehash.
const std::vector<dwrite_raster::VariationCoord>* ChromiumVariationCoords(
    const void* typeface)
{
    const std::lock_guard lock(g_font_mutex);
    const auto found = g_var_coords.find(typeface);
    if (found == g_var_coords.end() || found->second.empty()) {
        return nullptr;
    }
    return &found->second;
}

const std::vector<uint8_t>* ChromiumFontBytes(void* typeface)
{
    const std::lock_guard lock(g_font_mutex);
    const auto font = g_fonts.find(typeface);
    if (font == g_fonts.end() || font->second->empty()) {
        return nullptr;
    }
    return font->second.get();
}

// Runs before the typeface's own onFilterRec. Returns whether the caller
// should go on to run that, which it always should. This only needs to see
// the flags first, because Fontations clears kGenA8FromLCD_Flag and that flag
// is the only sign the surface cannot show subpixel text.
bool OnChromiumFilterRec(void* self, void* rec)
{
    if (rec == nullptr) {
        return true;
    }
    // Before Fontations turns a synthetic bold into a stroke. A family with a
    // real bold face is given it instead, which is the face Windows was handed
    // and the reason its rec carries no stroke at all.
    if (self != nullptr && chromium_patch::ParityWanted()) {
        FontBytes font;
        {
            const std::lock_guard lock(g_font_mutex);
            font = FontBytesLocked(self);
        }
        if (!font->empty() && bold_fallback::ClearSyntheticBold(rec, *font)) {
            static std::atomic said{false};
            if (!said.exchange(true)) {
                Report("synthetic bold replaced with a real bold face");
            }
        }
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
// Blink asked for bold, fell back to a face that has none, and settled for the
// stroke useStrokeForFakeBold leaves in the rec. Windows was handed the real
// Bold face instead, whose advances come from its own hmtx, so that face is
// what gets measured and drawn here. Null when this is not such a run.
//
// DirectWrite caches a font face per key and the typeface here is still the
// regular one, so the substitute is keyed on its own bytes, which live in
// bold_fallback's table for the life of the process.
static bold_fallback::Face BoldSubstitute(const void* context, const std::vector<uint8_t>& font)
{
    const auto* rec = static_cast<const unsigned char*>(context) + skia_abi::kContextRec;
    if (!bold_fallback::WasMarked(rec)) {
        return {};
    }
    const bool oblique =
        bold_fallback::IsOblique(skia_abi::Read<float>(rec, skia_abi::kRecPreSkewX));
    return bold_fallback::RealBoldFor(font, oblique);
}

// The outline Skia is about to stroke or fill from. Windows takes it from
// IDWriteFontFace::GetGlyphRunOutline and Linux from skrifa, and the two
// disagree by a fraction of a pixel per point, which every glyph drawn from
// its path shows as a scatter along each edge.
//
// SkPathData owns its point array as trailing storage and is otherwise
// immutable, so DirectWrite's points can be written straight into it when the
// two agree on the verbs. Nothing is allocated and no count changes.
//
// The write is declined unless every check below passes, and declining leaves
// skrifa's path exactly as it was.
namespace {

struct PathView
{
    unsigned char* data = nullptr;
    path_abi::Point* points = nullptr;
    size_t point_count = 0;
    const uint8_t* verbs = nullptr;
    size_t verb_count = 0;
};

// Everything the layout guarantees about itself, checked before anything is
// written. A build whose SkPathData moved a field fails at least one of these
// and gets left alone.
bool MirrorHolds(const PathView& v)
{
    if (v.point_count == 0 || v.verb_count == 0) {
        return false;
    }
    // The reference count is SkNVRefCnt's, and a path shared with anything
    // else must not be edited underneath it.
    if (skia_abi::Read<int32_t>(v.data, path_abi::kDataRefCnt) != 1) {
        return false;
    }
    if (!path_abi::PlausibleUniqueID(skia_abi::Read<uint32_t>(v.data, path_abi::kDataUniqueID))) {
        return false;
    }
    // A rect, oval or rrect caches geometry the points would no longer agree
    // with.
    if (skia_abi::Read<uint8_t>(v.data, path_abi::kDataType) != path_abi::kIsAGeneral) {
        return false;
    }
    // Conic weights live in their own array, which is not rewritten, so a
    // path holding any is out of scope. Neither source emits them.
    if (skia_abi::Read<size_t>(v.data, path_abi::kDataConics + path_abi::kSpanCount) != 0) {
        return false;
    }
    // The three arrays are trailing storage laid out points, conics, verbs.
    if (reinterpret_cast<unsigned char*>(v.points) != v.data + path_abi::kDataSize ||
        v.verbs != reinterpret_cast<const uint8_t*>(v.points + v.point_count)) {
        return false;
    }
    size_t implied = 0;
    for (size_t i = 0; i < v.verb_count; ++i) {
        const int n = path_abi::PointsForVerb(v.verbs[i]);
        if (n < 0) {
            return false;
        }
        implied += static_cast<size_t>(n);
    }
    return implied == v.point_count;
}

// SkPathData::finishInit's bounds, which are the bounds of every point,
// control points included, not the tight bounds of the curves.
void PointBounds(const path_abi::Point* p, const size_t count, float* out)
{
    out[0] = out[2] = p[0].x;
    out[1] = out[3] = p[0].y;
    for (size_t i = 1; i < count; ++i) {
        out[0] = std::min(out[0], p[i].x);
        out[1] = std::min(out[1], p[i].y);
        out[2] = std::max(out[2], p[i].x);
        out[3] = std::max(out[3], p[i].y);
    }
}

// The two outlines are meant to be the same shape a fraction of a pixel
// apart. Anything further means they are not the same glyph in the same
// place, whatever the verbs say, and the safe answer is skrifa's path.
constexpr float kMaxPointDelta = 1.0f;

// Whether the two verb sequences describe the same path with only quads and
// cubics traded, which is the one disagreement check_quadratic can produce:
// DirectWrite hands Skia a cubic, and Skia folds it back to a quadratic only
// when the control points land within 10 ULPs. Any other difference is a
// different outline and not something to rewrite.
bool CurveOnlyDifference(const uint8_t* a, const uint8_t* b, const size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (a[i] == b[i]) {
            continue;
        }
        const bool curves = (a[i] == path_abi::kQuad || a[i] == path_abi::kCubic) &&
                            (b[i] == path_abi::kQuad || b[i] == path_abi::kCubic);
        if (!curves) {
            return false;
        }
    }
    return true;
}

// One line per power of two, so a page says how many outlines came from
// DirectWrite without a line per glyph.
void Replaced(const uint16_t glyph, const float worst, const bool regrown)
{
    static std::atomic<uint64_t> count{0};
    if (const uint64_t n = count.fetch_add(1) + 1; (n & (n - 1)) == 0) {
        Report("outline for glyph %u is DirectWrite's (%.3f px from skrifa's%s): %lu so far",
               glyph, static_cast<double>(worst), regrown ? ", regrown" : "", n);
    }
}

// The listener list owns nothing and refers only to itself, which is what
// makes a byte copy of it valid. Empty, pointed at its own inline element,
// and not owning heap storage: an object holding a listener or a grown array
// fails this and is left alone. A path built by generatePath has had no
// chance to acquire either.
bool ListIsCopyable(const unsigned char* data)
{
    return skia_abi::Read<int32_t>(data, path_abi::kListSize) == 0 &&
           skia_abi::Read<const unsigned char*>(data, path_abi::kListData) ==
               data + path_abi::kListInline &&
           (skia_abi::Read<uint32_t>(data, path_abi::kListCapacity) & 1) == 0;
}

uint8_t SegmentMaskFor(const uint8_t* verbs, const size_t count)
{
    uint8_t mask = 0;
    for (size_t i = 0; i < count; ++i) {
        switch (verbs[i]) {
            case path_abi::kLine: mask |= path_abi::kLineMask; break;
            case path_abi::kQuad: mask |= path_abi::kQuadMask; break;
            case path_abi::kConic: mask |= path_abi::kConicMask; break;
            case path_abi::kCubic: mask |= path_abi::kCubicMask; break;
            default: break;
        }
    }
    return mask;
}

// Gives the SkPath a new SkPathData holding these verbs and points, for the
// glyphs whose outlines are the same shape but not the same length.
//
// SkPathData::Alloc takes the object and its trailing arrays out of one
// ::operator new and points the spans inside it, so a replacement is that
// same allocation made again at the new size. Everything the old object holds
// that is not geometry is copied: the reference count, which MirrorHolds has
// already read as 1, the listener list, and the unique id, which is free to
// inherit because the old object is released here and its id retired with it.
// The one pointer that has to move is the listener list's own, which points
// at storage inside the object.
//
// ::operator new here is the executable's, which exports both it and the
// unsized ::operator delete SkPathData::operator delete calls, so the memory
// this hands to Skia comes from the allocator Skia will hand it back to.
bool ReplacePathData(void* path, const PathView& v, const std::vector<uint8_t>& verbs,
                     const std::vector<path_abi::Point>& points, const float* bounds)
{
    if (!ListIsCopyable(v.data)) {
        return false;
    }
    const size_t size = path_abi::kDataSize + points.size() * sizeof(path_abi::Point) +
                        verbs.size();
    auto* fresh = static_cast<unsigned char*>(::operator new(size));
    std::memcpy(fresh, v.data, path_abi::kDataSize);

    unsigned char* const inline_element = fresh + path_abi::kListInline;
    std::memcpy(fresh + path_abi::kListData, &inline_element, sizeof(inline_element));

    unsigned char* const trailing = fresh + path_abi::kDataSize;
    unsigned char* const verb_data = trailing + points.size() * sizeof(path_abi::Point);
    const size_t point_count = points.size();
    const size_t conic_count = 0;
    const size_t verb_count = verbs.size();
    std::memcpy(fresh + path_abi::kDataPoints, &trailing, sizeof(trailing));
    std::memcpy(fresh + path_abi::kDataPoints + path_abi::kSpanCount, &point_count,
                sizeof(point_count));
    std::memcpy(fresh + path_abi::kDataConics, &verb_data, sizeof(verb_data));
    std::memcpy(fresh + path_abi::kDataConics + path_abi::kSpanCount, &conic_count,
                sizeof(conic_count));
    std::memcpy(fresh + path_abi::kDataVerbs, &verb_data, sizeof(verb_data));
    std::memcpy(fresh + path_abi::kDataVerbs + path_abi::kSpanCount, &verb_count,
                sizeof(verb_count));
    std::memcpy(trailing, points.data(), points.size() * sizeof(path_abi::Point));
    std::memcpy(verb_data, verbs.data(), verbs.size());

    std::memcpy(fresh + path_abi::kDataBounds, bounds, 4 * sizeof(float));
    const uint8_t mask = SegmentMaskFor(verbs.data(), verbs.size());
    std::memcpy(fresh + path_abi::kDataSegmentMask, &mask, sizeof(mask));
    constexpr uint8_t unknown = path_abi::kConvexityUnknown;
    std::memcpy(fresh + path_abi::kDataConvexity, &unknown, sizeof(unknown));

    std::memcpy(static_cast<unsigned char*>(path) + path_abi::kPathData, &fresh, sizeof(fresh));
    // The destructor is a debug-only write to the unique id plus the listener
    // list's, and the list has just been shown to hold nothing and own
    // nothing, so releasing the storage is all there is to do.
    ::operator delete(v.data);
    return true;
}

}  // namespace

// Skia's DirectWrite font manager strips DirectWrite's simulations by asking
// again upright, except for a font with bitmap strikes, which it lets through
// (SkFontMgr_win_dw.cpp, FirstMatchingFontWithoutSimulations under
// SK_WIN_FONTMGR_NO_SIMULATIONS, guarded by an EBDT test). So on Windows such
// a face is slanted by DirectWrite at about 20 degrees and the rec carries no
// skew at all, while here Blink hands Skia its own 0.25 skew, which is 14.
//
// Where both hold the slant belongs to the face, and the rec is flattened so
// the matrix does not apply it a second time.
static bool SimulatesOblique(const skia_abi::Rec& rec, const std::vector<uint8_t>& font,
                             const uint32_t face_index)
{
    static const bool off = EnvDisables("DWC_OBLIQUE_SIM");
    return !off && rec.pre_skew_x != 0.0f && font_facts::HasEbdt(font, face_index);
}

// The subpixel position SkScalerContext_DW puts in the matrix before walking a
// color glyph's paint tree, in em units for the walk and in device pixels for
// the clip box. All zero when the glyph is not subpixel positioned or the
// matrix will not invert, which leaves the walk unshifted.
static void ColorGlyphPhase(const skia_abi::Rec& rec, const skia_abi::Glyph& g, float* phase_x,
                            float* phase_y, float* sub_x, float* sub_y)
{
    *phase_x = 0;
    *phase_y = 0;
    *sub_x = 0;
    *sub_y = 0;
    if (!rec.IsSubpixel() || !g.packed_id_known) {
        return;
    }
    // SkPackedGlyphID keeps two bits per axis, so SkFixedToScalar of what
    // getSubXFixed returns is the index over four.
    const float dx = static_cast<float>(g.SubX()) / 4.0f;
    const float dy = static_cast<float>(g.SubY()) / 4.0f;
    if (dx == 0.0f && dy == 0.0f) {
        return;
    }
    if (!rec.DeviceOffsetToEm(dx, dy, phase_x, phase_y)) {
        return;
    }
    *sub_x = dx;
    *sub_y = dy;
}

static skia_abi::Rec WithoutSkew(skia_abi::Rec rec)
{
    rec.pre_skew_x = 0.0f;
    return rec;
}

void OnChromiumPath(void* result, void* context, const void* glyph)
{
    static const bool enabled = !EnvDisables("DWC_DW_OUTLINE");
    static const bool log = std::getenv("DWC_PATH_LOG") != nullptr;
    static const bool realloc = !EnvDisables("DWC_PATH_REGROW");
    if (result == nullptr || context == nullptr || glyph == nullptr ||
        !chromium_patch::ParityWanted() || !enabled) {
        return;
    }
    auto* out = static_cast<unsigned char*>(result);
    if (skia_abi::Read<uint8_t>(out, path_abi::kGeneratedEngaged) == 0) {
        return;
    }
    PathView v;
    v.data = skia_abi::Read<unsigned char*>(out, path_abi::kGeneratedPath + path_abi::kPathData);
    if (v.data == nullptr) {
        return;
    }
    v.points = skia_abi::Read<path_abi::Point*>(v.data, path_abi::kDataPoints);
    v.point_count = skia_abi::Read<size_t>(v.data, path_abi::kDataPoints + path_abi::kSpanCount);
    v.verbs = skia_abi::Read<const uint8_t*>(v.data, path_abi::kDataVerbs);
    v.verb_count = skia_abi::Read<size_t>(v.data, path_abi::kDataVerbs + path_abi::kSpanCount);
    if (v.points == nullptr || v.verbs == nullptr || !MirrorHolds(v)) {
        static std::atomic said{false};
        if (!said.exchange(true)) {
            Report("the SkPathData layout does not check out; outlines stay with skrifa");
        }
        return;
    }

    const skia_abi::Glyph g = skia_abi::Glyph::From(glyph);
    auto* typeface = skia_abi::Read<void*>(context, skia_abi::kContextTypeface);
    if (!g.packed_id_known || typeface == nullptr) {
        return;
    }
    const skia_abi::Rec rec = skia_abi::Rec::From(context);
    float scale_y = 0;
    windows_path::Matrix2x2 remaining;
    if (!windows_path::ComputeMatrices(rec, &scale_y, &remaining)) {
        return;
    }
    const float gdi_text_size = std::round(scale_y * 64.0f) / 64.0f;
    const int gasp_ppem = static_cast<int>(std::floor(gdi_text_size + 0.5f));
    const int bitmap_ppem = static_cast<int>(gdi_text_size);

    FontBytes font;
    {
        const std::lock_guard lock(g_font_mutex);
        font = FontBytesLocked(typeface);
    }
    if (font->empty()) {
        return;
    }
    const std::vector<uint8_t>* use = font.get();
    const void* face_key = typeface;
    uint32_t face_index = 0;
    bool simulate_bold = false;
    if (const bold_fallback::Face bold = BoldSubstitute(context, *font);
        bold.bytes != nullptr || bold.simulate) {
        simulate_bold = bold.simulate;
        if (bold.bytes != nullptr) {
            use = bold.bytes;
            face_key = bold.bytes->data();
            face_index = bold.face_index;
        }
    }
    // A substituted face draws a different shape on purpose, so the checks
    // below have nothing to compare its outline against and would throw it
    // away. The swap is the same one the mask takes, and it is settled before
    // the outline is asked for, so the path is replaced whole instead. It
    // cannot be rewritten in place across a shape change: the verb sequence
    // and the point count are both the array's own.
    const bool substituted = use != font.get();

    // This hook applies `remaining` to DirectWrite's points itself, so a skew
    // left in there would slant an outline the face has already slanted.
    const bool simulate_oblique = SimulatesOblique(rec, *use, face_index);
    const skia_abi::Rec flat = simulate_oblique ? WithoutSkew(rec) : rec;
    int oblique_gasp_ppem = gasp_ppem;
    int oblique_bitmap_ppem = bitmap_ppem;
    if (simulate_oblique) {
        if (!windows_path::ComputeMatrices(flat, &scale_y, &remaining)) {
            return;
        }
        const float flat_gdi_size = std::round(scale_y * 64.0f) / 64.0f;
        oblique_gasp_ppem = static_cast<int>(std::floor(flat_gdi_size + 0.5f));
        oblique_bitmap_ppem = static_cast<int>(flat_gdi_size);
    }

    const windows_path::Decision d = windows_path::Decide(
        windows_path::WithWindowsHinting(flat), scale_y,
        font_facts::Describe(*use, oblique_gasp_ppem, oblique_bitmap_ppem, face_index));

    static thread_local std::vector<uint8_t> dw_verbs;
    static thread_local std::vector<path_abi::Point> dw_points;
    if (!dwrite_raster::GlyphOutline(face_key, *use, g.GlyphId(), d.text_size_render, &dw_verbs,
                                     &dw_points, face_index, simulate_bold, simulate_oblique)) {
        return;
    }
    // Both scaler contexts generate at scale.fY and then apply what
    // computeMatrices left over, so DirectWrite's outline needs the same
    // matrix before it can be compared with the one already in the path.
    for (path_abi::Point& p : dw_points) {
        p = {remaining.scale_x * p.x + remaining.skew_x * p.y,
             remaining.skew_y * p.x + remaining.scale_y * p.y};
    }

    if (!substituted &&
        (dw_verbs.size() != v.verb_count ||
         !CurveOnlyDifference(dw_verbs.data(), v.verbs, v.verb_count))) {
        if (log) {
            Report("path: glyph %u verbs differ (%zu/%zu against %zu/%zu); kept skrifa's",
                   g.GlyphId(), dw_verbs.size(), dw_points.size(), v.verb_count, v.point_count);
        }
        return;
    }

    float bounds[4];
    PointBounds(dw_points.data(), dw_points.size(), bounds);

    // Rewriting points in place leaves the verbs alone, so it is only correct
    // where they already agree. Two offsetting swaps would keep the count and
    // change the sequence, and DirectWrite's points under skrifa's verbs is a
    // different curve.
    if (!substituted && dw_points.size() == v.point_count &&
        std::memcmp(dw_verbs.data(), v.verbs, v.verb_count) == 0) {
        float worst = 0;
        for (size_t i = 0; i < dw_points.size(); ++i) {
            worst = std::max(worst, std::abs(dw_points[i].x - v.points[i].x));
            worst = std::max(worst, std::abs(dw_points[i].y - v.points[i].y));
        }
        if (worst > kMaxPointDelta) {
            if (log) {
                Report("path: glyph %u is %.3f px away from skrifa's; kept skrifa's",
                       g.GlyphId(), static_cast<double>(worst));
            }
            return;
        }
        std::memcpy(v.points, dw_points.data(), dw_points.size() * sizeof(path_abi::Point));
        std::memcpy(v.data + path_abi::kDataBounds, bounds, sizeof(bounds));
        // Convexity is derived from the points and was computed, if at all,
        // from the ones just replaced.
        constexpr uint8_t unknown = path_abi::kConvexityUnknown;
        std::memcpy(v.data + path_abi::kDataConvexity, &unknown, sizeof(unknown));
        Replaced(g.GlyphId(), worst, false);
        return;
    }

    // A quad on one side against a cubic on the other, so the arrays are not
    // the same length and the object cannot hold both. With no point to pair
    // off against, the two outlines are held to be the same glyph in the same
    // place by their bounds.
    if (!realloc) {
        if (log) {
            Report("path: glyph %u needs %zu points where %zu fit; kept skrifa's", g.GlyphId(),
                   dw_points.size(), v.point_count);
        }
        return;
    }
    float worst = 0;
    if (!substituted) {
        float existing[4];
        PointBounds(v.points, v.point_count, existing);
        for (int i = 0; i < 4; ++i) {
            worst = std::max(worst, std::abs(bounds[i] - existing[i]));
        }
        if (worst > kMaxPointDelta) {
            if (log) {
                Report("path: glyph %u is %.3f px away from skrifa's; kept skrifa's", g.GlyphId(),
                       static_cast<double>(worst));
            }
            return;
        }
    }
    if (!ReplacePathData(out + path_abi::kGeneratedPath, v, dw_verbs, dw_points, bounds)) {
        if (log) {
            Report("path: glyph %u holds a listener, so its path stays skrifa's", g.GlyphId());
        }
        return;
    }
    // The path is no longer the one the font would draw, which is what this
    // flag says. It reaches PDF output and glyph serialization, not the
    // raster.
    constexpr bool kModified = true;
    std::memcpy(out + path_abi::kGeneratedModified, &kModified, sizeof(kModified));
    Replaced(g.GlyphId(), worst, true);
}

// A color glyph's bounds are measured inside the original generateMetrics, so
// the subpixel position has to be set before it runs. The rest of the metrics
// work runs after it, in OnChromiumMetrics.
void OnChromiumMetricsPre(void* context, const void* glyph)
{
    if (!g_patched.load(std::memory_order_acquire) || context == nullptr || glyph == nullptr) {
        return;
    }
    const skia_abi::Glyph g = skia_abi::Glyph::From(glyph);
    const skia_abi::Rec rec = skia_abi::Rec::From(context);
    float phase_x = 0;
    float phase_y = 0;
    float sub_x = 0;
    float sub_y = 0;
    ColorGlyphPhase(rec, g, &phase_x, &phase_y, &sub_x, &sub_y);
    colr_outline::SetPhase(phase_x, phase_y, sub_x, sub_y);
}

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

    FontBytes font;
    {
        const std::lock_guard lock(g_font_mutex);
        font = FontBytesLocked(typeface);
    }
    if (font->empty()) {
        static std::atomic said{false};
        if (!said.exchange(true)) {
            Report("  metrics: no font bytes for typeface %p", typeface);
        }
        return;
    }
    const std::vector<uint8_t>* use = font.get();
    const void* face_key = typeface;
    uint32_t face_index = 0;
    bool simulate_bold = false;
    if (const bold_fallback::Face bold = BoldSubstitute(context, *font);
        bold.bytes != nullptr || bold.simulate) {
        simulate_bold = bold.simulate;
        if (bold.bytes != nullptr) {
            use = bold.bytes;
            face_key = bold.bytes->data();
            face_index = bold.face_index;
        }
    }

    // With the slant on the face the rec has no skew, so the scale the
    // decision is made at is the one Windows would have used.
    const bool simulate_oblique = SimulatesOblique(rec, *use, face_index);
    const skia_abi::Rec flat = simulate_oblique ? WithoutSkew(rec) : rec;
    const float use_scale_y = simulate_oblique ? windows_path::DeviceScaleY(flat) : scale_y;
    const float use_gdi_size = std::round(use_scale_y * 64.0f) / 64.0f;
    const int use_gasp_ppem =
        simulate_oblique ? static_cast<int>(std::floor(use_gdi_size + 0.5f)) : gasp_ppem;
    const int use_bitmap_ppem =
        simulate_oblique ? static_cast<int>(use_gdi_size) : bitmap_ppem;

    const windows_path::Decision d = windows_path::Decide(
        windows_path::WithWindowsHinting(flat), use_scale_y,
        font_facts::Describe(*use, use_gasp_ppem, use_bitmap_ppem, face_index));

    float advance = 0;
    float advance_y = 0;
    if (!dwrite_raster::GlyphAdvance(face_key, *use, g.GlyphId(), flat, d, &advance,
                                     &advance_y, face_index, simulate_bold, simulate_oblique)) {
        static std::atomic said{false};
        if (!said.exchange(true)) {
            Report("DirectWrite would not measure glyph %u; advances stay Skia's",
                   g.GlyphId());
        }
        return;
    }
    std::memcpy(static_cast<unsigned char*>(result) + skia_abi::kMetricsAdvanceX,
                &advance, sizeof(advance));
    std::memcpy(static_cast<unsigned char*>(result) + skia_abi::kMetricsAdvanceY,
                &advance_y, sizeof(advance_y));

    // The bounds too, since generateImage is asked to fill this box. A color
    // glyph is left alone; the raster path declines those and Skia's own
    // image has to keep Skia's box.
    //
    // Fontations answers every outline glyph with computeFromPath, so Skia
    // takes the bounds from the path it generated and never reads the ones
    // here. Clearing it is what makes these the glyph's box, and only the
    // glyphs the raster path draws carry kFontationsPath.
    const auto metrics_mask =
        skia_abi::Read<uint8_t>(result, skia_abi::kMetricsMaskFormat);
    const auto metrics_bits =
        skia_abi::Read<uint16_t>(result, skia_abi::kMetricsExtraBits);
    if (static const bool tell_bits = std::getenv("DWC_METRICS_LOG") != nullptr; tell_bits) {
        static std::atomic<int> told{0};
        if (told.fetch_add(1, std::memory_order_relaxed) < 8) {
            Report("  box: glyph %u mask=%u bits=%u substituted=%d", g.GlyphId(),
                   static_cast<unsigned>(metrics_mask), static_cast<unsigned>(metrics_bits),
                   use != font.get() ? 1 : 0);
        }
    }
    // A substituted face is asked for its box too, and it is the box that
    // decides where the mask is drawn.
    if (metrics_mask != skia_abi::kARGB32 &&
        (metrics_bits == skia_abi::kFontationsPath || use != font.get())) {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        const bool got = dwrite_raster::GlyphBounds(face_key, *use, g, flat, d,
                                                    d.rendering_mode, d.texture_type,
                                                    &left, &top, &right, &bottom,
                                                    face_index, simulate_bold, simulate_oblique);
        static const bool tell = std::getenv("DWC_METRICS_LOG") != nullptr;
        if (tell) {
            static std::atomic<int> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 6) {
                Report("  bounds: glyph %u %s %d,%d,%d,%d mode=%s tex=%s size=%.3f "
                       "skew=%.4f mask=%u flags=0x%x branch=%s gridfit=%d",
                       g.GlyphId(), got ? "ok" : "declined", left, top, right, bottom,
                       windows_path::RenderingModeName(d.rendering_mode),
                       windows_path::TextureTypeName(d.texture_type),
                       static_cast<double>(d.text_size_render),
                       static_cast<double>(rec.pre_skew_x),
                       static_cast<unsigned>(rec.mask_format),
                       static_cast<unsigned>(rec.flags), d.branch,
                       static_cast<int>(d.grid_fit_mode));
            }
        }
        if (got) {
            const float box[4] = {static_cast<float>(left), static_cast<float>(top),
                                  static_cast<float>(right), static_cast<float>(bottom)};
            std::memcpy(static_cast<unsigned char*>(result) + skia_abi::kMetricsBounds, box,
                        sizeof(box));
            constexpr bool kFromPath = false;
            std::memcpy(static_cast<unsigned char*>(result) + skia_abi::kMetricsComputeFromPath,
                        &kFromPath, sizeof(kFromPath));
        }
        // An empty rect is Skia's signal to try another texture type, and it
        // then draws the glyph a different way. Nothing here follows it that
        // far, so Skia's own bounds stay and so does its image.
    }

    // useStrokeForFakeBold leaves a frame width behind, and SkScalerContext
    // sets fGenerateImageFromPath for any rec that has one, so the glyph is
    // stroked from its own outline and generateImage is never asked for a
    // mask. Windows carries no stroke on this run, having been given a real
    // bold face, so the mask is asked for here and the substitute is what
    // fills it.

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

    FontBytes font;
    {
        const std::lock_guard lock(g_font_mutex);
        font = FontBytesLocked(typeface);
    }
    if (font->empty()) {
        return;
    }
    const std::vector<uint8_t>* use = font.get();
    const void* face_key = typeface;
    uint32_t face_index = 0;
    bool simulate_bold = false;
    if (const bold_fallback::Face bold = BoldSubstitute(context, *font);
        bold.bytes != nullptr || bold.simulate) {
        simulate_bold = bold.simulate;
        if (bold.bytes != nullptr) {
            use = bold.bytes;
            face_key = bold.bytes->data();
            face_index = bold.face_index;
        }
    }

    const bool simulate_oblique = SimulatesOblique(rec, *use, face_index);
    const skia_abi::Rec flat = simulate_oblique ? WithoutSkew(rec) : rec;
    const float use_scale_y = simulate_oblique ? windows_path::DeviceScaleY(flat) : scale_y;

    const windows_path::Decision d =
        windows_path::Decide(windows_path::WithWindowsHinting(flat), use_scale_y,
                             font_facts::Describe(*use, gasp_ppem, bitmap_ppem, face_index));
    if (dwrite_raster::FontMetrics(face_key, *use, d, metrics, face_index, simulate_bold,
                                   simulate_oblique)) {
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
    // The bytes of the face that will actually be drawn. A substituted bold
    // face answers the gasp and strike questions for itself, so it has to be
    // resolved before the tree is walked and not just before the draw.
    FontBytes font;
    if (chromium_patch::ParityWanted()) {
        const std::lock_guard lock(g_font_mutex);
        font = FontBytesLocked(typeface);
    }
    if (font == nullptr) {
        font = std::make_shared<const std::vector<uint8_t>>();
    }
    const std::vector<uint8_t>* use = font.get();
    const void* face_key = typeface;
    uint32_t face_index = 0;
    bool simulate_bold = false;
    const bold_fallback::Face bold = BoldSubstitute(context, *font);
    if (bold.bytes != nullptr || bold.simulate) {
        simulate_bold = bold.simulate;
        if (bold.bytes != nullptr) {
            use = bold.bytes;
            face_key = bold.bytes->data();
            face_index = bold.face_index;
        }
    } else if ((rec.flags & skia_abi::kEmbolden) != 0) {
        // The rec asks for a synthetic bold and no face was found to carry it.
        // Drawing here would return a glyph of ordinary weight and report it
        // as done, and Skia's own stroke never runs, so the run loses its
        // bold. The glyph goes back to Skia, which strokes it as it would
        // have without any of this. A build whose font list cannot be read is
        // where this happens, since nothing is mapped to substitute.
        static std::atomic said{false};
        if (!said.exchange(true)) {
            Report("no bold face is available, so emboldened glyphs stay with Skia");
        }
        return true;
    }

    // Without DirectWrite the tree takes the branch Skia takes for a font with
    // no gasp and no strike.
    const windows_path::FontFacts facts =
        chromium_patch::ParityWanted()
            ? (use == font.get() ? FactsFor(typeface, gasp_ppem, bitmap_ppem)
                            : font_facts::Describe(*use, gasp_ppem, bitmap_ppem, face_index))
            : windows_path::FontFacts{};
    // The mask has to be drawn the way its box was measured, so the same
    // flattening the metrics hook applied is applied here.
    const bool simulate_oblique = SimulatesOblique(rec, *use, face_index);
    const skia_abi::Rec flat = simulate_oblique ? WithoutSkew(rec) : rec;
    const float use_scale_y = simulate_oblique ? windows_path::DeviceScaleY(flat) : scale_y;
    const windows_path::Decision d =
        windows_path::Decide(windows_path::WithWindowsHinting(flat), use_scale_y, facts);

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

    // A color glyph is declined below and Skia draws it, but its layer
    // outlines come from the bridge and this is the only place that knows the
    // face and the size Windows would have asked for them at.
    if (g.mask_format == skia_abi::kARGB32) {
        float phase_x = 0;
        float phase_y = 0;
        float sub_x = 0;
        float sub_y = 0;
        ColorGlyphPhase(rec, g, &phase_x, &phase_y, &sub_x, &sub_y);
        colr_outline::SetPhase(phase_x, phase_y, sub_x, sub_y);
        colr_outline::SetSource(face_key, use, face_index, d.text_size_render);
        if (std::getenv("DWC_COLR_PHASE") != nullptr) {
            static std::atomic<int> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 8) {
                Report("  colr glyph %u subx=%d suby=%d size=%.3f", g.GlyphId(), g.SubX(),
                       g.SubY(), static_cast<double>(d.text_size_render));
            }
        }
    }

    const skia_abi::PreBlend preblend = skia_abi::PreBlend::From(context);
    const bool drawn =
        dwrite_raster::RenderGlyph(face_key, *use, flat, g, preblend, d, image_buffer,
                                   face_index, simulate_bold, simulate_oblique);
    static std::atomic<uint64_t> drawn_count{0};
    static std::atomic<uint64_t> declined_count{0};
    const uint64_t seen =
        drawn ? drawn_count.fetch_add(1, std::memory_order_relaxed) + 1
              : declined_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen <= 3 || (seen & 0x3ff) == 1) {
        Report("%s glyph %u (%ux%u) mask=%u scaler=%u: %llu so far",
               drawn ? "DirectWrite drew" : "declined", g.GlyphId(), g.width, g.height,
               g.mask_format, g.scaler_bits, static_cast<unsigned long long>(seen));
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
    // Reading a symbol table needs the filesystem, and a build that compiles
    // HarfBuzz in is only reachable through one.
    bold_shaping::InstallAtLoad();
    // Reads the mapped image to find where the run's weight lives, which the
    // per-character fallback drops before it asks.
    bold_weight::InstallAtLoad();
    colr_outline::InstallAtLoad();
    weight_style::InstallAtLoad();
    weight_style::InstallFontconfig();
    // Mapping the bold faces needs the filesystem too, and a renderer forked
    // from here inherits the mappings it can no longer make for itself.
    bold_fallback::MapAtLoad();
    // Creating the factory needs the filesystem, so a renderer that waits
    // until it is sandboxed gets nothing. One forked from here inherits a built
    // factory, and its fonts come from memory.
    if (g_patched.load(std::memory_order_acquire) && chromium_patch::ParityWanted()) {
        (void)dwrite_raster::Available();
    }
}
