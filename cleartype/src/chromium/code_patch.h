//+--------------------------------------------------------------------------
//
//  code_patch.h - replace a function where it stands.
//
//  A preloaded library only stands in front of calls the loader resolves. A
//  call to a function compiled into the same binary is resolved at build time
//  and never reaches the interposing definition, so the function itself is
//  overwritten instead.
//
//  The replacement takes over outright. Nothing is kept of the original, so a
//  replacement that has to defer to the real behavior needs another way to
//  reach it.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_CODE_PATCH_H_INCLUDED
#define CHROMIUM_CODE_PATCH_H_INCLUDED

#include <cstdint>
#include <cstring>

#include <cstddef>
#include <cstdint>

#include <link.h>

#include <sys/mman.h>
#include <unistd.h>

namespace code_patch {

// Point `at` at `to` with a movabs and an indirect jump. Returns false and
// leaves the function alone if the page will not open, with *why naming what
// stopped it. A true return with *why set means the detour is live but its
// page stayed writable.
inline bool WriteDetour(void* at, void* to, const char** why)
{
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        *why = "the page size is not known";
        return false;
    }
    constexpr size_t kPatch = 12;
    auto* bytes = static_cast<unsigned char*>(at);
    const auto start = reinterpret_cast<uintptr_t>(bytes) & ~static_cast<uintptr_t>(page - 1);
    const uintptr_t last =
        (reinterpret_cast<uintptr_t>(bytes) + kPatch - 1) & ~static_cast<uintptr_t>(page - 1);
    const size_t len = last - start + static_cast<size_t>(page);
    auto* base = reinterpret_cast<void*>(start);
    if (mprotect(base, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        *why = "the page would not open for writing";
        return false;
    }
    // movabs rax, imm64 ; jmp rax
    unsigned char code[kPatch] = {0x48, 0xB8};
    const auto target = reinterpret_cast<uint64_t>(to);
    std::memcpy(code + 2, &target, sizeof(target));
    code[10] = 0xFF;
    code[11] = 0xE0;
    std::memcpy(bytes, code, kPatch);
    if (mprotect(base, len, PROT_READ | PROT_EXEC) != 0) {
        *why = "the page would not close again";
    }
    __builtin___clear_cache(reinterpret_cast<char*>(bytes),
                            reinterpret_cast<char*>(bytes + kPatch));
    return true;
}


// The executable segment of whichever image holds `addr`. A host that keeps
// Blink in libcef.so needs the segment that symbol lives in and not the main
// executable's, and the symbol's own address is what says which that is.
struct TextSpan
{
    const unsigned char* text = nullptr;
    size_t size = 0;
    const void* want = nullptr;
};

inline int NoteSpanFor(dl_phdr_info* info, size_t, void* out)
{
    auto* span = static_cast<TextSpan*>(out);
    const auto at = reinterpret_cast<uintptr_t>(span->want);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) {
            continue;
        }
        if (const uintptr_t base = info->dlpi_addr + ph.p_vaddr; at >= base && at < base + ph.p_memsz) {
            span->text = reinterpret_cast<const unsigned char*>(base);
            span->size = ph.p_memsz;
            return 1;
        }
    }
    return 0;
}

inline TextSpan TextHolding(const void* addr)
{
    TextSpan span;
    span.want = addr;
    dl_iterate_phdr(&NoteSpanFor, &span);
    return span;
}

