//+--------------------------------------------------------------------------
//
//  static_fontconfig.cpp - impose the Windows fallback order on a build that
//  compiled fontconfig in.
//
//  A build that links fontconfig shared has its calls interposed by
//  cleartype/src/fontconfig.cpp, which reorders each sort as it happens. A
//  build that compiles it in exports no fontconfig symbol and keeps no symbol
//  table, so there is nothing to interpose and the order has to be stated up
//  front instead. That fontconfig still reads its configuration through the C
//  library, which is always shared, so the configuration is what this changes.
//
//  Chromium builds one fallback set per locale in
//  CachedFontSet::CreateFcFontSetForLocale and walks it in
//  GetFallbackFontForChar, taking the first family that covers the character
//  (ui/gfx/font_fallback_linux.cc). The order of that set decides the answer.
//  Blink asks with FontDescription::LocaleOrDefault, which falls back to the
//  UI locale, so a run's own language rarely reaches the pattern and one order
//  serves nearly every lookup.
//
//  The order has to satisfy, for every script, that the family
//  fallback_order.cpp names first comes before any other listed family that
//  also covers that script. Those are edges in a graph and the order is its
//  topological sort. Two scripts can disagree about two families that cover
//  both, which is a cycle; the edge serving the fewer code points gives way.
//
//----------------------------------------------------------------------------

#define _GNU_SOURCE

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <algorithm>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dwrite_raster.h"
#include "fallback_order.h"
#include "hb_abi.h"
#include "../windows_fonts.h"
#include "parity_gate.h"

