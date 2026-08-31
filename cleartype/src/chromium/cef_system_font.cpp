// system-ui on a CEF host.
//
// PlatformFontSkia::EnsuresDefaultFontIsInitialized takes the system font from
// ui::LinuxUi when there is one, and CEF installs none, so `family` stays at
// kFallbackFontFamilyName, which is the string "serif". The generic then
// resolves to Times New Roman, while Windows takes the
// win::GetDefaultSystemFont branch and answers Segoe UI. That is the whole of
// the system-ui divergence, and it costs the two Arabic pages.
//
// Chromium reads --system-font-family on exactly this path
// (render_view_host_impl.cc, ui/base/ui_base_switches.h), so the switch is
// added to the browser process's argv here, before it parses them. Nothing is
// asked of the embedding application.

#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <vector>

#include <cstdio>
#include <cstdlib>

#include "parity_gate.h"
#include "../parity_mode.h"

namespace {

const char kSwitch[] = "--system-font-family=Segoe UI";

// Whether libcef.so is in the link map. It is a DT_NEEDED of the executable,
// so it is mapped before control reaches here, and neither Electron build
// references it. This is what keeps the switch off a host that has a LinuxUi
// of its own and is already answering what Windows answers.
int NoteCef(dl_phdr_info* info, size_t, void* found)
{
    if (info->dlpi_name != nullptr && std::strstr(info->dlpi_name, "libcef.so") != nullptr) {
        *static_cast<bool*>(found) = true;
    }
    return 0;
}

bool HostIsCef()
{
    bool found = false;
    dl_iterate_phdr(&NoteCef, &found);
    return found;
}

// The browser process is the one with no --type=. Renderers take the family
// over IPC in renderer preferences, so the switch belongs only here.
bool BrowserProcess(const int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && std::strncmp(argv[i], "--type=", 7) == 0) {
            return false;
        }
    }
    return true;
}

bool AlreadyAsked(const int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && std::strncmp(argv[i], "--system-font-family", 20) == 0) {
            return true;
        }
    }
    return false;
}

// Kept alive for the process, since argv has to outlive this call.
std::vector<char*>& Argv()
{
    static std::vector<char*> v;
    return v;
}

}  // namespace

using StartFn = int (*)(int (*)(int, char**, char**), int, char**, int (*)(int, char**, char**),
                        void (*)(), void (*)(), void*);

extern "C" __attribute__((visibility("default"))) int __libc_start_main(int (*main_fn)(int, char**, char**), int argc, char** argv,
                                 int (*init)(int, char**, char**), void (*fini)(),
                                 void (*rtld_fini)(), void* stack_end)
{
    static const auto real = reinterpret_cast<StartFn>(dlsym(RTLD_NEXT, "__libc_start_main"));
    if (real == nullptr) {
        __builtin_trap();
    }
    // Deliberately not ParityWanted(). This runs before the library's own
    // constructors, and that answer is cached in a static on first call, so
    // asking here would freeze whatever it decides before the host is fully
    // mapped. The tests below need no state of ours.
    if (argv != nullptr && argc > 0 && !dwcft::IsOffValue(std::getenv("CLEARTYPE_CHROMIUM")) &&
        BrowserProcess(argc, argv) && !AlreadyAsked(argc, argv) && HostIsCef()) {
        if (std::getenv("DWC_CEF_FONT_LOG") != nullptr) {
            (void)std::fprintf(stderr, "chromium-patch: cef system font: adding %s\n", kSwitch);
        }
        std::vector<char*>& held = Argv();
        held.assign(argv, argv + argc);
        held.push_back(const_cast<char*>(kSwitch));
        held.push_back(nullptr);
        return real(main_fn, argc + 1, held.data(), init, fini, rtld_fini, stack_end);
    }
    return real(main_fn, argc, argv, init, fini, rtld_fini, stack_end);
}
