// Style inspections left as they are: the shapes they suggest either read
// worse against the sources being mirrored, or would change which overload
// is chosen if one were ever added.
// ReSharper disable RadGlobal

#include "dwrite_raster.h"

#include "bold_shaping.h"

// Skia compares these to zero exactly. A tolerance check would take a
// different branch than Windows does on the same input.
#pragma GCC diagnostic ignored "-Wfloat-equal"

#include "compat.h"
#include "dwrite_3.h"
#include "dwrite_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <dlfcn.h>
#include <unistd.h>

// ID2D1SimplifiedGeometrySink, reconstructed. IDWriteFontFace::GetGlyphRunOutline
// takes one of these, and include/dwrite.h only forward declares it, since the
// SDK puts it in d2d1.h, which is outside the DirectWrite header set this
// repository mirrors. Declared here in its documented method order, and only
// ever implemented, never consumed from DirectWrite.

typedef enum D2D1_FILL_MODE
{
    D2D1_FILL_MODE_ALTERNATE = 0,
    D2D1_FILL_MODE_WINDING = 1,
} D2D1_FILL_MODE;

typedef enum D2D1_PATH_SEGMENT
{
    D2D1_PATH_SEGMENT_NONE = 0,
    D2D1_PATH_SEGMENT_FORCE_UNSTROKED = 1,
    D2D1_PATH_SEGMENT_FORCE_ROUND_LINE_JOIN = 2,
} D2D1_PATH_SEGMENT;

typedef enum D2D1_FIGURE_BEGIN
{
    D2D1_FIGURE_BEGIN_FILLED = 0,
    D2D1_FIGURE_BEGIN_HOLLOW = 1,
} D2D1_FIGURE_BEGIN;

typedef enum D2D1_FIGURE_END
{
    D2D1_FIGURE_END_OPEN = 0,
    D2D1_FIGURE_END_CLOSED = 1,
} D2D1_FIGURE_END;

typedef struct D2D1_BEZIER_SEGMENT
{
    D2D1_POINT_2F point1;
    D2D1_POINT_2F point2;
    D2D1_POINT_2F point3;
} D2D1_BEZIER_SEGMENT;

interface ID2D1SimplifiedGeometrySink : IUnknown
{
    STDMETHOD_(void, SetFillMode)(D2D1_FILL_MODE fillMode) PURE;
    STDMETHOD_(void, SetSegmentFlags)(D2D1_PATH_SEGMENT vertexFlags) PURE;
    STDMETHOD_(void, BeginFigure)(D2D1_POINT_2F startPoint, D2D1_FIGURE_BEGIN figureBegin) PURE;
    STDMETHOD_(void, AddLines)(_In_reads_(pointsCount) const D2D1_POINT_2F* points,
                               UINT32 pointsCount) PURE;
    STDMETHOD_(void, AddBeziers)(_In_reads_(beziersCount) const D2D1_BEZIER_SEGMENT* beziers,
                                 UINT32 beziersCount) PURE;
    STDMETHOD_(void, EndFigure)(D2D1_FIGURE_END figureEnd) PURE;
    STDMETHOD(Close)() PURE;
};

namespace dwrite_raster {
namespace {

void Say(const char* what)
{
    (void)std::fprintf(stderr, "chromium-patch: dwrite: %s\n", what);
}

using PfnCreateFactory = HRESULT (*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);

struct Dwrite
{
    IDWriteFactory5* factory5 = nullptr;
    IDWriteFactory2* factory2 = nullptr;
    IDWriteInMemoryFontFileLoader* loader = nullptr;
    bool tried = false;
    bool ok = false;
};

// Three locks, so unrelated faces render concurrently instead of the whole
// shim serializing behind one mutex. What is shared is the factory, written
// once, and the face cache, a map. The DirectWrite calls themselves are
// per-face, so they take that face's own lock.
//
// Lock order is faces or collection first, then init. Nothing takes g_init
// and then reaches for either of the others.
std::mutex g_init_mutex;
std::mutex g_faces_mutex;
std::mutex g_collection_mutex;
Dwrite g_dw;

// A cached face and the lock that serializes DirectWrite work on it. The slot
// is stable in memory, so a caller can hold the pointer while the map grows.
struct FaceSlot
{
    IDWriteFontFace* face = nullptr;
    std::mutex mu;
};

// Loading the library and building a factory are deliberately separate, and
// happen on opposite sides of the fork.
//
// The dlopen has to be early: a renderer is forked from the zygote and can no
// longer open a file, so the mapping must already exist. The factory has to
// be late: DirectWrite starts threads and takes locks of its own while
// initializing, and only the forking thread survives a fork, so a factory
// built in the zygote leaves the child holding locks nobody will ever
// release, which hangs the renderer.
//
// Splitting them keeps both constraints: the file is mapped before the
// sandbox closes, and every DirectWrite object is created in the process that
// uses it. Nothing needs the filesystem after the dlopen, because the fonts
// come from memory.
PfnCreateFactory g_create = nullptr;

bool PreloadLibrary()
{
    if (g_create != nullptr) {
        return true;
    }
    void* handle = dlopen("libdwritecore.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* why = dlerror();
        (void)std::fprintf(stderr, "chromium-patch: dwrite: libdwritecore.so did not load (%s); "
                                   "glyphs stay with Fontations\n",
                           why != nullptr ? why : "no reason given");
        return false;
    }
    g_create = reinterpret_cast<PfnCreateFactory>(dlsym(handle, "DWriteCoreCreateFactory"));
    if (g_create == nullptr) {
        Say("libdwritecore.so has no DWriteCoreCreateFactory");
        return false;
    }
    return true;
}

// Guards itself, so every entry point can call it without holding anything
// wider. The mutex is also what publishes g_dw to the threads that read it
// afterwards without a lock, since they all pass through here first.
bool EnsureFactory()
{
    const std::lock_guard lock(g_init_mutex);
    if (g_dw.tried) {
        return g_dw.ok;
    }
    g_dw.tried = true;
    if (!PreloadLibrary()) {
        return false;
    }
    const PfnCreateFactory create = g_create;
    IUnknown* unknown = nullptr;
    if (FAILED(create(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory5), &unknown)) ||
        unknown == nullptr) {
        Say("no IDWriteFactory5");
        return false;
    }
    g_dw.factory5 = static_cast<IDWriteFactory5*>(unknown);
    if (FAILED(g_dw.factory5->QueryInterface(__uuidof(IDWriteFactory2),
                                             reinterpret_cast<void**>(&g_dw.factory2)))) {
        g_dw.factory2 = nullptr;
    }
    if (FAILED(g_dw.factory5->CreateInMemoryFontFileLoader(&g_dw.loader)) ||
        g_dw.loader == nullptr) {
        Say("no in-memory font file loader");
        return false;
    }
    if (FAILED(g_dw.factory5->RegisterFontFileLoader(g_dw.loader))) {
        Say("the in-memory loader would not register");
        return false;
    }
    g_dw.ok = true;
    Say("DWriteCore ready");
    return true;
}

// What identifies one font face: the bytes it was built from, which face of a
// collection it is, and which simulations DirectWrite is applying to it.
struct FaceKey
{
    const void* typeface;
    uint32_t face_index;
    bool simulate_bold;
    bool simulate_oblique;

