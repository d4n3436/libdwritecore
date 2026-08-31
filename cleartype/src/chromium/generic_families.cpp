//+--------------------------------------------------------------------------
//
//  generic_families.cpp - the per-script generic families, as Windows has them.
//
//  Blink resolves serif, sans-serif and the rest per script, in
//  GenericFontFamilySettings::GenericFontFamilyForScript. The maps are filled
//  by chrome/browser/ui/prefs/prefs_tab_helper.cc, and in Electron by its copy
//  in shell/browser/font_defaults.cc, from
//  chrome/app/resources/locale_settings_<platform>.grd. Entries and values are
//  both chosen at build time, so a Linux build answers Japanese sans-serif
//  with Arial where a Windows one answers Yu Gothic.
//
//  Values are rewritten in the resource bundle as it is mapped, and the table
//  of rows is relocated and extended so scripts this platform leaves out get
//  the rows Windows has.
//
//  fontconfig cannot answer this. A family reaches it already resolved to
//  "Arial", which is what an author naming Arial sends too.
//
//----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include "parity_gate.h"

namespace {

// The Windows value beside the one this platform ships, from
// locale_settings_win.grd and locale_settings_linux.grd.
//
// The Noto families that open every Windows list are left out. This library
// hides them from fontconfig, so FirstAvailableOrFirst never settles on one,
// and the remainder fits the space the platform value occupies.
//
// prefs names the rows that read the value, for the table patch below. One
// value can serve several rows, since Windows gives its per-script fixed rows
// the same family.
constexpr unsigned kMaxPrefsPerValue = 3;

struct Substitution
{
    const char* platform;
    const char* windows;
    const char* prefs[kMaxPrefsPerValue];
};

constexpr Substitution kSubstitutions[] = {
    {"Latin Modern Math", ",Cambria Math", {}},
    // Windows swaps the fixed family for IDS_FIXED_FONT_FAMILY_ALT_WIN when the
    // shipped one is Courier and ClearType smoothing is on (font_defaults.cc).
    {"Monospace", ",Consolas", {}},
    {"Noto Sans Devanagari", ",Nirmala UI", {}},
    // The fixed rows Windows compiles for Arabic, Cyrillic and Greek all carry
    // Courier New, so one rewritten resource answers for all three. Blink
    // keys the map on the element's locale, so Latin text inside such an
    // element takes it as well.
    {"Noto Serif Devanagari", ",Courier New",
     {"webkit.webprefs.fonts.fixed.Arab", "webkit.webprefs.fonts.fixed.Cyrl",
      "webkit.webprefs.fonts.fixed.Grek"}},

    // Windows gives standard and sansserif the same value for every CJK
    // script, so one rewritten resource seats both rows and leaves the other
    // resource free for a row that had none.
    {",Noto Sans JP,Noto Sans CJK JP,Arial", ",Meiryo,Yu Gothic",
     {"webkit.webprefs.fonts.sansserif.Jpan",
      "webkit.webprefs.fonts.standard.Jpan"}},
    // The standard row's resource carries the fixed family instead. Only one
    // of the two fits, and the fixed family is the one whose absence shows.
    {",Noto Sans JP,Noto Sans CJK JP,Times New Roman", ",BIZ UDGothic,MS Gothic",
     {"webkit.webprefs.fonts.fixed.Jpan"}},
    // The serif families Windows names for the CJK scripts ship with neither
    // platform, so the whole list misses there and the script's standard
    // family answers. This platform would fall through to fontconfig's serif,
    // so the standard family is written as the last entry to land on the same
    // face. The Noto entries are dropped for the same reason. What mirrors
    // Windows is the value that resolves to the face Windows resolves to, not
    // the string it stores.
    {",Noto Serif JP,Noto Serif CJK JP,Times New Roman",
     ",Yu Mincho,MS PMincho,Yu Gothic",
     {"webkit.webprefs.fonts.serif.Jpan"}},

    // The fixed row joins them. Windows names Gulimche, which ships with
    // neither platform, so that row lands on the standard family too.
    {",Noto Sans KR,Noto Sans CJK KR,Arial", ",Malgun Gothic",
     {"webkit.webprefs.fonts.sansserif.Hang",
      "webkit.webprefs.fonts.standard.Hang",
      "webkit.webprefs.fonts.fixed.Hang"}},
    {",Noto Sans KR,Noto Sans CJK KR,Times New Roman", ",Gungsuh",
     {"webkit.webprefs.fonts.cursive.Hang"}},
    {",Noto Serif KR,Noto Serif CJK KR,Times New Roman",
     ",Batang,Malgun Gothic",
     {"webkit.webprefs.fonts.serif.Hang"}},

    // Arabic has no sans-serif family on Linux to rewrite, so the Japanese
    // fixed one carries Segoe UI for the row below.
    {"Noto Sans Mono CJK JP", ",Segoe UI", {"webkit.webprefs.fonts.sansserif.Arab"}},

    {",Noto Sans SC,Noto Sans CJK SC,Arial", ",Microsoft YaHei",
     {"webkit.webprefs.fonts.sansserif.Hans",
      "webkit.webprefs.fonts.standard.Hans"}},
    {",Noto Sans SC,Noto Sans CJK SC,Times New Roman", ",KaiTi",
     {"webkit.webprefs.fonts.cursive.Hans"}},
    {",Noto Serif SC,Noto Serif CJK SC,Times New Roman", ",Simsun",
     {"webkit.webprefs.fonts.serif.Hans"}},

    // Windows differentiates the fixed family per script too;
    // IDS_FIXED_FONT_FAMILY_SIMPLIFIED_HAN is NSimsun, and Blink keys the map
    // on the element's locale, so Latin text inside a zh element takes it as
    // well. The Devanagari fixed resource carries it, since no table row
    // reads that one. A list, because a plain name is dropped between the
    // table and the renderer while the list form resolves to the same face.
    {"Noto Sans Mono", ",NSimsun", {"webkit.webprefs.fonts.fixed.Hans"}},

    // Same for MingLiU, which Windows names for the Traditional Han fixed row.
    {",Noto Sans TC,Noto Sans CJK TC,Arial", ",Microsoft JhengHei",
     {"webkit.webprefs.fonts.sansserif.Hant",
      "webkit.webprefs.fonts.standard.Hant",
      "webkit.webprefs.fonts.fixed.Hant"}},
    {",Noto Sans TC,Noto Sans CJK TC,Times New Roman", ",DFKai-SB",
     {"webkit.webprefs.fonts.cursive.Hant"}},
    {",Noto Serif TC,Noto Serif CJK TC,Times New Roman",
     ",PMingLiU,Microsoft JhengHei",
     {"webkit.webprefs.fonts.serif.Hant"}},
};

// A value no resource in the bundle can carry, added as a resource of its own.
// A Chromium build's Courier New and NSimsun ride in the Devanagari families,
// which CEF's locale pack leaves out, so on CEF there is nothing to rewrite.
struct Minted
{
    const char* windows;
    const char* prefs[kMaxPrefsPerValue];
};

constexpr Minted kMinted[] = {
    {",Courier New",
     {"webkit.webprefs.fonts.fixed.Arab", "webkit.webprefs.fonts.fixed.Cyrl",
      "webkit.webprefs.fonts.fixed.Grek"}},
    {",NSimsun", {"webkit.webprefs.fonts.fixed.Hans"}},
};

// Which resource carries each per-script list, learned while patching.
struct Learned
{
    const char* pref;
    uint16_t resource;
};

constexpr unsigned kLearnedMax =
    sizeof(kSubstitutions) / sizeof(kSubstitutions[0]) * kMaxPrefsPerValue;
Learned g_learned[kLearnedMax];
unsigned g_learned_count = 0;

// ui/base/resource/data_pack.cc: uint32 version, uint8 encoding, three bytes
// of padding, uint16 resource count, uint16 alias count. Then one six-byte
// entry per resource plus a sentinel, each a uint16 id and a uint32 offset,
// so an entry's value ends where the next one starts.
constexpr uint32_t kFileFormatV5 = 5;
constexpr size_t kHeaderLengthV5 = 12;
constexpr size_t kEntrySize = 6;
// Then one four-byte alias per alias count, a uint16 id and a uint16 index
// into the table above.
constexpr size_t kAliasSize = 4;

uint16_t ReadU16(const unsigned char* at)
{
    uint16_t v = 0;
    std::memcpy(&v, at, sizeof(v));
    return v;
}

uint32_t ReadU32(const unsigned char* at)
{
    uint32_t v = 0;
    std::memcpy(&v, at, sizeof(v));
    return v;
}

void WriteU16(unsigned char* at, const uint16_t v)
{
    std::memcpy(at, &v, sizeof(v));
}

void WriteU32(unsigned char* at, const uint32_t v)
{
    std::memcpy(at, &v, sizeof(v));
}

bool AlreadyLearned(const char* pref)
{
    for (unsigned i = 0; i < g_learned_count; ++i) {
        if (std::strcmp(g_learned[i].pref, pref) == 0) {
            return true;
        }
    }
    return false;
}

// How much shorter a substituted value is than the one it replaced, so the
// bundle can be laid out again without the padding.
struct Shrunk
{
    size_t entry;
    uint32_t size;
};

constexpr unsigned kShrunkMax = sizeof(kSubstitutions) / sizeof(kSubstitutions[0]);

// Appends a resource for every minted value the bundle carries none of, and
// lays the whole bundle out again to make room. The ids are above every one
// the bundle holds, so both tables stay sorted and no alias index moves; the
// substituted values give back more than the longer table costs.
void Mint(unsigned char* bytes, const size_t length, const size_t count,
          const size_t aliases, const Shrunk* shrunk, const unsigned shrunk_count)
{
    const Minted* wanted[sizeof(kMinted) / sizeof(kMinted[0])];
    unsigned want = 0;
    for (const Minted& m : kMinted) {
        if (m.prefs[0] != nullptr && !AlreadyLearned(m.prefs[0])) {
            wanted[want++] = &m;
        }
    }
    if (want == 0) {
        return;
    }

    unsigned char* const table = bytes + kHeaderLengthV5;
    const size_t grown = kHeaderLengthV5 + (count + 1 + want) * kEntrySize +
                         aliases * kAliasSize;
    size_t total = grown;
    uint16_t highest = 0;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* entry = table + i * kEntrySize;
        highest = ReadU16(entry) > highest ? ReadU16(entry) : highest;
        uint32_t size = ReadU32(entry + kEntrySize + 2) - ReadU32(entry + 2);
        for (unsigned j = 0; j < shrunk_count; ++j) {
            if (shrunk[j].entry == i) {
                size = shrunk[j].size;
            }
        }
        total += size;
    }
    for (size_t i = 0; i < aliases; ++i) {
        const unsigned char* alias = table + (count + 1) * kEntrySize + i * kAliasSize;
        highest = ReadU16(alias) > highest ? ReadU16(alias) : highest;
    }
    for (unsigned i = 0; i < want; ++i) {
        total += std::strlen(wanted[i]->windows);
    }
    if (total > length || highest > 0xFFFF - want) {
        return;
    }

