//+--------------------------------------------------------------------------
//
//  bold_fallback.cpp - the real bold face, mapped early and swapped in late.
//
//  Mirrors:
//
//    src/core/SkScalerContext.cpp   useStrokeForFakeBold, the stroke synthetic
//                                   bold becomes and the field it lands in
//    src/core/SkTextFormatParams.h  the width that stroke interpolates
//
//----------------------------------------------------------------------------

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bold_fallback.h"
#include "family_match.h"
#include "font_facts.h"
#include "parity_gate.h"

namespace {

constexpr int kFcResultMatch = 0;
// The weight Blink asked for is not recoverable. Synthetic bold is set for
// anything at or over kBoldThreshold, which is 600, and the description is
// reset to normal, so 700 is assumed here. That is the weight every `b`,
// `strong` and `font-weight: bold` carries.
constexpr float kAssumedWeight = 700;
constexpr uint32_t kNameTag = 0x6E616D65;  // 'name'
constexpr uint32_t kOs2Tag = 0x4F532F32;   // 'OS/2'
constexpr uint32_t kEbdtTag = 0x45424454;  // 'EBDT'

struct FcFontSet
{
    int nfont;
    void** fonts;
};

using InitLoadFn = void* (*)();
using ObjectSetCreateFn = void* (*)();
using ObjectSetAddFn = int (*)(void*, const char*);
using PatternCreateFn = void* (*)();
using FontListFn = FcFontSet* (*)(void*, void*, void*);
using GetStringFn = int (*)(const void*, const char*, int, unsigned char**);
using GetIntegerFn = int (*)(const void*, const char*, int, int*);

template <typename T>
T Sym(const char* name)
{
    return reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}

// One mapped file. The bytes are copied into a vector on first use, because
// that is what the raster path takes, and only the handful of faces a page
// actually substitutes ever pay for it.
struct Mapped
{
    const unsigned char* base = nullptr;
    size_t size = 0;
    std::vector<uint8_t> bytes;
    bool loaded = false;
};

std::unordered_map<std::string, Mapped>& Files()
{
    static std::unordered_map<std::string, Mapped> files;
    return files;
}

// The face DirectWrite would return for a bold request, by lowercased family
// name. A collection holds several faces in one file, so the index is part of
// the answer and the path alone is not.
struct Pick
{
    std::string path;
    uint32_t face_index = 0;
    float weight = 0;

    // Whether the family has an italic face at all. Windows would answer an
    // oblique request with it, so this face is not the right substitute then.
    bool has_italic = false;
};

std::unordered_map<std::string, Pick>& BoldByFamily()
{
    static std::unordered_map<std::string, Pick> by_family;
    return by_family;
}

std::mutex g_mutex;

std::string Lower(const char* s)
{
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

// SkFontMgr_win_dw.cpp's HasBitmapStrikes, which decides whether Windows keeps
// DirectWrite's bold simulation instead of letting Skia synthesize.
bool HasBitmapStrikes(const std::vector<uint8_t>& font)
{
    return font_facts::FindTable(font, kEbdtTag).data != nullptr;
}

// OS/2's usWeightClass, which is the weight fontconfig read to build its own.
float WeightOf(const std::vector<uint8_t>& font)
{
    const font_facts::Span table = font_facts::FindTable(font, kOs2Tag);
    if (table.data == nullptr || table.size < 6) {
        return 0;
    }
    return static_cast<float>(Be16(table.data + 4));
}

// The name table's family string, nameID 1. Windows records it as UTF-16BE
// under platform 3, which every font Chromium reaches here carries, and the
// ASCII of a family name is the low byte of each unit.
std::string FamilyName(const std::vector<uint8_t>& font)
{
    const font_facts::Span table = font_facts::FindTable(font, kNameTag);
    if (table.data == nullptr || table.size < 6) {
        return {};
    }
    const uint8_t* p = table.data;
    const unsigned count = Be16(p + 2);
    const unsigned storage = Be16(p + 4);
    if (6 + count * 12u > table.size) {
        return {};
    }
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t* rec = p + 6 + static_cast<size_t>(i) * 12;
        if (Be16(rec) != 3 || Be16(rec + 6) != 1) {
            continue;
        }
        const unsigned length = Be16(rec + 8);
        const unsigned offset = storage + Be16(rec + 10);
        if (offset + length > table.size || length < 2) {
            continue;
        }
        std::string out;
        for (unsigned k = 1; k < length; k += 2) {
            out.push_back(static_cast<char>(table.data[offset + k]));
        }
        return out;
    }
    return {};
}

void MapFile(const std::string& path)
{
    if (Files().contains(path)) {
        return;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    struct stat st{};
    Mapped entry;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        const void* m = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED) {
            entry.base = static_cast<const unsigned char*>(m);
            entry.size = static_cast<size_t>(st.st_size);
        }
    }
    close(fd);
    if (entry.base != nullptr) {
        Files().emplace(path, std::move(entry));
    }
}

}  // namespace