    bool operator==(const FaceKey& other) const = default;
};

struct KeyHash
{
    size_t operator()(const FaceKey& k) const
    {
        return std::hash<const void*>{}(k.typeface) ^
               (std::hash<uint32_t>{}(k.face_index) << 1) ^
               (static_cast<size_t>(k.simulate_bold) << 2) ^
               (static_cast<size_t>(k.simulate_oblique) << 3);
    }
};

// The face at the typeface's own design-space position. A variable font's
// clone carries its coordinates on the SkTypeface, not in the tables, so a
// face built from the bytes alone is the default instance. DirectWrite's
// axis values are the same numbers under a byte-swapped tag.
IDWriteFontFace* ApplyVariations(IDWriteFontFace* face, const void* typeface,
                                 const DWRITE_FONT_SIMULATIONS sims)
{
    const std::vector<VariationCoord>* coords = ChromiumVariationCoords(typeface);
    if (coords == nullptr || face == nullptr) {
        return face;
    }
    IDWriteFontFace5* face5 = nullptr;
    if (FAILED(face->QueryInterface(__uuidof(IDWriteFontFace5),
                                    reinterpret_cast<void**>(&face5))) ||
        face5 == nullptr) {
        return face;
    }
    IDWriteFontResource* resource = nullptr;
    if (FAILED(face5->GetFontResource(&resource)) || resource == nullptr) {
        face5->Release();
        return face;
    }
    // Windows builds this list from the resource's default axis values with
    // the arguments overriding by tag (apply_fontargument_variation,
    // src/ports/SkTypeface_win_dw.cpp). The typeface's design position states
    // the same axis values, and GetDefaultFontAxisValues does not work in a
    // sandboxed renderer, so the position is what goes in.
    std::vector<DWRITE_FONT_AXIS_VALUE> values;
    values.reserve(coords->size());
    for (const VariationCoord& c : *coords) {
        values.push_back({static_cast<DWRITE_FONT_AXIS_TAG>(__builtin_bswap32(c.axis)),
                          c.value});
    }
    if (std::getenv("DWC_VAR_LOG") != nullptr) {
        (void)std::fprintf(stderr, "chromium-patch: var [%d]: passing:", ::getpid());
        for (const DWRITE_FONT_AXIS_VALUE& v : values) {
            const uint32_t t = __builtin_bswap32(static_cast<uint32_t>(v.axisTag));
            (void)std::fprintf(stderr, " %c%c%c%c=%g", t >> 24, (t >> 16) & 0xff,
                               (t >> 8) & 0xff, t & 0xff, static_cast<double>(v.value));
        }
        (void)std::fprintf(stderr, "\n");
    }
    IDWriteFontFace5* varied = nullptr;
    const HRESULT hr = resource->CreateFontFace(
        sims, values.data(), static_cast<UINT32>(values.size()), &varied);
    resource->Release();
    face5->Release();
    if (FAILED(hr) || varied == nullptr) {
        return face;
    }
    face->Release();
    return varied;
}

// One font face per typeface, built from the bytes typeface_bridge rebuilt,
// so DirectWrite never touches the filesystem. That is what makes this work in
// a sandboxed renderer.
FaceSlot* SlotFor(const void* typeface, const std::vector<uint8_t>& bytes,
                  const uint32_t face_index, const bool simulate_bold,
                  const bool simulate_oblique)
{
    // A simulated face is a different face for the same bytes, and so is every
    // other face of a collection, whose bytes are the whole file and therefore
    // the same for all of them.
    static std::unordered_map<FaceKey, std::unique_ptr<FaceSlot>, KeyHash> faces;
    const FaceKey key{typeface, face_index, simulate_bold, simulate_oblique};
    const std::lock_guard lock(g_faces_mutex);
    if (!EnsureFactory()) {
        return nullptr;
    }
    if (const auto it = faces.find(key); it != faces.end()) {
        return it->second.get();
    }
    // A collection carries 'ttcf' where a single face carries its SFNT
    // version, and DirectWrite has to be told which of the two it was given
    // before face_index means anything.
    const bool collection = bytes.size() >= 4 && bytes[0] == 't' && bytes[1] == 't' &&
                            bytes[2] == 'c' && bytes[3] == 'f';
    const DWRITE_FONT_FACE_TYPE type = collection ? DWRITE_FONT_FACE_TYPE_TRUETYPE_COLLECTION
                                                  : DWRITE_FONT_FACE_TYPE_TRUETYPE;
    IDWriteFontFace* face = nullptr;
    IDWriteFontFile* file = nullptr;
    if (!bytes.empty() &&
        SUCCEEDED(g_dw.loader->CreateInMemoryFontFileReference(
            g_dw.factory5, bytes.data(), static_cast<UINT32>(bytes.size()), nullptr, &file)) &&
        file != nullptr) {
        const auto sims = static_cast<DWRITE_FONT_SIMULATIONS>(
            (simulate_bold ? DWRITE_FONT_SIMULATIONS_BOLD : 0) |
            (simulate_oblique ? DWRITE_FONT_SIMULATIONS_OBLIQUE : 0));
        if (FAILED(g_dw.factory5->CreateFontFace(type, 1, &file, face_index, sims, &face))) {
            face = nullptr;
        }
        file->Release();
        face = ApplyVariations(face, typeface, sims);
    }
    if (face == nullptr) {
        Say("could not build a font face from the rebuilt bytes");
    }
    auto slot = std::make_unique<FaceSlot>();
    slot->face = face;
    FaceSlot* held = slot.get();
    faces.emplace(key, std::move(slot));
    return held;
}

uint8_t ApplyLut(const uint8_t v, const uint8_t* table)
{
    return table != nullptr ? table[v] : v;
}

size_t RowBytes(const skia_abi::Glyph& g)
{
    switch (g.mask_format) {
        case skia_abi::kBW: return (static_cast<size_t>(g.width) + 7) >> 3;
        case skia_abi::kLCD16: return static_cast<size_t>(g.width) * 2;
        case skia_abi::kARGB32: return static_cast<size_t>(g.width) * 4;
        default: return g.width;
    }
}

// src/ports/SkScalerContext_win_dw.cpp, isLCD - the Rec's mask format, not
// the glyph's, is what chooses the conversion.
bool IsLcd(const skia_abi::Rec& rec)
{
    return rec.mask_format == skia_abi::kLCD16;
}

uint16_t Pack888ToRGB16(const uint8_t r, const uint8_t g, const uint8_t b)
{
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// SkDWriteGeometrySink, src/utils/win/SkDWriteGeometrySink.cpp. DirectWrite
// only ever emits lines and cubics, so Skia recovers the quadratics itself
// before the path is built. Reproducing that recovery is what makes the verb
// sequence comparable with skrifa's, which emits quadratics directly.
class Sink final : public IDWriteGeometrySink
{
public:
    Sink(std::vector<uint8_t>* verbs, std::vector<path_abi::Point>* points)
        : verbs_(verbs), points_(points)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override
    {
        *object = this;
        return S_OK;
    }
    // Stack allocated for the length of one call, so the count is never read.
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) override {}
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) override {}

