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

}  // namespace code_patch

#endif  // CHROMIUM_CODE_PATCH_H_INCLUDED
