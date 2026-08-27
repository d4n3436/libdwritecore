#include "parity_gate.h"

#include <cstdlib>
#include <cstring>

namespace chromium_patch {

bool ParityWanted()
{
    // Decided once. A vtable slot replaced at load cannot be put back, and
    // Blink caches render params per font, so the answer must not change
    // partway through the process.
    static const bool wanted = [] {
        const char* v = std::getenv("CHROMIUM_PATCH_DWRITE");
        return v != nullptr && (std::strcmp(v, "1") == 0 || std::strcmp(v, "on") == 0);
    }();
    return wanted;
}

}  // namespace chromium_patch
