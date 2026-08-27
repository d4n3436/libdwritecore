#include "typeface_bridge.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <cstdio>

namespace typeface_bridge {
namespace {

// SkSpan<T> is a trivially copyable {pointer, size}, so it occupies two
// integer registers and the two halves can be passed separately.
using GetTableTagsFn = int (*)(const void* self, uint32_t* tags, size_t count);
using GetTableDataFn = size_t (*)(const void* self, uint32_t tag, size_t offset, size_t length,
                                  void* data);

template <typename T>
T Read(const void* base, const size_t offset)
{
    T v{};
    std::memcpy(&v, static_cast<const unsigned char*>(base) + offset, sizeof(T));
    return v;
}

void* Slot(const void* object, const unsigned index)
{
    const auto* vptr = *reinterpret_cast<void* const* const*>(object);
    return vptr[index];
}

std::mutex g_mutex;
std::unordered_map<const void*, bool> g_usable;

// Function addresses that call fontations_ffi::table_tags / table_data.
std::vector<uintptr_t> g_calls_tags;
std::vector<uintptr_t> g_calls_data;

// Slots discovered from those anchors, per typeface vtable.
struct Slots
{
    unsigned tags = 0;
    unsigned data = 0;
    bool found = false;
};
std::unordered_map<const void*, Slots> g_slots;
std::unordered_map<const void*, const void*> g_real;

bool Calls(const std::vector<uintptr_t>& set, const uintptr_t fn)
{
    return std::binary_search(set.begin(), set.end(), fn);
}

// Which slots of this typeface's vtable are onGetTableTags and
// onGetTableData, by what they call rather than by where they ought to be.
Slots FindSlots(const void* typeface)
{
    Slots s;
    if (g_calls_tags.empty() || g_calls_data.empty()) {
        return s;
    }
    const auto* vptr = *reinterpret_cast<void* const* const*>(typeface);
    unsigned tags_found = 0;
    for (unsigned i = 0; i < kMaxSlotSearched; ++i) {
        const auto fn = reinterpret_cast<uintptr_t>(vptr[i]);
        if (fn != 0 && Calls(g_calls_tags, fn)) {
            s.tags = i;
            ++tags_found;
        }
    }
    // table_tags identifies onGetTableTags on its own. table_data does not,
    // since the scaler context reads tables too, so onGetTableData is taken as
    // the next slot by declaration order and then confirmed.
    s.data = s.tags + 1;
    s.found = tags_found == 1 && s.tags + 1 < kMaxSlotSearched &&
              Calls(g_calls_data, reinterpret_cast<uintptr_t>(vptr[s.data]));
    if (s.found) {
        (void)std::fprintf(stderr,
                           "chromium-patch: onGetTableTags is slot %u of vtable %p, "
                           "onGetTableData %u\n",
                           s.tags, static_cast<const void*>(vptr), s.data);
    } else if (Read<const void*>(typeface, kProxyRealTypeface) == nullptr) {
        (void)std::fprintf(stderr,
                           "chromium-patch: no vtable slot of this typeface reaches "
                           "fontations_ffi::table_tags, so the font cannot be read out; "
                           "leaving every glyph to Skia\n");
    }
    return s;
}

uint32_t BigEndian32(const uint32_t v)
{
    return __builtin_bswap32(v);
}

uint16_t BigEndian16(const uint16_t v)
{
    return __builtin_bswap16(v);
}

void Append32(std::vector<uint8_t>* out, const uint32_t v)
{
    const uint32_t be = BigEndian32(v);
    const auto* p = reinterpret_cast<const uint8_t*>(&be);
    out->insert(out->end(), p, p + 4);
}

void Append16(std::vector<uint8_t>* out, const uint16_t v)
{
    const uint16_t be = BigEndian16(v);
    const auto* p = reinterpret_cast<const uint8_t*>(&be);
    out->insert(out->end(), p, p + 2);
}

}  // namespace

void SetAnchors(const std::vector<uintptr_t>& calls_table_tags,
                const std::vector<uintptr_t>& calls_table_data)
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_calls_tags = calls_table_tags;
    g_calls_data = calls_table_data;
    std::sort(g_calls_tags.begin(), g_calls_tags.end());
    std::sort(g_calls_data.begin(), g_calls_data.end());
}

