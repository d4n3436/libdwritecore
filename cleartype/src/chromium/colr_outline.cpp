// A color layer is asked for at exactly units per em, and the size dedup
// below is an identity check. A tolerance would take a different branch.
#pragma GCC diagnostic ignored "-Wfloat-equal"

#include "colr_outline.h"

#include "code_patch.h"
#include "hb_abi.h"
#include "dwrite_raster.h"
#include "parity_gate.h"

#include "../parity_mode.h"
#include "path_abi.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <unistd.h>

namespace colr_outline {
namespace {

// fontations_ffi::get_path_verbs_points, as cxx generates it. The version
// number in the middle of the mangled name changes with the bridge, so the
// symbol is matched by its tail.
constexpr char kBridgePrefix[] = "fontations_ffi$cxxbridge1$";
constexpr char kEntryTail[] = "get_path_verbs_points";
constexpr char kDrawTail[] = "draw_colr_glyph";

// ColorPainterWrapper's vtable, in the order skpath_bridge.h declares it. The
// destructor takes the first two slots, as the Itanium ABI lays a virtual one
// out. Every method is pure virtual there and overridden by ColorPainter, so
// the object the bridge is handed carries a vtable and can be patched the way
// the scaler context already is.
enum Slot
{
    kIsBoundsMode = 2,
    kPushTransform = 3,
    kPopTransform = 4,
    kPushClipGlyph = 5,
    kPushClipRectangle = 6,
    kPopClip = 7,
    // ReSharper disable CppEnumeratorNeverUsed
    kFillSolid = 8,
    kFillLinear = 9,
    kFillRadial = 10,
    kFillSweep = 11,
    kFillGlyphSolid = 12,
    kFillGlyphRadial = 13,
    kFillGlyphLinear = 14,
    kFillGlyphSweep = 15,
    kSlotCount = 18,
};

// fontations_ffi::Transform and FillRadialParams, both plain floats.
struct Transform
{
    float xx, xy, yx, yy, dx, dy;
};

struct RadialParams
{
    float x0, y0, r0, x1, y1, r1;
};

using DrawFn = bool (*)(const void*, const void*, uint16_t, void*);
using FillGlyphRadialFn = void (*)(void*, uint16_t, const Transform*, const RadialParams*, void*,
                                   uint8_t);

// fontations_ffi::ClipBox, the glyph's COLRv1 bounding box in the size asked
// for.
struct ClipBox
{
    float x_min;
    float y_min;
    float x_max;
    float y_max;
};

using ClipBoxFn = bool (*)(const void*, const void*, uint16_t, float, ClipBox*);
using IsBoundsModeFn = bool (*)(void*);
using PushClipRectFn = void (*)(void*, float, float, float, float);
using PopClipFn = void (*)(void*);
using PushTransformFn = void (*)(void*, const Transform*);
using PopTransformFn = void (*)(void*);
using FillGlyphLinearFn = void (*)(void*, uint16_t, const Transform*, const void*, void*,
                                   uint8_t);
using PushClipGlyphFn = void (*)(void*, uint16_t);
using FillParamsFn = void (*)(void*, const void*, void*, uint8_t);

DrawFn g_draw = nullptr;
ClipBoxFn g_clip_box = nullptr;

// The methods one painter vtable held before its slots were replaced. Two
// painter classes reach draw_colr_glyph, the drawing one and BoundsPainter,
// and they carry different implementations in the same slots, so the originals
// are kept per vtable and never in one variable per slot.
struct PainterOps
{
    void** vtable = nullptr;
    FillGlyphRadialFn fill_glyph_radial = nullptr;
    PushTransformFn push_transform = nullptr;
    PopTransformFn pop_transform = nullptr;
    FillGlyphLinearFn fill_glyph_linear = nullptr;
    PushClipRectFn push_clip_rect = nullptr;
    FillGlyphLinearFn fill_glyph_sweep = nullptr;
    PushClipGlyphFn push_clip_glyph = nullptr;
    PopClipFn pop_clip = nullptr;
    FillParamsFn fill_linear = nullptr;
    FillParamsFn fill_radial = nullptr;
    FillParamsFn fill_sweep = nullptr;
    // Set once the vtable carries the hooks. The count is published first so a
    // hook finds its originals, which leaves the class listed and unpatched.
    std::atomic<bool> ready{false};
};

constexpr int kMaxPainters = 8;
PainterOps g_painter[kMaxPainters];
std::atomic g_painters{0};

// The originals for whichever class `self` belongs to, by its own vtable.
const PainterOps* OpsFor(const void* self)
{
    if (self == nullptr) {
        return nullptr;
    }
    void* const* vtable = *static_cast<void* const* const*>(self);
    const int count = g_painters.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        if (g_painter[i].vtable == vtable) {
            return &g_painter[i];
        }
    }
    return nullptr;
}

// outlines, glyph, size, coords, hinting instance, the two rust::Vec outputs
// and the metrics the caller reads back.
using PathFn = bool (*)(const void*, uint16_t, float, const void*, const void*, void*, void*,
                        void*);

PathFn g_original = nullptr;

// cxx's rust::Vec is the Rust one: pointer, capacity, length. Growing it has
// to go through Rust's allocator, which cxx exports a shim for per element
// type, so the three that a rewrite needs are resolved by name.
using VecDataFn = const void* (*)(const void*);
using VecReserveFn = void (*)(void*, size_t);
using VecSetLenFn = void (*)(void*, size_t);

struct VecOps
{
    VecDataFn data = nullptr;
    VecReserveFn reserve = nullptr;
    VecSetLenFn set_len = nullptr;
    // reserve is optional: where a build never emitted it for this element
    // type, writes stay inside the capacity the extraction call reserves.
    bool Ready() const { return data != nullptr && set_len != nullptr; }
};

// VerbsPointsPen::new reserves this many elements in both vectors on every
// extraction call, so a path this size fits without growing either one.
constexpr size_t kPathExtractionReserve = 150;

VecOps g_verbs;
VecOps g_points;

struct Source
{
    const void* face_key = nullptr;
    const std::vector<uint8_t>* font = nullptr;
    uint32_t face_index = 0;
    float render_size = 0;
    uint16_t upem = 0;
    // The glyph's subpixel position, in em units for the paint tree's own
    // space and in device pixels for the clip box.
    float phase_x = 0;
    float phase_y = 0;
    float sub_x = 0;
    float sub_y = 0;
};

thread_local Source t_source;


// Whether the current walk carries the subpixel transform PushPhase adds, so
// the clip snap can put the phase on both sides of its rounding.
thread_local bool t_phased = false;

uint16_t Be16At(const std::vector<uint8_t>& font, const size_t at)
{
    if (at + 1 >= font.size()) {
        return 0;
    }
    return static_cast<uint16_t>(font[at] << 8 | font[at + 1]);
}

uint32_t Be32At(const std::vector<uint8_t>& font, const size_t at)
{
    if (at + 3 >= font.size()) {
        return 0;
    }
    return static_cast<uint32_t>(font[at]) << 24 | static_cast<uint32_t>(font[at + 1]) << 16 |
           static_cast<uint32_t>(font[at + 2]) << 8 | font[at + 3];
}

// head.unitsPerEm, which is the scale skrifa is asked for a color layer at.
uint16_t UnitsPerEm(const std::vector<uint8_t>& font, const uint32_t face_index)
{
    size_t base = 0;
    if (Be32At(font, 0) == 0x74746366) {          // 'ttcf'
        base = Be32At(font, 12 + face_index * 4);
    }
    const uint16_t tables = Be16At(font, base + 4);
    for (uint16_t i = 0; i < tables; ++i) {
        const size_t entry = base + 12 + static_cast<size_t>(i) * 16;
        if (Be32At(font, entry) == 0x68656164) {  // 'head'
            return Be16At(font, Be32At(font, entry + 8) + 18);
        }
    }
    return 0;
}

// Put DWriteCore's outline into the two vectors, in the units the caller asked
// for. SkScalerContext_DW takes the layer outline at the render size and
// divides that back out, so the same outline scaled by upem/size is what its
// COLR drawing works from.
bool Substitute(const uint16_t glyph, void* verbs, void* points)
{
    const Source s = t_source;
    if (s.font == nullptr || s.render_size <= 0 || !g_verbs.Ready() || !g_points.Ready()) {
        return false;
    }
    std::vector<uint8_t> dw_verbs;
    std::vector<path_abi::Point> dw_points;
    if (!dwrite_raster::GlyphOutline(s.face_key, *s.font, glyph, s.render_size, &dw_verbs,
                                     &dw_points, s.face_index)) {
        return false;
    }
    // Skia scales the outline to em with SkMatrix::Scale(1 / render size)
    // before its canvas matrix multiplies the size back in, and the
    // Fontations scaler matrix divides the path by units per em. Multiplying
    // by the same reciprocal first and by upem second keeps every coordinate
    // on the float values Windows draws, since a power-of-two em makes the
    // upem factor exact.
    const float to_em = 1.0f / s.render_size;
    const float upem = static_cast<float>(s.upem);
    for (path_abi::Point& p : dw_points) {
        p.x = p.x * to_em * upem;
        p.y = p.y * to_em * upem;
    }
    if (g_verbs.reserve != nullptr) {
        g_verbs.reserve(verbs, dw_verbs.size());
    } else if (dw_verbs.size() > kPathExtractionReserve) {
        return false;
    }
    if (g_points.reserve != nullptr) {
        g_points.reserve(points, dw_points.size());
    } else if (dw_points.size() > kPathExtractionReserve) {
        return false;
    }
    auto* verb_data = const_cast<uint8_t*>(static_cast<const uint8_t*>(g_verbs.data(verbs)));
    auto* point_data =
        const_cast<path_abi::Point*>(static_cast<const path_abi::Point*>(g_points.data(points)));
    if (verb_data == nullptr || point_data == nullptr) {
        return false;
    }
    std::memcpy(verb_data, dw_verbs.data(), dw_verbs.size());
    std::memcpy(point_data, dw_points.data(), dw_points.size() * sizeof(path_abi::Point));
    g_verbs.set_len(verbs, dw_verbs.size());
    g_points.set_len(points, dw_points.size());
    return true;
}

bool Replacement(const void* outlines, const uint16_t glyph, const float size, const void* coords,
                 const void* hinting, void* verbs, void* points, void* metrics)
{
    const bool ok = g_original(outlines, glyph, size, coords, hinting, verbs, points, metrics);
    // A color layer is the only thing asked for at units per em; every other
    // path comes through at the render size and already carries DirectWrite's
    // outline, swapped in at the SkPath by the raster hook.
    if (ok && t_source.upem != 0 && size == static_cast<float>(t_source.upem)) {
        (void)Substitute(glyph, verbs, points);
    }
    return ok;
}

// SkScalerContext_DW::drawColorV1Image draws every layer as a clip on the
// layer glyph's outline followed by a drawPaint, so on Windows the paint runs
// under an antialiased clip and its blitter. The Fontations painter's
// fill_glyph_* shortcuts draw one drawPath instead, which composes the
// gradient through a different blitter once the glyph's own clip box is
// snapped to whole pixels and no longer antialiased. Decomposing a shortcut
// into its clip and fill halves is what the painter itself does when a clip
// is already open, and matches the Windows walk. Solid glyphs stay whole,
// since Windows draws DWRITE_PAINT_TYPE_SOLID_GLYPH with one drawPath too.
// Bounds mode keeps the original call, which joins the same box either way.
bool SplitFills(void* self)
{
    // Only for glyphs the parity path owns. A COLRv1 web font renders through
    // plain Fontations on Windows too, and its shortcut fills stay whole
    // there, so a walk with no source set keeps the painter untouched.
    if (self == nullptr || t_source.upem == 0) {
        return false;
    }
    auto** vtable = *static_cast<void***>(self);
    return !reinterpret_cast<IsBoundsModeFn>(vtable[kIsBoundsMode])(self);
}

void FillGlyphRadialHook(void* self, const uint16_t glyph, const Transform* transform,
                         const RadialParams* params, void* stops, const uint8_t extend)
{
    const PainterOps* ops = OpsFor(self);
    if (ops == nullptr) {
        return;
    }

    if (transform != nullptr && SplitFills(self)) {
        ops->push_clip_glyph(self, glyph);
        ops->push_transform(self, transform);
        ops->fill_radial(self, params, stops, extend);
        ops->pop_transform(self);
        ops->pop_clip(self);
        return;
    }
    ops->fill_glyph_radial(self, glyph, transform, params, stops, extend);
}

void FillGlyphSweepHook(void* self, const uint16_t glyph, const Transform* t, const void* params,
                        void* stops, const uint8_t extend)
{
    const PainterOps* ops = OpsFor(self);
    if (ops == nullptr) {
        return;
    }

    if (t != nullptr && SplitFills(self)) {
        ops->push_clip_glyph(self, glyph);
        ops->push_transform(self, t);
        ops->fill_sweep(self, params, stops, extend);
        ops->pop_transform(self);
        ops->pop_clip(self);
        return;
    }
    ops->fill_glyph_sweep(self, glyph, t, params, stops, extend);
}

void FillGlyphLinearHook(void* self, const uint16_t glyph, const Transform* t, const void* params,
                         void* stops, const uint8_t extend)
{
    const PainterOps* ops = OpsFor(self);
    if (ops == nullptr) {
        return;
    }

    if (t != nullptr && SplitFills(self)) {
        ops->push_clip_glyph(self, glyph);
        ops->push_transform(self, t);
        ops->fill_linear(self, params, stops, extend);
        ops->pop_transform(self);
        ops->pop_clip(self);
        return;
    }
    ops->fill_glyph_linear(self, glyph, t, params, stops, extend);
}

// SkRect::round, which SkCanvas::clipRect applies to a device edge when it is
// not antialiasing. An exact half goes toward positive infinity.
float SkRound(const float v)
{
    return static_cast<float>(std::floor(static_cast<double>(v) + 0.5));
}

// The same for a paint-tree y, which the painter negates before the device
// edge, so positive infinity there is negative infinity here.
float SkRoundNegated(const float v)
{
    return static_cast<float>(std::ceil(static_cast<double>(v) - 0.5));
}

// skrifa forwards the glyph's own COLRv1 clip box through push_clip_rectangle,
// which always antialiases, so an edge that falls between device pixels
// attenuates its boundary row or column by the fractional coverage. Windows
// applies the same box with SkCanvas::clipRect's default, which rounds every
// edge to a whole device pixel, so the box is snapped to the same pixels here
// and the antialiasing has nothing left to blend. Bounds mode keeps the
// unrounded box, which is what Windows measures.
void PushClipRectangleHook(void* self, float x_min, float y_min, float x_max, float y_max)
{
    const PainterOps* ops = OpsFor(self);
    if (ops == nullptr) {
        return;
    }

    if (t_source.upem != 0 && t_source.render_size > 0) {
        auto** vtable = *static_cast<void***>(self);
        if (!reinterpret_cast<IsBoundsModeFn>(vtable[kIsBoundsMode])(self)) {
            const float s = t_source.render_size / static_cast<float>(t_source.upem);
            const float ox = t_phased ? t_source.sub_x : 0.0f;
            const float oy = t_phased ? t_source.sub_y : 0.0f;
            x_min = (SkRound(x_min * s + ox) - ox) / s;
            x_max = (SkRound(x_max * s + ox) - ox) / s;
            // The painter negates each y on the way to the device rect.
            y_min = (SkRoundNegated(y_min * s - oy) + oy) / s;
            y_max = (SkRoundNegated(y_max * s - oy) + oy) / s;
        }
    }
    ops->push_clip_rect(self, x_min, y_min, x_max, y_max);
}
// A color glyph's bounds come from this box. generateColorV1Metrics in
// SkScalerContext_win_dw.cpp maps it with the subpixel position already in
// the matrix:
//
//     matrix = fSkXform; matrix.preScale(scale, scale);
//     if (this->isSubpixel())
//       matrix.postTranslate(SkFixedToScalar(glyph.getSubXFixed()), ...);
//     *bounds = sk_rect_from(clipBox); matrix.mapRect(bounds);
//
// The Fontations generateMetrics maps it through fRemainingMatrix alone, so
// its box is a pixel narrow whenever the position carries an edge past the
// next one and the mask loses a column. The offset is applied before Skia
// rounds the box out, so the position sits inside the rounding.
//
// The box arrives scaled by the size the caller asked for, in the paint
// tree's y-up space. phase_x and phase_y are the position divided by the whole
// matrix, so multiplying by that size lands them in the box's space; y is
// subtracted because the tree's y grows upward.
bool ClipBoxReplacement(const void* font_ref, const void* coords, const uint16_t glyph,
                        const float size, ClipBox* out)
{
    const bool ok = g_clip_box(font_ref, coords, glyph, size, out);
    if (!ok || out == nullptr ||
        (t_source.phase_x == 0.0f && t_source.phase_y == 0.0f)) {
        return ok;
    }
    const float dx = t_source.phase_x * size;
    const float dy = t_source.phase_y * size;
    out->x_min += dx;
    out->x_max += dx;
    out->y_min -= dy;
    out->y_max -= dy;
    return ok;
}

// Patch the painter's vtable once per distinct table. The object is built on
// the stack of drawCOLRGlyph, so the vtable is what persists, not the object.
// Two threads reach a color glyph at once on a fresh renderer. Without this
// both take the same slot, and one records the other's hooks as originals.
std::mutex g_patch_mutex;

void PatchPainter(void* painter)
{
    if (painter == nullptr) {
        return;
    }
    auto** vtable = *static_cast<void***>(painter);
    for (int i = 0, n = g_painters.load(std::memory_order_acquire); i < n; ++i) {
        if (g_painter[i].vtable == vtable && g_painter[i].ready.load(std::memory_order_acquire)) {
            return;
        }
    }
    // Only the holder adds a class, and marks it ready before letting go.
    const std::lock_guard patch_lock(g_patch_mutex);
    const int patched_count = g_painters.load(std::memory_order_acquire);
    for (int i = 0; i < patched_count; ++i) {
        if (g_painter[i].vtable == vtable) {
            return;
        }
    }
    if (patched_count >= kMaxPainters) {
        return;
    }
    PainterOps& ops = g_painter[patched_count];
    ops.vtable = vtable;
    ops.fill_glyph_radial = reinterpret_cast<FillGlyphRadialFn>(vtable[kFillGlyphRadial]);
    ops.push_transform = reinterpret_cast<PushTransformFn>(vtable[kPushTransform]);
    ops.pop_transform = reinterpret_cast<PopTransformFn>(vtable[kPopTransform]);
    ops.fill_glyph_linear = reinterpret_cast<FillGlyphLinearFn>(vtable[kFillGlyphLinear]);
    ops.push_clip_rect = reinterpret_cast<PushClipRectFn>(vtable[kPushClipRectangle]);
    ops.fill_glyph_sweep = reinterpret_cast<FillGlyphLinearFn>(vtable[kFillGlyphSweep]);
    ops.push_clip_glyph = reinterpret_cast<PushClipGlyphFn>(vtable[kPushClipGlyph]);
    ops.pop_clip = reinterpret_cast<PopClipFn>(vtable[kPopClip]);
    ops.fill_linear = reinterpret_cast<FillParamsFn>(vtable[kFillLinear]);
    ops.fill_radial = reinterpret_cast<FillParamsFn>(vtable[kFillRadial]);
    ops.fill_sweep = reinterpret_cast<FillParamsFn>(vtable[kFillSweep]);
    // Published before a slot is written, so a call arriving on the first
    // patched slot already finds this class's originals.
    g_painters.store(patched_count + 1, std::memory_order_release);
    const long page = sysconf(_SC_PAGESIZE);
    auto* slot = reinterpret_cast<unsigned char*>(&vtable[0]);
    auto* base = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) &
                                         ~static_cast<uintptr_t>(page - 1));
    if (mprotect(base, static_cast<size_t>(page) * 2, PROT_READ | PROT_WRITE) != 0) {
        return;
    }
    vtable[kFillGlyphRadial] = reinterpret_cast<void*>(&FillGlyphRadialHook);
    vtable[kPushClipRectangle] = reinterpret_cast<void*>(&PushClipRectangleHook);
    vtable[kFillGlyphLinear] = reinterpret_cast<void*>(&FillGlyphLinearHook);
    vtable[kFillGlyphSweep] = reinterpret_cast<void*>(&FillGlyphSweepHook);
    (void)mprotect(base, static_cast<size_t>(page) * 2, PROT_READ);
    ops.ready.store(true, std::memory_order_release);
}

