//+--------------------------------------------------------------------------
//
//  libxul_patch.cpp - the interception points outside FreeType.
//
//  Some of what Firefox on Windows does never reaches FreeType, because Gecko
//  answers it itself. Those things are intercepted here.
//
//  Font fallback. gfxWindowsPlatform::GetCommonFallbackFonts names the
//  families to try for a script and gfxPlatformGtk::GetCommonFallbackFonts
//  names a different set, with no FreeType entry point carrying the question,
//  so the vtable slot holding the Linux one is replaced.
//
//  The underline of a bad-underline family. Windows calls
//  gfxFont::SanitizeMetrics with aIsBadUnderlineFont true for the CJK families
//  in font.blacklist.underline_offset, which lowers the underline to at least
//  two pixels below the baseline; gfxFT2FontBase.cpp passes a literal false,
//  so on Linux the deepest an underline can go is maxDescent - underlineSize,
//  and MS Gothic at 13px stops a pixel short. The flag is a constant at that
//  call site, so the branch is not compiled in and nothing fed through the
//  post table reaches it. The two underline fields of the gfxFont::Metrics
//  InitMetrics just filled are set afterwards instead. The struct is found by
//  three of its own values: gfx/thebes/gfxFont.h declares Metrics as nineteen
//  consecutive gfxFloats, and the shim knows what emHeight, maxAscent and
//  maxDescent came out as, so the run of doubles that matches all three at the
//  right spacing is the struct. It acts on one match and otherwise does
//  nothing.
//
//  The font-units-to-pixels factor. gfxFT2FontBase::InitMetrics takes it from
//  FreeType's x_scale, which was built from a size rounded to 1/64 px;
//  gfxDWriteFont::ComputeMetrics divides the unrounded size by the design
//  units per em. COLRFonts scales a paint graph by it and
//  gfxFont::CreateVerticalMetrics scales the OS/2 and vhea fields by it, so
//  the field is put back beside the metrics.
//
//  The strikeout line. gfxFT2FontBase::InitMetrics ends its strikeout block
//  with SnapLineToPixels, which takes the thickness to a whole number of
//  pixels and rounds the offset with it; gfxDWriteFont::ComputeMetrics leaves
//  both fractional. The two fields are put back unsnapped beside the underline
//  ones.
//
//  The bad-underline bit itself. gfxDWriteFontList marks a family from the
//  same list; gfxFcPlatformFontList passes a literal false, so on Linux
//  gfxFontGroup::GetUnderlineOffset never takes the minimum across the group
//  the way Windows does. The bit lives in the shared font list, which the
//  parent process maps writable, so it is set there directly. See the comment
//  above MarkBadUnderlineFamiliesOnce.
//
//  The platform a media query answers with. The UA stylesheet puts
//  font-variant-east-asian: ruby on rt and rtc inside
//  `@media not (-moz-platform: windows)`, so everywhere but Windows a ruby
//  annotation is shaped with the font's ruby glyph designs. The feature
//  reaches the shaper long before any font call, so the answer is moved where
//  the query is evaluated. See the comment above FindPlatformCompare.
//
//  Translated from:
//    gfx/thebes/gfxWindowsPlatform.cpp  gfxWindowsPlatform::GetCommonFallbackFonts
//    gfx/thebes/gfxDWriteFontList.cpp   gfxDWriteFontList::PlatformGlobalFontFallback
//    gfx/thebes/gfxPlatformFontList.cpp CommonFontFallback, then GlobalFontFallback
//    gfx/thebes/gfxPlatformGtk.cpp      gfxPlatformGtk::GetCommonFallbackFonts
//    gfx/thebes/gfxFT2FontBase.cpp      gfxFT2FontBase::InitMetrics
//    gfx/thebes/gfxFont.cpp             gfxFont::SanitizeMetrics
//    gfx/thebes/gfxTextRun.cpp          gfxFontGroup::GetUnderlineOffset
//    gfx/thebes/gfxFcPlatformFontList.cpp  the two InitData call sites
//    layout/style/res/html.css          the rt and rtc rule for ruby glyphs
//    layout/style/nsMediaFeatures.cpp   Gecko_MediaFeatures_MatchesPlatform
//    servo/components/style/gecko/media_features.rs  enum Platform
//    gfx/thebes/SharedFontList.h        fontlist::Pointer, String, Family
//    gfx/thebes/SharedFontList-impl.h   FontList::Header and BlockHeader
//    gfx/thebes/gfxPlatform.h           FontPresentation, PrefersColor
//    intl/unicharutil/nsUGenCategory.h  kPunctuation and kSymbol groupings
//    xpcom/ds/nsTArray.h                nsTArrayHeader, and that the allocator
//                                       is plain malloc/realloc/free
//  The families, script codes and per-entry conditions come from
//  firefox_parity_data.h, generated from that tree.
//
//  Nothing here may be tied to a build, a distribution or a Firefox version.
//  libxul is found by soname wherever it was loaded from, the function to
//  replace from an anchor in its own rodata, its vtable slot by searching the
//  relocated read-only data, all through program headers alone so that a
//  stripped libxul is no obstacle, and the allocator is whichever malloc
//  libxul resolved.
//
//  Every step demands a unique answer, and where one is ambiguous - a Firefox
//  whose fallback list was rewritten, an optimizer that split the function, a
//  build with no eh_frame - nothing is written at all and the process runs as
//  if this file were not here.
//
//----------------------------------------------------------------------------

#if CLEARTYPE_FIREFOX_PARITY

#include "firefox_parity_data.h"
#include "parity_mode.h"
#include "shim_exports.h"

#include "dwrite_fallback_table.h"

#include <cstdarg>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <atomic>

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>


// Every comparison this reports asks whether a value is exactly a particular
// one, and two of them compare a box against the copy of it this library
// wrote, where a tolerance would match a neighboring box and a bitwise test
// would miss the two zeroes that differ only in sign.
#pragma GCC diagnostic ignored "-Wfloat-equal"

// The bounds and the resolved addresses this file works from come from the
// host, so a guard on one reads to the analysis as a guard that never holds
// and a bound passed from a single call site reads as a constant. Both are
// what keeps a mismatched build from being written to.
// ReSharper disable CppDFAConstantConditions
// ReSharper disable CppDFAConstantParameter

// Making one of these const would only put a const_cast at the call that hands
// the address on, since the shim's own entry points take a mutable pointer.
// ReSharper disable CppLocalVariableMayBeConst

// The type of an atomic is what says how the field is shared, so it is spelled
// out rather than deduced.
// ReSharper disable CppTemplateArgumentsCanBeDeduced

// Declined for the same reason as in cleartype/src/freetype.cpp.
// ReSharper disable CppTooWideScopeInitStatement
// ReSharper disable CppUseStructuredBinding

// The original load_glyph, and the stub that runs in front of it.
//
// The stub names the FontInstance to the shim and then jumps to the original,
// so the tail call leaves the return value and any hidden return pointer
// exactly as the caller laid them out and nothing needs to know the shape of
// what load_glyph returns. Only the argument registers are saved, since that
// is all a call can disturb that the original still needs.
extern "C" {
void* g_wr_load_glyph_orig = nullptr;
void DwcWrLoadGlyphThunk(void);
}

__asm__(".text\n"
        ".globl DwcWrLoadGlyphThunk\n"
        ".hidden DwcWrLoadGlyphThunk\n"
        ".type DwcWrLoadGlyphThunk, @function\n"
        "DwcWrLoadGlyphThunk:\n"
        "  .cfi_startproc\n"
        "  push %rdi\n"
        "  .cfi_adjust_cfa_offset 8\n"
        "  push %rsi\n"
        "  .cfi_adjust_cfa_offset 8\n"
        "  push %rdx\n"
        "  .cfi_adjust_cfa_offset 8\n"
        "  push %rcx\n"
        "  .cfi_adjust_cfa_offset 8\n"
        "  push %r8\n"
        "  .cfi_adjust_cfa_offset 8\n"
        "  push %r9\n"
        "  .cfi_adjust_cfa_offset 8\n"
        "  sub $8, %rsp\n"
        "  .cfi_adjust_cfa_offset 8\n"
        "  mov %rdx, %rdi\n"
        "  call CleartypeNoteWebRenderInstance@PLT\n"
        "  add $8, %rsp\n"
        "  .cfi_adjust_cfa_offset -8\n"
        "  pop %r9\n"
        "  .cfi_adjust_cfa_offset -8\n"
        "  pop %r8\n"
        "  .cfi_adjust_cfa_offset -8\n"
        "  pop %rcx\n"
        "  .cfi_adjust_cfa_offset -8\n"
        "  pop %rdx\n"
        "  .cfi_adjust_cfa_offset -8\n"
        "  pop %rsi\n"
        "  .cfi_adjust_cfa_offset -8\n"
        "  pop %rdi\n"
        "  .cfi_adjust_cfa_offset -8\n"
        "  jmp *g_wr_load_glyph_orig(%rip)\n"
        "  .cfi_endproc\n"
        ".size DwcWrLoadGlyphThunk, .-DwcWrLoadGlyphThunk\n");

namespace {

// Varargs and not a parameter pack, for the reason given above LogLine in
// cleartype/src/freetype.cpp.
// NOLINTNEXTLINE(cert-dcl50-cpp)
void Report(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// NOLINTNEXTLINE(cert-dcl50-cpp)  -- see the declaration above
void Report(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    (void)std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    CleartypeLogLine(buf);
}

// ReSharper disable once CppDFAConstantParameter
bool EnvDisables(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && (std::strcmp(v, "0") == 0 || strcasecmp(v, "off") == 0 ||
                            strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0);
}

// ---------------------------------------------------------------------------
// The image.
// ---------------------------------------------------------------------------

struct Region
{
    const unsigned char* begin = nullptr;
    const unsigned char* end = nullptr;
};

struct Image
{
    const char* path = nullptr;
    uintptr_t base = 0;
    const ElfW(Phdr)* phdr = nullptr;
    ElfW(Half) phnum = 0;
    Region rodata[8];      // readable, not writable, not executable
    unsigned rodata_count = 0;
    Region data[4];        // writable, where a runtime value lands
    unsigned data_count = 0;
    Region text[4];        // executable
    unsigned text_count = 0;
    Region relro;          // PT_GNU_RELRO, where the vtables live once relocated
    const unsigned char* eh_frame_hdr = nullptr;
    // How much of it is mapped, which is what bounds the FDE count below.
    size_t eh_frame_hdr_size = 0;
};

// libxul, whatever it is called on disk and wherever it was installed from.
// The loader's own recorded name for an image, which is the path it was opened
// by. Weaker than asking the object, and used only where asking is not
// available: LookForLibxul runs inside dl_iterate_phdr, where dlsym can
// deadlock against a concurrent dlopen, and libxul carries no DT_SONAME to
// read out of its dynamic section instead. Replacing it would mean walking
// each image's own symbol table for XRE_GetBootstrap, which needs the symbol
// count out of DT_GNU_HASH. Not worth it against a build that renames libxul,
// which nothing ships; ScanLoadedImages reports the miss if one ever does.
bool IsLibxul(const char* name)
{
    if (name == nullptr || *name == '\0') {
        return false;
    }
    const char* slash = std::strrchr(name, '/');
    const char* base = slash != nullptr ? slash + 1 : name;
    return std::strcmp(base, "libxul.so") == 0;
}

bool DescribeImage(const char* path, const uintptr_t base, const ElfW(Phdr)* phdr,
                   ElfW(Half) const phnum, Image* out)
{
    out->path = path;
    out->base = base;
    out->phdr = phdr;
    out->phnum = phnum;
    for (ElfW(Half) i = 0; i < phnum; ++i) {
        const ElfW(Phdr)& p = phdr[i];
        const auto* begin = reinterpret_cast<const unsigned char*>(base + p.p_vaddr);
        const unsigned char* end = begin + p.p_filesz;
        if (p.p_type == PT_LOAD) {
            if ((p.p_flags & PF_X) != 0) {
                if (out->text_count < 4) {
                    out->text[out->text_count++] = {.begin = begin, .end = end};
                }
            } else if ((p.p_flags & PF_W) == 0) {
                if (out->rodata_count < 8) {
                    out->rodata[out->rodata_count++] = {.begin = begin, .end = end};
                }
            } else if (out->data_count < 4) {
                // A value written at run time can sit past the end of what
                // the file holds, so the region spans p_memsz.
                out->data[out->data_count++] = {.begin = begin,
                                                .end = begin + p.p_memsz};
            }
        } else if (p.p_type == PT_GNU_RELRO) {
            out->relro = {.begin = begin, .end = end};
        } else if (p.p_type == PT_GNU_EH_FRAME) {
            out->eh_frame_hdr = begin;
            out->eh_frame_hdr_size = static_cast<size_t>(p.p_memsz);
        }
    }
    return out->text_count != 0 && out->rodata_count != 0 &&
           out->relro.begin != nullptr && out->eh_frame_hdr != nullptr;
}

// ---------------------------------------------------------------------------
// Finding the function.
//
// gfxPlatformGtk::GetCommonFallbackFonts is built from a run of static
// const char[] family names, which the compiler lays out adjacently in the
// order the source declares them. That run is the anchor: it is one blob of
// bytes that no other part of libxul contains, and the function that reaches
// for it more than any other is the one to replace.
// ---------------------------------------------------------------------------

// gfx/thebes/gfxPlatformGtk.cpp: kFontDejaVuSerif .. kFontSymbola, in order.
constexpr char kNameAnchor[] = "DejaVu Serif\0FreeSerif\0DejaVu Sans\0FreeSans\0Symbola";

// How many separate references into the block the winning function must make.
// The source appends eleven families from it; a runner-up that merely mentions
// one name is not a candidate.
constexpr unsigned kMinAnchorReferences = 6;

// ReSharper disable once CppDFAConstantParameter
const unsigned char* FindUnique(const Image& image, const void* needle, const size_t len)
{
    const unsigned char* found = nullptr;
    for (unsigned i = 0; i < image.rodata_count; ++i) {
        const Region& r = image.rodata[i];
        const unsigned char* at = r.begin;
        while (at + len <= r.end) {
            const void* hit = memmem(at, static_cast<size_t>(r.end - at), needle, len);
            if (hit == nullptr) {
                break;
            }
            if (found != nullptr) {
                return nullptr;             // not unique, so not an anchor
            }
            found = static_cast<const unsigned char*>(hit);
            at = found + 1;
        }
    }
    return found;
}

// .eh_frame_hdr's binary search table: every function start in the image, in
// order. Version 1 with a datarel sdata4 table is what every toolchain in use
// emits, and this declines to interpret anything else.
struct FunctionStarts
{
    const int32_t* table = nullptr;         // pairs: initial location, FDE
    // .eh_frame_hdr is a few megabytes at the very most, and each entry is
    // eight bytes, so this ceiling sits far above anything real.
    static constexpr uint32_t kMaxFdeCount = 1u << 24;

    uint32_t count = 0;
    uintptr_t datarel = 0;

    bool Parse(const unsigned char* hdr, const size_t size)
    {
        // Twelve bytes of header before the table starts.
        if (hdr == nullptr || size < 12 || hdr[0] != 1) {
            return false;
        }
        const unsigned char fde_count_enc = hdr[2];
        const unsigned char table_enc = hdr[3];
        if (fde_count_enc != 0x03 /* udata4 */ ||
            table_enc != 0x3b /* datarel | sdata4 */) {
            return false;
        }
        std::memcpy(&count, hdr + 8, sizeof(count));
        // Two int32 per entry, indexed as table[2 * i]: past this bound the
        // index calculation would wrap uint32_t, and long before that the
        // table would have run off the end of .eh_frame_hdr. A header this
        // code did not write cannot steer the search out of the section.
        if (count > kMaxFdeCount) {
            return false;
        }
        // Two int32 per entry after the header, all of which has to lie inside
        // the segment the program headers described.
        if (static_cast<uint64_t>(count) * 8 + 12 > static_cast<uint64_t>(size)) {
            return false;
        }
        table = reinterpret_cast<const int32_t*>(hdr + 12);
        datarel = reinterpret_cast<uintptr_t>(hdr);
        return count != 0;
    }

    uintptr_t Start(const uint32_t i) const
    {
        // The table holds signed displacements from `datarel`; the wrap that
        // makes a negative one subtract is the arithmetic, so it is spelled out.
        return datarel + static_cast<uintptr_t>(
                             static_cast<intptr_t>(table[2 * static_cast<size_t>(i)]));
    }

    // The function containing an address: the last entry that starts at or
    // before it.
    uintptr_t Enclosing(const uintptr_t addr) const
    {
        if (count == 0 || addr < Start(0)) {
            return 0;
        }
        uint32_t lo = 0, hi = count - 1;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo + 1) / 2;
            if (Start(mid) <= addr) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        return Start(lo);
    }

    // How far the function starting here reaches, taken as the distance to the
    // next start. The table is sorted and covers the text, so this is the
    // function's own size wherever the next entry belongs to another function.
    size_t Extent(const uintptr_t start) const
    {
        if (count == 0) {
            return 0;
        }
        uint32_t lo = 0, hi = count - 1;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo + 1) / 2;
            if (Start(mid) <= start) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        if (Start(lo) != start || lo + 1 >= count) {
            return 0;
        }
        return static_cast<size_t>(Start(lo + 1) - start);
    }
};

// One `lea reg, [rip + disp32]` whose target lands in [lo, hi).
bool IsRipLeaTo(const unsigned char* p, const uintptr_t lo, const uintptr_t hi)
{
    if (p[0] < 0x48 || p[0] > 0x4F || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) {
        return false;                        // not REX.W lea reg, [rip + d32]
    }
    int32_t disp;
    std::memcpy(&disp, p + 3, sizeof(disp));
    const uintptr_t target = reinterpret_cast<uintptr_t>(p) + 7 +
                             static_cast<uintptr_t>(static_cast<intptr_t>(disp));
    return target >= lo && target < hi;
}

// The anchor is a parameter: which bytes identify the function is a property
// of the libxul build.
// ReSharper disable once CppDFAConstantParameter
uintptr_t FindOwningFunction(const Image& image, const FunctionStarts& starts,
                             const unsigned char* block, const size_t block_len)
{
    const uintptr_t lo = reinterpret_cast<uintptr_t>(block);
    const uintptr_t hi = lo + block_len;

    // The two best candidates by reference count. The decision below is only
    // whether the winner beat the runner-up, so no runner-up address is kept.
    uintptr_t best = 0;
    unsigned best_n = 0, second_n = 0;
    uintptr_t current = 0;
    unsigned current_n = 0;

    auto offer = [&](const uintptr_t fn, const unsigned n) {
        if (fn == 0 || n == 0) {
            return;
        }
        if (n > best_n) {
            second_n = best_n;
            best = fn; best_n = n;
        } else if (fn != best && n > second_n) {
            second_n = n;
        }
    };

    for (unsigned i = 0; i < image.text_count; ++i) {
        const Region& r = image.text[i];
        for (const unsigned char* p = r.begin; p + 7 <= r.end; ++p) {
            if (!IsRipLeaTo(p, lo, hi)) {
                continue;
            }
            const uintptr_t fn = starts.Enclosing(reinterpret_cast<uintptr_t>(p));
            if (fn != current) {
                offer(current, current_n);
                current = fn;
                current_n = 0;
            }
            ++current_n;
        }
    }
    offer(current, current_n);

    if (best_n < kMinAnchorReferences || best_n == second_n) {
        Report("libxul: the fallback anchor names no single function "
               "(best %u, runner-up %u references)", best_n, second_n);
        return 0;
    }
    return best;
}

// The one place in the relocated read-only data that holds this function: its
// vtable slot. More than one, and which to write is a guess.
void** FindVtableSlot(const Image& image, uintptr_t function);

void** FindVtableSlot(const Image& image, const uintptr_t function)
{
    void** found = nullptr;
    const auto* p = reinterpret_cast<const uintptr_t*>(image.relro.begin);
    const auto* end = reinterpret_cast<const uintptr_t*>(image.relro.end);
    for (; p + 1 <= end; ++p) {
        if (*p != function) {
            continue;
        }
        if (found != nullptr) {
            Report("libxul: %#lx is stored in more than one place", function);
            return nullptr;
        }
        found = const_cast<void**>(reinterpret_cast<void* const*>(p));
    }
    return found;
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
    // ReSharper disable once CppRedundantParentheses
    const uintptr_t last = (reinterpret_cast<uintptr_t>(slot) + sizeof(void*) - 1) &
                     ~static_cast<uintptr_t>(page - 1);
    const size_t len = last - first + static_cast<size_t>(page);
    auto* at = reinterpret_cast<void*>(first);
    if (mprotect(at, len, PROT_READ | PROT_WRITE) != 0) {
        Report("libxul: the vtable page would not open for writing");
        return false;
    }
    *slot = value;
    if (mprotect(at, len, PROT_READ) != 0) {
        Report("libxul: the vtable page would not close again");
        // The slot is written and correct; leaving it writable is worse than
        // it was but not wrong. Nothing to undo.
    }
    return true;
}

// ---------------------------------------------------------------------------
// The replacement.
// ---------------------------------------------------------------------------

using GetCommonFallbackFontsFn = void (*)(void* self, uint32_t ch, int32_t script,
                                          uint8_t presentation, void* font_list);

GetCommonFallbackFontsFn g_original = nullptr;

// gfxFT2FontBase::InitMetrics takes only its object.
using InitMetricsFn = void (*)(void* self);
InitMetricsFn g_init_metrics = nullptr;