    void STDMETHODCALLTYPE BeginFigure(const D2D1_POINT_2F start, D2D1_FIGURE_BEGIN) override
    {
        started_ = false;
        current_ = start;
    }

    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* points, const UINT32 count) override
    {
        for (UINT32 i = 0; i < count; ++i) {
            if (CurrentIsNot(points[i])) {
                GoingTo(points[i]);
                Emit(path_abi::kLine, {points[i]});
            }
        }
    }

    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* beziers,
                                      const UINT32 count) override
    {
        for (UINT32 i = 0; i < count; ++i) {
            if (!CurrentIsNot(beziers[i].point1) && !CurrentIsNot(beziers[i].point2) &&
                !CurrentIsNot(beziers[i].point3)) {
                continue;
            }
            const D2D1_POINT_2F from = current_;
            GoingTo(beziers[i].point3);
            if (D2D1_POINT_2F quad_control{};
                IsQuadratic(from, beziers[i], &quad_control)) {
                Emit(path_abi::kQuad, {quad_control, beziers[i].point3});
            } else {
                Emit(path_abi::kCubic, {beziers[i].point1, beziers[i].point2, beziers[i].point3});
            }
        }
    }

    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END) override
    {
        if (started_) {
            verbs_->push_back(path_abi::kClose);
        }
    }

    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

private:
    void GoingTo(const D2D1_POINT_2F pt)
    {
        if (!started_) {
            started_ = true;
            Emit(path_abi::kMove, {current_});
        }
        current_ = pt;
    }

    bool CurrentIsNot(const D2D1_POINT_2F pt) const
    {
        return current_.x != pt.x || current_.y != pt.y;
    }

    void Emit(const uint8_t verb, const std::initializer_list<D2D1_POINT_2F> pts)
    {
        verbs_->push_back(verb);
        for (const D2D1_POINT_2F& p : pts) {
            points_->push_back({p.x, p.y});
        }
    }

    // SkFloatingPoint<float, 10>::AlmostEquals, src/utils/SkFloatUtils.h: the
    // two values must be within ten units in the last place of each other.
    static bool AlmostEquals(const float a, const float b)
    {
        uint32_t ia = 0;
        uint32_t ib = 0;
        std::memcpy(&ia, &a, sizeof(ia));
        std::memcpy(&ib, &b, sizeof(ib));
        constexpr uint32_t kSign = 0x80000000u;
        if ((ia & 0x7F800000u) == 0x7F800000u && (ia & 0x007FFFFFu) != 0) { return false; }
        if ((ib & 0x7F800000u) == 0x7F800000u && (ib & 0x007FFFFFu) != 0) { return false; }
        const uint32_t biased_a = (ia & kSign) != 0 ? ~ia + 1 : kSign | ia;
        const uint32_t biased_b = (ib & kSign) != 0 ? ~ib + 1 : kSign | ib;
        const uint32_t dist = biased_a >= biased_b ? biased_a - biased_b : biased_b - biased_a;
        return dist <= 10;
    }

    // check_quadratic, the same file: a cubic that is an exact promotion of a
    // quadratic has both control points two thirds of the way to one shared
    // point, which is the quadratic's own control point.
    static bool IsQuadratic(const D2D1_POINT_2F from, const D2D1_BEZIER_SEGMENT& b,
                            D2D1_POINT_2F* control)
    {
        const float dx10 = b.point1.x - from.x;
        const float dx23 = b.point2.x - b.point3.x;
        const float mid_x = from.x + dx10 * 3 / 2;
        if (!AlmostEquals(mid_x, dx23 * 3 / 2 + b.point3.x)) {
            return false;
        }
        const float dy10 = b.point1.y - from.y;
        const float dy23 = b.point2.y - b.point3.y;
        const float mid_y = from.y + dy10 * 3 / 2;
        if (!AlmostEquals(mid_y, dy23 * 3 / 2 + b.point3.y)) {
            return false;
        }
        *control = {mid_x, mid_y};
        return true;
    }

    std::vector<uint8_t>* verbs_;
    std::vector<path_abi::Point>* points_;
    bool started_ = false;
    D2D1_POINT_2F current_{};
};

}  // namespace