    // Built beside the mapping, since the data moves both ways: the table
    // grows and the values shrink.
    auto* built = static_cast<unsigned char*>(std::malloc(total));
    if (built == nullptr) {
        return;
    }
    std::memcpy(built, bytes, kHeaderLengthV5);
    WriteU16(built + 8, static_cast<uint16_t>(count + want));
    unsigned char* out = built + kHeaderLengthV5;
    uint32_t at = static_cast<uint32_t>(grown);
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* entry = table + i * kEntrySize;
        const uint32_t from = ReadU32(entry + 2);
        uint32_t size = ReadU32(entry + kEntrySize + 2) - from;
        for (unsigned j = 0; j < shrunk_count; ++j) {
            if (shrunk[j].entry == i) {
                size = shrunk[j].size;
            }
        }
        WriteU16(out + i * kEntrySize, ReadU16(entry));
        WriteU32(out + i * kEntrySize + 2, at);
        std::memcpy(built + at, bytes + from, size);
        at += size;
    }
    for (unsigned i = 0; i < want; ++i) {
        const size_t size = std::strlen(wanted[i]->windows);
        const auto id = static_cast<uint16_t>(highest + 1 + i);
        WriteU16(out + (count + i) * kEntrySize, id);
        WriteU32(out + (count + i) * kEntrySize + 2, at);
        std::memcpy(built + at, wanted[i]->windows, size);
        at += static_cast<uint32_t>(size);
        for (const char* pref : wanted[i]->prefs) {
            if (pref != nullptr && g_learned_count < kLearnedMax) {
                g_learned[g_learned_count++] = {pref, id};
            }
        }
        if (std::getenv("DWC_GENERIC_LOG") != nullptr) {
            (void)std::fprintf(stderr,
                               "chromium-patch: generic families: resource %u "
                               "minted as %s (%s)\n",
                               id, wanted[i]->windows, wanted[i]->prefs[0]);
        }
    }
    // The sentinel keeps its id and holds the end of the data.
    WriteU16(out + (count + want) * kEntrySize, ReadU16(table + count * kEntrySize));
    WriteU32(out + (count + want) * kEntrySize + 2, at);
    std::memcpy(out + (count + 1 + want) * kEntrySize,
                table + (count + 1) * kEntrySize, aliases * kAliasSize);

    std::memcpy(bytes, built, total);
    std::free(built);
}

}  // namespace

