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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include <link.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dwrite_raster.h"
#include "fallback_order.h"
#include "hb_abi.h"
#include "../windows_fonts.h"
#include "fallback_hook.h"
#include "family_match.h"
#include "parity_gate.h"
#include "static_fontconfig.h"

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
    const auto note = [&seen, &out](const char* name) {
        if (name == nullptr || seen.contains(name) || !ShipsWithWindows(name)) {
            return;
        }
        seen.emplace(name, true);
        out.families.emplace_back(name);
    };
    for (unsigned r = 0; r < row_count; ++r) {
        for (unsigned f = 0; f < rows[r].count; ++f) {
            note(rows[r].families[f]);
        }
    }
    // The measured DirectWrite answers too. Yu Gothic UI is named by no script
    // row, so without this the survey never learns it and the block that ends
    // in it is dropped for a family it thinks is not installed.
    for (unsigned i = 0; i < fallback_order::DWriteRowCount(); ++i) {
        note(fallback_order::DWriteRowFamily(static_cast<int>(i)));
        note(fallback_order::DWriteRowFront(static_cast<int>(i)));
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
    const auto answers = std::make_unique<bool[]>(points.size());
    for (const std::string& family : out.families) {
        if (!dwrite_raster::FamilyCoverage(family.c_str(), points.data(),
                                           static_cast<unsigned>(points.size()),
                                           answers.get())) {
            continue;
        }
        std::vector covered(row_count, false);
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
            if (std::ranges::find(list, edge.second) == list.end()) {
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
            if (std::ranges::find(out, n) == out.end()) {
                stuck.push_back(n);
            }
        }
        const auto in_stuck = [&stuck](const std::string& n) {
            return std::ranges::find(stuck, n) != stuck.end();
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

// Which of the table's families this machine has, and what each covers.
// Read by the order below and by the per-language blocks, so it is surveyed
// once.
const Installed& Surveyed()
{
    static const Installed have = [] {
        unsigned row_count = 0;
        const fallback_order::ScriptRow* rows = fallback_order::Scripts(&row_count);
        if (rows == nullptr || row_count == 0) {
            return Installed{};
        }
        return Survey(rows, row_count);
    }();
    return have;
}

const std::vector<std::string>& Order()
{
    static const std::vector<std::string> order = [] {
        unsigned row_count = 0;
        const fallback_order::ScriptRow* rows = fallback_order::Scripts(&row_count);
        if (rows == nullptr || row_count == 0) {
            return std::vector<std::string>{};
        }
        const Installed& have = Surveyed();
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
                if (name != nullptr && have.covers.contains(name)) {
                    row.emplace_back(name);
                }
            }
            if (row.empty()) {
                continue;
            }
            const uint64_t weight = rows[r].last - rows[r].first + 1;
            // Only row[0] can ever answer: FirstAvailableFont takes the first
            // installed candidate and memoizes it, so the names behind it draw
            // nothing however much they cover. Ordering them against each other
            // constrains nothing real, and it is what puts Kana and Han in a
            // cycle over MS PGothic and Microsoft YaHei.
            for (const std::string& other : have.families) {
                if (other == row[0]) {
                    continue;
                }
                if (const auto seen = have.covers.find(other); seen != have.covers.end() && seen->second[r]) {
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
// The languages whose Han script is settled, most specific first. The tag is
// what fontconfig tests the pattern's lang against and the locale is what
// HanCandidates resolves; they differ only where a substring has to stay
// narrow enough not to catch another language.
struct LangHan
{
    const char* tag;
    const char* locale;
};

constexpr size_t kLangHanCount = 9;
const LangHan kLangHan[] = {
    {.tag = "zh-hant", .locale = "zh-Hant"}, {.tag = "zh-tw", .locale = "zh-TW"}, {.tag = "zh-hk", .locale = "zh-HK"},
    {.tag = "zh-mo", .locale = "zh-MO"},     {.tag = "zh-hans", .locale = "zh-Hans"}, {.tag = "zh-cn", .locale = "zh-CN"},
    {.tag = "zh-sg", .locale = "zh-SG"},     {.tag = "ja", .locale = "ja"},       {.tag = "ko", .locale = "ko"},
};

// The surveyed spelling of a family, or null when this machine has no such
// family. The pan-Unicode lists are lowercased, as the source keeps them, and
// the survey is keyed by the script table's own spelling.
const char* Surveyed(const Installed& have, const char* family)
{
    for (const std::string& known : have.families) {
        if (strcasecmp(known.c_str(), family) == 0) {
            return known.c_str();
        }
    }
    return nullptr;
}

// DWRITE_FONT_STYLE values, which dwrite_raster.h takes as plain ints.
constexpr int kStyleOblique = 1;
constexpr int kStyleItalic = 2;

// One rule per run of requested weights that answers with the same face, so a
// page asking for a weight no face carries lands where Windows puts it. The
// nine CSS weights are only the ones a keyword reaches; 550 between Regular
// and Bold is as reachable, and without a rule it falls to fontconfig's own
// nearest-weight logic, which picks the lighter face where Windows picks Bold.
//
// `style` is the DWRITE_FONT_STYLE the request carries and `slant` the
// fontconfig value that stands for it. The face weights of the whole family
// are only candidate boundaries: a slanted request is answered from the
// slanted faces alone, so runs that answer alike are merged and the extra
// boundaries cost nothing.
std::string WeightRanges(const std::string& family, const int style, const int slant)
{
    // Each boundary is searched for. The face weights say where the answer
    // can change, but a slanted request is answered from a different set of
    // faces and a simulated entry moves the boundary again, so an interval is
    // split until both ends answer alike.
    struct Answer
    {
        int weight;
        bool italic;
        bool ok;

        bool operator==(const Answer& other) const = default;
    };
    std::map<int, Answer> asked;
    const auto ask = [&](const int weight) {
        if (const auto found = asked.find(weight); found != asked.end()) {
            return found->second;
        }
        Answer a{};
        a.ok = dwrite_raster::FamilyMatchFace(family.c_str(), weight, style, &a.weight,
                                              &a.italic) &&
               a.weight != 0;
        asked.emplace(weight, a);
        return a;
    };

    struct Run
    {
        int lo;
        int hi;
        int weight;
        bool italic;
    };
    std::vector<Run> runs;
    const auto add = [&runs](const int lo, const int hi, const Answer& a) {
        if (!runs.empty() && runs.back().weight == a.weight &&
            runs.back().italic == a.italic && runs.back().hi + 1 == lo) {
            runs.back().hi = hi;
        } else {
            runs.push_back({.lo = lo, .hi = hi, .weight = a.weight, .italic = a.italic});
        }
    };
    // An explicit stack, walked low to high so the runs come out in order.
    std::vector<std::pair<int, int>> todo{{1, 1000}};
    while (!todo.empty()) {
        const auto [lo, hi] = todo.back();
        todo.pop_back();
        const Answer a = ask(lo);
        const Answer b = ask(hi);
        if (!a.ok || !b.ok) {
            return {};
        }
        if (a == b) {
            add(lo, hi, a);
            continue;
        }
        if (hi - lo == 1) {
            add(lo, lo, a);
            add(hi, hi, b);
            continue;
        }
        const int mid = lo + (hi - lo) / 2;
        todo.emplace_back(mid + 1, hi);
        todo.emplace_back(lo, mid);
    }
    if (runs.empty()) {
        return {};
    }

    // The rules are written on the fontconfig scale, which is coarse: a
    // hundred OpenType steps between Bold and Extrabold are five of its own,
    // so two runs can round to the same bound. Ranges that share a bound are
    // worse than coarse, since fontconfig applies each edit as it goes and a
    // later test reads the weight an earlier rule already assigned. The runs
    // are projected onto the fontconfig scale instead, one value at a time
    // through the same map Skia converts a request with, which makes them
    // disjoint and keeps every value answering for the request that produced
    // it.
    constexpr int kMaxFcWeight = 215;
    std::string out;
    int start = 0;
    for (int v = 0; v <= kMaxFcWeight; ++v) {
        const auto at = [&runs](const int fc) {
            // NOLINTNEXTLINE(bugprone-incorrect-roundings)
            const int ot = static_cast<int>(family_match::OpenTypeWeight(fc) + 0.5f);
            for (const Run& run : runs) {
                if (ot >= run.lo && ot <= run.hi) {
                    return &run;
                }
            }
            return static_cast<const Run*>(nullptr);
        };
        const Run* here = at(v);
        if (const Run* next = v < kMaxFcWeight ? at(v + 1) : nullptr; here != nullptr && next == here) {
            continue;
        }
        if (here != nullptr) {
            char rule[832];
            (void)std::snprintf(
                rule, sizeof(rule),
                "  <match target=\"pattern\">\n"
                "    <test name=\"family\"><string>%s</string></test>\n"
                "    <test name=\"weight\" compare=\"more_eq\"><int>%d</int></test>\n"
                "    <test name=\"weight\" compare=\"less_eq\"><int>%d</int></test>\n"
                "    <test name=\"slant\" compare=\"eq\"><int>%d</int></test>\n"
                "    <edit name=\"weight\" mode=\"assign\"><int>%d</int></edit>\n"
                "%s"
                "  </match>\n",
                family.c_str(), start, v, slant,
                family_match::FontconfigWeightNear(here->weight),
                slant != 0 && !here->italic
                    ? "    <edit name=\"slant\" mode=\"assign\"><int>0</int></edit>\n"
                    : "");
            out += rule;
        }
        start = v + 1;
    }
    return out;
}

std::string WeightRules()
{
    std::vector<std::string> families;
    if (!dwrite_raster::FamilyNames(&families)) {
        return {};
    }
    std::string out;
    unsigned ruled = 0;
    for (const std::string& family : families) {
        // Only what the configuration offers. The names come from the host,
        // so a name carrying the one character XML reserves is skipped.
        if (family.find('&') != std::string::npos || !ShipsWithWindows(family.c_str())) {
            continue;
        }
        // Upright, italic and oblique each answer from their own faces, and
        // an oblique request is answered by a simulated entry where an italic
        // one keeps a real italic face.
        for (const auto& [style, slant] : {std::pair{0, 0}, std::pair{kStyleItalic, 100},
                                           std::pair{kStyleOblique, 110}}) {
            if (std::string ranges = WeightRanges(family, style, slant); !ranges.empty()) {
                out += ranges;
                ++ruled;
            } else {
                Say("%s slant %d: no weight ranges", family.c_str(), slant);
            }
        }
    }
    Say("%u weight rules over %zu families", ruled, families.size());
    return out;
}

std::string RulesBlock()
{
    const std::vector<std::string>& order = Order();
    if (order.empty()) {
        return {};
    }
    const Installed& have = Surveyed();
    // The unified-Han family follows the run's language, and the language is on
    // the pattern as FC_LANG. fontconfig can test it, so the locales that name
    // a Han script get their own block ahead of the global one; append puts
    // them in front of it.
    // Latin, Greek and Cyrillic name one family, and it covers no Han, no
    // Indic and no symbol block, so nothing else has to come before it. Said
    // first, it answers those scripts wherever it is reached and stands in
    // front of nothing else. Without it the pan-Unicode list below, which
    // opens with Tahoma, answers them instead.
    // Weakly bound. Binding it strongly does put it in front of the second face
    // of the run's own family, which is what answers Cyrillic under zh-TW, but
    // it then wins the sort outright and answers Armenian, Hebrew, Arabic and
    // the symbol blocks as well, which is far worse than the four cells it
    // buys.
    std::string latin;
    if (have.covers.contains("Times New Roman")) {
        latin = "    <edit name=\"family\" mode=\"append\" binding=\"weak\">"
                "<string>Times New Roman</string></edit>\n";
    }

    // Windows picks the face within a named family with
    // GetFirstMatchingFont, and fontconfig picks the nearest weight on its own
    // scale with the set's order breaking ties, so a family whose faces do not
    // sit on the requested weight can answer differently. Segoe UI at 500 is
    // Semibold on Windows and regular here. Asking DirectWrite what it would
    // answer and writing that weight onto the pattern makes fontconfig's pick
    // an exact match, which no tie can move. The family list is left alone, so
    // nothing becomes available that was not.
    std::string out = WeightRules();
    // What Windows walks when the script's family does not cover the
    // character, said after that family so the two are read in that order.
    std::string pan;
    {
        unsigned pan_count = 0;
        const char* const* list = fallback_order::PanUnicode(false, &pan_count);
        for (unsigned i = 0; list != nullptr && i < pan_count; ++i) {
            const char* named = list[i] != nullptr ? Surveyed(have, list[i]) : nullptr;
            if (named == nullptr) {
                continue;
            }
            pan += "    <edit name=\"family\" mode=\"append\" binding=\"weak\">"
                   "<string>";
            pan += named;
            pan += "</string></edit>\n";
        }
    }
    // One block per family, reached by a tag on the language. The order a
    // pattern carries is settled once per locale, and Windows answers per
    // character, so the character's family is asked for by name. The hook in
    // fallback_hook.cpp appends this family's tag to the locale it was given,
    // and the block puts that family in front of everything else. Stated
    // before the blocks below, since append keeps what came first.
    for (size_t i = 0; i < order.size() && i < static_fontconfig::kMaxTaggedFamilies; ++i) {
        char tag[24];
        (void)std::snprintf(tag, sizeof(tag), "%s%02zu", static_fontconfig::kFamilyTag, i);
        out += "  <match target=\"pattern\">\n"
               "    <test name=\"lang\" compare=\"contains\"><string>";
        out += tag;
        out += "</string></test>\n";
        out += "    <edit name=\"family\" mode=\"append\" binding=\"weak\">"
               "<string>";
        out += order[i];
        out += "</string></edit>\n";
        out += pan;
        out += "  </match>\n";
    }
    // The list on its own, for a character no row names a family for.
    out += "  <match target=\"pattern\">\n"
           "    <test name=\"lang\" compare=\"contains\"><string>";
    out += static_fontconfig::kPanTag;
    out += "</string></test>\n";
    out += pan;
    out += "  </match>\n";
    // One block per measured IDWriteFontFallback row: the script's own
    // families, then the pan-Unicode list, then the family DirectWrite
    // answers with. Windows reaches the last one only after the first two
    // have missed, so stating them in that order keeps a character the script
    // or the list does cover with the family it already had.
    for (unsigned row = 0; row < fallback_order::DWriteRowCount(); ++row) {
        const char* family = fallback_order::DWriteRowFamily(static_cast<int>(row));
        const char* named = family != nullptr ? Surveyed(have, family) : nullptr;
        const char* wanted = fallback_order::DWriteRowFront(static_cast<int>(row));
        const char* front = wanted != nullptr ? Surveyed(have, wanted) : nullptr;
        if (named == nullptr && front == nullptr) {
            continue;           // the survey turned neither up, so nothing to name
        }
        const auto edit = [](const char* named_family) {
            std::string one =
                "    <edit name=\"family\" mode=\"append\" binding=\"weak\"><string>";
            one += named_family;
            one += "</string></edit>\n";
            return one;
        };
        // The script path's family first, then the families the script table
        // names, then the list, then DirectWrite's own answer, which is the
        // order Windows reads them in.
        std::string body;
        // A row that answers ahead of the list answers ahead of the script's
        // families too, since those carry the family the list would have used.
        const bool ahead = fallback_order::DWriteRowAhead(static_cast<int>(row));
        if (ahead && named != nullptr) {
            body += edit(named);
        }
        unsigned script_count = 0;
        const char* const* script = fallback_order::FamiliesFor(
            fallback_order::DWriteRowFirst(static_cast<int>(row)), &script_count);
        for (unsigned i = 0; script != nullptr && i < script_count; ++i) {
            const char* first = script[i] != nullptr ? Surveyed(have, script[i]) : nullptr;
            if (first != nullptr) {
                body += edit(first);
            }
        }
        body += pan;
        if (!ahead && named != nullptr) {
            body += edit(named);
        }
        const auto block = [&out](const char* tag, const std::string& text) {
            out += "  <match target=\"pattern\">\n"
                   "    <test name=\"lang\" compare=\"contains\"><string>";
            out += tag;
            out += "</string></test>\n";
            out += text;
            out += "  </match>\n";
        };
        char tag[32];
        if (front == nullptr) {
            (void)std::snprintf(tag, sizeof(tag), "%s%02u",
                                static_fontconfig::kDWriteTag, row);
            block(tag, body);
            continue;
        }
        // The front follows the run's language, so the row gets one block per
        // Han language and one for a run that names none of them. The tag
        // carries both indices, since the hook replaces the locale with it and
        // the language would otherwise be gone by the time this is read.
        for (size_t lang = 0; lang <= kLangHanCount; ++lang) {
            const bool unsettled = lang == kLangHanCount;
            unsigned count = 0;
            const char* const* candidates = fallback_order::HanCandidates(
                unsettled ? nullptr : kLangHan[lang].locale, &count);
            const char* first = nullptr;
            if (candidates != nullptr) {
                for (unsigned i = 0; i < count && first == nullptr; ++i) {
                    if (candidates[i] != nullptr && have.covers.contains(candidates[i])) {
                        first = candidates[i];
                    }
                }
            }
            if (first == nullptr) {
                first = front;      // the row's own default, for an unsettled run
            }
            (void)std::snprintf(tag, sizeof(tag), "%s%02u%02u",
                                static_fontconfig::kDWriteHanTag, row,
                                unsettled ? 99u : static_cast<unsigned>(lang));
            block(tag, edit(first) + body);
        }
    }
    for (const LangHan& row : kLangHan) {
        unsigned count = 0;
        const char* const* candidates =
            fallback_order::HanCandidates(row.locale, &count);
        if (candidates == nullptr) {
            continue;
        }
        const char* first = nullptr;
        for (unsigned i = 0; i < count && first == nullptr; ++i) {
            if (candidates[i] != nullptr && have.covers.contains(candidates[i])) {
                first = candidates[i];
            }
        }
        if (first == nullptr) {
            continue;
        }
        out += "  <match target=\"pattern\">\n"
               "    <test name=\"lang\" compare=\"contains\"><string>";
        out += row.tag;
        out += "</string></test>\n";
        out += latin;
        out += "    <edit name=\"family\" mode=\"append\" binding=\"weak\">"
               "<string>";
        out += first;
        out += "</string></edit>\n";
        // Then the pan-Unicode list, which is what Windows walks for the
        // characters that family lacks. Stating it here rather than leaving it
        // to the global order keeps a CJK page's own fonts from answering
        // first.
        unsigned pan_count = 0;
        const char* const* pan_list = fallback_order::PanUnicode(false, &pan_count);
        for (unsigned i = 0; pan_list != nullptr && i < pan_count; ++i) {
            const char* named = pan_list[i] != nullptr ? Surveyed(have, pan_list[i]) : nullptr;
            if (named == nullptr) {
                continue;
            }
            out += "    <edit name=\"family\" mode=\"append\" binding=\"weak\">"
                   "<string>";
            out += named;
            out += "</string></edit>\n";
        }
        out += "  </match>\n";
        Say("lang %s answers Han with %s", row.tag, first);
    }

    out += "  <match target=\"pattern\">\n";
    out += latin;
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

    // The bold marker, on its own and last. It names no family, so it applies
    // to whichever block the tag it was appended to selected, and only the
    // weight the sort scores against changes. A family with no bold face has
    // nothing that scores better and keeps the face it already answered with.
    // FC_WEIGHT_BOLD is 200, which is what IsFontBold compares against, so the
    // answer also comes back marked bold and Blink leaves it alone.
    out += "  <match target=\"pattern\">\n"
           "    <test name=\"lang\" compare=\"contains\"><string>";
    out += static_fontconfig::kBoldTag;
    out += "</string></test>\n"
           "    <edit name=\"weight\" mode=\"assign\"><int>200</int></edit>\n"
           "  </match>\n";
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
        if (std::ranges::find(dirs, path) == dirs.end()) {
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

}  // namespace

namespace static_fontconfig {

int TagFor(const char* family)
{
    if (family == nullptr) {
        return -1;
    }
    const std::vector<std::string>& order = Order();
    for (size_t i = 0; i < order.size() && i < kMaxTaggedFamilies; ++i) {
        if (strcasecmp(order[i].c_str(), family) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The locale layout_locale.cc's ToSkFontMgrLocale answers for a script subtag,
// which it reads before the language. Null where the locale carries none of
// the four scripts it names.
const char* ScriptLocale(const char* locale)
{
    struct Named
    {
        const char* subtag;
        const char* locale;
    };
    static const Named kNamed[] = {
        {.subtag = "hant", .locale = "zh-hant"}, {.subtag = "hans", .locale = "zh-hans"}, {.subtag = "jpan", .locale = "ja"},
        {.subtag = "kana", .locale = "ja"},      {.subtag = "hira", .locale = "ja"},      {.subtag = "kore", .locale = "ko"},
        {.subtag = "hang", .locale = "ko"},
    };
    for (const char* p = locale; *p != '\0' && *p != '.' && *p != '@';) {
        if (*p != '-' && *p != '_') {
            ++p;
            continue;
        }
        ++p;
        char sub[8] = {};
        unsigned k = 0;
        while (k + 1 < sizeof(sub) && p[k] != '\0' && p[k] != '-' && p[k] != '_' &&
               p[k] != '.' && p[k] != '@') {
            sub[k] = static_cast<char>(tolower(static_cast<unsigned char>(p[k])));
            ++k;
        }
        for (const Named& n : kNamed) {
            if (strcmp(sub, n.subtag) == 0) {
                return n.locale;
            }
        }
        p += k;
    }
    return nullptr;
}

int HanLocaleIndex(const char* locale)
{
    if (locale == nullptr || *locale == '\0') {
        return -1;
    }
    if (const char* named = ScriptLocale(locale); named != nullptr) {
        for (size_t i = 0; i < kLangHanCount; ++i) {
            if (strcasecmp(named, kLangHan[i].locale) == 0) {
                return static_cast<int>(i);
            }
        }
    }
    // Longest first, so zh-hant is not read as a bare zh and zh-tw not as one
    // of the zh-hans rows.
    int best = -1;
    size_t longest = 0;
    for (size_t i = 0; i < kLangHanCount; ++i) {
        const size_t len = strlen(kLangHan[i].locale);
        if (len > longest && strncasecmp(locale, kLangHan[i].locale, len) == 0) {
            longest = len;
            best = static_cast<int>(i);
        }
    }
    return best;
}

}  // namespace static_fontconfig

namespace {

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
                Say("pid %d serving %zu bytes for %s", getpid(), document.size(),
                    ConfigPath().c_str());
                // The order exists now, so the tags the hook names families by
                // can be answered.
                fallback_hook::Install();
                // The order is worked out from what this machine has, so the
                // only way to read it back is from the process that built it.
                if (const char* to = std::getenv("DWC_STATIC_FC_DUMP"); to != nullptr) {
                    if (FILE* f = std::fopen(to, "we"); f != nullptr) {
                        (void)std::fwrite(document.data(), 1, document.size(), f);
                        (void)std::fclose(f);
                    }
                }
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
int open(const char* path, const int flags, ...)
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
int open64(const char* path, const int flags, ...)
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
int openat(const int dirfd, const char* path, const int flags, ...)
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
int openat64(const int dirfd, const char* path, const int flags, ...)
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
