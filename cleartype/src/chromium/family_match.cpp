//+--------------------------------------------------------------------------
//
//  family_match.cpp - the face within a family that Windows would pick.
//
//  Mirrors, from src/ports/SkFontConfigInterface_direct.cpp:
//
//    fcpattern_from_skfontstyle   the weight the query carries
//    skfontstyle_from_fcpattern   the weight a candidate carries
//    map_ranges, map_range        the piecewise linear map between the scales
//    MatchFont                    which entry of the set is taken
//
//  The Windows rule has no source to cite. GetFirstMatchingFont takes the
//  face whose OpenType weight is nearest the one asked for, and on a tie the
//  face farther from 400, which is the lighter one below 400 and the heavier
//  one above. Neither the documented DWrite algorithm nor CSS Fonts 4
//  matching describes this.
//
//----------------------------------------------------------------------------

// ReSharper disable CppParameterMayBeConstPtrOrRef

#include <cstdio>
#include <cstdlib>
#include <strings.h>

#include <dlfcn.h>

#include "family_match.h"
#include "parity_gate.h"

namespace {

constexpr int kFcResultMatch = 0;

struct FcFontSet
{
    int nfont;
    void** fonts;
};

using PatternGetStringFn = int (*)(const void*, const char*, int, unsigned char**);
using PatternGetIntegerFn = int (*)(const void*, const char*, int, int*);

template <typename T>
T Sym(const char* name)
{
    return reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}

// A weight on each scale, as the two tables in SkFontConfigInterface_direct.cpp
// pair them. fontconfig's own FcWeightFromOpenType uses these same pairs, so a
// candidate's fontconfig weight maps back to the weight its OS/2 table holds.
struct WeightPair
{
    float fc;
    float open_type;
};

constexpr WeightPair kWeights[] = {
    {0, 100},     // THIN
    {40, 200},    // EXTRALIGHT
    {50, 300},    // LIGHT
    {55, 350},    // DEMILIGHT
    {75, 380},    // BOOK
    {80, 400},    // REGULAR
    {100, 500},   // MEDIUM
    {180, 600},   // DEMIBOLD
    {200, 700},   // BOLD
    {205, 800},   // EXTRABOLD
    {210, 900},   // BLACK
    {215, 1000},  // EXTRABLACK
};

// map_ranges, on the fontconfig column.
float OpenTypeWeightImpl(const int fc_weight)
{
    const auto value = static_cast<float>(fc_weight);
    if (value < kWeights[0].fc) {
        return kWeights[0].open_type;
    }
    constexpr int last = static_cast<int>(sizeof(kWeights) / sizeof(kWeights[0])) - 1;
    for (int i = 0; i < last; ++i) {
        if (value < kWeights[i + 1].fc) {
            const WeightPair& lo = kWeights[i];
            const WeightPair& hi = kWeights[i + 1];
            return lo.open_type +
                   (value - lo.fc) * (hi.open_type - lo.open_type) / (hi.fc - lo.fc);
        }
    }
    return kWeights[last].open_type;
}

// GetFirstMatchingFont's pick, among the weights the set offers.
bool BeatsImpl(const float candidate, const float best, const float wanted)
{
    const float d_candidate = candidate < wanted ? wanted - candidate : candidate - wanted;
    const float d_best = best < wanted ? wanted - best : best - wanted;
    if (d_candidate < d_best) {
        return true;
    }
    if (d_best < d_candidate) {
        return false;
    }
    const float from_normal_candidate = candidate < 400 ? 400 - candidate : candidate - 400;
    const float from_normal_best = best < 400 ? 400 - best : best - 400;
    return from_normal_candidate > from_normal_best;
}

bool HasFamily(const PatternGetStringFn get_string, const void* font, const char* family)
{
    // A face answers to every name in its list, and the one Chromium asked for
    // is not always the first: seguisb.ttf is "Segoe UI, Segoe UI Semibold".
    // The 255 is the bound MatchFont walks the same list with.
    for (int n = 0; n < 255; ++n) {
        unsigned char* name = nullptr;
        if (get_string(font, "family", n, &name) != kFcResultMatch) {
            return false;
        }
        if (strcasecmp(reinterpret_cast<const char*>(name), family) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace family_match {

float OpenTypeWeight(const int fc_weight) { return OpenTypeWeightImpl(fc_weight); }

bool BeatsForWindows(const float candidate, const float best, const float wanted)
{
    return BeatsImpl(candidate, best, wanted);
}

void ReorderForWindows(const void* pattern, void* sorted)
{
    auto* set = static_cast<FcFontSet*>(sorted);
    if (!chromium_patch::ParityWanted() || set == nullptr || set->nfont <= 1 ||
        pattern == nullptr) {
        return;
    }

    static const auto get_string = Sym<PatternGetStringFn>("FcPatternGetString");
    static const auto get_integer = Sym<PatternGetIntegerFn>("FcPatternGetInteger");
    if (get_string == nullptr || get_integer == nullptr) {
        return;
    }

    // Only the sort matchFamilyName does. It is the one query that asks for
    // SFNT, so the per-character fallback sort in ui/gfx/font_fallback_linux.cc
    // never arrives here. fallback_order.cpp answers that one by charset, and
    // reordering it would fight that.
    unsigned char* wrapper = nullptr;
    if (get_string(pattern, "fontwrapper", 0, &wrapper) != kFcResultMatch ||
        strcasecmp(reinterpret_cast<const char*>(wrapper), "SFNT") != 0) {
        return;
    }
    unsigned char* wanted_family = nullptr;
    if (get_string(pattern, "family", 0, &wanted_family) != kFcResultMatch) {
        return;
    }
    const auto* family = reinterpret_cast<const char*>(wanted_family);

    int fc_weight = 80;
    if (get_integer(pattern, "weight", 0, &fc_weight) != kFcResultMatch) {
        return;
    }
    const float wanted = OpenTypeWeightImpl(fc_weight);

    // Slant is left to fontconfig. Candidates are held to the slant of the
    // face it already chose, so an italic request keeps picking among italics
    // and only the weight within that group is decided here.
    int slant = 0;
    if (get_integer(set->fonts[0], "slant", 0, &slant) != kFcResultMatch) {
        return;
    }

    // Whether the family exists at all stays fontconfig's answer. When it
    // substitutes something that does not carry the name, MatchFont rejects
    // the set and Blink moves to the next family in the CSS list; promoting a
    // face that does carry it would make an unavailable family available.
    if (!HasFamily(get_string, set->fonts[0], family)) {
        return;
    }

    int winner = -1;
    float best = 0;
    for (int i = 0; i < set->nfont; ++i) {
        void* font = set->fonts[i];
        int font_slant = 0;
        int font_weight = 0;
        unsigned char* file = nullptr;
        if (get_integer(font, "slant", 0, &font_slant) != kFcResultMatch ||
            font_slant != slant ||
            get_integer(font, "weight", 0, &font_weight) != kFcResultMatch ||
            get_string(font, "file", 0, &file) != kFcResultMatch ||
            !HasFamily(get_string, font, family)) {
            continue;
        }
        const float weight = OpenTypeWeightImpl(font_weight);
        if (winner < 0 || BeatsImpl(weight, best, wanted)) {
            winner = i;
            best = weight;
        }
    }

    // Nothing to move when no candidate matched or the winner is already
    // first, which is the common case.
    if (winner <= 0) {
        return;
    }
    if (std::getenv("DWC_FAMILY_MATCH_LOG") != nullptr) {
        unsigned char* was = nullptr;
        unsigned char* now = nullptr;
        get_string(set->fonts[0], "file", 0, &was);
        get_string(set->fonts[winner], "file", 0, &now);
        std::fprintf(stderr, "[family_match] %s fc_weight=%d wanted=%.1f  %s -> %s\n",
                     family, fc_weight, static_cast<double>(wanted),
                     was ? reinterpret_cast<const char*>(was) : "?",
                     now ? reinterpret_cast<const char*>(now) : "?");
    }
    void* chosen = set->fonts[winner];
    for (int i = winner; i > 0; --i) {
        set->fonts[i] = set->fonts[i - 1];
    }
    set->fonts[0] = chosen;
}

}  // namespace family_match