namespace generic_families {

// Rewrites the generic font family values inside one mapped resource bundle,
// replacing what this platform ships with what Windows ships. Returns how many
// were replaced.
unsigned PatchBundle(void* base, const size_t length)
{
    auto* bytes = static_cast<unsigned char*>(base);
    if (length < kHeaderLengthV5 || ReadU32(bytes) != kFileFormatV5) {
        return 0;
    }
    const size_t count = ReadU16(bytes + 8);
    const size_t aliases = ReadU16(bytes + 10);
    if (length < kHeaderLengthV5 + (count + 1) * kEntrySize + aliases * kAliasSize) {
        return 0;
    }

    Shrunk shrunk[kShrunkMax];
    unsigned shrunk_count = 0;
    unsigned patched = 0;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* entry = bytes + kHeaderLengthV5 + i * kEntrySize;
        const uint32_t start = ReadU32(entry + 2);
        const uint32_t end = ReadU32(entry + kEntrySize + 2);
        if (end <= start || end > length) {
            continue;
        }
        // Whole values only. They sit end to end, so a search would find
        // "Noto Sans Mono" inside "Noto Sans Mono CJK JP".
        const size_t size = end - start;
        for (const Substitution& s : kSubstitutions) {
            if (std::strlen(s.platform) != size ||
                std::memcmp(bytes + start, s.platform, size) != 0) {
                continue;
            }
            // A value that starts with a comma is a list, which Blink resolves
            // in GenericFontFamilySettings::GenericFontFamilyForScript through
            // FontCache::FirstAvailableOrFirst, so a plain name can become one.
            const size_t wanted = std::strlen(s.windows);
            if (wanted > size) {
                break;
            }
            std::memcpy(bytes + start, s.windows, wanted);
            // FontList::FirstAvailableOrFirst splits on commas and keeps the
            // non-empty pieces, so trailing commas name nothing.
            std::memset(bytes + start + wanted, ',', size - wanted);
            if (shrunk_count < kShrunkMax) {
                shrunk[shrunk_count++] = {i, static_cast<uint32_t>(wanted)};
            }
            if (std::getenv("DWC_GENERIC_LOG") != nullptr) {
                (void)std::fprintf(stderr,
                                   "chromium-patch: generic families: resource "
                                   "%u now %s (%s)\n",
                                   ReadU16(entry), s.windows,
                                   s.prefs[0] != nullptr ? s.prefs[0] : "-");
            }
            for (const char* pref : s.prefs) {
                if (pref != nullptr && g_learned_count < kLearnedMax) {
                    g_learned[g_learned_count++] = {pref, ReadU16(entry)};
                }
            }
            ++patched;
            break;
        }
    }
    if (patched > 0) {
        Mint(bytes, length, count, aliases, shrunk, shrunk_count);
    }
    return patched;
}

}  // namespace generic_families

