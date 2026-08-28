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
//  Values are rewritten in the resource bundle as it is mapped, and the one
//  table row a Linux build can spare is repointed at a script Windows covers.
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

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include "generic_families.h"
#include "parity_gate.h"

namespace {

// The Windows value beside the one this platform ships, from
// locale_settings_win.grd and locale_settings_linux.grd.
//
// The Noto families that open every Windows list are left out. This library
// hides them from fontconfig, so FirstAvailableOrFirst never settles on one,
// and the remainder fits the space the platform value occupies.
//
// pref names which row reads the value, for the table patch below.
struct Substitution
{
    const char* platform;
    const char* windows;
    const char* pref;
};

constexpr Substitution kSubstitutions[] = {
    {"Latin Modern Math", ",Cambria Math", nullptr},
    // Windows swaps the fixed family for IDS_FIXED_FONT_FAMILY_ALT_WIN when the
    // shipped one is Courier and ClearType smoothing is on (font_defaults.cc).
    {"Monospace", ",Consolas", nullptr},
    {"Noto Sans Devanagari", ",Nirmala UI", nullptr},
    {"Noto Serif Devanagari", ",Nirmala UI", nullptr},

    {",Noto Sans JP,Noto Sans CJK JP,Arial", ",Meiryo,Yu Gothic",
     "webkit.webprefs.fonts.sansserif.Jpan"},
    {",Noto Sans JP,Noto Sans CJK JP,Times New Roman", ",Meiryo,Yu Gothic",
     "webkit.webprefs.fonts.standard.Jpan"},
    {",Noto Serif JP,Noto Serif CJK JP,Times New Roman", ",Yu Mincho,MS PMincho",
     "webkit.webprefs.fonts.serif.Jpan"},

    {",Noto Sans KR,Noto Sans CJK KR,Arial", ",Malgun Gothic",
     "webkit.webprefs.fonts.sansserif.Hang"},
    {",Noto Sans KR,Noto Sans CJK KR,Times New Roman", ",Malgun Gothic",
     "webkit.webprefs.fonts.standard.Hang"},
    {",Noto Serif KR,Noto Serif CJK KR,Times New Roman", ",Batang",
     "webkit.webprefs.fonts.serif.Hang"},

    // Arabic has no sans-serif family on Linux to rewrite, so the Japanese
    // fixed one carries Segoe UI for the row below.
    {"Noto Sans Mono CJK JP", ",Segoe UI", "webkit.webprefs.fonts.sansserif.Arab"},

    {",Noto Sans SC,Noto Sans CJK SC,Arial", ",Microsoft YaHei",
     "webkit.webprefs.fonts.sansserif.Hans"},
    {",Noto Sans SC,Noto Sans CJK SC,Times New Roman", ",Microsoft YaHei",
     "webkit.webprefs.fonts.standard.Hans"},
    {",Noto Serif SC,Noto Serif CJK SC,Times New Roman", ",Simsun",
     "webkit.webprefs.fonts.serif.Hans"},

    {",Noto Sans TC,Noto Sans CJK TC,Arial", ",Microsoft JhengHei",
     "webkit.webprefs.fonts.sansserif.Hant"},
    {",Noto Sans TC,Noto Sans CJK TC,Times New Roman", ",Microsoft JhengHei",
     "webkit.webprefs.fonts.standard.Hant"},
    {",Noto Serif TC,Noto Serif CJK TC,Times New Roman", ",PMingLiU",
     "webkit.webprefs.fonts.serif.Hant"},
};

// Which resource carries each per-script list, learned while patching.
struct Learned
{
    const char* pref;
    uint16_t resource;
};

constexpr unsigned kLearnedMax = sizeof(kSubstitutions) / sizeof(kSubstitutions[0]);
Learned g_learned[kLearnedMax];
unsigned g_learned_count = 0;

// ui/base/resource/data_pack.cc: uint32 version, uint8 encoding, three bytes
// of padding, uint16 resource count, uint16 alias count. Then one six-byte
// entry per resource plus a sentinel, each a uint16 id and a uint32 offset,
// so an entry's value ends where the next one starts.
constexpr uint32_t kFileFormatV5 = 5;
constexpr size_t kHeaderLengthV5 = 12;
constexpr size_t kEntrySize = 6;

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

}  // namespace