// gfx/thebes/gfxFont.h: struct Metrics, nineteen gfxFloats in this order.
constexpr unsigned kMetricsCapHeight = 0;
constexpr unsigned kMetricsXHeight = 1;
constexpr unsigned kMetricsStrikeoutSize = 2;
constexpr unsigned kMetricsStrikeoutOffset = 3;
constexpr unsigned kMetricsUnderlineSize = 4;
constexpr unsigned kMetricsUnderlineOffset = 5;
constexpr unsigned kMetricsInternalLeading = 6;
constexpr unsigned kMetricsExternalLeading = 7;
constexpr unsigned kMetricsEmHeight = 8;
constexpr unsigned kMetricsEmAscent = 9;
constexpr unsigned kMetricsEmDescent = 10;
constexpr unsigned kMetricsMaxHeight = 11;
constexpr unsigned kMetricsMaxAscent = 12;
constexpr unsigned kMetricsMaxDescent = 13;
constexpr unsigned kMetricsMaxAdvance = 14;
constexpr unsigned kMetricsAveCharWidth = 15;
constexpr unsigned kMetricsSpaceWidth = 16;
constexpr unsigned kMetricsZeroWidth = 17;
constexpr unsigned kMetricsIdeographicWidth = 18;
constexpr unsigned kMetricsFields = 19;

// How far into the object to look for it. gfxFT2FontBase::mMetrics sits after
// everything gfxFont carries; this is generous and bounded.
constexpr size_t kMetricsSearchBytes = 1024;

// No offset found yet, for the cache in FindMetrics.
constexpr size_t kMetricsNotFound = static_cast<size_t>(-1);

bool SameDouble(const double a, const double b)
{
    const double d = a - b;
    return (d < 0 ? -d : d) < 1e-6;
}

// ---------------------------------------------------------------------------
// The bad-underline flag Linux never sets.
// ---------------------------------------------------------------------------
//
// gfxDWriteFontList.cpp hands fontlist::Family::InitData the answer to
// mBadUnderlineFamilyNames.ContainsSorted(key); gfxFcPlatformFontList.cpp
// passes a literal false at both of its call sites, so no family is ever
// marked here and the branch for one in gfxFontGroup::GetUnderlineOffset is
// dead code. The group then answers with its first valid font's offset alone,
// where Windows answers with the minimum across the group - for a lang="ja"
// group that is MS PGothic's, two pixels lower.
//
// It shows wherever the first font's own descent is shallower than the
// underline that offset implies, which is exactly what the -moz-bullet-font
// list marker is: its frame measures 14px here against 16px on Windows, which
// moves the first line of every <li> half a pixel and rounds whichever entries
// sit on the boundary a whole pixel down.
//
// No pref reaches the flag - the list is loaded on Linux, just never consulted
// - and the call site that would carry it is an inlined constant in a function
// with no symbol. The bit itself is reachable, though: the shared font list is
// a self-describing structure in shared memory, and SharedFontList.cpp's
// FreezeWithMutableMapping keeps the parent's mapping writable while the
// children get a read-only one. Writing it in the parent is what Windows
// stores in that same field, and every child reads it back through the same
// pages.
//
// Only the parent runs any of this, and MarkBadUnderlineFamilies enforces
// that. A child has no writable mapping of the list to find, but it does have
// other rw-shared mappings, and letting it search them means dereferencing
// buffers that a busy page unmaps under the walk.
//
// Nothing below is taken on trust. Addresses out of /proc/self/maps are read
// through ReadWithoutFaulting, since the file describes what was mapped and
// not what still is. The block must then describe its own size, the family
// array must fit inside what the block says it has allocated, every key must
// decode to printable text, and the keys must come out in the sorted order
// the list is built in. If any of it fails this declines and Firefox keeps
// its own answer.
//
// A font list rebuilt mid-session (a font installed while Firefox runs) comes
// back with the flag clear, and this does not run a second time.

// SharedFontList-impl.h FontList::Header. The first two fields are the
// BlockHeader that every block, not only this one, begins with.
// ReSharper disable CppDeclaratorNeverUsed
// Every field is here to place the ones after it, so none of them may go even
// though only some are read.
struct ShmHeader
{
    uint32_t allocated;
    uint32_t block_size;
    uint32_t generation;
    uint32_t family_count;
    uint32_t block_count;
    uint32_t alias_count;
    uint32_t local_face_count;
    uint32_t families;                       // Pointer, below
    uint32_t aliases;
    uint32_t local_faces;
};
// ReSharper restore CppDeclaratorNeverUsed

// SharedFontList.h fontlist::Pointer: a 12-bit block index and a 20-bit
// offset into it, packed into one word.
constexpr uint32_t kPointerNull = 0xffffffffu;
constexpr uint32_t kPointerOffsetMask = (1u << 20) - 1;

uint32_t PointerBlock(const uint32_t p) { return p >> 20; }
uint32_t PointerOffset(const uint32_t p) { return p & kPointerOffsetMask; }

// SharedFontList.h fontlist::Family, listed here by offset:
// +0 mFaceCount, +4/+8 mKey's pointer and length, +12/+16 mName's, +20
// mCharacterMap, +24 mFaces, +28 mIndex, +32 mVisibility, +33 mIsSimple, and
// +34 the four one-bit flags, of which mIsBundled is the first.
constexpr size_t kFamilySize = 36;
constexpr size_t kFamilyKeyPointer = 4;
constexpr size_t kFamilyKeyLength = 8;
constexpr size_t kFamilySimple = 33;   // fontlist::Family::mIsSimple, a bool
constexpr size_t kFamilyFlags = 34;
constexpr uint8_t kBadUnderlineBit = 1u << 1;

// A writable shared mapping, which is what only the parent has of the list.
struct ShmBlock
{
    unsigned char* base;
    size_t size;
};

// Bytes one call may move through the probe pipe. A write of at most PIPE_BUF
// into an empty pipe moves the whole request or none of it, and the largest
// request made here is a thousand bytes.
constexpr size_t kProbeBytes = 4096;

int g_probe_pipe[2] = {-1, -1};
bool g_probe_pipe_failed = false;
pthread_mutex_t g_probe_mutex = PTHREAD_MUTEX_INITIALIZER;

// Reads by handing the address to the kernel to copy from. write() answers
// EFAULT for memory this process cannot read, where a load of the same address
// would fault. Gecko's seccomp policy allows pipe2 with these two flags and
// allows write, so this route is open in a content process.
//
// Every write is drained by the read after it, leaving the pipe empty for the
// next call, so a short write is the tail of the request being unreadable.
bool ReadThroughPipe(const void* addr, void* out, const size_t len)
{
    if (len == 0 || len > kProbeBytes) {
        return false;
    }
    pthread_mutex_lock(&g_probe_mutex);
    if (g_probe_pipe[1] < 0 && !g_probe_pipe_failed) {
        if (pipe2(g_probe_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
            g_probe_pipe[0] = -1;
            g_probe_pipe[1] = -1;
            g_probe_pipe_failed = true;
        }
    }
    ssize_t wrote = -1;
    if (g_probe_pipe[1] >= 0) {
        do {
            wrote = write(g_probe_pipe[1], addr, len);
        } while (wrote < 0 && errno == EINTR);
    }
    size_t have = 0;
    while (wrote > 0 && have < static_cast<size_t>(wrote)) {
        const ssize_t got = read(g_probe_pipe[0],
                                 static_cast<unsigned char*>(out) + have,
                                 static_cast<size_t>(wrote) - have);
        if (got > 0) {
            have += static_cast<size_t>(got);
        } else if (got == 0 || errno != EINTR) {
            break;
        }
    }
    // Anything a short read left behind would be handed to the next caller as
    // its own answer.
    if (wrote > 0 && have != static_cast<size_t>(wrote)) {
        unsigned char scrap[256];
        while (read(g_probe_pipe[0], scrap, sizeof(scrap)) > 0) {
        }
    }
    pthread_mutex_unlock(&g_probe_mutex);
    return have == len;
}

// Whether this process has had process_vm_readv refused by the sandbox.
std::atomic<bool> g_vm_readv_blocked{false};

// Reads `len` bytes from this process without dereferencing the address.
//
// Every address below comes from /proc/self/maps, which describes what was
// mapped when it was read and not what is mapped now. A shared mapping torn
// down in between leaves an address that looks fine and faults on the first
// load, which is a crash inside a host this library promises never to crash.
// Both routes here have the kernel do the reading, so an address this process
// cannot read comes back as an error.
//
// process_vm_readv is one syscall and needs no lock, and Gecko's seccomp
// policy (security/sandbox/linux/SandboxFilter.cpp) gives it to the parent
// alone. A content process traps on every call and each trap is logged, so the
// refusal is remembered and the pipe answers from then on.
// The length is the caller's to give.
// ReSharper disable once CppDFAConstantParameter
bool ReadWithoutFaulting(const void* addr, void* out, const size_t len)
{
    if (addr == nullptr || len == 0) {
        return false;
    }
    if (g_vm_readv_blocked.load(std::memory_order_relaxed)) {
        return ReadThroughPipe(addr, out, len);
    }
    const iovec local = { .iov_base = out, .iov_len = len };
    const iovec remote = { .iov_base = const_cast<void*>(addr), .iov_len = len };
    const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (got >= 0) {
        return got == static_cast<ssize_t>(len);   // short is the object ending inside the range
    }
    if (errno != EPERM && errno != ENOSYS && errno != EACCES) {
        return false;
    }
    g_vm_readv_blocked.store(true, std::memory_order_relaxed);
    return ReadThroughPipe(addr, out, len);
}

// The ceiling belongs to whoever supplied the array, for the same reason.
// ReSharper disable once CppDFAConstantParameter
unsigned WritableSharedMaps(ShmBlock* out, const unsigned max)
{
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        return 0;
    }
    unsigned found = 0;
    char line[512];
    while (found < max && std::fgets(line, sizeof(line), maps) != nullptr) {
        uintptr_t lo = 0;
        uintptr_t hi = 0;
        char perms[8] = {};
        // The count is checked on the next line. The suppression names no
        // check because the inspector that raises it honors only the bare form.
        // NOLINTNEXTLINE
        if (std::sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) {
            continue;
        }
        // Writable and shared, which the font list is only in the parent.
        // A child has none, but it does have other rw-shared mappings, which
        // is why the caller asks GeckoParentProcess() before getting here.
        if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 's') {
            continue;
        }
        if (hi <= lo) {
            continue;
        }
        out[found].base = reinterpret_cast<unsigned char*>(lo);
        out[found].size = hi - lo;
        ++found;
    }
    (void)std::fclose(maps);
    return found;
}

// Every block begins with a BlockHeader describing the mapping it is in.
//
// The whole header is copied out, not just the two fields tested here: every
// read the caller goes on to make of this mapping is inside it, so one probe
// covers them all. Callers that get true still read through `b.base`, which
// is why this is re-run at the start of each pass rather than cached.
bool LooksLikeBlock(const ShmBlock& b)
{
    if (b.size < sizeof(ShmHeader)) {
        return false;
    }
    ShmHeader head;
    if (!ReadWithoutFaulting(b.base, &head, sizeof(head))) {
        return false;
    }
    return head.block_size == b.size && head.allocated <= head.block_size;
}

const char* KeyAt(const ShmBlock* blocks, const unsigned block_count,
                  const uint32_t pointer, const uint32_t length)
{
    if (pointer == kPointerNull || length == 0 || length > 127) {
        return nullptr;
    }
    const uint32_t index = PointerBlock(pointer);
    if (index >= block_count || blocks[index].base == nullptr) {
        return nullptr;
    }
    const uint32_t offset = PointerOffset(pointer);
    const uint32_t allocated = reinterpret_cast<const uint32_t*>(blocks[index].base)[0];
    if (static_cast<uint64_t>(offset) + length > allocated) {
        return nullptr;
    }
    const auto* text = reinterpret_cast<const char*>(blocks[index].base + offset);
    for (uint32_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7f) {
            return nullptr;                  // keys are printable, UTF-8 included
        }
    }
    return text;
}

bool KeyIsBadUnderline(const char* key, const uint32_t length)
{
    for (const char* candidate : firefox_parity::kBadUnderlineFamilies) {
        const size_t n = std::strlen(candidate);
        if (n == length && strncasecmp(candidate, key, n) == 0) {
            return true;
        }
    }
    return false;
}

std::atomic g_fontlist_state{0};        // attempts, or -1 once settled

// Returns the number of families marked, or -1 if the list was not found or
// did not check out. Cheap and side-effect-free until the very last loop.
int MarkBadUnderlineFamiliesOnce()
{
    ShmBlock maps[256];
    const unsigned map_count = WritableSharedMaps(maps, 256);
    if (map_count == 0) {
        return -1;
    }

    // Which mapping is the list's first block: the one whose header describes
    // a family array that fits in what it says it has allocated.
    const ShmHeader* header = nullptr;
    unsigned header_map = 0;
    for (unsigned i = 0; i < map_count; ++i) {
        if (!LooksLikeBlock(maps[i])) {
            continue;
        }
        const auto* h = reinterpret_cast<const ShmHeader*>(maps[i].base);
        if (h->family_count == 0 || h->family_count > 65535) {
            continue;
        }
        if (h->block_count == 0 || h->block_count > 4096) {
            continue;
        }
        if (h->families == kPointerNull || PointerBlock(h->families) != 0) {
            continue;
        }
        const uint64_t end = static_cast<uint64_t>(PointerOffset(h->families)) +
                             static_cast<uint64_t>(h->family_count) * kFamilySize;
        if (end > h->allocated) {
            continue;
        }
        if (header != nullptr) {
            Report("libxul: more than one writable mapping reads as the shared "
                   "font list; leaving the bad-underline flag alone");
            return -1;
        }
        header = h;
        header_map = i;
    }
    if (header == nullptr) {
        return -1;                           // not built yet, or not the parent
    }

    // Block 0 is the header's own mapping. Any other block a key lives in is
    // resolved by elimination: the one candidate under which every key naming
    // it decodes. An ambiguous answer is refused.
    ShmBlock blocks[4096];
    const unsigned block_count = header->block_count;

    // The block and family ceilings validated above bound the indices and not
    // the work, since resolving blocks is block_count x map_count x
    // family_count. This runs on the metrics path, once per attempt.
    constexpr uint64_t kMaxResolveWork = 50ull * 1000 * 1000;
    if (static_cast<uint64_t>(block_count) * (map_count + 1) * header->family_count >
        kMaxResolveWork) {
        Report("libxul: the shared font list would take %llu steps to resolve, so it is not "
               "the structure this expects; leaving the bad-underline flag alone",
               static_cast<unsigned long long>(block_count) * (map_count + 1) *
                   header->family_count);
        return -1;
    }
    for (unsigned b = 0; b < block_count && b < 4096; ++b) {
        blocks[b].base = nullptr;
        blocks[b].size = 0;
    }
    blocks[0] = maps[header_map];

    const unsigned char* families = maps[header_map].base + PointerOffset(header->families);
    for (unsigned b = 1; b < block_count && b < 4096; ++b) {
        bool wanted = false;
        for (uint32_t i = 0; i < header->family_count && !wanted; ++i) {
            const auto* f = reinterpret_cast<const uint32_t*>(families + i * kFamilySize);
            wanted = f[kFamilyKeyPointer / 4] != kPointerNull &&
                     PointerBlock(f[kFamilyKeyPointer / 4]) == b;
        }
        if (!wanted) {
            continue;                        // no key lives there; never needed
        }
        unsigned matches = 0;
        unsigned winner = map_count;
        for (unsigned c = 0; c < map_count; ++c) {
            if (c == header_map || !LooksLikeBlock(maps[c])) {
                continue;
            }
            blocks[b] = maps[c];
            bool all = true;
            for (uint32_t i = 0; i < header->family_count && all; ++i) {
                const auto* f = reinterpret_cast<const uint32_t*>(families + i * kFamilySize);
                const uint32_t kp = f[kFamilyKeyPointer / 4];
                if (kp == kPointerNull || PointerBlock(kp) != b) {
                    continue;
                }
                all = KeyAt(blocks, block_count, kp, f[kFamilyKeyLength / 4]) != nullptr;
            }
            blocks[b].base = nullptr;
            if (all) {
                winner = c;
                if (++matches > 1) {
                    break;
                }
            }
        }
        if (matches != 1) {
            Report("libxul: block %u of the shared font list is %s; leaving the "
                   "bad-underline flag alone", b,
                   matches == 0 ? "not among the writable mappings" : "ambiguous");
            return -1;
        }
        blocks[b] = maps[winner];
    }

    // Every key has to decode, and they have to come out sorted: the list is
    // built that way, and a stride or an offset that is wrong will not
    // reproduce it by accident.
    const char* previous = nullptr;
    uint32_t previous_length = 0;
    for (uint32_t i = 0; i < header->family_count; ++i) {
        const auto* f = reinterpret_cast<const uint32_t*>(families + i * kFamilySize);
        const uint32_t length = f[kFamilyKeyLength / 4];
        const char* key = KeyAt(blocks, block_count, f[kFamilyKeyPointer / 4], length);
        if (key == nullptr) {
            Report("libxul: family %u of %u in the shared font list does not "
                   "read as one; leaving the bad-underline flag alone",
                   i, header->family_count);
            return -1;
        }
        if (previous != nullptr) {
            const uint32_t shortest = previous_length < length ? previous_length : length;
            int order = std::memcmp(previous, key, shortest);
            if (order == 0) {
                order = previous_length < length ? -1 : previous_length > length ? 1 : 0;
            }
            if (order > 0) {
                Report("libxul: the shared font list's families are not in the "
                       "order this expects; leaving the bad-underline flag alone");
                return -1;
            }
        }
        previous = key;
        previous_length = length;
    }

    // Everything above validates the key, and the write is to the flags. A
    // Gecko that reordered the four bitfields in the tail of fontlist::Family
    // would keep sizeof at 36 and keep the keys where they are, pass every
    // check so far, and have this set mIsForceClassic or mIsAltLocale instead,
    // in memory every child process reads back.
    //
    // So the bytes being written are checked against what the declaration can
    // hold. mIsSimple is a bool, and only four bits of the flags byte are
    // declared.
    for (uint32_t i = 0; i < header->family_count; ++i) {
        const unsigned char* f = families + i * kFamilySize;
        if (f[kFamilySimple] > 1 || (f[kFamilyFlags] & 0xF0u) != 0) {
            Report("libxul: family %u has %#x at the flags this would write and %#x beside "
                   "it, which fontlist::Family cannot hold; leaving the bad-underline flag "
                   "alone", i, f[kFamilyFlags], f[kFamilySimple]);
            return -1;
        }
    }

    // Checked out. This is the only write.
    int marked = 0;
    for (uint32_t i = 0; i < header->family_count; ++i) {
        unsigned char* f = const_cast<unsigned char*>(families) + i * kFamilySize;
        const auto* w = reinterpret_cast<const uint32_t*>(f);
        const uint32_t length = w[kFamilyKeyLength / 4];
        const char* key = KeyAt(blocks, block_count, w[kFamilyKeyPointer / 4], length);
        if (key == nullptr || !KeyIsBadUnderline(key, length)) {
            continue;
        }
        if ((f[kFamilyFlags] & kBadUnderlineBit) == 0) {
            f[kFamilyFlags] = static_cast<unsigned char>(f[kFamilyFlags] | kBadUnderlineBit);
            ++marked;
        }
    }
    Report("libxul: %d of %u shared font list families marked bad-underline, "
           "as gfxDWriteFontList would have", marked, header->family_count);
    return marked;
}