namespace {

// Which entries kFontDefaults has is also decided at build time, and a Linux
// build has only the seven script-less ones (font_defaults.cc), so the rest
// are appended and the loop that reads the table is pointed at the longer one.
//
// The rows a Windows build compiles, in kFontDefaults order. Every one is
// covered. The families Windows names that ship with neither platform resolve
// to the script's standard family, which a resource already carries.
constexpr const char* kWindowsRows[] = {
    "webkit.webprefs.fonts.standard.Jpan",
    "webkit.webprefs.fonts.fixed.Jpan",
    "webkit.webprefs.fonts.serif.Jpan",
    "webkit.webprefs.fonts.sansserif.Jpan",
    "webkit.webprefs.fonts.standard.Hang",
    "webkit.webprefs.fonts.serif.Hang",
    "webkit.webprefs.fonts.sansserif.Hang",
    "webkit.webprefs.fonts.standard.Hans",
    "webkit.webprefs.fonts.serif.Hans",
    "webkit.webprefs.fonts.sansserif.Hans",
    "webkit.webprefs.fonts.standard.Hant",
    "webkit.webprefs.fonts.serif.Hant",
    "webkit.webprefs.fonts.sansserif.Hant",
    "webkit.webprefs.fonts.cursive.Hang",
    "webkit.webprefs.fonts.cursive.Hans",
    "webkit.webprefs.fonts.cursive.Hant",
    "webkit.webprefs.fonts.fixed.Hang",
    "webkit.webprefs.fonts.fixed.Hant",
    "webkit.webprefs.fonts.sansserif.Arab",
    "webkit.webprefs.fonts.fixed.Hans",
    "webkit.webprefs.fonts.fixed.Arab",
    "webkit.webprefs.fonts.fixed.Cyrl",
    "webkit.webprefs.fonts.fixed.Grek",
};

// The row the array is found by. Its value is one no other table holds.
constexpr const char* kAnchorPref = "webkit.webprefs.fonts.fantasy.Zyyy";

constexpr const char* kPrefPrefix = "webkit.webprefs.fonts.";
constexpr size_t kRowSize = 16;   // const char* then int, padded

struct Image
{
    const unsigned char* begin;
    const unsigned char* end;
};

Image g_image{};

// The segments that can hold strings and tables, and separately the code. A
// string search stays out of the code, which costs more than the browser's
// startup can absorb; only the loop bound is looked for there.
struct Segment
{
    const unsigned char* begin;
    const unsigned char* end;
};

constexpr unsigned kMaxSegments = 8;
Segment g_data[kMaxSegments];
unsigned g_data_count = 0;

// The code, for the one instruction that bounds the table's read loop.
Segment g_text[kMaxSegments];
unsigned g_text_count = 0;

// Both tables hold relocated pointers, so both live in the relro span. Writes
// are kept inside it: a page outside is one the process may still write, and
// closing it again read-only would fault the next time it does.
Segment g_relro{};

bool InRelro(const void* p)
{
    const auto* at = static_cast<const unsigned char*>(p);
    return g_relro.begin != nullptr && at >= g_relro.begin && at < g_relro.end;
}

// Whether this module's read-only data holds the anchor pref, which is what
// says it is the one that compiled kFontDefaults. Electron compiles it into
// the executable; CEF compiles it into libcef.so and leaves the executable a
// loader with nothing in it.
bool HoldsTable(const dl_phdr_info* info)
{
    // This library writes the anchor down too, so its own image never counts.
    static const uintptr_t self = [] {
        Dl_info me{};
        return dladdr(reinterpret_cast<const void*>(kAnchorPref), &me) != 0
                   ? reinterpret_cast<uintptr_t>(me.dli_fbase)
                   : 0;
    }();
    if (self != 0 && info->dlpi_addr == self) {
        return false;
    }
    const size_t length = std::strlen(kAnchorPref) + 1;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) != 0) {
            continue;
        }
        const auto* from = reinterpret_cast<const unsigned char*>(info->dlpi_addr +
                                                                 header.p_vaddr);
        if (::memmem(from, header.p_filesz, kAnchorPref, length) != nullptr) {
            return true;
        }
    }
    return false;
}

