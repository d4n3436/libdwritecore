//+--------------------------------------------------------------------------
//
//  bold_shaping.cpp - shape a substituted bold run with the bold face.
//
//  bold_fallback.cpp swaps the face at the point a glyph is measured and
//  drawn, which fixes advances and ink. Positioning is decided above that, in
//  HarfBuzz, and HarfBuzz is looking at the regular face: Blink caches one
//  hb_face per SkTypeface (HarfBuzzFontCache::GetOrCreate, keyed on
//  FontPlatformData::UniqueID, which is Typeface()->uniqueID()), and
//  PlatformFallbackFontForCharacter normalizes the weight before asking for
//  the FontPlatformData, so a bold fallback run and a regular one share it.
//  Kerning, mark attachment and every other GPOS adjustment therefore come
//  from the regular face's tables.
//
//  So the face is swapped a second time, here. The glyph ids and advances
//  stay Skia's, which the scaler patch has already made the bold face's; only
//  the tables HarfBuzz reads for positioning change. That works because the
//  two faces share a glyph order, which is checked before any substitution.
//
//  Mirrors:
//
//    blink/renderer/platform/fonts/shaping/harfbuzz_face.cc
//                                     CreateFace, CreateHarfBuzzFontData and
//                                     GetScaledFont, whose construction this
//                                     repeats with the bold face
//    blink/renderer/platform/fonts/shaping/harfbuzz_font_data.h
//                                     HarfBuzzFontData, whose SkFont says
//                                     whether the run is a synthetic bold
//    blink/renderer/platform/fonts/linux/font_cache_linux.cc
//                                     PlatformFallbackFontForCharacter, where
//                                     the weight is dropped
//    include/core/SkFont.h            the member order and PrivFlags read out
//                                     of that SkFont
//
//----------------------------------------------------------------------------

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include "bold_fallback.h"
#include "bold_shaping.h"
#include "code_patch.h"
#include "parity_gate.h"