// Called from the metrics path, which is where the flag matters and which the
// parent reaches as soon as it lays out any chrome text. Bounded: the list may
// not be built yet on the first calls, and if it never turns up this gives
// up, so nothing ends up reading /proc/self/maps forever.
void MarkBadUnderlineFamilies()
{
    // The parent owns the only writable mapping of the font list. A child has
    // none to find, and walking its rw-shared mappings to discover that means
    // reading graphics and IPC buffers that a busy page creates and destroys
    // continuously.
    if (!dwcft::GeckoParentProcess()) {
        return;
    }
    int state = g_fontlist_state.load(std::memory_order_relaxed);
    if (state < 0 || state > 64) {
        return;
    }
    if (!g_fontlist_state.compare_exchange_strong(state, state + 1,
                                                  std::memory_order_relaxed)) {
        return;                              // another thread is in there
    }
    if (MarkBadUnderlineFamiliesOnce() >= 0) {
        g_fontlist_state.store(-1, std::memory_order_relaxed);
    } else if (state == 64) {
        Report("libxul: the shared font list never turned up in a writable "
               "mapping; the bad-underline flag is left as Linux sets it");
        g_fontlist_state.store(-1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// The gamma SVG text is corrected with.
//
// A glyph mask is blended against the text color through a gamma curve, and on
// Windows that curve is not one value for the whole page.
// gfx/2d/DWriteSettings.cpp answers with its own initializers, sGamma{2.2f} and
// sEnhancedContrast{1.0f}, until gfxVars deliver the system values, and the
// blob rasterizer that draws SVG text runs before they do.
//
// gfx/2d/DrawTargetSkia.cpp UpdateSurfaceProps has no such split on Linux. Its
// MOZ_WIDGET_GTK branch reads gfx.font_rendering.freetype.gamma, which is in
// place before anything draws, so one pair reaches every Skia consumer in the
// process.
//
// WebRender's own text keeps its copy of the pair, which travels with the font
// instance, and canvas draws in a content process while this runs in the
// parent.
// ---------------------------------------------------------------------------

// gfx/skia/skia/include/core/SkSurfaceProps.h, in order. mozilla::Maybe keeps
// the value first, so these four are contiguous wherever the Maybe sits.
struct SurfacePropsShape
{
    uint32_t flags;
    int32_t pixel_geometry;
    float text_contrast;
    float text_gamma;
};

// UpdateSurfaceProps' own arithmetic, so the comparisons below match the
// numbers it wrote. It bounds the two differently.
// ReSharper disable CppDFAUnreachableCode
float GammaAsScalar(const int percent)
{
    return static_cast<float>(percent < 400 ? percent : 400) / 100.0f;
}

float ContrastAsScalar(const int percent)
{
    return static_cast<float>(percent < 100 ? percent : 100) / 100.0f;
}
// ReSharper restore CppDFAUnreachableCode

// kUnknown through kBGR_V, the whole of SkPixelGeometry.
constexpr int32_t kPixelGeometryMax = 4;

// Exact, because the value looked for is the one UpdateSurfaceProps wrote.
// memcmp does that without a float comparison.
bool SameScalar(const float a, const float b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0; // NOLINT
}

bool LooksLikeSurfaceProps(const SurfacePropsShape& s)
{
    return s.flags == 0 && s.pixel_geometry >= 0 && s.pixel_geometry <= kPixelGeometryMax &&
           SameScalar(s.text_contrast, ContrastAsScalar(firefox_parity::kPageEnhancedContrast)) &&
           SameScalar(s.text_gamma, GammaAsScalar(firefox_parity::kPageGamma));
}

// Null while the value has not been written, and null when more than one
// place in the image matches.
float* FindSurfacePropsGamma(const Image& image)
{
    float* found = nullptr;
    for (unsigned i = 0; i < image.data_count; ++i) {
        const Region& r = image.data[i];
        for (const unsigned char* at = r.begin;
             at + sizeof(SurfacePropsShape) <= r.end; at += sizeof(uint32_t)) {
            SurfacePropsShape shape = {};
            std::memcpy(&shape, at, sizeof(shape));
            if (!LooksLikeSurfaceProps(shape)) {
                continue;
            }
            if (found != nullptr) {
                return nullptr;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            auto* value = const_cast<unsigned char*>(at) + offsetof(SurfacePropsShape, text_gamma);
            found = reinterpret_cast<float*>(value);
        }
    }
    return found;
}

Image g_libxul;
std::atomic g_libxul_known{false};
std::atomic g_blob_gamma_state{0};       // attempts, or -1 once settled

// 1 once the value is moved, 0 while there is nothing to move.
int SetBlobTextGammaOnce()
{
    if (!g_libxul_known.load(std::memory_order_acquire)) {
        return 0;
    }
    float* gamma = FindSurfacePropsGamma(g_libxul);
    if (gamma == nullptr) {
        return 0;
    }
    *gamma = GammaAsScalar(firefox_parity::kBlobGamma);
    Report("libxul: SVG text gamma %.2f rather than %.2f, as DWriteSettings' "
           "initializers leave it on Windows",
           static_cast<double>(GammaAsScalar(firefox_parity::kBlobGamma)),
           static_cast<double>(GammaAsScalar(firefox_parity::kPageGamma)));
    return 1;
}

// The value appears the first time this process draws through Skia, which is
// after everything here has loaded, so this retries within a bound.
void SetBlobTextGamma()
{
    if (!dwcft::GeckoParentProcess()) {
        return;                              // the blob rasterizer is the parent's
    }
    int state = g_blob_gamma_state.load(std::memory_order_relaxed);
    if (state < 0 || state > 64) {
        return;
    }
    if (!g_blob_gamma_state.compare_exchange_strong(state, state + 1,
                                                    std::memory_order_relaxed)) {
        return;                              // another thread is in there
    }
    if (SetBlobTextGammaOnce() > 0) {
        g_blob_gamma_state.store(-1, std::memory_order_relaxed);
    } else if (state == 64) {
        Report("libxul: no surface properties to read a text gamma out of; SVG "
               "text keeps the gamma the rest of the page is drawn with");
        g_blob_gamma_state.store(-1, std::memory_order_relaxed);
    }
}

// Installed into libxul by address and called by name from nowhere, so the
// declaration here is the only one there will be.
extern "C" void DwcInitMetrics(void* self);
// The one place in the object where those three sit at the spacing
// gfxFont::Metrics gives them. Null when no candidate matches, or when more
// than one does and the struct is therefore not identifiable.
double* FindMetrics(void* self, const double em, const double asc, const double desc)
{
    // ComputeMetrics on Windows writes mAdjustedSize into emHeight unrounded,
    // while gfxFT2FontBase::InitMetrics writes floor(emHeight + 0.5). The two
    // agree at a whole-pixel size and part at every other one, so both are
    // accepted, and maxHeight is checked as well so the wider test cannot land
    // on some other run of doubles. InitMetrics sets maxHeight from the sum,
    // and SanitizeMetrics, which runs after it, recomputes maxHeight on both of
    // the paths where it moves an ascent or a descent.
    //
    // InitMetrics rounds units_per_EM * yScale and not the size itself, and
    // yScale carries FreeType's 16.16 scale, which reconstructs the size a
    // fraction low. A size that ends in half a pixel therefore floors to the
    // pixel below the one the size rounds to, so that value is accepted too.
    const double rounded = std::floor(em + 0.5);
    const double under = std::ceil(em - 0.5);
    auto* base = static_cast<unsigned char*>(self);

    auto matches = [&](const double* c) {
        const double got = c[kMetricsEmHeight];
        return (SameDouble(got, em) || SameDouble(got, rounded) || SameDouble(got, under)) &&
               SameDouble(c[kMetricsMaxAscent], asc) && SameDouble(c[kMetricsMaxDescent], desc) &&
               SameDouble(c[kMetricsMaxHeight], asc + desc);
    };

    // The offset of mMetrics inside gfxFT2FontBase is fixed by the build, so
    // once it is known only the struct itself is read.
    static std::atomic known{kMetricsNotFound};
    const size_t cached = known.load(std::memory_order_relaxed);
    if (cached != kMetricsNotFound) {
        // Read directly, since the offset was verified on this build's
        // layout and these bytes are inside the object the caller holds.
        // ReadWithoutFaulting's content-process fallback would refuse a read
        // that crosses the page boundary even though the object spans it.
        double one[kMetricsFields];
        std::memcpy(one, base + cached, sizeof(one));
        if (matches(one)) {
            return reinterpret_cast<double*>(base + cached);
        }
    }

    // The window is a guess at how far into the object mMetrics can sit, and
    // the object does not say how long it is, so reading it directly runs off
    // the end. process_vm_readv copies what is mapped and fails instead of
    // faulting, so the window shrinks to what the object's page holds.
    unsigned char window[kMetricsSearchBytes];
    size_t have = kMetricsSearchBytes;
    while (have >= kMetricsFields * sizeof(double) && !ReadWithoutFaulting(self, window, have)) {
        have /= 2;
    }
    if (have < kMetricsFields * sizeof(double)) {
        return nullptr;
    }

    double* found = nullptr;
    for (size_t at = 0; at + kMetricsFields * sizeof(double) <= have; at += sizeof(double)) {
        const auto* candidate = reinterpret_cast<const double*>(window + at);
        if (matches(candidate)) {
            if (found != nullptr) {
                return nullptr;              // ambiguous, so nothing is written
            }
            found = reinterpret_cast<double*>(base + at);
            known.store(at, std::memory_order_relaxed);
        }
    }
    return found;
}

// The distance back from the metrics struct to mAdjustedSize, in doubles.
// Fixed by the build, so it is learned once and used from then on.
std::atomic<size_t> g_adjusted_size_delta{0};

// gfxFont::mAdjustedSize, the size everything below the metrics is scaled
// from and the one gfxFontconfigFont::GetScaledFont hands WebRender.
//
// gfx/thebes/gfxFont.h declares it between the style and a pair of tracking
// fields that nothing has touched by the time InitMetrics returns:
//
//     gfxFontStyle mStyle;
//     mutable gfxFloat mAdjustedSize;
//     gfxFloat mTracking = 0.0;
//     gfxFloat mCachedTrackingSize = -1.0;
//
// gfxFT2FontBase::mMetrics sits after everything gfxFont carries, so the field
// lies between the object's start and the struct FindMetrics located. The two
// trailing fields identify it; the size on its own appears several times over.
double* FindAdjustedSize(void* self, const double* metrics, const double size)
{
    // A second run of doubles matching the same pattern makes the search
    // ambiguous and nothing is written.
    std::atomic<size_t>& known = g_adjusted_size_delta;

    const auto* base = static_cast<const double*>(self);
    if (metrics < base + 3) {
        return nullptr;
    }
    if (const size_t delta = known.load(std::memory_order_relaxed); delta != 0) {
        double* at = const_cast<double*>(metrics) - delta;
        if (at >= base && SameDouble(at[0], size)) {
            return at;
        }
    }
    double* found = nullptr;
    for (const double* at = metrics - 3; at >= base; --at) {
        if (SameDouble(at[0], size) && SameDouble(at[1], 0.0) && SameDouble(at[2], -1.0)) {
            if (found != nullptr) {
                return nullptr;              // ambiguous, so nothing is written
            }
            found = const_cast<double*>(at);
        }
    }
    if (found != nullptr) {
        known.store(static_cast<size_t>(metrics - found), std::memory_order_relaxed);
    }
    return found;
}

// Where the RefPtr<SharedFTFace> sits in the font, in words from its start.
// gfxFT2FontBase declares `RefPtr<mozilla::gfx::SharedFTFace> mFTFace; Metrics
// mMetrics;`, so it is the word before the struct FindMetrics locates. Learned
// from the first font whose struct was found, then used before the accessors
// are asked anything.
std::atomic<size_t> g_ftface_word{0};

// SharedFTFace keeps `FT_Face mFace` behind an atomic refcount, so the face is
// one of the first few words of it. Each word is offered to the shim, which
// knows its own faces and refuses anything else.
// mFTSize, the size this font set the face to. `Metrics mMetrics; int
// mFTLoadFlags; bool mEmbolden; gfxFloat mFTSize;` puts it one word past the
// struct, the int and the bool sharing that word.
constexpr size_t kFTSizeWord = kMetricsFields + 1;

// The address of the `words`th pointer-sized field of an object.
//
// The reads and writes below are all of a field at a word offset, and memcpy
// takes void pointers, so handing it the word address directly is a conversion
// through two levels of indirection that says nothing about what is copied.
// Naming the address once keeps those calls reading as the field accesses they
// are.
void* WordAt(void* self, const size_t words)
{
    return static_cast<void*>(static_cast<void**>(self) + words);
}

bool SameBits(float a, float b);

// gfxFont::mFUnitsConvFactor, the one float between the object's start and its
// metrics that holds the value gfxFT2FontBase::InitMetrics just wrote there.
// gfxFont declares it and gfxFT2FontBase declares the metrics, so it lies at a
// lower address, and the bits are an exact fingerprint, so a second float
// reading alike leaves neither one nameable and nothing is written.
float* FindUnitsPerPixel(void* self, const double* metrics, const double linux_factor)
{
    const auto stop = reinterpret_cast<const unsigned char*>(metrics);
    auto* from = static_cast<unsigned char*>(self);
    if (stop <= from) {
        return nullptr;
    }
    const auto want = static_cast<float>(linux_factor);
    float* found = nullptr;
    for (unsigned char* at = from; at + sizeof(float) <= stop; at += sizeof(float)) {
        float seen = 0.0f;
        std::memcpy(&seen, at, sizeof(seen));
        if (!SameBits(seen, want)) {
            continue;
        }
        if (found != nullptr) {
            return nullptr;
        }
        found = reinterpret_cast<float*>(at);
    }
    return found;
}

void ClaimOwnFace(void* self)
{
    const size_t at = g_ftface_word.load(std::memory_order_relaxed);
    if (self == nullptr || at == 0) {
        return;
    }
    auto* const shared = static_cast<void* const*>(self)[at - 1];
    if (shared == nullptr) {
        return;
    }
    // The face alone does not name an instance. Several fonts share one face
    // at sizes that round to the same ppem, and the face carries whichever was
    // installed last, so the size goes with the claim. A size out of range is
    // passed as zero and the shim falls back to the one the face carries.
    double ft_size = 0.0;
    std::memcpy(&ft_size, WordAt(self, at + kFTSizeWord), sizeof(ft_size));
    if (!(ft_size > 0.0) || !(ft_size < 65536.0)) {
        ft_size = 0.0;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (CleartypeClaimFace(static_cast<void* const*>(shared)[i], ft_size) != 0) {
            return;
        }
    }
}

// The size this font is about to be measured at, named before InitMetrics runs
// rather than after.
//
// gfxFont::GetAdjustedSize fills mAdjustedSize lazily from mStyle.size, and
// InitMetrics asks for it before it loads a glyph, so by the time the metrics
// struct is being filled the field already holds the size the face will be set
// to. It is read here so that the advances InitMetrics caches for space, zero
// and the water ideograph are measured at the same size as everything after
// them; claiming afterwards leaves those three off by a rounding. Nothing is
// claimed until an earlier font has taught the two offsets.
// True once the size has been claimed, which takes a face this library knows
// and a scalable one.
bool PreClaimOwnSize(void* self)
{
    const size_t at = g_ftface_word.load(std::memory_order_relaxed);
    const size_t back = g_adjusted_size_delta.load(std::memory_order_relaxed);
    if (self == nullptr || at == 0 || back == 0 || back > at) {
        return false;
    }
    auto* const shared = static_cast<void* const*>(self)[at - 1];
    if (shared == nullptr) {
        return false;
    }
    double size = 0.0;
    std::memcpy(&size, static_cast<double*>(self) + (at - back), sizeof(size));
    if (!(size > 0.0) || !(size < 65536.0)) {
        return false;                        // still the -1.0 it starts at
    }
    for (size_t i = 0; i < 4; ++i) {
        if (CleartypeClaimSize(static_cast<void* const*>(shared)[i], size) != 0) {
            return true;
        }
    }
    return false;
}

// mAdjustedSize back to the -1.0 a gfxFont starts with, so GetAdjustedSize()
// works the size out from mStyle again.
//
// gfxFontconfigFontEntry::CreateFontInstance settles font-size-adjust before
// the font exists, from an aspect gfxFontconfigFontEntry::GetAspect measures on
// a separate 256px font, and gfxFontconfigFont's constructor stores the result
// as mAdjustedSize. gfxDWriteFont has no such step. Its font is born at
// mStyle.size and gfxDWriteFont::ComputeMetrics does the whole adjustment
// itself. The two agree on the size they finish at, since the block inside
// InitMetrics recomputes the adjustment from this font's own metrics and an
// aspect is a ratio, but they disagree on the size the font is carrying while
// that block runs. gfxFont::CreateVerticalMetrics takes emHeight from
// GetAdjustedSize() and is published once for the life of the font, and the
// ic-height basis is the one basis that asks for a vertical advance, so Windows
// keeps the vertical metrics of the unadjusted size where Linux keeps the
// adjusted one.
//
// Only for a font that has not been measured yet, which is the same condition
// InitMetrics puts on the adjustment block, and only for a scalable face, since
// ChooseFontSize departs from the style's own size for no other kind.
void ResetAdjustedSize(void* self)
{
    const size_t at = g_ftface_word.load(std::memory_order_relaxed);
    const size_t back = g_adjusted_size_delta.load(std::memory_order_relaxed);
    if (self == nullptr || at == 0 || back == 0 || back > at) {
        return;
    }
    double ft_size = 0.0;
    std::memcpy(&ft_size, WordAt(self, at + kFTSizeWord), sizeof(ft_size));
    if (!SameDouble(ft_size, 0.0)) {
        return;
    }
    constexpr double unset = -1.0;
    std::memcpy(static_cast<double*>(self) + (at - back), &unset, sizeof(unset));
}

// The mFTSize a font carried before the strike rewrite below replaced it.
//
// That rewrite puts the rounded strike size into both size fields, which is
// what leaves GetFTGlyphExtents scaling by one. It also drops the only value
// naming the Windows instance the font's glyphs were loaded through, and
// SubstituteInkBox needs that instance to recover the exact ink box. Keeping
// it here costs one slot per font and is read by address.
struct PriorFTSize
{
    void* self;
    double px;
};

PriorFTSize g_prior_ft_sizes[64] = {};
size_t g_prior_ft_next = 0;
pthread_mutex_t g_prior_ft_mutex = PTHREAD_MUTEX_INITIALIZER;

void RecordPriorFTSize(void* self, const double px)
{
    if (self == nullptr || !(px > 0.0) || !(px < 65536.0)) {
        return;
    }
    pthread_mutex_lock(&g_prior_ft_mutex);
    for (PriorFTSize& slot : g_prior_ft_sizes) {
        if (slot.self == self) {
            slot.px = px;
            pthread_mutex_unlock(&g_prior_ft_mutex);
            return;
        }
    }
    g_prior_ft_sizes[g_prior_ft_next] = PriorFTSize{.self = self, .px = px};
    g_prior_ft_next = g_prior_ft_next + 1;
    if (g_prior_ft_next >= sizeof(g_prior_ft_sizes) / sizeof(g_prior_ft_sizes[0])) {
        g_prior_ft_next = 0;
    }
    pthread_mutex_unlock(&g_prior_ft_mutex);
}

double PriorFTSizeFor(void* self)
{
    double px = 0.0;
    pthread_mutex_lock(&g_prior_ft_mutex);
    for (const PriorFTSize& slot : g_prior_ft_sizes) {
        if (slot.self == self) {
            px = slot.px;
            break;
        }
    }
    pthread_mutex_unlock(&g_prior_ft_mutex);
    return px;
}

extern "C" void DwcInitMetrics(void* self)
{
    if (PreClaimOwnSize(self)) {
        ResetAdjustedSize(self);
    }
    g_init_metrics(self);
    ClaimOwnFace(self);

    // Not about this font: the one place in the parent that is reached once
    // the font list exists. Settles after one success.
    MarkBadUnderlineFamilies();
    SetBlobTextGamma();

    if (self == nullptr) {
        return;
    }
    // The leadings answer for every face, the underline only for a family on
    // the bad-underline list. Either one carries the three fields that find
    // the struct, and they agree on them, so whichever answered will do.
    double em = 0.0, asc = 0.0, desc = 0.0;
    double il = 0.0, el = 0.0;
    double linux_el = 0.0;
    const bool leading = CleartypeWindowsLeading(&il, &el, &em, &asc, &desc, &linux_el) != 0;
    double uo = 0.0, us = 0.0, fold = 0.0;
    const bool underline = CleartypeWindowsUnderline(&uo, &us, &em, &asc, &desc, &fold) != 0;
    double so = 0.0, ss = 0.0;
    const bool strikeout = CleartypeWindowsStrikeout(&so, &ss, &em, &asc, &desc) != 0;
    double linux_upp = 0.0, win_upp = 0.0;
    const bool upp = CleartypeWindowsUnitsPerPixel(&linux_upp, &win_upp, &em, &asc, &desc) != 0;
    double ave = 0.0, adv = 0.0, cw_em = 0.0, cw_asc = 0.0, cw_desc = 0.0;
    const bool char_width = CleartypeWindowsCharWidth(&ave, &adv, &cw_em, &cw_asc, &cw_desc) != 0;
    double xh = 0.0, ch = 0.0, xh_em = 0.0, xh_asc = 0.0, xh_desc = 0.0;
    const bool xcap = CleartypeWindowsXCapHeight(&xh, &ch, &xh_em, &xh_asc, &xh_desc) != 0;
    double rounded = 0.0, unrounded = 0.0, sk_space = 0.0, sk_zero = 0.0, sk_ideo = 0.0;
    double sk_em = 0.0, sk_asc = 0.0, sk_desc = 0.0;
    const bool strike = CleartypeWindowsStrikeSize(&rounded, &unrounded, &sk_space, &sk_zero,
                                                   &sk_ideo, &sk_em, &sk_asc, &sk_desc) != 0;
    // A font smaller than a pixel, measured against the size it was actually
    // given rather than the whole pixel FreeType was clamped to.
    //
    // gfxFT2FontBase::FindClosestSize clamps mFTSize to 1.0 below a pixel and
    // lets glyph extents scale back from it, but InitMetrics takes the ascent
    // and descent from the clamped FreeType metrics with no such correction,
    // so every size under a pixel comes out with a whole pixel of ascent where
    // gfxDWriteFont::ComputeMetrics rounds to none. mAdjustedSize still holds
    // the real size, and the two offsets that reach it are learned from
    // ordinary fonts, where it and mFTSize agree.
    double tiny_asc = 0.0, tiny_desc = 0.0;
    bool tiny = false;
    if (const size_t at_w = g_ftface_word.load(std::memory_order_relaxed),
        back_w = g_adjusted_size_delta.load(std::memory_order_relaxed);
        at_w != 0 && back_w != 0 && back_w <= at_w) {
        double adjusted = 0.0;
        std::memcpy(&adjusted, static_cast<double*>(self) + (at_w - back_w), sizeof(adjusted));
        if (adjusted > 0.0 && adjusted < 1.0) {
            tiny = CleartypeWindowsMetricsAtSize(adjusted, &tiny_asc, &tiny_desc) != 0;
        }
    }
    // All three are read together and the claim is then dropped. The answers
    // belong to this call alone, and a later InitMetrics that returns before
    // reading OS/2 would otherwise be handed this face's numbers.
    CleartypeEndInitMetrics();
    if (!leading && !underline && !strikeout && !upp) {
        return;
    }

    double* found = FindMetrics(self, em, asc, desc);
    if (found == nullptr) {
        return;
    }
    if (g_ftface_word.load(std::memory_order_relaxed) == 0) {
        g_ftface_word.store(
            static_cast<size_t>(reinterpret_cast<void**>(found) -
                                static_cast<void**>(self)),
            std::memory_order_relaxed);
    }

    // The sub-pixel ascent and descent, put back over what the clamped
    // FreeType metrics produced. InitMetrics has already run, so its
    // sTypoAscender fallback, which fires when both are zero, has fired
    // against the clamped numbers and cannot undo these.
    if (tiny) {
        found[kMetricsMaxAscent] = tiny_asc;
        found[kMetricsMaxDescent] = tiny_desc;
        found[kMetricsMaxHeight] = tiny_asc + tiny_desc;
    }

    // Where mAdjustedSize sits, learned even when nothing below writes it, so
    // that PreClaimOwnSize can read it on the next font. mFTSize is the same
    // value for a scalable face, which is what the pattern needs to match on.
    if (const size_t word = g_ftface_word.load(std::memory_order_relaxed);
        word != 0 && g_adjusted_size_delta.load(std::memory_order_relaxed) == 0) {
        double ft_size = 0.0;
        std::memcpy(&ft_size, WordAt(self, word + kFTSizeWord), sizeof(ft_size));
        if (ft_size > 0.0 && ft_size < 65536.0) {
            (void)FindAdjustedSize(self, found, ft_size);
        }
    }

    // gfxDWriteFont::ComputeMetrics rounds mAdjustedSize onto the strike it is
    // about to draw from, and gfxFT2FontBase::InitMetrics has no such step, so
    // the size Firefox hands WebRender is the fractional one and WebRender
    // resamples the strike by req_size / y_ppem.
    if (strike && SameDouble(sk_em, em) && SameDouble(sk_asc, asc) &&
        SameDouble(sk_desc, desc)) {
        // Both sizes or neither. GetFTGlyphExtents scales every advance it
        // reads by GetAdjustedSize() / mFTSize, so writing both leaves that
        // scale at 1 and the advance arrives as Windows measured it. Writing
        // mAdjustedSize alone would leave a scale to divide out, and the only
        // state available for that is per face and size, which several fonts
        // share.
        const size_t at = g_ftface_word.load(std::memory_order_relaxed);
        double* const adjusted =
            at != 0 ? FindAdjustedSize(self, found, unrounded) : nullptr;
        if (adjusted != nullptr) {
            double prior = 0.0;
            std::memcpy(&prior, WordAt(self, at + kFTSizeWord), sizeof(prior));
            RecordPriorFTSize(self, prior);
            *adjusted = rounded;
            std::memcpy(WordAt(self, at + kFTSizeWord), &rounded, sizeof(rounded));
            // Windows measures these three through the rounded instance and
            // InitMetrics through the unrounded one, so they are replaced
            // outright.
            found[kMetricsSpaceWidth] = sk_space;
            found[kMetricsZeroWidth] = sk_zero;
            found[kMetricsIdeographicWidth] = sk_ideo;
        }
    }

    // The pair that sizes a text control and places nothing.
    if (char_width && SameDouble(cw_em, em) && SameDouble(cw_asc, asc) &&
        SameDouble(cw_desc, desc)) {
        found[kMetricsAveCharWidth] = ave;
        found[kMetricsMaxAdvance] = adv;
    }

    // gfxFont::SanitizeMetrics replaces the external leading outright when the
    // @font-face rule carries line-gap-override, and it changes nothing else.
    // An ascent or descent override moves maxAscent or maxDescent, so
    // FindMetrics already declines those; this one slips past it.
    //
    // linux_external is what InitMetrics derived before SanitizeMetrics ran, so
    // a field that no longer holds it is the author's override. Both platforms
    // apply that override alike, and replacing it here would undo it.
    //
    // The test allows half a pixel. The reconstruction rebuilds InitMetrics'
    // arithmetic from the size metrics and the OS/2 table and lands within a
    // rounding step of it, while an override is written as a percentage of the
    // font size and moves the leading by whole pixels. A tighter test would
    // read the rounding step as an override.
    const bool overridden =
        leading && std::fabs(found[kMetricsExternalLeading] - linux_el) > 0.5;

    if (xcap) {
        // The field InitMetrics derives from the FreeType face rather than from
        // the OS/2 table scaled. Everything that positions an inline box
        // against the parent's x-height reads it, vertical-align: middle among
        // them, so a fraction of a pixel here becomes a row of text elsewhere.
        //
        // capHeight sits beside it and is left alone. Writing the Windows one
        // as well moves two runs of the Wikipedia page at scroll 600, which the
        // Linux value renders identically, so only the field that was shown to
        // diverge is answered. ch carries it for whoever needs it next.
        // Only for the face this struct belongs to.
        //
        // FindMetrics locates the struct by the em, ascent and descent the
        // leading and underline answered with, and this export answers from
        // the face last measured on its own. The two are the same face for an
        // ordinary InitMetrics and part company when one returns early, so the
        // three are compared before anything is written; otherwise one face's
        // x-height lands in another's metrics, which is a length no page
        // agrees with on either platform.
        const bool same_face =
            std::fabs(xh_em - em) < 1e-9 && std::fabs(xh_asc - asc) < 1e-9 &&
            std::fabs(xh_desc - desc) < 1e-9;
        // Written in full, not to a threshold.
        //
        // Rounding the comparison to app units first looks safe, since layout
        // keeps lengths in them and two x-heights that round alike are the
        // same length to everything downstream. font-size-adjust is the
        // exception: it divides the specified size by the face's own
        // x-height ratio, so a difference too small to see in a length comes
        // back multiplied. Ahem at 40px with font-size-adjust 0.9 resolves a
        // device pixel apart from Windows when the fraction is dropped, and
        // exactly with it.
        if (same_face) {
            found[kMetricsXHeight] = xh;
        }
    }
    if (leading && !overridden) {
        // gfxFT2FontBase::InitMetrics writes floor(size + 0.5) into emHeight
        // where gfxDWriteFont::ComputeMetrics writes the unrounded
        // mAdjustedSize, so all three fields that make a line height are put
        // back as Windows holds them. The leadings then need no correction of
        // their own, and GetNormalLineHeight reaches the Windows answer from
        // the inputs Windows gives it.
        //
        // emHeight is not only a line-height term, which is why correcting the
        // leading against a rounded one was not equivalent.
        // gfxFcPlatformFontList reads it for the font-size-adjust ratios,
        // nsTextFrame for emphasis and decoration placement, and
        // gfxFont::CreateVerticalMetrics divides the horizontal internal
        // leading by it to synthesize the vertical one. That last one made
        // every fractional font size differ in a vertical writing mode, form
        // controls included, since their 13.3281px is never whole.
        found[kMetricsEmHeight] = em;
        found[kMetricsInternalLeading] = il;
        found[kMetricsExternalLeading] = el;
    }
    if (upp) {
        // COLRFonts scales a paint graph's font units by this, and
        // gfxFont::CreateVerticalMetrics multiplies the OS/2 and vhea fields by
        // it, so the rounding FreeType's scale carries reaches a color glyph's
        // gradients and a vertical line box alike.
        //
        // Written whenever the face answered, not only when the two factors
        // are a millionth apart. The field is a float and the fields it
        // multiplies are whole font units, so two factors that agree to that
        // tolerance still put a product either side of a rounding boundary: a
        // CJK face with vhea gives a vertical line box a device pixel out at
        // four sizes in seventeen, in both directions, which is the shape of a
        // rounding split rather than an offset.
        if (float* factor = FindUnitsPerPixel(self, found, linux_upp)) {
            *factor = static_cast<float>(win_upp);
        }
    }
    if (strikeout) {
        // gfxFT2FontBase::InitMetrics snaps the strikeout to whole pixels and
        // gfxDWriteFont::ComputeMetrics leaves it fractional, so the pair goes
        // back unsnapped. nsCSSRendering::GetTextDecorationRectInternal takes
        // both as they stand, the thickness as the line-through's height and
        // the offset as where its middle sits.
        found[kMetricsStrikeoutOffset] = so;
        found[kMetricsStrikeoutSize] = ss;
    }
    if (underline) {
        found[kMetricsUnderlineOffset] = uo;
        found[kMetricsUnderlineSize] = us;
        // ApplyWindowsMetrics moved half a pixel from the ascent to the descent
        // so that nsFontMetrics, folding the underline InitMetrics computed on
        // Linux, still reached the Windows MaxAscent and MaxDescent. The two
        // lines above just replaced that underline with the Windows one, which
        // folds to those values on its own, so the half pixel goes back. It is
        // visible on its own in canvas TextMetrics, whose fontBoundingBox
        // ascent and descent are these two fields unrounded.
        if (fold != 0.0) {
            found[kMetricsMaxAscent] = asc + fold;
            found[kMetricsMaxDescent] = desc - fold;
        }
    }
    if (leading && !overridden) {
        // gfxDWriteFonts.cpp: emAscent = emHeight * maxAscent / maxHeight, and
        // emDescent is the remainder. InitMetrics derived both from the
        // rounded emHeight, so they no longer sum to the one written above.
        // Canvas places textBaseline top, middle and bottom on this pair.
        const double height = found[kMetricsMaxAscent] + found[kMetricsMaxDescent];
        if (height > 0.0) {
            found[kMetricsEmAscent] = em * found[kMetricsMaxAscent] / height;
            found[kMetricsEmDescent] = em - found[kMetricsEmAscent];
        }
    }
}
void* (*g_malloc)(size_t) = nullptr;
void (*g_free)(void*) = nullptr;
int (*g_unichar_type)(uint32_t) = nullptr;

// xpcom/ds/nsTArray.h: nsTArrayHeader, and nsTArray_base's only member is the
// pointer to it. Elements follow the header; the allocator is plain malloc.
struct TArrayHeader
{
    uint32_t length;
    uint32_t capacity_and_auto;

    uint32_t Capacity() const { return capacity_and_auto & 0x7FFFFFFFu; }
    bool IsAuto() const { return (capacity_and_auto & 0x80000000u) != 0; }
    void Set(const uint32_t cap, const bool is_auto)
    {
        capacity_and_auto = (cap & 0x7FFFFFFFu) | (is_auto ? 0x80000000u : 0u);
    }
};

const char** ElementsOf(TArrayHeader* h)
{
    return reinterpret_cast<const char**>(h + 1);
}

// True if this header is one nsTArray would free: not the shared empty one
// (capacity 0), and not an AutoTArray's inline buffer.
//
// The pointer identifies it; the flag does not.
// nsTArray_base::UsesAutoArrayBuffer
// compares mHdr against GetAutoArrayHeader(), and EnsureCapacityImpl copies
// the old header wholesale when it moves an auto array to the heap - so
// mIsAutoArray stays set on a heap header, and means "this array has an inline
// buffer somewhere", not "this is it".
bool IsHeapHeader(const TArrayHeader* h, const void* array)
{
    if (h == nullptr || h->Capacity() == 0) {
        return false;
    }
    // xpcom/ds/nsTArray.h: kAutoTArrayHeaderOffset.
    const unsigned char* autobuf = static_cast<const unsigned char*>(array) + 8;
    return reinterpret_cast<const unsigned char*>(h) != autobuf;
}

bool Append(void* array, const char* name)
{
    auto** slot = static_cast<TArrayHeader**>(array);
    TArrayHeader* hdr = *slot;
    if (hdr == nullptr || hdr->length > hdr->Capacity()) {
        return false;                        // not a shape this understands
    }
    if (hdr->length == hdr->Capacity()) {
        const uint32_t want = hdr->Capacity() != 0 ? hdr->Capacity() * 2 : 8;
        auto* grown = static_cast<TArrayHeader*>(
            g_malloc(sizeof(TArrayHeader) + static_cast<size_t>(want) * sizeof(const char*)));
        if (grown == nullptr) {
            return false;
        }
        grown->length = hdr->length;
        // The flag travels with the header, exactly as
        // RelocateNonOverlappingRegionWithHeader would carry it: clearing it
        // makes GetAutoArrayHeader() return null, and nsTArray then loses the
        // way back to its own inline buffer.
        grown->Set(want, hdr->IsAuto());
        // The cast is explicit because
        // bugprone-multi-level-implicit-pointer-conversion asks for it, and
        // .clang-tidy is versioned with the repo.
        // ReSharper disable CppRedundantCastExpression
        std::memcpy(static_cast<void*>(ElementsOf(grown)),
                    static_cast<const void*>(ElementsOf(hdr)),
                    // ReSharper restore CppRedundantCastExpression
                    static_cast<size_t>(hdr->length) * sizeof(const char*));
        if (IsHeapHeader(hdr, array)) {
            g_free(hdr);
        }
        *slot = grown;
        hdr = grown;
    }
    ElementsOf(hdr)[hdr->length] = name;
    hdr->length += 1;
    return true;
}

// gfx/thebes/gfxPlatform.h: PrefersColor is aPresentation >= EmojiDefault, and
// FontPresentation counts Any, TextDefault, TextExplicit, EmojiDefault, ...
constexpr uint8_t kFontPresentationEmojiDefault = 3;

bool PrefersColor(const uint8_t presentation)
{
    return presentation >= kFontPresentationEmojiDefault;
}

// intl/unicharutil/nsUGenCategory.h: kPunctuation is Pc, Pd, Ps, Pe, Pi, Pf
// and Po; kSymbol is Sm, Sc, Sk and So. GLib's GUnicodeType lists the same
// categories, punctuation at 16..22 and symbols at 23..26, and GLib is in the
// process already because GTK is.
bool IsSymbolOrPunctuation(const uint32_t ch)
{
    const int type = g_unichar_type(ch);
    return type >= 16 && type <= 26;
}

// gfx/thebes/gfxWindowsPlatform.cpp: the condition on the Segoe UI block.
bool PunctOrCommon(const uint32_t ch, const int32_t script)
{
    const uint32_t b = ch >> 8;
    return script == firefox_parity::kScriptCommon ||
           (b >= 0x20 && b <= 0x2b) || b == 0x2e ||
           IsSymbolOrPunctuation(ch);
}

bool ConditionHolds(const int condition, const uint32_t ch, const int32_t script, const uint8_t presentation)
{
    switch (condition) {
    case firefox_parity::kFallbackAlways:
        return true;
    case firefox_parity::kFallbackColor:
        return PrefersColor(presentation);
    case firefox_parity::kFallbackNotColor:
        return !PrefersColor(presentation);
    case firefox_parity::kFallbackSupplementary:
        return ch > 0xFFFF;
    case firefox_parity::kFallbackPunctOrCommon:
        return PunctOrCommon(ch, script);
    default:
        return false;
    }
}

bool AppendRange(void* list, const unsigned first, const unsigned count, const uint32_t ch,
                 const int32_t script, const uint8_t presentation)
{
    for (unsigned i = first; i < first + count; ++i) {
        const firefox_parity::CommonFallbackFamily& f =
            firefox_parity::kWindowsCommonFallbackFamilies[i];
        if (!ConditionHolds(f.condition, ch, script, presentation)) {
            continue;
        }
        if (!Append(list, f.name)) {
            return false;
        }
    }
    return true;
}

// The step that comes after the common list on Windows:
// gfxPlatformFontList::CommonFontFallback tries the names above in order, and
// only if none of them has the character does GlobalFontFallback run, which on
// Windows is gfxDWriteFontList::PlatformGlobalFontFallback - a text layout
// drawn with DirectWrite's system fallback, whose answer is the first family
// in its table for this codepoint that is installed and has the glyph.
//
// Gecko applies exactly that test to each name in this list, in order, so
// appending DirectWrite's candidates after the common ones puts the same
// question in the same order: common list first, then what DirectWrite would
// have said, and after that the cmap scan both platforms end with.
//
// The locale never varies. gfxDWriteFontList::PlatformGlobalFontFallback
// creates its fallback text format with L"en-us" hardwired, so none of the six
// locale conditions in the table can apply and the default one always does.
void AppendSystemFallback(void* list, const uint32_t ch)
{
    namespace dw = dwc::dwrite_fallback;

    constexpr unsigned count = sizeof(dw::kRanges) / sizeof(dw::kRanges[0]);
    unsigned lo = 0, hi = count;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        if (ch > dw::kRanges[mid].last) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= count || ch < dw::kRanges[lo].first) {
        return;                              // outside every range: no mapping
    }

    constexpr unsigned conditions = sizeof(dw::kConditions) / sizeof(dw::kConditions[0]);
    for (unsigned c = dw::kRanges[lo].condition;
         c < conditions && dw::kConditions[c].range == lo; ++c) {
        if (dw::kConditions[c].locale >= 0) {
            continue;                        // a locale en-us is not
        }
        for (unsigned f = dw::kConditions[c].families; dw::kFamilyIndices[f] >= 0; ++f) {
            if (!Append(list, dw::kStrings[dw::kFamilyIndices[f]])) {
                return;
            }
        }
        return;
    }
}

// gfxWindowsPlatform::GetCommonFallbackFonts, with the switch expressed as the
// generated table. The Linux implementation this displaces is not called: on
// Windows this list is the whole answer, and a family that is not installed
// here is skipped by the same code that skips one missing on Windows.
// Likewise installed by address; see DwcInitMetrics above.
extern "C" void DwcGetCommonFallbackFonts(void* self, uint32_t ch, int32_t wide_script,
                                          uint8_t presentation, void* font_list);
extern "C" void DwcGetCommonFallbackFonts(void* self, const uint32_t ch, const int32_t wide_script,
                                          const uint8_t presentation, void* font_list)
{
    namespace fp = firefox_parity;

    // mozilla::intl::Script is `enum class Script : int16_t`, and the x86-64
    // psABI leaves the upper bits of a narrow argument unspecified, so the
    // parameter is taken wide and narrowed here. Reading the full register
    // would compare whatever the caller left above the low sixteen bits
    // against a table of shorts, and no rule would match.
    const int32_t script = static_cast<int16_t>(wide_script);

    // Once, on the first call. A patched slot that is never reached looks
    // exactly like a patch that did not take, and the difference is not
    // visible from outside.
    static bool reported = false;
    if (!reported) {
        reported = true;
        Report("libxul: GetCommonFallbackFonts reached (U+%04X, script %d, "
               "presentation %u)", ch, script, static_cast<unsigned>(presentation));
    }

    // A caller shape that is not understood is answered by the code that was
    // there before, so the worst a surprise costs is Linux fallback.
    auto* const* probe = static_cast<TArrayHeader* const*>(font_list);
    if (font_list == nullptr || *probe == nullptr ||
        (*probe)->length > (*probe)->Capacity()) {
        g_original(self, ch, script, presentation, font_list);
        return;
    }

    if (!AppendRange(font_list, fp::kWindowsCommonFallbackHeadFirst,
                     fp::kWindowsCommonFallbackHeadCount, ch, script, presentation)) {
        return;
    }

    for (unsigned r = 0; r < fp::kWindowsCommonFallbackRuleCount; ++r) {
        const fp::CommonFallbackRule& rule = fp::kWindowsCommonFallbackRules[r];
        bool matches = false;
        for (unsigned s = 0; s < rule.script_count && !matches; ++s) {
            matches = fp::kWindowsCommonFallbackScripts[rule.script_first + s] == script;
        }
        if (!matches) {
            continue;
        }
        if (!AppendRange(font_list, rule.family_first, rule.family_count, ch,
                         script, presentation)) {
            return;
        }
        break;                               // one case per script, as in the switch
    }

    if (!AppendRange(font_list, fp::kWindowsCommonFallbackTailFirst,
                     fp::kWindowsCommonFallbackTailCount, ch, script, presentation)) {
        return;
    }

    AppendSystemFallback(font_list, ch);
}

// ---------------------------------------------------------------------------
// Applying it.
// ---------------------------------------------------------------------------

// One attempt per process, and only one thread makes it: two dlopens racing
// could otherwise have the second read the slot after the first wrote it and
// take this file's own function as the original, which is a loop.
std::atomic g_done{false};

// The allocator has to be libxul's own, and dlsym cannot find it: Firefox
// links mozjemalloc into libxul, which replaces malloc for everything inside
// it while exporting nothing, so dlsym answers with glibc's. Memory from the
// wrong allocator is handed straight to nsTArray, which reallocs and frees it
// with libxul's - and that does not survive contact.
//
// So take it from where libxul's own calls take it: the GOT slot its dynamic
// relocation for "malloc" points at, read after the loader has bound it. That
// is the same function libxul calls, whichever build this is.
// The GOT slot libxul's own calls to an imported function go through: the
// True when [ptr, ptr+len) lies inside one of the image's read-only regions.
bool InRodata(const Image& image, const void* ptr, const size_t len)
{
    const auto* p = static_cast<const unsigned char*>(ptr);
    for (unsigned i = 0; i < image.rodata_count; ++i) {
        if (p >= image.rodata[i].begin && p + len <= image.rodata[i].end) {
            return true;
        }
    }
    return false;
}

// The GOT slot holding `want`, or null. SymbolFromGot returns what is in it.
//
// DT_STRTAB and DT_SYMTAB hold a relocated address on a build whose .dynamic
// sits in a writable segment and the link-time vaddr otherwise, so both
// readings are tried and each has to land inside the image's read-only data
// before it is used.
void** GotSlot(const Image& image, const char* want)
{
    const ElfW(Dyn)* dyn = nullptr;
    for (ElfW(Half) i = 0; i < image.phnum; ++i) {
        if (image.phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const ElfW(Dyn)*>(image.base + image.phdr[i].p_vaddr);
        }
    }
    if (dyn == nullptr) {
        return nullptr;
    }

    ElfW(Addr) strtab_addr = 0;
    ElfW(Addr) symtab_addr = 0;
    const ElfW(Rela)* rela[2] = {nullptr, nullptr};
    size_t relasz[2] = {0, 0};
    for (; dyn->d_tag != DT_NULL; ++dyn) {
        switch (dyn->d_tag) {
        case DT_STRTAB: strtab_addr = dyn->d_un.d_ptr; break;
        case DT_SYMTAB: symtab_addr = dyn->d_un.d_ptr; break;
        case DT_RELA:   rela[0] = reinterpret_cast<const ElfW(Rela)*>(dyn->d_un.d_ptr); break;
        case DT_RELASZ: relasz[0] = dyn->d_un.d_val; break;
        case DT_JMPREL: rela[1] = reinterpret_cast<const ElfW(Rela)*>(dyn->d_un.d_ptr); break;
        case DT_PLTRELSZ: relasz[1] = dyn->d_un.d_val; break;
        default: break;
        }
    }

    auto resolve = [&](ElfW(Addr) const addr, const size_t len) -> const void* {
        const auto* direct = reinterpret_cast<const void*>(addr);
        if (InRodata(image, direct, len)) {
            return direct;
        }
        const auto* based = reinterpret_cast<const void*>(image.base + addr);
        return InRodata(image, based, len) ? based : nullptr;
    };

    const auto* strtab = static_cast<const char*>(resolve(strtab_addr, 1));
    const auto* symtab = static_cast<const ElfW(Sym)*>(resolve(symtab_addr, sizeof(ElfW(Sym))));
    if (strtab == nullptr || symtab == nullptr) {
        return nullptr;
    }

    for (int which = 0; which < 2; ++which) {
        if (rela[which] == nullptr) {
            continue;
        }
        const size_t count = relasz[which] / sizeof(ElfW(Rela));
        for (size_t i = 0; i < count; ++i) {
            const ElfW(Rela)& r = rela[which][i];
            const uint32_t type = ELF64_R_TYPE(r.r_info);
            if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT) {
                continue;
            }
            const ElfW(Sym)* sym = symtab + ELF64_R_SYM(r.r_info);
            if (!InRodata(image, sym, sizeof(*sym)) ||
                !InRodata(image, strtab + sym->st_name, 1) ||
                std::strcmp(strtab + sym->st_name, want) != 0) {
                continue;
            }
            return reinterpret_cast<void**>(image.base + r.r_offset);
        }
    }
    return nullptr;
}

void* SymbolFromGot(const Image& image, const char* want)
{
    void** const slot = GotSlot(image, want);
    return slot != nullptr ? *slot : nullptr;   // bound by the loader already
}

// Every function that calls an imported function through its GOT slot,
// `call *offset(%rip)`.
// False when more functions matched than `out` holds. The caller intersects
// two of these lists and treats a lone survivor as the unique answer, so a
// truncated list would make an ambiguous case look decided.
bool FunctionsCalling(const Image& image, const FunctionStarts& starts, void* const* slot,
                      uintptr_t* out, unsigned* count, const unsigned max)
{
    *count = 0;
    if (slot == nullptr) {
        return false;
    }
    const uintptr_t want = reinterpret_cast<uintptr_t>(slot);
    for (unsigned i = 0; i < image.text_count; ++i) {
        const Region& r = image.text[i];
        for (const unsigned char* p = r.begin; p + 6 <= r.end; ++p) {
            if (p[0] != 0xFF || p[1] != 0x15) {
                continue;
            }
            int32_t disp;
            std::memcpy(&disp, p + 2, sizeof(disp));
            if (reinterpret_cast<uintptr_t>(p) + 6 +
                    static_cast<uintptr_t>(static_cast<intptr_t>(disp)) != want) {
                continue;
            }
            const uintptr_t fn = starts.Enclosing(reinterpret_cast<uintptr_t>(p));
            bool seen = false;
            for (unsigned k = 0; k < *count; ++k) {
                seen = seen || out[k] == fn;
            }
            if (!seen && fn != 0) {
                if (*count == max) {
                    return false;            // more than this can account for
                }
                out[(*count)++] = fn;
            }
        }
    }
    return true;
}

// Rewrite the displacement of every direct call to `from` so it lands on `to`.
// The only write this file makes into executable memory.
//
// This runs whenever libxul arrives, which through the interposed dlopen is
// after its initializers have run and may have started threads. Refusing then
// would be the safer-looking choice and the wrong one: it would drop the
// metrics correction on every Firefox that loads libxul the usual way, and
// parity is what this library exists for. CLEARTYPE=0 is where a caller says
// otherwise. The displacement write is made atomic where alignment allows,
// which is what the thread case actually needs.
//
// The candidates come from a byte scan for E8 followed by a displacement that
// decodes to `from`, and a byte scan cannot tell an instruction from the
// middle of one. Proving an address is an instruction boundary would need a
// length decoder run from the enclosing function's start, which is more
// machinery, and more ways to be wrong, than the hazard it removes. So the
// failure is bounded:
//
//   * the sites are collected and checked before anything is written, and one
//     bad candidate abandons the whole patch, so libxul is never left with
//     some call sites redirected and others not;
//   * a candidate whose five bytes straddle two functions is rejected, since
//     no real call does that;
//   * an implausible number of sites is rejected, since a private method has a
//     handful of direct callers;
//   * the displacement goes down in a single atomic store wherever it fits in
//     one aligned eight-byte word, so a thread executing that call cannot see
//     half of it.
//
// A fixed byte pattern inside another instruction can still decode to `from`,
// and a build where that happens writes four bytes into an operand it should
// not.
constexpr unsigned kMaxCallSites = 32;

// Writes the displacement, and answers whether one store covered it.
bool WriteDisplacement(unsigned char* site, const int32_t rel)
{
    const uintptr_t at = reinterpret_cast<uintptr_t>(site);
    const uintptr_t word = at & ~static_cast<uintptr_t>(7);
    const size_t offset = at - word;
    if (offset + sizeof(rel) <= sizeof(uint64_t)) {
        // The bytes around the displacement are rewritten to what they already
        // hold, so one aligned store covers the change.
        uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(word), sizeof(value));
        std::memcpy(reinterpret_cast<unsigned char*>(&value) + offset, &rel, sizeof(rel));
        __atomic_store_n(reinterpret_cast<uint64_t*>(word), value, __ATOMIC_RELAXED);
        return true;
    }
    // The four bytes span two aligned words, so no single store covers them.
    std::memcpy(site, &rel, sizeof(rel));
    return false;
}

unsigned RedirectCalls(const Image& image, const FunctionStarts& starts, const uintptr_t from,
                       const void* to)
{
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return 0;
    }

    // First pass: collect and check. Nothing is written here.
    unsigned char* sites[kMaxCallSites];
    unsigned count = 0;
    for (unsigned i = 0; i < image.text_count; ++i) {
        const Region& r = image.text[i];
        for (const unsigned char* p = r.begin; p + 5 <= r.end; ++p) {
            if (p[0] != 0xE8) {
                continue;
            }
            int32_t disp;
            std::memcpy(&disp, p + 1, sizeof(disp));
            const uintptr_t at = reinterpret_cast<uintptr_t>(p);
            if (at + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(disp)) != from) {
                continue;
            }
            if (count == kMaxCallSites) {
                Report("libxul: more than %u apparent calls to %#lx, so the scan is not "
                       "finding call sites; leaving them alone", kMaxCallSites, from);
                return 0;
            }
            // A real call instruction lies inside one function.
            const uintptr_t owner = starts.Enclosing(at);
            if (owner == 0 || owner != starts.Enclosing(at + 4)) {
                Report("libxul: an apparent call to %#lx at %#lx is not inside one function; "
                       "leaving them alone", from, at);
                return 0;
            }
            const intptr_t want = reinterpret_cast<intptr_t>(to) - static_cast<intptr_t>(at + 5);
            if (want < INT32_MIN || want > INT32_MAX) {
                Report("libxul: %p is out of reach of a call at %#lx, so the rest are left "
                       "alone too", to, at);
                return 0;
            }
            sites[count++] = const_cast<unsigned char*>(p) + 1;
        }
    }
    if (count == 0) {
        return 0;
    }

    // Every page is opened before anything is written, so a page that will not
    // open leaves the patch unapplied instead of half applied.
    for (unsigned i = 0; i < count; ++i) {
        const uintptr_t first = reinterpret_cast<uintptr_t>(sites[i]) & ~static_cast<uintptr_t>(page - 1);
        // ReSharper disable once CppRedundantParentheses
        const uintptr_t last = (reinterpret_cast<uintptr_t>(sites[i]) + 3) &
                               ~static_cast<uintptr_t>(page - 1);
        const size_t len = last - first + static_cast<size_t>(page);
        // Writable and executable at once. This patches live text in a
        // running process, where other threads are running by now, and
        // dropping PROT_EXEC for the duration would fault any of them that
        // entered the page while it was non-executable.
        // NOLINTNEXTLINE(clang-analyzer-security.MmapWriteExec)
        if (mprotect(reinterpret_cast<void*>(first), len,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            Report("libxul: a text page would not open for writing, so InitMetrics is left "
                   "alone entirely");
            for (unsigned k = 0; k < i; ++k) {
                const uintptr_t done = reinterpret_cast<uintptr_t>(sites[k]) &
                                       ~static_cast<uintptr_t>(page - 1);
                mprotect(reinterpret_cast<void*>(done), static_cast<size_t>(page),
                         PROT_READ | PROT_EXEC);
            }
            return 0;
        }
    }

    unsigned torn_risk = 0;
    for (unsigned i = 0; i < count; ++i) {
        const intptr_t want = reinterpret_cast<intptr_t>(to) -
                              static_cast<intptr_t>(reinterpret_cast<uintptr_t>(sites[i]) + 4);
        if (!WriteDisplacement(sites[i], static_cast<int32_t>(want))) {
            ++torn_risk;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        const uintptr_t first = reinterpret_cast<uintptr_t>(sites[i]) & ~static_cast<uintptr_t>(page - 1);
        // ReSharper disable once CppRedundantParentheses
        const uintptr_t last = (reinterpret_cast<uintptr_t>(sites[i]) + 3) &
                               ~static_cast<uintptr_t>(page - 1);
        const size_t len = last - first + static_cast<size_t>(page);
        mprotect(reinterpret_cast<void*>(first), len, PROT_READ | PROT_EXEC);
    }
    if (torn_risk != 0) {
        Report("libxul: %u of %u call sites were written without a single store", torn_risk, count);
    }
    return count;
}

bool ResolveHelpers(const Image& image)
{
    g_malloc = reinterpret_cast<void* (*)(size_t)>(SymbolFromGot(image, "malloc"));
    g_free = reinterpret_cast<void (*)(void*)>(SymbolFromGot(image, "free"));
    if (g_malloc == nullptr || g_free == nullptr) {
        Report("libxul: no malloc/free in its own relocations, so nothing can "
               "be handed to nsTArray safely; leaving libxul alone");
        return false;
    }
    g_unichar_type = reinterpret_cast<int (*)(uint32_t)>(dlsym(RTLD_DEFAULT, "g_unichar_type"));
    if (g_unichar_type == nullptr) {
        // Without it the tail's condition cannot be evaluated, and a partial
        // answer is not the Windows answer.
        Report("libxul: no g_unichar_type in this process, so the fallback "
               "tail cannot be decided; leaving libxul alone");
        return false;
    }
    return g_malloc != nullptr && g_free != nullptr;
}

// gfxFT2FontBase::InitMetrics -> the same call, with the underline of a
// bad-underline family corrected afterwards.
bool PatchUnderline(const Image& image, const FunctionStarts& starts)
{
    void** sfnt = GotSlot(image, "FT_Get_Sfnt_Table");
    void** char_size = GotSlot(image, "FT_Set_Char_Size");
    if (sfnt == nullptr || char_size == nullptr) {
        Report("libxul: no FreeType relocations, so InitMetrics cannot be found");
        return false;
    }

    // InitMetrics reads head, OS/2 and post, and it is the only function that
    // does that and sets a character size: the Skia ports read the same tables
    // but never size a face.
    uintptr_t readers[16];
    uintptr_t sizers[64];
    unsigned n_readers = 0, n_sizers = 0;
    if (!FunctionsCalling(image, starts, sfnt, readers, &n_readers, 16) ||
        !FunctionsCalling(image, starts, char_size, sizers, &n_sizers, 64)) {
        Report("libxul: more functions read the sfnt tables or size a face than can be "
               "accounted for, so InitMetrics cannot be identified");
        return false;
    }

    uintptr_t found = 0;
    for (unsigned i = 0; i < n_readers; ++i) {
        for (unsigned k = 0; k < n_sizers; ++k) {
            if (readers[i] != sizers[k]) {
                continue;
            }
            if (found != 0 && found != readers[i]) {
                Report("libxul: more than one function reads the sfnt tables and "
                       "sizes a face; leaving InitMetrics alone");
                return false;
            }
            found = readers[i];
        }
    }
    if (found == 0) {
        Report("libxul: no function both reads the sfnt tables and sizes a face");
        return false;
    }

    g_init_metrics = reinterpret_cast<InitMetricsFn>(found);
    const unsigned patched =
        RedirectCalls(image, starts, found, reinterpret_cast<void*>(&DwcInitMetrics));
    if (patched == 0) {
        g_init_metrics = nullptr;
        Report("libxul: InitMetrics at %#lx is not called directly anywhere", found);
        return false;
    }
    Report("libxul: InitMetrics %#lx now returns through this library (%u call site%s)",
           found, patched, patched == 1 ? "" : "s");
    return true;
}

// ---------------------------------------------------------------------------
// App units per device pixel.
//
// gfxFont::PostShapingFixup is where Windows widens a synthesized bold face,
// through gfxShapedText::ApplyTrackingToClusters, which adds a whole number of
// app units. The shim reproduces that widening inside the advance, because
// nothing below the DOM applies it on this platform: mApplySyntheticBold is
// set in gfxDWriteFonts.cpp and gfxMacFont.cpp and nowhere else, so the
// FreeType path leaves it false and PostShapingFixup returns without doing
// anything. Reproducing it needs the size of an app unit, and that is 60 to a
// CSS pixel but 60/ratio to a device pixel, so a scaled display counts them
// differently and the reproduction lands a unit out at every ratio above one.
//
// The number lives in gfxShapedText, which PostShapingFixup is handed. This
// hook is only there to read it: it takes no decision, and the call goes
// through to the original whatever it finds.
//
// The number is read from the copy gfxFont::ShapeText inlines the fixup into,
// which is what runs here; the out-of-line copy belongs to gfxFT2Font and is
// only located on the way to it.
// ---------------------------------------------------------------------------

// The function the shaping path reaches with the fixup inlined into it.
//
// Its name is not needed and its parameter list is not guessed: both call
// sites set up six registers and push two more, and the last of those is a
// zero-extended uint16. Eight integer arguments and no floating point ones, so
// they forward exactly whatever they mean.
using InlinedFixupFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
InlinedFixupFn g_inlined_fixup = nullptr;

std::atomic<bool> g_app_units_known{false};
// The call sites standing in front of the fixup, kept so they can be put
// back. Reading the count of app units needs the hook to run once; leaving it
// in front of a function entered on every shaped run costs about twenty times
// the shaping path, which is not a price the answer is worth. So the hook
// removes itself as soon as it has read the number.
unsigned char* g_fixup_sites[kMaxCallSites];
unsigned g_fixup_sites_n = 0;
uintptr_t g_fixup_original = 0;

// gfx/thebes/gfxFont.h: gfxShapedText is polymorphic, then holds
// mDetailedGlyphs, mLength, mFlags and mAppUnitsPerDevUnit in that order.
constexpr size_t kShapedTextLength = 16;
constexpr size_t kShapedTextAppUnits = 22;
constexpr size_t kShapedTextProbe = 24;

// Whether this pointer is a gfxShapedText, checked before it is believed.
//
// The compiler clones PostShapingFixup and drops the arguments it can see are
// unused, so which register carries the shaped text is a property of the build
// and not of the signature. Every candidate is therefore offered here and the
// one that answers is the one used; a register holding something else fails on
// its vtable, its length or its app units, and a register holding nothing at
// all fails the read.
bool ShapedTextAppUnits(const void* p, uint16_t* units)
{
    if (p == nullptr || (reinterpret_cast<uintptr_t>(p) & 7U) != 0 ||
        !g_libxul_known.load(std::memory_order_acquire)) {
        return false;
    }
    unsigned char buf[kShapedTextProbe];
    if (!ReadWithoutFaulting(p, buf, sizeof(buf))) {
        return false;
    }
    const void* vptr = nullptr;
    std::memcpy(&vptr, buf, sizeof(vptr));
    const auto* v = static_cast<const unsigned char*>(vptr);
    if (v == nullptr || v < g_libxul.relro.begin || v >= g_libxul.relro.end) {
        return false;                        // not a vtable in this library
    }
    // A pointer into the relocated data is not yet a vtable. The first slot of
    // one holds a function, so it has to land in the library's own text; an
    // ordinary struct whose first word happens to point into that segment
    // fails here.
    const void* first = nullptr;
    if (!ReadWithoutFaulting(v, &first, sizeof(first))) {
        return false;
    }
    bool in_text = false;
    for (unsigned i = 0; i < g_libxul.text_count; ++i) {
        const Region& t = g_libxul.text[i];
        const auto* f = static_cast<const unsigned char*>(first);
        in_text = in_text || (f >= t.begin && f < t.end);
    }
    if (!in_text) {
        return false;
    }
    uint32_t length = 0;
    std::memcpy(&length, buf + kShapedTextLength, sizeof(length));
    uint16_t got = 0;
    std::memcpy(&got, buf + kShapedTextAppUnits, sizeof(got));
    // A text run is not billions of characters long, and no display counts
    // more app units to a device pixel than it does to a CSS one.
    if (length > (1U << 24) || got == 0 || got > 60) {
        return false;
    }
    *units = got;
    return true;
}


// The reading last handed to the shim; a rescale is a change in this.
std::atomic<uint16_t> g_app_units_seen{0};

extern "C" uint64_t DwcInlinedFixup(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                    uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8);
extern "C" uint64_t DwcInlinedFixup(const uint64_t a1, const uint64_t a2, const uint64_t a3,
                                    const uint64_t a4, const uint64_t a5, const uint64_t a6,
                                    const uint64_t a7, const uint64_t a8)
{
    // The eighth argument is the only one read. A count of app units to a
    // device pixel is 60 at a ratio of one and smaller above it, and never
    // zero, so a value outside that is some other field and is left alone.
    // One of the pointers is the shaped text the inlined fixup works on. Which
    // one is a property of the build, so each is offered to the same check the
    // shaped text has to pass, and a value is taken only from one that does.
    // Probing costs a syscall per candidate, and this sits on the shaping
    // path, so it is done once and then only occasionally.
    //
    // The first pass finds which argument carries the shaped text; after that
    // only that one is looked at, and only every so often, which is what keeps
    // a display rescaled under a running browser from going unnoticed without
    // charging every run for the watch. A browser slowed down here does not
    // render differently, but it does miss the capture handshake's deadlines,
    // and those frames read as differences that are not there.
    {
        const uint64_t args[] = { a1, a2, a3, a4, a5, a6, a7, a8 };
        // Which argument carried the shaped text, once it is known, and how
        // many calls have gone by since it was last read. The count is what
        // the watch costs on the calls in between.
        static std::atomic<int> carrier{-1};
        static std::atomic<unsigned> since{0};
        constexpr unsigned kCallsBetweenReads = 256;

        const int known = carrier.load(std::memory_order_relaxed);
        if (known >= 0) {
            if (since.fetch_add(1, std::memory_order_relaxed) + 1 >= kCallsBetweenReads) {
                since.store(0, std::memory_order_relaxed);
                const uint64_t v = args[known];
                uint16_t units = 0;
                if (v >= 0x10000 && v < 0x800000000000ULL && (v & 7U) == 0 &&
                    ShapedTextAppUnits(reinterpret_cast<const void*>(v), &units) &&
                    units != g_app_units_seen) {
                    g_app_units_seen = units;
                    CleartypeSetAppUnitsPerDevPixel(static_cast<int>(units));
                    Report("libxul: app units per device pixel is now %u", units);
                }
            }
        } else {
            for (size_t i = 0; i < 8; ++i) {
                const uint64_t v = args[i];
                // A syscall is what a probe costs, so an argument that cannot
                // be a pointer is dismissed without one.
                if (v < 0x10000 || v >= 0x800000000000ULL || (v & 7U) != 0) {
                    continue;
                }
                uint16_t units = 0;
                if (!ShapedTextAppUnits(reinterpret_cast<const void*>(v), &units)) {
                    continue;
                }
                g_app_units_seen = units;
                CleartypeSetAppUnitsPerDevPixel(static_cast<int>(units));
                Report("libxul: app units per device pixel is %u", units);
                carrier.store(static_cast<int>(i), std::memory_order_relaxed);
                break;
            }
        }
    }
    return g_inlined_fixup(a1, a2, a3, a4, a5, a6, a7, a8);
}

// Every rip-relative reference to one address, by the function holding it.
unsigned FunctionsReferencing(const Image& image, const FunctionStarts& starts,
                              const uintptr_t what, uintptr_t* out, const unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < image.text_count; ++i) {
        const Region& r = image.text[i];
        for (const unsigned char* p = r.begin; p + 4 <= r.end; ++p) {
            int32_t disp = 0;
            std::memcpy(&disp, p, sizeof(disp));
            // The displacement of these SSE loads is the last field of the
            // instruction, so the next instruction begins right after it.
            if (reinterpret_cast<uintptr_t>(p) + 4 +
                    static_cast<uintptr_t>(static_cast<intptr_t>(disp)) != what) {
                continue;
            }
            const uintptr_t fn = starts.Enclosing(reinterpret_cast<uintptr_t>(p));
            bool seen = false;
            for (unsigned k = 0; k < n; ++k) {
                seen = seen || out[k] == fn;
            }
            if (!seen && fn != 0) {
                if (n == max) {
                    return max + 1;          // more than can be accounted for
                }
                out[n++] = fn;
            }
        }
    }
    return n;
}