int NoteImage(dl_phdr_info* info, size_t, void* data)
{
    if (!HoldsTable(info)) {
        return 0;
    }
    auto* image = static_cast<Image*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD && header.p_type != PT_GNU_RELRO) {
            continue;
        }
        const auto* from = reinterpret_cast<const unsigned char*>(info->dlpi_addr +
                                                                 header.p_vaddr);
        const unsigned char* to = from + header.p_memsz;
        if (image->begin == nullptr || from < image->begin) {
            image->begin = from;
        }
        if (to > image->end) {
            image->end = to;
        }
        if (header.p_type == PT_GNU_RELRO) {
            g_relro = {from, from + header.p_memsz};
            continue;
        }
        if ((header.p_flags & PF_X) == 0) {
            if (g_data_count < kMaxSegments) {
                g_data[g_data_count++] = {from, to};
            }
        } else if (g_text_count < kMaxSegments) {
            g_text[g_text_count++] = {from, to};
        }
    }
    return 1;
}

// At load, because dl_iterate_phdr takes the loader's lock and the table patch
// below runs from inside mmap.
__attribute__((constructor)) void NoteImageAtLoad()
{
    dl_iterate_phdr(NoteImage, &g_image);
}

const unsigned char* FindBytes(const unsigned char* from, const unsigned char* to,
                               const void* what, const size_t length)
{
    if (from == nullptr || to <= from || static_cast<size_t>(to - from) < length) {
        return nullptr;
    }
    const auto* found = static_cast<const unsigned char*>(
        ::memmem(from, static_cast<size_t>(to - from), what, length));
    return found;
}

// The first occurrence at or after `from`, which is null for the first call
// and one past the last hit to walk the rest. CEF's image holds the anchor
// twice and its pointer many times, so the first hit is not always the row.
const unsigned char* FindInData(const void* what, const size_t length,
                                const unsigned char* from = nullptr)
{
    for (unsigned i = 0; i < g_data_count; ++i) {
        const unsigned char* begin = g_data[i].begin;
        if (from != nullptr) {
            if (from >= g_data[i].end) {
                continue;
            }
            if (from > begin) {
                begin = from;
            }
        }
        if (const unsigned char* at = FindBytes(begin, g_data[i].end,
                                                what, length)) {
            return at;
        }
    }
    return nullptr;
}

bool InImage(const void* p)
{
    const auto* at = static_cast<const unsigned char*>(p);
    return at >= g_image.begin && at < g_image.end;
}

// The lea that loads a bound of the table, as the loop in
// MakeDefaultFontCopier reads it:
//
//     lea  <first row>(%rip), %r13     the cursor
//     ...
//     add  $16, %r13
//     lea  <past last row>(%rip), %rax
//     cmp  %rax, %r13
//
// How far apart the two leas sit depends on how much of the loop body the
// compiler put between them, so the span is wide enough for an inlined body.
// It only narrows the search; the compare after the end lea is what separates
// the loop from an unrelated pair of leas.
constexpr unsigned char kLeaOpcode = 0x8d;
constexpr unsigned char kRipModRm = 0x05;
constexpr size_t kLeaLength = 7;
constexpr ptrdiff_t kLoopSpan = 256;