bool Preload()
{
    const std::lock_guard lock(g_init_mutex);
    return PreloadLibrary();
}

// The regular face of a family in the system collection, or null. The caller
// releases it.
IDWriteFont* RegularFace(IDWriteFontCollection* collection, const char* family)
{
    // The family names in the tables are ASCII, so widening a byte at a time
    // is the whole conversion.
    std::u16string wide;
    for (const char* p = family; *p != '\0'; ++p) {
        wide.push_back(static_cast<char16_t>(static_cast<unsigned char>(*p)));
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    IDWriteFontFamily* group = nullptr;
    IDWriteFont* font = nullptr;
    if (SUCCEEDED(collection->FindFamilyName(wide.c_str(), &index, &exists)) && exists &&
        SUCCEEDED(collection->GetFontFamily(index, &group)) && group != nullptr) {
        if (FAILED(group->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, &font))) {
            font = nullptr;
        }
    }
    if (group != nullptr) {
        group->Release();
    }
    return font;
}

bool FamilyCoverage(const char* family, const unsigned* points, const unsigned count,
                    bool* covers)
{
    if (family == nullptr || points == nullptr || covers == nullptr) {
        return false;
    }
    const std::lock_guard lock(g_collection_mutex);
    if (!EnsureFactory()) {
        return false;
    }
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(g_dw.factory5->GetSystemFontCollection(&collection, FALSE)) ||
        collection == nullptr) {
        return false;
    }
    IDWriteFont* font = RegularFace(collection, family);
    if (font != nullptr) {
        for (unsigned i = 0; i < count; ++i) {
            BOOL has = FALSE;
            covers[i] = SUCCEEDED(font->HasCharacter(points[i], &has)) && has;
        }
        font->Release();
    }
    collection->Release();
    return font != nullptr;
}

// Every family the system collection holds, so a caller that has to state
// something per family does not have to guess which ones exist.
bool FamilyNames(std::vector<std::string>* out)
{
    if (out == nullptr) {
        return false;
    }
    const std::lock_guard lock(g_collection_mutex);
    if (!EnsureFactory()) {
        return false;
    }
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(g_dw.factory5->GetSystemFontCollection(&collection, FALSE)) ||
        collection == nullptr) {
        return false;
    }
    const UINT32 count = collection->GetFontFamilyCount();
    for (UINT32 i = 0; i < count; ++i) {
        IDWriteFontFamily* group = nullptr;
        if (FAILED(collection->GetFontFamily(i, &group)) || group == nullptr) {
            continue;
        }
        IDWriteLocalizedStrings* names = nullptr;
        if (SUCCEEDED(group->GetFamilyNames(&names)) && names != nullptr &&
            names->GetCount() > 0) {
            UINT32 length = 0;
            if (SUCCEEDED(names->GetStringLength(0, &length)) && length > 0) {
                std::u16string wide(length + 1, u'\0');
                if (SUCCEEDED(names->GetString(0, wide.data(), length + 1))) {
                    std::string narrow;
                    for (const char16_t ch : wide) {
                        if (ch == u'\0') {
                            break;
                        }
                        // The names this is asked about are ASCII; anything
                        // else is a family no table names.
                        if (ch > 0x7F) {
                            narrow.clear();
                            break;
                        }
                        narrow.push_back(static_cast<char>(ch));
                    }
                    if (!narrow.empty()) {
                        out->push_back(std::move(narrow));
                    }
                }
            }
        }
        if (names != nullptr) {
            names->Release();
        }
        group->Release();
    }
    collection->Release();
    return !out->empty();
}

// The face GetFirstMatchingFont answers with, weight and slant both.
// GetFirstMatchingFont is what SkFontStyleSet_DirectWrite::matchStyle calls,
// so this is the rule itself rather than a restatement of it. The slant goes
// before the weight at the top of the scale: Arial at 900 italic is answered
// by Arial Black, which has no italic face, while 800 italic stays on an
// italic one.
bool FamilyMatchFace(const char* family, const int weight, const int style,
                     int* out_weight, bool* out_italic)
{
    if (family == nullptr) {
        return false;
    }
    const std::lock_guard lock(g_collection_mutex);
    if (!EnsureFactory()) {
        return false;
    }
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(g_dw.factory5->GetSystemFontCollection(&collection, FALSE)) ||
        collection == nullptr) {
        return false;
    }
    std::u16string wide;
    for (const char* p = family; *p != '\0'; ++p) {
        wide.push_back(static_cast<char16_t>(static_cast<unsigned char>(*p)));
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    IDWriteFontFamily* group = nullptr;
    IDWriteFont* font = nullptr;
    bool got = false;
    if (SUCCEEDED(collection->FindFamilyName(wide.c_str(), &index, &exists)) && exists &&
        SUCCEEDED(collection->GetFontFamily(index, &group)) && group != nullptr &&
        SUCCEEDED(group->GetFirstMatchingFont(
            static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STRETCH_NORMAL,
            static_cast<DWRITE_FONT_STYLE>(style), &font)) &&
        font != nullptr) {
        if (out_weight != nullptr) {
            *out_weight = static_cast<int>(font->GetWeight());
        }
        if (out_italic != nullptr) {
            // GetStyle reports the simulated style, so a face that only slants
            // under an oblique simulation reads as italic. The face underneath
            // is upright, and that is what has to be matched here.
            *out_italic = font->GetStyle() != DWRITE_FONT_STYLE_NORMAL &&
                          (font->GetSimulations() & DWRITE_FONT_SIMULATIONS_OBLIQUE) == 0;
        }
        got = true;
    }
    if (font != nullptr) {
        font->Release();
    }
    if (group != nullptr) {
        group->Release();
    }
    collection->Release();
    return got;
}