// The functions holding a direct call to `to`.
unsigned FunctionsDirectlyCalling(const Image& image, const FunctionStarts& starts,
                                  const uintptr_t to, uintptr_t* out, const unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < image.text_count; ++i) {
        const Region& r = image.text[i];
        for (const unsigned char* p = r.begin; p + 5 <= r.end; ++p) {
            if (p[0] != 0xE8) {
                continue;
            }
            int32_t disp = 0;
            std::memcpy(&disp, p + 1, sizeof(disp));
            if (reinterpret_cast<uintptr_t>(p) + 5 +
                    static_cast<uintptr_t>(static_cast<intptr_t>(disp)) != to) {
                continue;
            }
            const uintptr_t fn = starts.Enclosing(reinterpret_cast<uintptr_t>(p));
            bool seen = false;
            for (unsigned k = 0; k < n; ++k) {
                seen = seen || out[k] == fn;
            }
            if (!seen && fn != 0) {
                if (n == max) {
                    return max + 1;
                }
                out[n++] = fn;
            }
        }
    }
    return n;
}

// PostShapingFixup, found through the one constant its arithmetic needs.
//
// gfxFont::GetSyntheticBoldOffset divides by a threshold of 48, and that
// double occurs once in the whole library. The function holding it is small
// and returns the offset; the small function that calls it is the fixup. Both
// steps insist on exactly one candidate, so a build that inlines them
// differently is left alone rather than guessed at.
constexpr size_t kSmallFunction = 512;