// A page of executable memory a rel32 call from `near` can reach. The shim
// is mapped in the usual mmap area, gigabytes away from a 163 MB executable,
// so a call site cannot name it directly; it names a page here instead.
inline void* NearbyPage(const unsigned char* near, const long page)
{
    const auto anchor = reinterpret_cast<uintptr_t>(near) & ~static_cast<uintptr_t>(page - 1);
    // Step outward until the first hole, staying well inside rel32 range.
    for (uintptr_t step = static_cast<uintptr_t>(page); step < 0x70000000u;
         step += static_cast<uintptr_t>(page) * 64) {
        for (int dir = 0; dir < 2; ++dir) {
            const uintptr_t want = dir == 0 ? anchor + step : anchor - step;
            if (want < 0x10000u) {
                continue;
            }
            void* got = mmap(reinterpret_cast<void*>(want), static_cast<size_t>(page),
                               PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (got == reinterpret_cast<void*>(want)) {
                return got;
            }
            if (got != MAP_FAILED) {
                (void)munmap(got, static_cast<size_t>(page));
            }
        }
    }
    return nullptr;
}

// Whether the five bytes at `at` are a direct call or tail jump naming
// `target`. E8 is a call, E9 a jump. A function whose last statement is
// `return other(...)` is compiled as a tail call, which is a jump to the entry
// and never a call, so a scan for E8 alone finds nothing.
inline bool IsBranchTo(const unsigned char* at, const unsigned char* target)
{
    if (at[0] != 0xE8 && at[0] != 0xE9) {
        return false;
    }
    int32_t rel = 0;
    std::memcpy(&rel, at + 1, 4);
    return at + 5 + rel == target;
}

// Every site in the segment that branches to `target`, up to `max` of them.
// Reads only the segment, so it answers what RedirectCalls would otherwise
// have to be run to ask: which code reaches an address no symbol names. Pair
// it with a scan for the entry below a site to name the enclosing function,
// and fingerprint that candidate against what it should not call before
// redirecting anything.
inline unsigned CallSitesOf(const unsigned char* text, const size_t text_size,
                            const unsigned char* target, const unsigned char** out,
                            const unsigned max)
{
    unsigned found = 0;
    for (size_t i = 0; i + 5 <= text_size; ++i) {
        const unsigned char* at = text + i;
        if (!IsBranchTo(at, target)) {
            continue;
        }
        if (out != nullptr && found < max) {
            out[found] = at;
        }
        ++found;
    }
    return found;
}

// Point every direct call or tail jump to `target` at `to`, inside the
// executable segment already found. hb_abi has the same idea but resolves the
// image with dladdr,
// which does not answer for a stripped main executable, so the segment is
// passed in here instead.
inline unsigned RedirectCalls(const unsigned char* text, const size_t text_size,
                              const unsigned char* target, void* to)
{
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return 0;
    }
    unsigned char* hop = nullptr;
    unsigned moved = 0;
    for (size_t i = 0; i + 5 <= text_size; ++i) {
        unsigned char* at = const_cast<unsigned char*>(text) + i;
        // Redirecting a tail jump is the same as redirecting a call: the
        // replacement returns what the caller was going to return.
        if (!IsBranchTo(at, target)) {
            continue;
        }
        if (hop == nullptr) {
            hop = static_cast<unsigned char*>(NearbyPage(at, page));
            if (hop == nullptr) {
                return 0;
            }
            // jmp *0(%rip), then the address it reads. No register is
            // touched and no stack slot is used, so the call arrives at the
            // replacement with the ABI exactly as the caller left it.
            static const unsigned char kJump[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
            std::memcpy(hop, kJump, sizeof(kJump));
            std::memcpy(hop + sizeof(kJump), static_cast<const void*>(&to), sizeof(to));
        }
        const auto want = reinterpret_cast<intptr_t>(hop) - reinterpret_cast<intptr_t>(at + 5);
        if (want > INT32_MAX || want < INT32_MIN) {
            continue;
        }
        auto* start = reinterpret_cast<unsigned char*>(
            reinterpret_cast<uintptr_t>(at) & ~static_cast<uintptr_t>(page - 1));
        const size_t span = static_cast<size_t>(at + 5 - start);
        if (mprotect(start, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            continue;
        }
        const auto rel32 = static_cast<int32_t>(want);
        std::memcpy(at + 1, &rel32, 4);
        (void)mprotect(start, span, PROT_READ | PROT_EXEC);
        ++moved;
    }
    return moved;
}


}  // namespace code_patch

#endif  // CHROMIUM_CODE_PATCH_H_INCLUDED