namespace generic_families {

unsigned PatchBundle(void* base, const size_t length)
{
    auto* bytes = static_cast<unsigned char*>(base);
    if (length < kHeaderLengthV5 || ReadU32(bytes) != kFileFormatV5) {
        return 0;
    }
    const size_t count = ReadU16(bytes + 8);
    if (length < kHeaderLengthV5 + (count + 1) * kEntrySize) {
        return 0;
    }

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
            if (s.pref != nullptr && g_learned_count < kLearnedMax) {
                g_learned[g_learned_count++] = {s.pref, ReadU16(entry)};
            }
            ++patched;
            break;
        }
    }
    return patched;
}

}  // namespace generic_families

namespace {

// Which entries kFontDefaults has is also decided at build time, and a Linux
// build has only the seven script-less ones (font_defaults.cc). Rows are
// repointed at scripts Windows covers when losing them costs nothing.
//
// font_defaults.cc reads a row through FamilyMapByName, which lists cursive,
// fixed, sansserif, serif and standard, so the fantasy and math rows are read
// by nothing and are free.
//
// standard, serif and sansserif carry a Zyyy value the WebPreferences
// constructor already holds, so those rows go too, but only once
// kFontFamilyMap stops blanking the pref, which is safe on a host with no
// pref registry. Cursive goes the same way: the constructor says Script where
// Windows says Comic Sans MS, and fontconfig.cpp renames the one to the other
// when it arrives as a family.
struct Spare
{
    const char* pref;
    bool blanked;      // whether kFontFamilyMap lists it
};

constexpr Spare kSpares[] = {
    {"webkit.webprefs.fonts.fantasy.Zyyy", false},
    {"webkit.webprefs.fonts.math.Zyyy", false},
    {"webkit.webprefs.fonts.standard.Zyyy", true},
    {"webkit.webprefs.fonts.serif.Zyyy", true},
    {"webkit.webprefs.fonts.sansserif.Zyyy", true},
    {"webkit.webprefs.fonts.cursive.Zyyy", true},
};

// Chrome registers every kFontFamilyMap name in a pref registry and reads them
// back, so a name this stops the second loop blanking is a name Chrome never
// registers, and the first read of it ends the process. Electron builds
// WebPreferences straight from the table and has no registry. Browser prefs
// only a full PrefService carries tell the two apart.
constexpr const char* kBrowserPrefs[] = {
    "bookmark_bar.show_on_all_tabs",
    "browser.show_home_button",
};

// The row the array is found by, and what a blanked pref name is pointed at.
// Its family is not one FamilyMapByName carries, so the second loop skips it.
constexpr const char* kInertPref = "webkit.webprefs.fonts.fantasy.Zyyy";

constexpr const char* kPrefPrefix = "webkit.webprefs.fonts.";
constexpr size_t kRowSize = 16;   // const char* then int, padded

// The rows to hand the spares to, highest value first.
constexpr const char* kWanted[] = {
    "webkit.webprefs.fonts.sansserif.Hans",
    "webkit.webprefs.fonts.sansserif.Arab",
    "webkit.webprefs.fonts.sansserif.Hang",
    "webkit.webprefs.fonts.sansserif.Jpan",
    "webkit.webprefs.fonts.standard.Jpan",
    "webkit.webprefs.fonts.serif.Hans",
};

struct Executable
{
    const unsigned char* begin;
    const unsigned char* end;
};

Executable g_exe{};

// Only the segments that can hold strings and tables. Searching the code as
// well costs more than the browser's startup can absorb.
struct Segment
{
    const unsigned char* begin;
    const unsigned char* end;
};

constexpr unsigned kMaxSegments = 8;
Segment g_data[kMaxSegments];
unsigned g_data_count = 0;

// Both tables hold relocated pointers, so both live in the relro span. Writes
// are kept inside it: a page outside is one the process may still write, and
// closing it again read-only would fault the next time it does.
Segment g_relro{};

bool InRelro(const void* p)
{
    const auto* at = static_cast<const unsigned char*>(p);
    return g_relro.begin != nullptr && at >= g_relro.begin && at < g_relro.end;
}

int NoteExecutable(dl_phdr_info* info, size_t, void* data)
{
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
        return 0;
    }
    auto* image = static_cast<Executable*>(data);
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
        if ((header.p_flags & PF_X) == 0 && g_data_count < kMaxSegments) {
            g_data[g_data_count++] = {from, to};
        }
    }
    return 1;
}