int FamilyMatchWeight(const char* family, const int weight)
{
    if (family == nullptr) {
        return 0;
    }
    const std::lock_guard lock(g_collection_mutex);
    if (!EnsureFactory()) {
        return 0;
    }
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(g_dw.factory5->GetSystemFontCollection(&collection, FALSE)) ||
        collection == nullptr) {
        return 0;
    }
    std::u16string wide;
    for (const char* p = family; *p != '\0'; ++p) {
        wide.push_back(static_cast<char16_t>(static_cast<unsigned char>(*p)));
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    IDWriteFontFamily* group = nullptr;
    IDWriteFont* font = nullptr;
    int picked = 0;
    if (SUCCEEDED(collection->FindFamilyName(wide.c_str(), &index, &exists)) && exists &&
        SUCCEEDED(collection->GetFontFamily(index, &group)) && group != nullptr &&
        SUCCEEDED(group->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(weight),
                                              DWRITE_FONT_STRETCH_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, &font)) &&
        font != nullptr) {
        picked = static_cast<int>(font->GetWeight());
    }
    if (font != nullptr) {
        font->Release();
    }
    if (group != nullptr) {
        group->Release();
    }
    collection->Release();
    return picked;
}

bool FamilyDirectory(const char* family, char* out, const size_t size)
{
    if (family == nullptr || out == nullptr || size == 0) {
        return false;
    }
    const std::lock_guard lock(g_collection_mutex);
    if (!EnsureFactory()) {
        return false;
    }
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(g_dw.factory5->GetSystemFontCollection(&collection, FALSE)) ||
        collection == nullptr) {
        return false;
    }

    bool done = false;
    IDWriteFont* font = RegularFace(collection, family);
    IDWriteFontFace* face = nullptr;
    if (font != nullptr && SUCCEEDED(font->CreateFontFace(&face)) && face != nullptr) {
        UINT32 files = 1;
        IDWriteFontFile* file = nullptr;
        if (SUCCEEDED(face->GetFiles(&files, &file)) && files == 1 && file != nullptr) {
            const void* key = nullptr;
            UINT32 key_size = 0;
            IDWriteFontFileLoader* loader = nullptr;
            IDWriteLocalFontFileLoader* local = nullptr;
            if (SUCCEEDED(file->GetReferenceKey(&key, &key_size)) &&
                SUCCEEDED(file->GetLoader(&loader)) && loader != nullptr &&
                SUCCEEDED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                                                 reinterpret_cast<void**>(&local))) &&
                local != nullptr) {
                UINT32 length = 0;
                if (SUCCEEDED(local->GetFilePathLengthFromKey(key, key_size, &length)) &&
                    length > 0) {
                    std::u16string path(length + 1, u'\0');
                    if (SUCCEEDED(local->GetFilePathFromKey(key, key_size, path.data(),
                                                            length + 1))) {
                        std::string narrow;
                        for (const char16_t ch : path) {
                            if (ch == u'\0') {
                                break;
                            }
                            narrow.push_back(static_cast<char>(ch));
                        }
                        const size_t cut = narrow.rfind('/');
                        if (cut != std::string::npos && cut > 0 && cut < size) {
                            std::memcpy(out, narrow.data(), cut);
                            out[cut] = '\0';
                            done = true;
                        }
                    }
                }
            }
            if (local != nullptr) {
                local->Release();
            }
            if (loader != nullptr) {
                loader->Release();
            }
            file->Release();
        }
    }
    if (face != nullptr) {
        face->Release();
    }
    if (font != nullptr) {
        font->Release();
    }
    collection->Release();
    return done;
}

bool Available()
{
    return EnsureFactory();
}

// SkScalerContext_DW::generateDWMetrics. The run is one glyph with a zero
// advance at the origin and the sub-pixel position in the transform, exactly
// as the raster path builds it, so the box asked for here is the box the
// glyph is later drawn into.
bool GlyphBounds(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const skia_abi::Glyph& glyph, const skia_abi::Rec& rec,
                 const windows_path::Decision& decision,
                 const windows_path::RenderingMode rendering_mode,
                 const windows_path::TextureType texture_type, int* left, int* top,
                 int* right, int* bottom, const uint32_t face_index,
                 const bool simulate_bold, const bool simulate_oblique)
{
    if (left == nullptr || top == nullptr || right == nullptr || bottom == nullptr) {
        return false;
    }
    FaceSlot* slot = SlotFor(typeface, font_bytes, face_index, simulate_bold, simulate_oblique);
    if (slot == nullptr || slot->face == nullptr) {
        return false;
    }
    // Only this face is held, so a thread measuring another one
    // does not wait here.
    const std::lock_guard lock(slot->mu);
    IDWriteFontFace* face = slot->face;

    DWRITE_MATRIX transform{};
    float scale_y = 0;
    windows_path::Matrix2x2 remaining;
    if (!windows_path::ComputeMatrices(rec, &scale_y, &remaining)) {
        return false;
    }
    transform.m11 = remaining.scale_x;
    transform.m12 = remaining.skew_y;
    transform.m21 = remaining.skew_x;
    transform.m22 = remaining.scale_y;
    transform.dx = static_cast<float>(glyph.SubX()) / 4.0f;
    transform.dy = static_cast<float>(glyph.SubY()) / 4.0f;

    FLOAT advance = 0.0f;
    UINT16 index = glyph.GlyphId();
    DWRITE_GLYPH_OFFSET offset{};
    DWRITE_GLYPH_RUN run{};
    run.glyphCount = 1;
    run.glyphAdvances = &advance;
    run.fontFace = face;
    run.fontEmSize = decision.text_size_render;
    run.bidiLevel = 0;
    run.glyphIndices = &index;
    run.isSideways = FALSE;
    run.glyphOffsets = &offset;

    IDWriteGlyphRunAnalysis* analysis = nullptr;
    HRESULT hr = E_FAIL;
    if (g_dw.factory2 != nullptr &&
        (decision.grid_fit_mode == windows_path::kGridFitDisabled ||
         decision.anti_alias_mode == windows_path::kAntiAliasGrayscale)) {
        hr = g_dw.factory2->CreateGlyphRunAnalysis(
            &run, &transform, static_cast<DWRITE_RENDERING_MODE>(rendering_mode),
            static_cast<DWRITE_MEASURING_MODE>(decision.measuring_mode),
            static_cast<DWRITE_GRID_FIT_MODE>(decision.grid_fit_mode),
            static_cast<DWRITE_TEXT_ANTIALIAS_MODE>(decision.anti_alias_mode), 0.0f, 0.0f,
            &analysis);
    } else if (g_dw.factory5 != nullptr) {
        hr = g_dw.factory5->CreateGlyphRunAnalysis(
            &run, 1.0f, &transform, static_cast<DWRITE_RENDERING_MODE>(rendering_mode),
            static_cast<DWRITE_MEASURING_MODE>(decision.measuring_mode), 0.0f, 0.0f,
            &analysis);
    }
    if (FAILED(hr) || analysis == nullptr) {
        return false;
    }

    RECT bbox{};
    hr = analysis->GetAlphaTextureBounds(static_cast<DWRITE_TEXTURE_TYPE>(texture_type),
                                         &bbox);
    analysis->Release();
    if (FAILED(hr)) {
        return false;
    }
    // GetAlphaTextureBounds succeeds but sometimes returns an empty rect for
    // small but not quite zero and large but not really large glyphs.
    if (bbox.left >= bbox.right || bbox.top >= bbox.bottom) {
        return false;
    }
    *left = bbox.left;
    *top = bbox.top;
    *right = bbox.right;
    *bottom = bbox.bottom;
    return true;
}

