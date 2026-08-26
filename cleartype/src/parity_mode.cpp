//+--------------------------------------------------------------------------
//
//  parity_mode.cpp - deciding, at runtime, whether Gecko is the caller.
//
//  Two signals, because one of them is not available when the first decision
//  has to be made.
//
//  libxul being loaded is the authoritative answer, and libxul_patch.cpp
//  already learns it: it scans the loaded images in a constructor and
//  interposes dlopen for the case that matters, since the firefox binary has
//  no DT_NEEDED on libxul and dlopens it after the constructor has run. That
//  signal arrives through NoteGeckoLoaded.
//
//  It arrives too late for two things. The prefs shim has to put
//  MOZ_DEFAULT_PREFS in the environment before Preferences reads it, and the
//  DirectWrite factory has to exist before a content process's sandbox starts
//  and denies the dlopen that would create it - both before libxul is mapped.
//  So there is a second signal that needs nothing loaded:
//
//      libxul.so sits in the same directory as /proc/self/exe
//
//  which is true of every Gecko application and describes the installation,
//  not a name. /usr/bin/firefox is a two-line
//  script that execs /usr/lib/firefox/firefox, so the process this runs in has
//  /usr/lib/firefox/firefox as its executable and libxul.so beside it; the
//  same holds for Thunderbird, for a downloaded tarball in a home directory,
//  and for a fork that renamed the binary. Matching a list of brand names
//  would recognize none of those.
//
//  Either signal is enough. CLEARTYPE_FORCE_PARITY=0 or 1 overrides both,
//  and CLEARTYPE=0 switches the whole library off ahead of either.
//
//----------------------------------------------------------------------------

#include "parity_mode.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>

#include <limits.h>

namespace dwcft
{
namespace
{

bool IsOffValue(const char* v)
{
    return v != nullptr &&
           (std::strcmp(v, "0") == 0 || strcasecmp(v, "off") == 0 ||
            strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0);
}

}  // namespace

// Read once. An environment variable does not change under a running process
// without an exec, and an exec starts this over.
bool Enabled()
{
    static const bool on = !IsOffValue(std::getenv("CLEARTYPE"));
    return on;
}

}  // namespace dwcft

#if DWRITECORE_FIREFOX_PARITY

namespace dwcft
{
namespace
{

using dwcft::IsOffValue;

std::atomic g_gecko_loaded{false};

enum class Override
{
    kAuto,
    kOn,
    kOff
};

Override ReadOverride()
{
    const char* v = std::getenv("CLEARTYPE_FORCE_PARITY");
    if (v == nullptr || strcasecmp(v, "auto") == 0) {
        return Override::kAuto;
    }
    if (IsOffValue(v)) {
        return Override::kOff;
    }
    return Override::kOn;
}

// Is there a libxul.so next to this process's executable? Answered once: a
// process does not change its executable without exec'ing, and an exec starts
// this over.
bool GeckoInstallLayout()
{
    char path[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) {
        return false;
    }
    path[n] = '\0';

    const char* slash = std::strrchr(path, '/');
    if (slash == nullptr) {
        return false;
    }

    static constexpr char kLibxul[] = "libxul.so";
    const size_t dir_len = static_cast<size_t>(slash - path) + 1;
    if (dir_len + sizeof(kLibxul) > sizeof(path)) {
        return false;
    }
    std::memcpy(path + dir_len, kLibxul, sizeof(kLibxul));

    return access(path, F_OK) == 0;
}

}  // namespace

void NoteGeckoLoaded()
{
    g_gecko_loaded.store(true, std::memory_order_relaxed);
}

// Every child carries -contentproc; the parent carries none, so one read of
// its own command line settles it without asking libxul anything.
bool GeckoParentProcess()
{
    static const bool parent = [] {
        const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        char buf[4096];
        const ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) {
            return false;
        }
        buf[n] = '\0';
        for (ssize_t i = 0; i < n; ++i) {
            // Arguments are NUL-separated, so each one starts after a NUL.
            if ((i == 0 || buf[i - 1] == '\0') && std::strcmp(&buf[i], "-contentproc") == 0) {
                return false;
            }
        }
        return true;
    }();
    return parent;
}

bool ParityActive()
{
    // The master switch comes first: with the library off, nothing about
    // Gecko matters.
    if (!Enabled()) {
        return false;
    }
    static const Override override_mode = ReadOverride();
    if (override_mode != Override::kAuto) {
        return override_mode == Override::kOn;
    }

    // Checked before the flag, because it is the signal that is already true
    // at the point the earliest decisions are made.
    static const bool install_layout = GeckoInstallLayout();
    return install_layout || g_gecko_loaded.load(std::memory_order_relaxed);
}

}  // namespace dwcft

#endif  // DWRITECORE_FIREFOX_PARITY