// At load, because dl_iterate_phdr takes the loader's lock and the table patch
// below runs from inside mmap.
__attribute__((constructor)) void NoteImageAtLoad()
{
    dl_iterate_phdr(NoteExecutable, &g_exe);
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

const unsigned char* FindInData(const void* what, const size_t length)
{
    for (unsigned i = 0; i < g_data_count; ++i) {
        if (const unsigned char* at = FindBytes(g_data[i].begin, g_data[i].end,
                                                what, length)) {
            return at;
        }
    }
    return nullptr;
}

bool InExecutable(const void* p)
{
    const auto* at = static_cast<const unsigned char*>(p);
    return at >= g_exe.begin && at < g_exe.end;
}

bool WriteRow(unsigned char* row, const char* pref, const uint16_t resource)
{
    auto* page = reinterpret_cast<unsigned char*>(
        reinterpret_cast<uintptr_t>(row) & ~static_cast<uintptr_t>(0xFFF));
    const size_t span = static_cast<size_t>(row + kRowSize - page);
    if (mprotect(page, span, PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    std::memcpy(row, &pref, sizeof(pref));
    const int id = resource;
    std::memcpy(row + sizeof(pref), &id, sizeof(id));
    (void)mprotect(page, span, PROT_READ);
    return true;
}

bool WritePointer(unsigned char* at, const char* value)
{
    auto* page = reinterpret_cast<unsigned char*>(
        reinterpret_cast<uintptr_t>(at) & ~static_cast<uintptr_t>(0xFFF));
    const size_t span = static_cast<size_t>(at + sizeof(value) - page);
    if (mprotect(page, span, PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    std::memcpy(at, &value, sizeof(value));
    (void)mprotect(page, span, PROT_READ);
    return true;
}

// Whether a slot holds a pointer to a pref name.
bool HoldsPrefName(const unsigned char* at)
{
    if (!InExecutable(at) || !InExecutable(at + sizeof(void*) - 1)) {
        return false;
    }
    const char* name = nullptr;
    std::memcpy(&name, at, sizeof(name));
    return InExecutable(name) &&
           std::strncmp(name, kPrefPrefix, std::strlen(kPrefPrefix)) == 0;
}

// Whether a slot sits in kFontFamilyMap. That array is one pref name per
// script per family, so a slot of it lies in a long run of them; the other
// pref-name arrays in the image are far shorter, and a kFontDefaults row is
// not a run at all, since it carries a resource id on either side of its name.
// kFontFamilyMap is grouped by family, one entry per script, so a slot of it
// has a neighbour naming the same family and a different script. The other
// pref-name arrays in the image mix families, and a kFontDefaults row has a
// resource id on either side of its name rather than a name at all.
bool SharesFamily(const unsigned char* at, const char* pref, const size_t family)
{
    if (!HoldsPrefName(at)) {
        return false;
    }
    const char* held = nullptr;
    std::memcpy(&held, at, sizeof(held));
    return std::strncmp(held, pref, family) == 0 && std::strcmp(held, pref) != 0;
}

bool IsMapSlot(const unsigned char* at, const char* pref)
{
    const char* dot = std::strrchr(pref, '.');
    if (dot == nullptr) {
        return false;
    }
    const size_t family = static_cast<size_t>(dot - pref) + 1;
    return SharesFamily(at - sizeof(void*), pref, family) ||
           SharesFamily(at + sizeof(void*), pref, family);
}

// Whether a row holds a pointer to a pref name.
bool IsRow(const unsigned char* at)
{
    if (!InRelro(at) || !InExecutable(at + kRowSize - 1)) {
        return false;
    }
    const char* name = nullptr;
    std::memcpy(&name, at, sizeof(name));
    if (!InExecutable(name) ||
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

// The second loop in font_defaults.cc's MakeDefaultFontCopier writes an empty
// family for every pref kFontFamilyMap lists that no row set, which would wipe
// the constructor value the row being given away was carrying. Pointing that
// slot at a family FamilyMapByName does not carry leaves the value in place.
unsigned NeutralizeMapEntry(const char* pref, const char* inert,
                            const unsigned char* first, const unsigned char* last)
{
    unsigned written = 0;
    // By content, not by address. kFontFamilyMap builds its names by pasting
    // literals together, so they are their own objects and not the ones
    // pref_names.h holds and kFontDefaults points at.
    for (unsigned i = 0; i < g_data_count; ++i) {
        for (const unsigned char* at = g_data[i].begin;
             at + sizeof(void*) <= g_data[i].end; at += sizeof(void*)) {
            if (at >= first && at <= last) {
                continue;
            }
            const char* held = nullptr;
            std::memcpy(&held, at, sizeof(held));
            if (InRelro(at) && HoldsPrefName(at) && std::strcmp(held, pref) == 0 &&
                IsMapSlot(at, pref) && WritePointer(const_cast<unsigned char*>(at), inert)) {
                ++written;
            }
        }
    }
    return written;
}

// Hands the spare rows to the scripts Windows covers that this build does not.
// Silent when the table cannot be recognized.
void PatchFontDefaults()
{
    static bool done = false;
    if (done || g_exe.begin == nullptr) {
        return;
    }
    done = true;
    if (!IsBrowserProcess()) {
        return;
    }

    const unsigned char* anchor = FindInData(kInertPref, std::strlen(kInertPref) + 1);
    if (anchor == nullptr) {
        return;
    }
    const unsigned char* row = FindInData(&anchor, sizeof(anchor));
    // A row of kFontDefaults, not a bare pointer in some other table.
    if (row == nullptr || !IsRow(row) || !IsRow(row - kRowSize) ||
        !IsRow(row + kRowSize)) {
        return;
    }
    const unsigned char* first = row;
    while (IsRow(first - kRowSize)) {
        first -= kRowSize;
    }
    const unsigned char* last = row;
    while (IsRow(last + kRowSize)) {
        last += kRowSize;
    }
    bool registry = false;
    for (const char* pref : kBrowserPrefs) {
        registry = registry || FindInData(pref, std::strlen(pref) + 1) != nullptr;
    }

    const auto* inert = reinterpret_cast<const char*>(
        FindInData(kInertPref, std::strlen(kInertPref) + 1));

    unsigned next_spare = 0;
    for (const char* wanted : kWanted) {
        const unsigned char* name = FindInData(wanted, std::strlen(wanted) + 1);
        if (name == nullptr) {
            continue;
        }
        // Only rows count. The same string is listed in kFontFamilyMap, whose
        // entries are bare pointers, so a search of the whole image would
        // always report the row as present.
        bool present = false;
        for (const unsigned char* at = first; at <= last && !present; at += kRowSize) {
            const char* held = nullptr;
            std::memcpy(&held, at, sizeof(held));
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
        if (resource == 0) {
            continue;
        }
        // The next spare row still holding its own pref name, and still safe
        // to give away.
        unsigned char* give = nullptr;
        while (next_spare < sizeof(kSpares) / sizeof(kSpares[0]) && give == nullptr) {
            const Spare& candidate = kSpares[next_spare++];
            unsigned char* found = nullptr;
            for (const unsigned char* at = first; at <= last && found == nullptr;
                 at += kRowSize) {
                const char* held = nullptr;
                std::memcpy(&held, at, sizeof(held));
                if (held != nullptr && std::strcmp(held, candidate.pref) == 0) {
                    found = const_cast<unsigned char*>(at);
                }
            }
            if (found == nullptr) {
                continue;
            }
            // While the name is still in the row to be found by. A row whose
            // pref would go on being blanked stays where it is: losing its
            // value outright is worse than not covering the script.
            if (candidate.blanked &&
                (registry || inert == nullptr ||
                 NeutralizeMapEntry(candidate.pref, inert, first, last) == 0)) {
                continue;
            }
            give = found;
        }
        if (give == nullptr) {
            break;
        }
        (void)WriteRow(give, reinterpret_cast<const char*>(name), resource);
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