bool PatchPostShapingFixup(const Image& image, const FunctionStarts& starts)
{
    constexpr double kThreshold = 48.0;
    // Every aligned copy of the threshold. The mapped image holds more than
    // the one the file does, so the constant alone does not name the function;
    // it only narrows where to look, and the chain below is what decides.
    const unsigned char* where[8];
    unsigned n_where = 0;
    for (unsigned i = 0; i < image.rodata_count && n_where < 8; ++i) {
        const Region& r = image.rodata[i];
        for (const unsigned char* p = r.begin; p + sizeof(double) <= r.end; ++p) {
            if ((reinterpret_cast<uintptr_t>(p) & 7U) != 0) {
                continue;                    // a double the compiler emitted is aligned
            }
            double v = 0.0;
            std::memcpy(&v, p, sizeof(v));
            if (!SameDouble(v, kThreshold)) {
                continue;
            }
            bool seen = false;
            for (unsigned k = 0; k < n_where; ++k) {
                seen = seen || where[k] == p;
            }
            if (!seen) {
                if (n_where == 8) {
                    Report("libxul: too many copies of the synthetic bold threshold; "
                           "app units per device pixel stay at 60");
                    return false;
                }
                where[n_where++] = p;
            }
        }
    }
    if (n_where == 0) {
        Report("libxul: no synthetic bold threshold; app units per device pixel stay at 60");
        return false;
    }

    // The offset function is small, returns the offset and is referenced by one
    // of those copies. Exactly one such function must exist across them all.
    uintptr_t offset_fn = 0;
    for (unsigned w = 0; w < n_where; ++w) {
        uintptr_t holders[8];
        const unsigned n_hold = FunctionsReferencing(image, starts,
                                                     reinterpret_cast<uintptr_t>(where[w]),
                                                     holders, 8);
        for (unsigned i = 0; i < n_hold && i < 8; ++i) {
            const size_t extent = starts.Extent(holders[i]);
            if (extent == 0 || extent > kSmallFunction) {
                continue;
            }
            if (offset_fn != 0 && offset_fn != holders[i]) {
                Report("libxul: more than one small function computes the synthetic "
                       "bold offset; app units per device pixel stay at 60");
                return false;
            }
            offset_fn = holders[i];
        }
    }
    // The large function holding the inlined fixup, which is the copy that
    // runs here. Patched through its callers, whose argument setup is what
    // says how many there are.
    for (unsigned w = 0; w < n_where; ++w) {
        uintptr_t holders[8];
        const unsigned n_hold = FunctionsReferencing(image, starts,
                                                     reinterpret_cast<uintptr_t>(where[w]),
                                                     holders, 8);
        for (unsigned i = 0; i < n_hold && i < 8; ++i) {
            if (starts.Extent(holders[i]) <= kSmallFunction) {
                continue;
            }
            // This is the copy that runs, and the shaped text among its
            // arguments carries the count. It is entered once per shaped run,
            // so the hook reads the number and then puts these call sites
            // back; the sites are remembered here for that.
            if (EnvDisables("CLEARTYPE_APP_UNITS")) {
                break;                       // for pricing the hook against no hook
            }
            g_inlined_fixup = reinterpret_cast<InlinedFixupFn>(holders[i]);
            const unsigned n = RedirectCalls(image, starts, holders[i],
                                             reinterpret_cast<void*>(&DwcInlinedFixup));
            if (n == 0) {
                g_inlined_fixup = nullptr;
                break;
            }
            g_fixup_original = holders[i];
            g_fixup_sites_n = 0;
            for (unsigned t = 0; t < image.text_count; ++t) {
                const Region& r = image.text[t];
                for (const unsigned char* q = r.begin; q + 5 <= r.end; ++q) {
                    if (q[0] != 0xE8) {
                        continue;
                    }
                    int32_t disp = 0;
                    std::memcpy(&disp, q + 1, sizeof(disp));
                    const uintptr_t at = reinterpret_cast<uintptr_t>(q);
                    if (at + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(disp)) !=
                        reinterpret_cast<uintptr_t>(&DwcInlinedFixup)) {
                        continue;
                    }
                    if (g_fixup_sites_n < kMaxCallSites) {
                        g_fixup_sites[g_fixup_sites_n++] = const_cast<unsigned char*>(q) + 1;
                    }
                }
            }
            Report("libxul: the inlined fixup at %#lx returns through this library "
                   "until it has answered (%u call site%s, %u remembered)",
                   holders[i], n, n == 1 ? "" : "s", g_fixup_sites_n);
            break;
        }
        if (g_inlined_fixup != nullptr) {
            break;
        }
    }

    if (offset_fn == 0) {
        Report("libxul: the synthetic bold offset is inlined everywhere; app units "
               "per device pixel stay at 60");
        return false;
    }

    uintptr_t callers[16];
    const unsigned n_call = FunctionsDirectlyCalling(image, starts, offset_fn, callers, 16);
    uintptr_t fixup = 0;
    for (unsigned i = 0; i < n_call && i < 16; ++i) {
        if (starts.Extent(callers[i]) <= kSmallFunction) {
            if (fixup != 0) {
                Report("libxul: more than one small function calls the synthetic bold "
                       "offset; app units per device pixel stay at 60");
                return false;
            }
            fixup = callers[i];
        }
    }
    if (fixup == 0) {
        Report("libxul: no small caller of the synthetic bold offset; app units per "
               "device pixel stay at 60");
        return false;
    }



    // Located, not redirected. gfxFcFont is what shapes on this desktop and
    // gfxFont::ShapeText inlines the fixup into itself, so this out-of-line
    // copy is the one gfxFT2Font calls and nothing here does. Standing in
    // front of it would write into text that never executes.
    (void)fixup;
    return true;
}