// The mark is one exact value Skia never writes. A tolerance check would catch
// widths it does.
#pragma GCC diagnostic ignored "-Wfloat-equal"

namespace bold_fallback {

void MapAtLoad()
{
    if (!chromium_patch::ParityWanted()) {
        return;
    }
    // Straight to the real fontconfig. This library interposes FcFontSort and
    // FcFontMatch, and running the query through its own hooks before it is
    // finished loading would be circular.
    const auto init_load = Sym<InitLoadFn>("FcInitLoadConfigAndFonts");
    const auto pattern_create = Sym<PatternCreateFn>("FcPatternCreate");
    const auto objectset_create = Sym<ObjectSetCreateFn>("FcObjectSetCreate");
    const auto objectset_add = Sym<ObjectSetAddFn>("FcObjectSetAdd");
    const auto font_list = Sym<FontListFn>("FcFontList");
    const auto get_string = Sym<GetStringFn>("FcPatternGetString");
    const auto get_integer = Sym<GetIntegerFn>("FcPatternGetInteger");
    if (init_load == nullptr || pattern_create == nullptr || objectset_create == nullptr ||
        objectset_add == nullptr || font_list == nullptr || get_string == nullptr ||
        get_integer == nullptr) {
        return;
    }

    void* config = init_load();
    void* pattern = pattern_create();
    void* objects = objectset_create();
    if (config == nullptr || pattern == nullptr || objects == nullptr) {
        return;
    }
    objectset_add(objects, "family");
    objectset_add(objects, "weight");
    objectset_add(objects, "slant");
    objectset_add(objects, "file");
    objectset_add(objects, "index");

    const FcFontSet* set = font_list(config, pattern, objects);
    if (set == nullptr) {
        return;
    }
    // Every upright face, by the family names it answers to. Windows picks
    // among all of them and not the bold ones alone, so a family with no bold
    // face comes out of this holding its regular, which then loses to the face
    // already in hand and leaves the run alone.
    std::unordered_map<std::string, Pick> best;
    std::unordered_set<std::string> has_italic;
    for (int i = 0; i < set->nfont; ++i) {
        const void* font = set->fonts[i];
        int fc_weight = 0;
        int slant = 0;
        int face_index = 0;
        unsigned char* file = nullptr;
        if (get_integer(font, "weight", 0, &fc_weight) != kFcResultMatch ||
            get_integer(font, "slant", 0, &slant) != kFcResultMatch ||
            get_string(font, "file", 0, &file) != kFcResultMatch) {
            continue;
        }
        if (slant != 0) {
            for (int n = 0; n < 255; ++n) {
                unsigned char* name = nullptr;
                if (get_string(font, "family", n, &name) != kFcResultMatch) {
                    break;
                }
                has_italic.insert(Lower(reinterpret_cast<const char*>(name)));
            }
            continue;
        }
        // A collection puts several faces in one file, so Nirmala UI and
        // Nirmala UI Bold share a path and differ only here.
        if (get_integer(font, "index", 0, &face_index) != kFcResultMatch) {
            face_index = 0;
        }
        const Pick pick{.path = std::string(reinterpret_cast<const char*>(file)),
                        .face_index = static_cast<uint32_t>(face_index),
                        .weight = family_match::OpenTypeWeight(fc_weight)};
        for (int n = 0; n < 255; ++n) {
            unsigned char* name = nullptr;
            if (get_string(font, "family", n, &name) != kFcResultMatch) {
                break;
            }
            const std::string key = Lower(reinterpret_cast<const char*>(name));
            if (const auto seen = best.find(key); seen == best.end()) {
                best.emplace(key, pick);
            } else if (family_match::BeatsForWindows(pick.weight, seen->second.weight,
                                                     kAssumedWeight)) {
                seen->second = pick;
            }
        }
    }
    for (auto& [family, winner] : best) {
        winner.has_italic = has_italic.contains(family);
        BoldByFamily().emplace(family, winner);
        MapFile(winner.path);
    }
}

// Written into fReservedAlign, which Skia zeroes and never reads.
constexpr uint8_t kMarked = 0xB0;

bool ClearSyntheticBold(void* rec, const std::vector<uint8_t>& font)
{
    if (rec == nullptr) {
        return false;
    }
    const auto flags = skia_abi::Read<uint16_t>(rec, skia_abi::kRecFlags);
    if ((flags & skia_abi::kEmbolden) == 0) {
        return false;
    }
    // Windows synthesizes only over a face lighter than semibold
    // (font_cache_skia_win.cc `!typeface->isBold()`, which is weight 600),
    // while Linux strokes whenever the request sits 200 above the face
    // (font_cache_skia.cc). A face already at 600 or more keeps its own
    // outlines on Windows, so the stroke goes and nothing is substituted.
    if (WeightOf(font) >= 600) {
        const auto cleared = static_cast<uint16_t>(flags & ~skia_abi::kEmbolden);
        std::memcpy(static_cast<unsigned char*>(rec) + skia_abi::kRecFlags, &cleared,
                    sizeof(cleared));
        return true;
    }
    // Synthetic italic, which Blink writes as the SkFont's skew
    // (font_platform_data.cc, `font.setSkewX(synthetic_italic_ ? -1/4 : 0)`)
    // and SkScalerContext copies into fPreSkewX. Windows draws such a run
    // with the family's Bold Italic face; only upright faces are mapped here,
    // so the run keeps the synthetic bold rather than taking an upright Bold.
    const bool oblique = IsOblique(skia_abi::Read<float>(rec, skia_abi::kRecPreSkewX));
    // ReSharper disable once CppDFAConstantConditions
    if (const Face bold = RealBoldFor(font, oblique); bold.bytes == nullptr && !bold.simulate) {
        return false;
    }
    const auto cleared = static_cast<uint16_t>(flags & ~skia_abi::kEmbolden);
    std::memcpy(static_cast<unsigned char*>(rec) + skia_abi::kRecFlags, &cleared,
                sizeof(cleared));
    std::memcpy(static_cast<unsigned char*>(rec) + skia_abi::kRecReserved, &kMarked,
                sizeof(kMarked));
    return true;
}

bool WasMarked(const void* rec)
{
    return rec != nullptr && skia_abi::Read<uint8_t>(rec, skia_abi::kRecReserved) == kMarked;
}

bool IsOblique(const float pre_skew_x) { return pre_skew_x != 0; }

Face RealBoldFor(const std::vector<uint8_t>& font, const bool oblique)
{
    if (font.empty() || BoldByFamily().empty()) {
        return {};
    }
    const std::string family = FamilyName(font);
    if (family.empty()) {
        return {};
    }
    const std::lock_guard lock(g_mutex);
    const auto named = BoldByFamily().find(Lower(family.c_str()));
    // An unknown family says nothing about whether Windows found a bold face
    // for it, so neither substituting nor simulating is warranted.
    if (named == BoldByFamily().end()) {
        return {};
    }
    // Nothing heavier than what is already in hand means Windows was given
    // this same face. It synthesizes as Linux does, unless the face carries
    // bitmap strikes, which is the one case FirstMatchingFontWithoutSimulations
    // leaves DirectWrite's own bold simulation on.
    if (named->second.weight <= WeightOf(font)) {
        return HasBitmapStrikes(font) ? Face{.bytes = nullptr, .face_index = 0, .simulate = true} : Face{};
    }
    // An oblique run whose family has an italic face. Windows draws it with
    // that face, so the upright bold mapped here is the wrong answer. A family
    // with no italic face keeps this one, which is what Windows falls back to.
    if (oblique && named->second.has_italic) {
        return {};
    }
    const auto file = Files().find(named->second.path);
    if (file == Files().end()) {
        return {};
    }
    Mapped& mapped = file->second;
    if (!mapped.loaded) {
        mapped.loaded = true;
        mapped.bytes.assign(mapped.base, mapped.base + mapped.size);
    }
    return Face{.bytes = &mapped.bytes, .face_index = named->second.face_index};
}


bool BoldFileFor(const char* family, const bool oblique, const char** path,
                 uint32_t* face_index)
{
    if (family == nullptr || path == nullptr || face_index == nullptr) {
        return false;
    }
    std::string key(family);
    for (char& c : key) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    const auto& by_family = BoldByFamily();
    const auto found = by_family.find(key);
    if (found == by_family.end() || found->second.path.empty()) {
        return false;
    }
    if (oblique && found->second.has_italic) {
        return false;
    }
    *path = found->second.path.c_str();
    *face_index = found->second.face_index;
    return true;
}

}  // namespace bold_fallback