// Push the glyph's subpixel position as a transform over the whole walk, which
// is what SkScalerContext_DW does in the matrix it builds:
//
//     matrix = fSkXform; matrix.preScale(size, size);
//     if (isSubpixel()) matrix.postTranslate(subX, subY);
//
// The Fontations scaler's generateMetrics and drawCOLRGlyph concat
// fRec.getSingleMatrix() alone, so without this the glyph is rasterized at
// phase zero. One push covers both walks, since both reach draw_colr_glyph.
//
// canvas.concat multiplies on the right, so this transform lands before the
// scaler matrix rather than after it and the offset goes in divided by that
// matrix. DeviceOffsetToEm divides, since the rec is what knows the matrix.
bool PushPhase(void* painter)
{
    if (t_source.upem == 0 || (t_source.phase_x == 0.0f && t_source.phase_y == 0.0f)) {
        return false;
    }
    auto** vtable = *static_cast<void***>(painter);
    const auto push = reinterpret_cast<PushTransformFn>(vtable[kPushTransform]);
    const auto em = static_cast<float>(t_source.upem);
    // The bridge negates the translation's y, since the paint tree's space is
    // y up and the canvas is y down.
    const Transform t{.xx = 1.0f, .xy = 0.0f, .yx = 0.0f, .yy = 1.0f,
                      .dx = t_source.phase_x * em, .dy = -t_source.phase_y * em};
    push(painter, &t);
    return true;
}