// ---------------------------------------------------------------------------
// The glyph path.
//
// SkScalerContext_FreeType::generatePath builds the SkPath that everything
// drawn from an outline goes through: a stroked glyph, a COLR layer under a
// gradient, text past the size Skia keeps in its atlas. SkScalerContext_DW
// builds the same path from the outline DirectWrite returns, so replacing this
// walk is what puts the two sides on one set of curves; see
// FT_Outline_Decompose in freetype.cpp for the substitution itself and for why
// the coordinates cannot come through it.
//
// Nothing here names the function. It is found by shape, as the one virtual
// function that walks an outline, sits in a vtable beside two others that
// load a glyph the same way, and touches nothing a rasterizer would.
// ---------------------------------------------------------------------------

// std::optional<GeneratedPath> generatePath(const SkGlyph&) returns a type
// with a destructor, so it comes back through a hidden pointer.
using GeneratePathFn = void* (*)(void* sret, void* self, const void* glyph);
GeneratePathFn g_generate_path = nullptr;

// A handful either way, since the vtable holds one walker and only a few
// functions share a loader.
constexpr unsigned kMaxGlyphFns = 12;

struct DirectCallers
{
    uintptr_t target = 0;
    uintptr_t callers[kMaxGlyphFns] = {};
    unsigned count = 0;
    bool overflowed = false;
};

// Every function making a direct call to any of `n` targets, in one pass over
// the text. Separate passes would each walk the whole section again.
void CollectDirectCallers(const Image& image, const FunctionStarts& starts,
                          DirectCallers* set, const unsigned n)
{
    for (unsigned i = 0; i < image.text_count; ++i) {
        const Region& r = image.text[i];
        for (const unsigned char* p = r.begin; p + 5 <= r.end; ++p) {
            if (p[0] != 0xE8) {
                continue;
            }
            int32_t disp;
            std::memcpy(&disp, p + 1, sizeof(disp));
            const uintptr_t at = reinterpret_cast<uintptr_t>(p);
            const uintptr_t to = at + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(disp));
            for (unsigned k = 0; k < n; ++k) {
                if (set[k].target != to) {
                    continue;
                }
                const uintptr_t owner = starts.Enclosing(at);
                if (owner == 0) {
                    break;
                }
                bool seen = false;
                for (unsigned j = 0; j < set[k].count; ++j) {
                    seen = seen || set[k].callers[j] == owner;
                }
                if (!seen) {
                    if (set[k].count == kMaxGlyphFns) {
                        set[k].overflowed = true;
                    } else {
                        set[k].callers[set[k].count++] = owner;
                    }
                }
                break;
            }
        }
    }
}

bool Holds(const uintptr_t* list, const unsigned count, const uintptr_t value)
{
    for (unsigned i = 0; i < count; ++i) {
        if (list[i] == value) {
            return true;
        }
    }
    return false;
}

bool InText(const Image& image, const uintptr_t addr)
{
    for (unsigned i = 0; i < image.text_count; ++i) {
        if (addr >= reinterpret_cast<uintptr_t>(image.text[i].begin) &&
            addr < reinterpret_cast<uintptr_t>(image.text[i].end)) {
            return true;
        }
    }
    return false;
}

// SkScalerContext declares seven virtual functions, and the two slots before
// generatePath hold generateImage and generateMetrics. All three load a glyph
// through the same helper. So the shape to look for is a walker in a vtable
// whose two preceding slots share its loader.
uintptr_t FindGeneratePath(const Image& image, const FunctionStarts& starts, void*** out_slot)
{
    void** const decompose = GotSlot(image, "FT_Outline_Decompose");
    void** const load = GotSlot(image, "FT_Load_Glyph");
    if (decompose == nullptr || load == nullptr) {
        Report("libxul: no outline walk or glyph load to find generatePath by");
        return 0;
    }

    uintptr_t walkers[kMaxGlyphFns];
    uintptr_t loaders[kMaxGlyphFns];
    unsigned n_walkers = 0, n_loaders = 0;
    if (!FunctionsCalling(image, starts, decompose, walkers, &n_walkers, kMaxGlyphFns) ||
        !FunctionsCalling(image, starts, load, loaders, &n_loaders, kMaxGlyphFns)) {
        Report("libxul: more functions walk an outline or load a glyph than can be "
               "accounted for, so generatePath cannot be identified");
        return 0;
    }

    // Callers that disqualify a candidate. A rasterizer walks an outline too,
    // so a function reached from one is not the path walker.
    uintptr_t rasterizers[kMaxGlyphFns * 2];
    unsigned n_rasterizers = 0;
    static constexpr const char* kRasterEntries[] = { "FT_Render_Glyph", "FT_Outline_Get_Bitmap" };
    for (const char* const name : kRasterEntries) {
        void** const slot = GotSlot(image, name);
        uintptr_t found[kMaxGlyphFns];
        unsigned n = 0;
        if (slot == nullptr || !FunctionsCalling(image, starts, slot, found, &n, kMaxGlyphFns)) {
            continue;
        }
        for (unsigned i = 0; i < n && n_rasterizers < kMaxGlyphFns * 2; ++i) {
            rasterizers[n_rasterizers++] = found[i];
        }
    }

    DirectCallers set[kMaxGlyphFns * 2];
    unsigned n_set = 0;
    for (unsigned i = 0; i < n_walkers; ++i) {
        set[n_set++].target = walkers[i];
    }
    for (unsigned i = 0; i < n_loaders; ++i) {
        set[n_set++].target = loaders[i];
    }
    CollectDirectCallers(image, starts, set, n_set);

    uintptr_t found = 0;
    void** found_slot = nullptr;
    for (unsigned w = 0; w < n_walkers; ++w) {
        const DirectCallers& walker = set[w];
        for (unsigned c = 0; c < walker.count; ++c) {
            const uintptr_t candidate = walker.callers[c];
            if (Holds(rasterizers, n_rasterizers, candidate)) {
                continue;                    // that one is generateImage
            }
            for (unsigned l = 0; l < n_loaders; ++l) {
                const DirectCallers& loader = set[n_walkers + l];
                if (loader.overflowed || !Holds(loader.callers, loader.count, candidate)) {
                    continue;
                }
                void** const slot = FindVtableSlot(image, candidate);
                if (slot == nullptr ||
                    reinterpret_cast<const unsigned char*>(slot - 2) < image.relro.begin) {
                    continue;
                }
                const auto image_fn = reinterpret_cast<uintptr_t>(slot[-1]);
                const auto metrics_fn = reinterpret_cast<uintptr_t>(slot[-2]);
                if (!InText(image, image_fn) || !InText(image, metrics_fn) ||
                    !Holds(loader.callers, loader.count, image_fn) ||
                    !Holds(loader.callers, loader.count, metrics_fn)) {
                    continue;
                }
                if (found != 0 && found != candidate) {
                    Report("libxul: more than one outline walk sits in a vtable beside two "
                           "glyph loads; leaving generatePath alone");
                    return 0;
                }
                found = candidate;
                found_slot = slot;
            }
        }
    }
    if (found == 0) {
        Report("libxul: no outline walk has the shape generatePath has");
        return 0;
    }
    *out_slot = found_slot;
    return found;
}

// The walk predicts these values exactly, so floats are compared here by bit
// pattern.
bool SameBits(const float a, const float b)
{
    uint32_t left = 0, right = 0;
    std::memcpy(&left, &a, sizeof(left));
    std::memcpy(&right, &b, sizeof(right));
    return left == right;
}

// How far into SkPathData the point span can sit. The object is a refcount, a
// listener list and four spans, so this reaches well past it.
constexpr size_t kPathDataSearchBytes = 256;

// Which answer the search below gave, said once for each.
void ReportRepairOnce(const char* what, const unsigned count)
{
    static std::atomic<const char*> said[3];
    for (auto& slot : said) {
        const char* had = slot.load(std::memory_order_relaxed);
        if (had == what) {
            return;
        }
        if (had == nullptr &&
            slot.compare_exchange_strong(had, what, std::memory_order_relaxed)) {
            Report("libxul: glyph path repair: %s (%u points)", what, count);
            return;
        }
    }
}

// Puts the unquantized coordinates into the finished path.
//
// Nothing here knows SkPathData's layout. The span is found by its contents.
// The walk recorded what every point would read as once SkFDot6ToScalar had
// divided it by 64, and 1/64 is a power of two, so those floats are bit for
// bit what the builder stored. A run of them as long as the walk, reached
// through a pointer and a count sitting side by side, is the point array.
//
// A search that finds no array leaves every coordinate on the walk's 1/64
// grid, which is why each outcome is reported.
void RepairGlyphPath(void* sret, const CleartypeGlyphPathPoint* points, const unsigned count)
{
    void* data = nullptr;
    if (!ReadWithoutFaulting(sret, static_cast<void*>(&data), sizeof(data)) || data == nullptr ||
        (reinterpret_cast<uintptr_t>(data) & 7u) != 0) {
        return;
    }

    unsigned char window[kPathDataSearchBytes];
    size_t have = kPathDataSearchBytes;
    while (have >= 2 * sizeof(void*) && !ReadWithoutFaulting(data, window, have)) {
        have /= 2;
    }
    if (have < 2 * sizeof(void*)) {
        return;
    }

    float* stored = nullptr;
    for (size_t at = 0; at + 2 * sizeof(void*) <= have; at += sizeof(void*)) {
        void* ptr = nullptr;
        size_t size = 0;
        std::memcpy(static_cast<void*>(&ptr), window + at, sizeof(ptr));
        std::memcpy(&size, window + at + sizeof(void*), sizeof(size));
        if (size != count || ptr == nullptr ||
            (reinterpret_cast<uintptr_t>(ptr) & 3u) != 0) {
            continue;
        }
        // Two floats a point, and every one of them has to be the value the
        // walk predicted. A run this long agreeing exactly is the array.
        auto* candidate = static_cast<float*>(ptr);
        float seen[2];
        bool matches = true;
        for (size_t i = 0; matches && i < count; ++i) {
            if (!ReadWithoutFaulting(candidate + 2 * i, seen, sizeof(seen))) {
                matches = false;
                break;
            }
            matches = SameBits(seen[0], points[i].match_x) &&
                      SameBits(seen[1], points[i].match_y);
        }
        if (!matches) {
            continue;
        }
        if (stored != nullptr) {
            ReportRepairOnce("two arrays read alike; neither was written", count);
            return;
        }
        stored = candidate;
    }
    if (stored == nullptr) {
        ReportRepairOnce("no array in the finished path reads as the walk's points", count);
        return;
    }
    ReportRepairOnce("the walk's points were found and replaced with the exact ones", count);

    float left = points[0].exact_x, right = points[0].exact_x;
    float top = points[0].exact_y, bottom = points[0].exact_y;
    for (size_t i = 0; i < count; ++i) {
        stored[2 * i] = points[i].exact_x;
        stored[2 * i + 1] = points[i].exact_y;
        left = std::fmin(left, points[i].exact_x);
        right = std::fmax(right, points[i].exact_x);
        top = std::fmin(top, points[i].exact_y);
        bottom = std::fmax(bottom, points[i].exact_y);
    }

    // The bounds cached beside the points were computed from the quantized
    // ones, so they are up to a 64th of a pixel out. They are found the same
    // way the array was, as four floats reading as the box it used to make.
    float was[4] = { points[0].match_x, points[0].match_y,
                     points[0].match_x, points[0].match_y };
    for (unsigned i = 0; i < count; ++i) {
        was[0] = std::fmin(was[0], points[i].match_x);
        was[1] = std::fmin(was[1], points[i].match_y);
        was[2] = std::fmax(was[2], points[i].match_x);
        was[3] = std::fmax(was[3], points[i].match_y);
    }
    // The box is matched by value. SkFTGeometrySink negates every y on its
    // way to the builder, so a point on the baseline arrives as -0.0 and the
    // box built here carries that sign, while the one Skia built may carry
    // either. The two zeroes are the same number and compare equal.
    const float now[4] = { left, top, right, bottom };
    for (size_t at = 0; at + sizeof(was) <= have; at += sizeof(float)) {
        float seen_box[4];
        std::memcpy(seen_box, window + at, sizeof(seen_box));
        if (seen_box[0] == was[0] && seen_box[1] == was[1] &&
            seen_box[2] == was[2] && seen_box[3] == was[3]) {
            std::memcpy(static_cast<unsigned char*>(data) + at, now, sizeof(now));
            return;
        }
    }
}

// GlyphMetrics generateMetrics(const SkGlyph&, SkArenaAlloc*), the vtable slot
// two before generatePath. Same shape as the walk, with a type that has a
// destructor coming back through a hidden pointer.
using GenerateMetricsFn = void* (*)(void* sret, void* self, const void* glyph, void* alloc);
GenerateMetricsFn g_generate_metrics = nullptr;

using GenerateImageFn = void (*)(void* self, const void* glyph, void* buffer);
GenerateImageFn g_generate_image = nullptr;

// Asks Skia to draw this scaler's glyphs from their outlines.
//
// SkScalerContext_DW::generateMetrics falls through to ScalerContextBits::PATH
// whenever generateDWMetrics cannot get texture bounds, and its generateImage
// then takes SkScalerContext::generateImageFromPath, so that scaler draws the
// glyph with Skia's own scan converter over the outline DirectWrite returns.
// SkScalerContext_FreeType has no such fallback, and the route is not chosen
// per glyph. SkScalerContext::internalGetImage reads fGenerateImageFromPath,
// so setting that field puts this scaler on the same route.
//
// The field is found by its neighbors, a live typeface reference followed by
// the two optional objects, which are null for text carrying neither a path
// effect nor a mask filter. Applied on a blob rasterizer only.
// SkScalerContext's layout, which the fields below check before anything is
// written:
//
//     vtable                              0
//     const SkScalerContextRec fRec;      8   56 bytes, ending at 63
//     SkTypeface& fTypeface;             64
//     sk_sp<SkPathEffect> fPathEffect;   72   null without a path effect
//     sk_sp<SkMaskFilter> fMaskFilter;   80   null without a mask filter
//     const bool fGenerateImageFromPath; 88
//
// fRec is a uint32 id, nine floats, two uint32 and eight bytes of small
// fields, so the text size sits at 12 and says whether this is the object at
// all.
constexpr size_t kRecTextSizeAt = 12;
// fRec is a uint32 id then nine floats, so the pre-scale and pre-skew follow
// the size and the relaxed 2x2 follows them. getSingleMatrix is built out of
// exactly these five.
constexpr size_t kRecPreScaleXAt = 16;
constexpr size_t kRecPreSkewXAt = 20;
constexpr size_t kRecPost2x2At = 24;
constexpr size_t kTypefaceAt = 64;
constexpr size_t kPathEffectAt = 72;
constexpr size_t kMaskFilterAt = 80;
constexpr size_t kImageFromPathAt = 88;
constexpr size_t kScalerProbeBytes = 96;