namespace {

// NOLINTNEXTLINE(cert-dcl50-cpp)
void Say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// NOLINTNEXTLINE(cert-dcl50-cpp)
void Say(const char* fmt, ...)
{
    if (std::getenv("DWC_STATIC_FC_LOG") == nullptr) {
        return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    (void)std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)std::fprintf(stderr, "chromium-patch: static fontconfig: %s\n", buf);
}

// An order written down in the table outranks one inferred from what a family
// happens to cover, so a cycle gives up the inference first.
constexpr uint64_t kWrittenOrder = 1ULL << 32;

// How many code points stand for a range when asking whether a family covers
// it. Enough to tell a family that has the script from one holding a few
// borrowed letters.
constexpr unsigned kSamples = 8;

// Whether this process needs any of it. A build whose fontconfig is a shared
// library is served by the interposers instead, and answering here as well
// would impose the order twice.
bool Wanted()
{
    static const bool wanted = [] {
        if (!chromium_patch::ParityWanted()) {
            return false;
        }
        // A build that links fontconfig shared is served by the interposers
        // in cleartype/src/fontconfig.cpp instead, which reorder each sort as
        // it happens.
        return !hb_abi::ExecutableImports("FcFontSort") &&
               !hb_abi::ExecutableImports("FcPatternCreate");
    }();
    return wanted;
}

// A candidate only counts if a stock Windows 11 would have it. The lists name
// fonts a Linux install often has and Windows never does, and taking the first
// installed candidate would answer Noto Sans CJK here where Windows answers
// Malgun Gothic. fallback_order.cpp drops them the same way.
bool ShipsWithWindows(const char* name)
{
    for (const char* known : windows_fonts::kBaseInstall) {
        if (strcasecmp(known, name) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<unsigned> SampleRange(const unsigned first, const unsigned last)
{
    std::vector<unsigned> out;
    const unsigned span = last - first + 1;
    const unsigned step = span / kSamples > 0 ? span / kSamples : 1;
    for (unsigned c = first; c <= last && out.size() < kSamples; c += step) {
        out.push_back(c);
    }
    return out;
}

// The families the table names, in the order they first appear, paired with
// which of each range's sample points they cover. A family with no face behind
// it is left out, since it can never answer.
struct Installed
{
    std::vector<std::string> families;
    std::unordered_map<std::string, std::vector<bool>> covers;   // per script row
};

Installed Survey(const fallback_order::ScriptRow* rows, const unsigned row_count)
{
    Installed out;
    std::unordered_map<std::string, bool> seen;
    for (unsigned r = 0; r < row_count; ++r) {
        for (unsigned f = 0; f < rows[r].count; ++f) {
            const char* name = rows[r].families[f];
            if (name == nullptr || seen.count(name) != 0 || !ShipsWithWindows(name)) {
                continue;
            }
            seen.emplace(name, true);
            out.families.emplace_back(name);
        }
    }

    // One collection lookup per family, each answering every row at once.
    std::vector<unsigned> points;
    std::vector<unsigned> row_start;
    for (unsigned r = 0; r < row_count; ++r) {
        row_start.push_back(static_cast<unsigned>(points.size()));
        for (const unsigned c : SampleRange(rows[r].first, rows[r].last)) {
            points.push_back(c);
        }
    }
    row_start.push_back(static_cast<unsigned>(points.size()));

    std::vector<std::string> present;
    auto answers = std::make_unique<bool[]>(points.size());
    for (const std::string& family : out.families) {
        if (!dwrite_raster::FamilyCoverage(family.c_str(), points.data(),
                                           static_cast<unsigned>(points.size()),
                                           answers.get())) {
            continue;
        }
        std::vector<bool> covered(row_count, false);
        for (unsigned r = 0; r < row_count; ++r) {
            for (unsigned i = row_start[r]; i < row_start[r + 1]; ++i) {
                if (answers[i]) {
                    covered[r] = true;
                    break;
                }
            }
        }
        present.push_back(family);
        out.covers.emplace(family, std::move(covered));
    }
    out.families = std::move(present);
    return out;
}

// Kahn's algorithm, dropping the lightest edge that closes a cycle.
std::vector<std::string> TopoSort(const std::vector<std::string>& nodes,
                                  std::map<std::pair<std::string, std::string>, uint64_t> edges)
{
    while (true) {
        std::unordered_map<std::string, std::vector<std::string>> after;
        std::unordered_map<std::string, unsigned> indegree;
        for (const std::string& n : nodes) {
            after[n];
            indegree[n] = 0;
        }
        for (const auto& [edge, weight] : edges) {
            (void)weight;
            auto& list = after[edge.first];
            if (std::find(list.begin(), list.end(), edge.second) == list.end()) {
                list.push_back(edge.second);
                ++indegree[edge.second];
            }
        }
        std::vector<std::string> ready;
        for (const std::string& n : nodes) {
            if (indegree[n] == 0) {
                ready.push_back(n);
            }
        }
        std::vector<std::string> out;
        for (unsigned i = 0; i < ready.size(); ++i) {
            const std::string n = ready[i];
            out.push_back(n);
            for (const std::string& m : after[n]) {
                if (--indegree[m] == 0) {
                    ready.push_back(m);
                }
            }
        }
        if (out.size() == nodes.size()) {
            return out;
        }
        // Whatever is left sits in a cycle. Drop its lightest edge and retry.
        std::vector<std::string> stuck;
        for (const std::string& n : nodes) {
            if (std::find(out.begin(), out.end(), n) == out.end()) {
                stuck.push_back(n);
            }
        }
        const auto in_stuck = [&stuck](const std::string& n) {
            return std::find(stuck.begin(), stuck.end(), n) != stuck.end();
        };
        auto weakest = edges.end();
        for (auto it = edges.begin(); it != edges.end(); ++it) {
            if (!in_stuck(it->first.first) || !in_stuck(it->first.second)) {
                continue;
            }
            if (weakest == edges.end() || it->second < weakest->second) {
                weakest = it;
            }
        }
        if (weakest == edges.end()) {
            out.insert(out.end(), stuck.begin(), stuck.end());
            return out;
        }
        Say("cycle: %s before %s dropped (%llu)", weakest->first.first.c_str(),
            weakest->first.second.c_str(),
            static_cast<unsigned long long>(weakest->second));
        edges.erase(weakest);
    }
}

const std::vector<std::string>& Order()
{
    static const std::vector<std::string> order = [] {
        unsigned row_count = 0;
        const fallback_order::ScriptRow* rows = fallback_order::Scripts(&row_count);
        if (rows == nullptr || row_count == 0) {
            return std::vector<std::string>{};
        }
        const Installed have = Survey(rows, row_count);
        if (have.families.empty()) {
            return std::vector<std::string>{};
        }

        std::map<std::pair<std::string, std::string>, uint64_t> edges;
        const auto want = [&edges](const std::string& a, const std::string& b,
                                   const uint64_t weight) {
            edges[{a, b}] += weight;
        };
        for (unsigned r = 0; r < row_count; ++r) {
            // FirstAvailableFont picks on presence, so a list whose first names
            // are not installed is answered by the first one that is, and that
            // is the family the others have to come after.
            std::vector<std::string> row;
            for (unsigned f = 0; f < rows[r].count; ++f) {
                const char* name = rows[r].families[f];
                if (name != nullptr && have.covers.count(name) != 0) {
                    row.emplace_back(name);
                }
            }
            if (row.empty()) {
                continue;
            }
            const uint64_t weight = rows[r].last - rows[r].first + 1;
            for (size_t i = 0; i < row.size(); ++i) {
                for (size_t j = i + 1; j < row.size(); ++j) {
                    want(row[i], row[j], kWrittenOrder + weight);
                }
            }
            for (const std::string& other : have.families) {
                if (std::find(row.begin(), row.end(), other) != row.end()) {
                    continue;
                }
                const auto seen = have.covers.find(other);
                if (seen != have.covers.end() && seen->second[r]) {
                    want(row[0], other, weight);
                }
            }
        }
        std::vector<std::string> sorted = TopoSort(have.families, std::move(edges));
        Say("%zu families ordered", sorted.size());
        return sorted;
    }();
    return order;
}

}  // namespace

namespace {

// The rules to add, as fontconfig's own configuration language. Appended
// weakly, so a pattern that already names a family keeps it first and only the
// families behind it are reordered.
std::string RulesBlock()
{
    const std::vector<std::string>& order = Order();
    if (order.empty()) {
        return {};
    }
    std::string out = "  <match target=\"pattern\">\n";
    for (const std::string& family : order) {
        out += "    <edit name=\"family\" mode=\"append\" binding=\"weak\">"
               "<string>";
        // The names come from the table and hold no markup, so the only
        // character that has to be spelled out is the one XML reserves.
        for (const char c : family) {
            out += c == '&' ? "&amp;" : std::string(1, c);
        }
        out += "</string></edit>\n";
    }
    out += "  </match>\n";
    return out;
}

// The file fontconfig reads its configuration from, the way FcConfigFilename
// resolves it with no argument: FONTCONFIG_FILE when set, otherwise
// "fonts.conf" under FONTCONFIG_PATH, which Chromium compiles as /etc/fonts
// (third_party/fontconfig/include/meson-config.h).
const std::string& ConfigPath()
{
    static const std::string path = [] {
        if (const char* file = std::getenv("FONTCONFIG_FILE");
            file != nullptr && file[0] == '/') {
            return std::string(file);
        }
        const char* dir = std::getenv("FONTCONFIG_PATH");
        std::string base = dir != nullptr && dir[0] != '\0' ? dir : "/etc/fonts";
        if (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
        return base + "/fonts.conf";
    }();
    return path;
}

using OpenFn = int (*)(const char*, int, ...);
using OpenAtFn = int (*)(int, const char*, int, ...);

template <typename T>
T Next(const char* name)
{
    return reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}

// Set while the document is being built. Working out the order asks
// DWriteCore for the collection, which reads the same configuration file
// through its own copy of fontconfig and arrives back here; that read has to
// go to the real file, and it must not re-enter the build.
thread_local bool g_building = false;

// The directories the families in the order come from. Asking the collection
// where a family's file is keeps the answer to what this machine actually has,
// and a family that is not installed contributes nothing.
std::vector<std::string> FontDirectories()
{
    std::vector<std::string> dirs;
    for (const std::string& family : Order()) {
        char path[4096];
        if (!dwrite_raster::FamilyDirectory(family.c_str(), path, sizeof(path))) {
            continue;
        }
        if (std::find(dirs.begin(), dirs.end(), path) == dirs.end()) {
            dirs.emplace_back(path);
        }
    }
    return dirs;
}

// The configuration served in place of the host's. It replaces rather than
// extends it, because the host's own rules bind their families strongly and an
// addition lands behind them; the sort then answers a script from whatever the
// machine happens to have. Only the directories the ordered families live in
// are offered, which is what HideFromChromium does a pattern at a time on a
// build whose fontconfig can be interposed.
//
// The cache directories are the ones fontconfig names in its own built-in
// configuration (fcinit.c, FcInitFallbackConfig), so nothing new is written
// anywhere the host was not already writing.
std::string Document()
{
    const std::string rules = RulesBlock();
    const std::vector<std::string> dirs = FontDirectories();
    if (rules.empty() || dirs.empty()) {
        return {};
    }
    std::string out =
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n"
        "<fontconfig>\n";
    for (const std::string& dir : dirs) {
        out += "  <dir>" + dir + "</dir>\n";
    }
    out +=
        "  <cachedir>/var/cache/fontconfig</cachedir>\n"
        "  <cachedir prefix=\"xdg\">fontconfig</cachedir>\n";
    out += rules;
    out += "</fontconfig>\n";
    return out;
}

// The document in an unnamed file, so nothing is written to disk. Built once.
int ServeConfig()
{
    static std::mutex mutex;
    static std::string document;
    static bool built = false;
    {
        const std::lock_guard lock(mutex);
        if (!built) {
            built = true;
            g_building = true;
            document = Document();
            g_building = false;
            if (!document.empty()) {
                Say("serving %zu bytes for %s", document.size(), ConfigPath().c_str());
            }
        }
    }
    if (document.empty()) {
        return -1;
    }
    const int fd = memfd_create("dwc-fonts.conf", 0);
    if (fd < 0) {
        return -1;
    }
    const char* p = document.data();
    for (size_t left = document.size(); left > 0;) {
        const ssize_t n = write(fd, p, left);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Whether this open is the one fontconfig makes for its configuration. The
// path is compared as given, since that is the string fontconfig built.
bool IsConfigRead(const char* path, const int flags)
{
    return !g_building && Wanted() && path != nullptr &&
           (flags & O_ACCMODE) == O_RDONLY && ConfigPath() == path;
}

// Both answers are settled here, at load, because the first open() would
// otherwise settle them: the gate walks the link map and that takes the
// loader's lock, which an open() made from inside the loader already holds.
__attribute__((constructor)) void SettleAtLoad()
{
    (void)Wanted();
    (void)ConfigPath();
}

}  // namespace

extern "C" {

__attribute__((visibility("default")))
int open(const char* path, int flags, ...)
{
    static const auto real = Next<OpenFn>("open");
    mode_t mode = 0;
    if ((flags & (O_CREAT | O_TMPFILE)) != 0) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (IsConfigRead(path, flags)) {
        if (const int fd = ServeConfig(); fd >= 0) {
            return fd;
        }
    }
    return real != nullptr ? real(path, flags, mode) : -1;
}

__attribute__((visibility("default")))
int open64(const char* path, int flags, ...)
{
    static const auto real = Next<OpenFn>("open64");
    mode_t mode = 0;
    if ((flags & (O_CREAT | O_TMPFILE)) != 0) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (IsConfigRead(path, flags)) {
        if (const int fd = ServeConfig(); fd >= 0) {
            return fd;
        }
    }
    return real != nullptr ? real(path, flags, mode) : -1;
}

__attribute__((visibility("default")))
int openat(int dirfd, const char* path, int flags, ...)
{
    static const auto real = Next<OpenAtFn>("openat");
    mode_t mode = 0;
    if ((flags & (O_CREAT | O_TMPFILE)) != 0) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (IsConfigRead(path, flags)) {
        if (const int fd = ServeConfig(); fd >= 0) {
            return fd;
        }
    }
    return real != nullptr ? real(dirfd, path, flags, mode) : -1;
}

__attribute__((visibility("default")))
int openat64(int dirfd, const char* path, int flags, ...)
{
    static const auto real = Next<OpenAtFn>("openat64");
    mode_t mode = 0;
    if ((flags & (O_CREAT | O_TMPFILE)) != 0) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (IsConfigRead(path, flags)) {
        if (const int fd = ServeConfig(); fd >= 0) {
            return fd;
        }
    }
    return real != nullptr ? real(dirfd, path, flags, mode) : -1;
}

}  // extern "C"