namespace {

void Report(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void Report(const char* fmt, ...)
{
    static const bool on = std::getenv("DWC_BOLD_SHAPING_LOG") != nullptr;
    if (!on) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[hbbold] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

// SkFont, from include/core/SkFont.h. Its members are sk_sp<SkTypeface>,
// then three SkScalars, then three bytes of enum, and nothing before them.
constexpr size_t kFontTypeface = 0;
constexpr size_t kFontSize = 8;
constexpr size_t kFontSkewX = 16;
constexpr size_t kFontFlags = 20;
constexpr size_t kFontEdging = 21;
constexpr size_t kFontHinting = 22;
constexpr uint8_t kEmboldenPrivFlag = 1 << 4;

// HarfBuzzFontData holds unscaled_font_ and then font_. It is a
// GarbageCollected struct whose Trace is not virtual, so font_ sits one
// pointer in, but a build that gives it a vtable would push it one further.
// Both are tried and the one that answers is kept.
constexpr size_t kDataFontNoVtable = 8;
constexpr size_t kDataFontVtable = 16;

template <typename T>
T ReadAt(const void* base, const size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset,
                sizeof(T));
    return value;
}

// Whether the bytes at `font` look like an SkFont over a typeface the scaler
// hooks have already read. Every test is either a table lookup or a field
// whose range is known, so a wrong offset, or a font Blink did not bind, fails
// it without anything being dereferenced. That matters: the user_data on a
// browser-process font is not a HarfBuzzFontData at all, and the pointer this
// reads out of it is not a typeface.
bool LooksLikeSkFont(const void* font)
{
    auto* typeface = ReadAt<void*>(font, kFontTypeface);
    if (typeface == nullptr || (reinterpret_cast<uintptr_t>(typeface) & 7) != 0 ||
        ChromiumFontBytes(typeface) == nullptr) {
        return false;
    }

    const float size = ReadAt<float>(font, kFontSize);
    if (!std::isfinite(size) || size <= 0 || size > 4096) {
        return false;
    }
    // Only six PrivFlags are defined, and SkFontHinting and SkFont::Edging
    // have four and three values.
    return ReadAt<uint8_t>(font, kFontFlags) <= 0x3F &&
           ReadAt<uint8_t>(font, kFontEdging) <= 2 &&
           ReadAt<uint8_t>(font, kFontHinting) <= 3;
}

// Where font_ sits inside a HarfBuzzFontData. Decided once, on the first
// user_data that answers, since it cannot vary within a process. Zero until
// then, and zero again for a build where neither offset works.
size_t g_font_offset = 0;

const void* SkFontIn(const void* data)
{
    if (g_font_offset != 0) {
        const auto* font =
            static_cast<const unsigned char*>(data) + g_font_offset;
        return LooksLikeSkFont(font) ? font : nullptr;
    }

    for (const size_t offset : {kDataFontNoVtable, kDataFontVtable}) {
        const auto* font = static_cast<const unsigned char*>(data) + offset;
        if (LooksLikeSkFont(font)) {
            g_font_offset = offset;
            Report("HarfBuzzFontData holds its SkFont at +%zu", offset);
            return font;
        }
    }
    return nullptr;
}

//----------------------------------------------------------------------------
// The HarfBuzz entry points this file calls, and the two it interposes.
//----------------------------------------------------------------------------

template <typename Fn>
Fn Hb(const char* name)
{
    return reinterpret_cast<Fn>(hb_abi::Real(name));
}

struct Funcs
{
    hb_abi::ShapeFn shape = nullptr;
    hb_abi::ShapeFullFn shape_full = nullptr;
    hb_abi::BlobCreateFn blob_create = nullptr;
    hb_abi::BlobDestroyFn blob_destroy = nullptr;
    hb_abi::FaceCreateFn face_create = nullptr;
    hb_abi::FaceDestroyFn face_destroy = nullptr;
    hb_abi::FaceGetUpemFn face_upem = nullptr;
    hb_abi::FaceGetGlyphCountFn face_glyphs = nullptr;
    hb_abi::FontCreateFn font_create = nullptr;
    hb_abi::FontCreateSubFontFn font_sub = nullptr;
    hb_abi::FontDestroyFn font_destroy = nullptr;
    hb_abi::FontGetFaceFn font_face = nullptr;
    hb_abi::FontGetScaleFn font_get_scale = nullptr;
    hb_abi::FontSetScaleFn font_set_scale = nullptr;
    hb_abi::FontGetPtemFn font_get_ptem = nullptr;
    hb_abi::FontSetPtemFn font_set_ptem = nullptr;
    hb_abi::FontSetFuncsFn font_set_funcs = nullptr;
    hb_abi::OtFontSetFuncsFn ot_set_funcs = nullptr;
    hb_abi::OtVarGetAxisCountFn var_axes = nullptr;
    hb_abi::FontFuncsGetEmptyFn funcs_empty = nullptr;
    hb_abi::FaceGetEmptyFn face_empty = nullptr;

    bool complete() const
    {
        return (shape || shape_full) && blob_create && blob_destroy && face_create &&
               face_destroy &&
               face_upem && face_glyphs && font_create && font_sub &&
               font_destroy && font_face && font_set_scale && font_set_ptem &&
               font_set_funcs &&
               ot_set_funcs && var_axes;
    }
};

const Funcs& Real()
{
    static const Funcs f = [] {
        Funcs r;
        r.shape = Hb<hb_abi::ShapeFn>("hb_shape");
        r.shape_full = Hb<hb_abi::ShapeFullFn>("hb_shape_full");
        r.blob_create = Hb<hb_abi::BlobCreateFn>("hb_blob_create");
        r.blob_destroy = Hb<hb_abi::BlobDestroyFn>("hb_blob_destroy");
        r.face_create = Hb<hb_abi::FaceCreateFn>("hb_face_create");
        r.face_destroy = Hb<hb_abi::FaceDestroyFn>("hb_face_destroy");
        r.face_upem = Hb<hb_abi::FaceGetUpemFn>("hb_face_get_upem");
        r.face_glyphs = Hb<hb_abi::FaceGetGlyphCountFn>("hb_face_get_glyph_count");
        r.font_create = Hb<hb_abi::FontCreateFn>("hb_font_create");
        r.font_sub = Hb<hb_abi::FontCreateSubFontFn>("hb_font_create_sub_font");
        r.font_destroy = Hb<hb_abi::FontDestroyFn>("hb_font_destroy");
        r.font_face = Hb<hb_abi::FontGetFaceFn>("hb_font_get_face");
        r.font_get_scale = Hb<hb_abi::FontGetScaleFn>("hb_font_get_scale");
        r.font_set_scale = Hb<hb_abi::FontSetScaleFn>("hb_font_set_scale");
        r.font_get_ptem = Hb<hb_abi::FontGetPtemFn>("hb_font_get_ptem");
        r.font_set_ptem = Hb<hb_abi::FontSetPtemFn>("hb_font_set_ptem");
        r.font_set_funcs = Hb<hb_abi::FontSetFuncsFn>("hb_font_set_funcs");
        r.ot_set_funcs = Hb<hb_abi::OtFontSetFuncsFn>("hb_ot_font_set_funcs");
        r.var_axes = Hb<hb_abi::OtVarGetAxisCountFn>("hb_ot_var_get_axis_count");
        r.funcs_empty = Hb<hb_abi::FontFuncsGetEmptyFn>("hb_font_funcs_get_empty");
        r.face_empty = Hb<hb_abi::FaceGetEmptyFn>("hb_face_get_empty");
        return r;
    }();
    return f;
}

// HarfBuzz's own shaping. hb_shape is one line over hb_shape_full, so calling
// hb_shape_full is what HarfBuzz would have done and, where hb_shape has been
// replaced in place, the only way past the replacement.
void Shape(const Funcs& hb, hb_font_t* font, hb_buffer_t* buffer,
           const hb_feature_t* features, unsigned int num_features)
{
    if (hb.shape_full != nullptr) {
        (void)hb.shape_full(font, buffer, features, num_features, nullptr);
    } else if (hb.shape != nullptr) {
        hb.shape(font, buffer, features, num_features);
    }
}

//----------------------------------------------------------------------------
// What Blink set on each font, since HarfBuzz has no way to ask for it back.
//----------------------------------------------------------------------------

struct Bound
{
    hb_font_funcs_t* klass = nullptr;
    void* data = nullptr;
};

std::mutex g_bound_mutex;
std::unordered_map<hb_font_t*, Bound>& Bounds()
{
    static std::unordered_map<hb_font_t*, Bound> map;
    return map;
}

bool BoundInFont(hb_font_t* font, Bound* out);
void CheckFontLayout(hb_font_t* font, const Bound& recorded);

bool BoundOn(hb_font_t* font, Bound* out)
{
    const std::lock_guard lock(g_bound_mutex);
    auto& map = Bounds();
    const auto found = map.find(font);
    if (found == map.end()) {
        return false;
    }
    *out = found->second;
    return true;
}

//----------------------------------------------------------------------------
// The bold font built beside Blink's, one per font it was asked to stand in
// for. Blink's FontGlobalContext is per thread and so is the font it hands
// hb_shape, so these are too, which keeps an hb_font_t off two threads at
// once without a lock around shaping.
//----------------------------------------------------------------------------

struct Substitute
{
    hb_font_t* parent = nullptr;
    hb_font_t* font = nullptr;
    hb_face_t* face = nullptr;
    // Nothing to swap in for this font. Kept so the decision is made once.
    bool declined = false;
};

std::unordered_map<hb_font_t*, Substitute>& Substitutes()
{
    static thread_local std::unordered_map<hb_font_t*, Substitute> map;
    return map;
}

// Build the bold face and the font over it, the way CreateFace and
// CreateHarfBuzzFontData build Blink's. Declines wherever the bold face
// cannot stand in for the regular one glyph for glyph.
Substitute Build(hb_font_t* font, const Bound& bound,
                 const bold_fallback::Face& bold)
{
    const Funcs& hb = Real();
    Substitute out;
    out.declined = true;

    hb_face_t* regular = hb.font_face(font);
    if (regular == nullptr) {
        return out;
    }

    hb_blob_t* blob = hb.blob_create(
        reinterpret_cast<const char*>(bold.bytes->data()),
        static_cast<unsigned int>(bold.bytes->size()), HB_MEMORY_MODE_READONLY,
        nullptr, nullptr);
    if (blob == nullptr) {
        return out;
    }

    hb_face_t* face = hb.face_create(blob, bold.face_index);
    // hb_face_create takes its own reference to the blob.
    hb.blob_destroy(blob);
    if (face == nullptr) {
        return out;
    }

    // The glyph ids and advances still come from Skia, over the regular
    // typeface, so the bold face only fits if it orders its glyphs the same
    // way and measures them in the same units. A variable face is left alone
    // outright, since Blink pins its axes on the font this does not build.
    const unsigned int regular_glyphs = hb.face_glyphs(regular);
    const unsigned int bold_glyphs = hb.face_glyphs(face);
    const unsigned int regular_upem = hb.face_upem(regular);
    const unsigned int bold_upem = hb.face_upem(face);
    if (regular_glyphs != bold_glyphs || regular_upem != bold_upem ||
        hb.var_axes(face) != 0 || hb.var_axes(regular) != 0) {
        Report("declined: %u/%u glyphs, %u/%u upem, %u/%u axes", regular_glyphs,
               bold_glyphs, regular_upem, bold_upem, hb.var_axes(regular),
               hb.var_axes(face));
        hb.face_destroy(face);
        return out;
    }

    hb_font_t* parent = hb.font_create(face);
    if (parent == nullptr) {
        hb.face_destroy(face);
        return out;
    }
    hb.ot_set_funcs(parent);

    hb_font_t* sub = hb.font_sub(parent);
    if (sub == nullptr) {
        hb.font_destroy(parent);
        hb.face_destroy(face);
        return out;
    }
    // The same callbacks over the same HarfBuzzFontData, so glyph lookup and
    // advances answer exactly as they do for Blink's own font. Blink passes no
    // destructor with them and neither does this.
    hb.font_set_funcs(sub, bound.klass, bound.data, nullptr);

    Report("substituting a %u-glyph bold face, %zu bytes", bold_glyphs,
           bold.bytes->size());
    out.parent = parent;
    out.font = sub;
    out.face = face;
    out.declined = false;
    return out;
}

// The bold font for this one, or null when there is nothing to swap in.
//
// Blink hands hb_shape one font per typeface and rewrites its SkFont for every
// run, so whether the run is bold is a question about this call and cannot be
// remembered. What is remembered is the font built for it, and a decline,
// which are both settled by the typeface and stay put.
hb_font_t* SubstituteFor(hb_font_t* font)
{
    Bound bound;
    const bool recorded = BoundOn(font, &bound);
    if (recorded) {
        CheckFontLayout(font, bound);
    }
    const bool have = recorded || BoundInFont(font, &bound);
    const void* sk_font =
        have && bound.data != nullptr ? SkFontIn(bound.data) : nullptr;
    if (sk_font == nullptr ||
        (ReadAt<uint8_t>(sk_font, kFontFlags) & kEmboldenPrivFlag) == 0) {
        return nullptr;
    }

    auto& map = Substitutes();
    if (const auto found = map.find(font); found != map.end()) {
        return found->second.declined ? nullptr : found->second.font;
    }

    Substitute made;
    made.declined = true;

    auto* typeface = ReadAt<void*>(sk_font, kFontTypeface);
    if (const std::vector<uint8_t>* bytes = ChromiumFontBytes(typeface)) {
        const bool oblique =
            bold_fallback::IsOblique(ReadAt<float>(sk_font, kFontSkewX));
        const bold_fallback::Face bold =
            bold_fallback::RealBoldFor(*bytes, oblique);
        // A simulated bold is the regular face with DirectWrite widening it,
        // so its tables are the ones already in use.
        if (bold.bytes != nullptr && !bold.simulate) {
            made = Build(font, bound, bold);
        } else {
            Report("no bold face for this typeface");
        }
    }

    map[font] = made;
    return made.declined ? nullptr : made.font;
}

void ForgetFont(hb_font_t* font)
{
    {
        const std::lock_guard lock(g_bound_mutex);
        Bounds().erase(font);
    }

    auto& map = Substitutes();
    const auto found = map.find(font);
    if (found == map.end()) {
        return;
    }
    const Funcs& hb = Real();
    if (found->second.font != nullptr) {
        hb.font_destroy(found->second.font);
        hb.font_destroy(found->second.parent);
        hb.face_destroy(found->second.face);
    }
    map.erase(found);
}

// Where Blink's binding sits inside an hb_font_t. Needed where
// hb_font_set_funcs is called inside the binary, since nothing records the
// binding there. LearnFontLayout measures both offsets.
size_t g_klass_offset = 0;
size_t g_data_offset = 0;
bool g_layout_known = false;
// Where the scale and ptem sit, for a build that inlined their getters.
// libcef.so is that build: it names hb_font_set_scale and hb_font_set_ptem and
// neither of the two reads.
size_t g_scale_offset = 0;
size_t g_ptem_offset = 0;
bool g_scale_known = false;
bool g_ptem_known = false;

// How far into an hb_font_t to look. Past both fields, and clamped to the end
// of the page so a short allocation is never read past.
constexpr size_t kFontScan = 384;

void LearnFontLayout()
{
    const Funcs& hb = Real();
    if (g_layout_known || hb.face_empty == nullptr || hb.funcs_empty == nullptr ||
        hb.font_create == nullptr || hb.font_destroy == nullptr ||
        hb.font_set_funcs == nullptr) {
        return;
    }
    hb_font_funcs_t* klass = hb.funcs_empty();
    hb_font_t* font = hb.font_create(hb.face_empty());
    if (klass == nullptr || font == nullptr) {
        return;
    }
    // Its own address, so nothing else in the font can hold the same value.
    static char sentinel = 0;
    hb.font_set_funcs(font, klass, &sentinel, nullptr);
    // Values no font would hold by accident, planted through the setters that
    // this build does name so the reads that it does not can be measured.
    constexpr int kScaleX = 0x5EDCAFE;
    constexpr int kScaleY = 0x5EDBABE;
    constexpr float kPtem = 4321.5f;
    hb.font_set_scale(font, kScaleX, kScaleY);
    hb.font_set_ptem(font, kPtem);

    const long page = sysconf(_SC_PAGESIZE);
    const auto at = reinterpret_cast<uintptr_t>(font);
    const uintptr_t page_end =
        page > 0 ? (at + static_cast<uintptr_t>(page)) & ~static_cast<uintptr_t>(page - 1) : at;
    const size_t room = page_end > at ? static_cast<size_t>(page_end - at) : 0;
    const size_t limit = room < kFontScan ? room : kFontScan;

    size_t klass_at = 0;
    size_t data_at = 0;
    unsigned klass_seen = 0;
    unsigned data_seen = 0;
    for (size_t off = 0; off + sizeof(void*) <= limit; off += sizeof(void*)) {
        const void* held = ReadAt<void*>(font, off);
        if (held == klass) {
            klass_at = off;
            ++klass_seen;
        } else if (held == &sentinel) {
            data_at = off;
            ++data_seen;
        }
    }
    // The scale is two ints in a row and the ptem one float, so each is looked
    // for at its own alignment rather than the pointer's.
    size_t scale_at = 0;
    size_t ptem_at = 0;
    unsigned scale_seen = 0;
    unsigned ptem_seen = 0;
    for (size_t off = 0; off + 2 * sizeof(int) <= limit; off += sizeof(int)) {
        if (ReadAt<int>(font, off) == kScaleX &&
            ReadAt<int>(font, off + sizeof(int)) == kScaleY) {
            scale_at = off;
            ++scale_seen;
        }
    }
    for (size_t off = 0; off + sizeof(float) <= limit; off += sizeof(float)) {
        if (ReadAt<float>(font, off) == kPtem) {
            ptem_at = off;
            ++ptem_seen;
        }
    }
    hb.font_destroy(font);
    g_scale_known = scale_seen == 1;
    g_ptem_known = ptem_seen == 1;
    g_scale_offset = scale_at;
    g_ptem_offset = ptem_at;

    // Either field appearing twice means the wrong one could be read, so the
    // whole route is declined rather than half trusted.
    if (klass_seen != 1 || data_seen != 1) {
        Report("hb_font_t holds klass %u time(s) and the user data %u, so its "
               "layout is not settled and nothing is read from it",
               klass_seen, data_seen);
        return;
    }
    g_klass_offset = klass_at;
    g_data_offset = data_at;
    g_layout_known = true;
    Report("hb_font_t holds its funcs at +%zu and their user data at +%zu",
           klass_at, data_at);
}

// Hold the measured offsets against a binding Blink recorded. Only a build
// that interposes hb_font_set_funcs has both, and a mismatch there withdraws
// the read for every build that has only one.
void CheckFontLayout(hb_font_t* font, const Bound& recorded)
{
    static bool said = false;
    Bound read;
    if (said || !BoundInFont(font, &read)) {
        return;
    }
    said = true;
    if (read.klass == recorded.klass && read.data == recorded.data) {
        Report("the binding read off the font matches the one recorded");
    } else {
        Report("the binding read off the font is %p/%p but Blink bound %p/%p, "
               "so the offsets are wrong for this build",
               static_cast<void*>(read.klass), read.data,
               static_cast<void*>(recorded.klass), recorded.data);
        g_layout_known = false;
    }
}

bool BoundInFont(hb_font_t* font, Bound* out)
{
    if (!g_layout_known || font == nullptr) {
        return false;
    }
    out->klass = ReadAt<hb_font_funcs_t*>(font, g_klass_offset);
    out->data = ReadAt<void*>(font, g_data_offset);
    return out->data != nullptr;
}

}  // namespace

namespace bold_shaping {

// Settle how HarfBuzz got into this process and, where the loader will not do
// it, put the swap in front of hb_shape by hand. Runs in the zygote, because
// reading a symbol table off disk needs a file.
void InstallAtLoad()
{
    hb_abi::ResolveAtLoad();
    if (!chromium_patch::ParityWanted() || hb_abi::Where() == hb_abi::Linkage::kAbsent) {
        return;
    }

    const Funcs& hb = Real();
    if (!hb.complete()) {
        Report("some of HarfBuzz did not resolve, so nothing is swapped");
        return;
    }
    LearnFontLayout();

    if (hb_abi::Where() != hb_abi::Linkage::kInImage) {
        return;
    }
    // The replacement reaches the HarfBuzzFontData only through the measured
    // offsets, since nothing records the binding here. Without them it would
    // decline every run.
    if (!g_layout_known) {
        Report("hb_font_t's layout is not known, so hb_shape is left alone");
        return;
    }
    void* at = hb_abi::Real("hb_shape");
    if (at == nullptr) {
        return;
    }
    // Where hb_shape_full is a function of its own, hb_shape is replaced and
    // the replacement calls it, which is what HarfBuzz's own hb_shape does.
    // A build that inlined hb_shape_full leaves nothing to call, so hb_shape is
    // left standing and its callers are moved onto the replacement instead;
    // the replacement then calls the real hb_shape.
    if (hb.shape_full == nullptr) {
        const unsigned moved =
            hb_abi::RedirectCallsTo(at, reinterpret_cast<void*>(&::hb_shape));
        if (moved == 0) {
            Report("hb_shape %p has no reachable call site, so nothing is swapped", at);
        } else {
            Report("hb_shape_full is inlined here, so %u call(s) of hb_shape %p were "
                   "moved onto the replacement", moved, at);
        }
        return;
    }
    const char* why = nullptr;
    if (code_patch::WriteDetour(at, reinterpret_cast<void*>(&::hb_shape), &why)) {
        Report("hb_shape %p replaced where it stands%s%s", at,
               why != nullptr ? ", but " : "", why != nullptr ? why : "");
    } else {
        Report("hb_shape %p was not replaced: %s", at, why != nullptr ? why : "");
    }
}

}  // namespace bold_shaping

// Records what Blink bound to the font, which is the only way back to the
// HarfBuzzFontData at shaping time.
extern "C" __attribute__((visibility("default")))
void hb_font_set_funcs(hb_font_t* font, hb_font_funcs_t* klass, void* font_data,
                       hb_destroy_func_t destroy)
{
    const auto real = Real().font_set_funcs;
    if (real == nullptr) {
        return;
    }
    real(font, klass, font_data, destroy);

    // A build that compiles HarfBuzz in never reaches this, so recording here
    // would leave the read that build depends on untested. Skipping it makes
    // the two builds take the same route.
    if (chromium_patch::ParityWanted() && font != nullptr &&
        hb_abi::Where() != hb_abi::Linkage::kInImage) {
        const std::lock_guard lock(g_bound_mutex);
        Bounds()[font] = {klass, font_data};
    }
}

// An hb_font_t's address is reused once it is freed, so what was learned about
// it has to go with it.
extern "C" __attribute__((visibility("default")))
void hb_font_destroy(hb_font_t* font)
{
    const auto real = Real().font_destroy;
    if (real == nullptr) {
        return;
    }
    if (chromium_patch::ParityWanted() && font != nullptr) {
        ForgetFont(font);
    }
    real(font);
}

void hb_shape(hb_font_t* font, hb_buffer_t* buffer, const hb_feature_t* features,
              unsigned int num_features)
{
    const Funcs& hb = Real();
    if (hb.shape_full == nullptr && hb.shape == nullptr) {
        return;
    }

    if (!chromium_patch::ParityWanted() || !hb.complete() || font == nullptr) {
        Shape(hb, font, buffer, features, num_features);
        return;
    }

    hb_font_t* bold = SubstituteFor(font);
    if (bold == nullptr) {
        Shape(hb, font, buffer, features, num_features);
        return;
    }

    // GetScaledFont sets both on Blink's font immediately before shaping, so
    // they are read off it rather than kept.
    int x_scale = 0;
    int y_scale = 0;
    if (hb.font_get_scale != nullptr) {
        hb.font_get_scale(font, &x_scale, &y_scale);
    } else {
        x_scale = ReadAt<int>(font, g_scale_offset);
        y_scale = ReadAt<int>(font, g_scale_offset + sizeof(int));
    }
    hb.font_set_scale(bold, x_scale, y_scale);
    hb.font_set_ptem(bold, hb.font_get_ptem != nullptr ? hb.font_get_ptem(font)
                                                       : ReadAt<float>(font, g_ptem_offset));

    Shape(hb, bold, buffer, features, num_features);
}