void DrawGlyphsFromPath(void* self)
{
    if (self == nullptr || CleartypeOnBlobRaster() == 0) {
        return;
    }
    unsigned char probe[kScalerProbeBytes];
    if (!ReadWithoutFaulting(self, probe, sizeof(probe))) {
        return;
    }
    float text_size = 0.0f;
    std::memcpy(&text_size, probe + kRecTextSizeAt, sizeof(text_size));
    if (!std::isfinite(text_size) || text_size <= 0.0f || text_size > 4096.0f) {
        return;
    }
    uintptr_t typeface = 0, effect = 1, filter = 1;
    std::memcpy(&typeface, probe + kTypefaceAt, sizeof(typeface));
    std::memcpy(&effect, probe + kPathEffectAt, sizeof(effect));
    std::memcpy(&filter, probe + kMaskFilterAt, sizeof(filter));
    if (typeface < 0x10000 || (typeface & 7u) != 0 || effect != 0 || filter != 0) {
        return;                              // not the shape this expects
    }
    if (CleartypeBlobPrefersMask() != 0) {
        return;                              // this one comes from the mask
    }
    if (probe[kImageFromPathAt] != 0) {
        return;                              // already set, or not a boolean
    }
    static_cast<unsigned char*>(self)[kImageFromPathAt] = 1;
    static std::atomic_flag said = ATOMIC_FLAG_INIT;
    if (!said.test_and_set(std::memory_order_relaxed)) {
        Report("libxul: a blob scaler at %.2f px now draws its glyphs from the path",
               static_cast<double>(text_size));
    }
}

// What this scaler was built with, handed to the shim.
//
// SkScalerContext_CairoFT::Lock spends the product of the size and the device
// matrix on FT_Set_Char_Size and hands the shape over separately, so what
// reaches FreeType is that product rounded to a 26.6 and a matrix normalized
// to 16.16. Neither carries the terms it was made of, and under 16 px of size
// two neighboring 1024ths of the device matrix round to one char size, so the
// product cannot be taken apart again. It never has to be: the five fields
// getSingleMatrix builds the total matrix out of are in this object, and this
// hook is already reading the bytes they sit in.
//
// The neighbors say whether this is the object at all, and the shim checks the
// matrix against the char sizes the face actually received before using it.
void ReportScalerTextSize(void* self)
{
    if (self == nullptr) {
        CleartypeSkiaScaler(0.0, 0.0, 0.0, nullptr);
        return;
    }
    unsigned char probe[kScalerProbeBytes];
    float text_size = 0.0f;
    float pre_scale_x = 0.0f;
    float pre_skew_x = 0.0f;
    float post[4] = {};
    uintptr_t typeface = 0;
    if (!ReadWithoutFaulting(self, probe, sizeof(probe))) {
        CleartypeSkiaScaler(0.0, 0.0, 0.0, nullptr);
        return;
    }
    std::memcpy(&text_size, probe + kRecTextSizeAt, sizeof(text_size));
    std::memcpy(&pre_scale_x, probe + kRecPreScaleXAt, sizeof(pre_scale_x));
    std::memcpy(&pre_skew_x, probe + kRecPreSkewXAt, sizeof(pre_skew_x));
    std::memcpy(post, probe + kRecPost2x2At, sizeof(post));
    std::memcpy(&typeface, probe + kTypefaceAt, sizeof(typeface));
    bool usable = std::isfinite(text_size) && text_size > 0.0f && text_size <= 4096.0f &&
                  typeface >= 0x10000 && (typeface & 7u) == 0 && std::isfinite(pre_scale_x) &&
                  std::isfinite(pre_skew_x);
    for (const float term : post) {
        usable = usable && std::isfinite(term);
    }
    if (!usable) {
        CleartypeSkiaScaler(0.0, 0.0, 0.0, nullptr);
        return;
    }
    // The rec holds these as SkScalar, and the shim rebuilds the total matrix
    // in double, as SkScalerContextRec::getSingleMatrix does.
    const double terms[4] = { static_cast<double>(post[0]), static_cast<double>(post[1]),
                              static_cast<double>(post[2]), static_cast<double>(post[3]) };
    CleartypeSkiaScaler(static_cast<double>(text_size), static_cast<double>(pre_scale_x),
                        static_cast<double>(pre_skew_x), terms);
}

extern "C" void* DwcGenerateMetrics(void* sret, void* self, const void* glyph, void* alloc);

extern "C" void* DwcGenerateMetrics(void* sret, void* self, const void* glyph, void* alloc)
{
    // The size before the call, since the glyph this scaler measures is loaded
    // inside it.
    ReportScalerTextSize(self);
    // The route after it, so the shim has seen this scaler's face by the time
    // it is chosen; internalMakeGlyph reads the flag only after
    // generateMetrics returns, so setting it there is still in time.
    CleartypeEnterSkiaScaler();
    void* const result = g_generate_metrics(sret, self, glyph, alloc);
    CleartypeLeaveSkiaScaler();
    DrawGlyphsFromPath(self);
    return result;
}

// A strike measures its glyphs when they are laid out and draws them when the
// display list reaches them, and other scalers run on the thread in between.
// So the size and the device matrix are read again here, where the mask this
// scaler is about to fill in is the one the shim answers.
extern "C" void DwcGenerateImage(void* self, const void* glyph, void* buffer);

extern "C" void DwcGenerateImage(void* self, const void* glyph, void* buffer)
{
    ReportScalerTextSize(self);
    CleartypeEnterSkiaScaler();
    g_generate_image(self, glyph, buffer);
    CleartypeLeaveSkiaScaler();
}

extern "C" void* DwcGeneratePath(void* sret, void* self, const void* glyph);

extern "C" void* DwcGeneratePath(void* sret, void* self, const void* glyph)
{
    ReportScalerTextSize(self);
    CleartypeBeginGlyphPath();
    CleartypeEnterSkiaScaler();
    void* const result = g_generate_path(sret, self, glyph);
    CleartypeLeaveSkiaScaler();
    const CleartypeGlyphPathPoint* points = nullptr;
    const unsigned count = CleartypeEndGlyphPath(&points);
    if (count != 0 && result != nullptr) {
        RepairGlyphPath(sret, points, count);
    }
    return result;
}

