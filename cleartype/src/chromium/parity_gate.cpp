#include "parity_gate.h"

#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <link.h>

#include "../parity_mode.h"

namespace {

// Where this library is loaded, so a scan of the other images can leave it
// out. Its own copies of the strings being looked for would otherwise match.
const void* SelfBase()
{
    Dl_info self{};
    return dladdr(reinterpret_cast<const void*>(&SelfBase), &self) != 0 ? self.dli_fbase
                                                                       : nullptr;
}

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
    Ask ask{SelfBase(), false};
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

// What stands ahead of the product version in the user agent string every
// Chromium keeps in read-only data.
constexpr char kVersionMark[] = "Chrome/";

// The major number of a `<major>.<minor>.<build>.<patch>` at `p`, or 0 when
// the four numbers are not all there. Four are required so that a "Chrome/"
// followed by anything else is passed over.
int MilestoneAt(const unsigned char* p, const unsigned char* end)
{
    int major = 0;
    for (int part = 0; part < 4; ++part) {
        if (part != 0) {
            if (p == end || *p != '.') {
                return 0;
            }
            ++p;
        }
        const unsigned char* first = p;
        int value = 0;
        while (p != end && *p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            ++p;
        }
        // An empty or absurdly long run is not a version number.
        if (p == first || p - first > 9) {
            return 0;
        }
        if (part == 0) {
            major = value;
        }
    }
    return major;
}

// The milestone named in the read-only data of some loaded image. A release
// build is stripped, so the version string is the only place it says which
// Chromium it came from; every image that carries one agrees on it.
int MilestoneFromImages()
{
    struct Ask
    {
        const void* self_base;
        int milestone;
    };
    Ask ask{SelfBase(), 0};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) {
            auto* a = static_cast<Ask*>(data);
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
                const unsigned char* end = begin + p.p_filesz;
                for (const unsigned char* at = begin; at < end;) {
                    const auto* found = static_cast<const unsigned char*>(
                        memmem(at, static_cast<size_t>(end - at), kVersionMark,
                               sizeof(kVersionMark) - 1));
                    if (found == nullptr) {
                        break;
                    }
                    const int milestone =
                        MilestoneAt(found + sizeof(kVersionMark) - 1, end);
                    if (milestone != 0) {
                        a->milestone = milestone;
                        return 1;
                    }
                    at = found + 1;
                }
            }
            return 0;
        },
        &ask);
    return ask.milestone;
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

int ChromiumMilestone()
{
    // Reading it walks tens of megabytes of read-only data, so it happens
    // once. An image is never unloaded and reloaded at a different version.
    static const int milestone = MilestoneFromImages();
    return milestone;
}

}  // namespace chromium_patch