void PopPhase(void* painter)
{
    auto** vtable = *static_cast<void***>(painter);
    reinterpret_cast<PopTransformFn>(vtable[kPopTransform])(painter);
}

// SkScalerContext_DW::drawColorV1Image clips the canvas to the glyph's own
// COLRv1 box before walking the paint tree; SkFontationsScalerContext's
// drawCOLRGlyph concats the matrix and walks it with no clip at all, so a
// layer that reaches past the box paints here and is cut off on Windows.
// Bounds mode is left alone, since that walk is measuring the box rather than
// drawing inside it.
bool DrawReplacement(const void* font_ref, const void* coords, const uint16_t glyph, void* painter)
{
    PatchPainter(painter);
    const bool phased = painter != nullptr && PushPhase(painter);
    t_phased = phased;
    ClipBox box{};
    if (g_clip_box != nullptr && painter != nullptr && t_source.upem != 0 &&
        t_source.render_size > 0) {
        auto** vtable = *static_cast<void***>(painter);
        const auto bounds_mode = reinterpret_cast<IsBoundsModeFn>(vtable[kIsBoundsMode]);
        if (!bounds_mode(painter) &&
            g_clip_box(font_ref, coords, glyph, static_cast<float>(t_source.upem), &box)) {
            const auto push = reinterpret_cast<PushClipRectFn>(vtable[kPushClipRectangle]);
            const auto pop = reinterpret_cast<PopClipFn>(vtable[kPopClip]);
            // Windows clips with SkCanvas::clipRect's default, which is not
            // antialiased, so the box lands on whole device pixels.
            // push_clip_rectangle always antialiases, so the box is snapped
            // here instead and the antialiasing has nothing left to blend.
            const float s = t_source.render_size /
                            static_cast<float>(t_source.upem);
            // Windows rounds the box after the subpixel offset is in the
            // matrix, so the phase belongs on both sides of the rounding.
            const float ox = phased ? t_source.sub_x : 0.0f;
            const float oy = phased ? t_source.sub_y : 0.0f;
            const auto snap_x = [s, ox](const float v) {
                return (SkRound(v * s + ox) - ox) / s;
            };
            // It also builds SkRect::MakeLTRB(x_min, -y_min, x_max, -y_max), so
            // the y pair goes in swapped to come out as a sorted rect, and the
            // device edge each one lands on is negated.
            const auto snap_y = [s, oy](const float v) {
                return (SkRoundNegated(v * s - oy) + oy) / s;
            };
            push(painter, snap_x(box.x_min), snap_y(box.y_max), snap_x(box.x_max),
                 snap_y(box.y_min));
            const bool ok = g_draw(font_ref, coords, glyph, painter);
            pop(painter);
            if (phased) {
                PopPhase(painter);
            }
            t_phased = false;
            return ok;
        }
    }
    const bool ok = g_draw(font_ref, coords, glyph, painter);
    if (phased) {
        PopPhase(painter);
    }
    t_phased = false;
    return ok;
}

}  // namespace