bool PatchGlyphPath(const Image& image, const FunctionStarts& starts)
{
    void** slot = nullptr;
    const uintptr_t found = FindGeneratePath(image, starts, &slot);
    if (found == 0 || slot == nullptr) {
        return false;
    }
    g_generate_path = reinterpret_cast<GeneratePathFn>(found);
    if (!WriteSlot(slot, reinterpret_cast<void*>(&DwcGeneratePath))) {
        g_generate_path = nullptr;
        return false;
    }
    Report("libxul: generatePath %#lx now returns through this library "
           "(vtable slot %p)", found, static_cast<void*>(slot));

    // The walk is worth having on its own, so a metrics slot that will not
    // take leaves it in place.
    const auto metrics = reinterpret_cast<uintptr_t>(slot[-2]);
    if (metrics != 0) {
        g_generate_metrics = reinterpret_cast<GenerateMetricsFn>(metrics);
        if (WriteSlot(slot - 2, reinterpret_cast<void*>(&DwcGenerateMetrics))) {
            Report("libxul: generateMetrics %#lx now returns through this library",
                   metrics);
        } else {
            g_generate_metrics = nullptr;
        }
    }

    const auto raster = reinterpret_cast<uintptr_t>(slot[-1]);
    if (raster != 0) {
        g_generate_image = reinterpret_cast<GenerateImageFn>(raster);
        if (WriteSlot(slot - 1, reinterpret_cast<void*>(&DwcGenerateImage))) {
            Report("libxul: generateImage %#lx now returns through this library", raster);
        } else {
            g_generate_image = nullptr;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The glyph ink box.
//
// gfxFT2FontBase::GetGlyphBounds answers what canvas measureText and layout
// use for the ink extents of a run. It reads the box back out of the glyph
// slot's FT_Glyph_Metrics, which are 26.6, so the exact edges the shim wrote
// there arrive rounded to 1/64 px. Windows has no such step. gfxDWriteFont
// takes GetDesignGlyphMetrics and scales it, in doubles the whole way.
//
// Everything else downstream carries that rounding as a rounding.
// gfxFont::Measure widens a synthetically obliqued box by ceil(skew * edge) in
// app units, where half a 1/64 px moves the ceiling by one and takes 1/60 px
// of ink on the reported left or right with it.
//
// So the real function runs and the four edges are put back at full precision
// afterwards. Nothing here names it. It is found as the one function that
// compares both the SVG and the COLR sfnt tags while emboldening through
// FT_MulFix and sizing through FT_Set_Char_Size, whose one caller sits in a
// vtable.
// ---------------------------------------------------------------------------

using GetGlyphBoundsFn = bool (*)(void* self, uint16_t gid, double* bounds, bool tight);
GetGlyphBoundsFn g_glyph_bounds = nullptr;

// gfxFont builds them as TRUETYPE_TAG(a,b,c,d), which packs the first
// character into the high byte, so the immediate reads back to front.
constexpr uint32_t kSvgTag = 0x53564720;     // 'SVG '
constexpr uint32_t kColrTag = 0x434F4C52;    // 'COLR'

constexpr unsigned kMaxBoundsFns = 24;

// Every function holding `value` as a four-byte immediate. A byte scan cannot
// tell an operand from the middle of an instruction, so this only ever narrows
// a candidate set that other tests have to agree with.
bool FunctionsHolding(const Image& image, const FunctionStarts& starts, const uint32_t value,
                      uintptr_t* out, unsigned* count, const unsigned max)
{
    unsigned char want[sizeof(value)];
    std::memcpy(want, &value, sizeof(want));
    *count = 0;
    for (unsigned i = 0; i < image.text_count; ++i) {
        const Region& r = image.text[i];
        for (const unsigned char* p = r.begin; p + sizeof(want) <= r.end; ++p) {
            if (std::memcmp(p, want, sizeof(want)) != 0) {
                continue;
            }
            const uintptr_t fn = starts.Enclosing(reinterpret_cast<uintptr_t>(p));
            bool seen = false;
            for (unsigned k = 0; k < *count; ++k) {
                seen = seen || out[k] == fn;
            }
            if (!seen && fn != 0) {
                if (*count == max) {
                    return false;            // more than this can account for
                }
                out[(*count)++] = fn;
            }
        }
    }
    return true;
}

// Every place in relro holding `function`. FindVtableSlot answers for one
// slot and refuses a second; a method inherited by two concrete classes is in
// both their vtables, and both have to be written.
unsigned CollectVtableSlots(const Image& image, const uintptr_t function, void** out,
                            const unsigned max)
{
    unsigned count = 0;
    const auto* p = reinterpret_cast<const uintptr_t*>(image.relro.begin);
    const auto* end = reinterpret_cast<const uintptr_t*>(image.relro.end);
    for (; p + 1 <= end; ++p) {
        if (*p != function) {
            continue;
        }
        if (count == max) {
            return 0;
        }
        out[count++] = const_cast<void*>(static_cast<const void*>(p));
    }
    return count;
}

// gfxFT2FontBase::GetFTGlyphExtents, the one function that asks the entry for
// both an SVG and a COLR table while it has a face sized and emboldened.
uintptr_t FindGlyphExtents(const Image& image, const FunctionStarts& starts)
{
    void** const size = GotSlot(image, "FT_Set_Char_Size");
    void** const embolden = GotSlot(image, "FT_MulFix");
    if (size == nullptr || embolden == nullptr) {
        Report("libxul: no glyph sizing or emboldening to find the ink box by");
        return 0;
    }
    uintptr_t svg[kMaxBoundsFns], colr[kMaxBoundsFns];
    uintptr_t sizers[kMaxBoundsFns], embolders[kMaxBoundsFns];
    unsigned n_svg = 0, n_colr = 0, n_sizers = 0, n_embolders = 0;
    if (!FunctionsHolding(image, starts, kSvgTag, svg, &n_svg, kMaxBoundsFns) ||
        !FunctionsHolding(image, starts, kColrTag, colr, &n_colr, kMaxBoundsFns) ||
        !FunctionsCalling(image, starts, size, sizers, &n_sizers, kMaxBoundsFns) ||
        !FunctionsCalling(image, starts, embolden, embolders, &n_embolders, kMaxBoundsFns)) {
        Report("libxul: more functions carry an sfnt tag or size a face than can be "
               "accounted for, so the ink box cannot be identified");
        return 0;
    }
    uintptr_t found = 0;
    for (unsigned i = 0; i < n_svg; ++i) {
        const uintptr_t candidate = svg[i];
        if (!Holds(colr, n_colr, candidate) || !Holds(sizers, n_sizers, candidate) ||
            !Holds(embolders, n_embolders, candidate)) {
            continue;
        }
        if (found != 0) {
            Report("libxul: more than one function reads both sfnt tags off a sized "
                   "face; leaving the ink box alone");
            return 0;
        }
        found = candidate;
    }
    if (found == 0) {
        Report("libxul: no function has the shape GetFTGlyphExtents has");
    }
    return found;
}

// gfxFT2FontBase::GetGlyphBounds, the only caller of that which a vtable
// holds. The other three are GetCharExtents, GetCachedGlyphMetrics and
// InitMetrics, none of them virtual.
uintptr_t FindGlyphBounds(const Image& image, const FunctionStarts& starts, void** slots,
                          unsigned* n_slots, const unsigned max)
{
    const uintptr_t extents = FindGlyphExtents(image, starts);
    if (extents == 0) {
        return 0;
    }
    DirectCallers set[1];
    set[0].target = extents;
    CollectDirectCallers(image, starts, set, 1);
    if (set[0].overflowed) {
        Report("libxul: GetFTGlyphExtents at %#lx has more callers than a private "
               "method has", extents);
        return 0;
    }
    uintptr_t found = 0;
    unsigned found_slots = 0;
    for (unsigned i = 0; i < set[0].count; ++i) {
        void* here[kMaxBoundsFns];
        const unsigned n = CollectVtableSlots(image, set[0].callers[i], here, kMaxBoundsFns);
        if (n == 0) {
            continue;
        }
        if (found != 0) {
            Report("libxul: more than one caller of GetFTGlyphExtents sits in a vtable; "
                   "leaving the ink box alone");
            return 0;
        }
        found = set[0].callers[i];
        found_slots = n > max ? 0 : n;
        for (unsigned k = 0; k < found_slots; ++k) {
            slots[k] = here[k];
        }
    }
    if (found == 0 || found_slots == 0) {
        Report("libxul: no caller of GetFTGlyphExtents at %#lx is in a vtable", extents);
        return 0;
    }
    *n_slots = found_slots;
    return found;
}

// The four edges the real function returned, put back at the precision the
// slot's 26.6 metrics could not hold.
//
// gfxFT2FontBase::GetFTGlyphExtents builds its rect out of them as
//
//     x = horiBearingX;   y = -horiBearingY;   x2 = x + width;   y2 = y + height
//     y -= bold.y;        x2 += bold.x
//     rect = (x, y, x2 - x, y2 - y)
//
// and scales it by GetAdjustedSize() / mFTSize. Where the shim wrote that box
// it also placed the bearings so the emboldening lands on the Windows edge, so
// all four edges come back as whole 1/64 px of the shim's own numbers. Each is
// checked against them before it moves, and a box that does not agree is left
// as it came: it went through hint rounding, or the color fallback replaced it,
// or FreeType's own metrics were never overwritten.
void SubstituteInkBox(void* self, const uint16_t gid, double* bounds)
{
    const size_t at = g_ftface_word.load(std::memory_order_relaxed);
    const size_t back = g_adjusted_size_delta.load(std::memory_order_relaxed);
    if (self == nullptr || bounds == nullptr || at == 0 || back == 0 || back > at) {
        return;
    }
    auto* const shared = static_cast<void* const*>(self)[at - 1];
    if (shared == nullptr) {
        return;
    }
    // `Metrics mMetrics; int mFTLoadFlags; bool mEmbolden; gfxFloat mFTSize;`
    // puts the flags and the bool in the word before the size.
    unsigned char flags_word[sizeof(void*)] = {};
    double ft_size = 0.0, adjusted = 0.0;
    std::memcpy(flags_word, WordAt(self, at + kMetricsFields), sizeof(flags_word));
    std::memcpy(&ft_size, WordAt(self, at + kFTSizeWord), sizeof(ft_size));
    std::memcpy(&adjusted, static_cast<double*>(self) + (at - back), sizeof(adjusted));
    if (!(ft_size > 0.0) || !(adjusted > 0.0) || flags_word[sizeof(int)] > 1) {
        return;                              // not a boolean where one should be
    }
    const int embolden = flags_word[sizeof(int)];
    // The rewrite above replaced mFTSize with the strike size, and the glyphs
    // in the slot were loaded through the instance the font carried before
    // that. Asking at the current size looks for an instance never built.
    const double prior = PriorFTSizeFor(self);
    const double ask = prior > 0.0 ? prior : ft_size;
    double box[4] = {};
    bool have = false;
    for (size_t i = 0; i < 4 && !have; ++i) {
        have = CleartypeGlyphInkBox(static_cast<void* const*>(shared)[i], ask, embolden,
                                    gid, box) != 0;
    }
    if (!have) {
        return;
    }
    const double scale = adjusted / ft_size;
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        return;
    }
    // What the shim rounded into the slot, and what the rect above made of it.
    const double want[4] = {std::round(box[0] * 64.0), -std::round(box[1] * 64.0),
                            std::round(box[2] * 64.0), std::round(box[3] * 64.0)};
    const double to_26_6 = 64.0 / scale;
    const double got[4] = {bounds[0] * to_26_6, bounds[1] * to_26_6,
                           (bounds[0] + bounds[2]) * to_26_6,
                           (bounds[1] + bounds[3]) * to_26_6};
    constexpr double kNear = 1.0 / 4096.0;
    for (unsigned i = 0; i < 4; ++i) {
        if (std::fabs(got[i] - want[i]) > kNear) {
            return;
        }
    }
    const double left = box[0] * scale;
    const double top = -box[1] * scale;
    bounds[0] = left;
    bounds[1] = top;
    bounds[2] = box[2] * scale - left;
    bounds[3] = box[3] * scale - top;
}

extern "C" bool DwcGetGlyphBounds(void* self, uint16_t gid, double* bounds, bool tight);

extern "C" bool DwcGetGlyphBounds(void* self, const uint16_t gid, double* bounds,
                                  const bool tight)
{
    const bool ok = g_glyph_bounds(self, gid, bounds, tight);
    if (ok) {
        SubstituteInkBox(self, gid, bounds);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// The horizontal origin an upright glyph is drawn from in vertical text.
//
// gfxHarfBuzzShaper::GetGlyphVOrigin sets it to half the advance
// GetGlyphHAdvance answers with, and the two platforms answer that from
// different places. gfxDWriteFont::ProvidesGlyphWidths is false while
// mUseSubpixelPositions holds and the face carries no bold simulation, so the
// shaper reads the hmtx table and the advance carries no synthetic bold, which
// gfxFont::Draw adds afterwards as cluster tracking. gfxFT2FontBase::
// ProvidesGlyphWidths is true for every font and GetFTGlyphExtents adds the
// embolden strength to the advance it returns, so half that strength lands on
// the origin of every upright glyph.
//
// Only the x is replaced. The y comes from VORG or vmtx, which both platforms
// read alike.
// ---------------------------------------------------------------------------

using GetGlyphVOriginFn = void (*)(void* self, uint32_t glyph, int32_t* x, int32_t* y);
GetGlyphVOriginFn g_glyph_v_origin = nullptr;

constexpr uint32_t kHheaTag = 0x68686561;    // 'hhea'
constexpr uint32_t kOS2Tag = 0x4F532F32;     // 'OS/2'
constexpr uint32_t kVheaTag = 0x76686561;    // 'vhea'
constexpr uint32_t kPostTag = 0x706F7374;    // 'post'
constexpr uint32_t kHmtxTag = 0x686D7478;    // 'hmtx'

// gfxHarfBuzzShaper::GetGlyphVOrigin, the one function that reads the hhea
// table, reads no other metrics table, and is called from a function nothing
// calls.
//
// gfxFont::InitMetricsFromSfntTables reads OS/2 beside hhea,
// gfxFont::CreateVerticalMetrics reads vhea, post and OS/2, and
// gfxHarfBuzzShaper::LoadHmtxTable reads hmtx; a byte scan cannot always see
// the second tag, since a cold block carrying it starts a function of its own.
// The caller settles what is left. GetGlyphVOrigin is reached only through
// HBGetGlyphVOrigin, a static handed to hb_font_funcs_set_glyph_v_origin_func
// and called through the font funcs, so nothing in the image calls it, while
// the other three are reached from ordinary member functions.
uintptr_t FindGlyphVOrigin(const Image& image, const FunctionStarts& starts)
{
    uintptr_t hhea[kMaxGlyphFns], os2[kMaxBoundsFns], vhea[kMaxBoundsFns];
    uintptr_t post[kMaxBoundsFns], hmtx[kMaxBoundsFns];
    unsigned n_hhea = 0, n_os2 = 0, n_vhea = 0, n_post = 0, n_hmtx = 0;
    if (!FunctionsHolding(image, starts, kHheaTag, hhea, &n_hhea, kMaxGlyphFns) ||
        !FunctionsHolding(image, starts, kOS2Tag, os2, &n_os2, kMaxBoundsFns) ||
        !FunctionsHolding(image, starts, kVheaTag, vhea, &n_vhea, kMaxBoundsFns) ||
        !FunctionsHolding(image, starts, kPostTag, post, &n_post, kMaxBoundsFns) ||
        !FunctionsHolding(image, starts, kHmtxTag, hmtx, &n_hmtx, kMaxBoundsFns)) {
        Report("libxul: more functions carry a metrics table tag than can be accounted "
               "for, so the vertical origin cannot be identified");
        return 0;
    }
    DirectCallers below[kMaxGlyphFns];
    unsigned n_below = 0;
    for (unsigned i = 0; i < n_hhea; ++i) {
        const uintptr_t candidate = hhea[i];
        if (Holds(os2, n_os2, candidate) || Holds(vhea, n_vhea, candidate) ||
            Holds(post, n_post, candidate) || Holds(hmtx, n_hmtx, candidate)) {
            continue;
        }
        below[n_below++].target = candidate;
    }
    if (n_below == 0) {
        Report("libxul: no function reads hhea and no other metrics table");
        return 0;
    }
    CollectDirectCallers(image, starts, below, n_below);

    // The one caller each survivor has, asked in turn whether anything calls it.
    DirectCallers above[kMaxGlyphFns];
    unsigned n_above = 0;
    for (unsigned i = 0; i < n_below; ++i) {
        if (below[i].overflowed || below[i].count != 1) {
            continue;
        }
        above[n_above++].target = below[i].callers[0];
    }
    if (n_above == 0) {
        Report("libxul: every function reading hhea alone has more than one caller");
        return 0;
    }
    CollectDirectCallers(image, starts, above, n_above);

    uintptr_t found = 0;
    for (unsigned i = 0; i < n_below; ++i) {
        if (below[i].overflowed || below[i].count != 1) {
            continue;
        }
        bool orphan = false;
        for (unsigned k = 0; k < n_above; ++k) {
            if (above[k].target == below[i].callers[0]) {
                orphan = !above[k].overflowed && above[k].count == 0;
            }
        }
        if (!orphan) {
            continue;
        }
        if (found != 0) {
            Report("libxul: more than one function reads hhea alone from a caller "
                   "nothing calls; leaving the vertical origin alone");
            return 0;
        }
        found = below[i].target;
    }
    if (found == 0) {
        Report("libxul: no function reads hhea alone from a caller nothing calls");
    }
    return found;
}

// The font the shaper is working with. gfxFontShaper holds it in the word after
// the vtable pointer and gfxHarfBuzzShaper derives from it alone, so it is the
// second word of the object. Answered only when the word looks like a font this
// library already knows how to read.
void* ShaperFont(void* self)
{
    const size_t at = g_ftface_word.load(std::memory_order_relaxed);
    if (self == nullptr || at == 0) {
        return nullptr;
    }
    void* font = nullptr;
    if (!ReadWithoutFaulting(static_cast<const void*>(static_cast<void**>(self) + 1),
                             static_cast<void*>(&font), sizeof(font)) ||
        font == nullptr) {
        return nullptr;
    }
    return font;
}

extern "C" void DwcGetGlyphVOrigin(void* self, uint32_t glyph, int32_t* x, int32_t* y);

extern "C" void DwcGetGlyphVOrigin(void* self, const uint32_t glyph, int32_t* x, int32_t* y)
{
    g_glyph_v_origin(self, glyph, x, y);
    void* const font = ShaperFont(self);
    if (font == nullptr || x == nullptr) {
        return;
    }
    const size_t at = g_ftface_word.load(std::memory_order_relaxed);
    void* shared = nullptr;
    unsigned char flags_word[sizeof(void*)] = {};
    double ft_size = 0.0;
    if (!ReadWithoutFaulting(static_cast<const void*>(static_cast<void**>(font) + (at - 1)),
                             static_cast<void*>(&shared), sizeof(shared)) ||
        shared == nullptr ||
        !ReadWithoutFaulting(WordAt(font, at + kMetricsFields), flags_word, sizeof(flags_word)) ||
        !ReadWithoutFaulting(WordAt(font, at + kFTSizeWord), &ft_size, sizeof(ft_size))) {
        return;
    }
    if (!(ft_size > 0.0) || !(ft_size < 65536.0) || flags_word[sizeof(int)] > 1) {
        return;                              // not a boolean where one should be
    }
    const int embolden = flags_word[sizeof(int)];
    int32_t windows = 0;
    for (size_t i = 0; i < 4; ++i) {
        void* candidate = nullptr;
        if (!ReadWithoutFaulting(static_cast<const void*>(static_cast<void**>(shared) + i),
                                 static_cast<void*>(&candidate), sizeof(candidate))) {
            return;
        }
        if (CleartypeWindowsVOriginX(candidate, ft_size, embolden, glyph, &windows) != 0) {
            *x = windows;
            return;
        }
    }
}

bool PatchGlyphVOrigin(const Image& image, const FunctionStarts& starts)
{
    const uintptr_t found = FindGlyphVOrigin(image, starts);
    if (found == 0) {
        return false;
    }
    g_glyph_v_origin = reinterpret_cast<GetGlyphVOriginFn>(found);
    const unsigned patched =
        RedirectCalls(image, starts, found, reinterpret_cast<void*>(&DwcGetGlyphVOrigin));
    if (patched == 0) {
        g_glyph_v_origin = nullptr;
        Report("libxul: GetGlyphVOrigin at %#lx is not called directly anywhere", found);
        return false;
    }
    Report("libxul: GetGlyphVOrigin %#lx now returns through this library (%u call site%s)",
           found, patched, patched == 1 ? "" : "s");
    return true;
}

// ---------------------------------------------------------------------------
// WebRender's own font size.
//
// gfxFont settles mAdjustedSize in the content process and the InitMetrics
// hook claims it there, but WebRender rasterizes in the parent process, where
// no gfxFont for the page's fonts exists. The size reaches FreeType only as
// FT_Set_Char_Size's 26.6 value, and two sizes a fraction apart round onto one
// of those: a whole app unit, and the sub- or superscript reduction of
// another. Inverting that rounding cannot separate them, so the size is read
// from the instance WebRender is rasterizing from.
//
// wr_glyph_rasterizer's load_glyph is the one function that reaches FreeType
// for a glyph. It sets the transform, sets the char size, picks a strike and
// loads the glyph, and nothing else in libxul calls all four.
// ---------------------------------------------------------------------------

// The FreeType entries load_glyph calls, and how many of them there are.
constexpr const char* kLoadGlyphCalls[] = {"FT_Set_Char_Size", "FT_Set_Transform",
                                           "FT_Load_Glyph", "FT_Select_Size"};
constexpr unsigned kLoadGlyphCallCount = 4;
// A function reaching one of these more than this many times is not the one.
constexpr unsigned kMaxFtOwners = 32;

uintptr_t FindWebRenderLoadGlyph(const Image& image, const FunctionStarts& starts)
{
    // libxul reaches FreeType through its GOT, so a call names the slot rather
    // than the function, and the slot is found by the value in it.
    uintptr_t slots[kLoadGlyphCallCount] = {};
    for (unsigned i = 0; i < kLoadGlyphCallCount; ++i) {
        void* fn = dlsym(RTLD_DEFAULT, kLoadGlyphCalls[i]);
        if (fn == nullptr) {
            return 0;
        }
        for (const unsigned char* p = image.relro.begin;
             p + sizeof(void*) <= image.relro.end; p += sizeof(void*)) {
            void* value = nullptr;
            std::memcpy(&value, p, sizeof(value));
            if (value == fn) {
                slots[i] = reinterpret_cast<uintptr_t>(p);
                break;
            }
        }
        if (slots[i] == 0) {
            return 0;
        }
    }

    uintptr_t owners[kLoadGlyphCallCount][kMaxFtOwners] = {};
    unsigned counts[kLoadGlyphCallCount] = {};
    for (unsigned i = 0; i < image.text_count; ++i) {
        const Region& r = image.text[i];
        for (const unsigned char* q = r.begin; q + 6 <= r.end; ++q) {
            if (q[0] != 0xFF || q[1] != 0x15) {
                continue;
            }
            int32_t disp = 0;
            std::memcpy(&disp, q + 2, sizeof(disp));
            const uintptr_t target = reinterpret_cast<uintptr_t>(q) + 6 + disp;
            for (unsigned k = 0; k < kLoadGlyphCallCount; ++k) {
                if (target != slots[k] || counts[k] >= kMaxFtOwners) {
                    continue;
                }
                const uintptr_t owner = starts.Enclosing(reinterpret_cast<uintptr_t>(q));
                if (owner == 0) {
                    continue;
                }
                bool seen = false;
                for (unsigned m = 0; m < counts[k]; ++m) {
                    seen = seen || owners[k][m] == owner;
                }
                if (!seen) {
                    owners[k][counts[k]++] = owner;
                }
            }
        }
    }

    uintptr_t found = 0;
    for (unsigned m = 0; m < counts[0]; ++m) {
        const uintptr_t candidate = owners[0][m];
        bool all = true;
        for (unsigned k = 1; k < kLoadGlyphCallCount; ++k) {
            bool here = false;
            for (unsigned n = 0; n < counts[k]; ++n) {
                here = here || owners[k][n] == candidate;
            }
            all = all && here;
        }
        if (!all) {
            continue;
        }
        // Two would mean the four calls no longer name one function.
        if (found != 0) {
            return 0;
        }
        found = candidate;
    }
    return found;
}

bool PatchWebRenderSize(const Image& image, const FunctionStarts& starts)
{
    const uintptr_t found = FindWebRenderLoadGlyph(image, starts);
    if (found == 0) {
        Report("libxul: WebRender's load_glyph not found; its sizes stay reconstructed");
        return false;
    }
    g_wr_load_glyph_orig = reinterpret_cast<void*>(found);
    const unsigned patched = RedirectCalls(image, starts, found,
                                           reinterpret_cast<void*>(&DwcWrLoadGlyphThunk));
    if (patched == 0) {
        g_wr_load_glyph_orig = nullptr;
        Report("libxul: WebRender's load_glyph at %#lx is not called directly anywhere", found);
        return false;
    }
    Report("libxul: WebRender's load_glyph %#lx now names its size (%u call site%s)",
           found, patched, patched == 1 ? "" : "s");
    return true;
}

bool PatchGlyphBounds(const Image& image, const FunctionStarts& starts)
{
    void* slots[kMaxBoundsFns];
    unsigned n_slots = 0;
    const uintptr_t found = FindGlyphBounds(image, starts, slots, &n_slots, kMaxBoundsFns);
    if (found == 0) {
        return false;
    }
    g_glyph_bounds = reinterpret_cast<GetGlyphBoundsFn>(found);
    unsigned written = 0;
    for (unsigned i = 0; i < n_slots; ++i) {
        written += WriteSlot(static_cast<void**>(slots[i]),
                             reinterpret_cast<void*>(&DwcGetGlyphBounds)) ? 1u : 0u;
    }
    if (written == 0) {
        g_glyph_bounds = nullptr;
        return false;
    }
    Report("libxul: GetGlyphBounds %#lx now returns through this library "
           "(%u of %u vtable slots)", found, written, n_slots);
    return true;
}

// ---------------------------------------------------------------------------
// The platform a media query answers with.
//
// Gecko_MediaFeatures_MatchesPlatform compares its argument against the one
// platform the build was made for and returns whether they are the same, so
// the whole function is an endbr64, a compare against that platform's
// ordinal, a sete and a ret. Those eleven bytes are its own anchor, appearing
// once in the image and starting a function. The ordinals are servo's Platform
// enum in declaration order.
//
// Rust calls it directly, so the dynamic symbol table offers nothing to
// interpose and the byte it compares against is what changes.
// ---------------------------------------------------------------------------

// servo/components/style/gecko/media_features.rs, enum Platform.
constexpr unsigned char kPlatformLinux = 1;
constexpr unsigned char kPlatformWindows = 4;

// endbr64; cmp edi, kPlatformLinux; sete al; ret
constexpr unsigned char kPlatformCompare[] = {0xF3, 0x0F, 0x1E, 0xFA, 0x83, 0xFF,
                                              kPlatformLinux, 0x0F, 0x94, 0xC0, 0xC3};
constexpr size_t kPlatformCompareImmediate = 6;

unsigned char* FindPlatformCompare(const Image& image, const FunctionStarts& starts)
{
    unsigned char* found = nullptr;
    for (unsigned i = 0; i < image.text_count; ++i) {
        const Region& r = image.text[i];
        const unsigned char* at = r.begin;
        while (at + sizeof(kPlatformCompare) <= r.end) {
            const void* hit = memmem(at, static_cast<size_t>(r.end - at), kPlatformCompare,
                                     sizeof(kPlatformCompare));
            if (hit == nullptr) {
                break;
            }
            at = static_cast<const unsigned char*>(hit) + 1;
            const auto address = reinterpret_cast<uintptr_t>(hit);
            if (starts.Enclosing(address) != address) {
                continue;                    // the same bytes inside a longer function
            }
            if (found != nullptr) {
                return nullptr;              // two of them, so neither is the anchor
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            found = const_cast<unsigned char*>(static_cast<const unsigned char*>(hit));
        }
    }
    return found;
}

// One byte of the instruction stream, opened and closed around the write.
bool WriteCode(unsigned char* at, const unsigned char value)
{
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return false;
    }
    const uintptr_t first = reinterpret_cast<uintptr_t>(at) & ~static_cast<uintptr_t>(page - 1);
    auto* start = reinterpret_cast<void*>(first);
    const auto len = static_cast<size_t>(page);
    // Writable and executable at once, as the InitMetrics sites are. This
    // patches live text, and dropping PROT_EXEC for the duration would fault
    // any thread that entered the page while it was non-executable.
    // NOLINTNEXTLINE(clang-analyzer-security.MmapWriteExec)
    if (mprotect(start, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        Report("libxul: the text page would not open for writing");
        return false;
    }
    // One byte, so no reader can see a half-written instruction.
    *at = value;
    if (mprotect(start, len, PROT_READ | PROT_EXEC) != 0) {
        Report("libxul: the text page would not close again");
        // The byte is written and correct. The page keeps write permission it
        // did not have before, which weakens it without changing what it holds.
    }
    return true;
}

bool PatchPlatformMediaFeature(const Image& image, const FunctionStarts& starts)
{
    unsigned char* body = FindPlatformCompare(image, starts);
    if (body == nullptr) {
        Report("libxul: no single platform comparison to move, so a ruby "
               "annotation keeps the ruby glyph designs Windows leaves alone");
        return false;
    }
    if (!WriteCode(body + kPlatformCompareImmediate, kPlatformWindows)) {
        return false;
    }
    Report("libxul: -moz-platform at %#lx answers windows rather than linux",
           reinterpret_cast<uintptr_t>(body));
    return true;
}

void Apply(const char* path, const uintptr_t base, const ElfW(Phdr)* phdr, ElfW(Half) const phnum)
{
    if (g_done.exchange(true)) {
        return;                              // one attempt, success or not
    }

    if (EnvDisables("CLEARTYPE_LIBXUL_PATCH")) {
        return;
    }

    Image image;
    if (!DescribeImage(path, base, phdr, phnum, &image)) {
        Report("libxul: %s has no shape this can read", path);
        return;
    }
    // SetBlobTextGamma needs the image long after this and cannot call
    // dl_iterate_phdr for it, which holds the loader's list lock while a glyph
    // on any thread could reach it.
    g_libxul = image;
    g_libxul_known.store(true, std::memory_order_release);
    if (!ResolveHelpers(image)) {
        return;
    }

    const unsigned char* block =
        FindUnique(image, kNameAnchor, sizeof(kNameAnchor));
    if (block == nullptr) {
        Report("libxul: the gfxPlatformGtk fallback names are absent or "
               "repeated in %s; leaving it alone", path);
        return;
    }

    FunctionStarts starts;
    if (!starts.Parse(image.eh_frame_hdr, image.eh_frame_hdr_size)) {
        Report("libxul: no usable eh_frame_hdr, so functions cannot be bounded");
        return;
    }

    // The anchor is the middle of the run; the references reach either side of
    // it, so give the search the whole neighborhood.
    const unsigned char* from = block > image.rodata[0].begin + 0x80
                                    ? block - 0x80 : image.rodata[0].begin;
    const uintptr_t function = FindOwningFunction(image, starts, from,
                                                  sizeof(kNameAnchor) + 0x180);
    if (function == 0) {
        return;
    }

    void** slot = FindVtableSlot(image, function);
    if (slot == nullptr) {
        Report("libxul: GetCommonFallbackFonts at %#lx is in no vtable",
               function);
        return;
    }

    g_original = reinterpret_cast<GetCommonFallbackFontsFn>(function);
    if (!WriteSlot(slot, reinterpret_cast<void*>(&DwcGetCommonFallbackFonts))) {
        g_original = nullptr;
        return;
    }
    Report("libxul: GetCommonFallbackFonts %#lx replaced through its vtable "
           "slot at %p (%s)", function, static_cast<void*>(slot), path);

    // Independent of the above, since one refusing says nothing about the
    // other.
    PatchUnderline(image, starts);
    PatchGlyphPath(image, starts);
    PatchGlyphBounds(image, starts);
    PatchGlyphVOrigin(image, starts);
    PatchWebRenderSize(image, starts);
    PatchPlatformMediaFeature(image, starts);
    PatchPostShapingFixup(image, starts);
}

// What the callback brings back. dlpi_name points into the link map and stays
// valid, so only these four scalars need carrying.
struct FoundLibxul
{
    const char* name = nullptr;
    uintptr_t base = 0;
    const ElfW(Phdr)* phdr = nullptr;
    ElfW(Half) phnum = 0;
    bool found = false;
};

// Records and stops, because dl_iterate_phdr holds the loader's list lock for
// the duration. dlsym(RTLD_DEFAULT) can take the loader's other lock and
// dlopen takes the two in the opposite order, so anything that resolves a
// symbol from in here can deadlock against a thread calling dlopen.
//
// The signature is dl_iterate_phdr's, so the pointer stays as it is: making
// the pointee const would make this a different callback type.
// ReSharper disable once CppParameterMayBeConstPtrOrRef
int LookForLibxul(dl_phdr_info* info, size_t, void* out)
{
    if (!IsLibxul(info->dlpi_name)) {
        return 0;
    }
    auto* found = static_cast<FoundLibxul*>(out);
    found->name = info->dlpi_name;
    found->base = info->dlpi_addr;
    found->phdr = info->dlpi_phdr;
    found->phnum = info->dlpi_phnum;
    found->found = true;
    return 1;
}

void ScanLoadedImages(const bool expected)
{
    FoundLibxul found;
    dl_iterate_phdr(LookForLibxul, &found);
    if (!found.found) {
        if (expected) {
            // The handle answered for XRE_GetBootstrap, so libxul is in this
            // process under a name the link-map scan does not recognize. Said
            // out loud, because the whole Firefox side is about to do nothing
            // and the reason would otherwise be invisible.
            Report("libxul is loaded but not named libxul.so in the link map; "
                   "the Firefox patches will not be applied");
        }
        return;
    }
    // Out of the callback, so the loader's lock is no longer held.
    //
    // Said before Apply, and whatever Apply then decides: that libxul is here
    // is what the rest of the shim keys its Firefox behavior on, and it is
    // still true if this particular patch refuses or is switched off.
    dwcft::NoteGeckoLoaded();
    // Nothing here is gated per call - these patches replace Gecko's own
    // metrics and fallback through its vtable, and once installed they apply
    // to every line Gecko lays out. So the switches have to be honored before
    // anything is installed: CLEARTYPE=0 means this library does nothing, and
    // that has to include not moving anyone's baselines.
    if (dwcft::ParityActive()) {
        Apply(found.name, found.base, found.phdr, found.phnum);
    }
}

__attribute__((constructor)) void AtLoad()
{
    // Only for the case where libxul is already mapped. The firefox binary has
    // no DT_NEEDED on it and dlopens it after this runs, which the interposed
    // dlopen below catches. Nothing is expected here, so a miss says nothing.
    ScanLoadedImages(/*expected=*/false);
}

}  // namespace

// Whether a dlopen just brought in libxul, asked of the object rather than of
// the string the caller passed. libxul exports two dynamic symbols and
// XRE_GetBootstrap is one of them, so a handle that answers for it is libxul
// whatever the file was called - and a handle that does not is not, since
// dlsym on a handle searches that object and its dependencies rather than the
// global scope. dlopen(NULL) is the one handle that does see the global scope,
// and there the answer is still right: libxul really is loaded.
//
// Safe to call from the interposer below, which runs after the real dlopen has
// returned and released the loader's locks. LookForLibxul, which runs inside
// dl_iterate_phdr with one of them held, cannot do this; see IsLibxul.
namespace
{

bool HandleIsLibxul(void* handle)
{
    if (handle == nullptr) {
        return false;
    }
    const bool is_libxul = dlsym(handle, "XRE_GetBootstrap") != nullptr;
    // A miss leaves an error behind that the caller would read back as its own
    // dlopen's.
    (void)dlerror();
    return is_libxul;
}

}  // namespace

// dlopen is interposed for one reason: to be told when libxul arrives. The
// real call is made first and its result handed back untouched, so a process
// that never loads libxul cannot tell this is here.
extern "C" __attribute__((visibility("default")))
void* dlopen(const char* file, const int mode)
{
    // Never remembered as null. dlsym can be asked before the loader is in a
    // state to answer, and a cached null here would make every dlopen in the
    // process return null for the rest of its life.
    static std::atomic<void* (*)(const char*, int)> resolved{nullptr};
    auto real = resolved.load(std::memory_order_acquire);
    if (real == nullptr) {
        real = reinterpret_cast<void* (*)(const char*, int)>(dlsym(RTLD_NEXT, "dlopen"));
        if (real == nullptr) {
            return nullptr;
        }
        resolved.store(real, std::memory_order_release);
    }
    void* handle = real(file, mode);
    if (handle != nullptr && !g_done.load() && HandleIsLibxul(handle)) {
        ScanLoadedImages(/*expected=*/true);
        // It resolves symbols on the way, and a failed dlsym leaves an error
        // the caller would read back as its own dlopen's.
        (void)dlerror();
    }
    return handle;
}

#endif  // CLEARTYPE_FIREFOX_PARITY
