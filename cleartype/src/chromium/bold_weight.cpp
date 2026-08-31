#include "bold_weight.h"

#include "code_patch.h"
#include "parity_gate.h"

#include "../parity_mode.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

namespace bold_weight {
namespace {

// kBoldThreshold is FontSelectionValue(600) and FontSelectionValue keeps its
// backing as an int16 scaled by fractionalEntropy, which is 4.
constexpr int16_t kBoldBacking = 600 * 4;

// kColorEmojiLocale, font_cache.cc. Referenced once in the image, from the
// emoji branch of the function wanted here.
constexpr char kAnchor[] = "und-Zsye";

// The signature of FontCache::PlatformFallbackFontForCharacter as a member
// function: this, the description, the character, the font being substituted,
// and the fallback priority.
using FallbackFn = const void* (*)(void*, const void*, int32_t, const void*, int);

FallbackFn g_original = nullptr;
long g_weight_offset = -1;
thread_local bool t_bold = false;

// FontCache::GetFontForCharacter, the static that makes the query. Taking it
// rather than the sandbox support's virtual keeps this on a direct call site,
// which can be redirected without touching a prologue.
using QueryFn = bool (*)(int32_t, const char*, void*);
QueryFn g_query = nullptr;

void Report(const char* what, const unsigned long value)
{
    if (std::getenv("DWC_BOLD_WEIGHT_LOG") == nullptr) {
        return;
    }
    (void)std::fprintf(stderr, "chromium-patch: bold weight [%d]: %s 0x%lx\n", ::getpid(),
                       what, value);
}

struct Image
{
    uintptr_t base = 0;
    const unsigned char* text = nullptr;
    size_t text_size = 0;
    const unsigned char* eh_hdr = nullptr;
    const unsigned char* ro = nullptr;
    size_t ro_size = 0;
};

// The entry of the function containing `addr`, from PT_GNU_EH_FRAME's sorted
// search table. Section headers are not mapped at runtime, so the unwind
// header is reached through the program headers instead.
uintptr_t FunctionStart(const unsigned char* hdr, const uintptr_t hdr_addr, const uintptr_t addr)
{
    if (hdr == nullptr || hdr[0] != 1) {
        return 0;
    }
    // pcrel sdata4 pointer, udata4 count, datarel sdata4 table. Any other
    // encoding is a layout this does not know how to walk.
    if (hdr[1] != 0x1b || hdr[2] != 0x03 || hdr[3] != 0x3b) {
        return 0;
    }
    uint32_t count = 0;
    std::memcpy(&count, hdr + 8, 4);
    const unsigned char* table = hdr + 12;
    uintptr_t best = 0;
    long best_at = -1;
    long lo = 0;
    long hi = static_cast<long>(count) - 1;
    while (lo <= hi) {
        const long mid = (lo + hi) / 2;
        int32_t rel = 0;
        std::memcpy(&rel, table + static_cast<size_t>(mid) * 8, 4);
        const uintptr_t loc = hdr_addr + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        if (loc <= addr) {
            best = loc;
            best_at = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    // The table covers only functions carrying unwind information, so the
    // nearest entry below an address can belong to an unrelated function far
    // in front of it. The next entry bounds the candidate.
    if (best_at >= 0 && best_at + 1 < static_cast<long>(count)) {
        int32_t rel = 0;
        std::memcpy(&rel, table + static_cast<size_t>(best_at + 1) * 8, 4);
        if (addr >= hdr_addr + static_cast<uintptr_t>(static_cast<intptr_t>(rel))) {
            return 0;
        }
    }
    return best;
}

// The greatest address at or below `addr` that something calls. The unwind
// table covers only functions carrying unwind information and this build has
// none for the one wanted here, so the calls are what names an entry: any
// address a direct call names is a function start, and the last one before an
// address is the start of the function holding it.
uintptr_t CalledEntryBelow(const Image& image, const uintptr_t addr)
{
    uintptr_t best = 0;
    for (size_t i = 0; i + 5 <= image.text_size; ++i) {
        const unsigned char* at = image.text + i;
        if (at[0] != 0xE8) {
            continue;
        }
        int32_t rel = 0;
        std::memcpy(&rel, at + 1, 4);
        const auto target = reinterpret_cast<uintptr_t>(at + 5 + rel);
        if (target <= addr && target > best &&
            target >= reinterpret_cast<uintptr_t>(image.text)) {
            best = target;
        }
    }
    return best;
}

// The displacement the function reads the weight from, taken from the load
// that feeds the threshold compare. Self-checking: a displacement that does
// not feed `cmp $0x960` is not accepted, so a wrong function start cannot
// become a wrong offset.
long WeightOffset(const unsigned char* at, const size_t span)
{
    for (size_t i = 0; i + 12 < span; ++i) {
        const bool rex = (at[i] & 0xF0) == 0x40;
        const size_t o = rex ? i + 1 : i;
        if (at[o] != 0x0f || at[o + 1] != 0xbf) {   // movswl
            continue;
        }
        const unsigned char modrm = at[o + 2];
        long disp = 0;
        size_t next = 0;
        if ((modrm & 0xC0) == 0x40) {
            disp = static_cast<signed char>(at[o + 3]);
            next = o + 4;
        } else if ((modrm & 0xC0) == 0x80) {
            int32_t d = 0;
            std::memcpy(&d, at + o + 3, 4);
            disp = d;
            next = o + 7;
        } else {
            continue;
        }
        // cmp $0x960,%eax
        if (at[next] == 0x3d && at[next + 1] == 0x60 && at[next + 2] == 0x09 &&
            at[next + 3] == 0x00 && at[next + 4] == 0x00) {
            return disp;
        }
    }
    return -1;
}

int NoteImage(dl_phdr_info* info, size_t, void* out)
{
    // The main image only. A CEF host keeps Blink in libcef.so and does not
    // need this, since its bold fallback already matches.
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
        return 0;
    }
    auto* image = static_cast<Image*>(out);
    image->base = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        const auto at = reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        if (ph.p_type == PT_GNU_EH_FRAME) {
            image->eh_hdr = at;
        } else if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X) != 0 &&
                   ph.p_memsz > image->text_size) {
            image->text = at;
            image->text_size = ph.p_memsz;
        } else if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X) == 0 &&
                   (ph.p_flags & PF_W) == 0 && ph.p_memsz > image->ro_size) {
            image->ro = at;
            image->ro_size = ph.p_memsz;
        }
    }
    return 1;
}