// SkScalerContext_DW::generateMetrics' advance, which is what decides where
// the next glyph goes. A GDI measuring mode takes it from
// GetGdiCompatibleGlyphMetrics and rounds the result; anything else takes it
// from GetDesignGlyphMetrics and leaves it fractional.
//
// The design-metrics branch is the one an ordinary page takes, and it is
// linear in the text size. That is why Windows advances step by exactly the
// same amount per pixel of size and a grid-fitted scaler's do not.
bool GlyphAdvance(const void* typeface, const std::vector<uint8_t>& font_bytes,
                  const uint16_t glyph_id, const skia_abi::Rec& rec,
                  const windows_path::Decision& decision, float* advance_x, float* advance_y,
                  const uint32_t face_index, const bool simulate_bold,
                  const bool simulate_oblique)
{
    if (advance_x == nullptr || advance_y == nullptr) {
        return false;
    }
    FaceSlot* slot = SlotFor(typeface, font_bytes, face_index, simulate_bold, simulate_oblique);
    if (slot == nullptr || slot->face == nullptr) {
        return false;
    }
    // Only this face is held, so a thread measuring another one
    // does not wait here.
    const std::lock_guard lock(slot->mu);
    IDWriteFontFace* face = slot->face;

    DWRITE_GLYPH_METRICS gm{};
    const UINT16 id = glyph_id;
    const bool gdi = decision.measuring_mode == windows_path::kMeasureGdiClassic ||
                     decision.measuring_mode == windows_path::kMeasureGdiNatural;
    HRESULT hr;
    if (gdi) {
        hr = face->GetGdiCompatibleGlyphMetrics(
            decision.text_size_measure, 1.0f, nullptr,
            decision.measuring_mode == windows_path::kMeasureGdiNatural ? TRUE : FALSE,
            &id, 1, &gm);
    } else {
        hr = face->GetDesignGlyphMetrics(&id, 1, &gm);
    }
    if (FAILED(hr)) {
        return false;
    }

    DWRITE_FONT_METRICS dwfm{};
    face->GetMetrics(&dwfm);
    if (dwfm.designUnitsPerEm == 0) {
        return false;
    }

    float x = decision.text_size_measure * static_cast<float>(gm.advanceWidth) /
              static_cast<float>(dwfm.designUnitsPerEm);
    if (gdi) {
        // DirectWrite produced 'compatible' metrics, but while close, the end
        // result is not always an integer as it would be with GDI.
        x = std::round(x);
    }

    // The advance is then mapped through sA, the same matrix the analysis
    // gets. That is the identity for ordinary axis-aligned text; for anything
    // stretched, skewed or rotated an unmapped advance puts every following
    // glyph in the wrong place.
    float scale_y = 0;
    windows_path::Matrix2x2 remaining;
    windows_path::ComputeMatrices(rec, &scale_y, &remaining);
    *advance_x = remaining.scale_x * x;
    *advance_y = remaining.skew_y * x;
    return true;
}