const unsigned char* LeaTarget(const unsigned char* at)
{
    if (at[0] != 0x48 && at[0] != 0x4C && at[0] != 0x49 && at[0] != 0x4D) {
        return nullptr;
    }
    if (at[1] != kLeaOpcode || (at[2] & 0xC7) != kRipModRm) {
        return nullptr;
    }
    int32_t disp = 0;
    std::memcpy(&disp, at + 3, sizeof(disp));
    return at + kLeaLength + disp;
}

bool WriteLeaTarget(unsigned char* at, const unsigned char* target)
{
    const ptrdiff_t disp = target - (at + kLeaLength);
    if (disp > INT32_MAX || disp < INT32_MIN) {
        return false;
    }
    auto* page = reinterpret_cast<unsigned char*>(
        reinterpret_cast<uintptr_t>(at) & ~static_cast<uintptr_t>(0xFFF));
    const size_t span = static_cast<size_t>(at + kLeaLength - page);
    if (mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    const auto narrowed = static_cast<int32_t>(disp);
    std::memcpy(at + 3, &narrowed, sizeof(narrowed));
    (void)mprotect(page, span, PROT_READ | PROT_EXEC);
    return true;
}

// Room for the compiled rows and every row this adds.
constexpr size_t kTableBytes =
    (sizeof(kWindowsRows) / sizeof(kWindowsRows[0]) + 16) * kRowSize;

// A page for the table, close enough to the code that a lea can reach it. The
// loop's bounds are RIP-relative with a signed 32-bit displacement, and this
// library's own data sits far outside that of the executable's text.
unsigned char* MapBeside(const unsigned char* beside)
{
    constexpr ptrdiff_t kReach = 0x7000'0000;   // inside 2 GB, with room to spare
    constexpr uintptr_t kStep = 0x10'0000;
    const auto at = reinterpret_cast<uintptr_t>(beside);
    // The kernel treats the address as a hint and places the mapping elsewhere
    // when it is taken, so each distance is tried on both sides and the
    // address that came back is checked.
    for (uintptr_t away = kStep; away < static_cast<uintptr_t>(kReach); away *= 2) {
        for (const uintptr_t hint : {at + away, at - away}) {
            void* got = mmap(reinterpret_cast<void*>(hint & ~static_cast<uintptr_t>(0xFFF)),
                             kTableBytes, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (got == MAP_FAILED) {
                continue;
            }
            if (const ptrdiff_t gap = static_cast<unsigned char*>(got) - beside;
                gap < kReach && gap > -kReach) {
                return static_cast<unsigned char*>(got);
            }
            (void)munmap(got, kTableBytes);
        }
    }
    return nullptr;
}

// Where the loop's end bound was found, so a later pack can move it again
// without searching the code twice.
unsigned char* g_loop_end = nullptr;

// Points the loop at a copy of the table. Refuses unless exactly one loop of
// that shape reads it, leaving a build that inlined the loop twice untouched
// instead of half patched.
// Whether a `cmp reg, reg` follows, which is how the loop tests its cursor
// against the end. REX prefix, 0x39, then a mod=11 modrm.
bool FollowedByCompare(const unsigned char* at)
{
    return (at[0] & 0xF8) == 0x48 && at[1] == 0x39 && (at[2] & 0xC0) == 0xC0;
}

bool PointLoopAt(const unsigned char* first, const unsigned char* end,
                 const unsigned char* table, const unsigned rows)
{
    unsigned char* begin_lea = nullptr;
    unsigned char* end_lea = nullptr;
    for (unsigned i = 0; i < g_text_count; ++i) {
        unsigned char* seen = nullptr;
        for (auto* at = const_cast<unsigned char*>(g_text[i].begin);
             at + kLeaLength <= g_text[i].end; ++at) {
            const unsigned char* target = LeaTarget(at);
            if (target == first) {
                seen = at;
                continue;
            }
            if (target != end || seen == nullptr || at - seen > kLoopSpan) {
                continue;
            }
            // The end lea is compared against the cursor immediately after
            // it. Without this test a lea of the table's first row anywhere in
            // the image pairs with an unrelated lea of its end and the write
            // lands in code that has nothing to do with the loop.
            if (!FollowedByCompare(at + kLeaLength)) {
                continue;
            }
            if (end_lea != nullptr) {
                return false;
            }
            begin_lea = seen;
            end_lea = at;
        }
    }
    if (end_lea == nullptr ||
        !WriteLeaTarget(begin_lea, table) ||
        !WriteLeaTarget(end_lea, table + rows * kRowSize)) {
        return false;
    }
    g_loop_end = end_lea;
    return true;
}

// Moves the end bound alone, for rows added after the loop was repointed.
void MoveLoopEnd(const unsigned char* end)
{
    if (g_loop_end != nullptr) {
        (void)WriteLeaTarget(g_loop_end, end);
    }
}

// Whether a row holds a pointer to a pref name.
bool IsRow(const unsigned char* at)
{
    if (!InRelro(at) || !InImage(at + kRowSize - 1)) {
        return false;
    }
    const char* name = nullptr;
    std::memcpy(&name, at, sizeof(name));
    if (!InImage(name) ||
        std::strncmp(name, kPrefPrefix, std::strlen(kPrefPrefix)) != 0) {
        return false;
    }
    // The resource id and its padding. Without this the walk runs on into
    // kFontFamilyMap, a bare pointer array where every other slot also holds
    // a pref name; there the padding is the top half of a pointer.
    const uint32_t resource = ReadU32(at + sizeof(name));
    return ReadU32(at + sizeof(name) + sizeof(resource)) == 0 && resource != 0 &&
           resource < 0x10000;
}

// SetFontDefaults runs in the browser process, which is the one started
// without a --type. Searching the executable image is far too slow to do in
// every child, and the zygote's boot handshake times out when it is.
bool IsBrowserProcess()
{
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char args[4096];
    const ssize_t n = read(fd, args, sizeof(args));
    (void)close(fd);
    for (ssize_t i = 0; i + 7 < n; ++i) {
        if (args[i] == '\0' && std::strncmp(args + i + 1, "--type=", 7) == 0) {
            return false;
        }
    }
    return n > 0;
}

void Note(const char* what, const char* which)
{
    static const bool on = std::getenv("DWC_GENERIC_LOG") != nullptr;
    if (on) {
        (void)std::fprintf(stderr, "chromium-patch: generic families: %s %s\n",
                           what, which);
    }
}

// Hands the spare rows to the scripts Windows covers that this build does not.
// Runs after every bundle patch, because the resources arrive one pack at a
// time and a row can only be written once its resource has been seen; a row
// already written is skipped. Silent when the table cannot be recognized.
//
// The compiled array cannot be written past, since another array starts
// immediately after it. A longer copy is built here instead and the loop that
// reads the table is pointed at that.
void PatchFontDefaults()
{
    if (g_image.begin == nullptr || !IsBrowserProcess()) {
        return;
    }

    // Searched once. The image is up to a gigabyte and a half on CEF, and this
    // runs again for every resource pack the browser maps.
    static const unsigned char* first = nullptr;
    static const unsigned char* end = nullptr;
    static bool searched = false;
    if (!searched) {
        searched = true;
        const size_t length = std::strlen(kAnchorPref) + 1;
        for (const unsigned char* anchor = FindInData(kAnchorPref, length);
             anchor != nullptr && first == nullptr;
             anchor = FindInData(kAnchorPref, length, anchor + 1)) {
            for (const unsigned char* row = FindInData(&anchor, sizeof(anchor));
                 row != nullptr;
                 row = FindInData(&anchor, sizeof(anchor), row + 1)) {
                // A row of kFontDefaults, not a bare pointer in some other
                // table.
                if (!IsRow(row) || !IsRow(row - kRowSize) ||
                    !IsRow(row + kRowSize)) {
                    continue;
                }
                first = row;
                while (IsRow(first - kRowSize)) {
                    first -= kRowSize;
                }
                end = row;
                while (IsRow(end + kRowSize)) {
                    end += kRowSize;
                }
                end += kRowSize;
                break;
            }
        }
    }
    if (first == nullptr) {
        return;
    }

    // The copy, kept for the process's life since the loop reads it on every
    // WebPreferences the browser builds. Filled from the compiled array once,
    // then extended as later resource packs are learned.
    static unsigned char* table = nullptr;
    static unsigned rows = 0;
    static bool pointed = false;
    if (table == nullptr) {
        const auto compiled = static_cast<size_t>(end - first);
        if (compiled == 0 || compiled > kTableBytes) {
            return;
        }
        table = MapBeside(g_image.end);
        if (table == nullptr) {
            return;
        }
        std::memcpy(table, first, compiled);
        rows = static_cast<unsigned>(compiled / kRowSize);
    }

    for (const char* wanted : kWindowsRows) {
        const unsigned char* name = FindInData(wanted, std::strlen(wanted) + 1);
        if (name == nullptr) {
            continue;
        }
        // Only rows count. The same string is listed in kFontFamilyMap, whose
        // entries are bare pointers, so a search of the whole image would
        // always report the row as present.
        bool present = false;
        for (unsigned i = 0; i < rows && !present; ++i) {
            const char* held = nullptr;
            std::memcpy(&held, table + i * kRowSize, sizeof(held));
            present = held == reinterpret_cast<const char*>(name);
        }
        if (present) {
            continue;
        }
        uint16_t resource = 0;
        for (unsigned i = 0; i < g_learned_count; ++i) {
            if (std::strcmp(g_learned[i].pref, wanted) == 0) {
                resource = g_learned[i].resource;
                break;
            }
        }
        // A row whose value no resource carries would answer with the family
        // this platform ships, which is worse than not covering the script.
        if (resource == 0 || (rows + 1) * kRowSize > kTableBytes) {
            continue;
        }
        const auto* pref = reinterpret_cast<const char*>(name);
        unsigned char* at = table + rows * kRowSize;
        std::memcpy(at, &pref, sizeof(pref));
        const int id = resource;
        std::memcpy(at + sizeof(pref), &id, sizeof(id));
        std::memset(at + sizeof(pref) + sizeof(id), 0,
                    kRowSize - sizeof(pref) - sizeof(id));
        ++rows;
        Note("row written for", wanted);
    }

    if (!pointed) {
        pointed = PointLoopAt(first, end, table, rows);
        if (!pointed) {
            Note("the loop that reads the table could not be repointed, so the "
                 "added rows go", "unread");
        }
    } else {
        MoveLoopEnd(table + rows * kRowSize);
    }

    if (std::getenv("DWC_GENERIC_LOG") != nullptr) {
        for (unsigned i = 0; i < rows; ++i) {
            const char* held = nullptr;
            std::memcpy(&held, table + i * kRowSize, sizeof(held));
            int id = 0;
            std::memcpy(&id, table + i * kRowSize + sizeof(held), sizeof(id));
            (void)std::fprintf(stderr,
                               "chromium-patch: generic families: pid %d row "
                               "%s -> %d\n",
                               getpid(), held != nullptr ? held : "?", id);
        }
    }
}

using MmapFn = void* (*)(void*, size_t, int, int, int, off_t);

bool NamesResourceBundle(const int fd)
{
    char link[64];
    (void)std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    char path[512];
    const ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (n <= 4) {
        return false;
    }
    path[n] = '\0';
    return std::strcmp(path + n - 4, ".pak") == 0;
}

// base::MemoryMappedFile asks for MAP_SHARED on a read-only descriptor, and
// the kernel refuses to make such a mapping writable afterwards. Mapping it
// private instead leaves the file alone and keeps the edit inside this
// process, which is all a resource bundle needs, since nothing writes it.
void* MapAndPatch(const MmapFn real, void* addr, const size_t length, const int prot,
                  const int flags, const int fd, const off_t offset)
{
    void* mapped = real(addr, length, prot | PROT_WRITE,
                        (flags & ~MAP_SHARED) | MAP_PRIVATE, fd, offset);
    if (mapped == MAP_FAILED) {
        return real(addr, length, prot, flags, fd, offset);
    }
    // One map at a time: PatchBundle appends to the learned list and the row
    // patch reads it, and two packs can arrive on two threads.
    static std::mutex patch_mutex;
    const std::lock_guard<std::mutex> lock(patch_mutex);
    if (generic_families::PatchBundle(mapped, length) > 0) {
        PatchFontDefaults();
    }
    (void)mprotect(mapped, length, prot);
    return mapped;
}

// Both spellings are interposed. A caller built with _FILE_OFFSET_BITS=64,
// which Chromium is, has its mmap calls redirected to the mmap64 symbol by
// glibc's header, so interposing only mmap sees none of them.
void* MmapImpl(const char* name, void* addr, const size_t length, const int prot,
               const int flags, const int fd, const off_t offset)
{
    const auto real = reinterpret_cast<MmapFn>(dlsym(RTLD_NEXT, name));
    if (real == nullptr) {
        return MAP_FAILED;
    }
    // dlsym and the parity gate both allocate, and an allocator that reaches
    // mmap would come back through here.
    static thread_local bool inside = false;
    if (inside || fd < 0 || offset != 0 || (prot & PROT_WRITE) != 0 ||
        (flags & MAP_SHARED) == 0) {
        return real(addr, length, prot, flags, fd, offset);
    }
    inside = true;
    const bool wanted = chromium_patch::ParityWanted() && NamesResourceBundle(fd);
    inside = false;
    return wanted ? MapAndPatch(real, addr, length, prot, flags, fd, offset)
                  : real(addr, length, prot, flags, fd, offset);
}

}  // namespace

extern "C" __attribute__((visibility("default")))
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return MmapImpl("mmap", addr, length, prot, flags, fd, offset);
}

extern "C" __attribute__((visibility("default")))
void* mmap64(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return MmapImpl("mmap64", addr, length, prot, flags, fd, offset);
}