// The query, with the run's weight added to the character. The browser
// answers this one and reads the mark back off, since the mojo call under it
// carries no style. The renderer's own cache is keyed on the character too, so
// the mark also keeps a bold answer from overwriting the regular one.
bool QueryReplacement(int32_t c, const char* locale, void* out)
{
    if (t_bold && c > 0 && (c & bold_weight::kBoldMark) == 0) {
        c |= bold_weight::kBoldMark;
        static bool said = false;
        if (!said) {
            said = true;
            Report("first bold query, character", static_cast<unsigned long>(c));
        }
    }
    return g_query(c, locale, out);
}

// The frame the weight is still on. Nothing here reads the description apart
// from that one field, and the flag is cleared again on the way out, so a
// nested fallback cannot leave a run marked.
const void* Replacement(void* self, const void* description, const int32_t c,
                        const void* substitute, const int priority)
{
    bool was = t_bold;
    if (description != nullptr && g_weight_offset >= 0) {
        int16_t backing = 0;
        std::memcpy(&backing,
                    static_cast<const unsigned char*>(description) + g_weight_offset, 2);
        t_bold = backing >= kBoldBacking;
    }
    const void* result = g_original(self, description, c, substitute, priority);
    t_bold = was;
    return result;
}

}  // namespace

bool RunIsBold()
{
    return t_bold;
}

