//+--------------------------------------------------------------------------
//
//  libxul_patch.cpp - the interception points outside FreeType.
//
//  Some of what Firefox on Windows does never reaches FreeType, because Gecko
//  answers it itself. Three such things are intercepted here.
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
//  The bad-underline bit itself. gfxDWriteFontList marks a family from the
//  same list; gfxFcPlatformFontList passes a literal false, so on Linux
//  gfxFontGroup::GetUnderlineOffset never takes the minimum across the group
//  the way Windows does. The bit lives in the shared font list, which the
//  parent process maps writable, so it is set there directly. See the comment
//  above MarkBadUnderlineFamiliesOnce.
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
#include <link.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>


// Declined for the same reason as in cleartype/src/freetype.cpp.
// ReSharper disable CppTooWideScopeInitStatement
// ReSharper disable CppUseStructuredBinding

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

// Reads `len` bytes from this process without dereferencing the address.
//
// Every address below comes from /proc/self/maps, which describes what was
// mapped when it was read and not what is mapped now. A shared mapping torn
// down in between leaves an address that looks fine and faults on the first
// load, which is a crash inside a host this library promises never to crash.
// process_vm_readv answers EFAULT for that instead, and needs no privilege
// against the calling process itself.
// The length is the caller's to give.
// ReSharper disable once CppDFAConstantParameter
bool ReadWithoutFaulting(const void* addr, void* out, const size_t len)
{
    const iovec local = { .iov_base = out, .iov_len = len };
    const iovec remote = { .iov_base = const_cast<void*>(addr), .iov_len = len };
    const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (got == static_cast<ssize_t>(len)) {
        return true;
    }
    if (got >= 0) {
        return false;                        // the object ends inside the range
    }
    // Gecko's seccomp policy (security/sandbox/linux/SandboxFilter.cpp) gives
    // process_vm_readv to the parent alone, so in a content process the call
    // never runs and says nothing about the mapping. The copy is made directly
    // and stops at the end of the page the address sits in, which is mapped
    // because the caller is holding an object in it.
    if (errno != EPERM && errno != ENOSYS && errno != EACCES) {
        return false;
    }
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return false;
    }
    const auto start = reinterpret_cast<uintptr_t>(addr);
    // ReSharper disable once CppRedundantParentheses
    const uintptr_t page_end = (start + static_cast<uintptr_t>(page)) &
                               ~static_cast<uintptr_t>(page - 1);
    if (len > page_end - start) {
        return false;                        // the caller halves it and asks again
    }
    std::memcpy(out, addr, len);
    return true;
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
// gfx/2d/DWriteSettings.cpp answers with its own initialisers, sGamma{2.2f} and
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
// ReSharper disable CppDFAConstantConditions
// ReSharper disable CppDFAConstantParameter
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
// ReSharper restore CppDFAConstantParameter
// ReSharper restore CppDFAConstantConditions

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
           "initialisers leave it on Windows",
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
    const double rounded = std::floor(em + 0.5);
    auto* base = static_cast<unsigned char*>(self);

    auto matches = [&](const double* c) {
        const double got = c[kMetricsEmHeight];
        return (SameDouble(got, em) || SameDouble(got, rounded)) &&
               SameDouble(c[kMetricsMaxAscent], asc) && SameDouble(c[kMetricsMaxDescent], desc) &&
               SameDouble(c[kMetricsMaxHeight], asc + desc);
    };

    // The offset of mMetrics inside gfxFT2FontBase is fixed by the build, so
    // once it is known only the struct itself is read.
    static std::atomic known{kMetricsNotFound};
    const size_t cached = known.load(std::memory_order_relaxed);
    if (cached != kMetricsNotFound) {
        double one[kMetricsFields];
        if (ReadWithoutFaulting(base + cached, one, sizeof(one)) && matches(one)) {
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

extern "C" void DwcInitMetrics(void* self)
{
    g_init_metrics(self);

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
    const bool leading = CleartypeWindowsLeading(&il, &el, &em, &asc, &desc) != 0;
    double uo = 0.0, us = 0.0, fold = 0.0;
    const bool underline = CleartypeWindowsUnderline(&uo, &us, &em, &asc, &desc, &fold) != 0;
    double ave = 0.0, adv = 0.0, cw_em = 0.0, cw_asc = 0.0, cw_desc = 0.0;
    const bool char_width = CleartypeWindowsCharWidth(&ave, &adv, &cw_em, &cw_asc, &cw_desc) != 0;
    // All three are read together and the claim is then dropped. The answers
    // belong to this call alone, and a later InitMetrics that returns before
    // reading OS/2 would otherwise be handed this face's numbers.
    CleartypeEndInitMetrics();
    if (!leading && !underline) {
        return;
    }

    double* found = FindMetrics(self, em, asc, desc);
    if (found == nullptr) {
        return;
    }

    // The pair that sizes a text control and places nothing.
    if (char_width && SameDouble(cw_em, em) && SameDouble(cw_asc, asc) &&
        SameDouble(cw_desc, desc)) {
        found[kMetricsAveCharWidth] = ave;
        found[kMetricsMaxAdvance] = adv;
    }

    if (leading) {
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
    if (leading) {
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