// SkScalerContext_DW::generateFontMetrics. Every field is the design value
// scaled by fTextSizeRender over the design units per em, with ascent
// negated.
//
// Blink then rounds ascent, descent and leading separately before adding them
// (SimpleFontData::PlatformInit, SetLineSpacing), so a fraction of a pixel of
// disagreement here becomes a whole pixel of line height.
bool FontMetrics(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const windows_path::Decision& decision, void* sk_font_metrics,
                 const uint32_t face_index, const bool simulate_bold,
                 const bool simulate_oblique)
{
    if (sk_font_metrics == nullptr) {
        return false;
    }
    FaceSlot* slot = SlotFor(typeface, font_bytes, face_index, simulate_bold, simulate_oblique);
    if (slot == nullptr || slot->face == nullptr) {
        return false;
    }
    // Only this face is held, so a thread measuring another one
    // does not wait here.
    const std::lock_guard lock(slot->mu);
    IDWriteFontFace* face = slot->face;

    DWRITE_FONT_METRICS dwfm{};
    const bool gdi = decision.measuring_mode == windows_path::kMeasureGdiClassic ||
                     decision.measuring_mode == windows_path::kMeasureGdiNatural;
    if (gdi) {
        DWRITE_MATRIX transform{};
        transform.m11 = 1.0f;
        transform.m22 = 1.0f;
        if (FAILED(face->GetGdiCompatibleMetrics(decision.text_size_render, 1.0f, &transform,
                                                 &dwfm))) {
            return false;
        }
    } else {
        face->GetMetrics(&dwfm);
    }
    if (dwfm.designUnitsPerEm == 0) {
        return false;
    }

    auto* m = static_cast<unsigned char*>(sk_font_metrics);
    const float size = decision.text_size_render;
    const auto upem = static_cast<float>(dwfm.designUnitsPerEm);
    const auto scaled = [&](const int design) { return size * static_cast<float>(design) / upem; };

    const float ascent = -scaled(dwfm.ascent);
    const float descent = scaled(dwfm.descent);
    const float leading = scaled(dwfm.lineGap);
    const float x_height = scaled(dwfm.xHeight);
    const float cap_height = scaled(dwfm.capHeight);
    const float underline_thickness = scaled(dwfm.underlineThickness);
    const float underline_position = -scaled(dwfm.underlinePosition);
    const float strikeout_thickness = scaled(dwfm.strikethroughThickness);
    const float strikeout_position = -scaled(dwfm.strikethroughPosition);

    std::memcpy(m + skia_abi::kFontMetricsAscent, &ascent, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsDescent, &descent, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsLeading, &leading, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsXHeight, &x_height, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsCapHeight, &cap_height, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsUnderlineThickness, &underline_thickness, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsUnderlinePosition, &underline_position, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsStrikeoutThickness, &strikeout_thickness, sizeof(float));
    std::memcpy(m + skia_abi::kFontMetricsStrikeoutPosition, &strikeout_position, sizeof(float));

    auto flags = skia_abi::Read<uint32_t>(m, skia_abi::kFontMetricsFlags);
    flags |= skia_abi::kUnderlineThicknessValid | skia_abi::kUnderlinePositionValid |
             skia_abi::kStrikeoutThicknessValid | skia_abi::kStrikeoutPositionValid;
    std::memcpy(m + skia_abi::kFontMetricsFlags, &flags, sizeof(flags));

    // SkScalerContext_DW leaves fAvgCharWidth alone, so on Windows it reaches
    // Blink as zero and SimpleFontData::PlatformInit measures 'x' instead.
    // Fontations fills it from OS/2, which Blink then prefers, and an input
    // with no size attribute is laid out from it.
    constexpr float kNoAvgCharWidth = 0;
    std::memcpy(m + skia_abi::kFontMetricsAvgCharWidth, &kNoAvgCharWidth, sizeof(float));

    // fTop, fBottom, fXMin and fXMax bound the ink rather than the line, and
    // Skia's own values are already in the struct.
    return true;
}

// SkScalerContext_DW::generatePath. DirectWrite is asked for the outline at
// fTextSizeRender with no hinting, exactly as the comment there says, and the
// sink above turns it into Skia's verbs.
bool GlyphOutline(const void* typeface, const std::vector<uint8_t>& font_bytes,
                  const uint16_t glyph_id, const float size, std::vector<uint8_t>* verbs,
                  std::vector<path_abi::Point>* points, const uint32_t face_index,
                  const bool simulate_bold, const bool simulate_oblique)
{
    FaceSlot* slot = SlotFor(typeface, font_bytes, face_index, simulate_bold, simulate_oblique);
    if (slot == nullptr || slot->face == nullptr) {
        return false;
    }
    // Only this face is held, so a thread measuring another one
    // does not wait here.
    const std::lock_guard lock(slot->mu);
    IDWriteFontFace* face = slot->face;
    verbs->clear();
    points->clear();
    Sink sink(verbs, points);
    UINT16 id = glyph_id;
    if (FAILED(face->GetGlyphRunOutline(size, &id, nullptr, nullptr, 1, FALSE, FALSE, &sink))) {
        return false;
    }
    return !verbs->empty();
}

