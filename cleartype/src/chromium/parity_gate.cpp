#include "parity_gate.h"

#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <link.h>
#include <cstring>

#include "../parity_mode.h"

namespace {

// Whether some loaded module was built from Skia's fontconfig font manager,
// which is the signal that this process is Chromium at all.
//
// Parts of the patch that reach shared code have no target of their own to
// find, so the host is identified here instead. Checking the executable's
// imports does not work, since a Gecko launcher imports no fontconfig either.
bool ChromiumHost()
{
    static constexpr char kAnchor[] = "skia/src/ports/SkFontMgr_FontConfigInterface.cpp";

    struct Ask
    {
        const void* self_base;
        bool found;
    };
    Dl_info self{};
    const void* self_base =
        dladdr(reinterpret_cast<const void*>(&ChromiumHost), &self) != 0 ? self.dli_fbase
                                                                         : nullptr;
    Ask ask{self_base, false};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) {
            auto* a = static_cast<Ask*>(data);
            // Skip this library, whose own copy of the anchor string would
            // otherwise match.
            if (a->self_base != nullptr &&
                reinterpret_cast<const void*>(info->dlpi_addr) == a->self_base) {
                return 0;
            }
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& p = info->dlpi_phdr[i];
                if (p.p_type != PT_LOAD || (p.p_flags & PF_X) != 0 || (p.p_flags & PF_W) != 0) {
                    continue;
                }
                const auto* begin =
                    reinterpret_cast<const unsigned char*>(info->dlpi_addr + p.p_vaddr);
                if (memmem(begin, p.p_filesz, kAnchor, sizeof(kAnchor) - 1) != nullptr) {
                    a->found = true;
                    return 1;
                }
            }
            return 0;
        },
        &ask);
    return ask.found;
}

}  // namespace

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
        if (!dwcft::IsOffValue(std::getenv("CLEARTYPE_CHROMIUM"))) {
            // The variable says the user wants the patch, not that the host
            // is Chromium, so the host still has to be identified. Without
            // that a Gecko process is pruned to the Windows faces by the sort
            // in cleartype/src/fontconfig.cpp and its per-character fallback
            // stops matching Firefox on Windows.
            return ChromiumHost();
        }
        return false;
    }();
    return wanted;
}

}  // namespace chromium_patch