void SetSource(const void* face_key, const std::vector<uint8_t>* font,
               const uint32_t face_index, const float render_size)
{
    t_source.face_key = face_key;
    t_source.font = font;
    t_source.face_index = face_index;
    t_source.render_size = render_size;
    t_source.upem = font != nullptr ? UnitsPerEm(*font, face_index) : 0;
}

void SetPhase(const float phase_x, const float phase_y, const float sub_x, const float sub_y)
{
    t_source.phase_x = phase_x;
    t_source.phase_y = phase_y;
    t_source.sub_x = sub_x;
    t_source.sub_y = sub_y;
}

void InstallAtLoad()
{
    if (!chromium_patch::ParityWanted()) {
        return;
    }
    void* entry = nullptr;
    for (const auto& [name, address] : hb_abi::SymbolsWithPrefix(kBridgePrefix)) {
        if (name.size() >= sizeof(kEntryTail) - 1 &&
            name.compare(name.size() - (sizeof(kEntryTail) - 1), sizeof(kEntryTail) - 1,
                         kEntryTail) == 0) {
            if (entry != nullptr && entry != address) {
                return;
            }
            entry = address;
        }
    }
    if (entry == nullptr) {
        return;
    }
    void* draw = nullptr;
    for (const auto& [name, address] : hb_abi::SymbolsWithPrefix(kBridgePrefix)) {
        if (name.size() >= sizeof(kDrawTail) - 1 &&
            name.compare(name.size() - (sizeof(kDrawTail) - 1), sizeof(kDrawTail) - 1,
                         kDrawTail) == 0) {
            draw = address;
        }
    }
    const code_patch::TextSpan image = code_patch::TextHolding(entry);
    if (image.text == nullptr) {
        return;
    }
    if (draw != nullptr) {
        g_draw = reinterpret_cast<DrawFn>(draw);
        const unsigned drew = code_patch::RedirectCalls(
            image.text, image.size, static_cast<const unsigned char*>(draw),
            reinterpret_cast<void*>(&DrawReplacement));
        if (drew == 0) {
            g_draw = nullptr;
        }
    }
    g_original = reinterpret_cast<PathFn>(entry);
    const unsigned moved = code_patch::RedirectCalls(
        image.text, image.size, static_cast<const unsigned char*>(entry),
        reinterpret_cast<void*>(&Replacement));
    if (moved == 0) {
        g_original = nullptr;
        return;
    }
    for (const auto& [name, address] : hb_abi::SymbolsWithPrefix(kBridgePrefix)) {
        if (name.size() > 19 && name.compare(name.size() - 19, 19, "get_colrv1_clip_box") == 0) {
            g_clip_box = reinterpret_cast<ClipBoxFn>(address);
        }
    }
    if (g_clip_box != nullptr) {
        (void)code_patch::RedirectCalls(
            image.text, image.size, reinterpret_cast<const unsigned char*>(g_clip_box),
            reinterpret_cast<void*>(&ClipBoxReplacement));
    }

    // The vector shims, one set per element type.
    for (const auto& [name, address] : hb_abi::SymbolsWithPrefix("cxxbridge1$rust_vec$")) {
        VecOps* ops = nullptr;
        if (name.rfind("cxxbridge1$rust_vec$u8$", 0) == 0) {
            ops = &g_verbs;
        } else if (name.rfind("cxxbridge1$rust_vec$fontations_ffi$FfiPoint$", 0) == 0) {
            ops = &g_points;
        }
        if (ops == nullptr) {
            continue;
        }
        if (name.size() > 5 && name.compare(name.size() - 5, 5, "$data") == 0) {
            ops->data = reinterpret_cast<VecDataFn>(address);
        } else if (name.size() > 14 && name.compare(name.size() - 14, 14, "$reserve_total") == 0) {
            ops->reserve = reinterpret_cast<VecReserveFn>(address);
        } else if (name.size() > 8 && name.compare(name.size() - 8, 8, "$set_len") == 0) {
            ops->set_len = reinterpret_cast<VecSetLenFn>(address);
        }
    }
    // set_len writes the length field, which sits at the same place whatever
    // the element type, so a build that only emitted the u8 one still covers
    // the points vector. Chromium 144 is such a build.
    if (g_points.set_len == nullptr) {
        g_points.set_len = g_verbs.set_len;
    }
    if (g_points.data == nullptr) {
        g_points.data = g_verbs.data;
    }
}

}  // namespace colr_outline