void InstallAtLoad()
{
    if (!chromium_patch::ParityWanted()) {
        return;
    }
    Image image;
    dl_iterate_phdr(&NoteImage, &image);
    if (image.text == nullptr || image.eh_hdr == nullptr || image.ro == nullptr) {
        return;
    }

    // The anchor, in the read-only segment.
    const unsigned char* anchor = nullptr;
    unsigned seen = 0;
    for (size_t i = 0; i + sizeof(kAnchor) <= image.ro_size; ++i) {
        if (std::memcmp(image.ro + i, kAnchor, sizeof(kAnchor)) == 0) {
            anchor = image.ro + i;
            ++seen;
        }
    }
    if (seen != 1) {
        Report("anchor is not unique, count", seen);
        return;
    }

    // The one RIP-relative lea that names it.
    const unsigned char* ref = nullptr;
    unsigned refs = 0;
    for (size_t i = 0; i + 7 <= image.text_size; ++i) {
        const unsigned char* at = image.text + i;
        if (at[0] != 0x48 || at[1] != 0x8d || (at[2] & 0xC7) != 0x05) {
            continue;
        }
        int32_t disp = 0;
        std::memcpy(&disp, at + 3, 4);
        if (at + 7 + disp == anchor) {
            ref = at;
            ++refs;
        }
    }
    if (refs != 1) {
        Report("lea reference is not unique, count", refs);
        return;
    }

    const auto hdr_addr = reinterpret_cast<uintptr_t>(image.eh_hdr);
    const auto ref_addr = reinterpret_cast<uintptr_t>(ref);
    // Whichever of the two sits closer in front of the anchor. The unwind
    // table answers for a function that has an entry, the call scan for one
    // that does not, and taking the later of the two is right either way.
    const uintptr_t unwound = FunctionStart(image.eh_hdr, hdr_addr, ref_addr);
    const uintptr_t called = CalledEntryBelow(image, ref_addr);
    const uintptr_t start = unwound > called ? unwound : called;
    if (start == 0) {
        Report("nothing names a function entry in front of the reference at", ref_addr);
        return;
    }
    const auto* entry = reinterpret_cast<const unsigned char*>(start);
    const size_t span = static_cast<size_t>(image.text + image.text_size - entry);
    g_weight_offset = WeightOffset(entry, span < 0x8000 ? span : 0x8000);
    if (g_weight_offset < 0) {
        Report("no threshold compare inside the function at", start);
        return;
    }

    g_original = reinterpret_cast<FallbackFn>(const_cast<unsigned char*>(entry));
    const unsigned moved = code_patch::RedirectCalls(image.text, image.text_size, entry, reinterpret_cast<void*>(&Replacement));
    if (moved == 0) {
        g_original = nullptr;
        g_weight_offset = -1;
        Report("no call site was rewritten for the function at", start);
        return;
    }
    Report("installed, function at", start);
    Report("  weight offset", static_cast<unsigned long>(g_weight_offset));
    Report("  call sites rewritten", moved);

    // The query itself. The emoji branch that names the anchor reads
    //
    //     lea  <anchor>,%rsi        the locale, kColorEmojiLocale
    //     mov  $kFamily,%edi        the character, since the branch fixes it
    //     call FontCache::GetFontForCharacter
    //
    // so the call right after the anchor is the one that asks. The two
    // arguments in front of it are what says so: the character is a literal
    // and it has to be a code point. GetFontForCharacter carries no unwind
    // entry of its own, so the table cannot confirm it and this does.
    if (dwcft::IsOffValue(std::getenv("DWC_BOLD_FALLBACK"))) {
        return;
    }
    if (ref[7] != 0xBF || ref[12] != 0xE8) {
        Report("the anchor is not followed by a character and a call at",
               reinterpret_cast<uintptr_t>(ref));
        return;
    }
    uint32_t character = 0;
    std::memcpy(&character, ref + 8, 4);
    if (character > 0x10FFFF) {
        Report("the value passed with the anchor is not a code point", character);
        return;
    }
    const unsigned char* call = ref + 12;
    int32_t rel = 0;
    std::memcpy(&rel, call + 1, 4);
    const auto* query = call + 5 + rel;
    const auto query_addr = reinterpret_cast<uintptr_t>(query);
    if (query < image.text || query >= image.text + image.text_size) {
        Report("the query is not in the text segment at", query_addr);
        return;
    }
    g_query = reinterpret_cast<QueryFn>(const_cast<unsigned char*>(query));
    const unsigned asked = code_patch::RedirectCalls(image.text, image.text_size, query, reinterpret_cast<void*>(&QueryReplacement));
    if (asked == 0) {
        g_query = nullptr;
        Report("no call site was rewritten for the query at", query_addr);
        return;
    }
    Report("the query is at", query_addr);
    Report("  call sites rewritten", asked);
}

}  // namespace bold_weight
