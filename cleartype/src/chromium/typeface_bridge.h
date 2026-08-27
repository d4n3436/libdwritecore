//+--------------------------------------------------------------------------
//
//  typeface_bridge.h - getting the font bytes back out of a Skia typeface.
//
//  DirectWrite needs a font file. Skia has one, but only behind SkTypeface
//  virtuals, and a stripped Chromium offers no header to call them through,
//  so they are called through the object's own vtable.
//
//  onGetTableTags and onGetTableData are the only ways out with no ABI beyond
//  scalars. onOpenStream hands back an SkStreamAsset whose vtable would then
//  have to be called, and onGetFamilyName an SkString. Tags and table bytes
//  are enough, since an SFNT is a table directory followed by its tables.
//
//  The slots are found rather than assumed, because they move between
//  versions. onGetTableTags is the vtable entry whose function calls
//  fontations_ffi::table_tags, which nothing else asks for. onGetTableData is
//  taken as the slot after it, being declared immediately after
//  onGetTableTags, and then confirmed to reach table_data.
//
//  Where the anchors are absent the bridge refuses rather than guessing.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_TYPEFACE_BRIDGE_H_INCLUDED
#define CHROMIUM_TYPEFACE_BRIDGE_H_INCLUDED

#include <cstddef>
#include <cstdint>
#include <vector>

namespace typeface_bridge {

// How far into a typeface's vtable to look. Chromium 154 puts onGetTableData
// at 30, and the margin covers further insertions.
constexpr unsigned kMaxSlotSearched = 48;

// sk_sp<SkTypeface> SkTypeface_proxy::fRealTypeface, from
// src/ports/SkTypeface_proxy.h. The typeface a scaler context holds on Linux
// is SkTypeface_FCI, which derives from SkTypeface_proxy and forwards the
// table calls indirectly, so the proxy's own slots never branch to the
// anchors. The real typeface behind it does.
constexpr size_t kProxyRealTypeface = 48;

// Which functions call which fontations entry point. Both sets hold
// vtable-referenced function addresses. Called once, when the patch installs.
void SetAnchors(const std::vector<uintptr_t>& calls_table_tags,
                const std::vector<uintptr_t>& calls_table_data);

// A vtable whose table slots are already known, for a build where nothing is
// exported and the anchors cannot be matched by what they call. Any typeface
// carrying this vtable uses these slots directly.
void SetSlotHint(const void* vtable, unsigned tags_slot, unsigned data_slot);

// Whether this typeface answers the canary calls sensibly. Cached per
// typeface pointer.
bool LooksUsable(const void* typeface);

// The whole font, rebuilt from its tables. Empty if anything went wrong.
std::vector<uint8_t> ReadFontFile(const void* typeface);

}  // namespace typeface_bridge

#endif  // CHROMIUM_TYPEFACE_BRIDGE_H_INCLUDED