bool RenderGlyph(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const skia_abi::Rec& rec, const skia_abi::Glyph& glyph,
                 const skia_abi::PreBlend& preblend, const windows_path::Decision& decision,
                 void* image_buffer, const uint32_t face_index, const bool simulate_bold,
                 const bool simulate_oblique)
{
    // Only a plain outline glyph. COLRv0, COLRv1 and embedded bitmaps are
    // drawn by Skia through paths an alpha texture cannot stand in for.
    if (glyph.scaler_bits != skia_abi::kFontationsPath) {
        return false;
    }
    if (glyph.mask_format == skia_abi::kARGB32 || glyph.width == 0 || glyph.height == 0 ||
        image_buffer == nullptr) {
        return false;
    }

    FaceSlot* slot = SlotFor(typeface, font_bytes, face_index, simulate_bold, simulate_oblique);
    if (slot == nullptr || slot->face == nullptr) {
        return false;
    }
    // Only this face is held, so a thread measuring another one
    // does not wait here.
    const std::lock_guard lock(slot->mu);
    IDWriteFontFace* face = slot->face;

    // getDWMaskBits: the transform carries the sub-pixel offset, and the run
    // is one glyph with a zero advance at the origin.
    //
    // The matrix is sA, the total matrix with the vertical scale taken out,
    // since Skia normalizes that scale into the em size and DirectWrite must
    // not apply it twice. DWRITE_MATRIX transposes the skews.
    DWRITE_MATRIX transform{};
    float scale_y = 0;
    windows_path::Matrix2x2 remaining;
    windows_path::ComputeMatrices(rec, &scale_y, &remaining);
    transform.m11 = remaining.scale_x;
    transform.m12 = remaining.skew_y;
    transform.m21 = remaining.skew_x;
    transform.m22 = remaining.scale_y;
    transform.dx = static_cast<float>(glyph.SubX()) / 4.0f;
    transform.dy = static_cast<float>(glyph.SubY()) / 4.0f;

    FLOAT advance = 0.0f;
    UINT16 index = glyph.GlyphId();
    DWRITE_GLYPH_OFFSET offset{};
    DWRITE_GLYPH_RUN run{};
    run.glyphCount = 1;
    run.glyphAdvances = &advance;
    run.fontFace = face;
    run.fontEmSize = decision.text_size_render;
    run.bidiLevel = 0;
    run.glyphIndices = &index;
    run.isSideways = FALSE;
    run.glyphOffsets = &offset;

    const auto rendering_mode = static_cast<DWRITE_RENDERING_MODE>(decision.rendering_mode);
    const auto measuring_mode = static_cast<DWRITE_MEASURING_MODE>(decision.measuring_mode);
    // windows_path names these the way SkScalerContext_DW does; DirectWrite has
    // its own enum with the same values, so the comparisons below are made in
    // DirectWrite's type rather than across the two.
    constexpr auto kDwriteAliased1x1 =
        static_cast<DWRITE_TEXTURE_TYPE>(windows_path::kTextureAliased1x1);
    constexpr auto kDwriteClearType3x1 =
        static_cast<DWRITE_TEXTURE_TYPE>(windows_path::kTextureClearType3x1);
    const auto texture_type = static_cast<DWRITE_TEXTURE_TYPE>(decision.texture_type);

    IDWriteGlyphRunAnalysis* analysis = nullptr;
    HRESULT hr = E_FAIL;
    // IDWriteFactory2::CreateGlyphRunAnalysis is very bad at aliased glyphs,
    // so Skia only uses it where it has to - grid fitting off, or grayscale.
    if (g_dw.factory2 != nullptr &&
        (decision.grid_fit_mode == windows_path::kGridFitDisabled ||
         decision.anti_alias_mode == windows_path::kAntiAliasGrayscale)) {
        hr = g_dw.factory2->CreateGlyphRunAnalysis(
            &run, &transform, rendering_mode, measuring_mode,
            static_cast<DWRITE_GRID_FIT_MODE>(decision.grid_fit_mode),
            static_cast<DWRITE_TEXT_ANTIALIAS_MODE>(decision.anti_alias_mode), 0.0f, 0.0f,
            &analysis);
    } else {
        hr = g_dw.factory5->CreateGlyphRunAnalysis(&run, 1.0f, &transform, rendering_mode,
                                                   measuring_mode, 0.0f, 0.0f, &analysis);
    }
    if (FAILED(hr) || analysis == nullptr) {
        return false;
    }

    const size_t pixels = static_cast<size_t>(glyph.width) * glyph.height;
    const size_t needed = texture_type == kDwriteClearType3x1 ? pixels * 3 : pixels;
    std::vector<uint8_t> bits(needed, 0);

    RECT bbox;
    bbox.left = glyph.left;
    bbox.top = glyph.top;
    bbox.right = glyph.left + glyph.width;
    bbox.bottom = glyph.top + glyph.height;

    hr = analysis->CreateAlphaTexture(texture_type, &bbox, bits.data(),
                                      static_cast<UINT32>(bits.size()));
    analysis->Release();
    if (FAILED(hr)) {
        return false;
    }

    // generateDWImage, with the conversions from the bottom of
    // SkScalerContext_win_dw.cpp. sk_apply_lut_if is the preblend, passing the
    // value through where the PreBlend is not applicable.
    const size_t row_bytes = RowBytes(glyph);
    const uint8_t* src = bits.data();
    auto* dst8 = static_cast<uint8_t*>(image_buffer);

    if (decision.rendering_mode == windows_path::kRenderAliased) {
        // BilevelToBW. The aliased texture is one byte per pixel holding 0 or
        // 0xFF, so masking each source byte with its own destination bit
        // packs eight pixels at a time.
        const int width = glyph.width;
        const size_t dst_rb = (static_cast<size_t>(width) + 7) >> 3;
        const int byte_count = width >> 3;
        const int bit_count = width & 7;
        uint8_t* dst = dst8;
        for (int row = 0; row < glyph.height; ++row) {
            for (int i = 0; i < byte_count; ++i) {
                unsigned byte = 0;
                byte |= src[0] & (1u << 7);
                byte |= src[1] & (1u << 6);
                byte |= src[2] & (1u << 5);
                byte |= src[3] & (1u << 4);
                byte |= src[4] & (1u << 3);
                byte |= src[5] & (1u << 2);
                byte |= src[6] & (1u << 1);
                byte |= src[7] & (1u << 0);
                dst[i] = static_cast<uint8_t>(byte);
                src += 8;
            }
            if (bit_count > 0) {
                unsigned byte = 0;
                unsigned mask = 0x80;
                for (int i = 0; i < bit_count; ++i) {
                    byte |= src[i] & mask;
                    mask >>= 1;
                }
                dst[byte_count] = static_cast<uint8_t>(byte);
            }
            src += bit_count;
            dst += dst_rb;
        }
        return true;
    }

    if (!IsLcd(rec)) {
        uint8_t* dst = dst8;
        if (texture_type == kDwriteAliased1x1) {
            // GrayscaleToA8
            for (int row = 0; row < glyph.height; ++row) {
                for (int i = 0; i < glyph.width; ++i) {
                    dst[i] = ApplyLut(*src++, preblend.g);
                }
                dst += row_bytes;
            }
        } else {
            // RGBToA8: the three subpixels averaged, then the green table.
            for (int row = 0; row < glyph.height; ++row) {
                for (int i = 0; i < glyph.width; ++i) {
                    const unsigned r = *src++;
                    const unsigned g = *src++;
                    const unsigned b = *src++;
                    dst[i] = ApplyLut(static_cast<uint8_t>((r + g + b) / 3), preblend.g);
                }
                dst += row_bytes;
            }
        }
        return true;
    }

    // RGBToLcd16, which needs the ClearType texture and a matching mask.
    if (texture_type != kDwriteClearType3x1 ||
        glyph.mask_format != skia_abi::kLCD16) {
        return false;
    }
    const bool rgb = (rec.flags & skia_abi::kLCD_BGROrder) == 0;
    for (int row = 0; row < glyph.height; ++row) {
        auto* dst = reinterpret_cast<uint16_t*>(dst8 + static_cast<size_t>(row) * row_bytes);
        for (int i = 0; i < glyph.width; ++i) {
            uint8_t r, g, b;
            if (rgb) {
                r = ApplyLut(*src++, preblend.r);
                g = ApplyLut(*src++, preblend.g);
                b = ApplyLut(*src++, preblend.b);
            } else {
                b = ApplyLut(*src++, preblend.b);
                g = ApplyLut(*src++, preblend.g);
                r = ApplyLut(*src++, preblend.r);
            }
            dst[i] = Pack888ToRGB16(r, g, b);
        }
    }
    return true;
}


}  // namespace dwrite_raster
