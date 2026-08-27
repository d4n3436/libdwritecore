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
            // Only a list for a list. A build that does not resolve one
            // takes the whole value as a family name, so a plain name stays
            // one.
            const size_t wanted = std::strlen(s.windows);
            if (wanted > size || s.platform[0] != ',') {
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
// build has only the seven script-less ones (font_defaults.cc). A row can be
// repointed at a script Windows covers when losing it costs nothing. The
// second loop there blanks every pref name listed in kFontFamilyMap that the
// first loop did not set, wiping the WebPreferences constructor default. That
// list covers cursive, fixed, sansserif, serif and standard, so fantasy is the
// one row that can be spared, and its constructor default is already what
// Windows resolves it to.
constexpr const char* kSpareRow = "webkit.webprefs.fonts.fantasy.Zyyy";
constexpr const char* kPrefPrefix = "webkit.webprefs.fonts.";
constexpr size_t kRowSize = 16;   // const char* then int, padded

// The row to hand the spare to, highest value first.
constexpr const char* kWanted[] = {
    "webkit.webprefs.fonts.sansserif.Jpan",
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

int NoteExecutable(dl_phdr_info* info, size_t, void* data)
{
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
        return 0;
    }
    auto* image = static_cast<Executable*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD) {
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

// Whether a row holds a pointer to a pref name.
bool IsRow(const unsigned char* at)
{
    if (!InExecutable(at) || !InExecutable(at + kRowSize - 1)) {
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

// Hands the spare row to the first script Windows covers that this build does
// not. Silent when the table cannot be recognized.
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

    const unsigned char* spare = FindInData(kSpareRow, std::strlen(kSpareRow) + 1);
    if (spare == nullptr) {
        return;
    }
    const unsigned char* row = FindInData(&spare, sizeof(spare));
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
        for (unsigned i = 0; i < g_learned_count; ++i) {
            if (std::strcmp(g_learned[i].pref, wanted) == 0) {
                WriteRow(const_cast<unsigned char*>(row),
                         reinterpret_cast<const char*>(name), g_learned[i].resource);
                return;
            }
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