// The object whose vtable actually answers the table calls: this typeface if
// its own slots reach the bridge, otherwise the one it proxies for.
const void* RealTypeface(const void* typeface, Slots* out)
{
    Slots s = FindSlots(typeface);
    if (s.found) {
        *out = s;
        return typeface;
    }
    const auto* inner = Read<const void*>(typeface, kProxyRealTypeface);
    if (inner == nullptr || inner == typeface) {
        *out = s;
        return nullptr;
    }
    // The vtable read below would fault on anything that is not a live
    // object.
    if ((reinterpret_cast<uintptr_t>(inner) & 7) != 0) {
        *out = s;
        return nullptr;
    }
    const auto* vptr = Read<const void*>(inner, 0);
    if (vptr == nullptr || (reinterpret_cast<uintptr_t>(vptr) & 7) != 0) {
        *out = s;
        return nullptr;
    }
    s = FindSlots(inner);
    *out = s;
    return s.found ? inner : nullptr;
}

bool LooksUsable(const void* typeface)
{
    if (typeface == nullptr) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_usable.find(typeface);
    if (it != g_usable.end()) {
        return it->second;
    }
    Slots s;
    const void* real = RealTypeface(typeface, &s);
    g_slots[typeface] = s;
    g_real[typeface] = real;
    g_usable[typeface] = real != nullptr;
    return real != nullptr;
}

std::vector<uint8_t> ReadFontFile(const void* typeface)
{
    std::vector<uint8_t> out;
    if (!LooksUsable(typeface)) {
        return out;
    }
    Slots slots;
    const void* real = nullptr;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        slots = g_slots[typeface];
        real = g_real[typeface];
    }
    if (!slots.found || real == nullptr) {
        return out;
    }
    typeface = real;

    const auto tags_fn = reinterpret_cast<GetTableTagsFn>(Slot(typeface, slots.tags));
    const int count = tags_fn(typeface, nullptr, 0);
    if (count <= 0 || count > 512) {
        return out;
    }
    std::vector<uint32_t> tags(static_cast<size_t>(count));
    if (tags_fn(typeface, tags.data(), tags.size()) != count) {
        return out;
    }

    const auto data_fn = reinterpret_cast<GetTableDataFn>(Slot(typeface, slots.data));

    struct Table
    {
        uint32_t tag = 0;
        std::vector<uint8_t> bytes;
        uint32_t offset = 0;
    };
    std::vector<Table> tables;
    tables.reserve(tags.size());
    for (const uint32_t tag : tags) {
        // onGetTableData returns min(copied, length), so a length of zero
        // answers zero. SkTypeface::getTableSize passes ~0U for the same
        // reason.
        const size_t len = data_fn(typeface, tag, 0, ~0U, nullptr);
        if (len == 0 || len > (32u << 20)) {
            continue;
        }
        Table t;
        t.tag = tag;
        t.bytes.resize(len);
        if (data_fn(typeface, tag, 0, len, t.bytes.data()) != len) {
            return out;
        }
        tables.push_back(std::move(t));
    }
    if (tables.empty()) {
        return out;
    }

    // An SFNT wants its table directory sorted by tag.
    std::sort(tables.begin(), tables.end(),
              [](const Table& a, const Table& b) { return a.tag < b.tag; });

    const auto num_tables = static_cast<uint16_t>(tables.size());
    // The directory's binary-search fields: the largest power of two not
    // exceeding numTables, times sixteen.
    uint16_t entry_selector = 0;
    while ((1u << (entry_selector + 1)) <= num_tables) {
        ++entry_selector;
    }
    const auto search_range = static_cast<uint16_t>((1u << entry_selector) * 16);
    const auto range_shift = static_cast<uint16_t>(num_tables * 16 - search_range);

    const size_t header = 12 + static_cast<size_t>(num_tables) * 16;
    size_t at = header;
    for (Table& t : tables) {
        t.offset = static_cast<uint32_t>(at);
        at += (t.bytes.size() + 3) & ~static_cast<size_t>(3);   // tables are 4-aligned
    }

    out.reserve(at);
    Append32(&out, 0x00010000);   // sfntVersion for TrueType outlines
    Append16(&out, num_tables);
    Append16(&out, search_range);
    Append16(&out, entry_selector);
    Append16(&out, range_shift);
    for (const Table& t : tables) {
        Append32(&out, t.tag);
        Append32(&out, 0);        // checksum: DirectWrite does not verify it
        Append32(&out, t.offset);
        Append32(&out, static_cast<uint32_t>(t.bytes.size()));
    }
    for (const Table& t : tables) {
        out.insert(out.end(), t.bytes.begin(), t.bytes.end());
        out.resize((out.size() + 3) & ~static_cast<size_t>(3), 0);
    }
    return out;
}

}  // namespace typeface_bridge
