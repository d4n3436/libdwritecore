#include "parity_gate.h"

#include <cstdlib>
#include <cstring>

#include "../parity_mode.h"

namespace chromium_patch {

bool ParityWanted()
{
    // Decided once. A vtable slot replaced at load cannot be put back, and
    // Blink caches render params per font, so the answer must not change
    // partway through the process.
    static const bool wanted = [] {
        if (!dwcft::Enabled()) {
            return false;
        }
        return !dwcft::IsOffValue(std::getenv("CLEARTYPE_CHROMIUM"));
    }();
    return wanted;
}

}  // namespace chromium_patch
