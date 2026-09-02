//+--------------------------------------------------------------------------
//
//  fallback_hook.cpp - answer per-character fallback on a build whose
//  fontconfig is compiled in.
//
//  ui/gfx/font_fallback_linux.cc sorts the font set once per locale and then
//  takes the first font in it whose charset holds the character. Windows picks
//  per script, so one order cannot answer every character: the families that
//  have to come first for one script have to come later for another. On a
//  build whose fontconfig can be interposed the sort is reordered per call;
//  here there is nothing to interpose, so the query itself is answered.
//
//  gfx::GetFallbackFontForChar takes the locale by value into a map of sorted
//  sets, so asking under a locale of our own gets a set ordered our way and
//  built by Chromium, with no font data crossing the boundary. The
//  configuration this library serves carries one block per family, reached by
//  a tag appended to the locale, and the tag names the family Windows would
//  have answered with.
//
//  The function carries no symbol, being a stripped static build, but it does
//  carry its trace event's name. That string is referenced once, and the
//  function it sits in is the nearest call target before it.
//
//----------------------------------------------------------------------------

#include "fallback_hook.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include "code_patch.h"
#include "fallback_order.h"
#include "parity_gate.h"
#include "bold_weight.h"
#include "static_fontconfig.h"

namespace {

// NOLINTNEXTLINE(cert-dcl50-cpp)
void Say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// NOLINTNEXTLINE(cert-dcl50-cpp)
void Say(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    (void)std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)std::fprintf(stderr, "chromium-patch: fallback hook [%d]: %s\n", ::getpid(), buf);
}

// The trace event CachedFontSet::GetFallbackFontForChar opens with. It is
// inlined into gfx::GetFallbackFontForChar, so the string sits in that
// function.
constexpr char kAnchor[] = "gfx::CachedFontSet::GetFallbackFontForChar";

using FallbackFn = bool (*)(int, const void*, void*);
FallbackFn g_original = nullptr;

// libc++ lays std::string out one of two ways and the builds differ, so which
// one is in front is read off the locale that arrives rather than assumed.
//
//   kFirstByte   the capacity word first, its low bit saying the string is
//                long; a short one keeps its length in the first byte, twice
//                over, and its characters from the second
//   kLastByte    the pointer first, then the size; a short one keeps its
//                length in the last byte, its top bit saying the string is
//                long, and its characters from the first
//
// Only a short string is ever written, so the two differ in where the length
// goes and where the characters start.
enum class Layout
{
    kUnknown,
    kFirstByte,
    kLastByte,
};

constexpr size_t kShortMax = 22;
constexpr size_t kStringSize = 24;

bool ShortStringReads(const unsigned char* at, const size_t length, const size_t from)
{
    if (length > kShortMax) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (at[from + i] < 0x20 || at[from + i] > 0x7E) {
            return false;
        }
    }
    return at[from + length] == '\0';
}

// Which layout the value in front reads as. Unknown when both do, which an
// empty string always does, and when neither does.
Layout LayoutOf(const void* at)
{
    if (at == nullptr) {
        return Layout::kUnknown;
    }
    const auto* bytes = static_cast<const unsigned char*>(at);
    const bool first = (bytes[0] & 1) == 0 &&
                       ShortStringReads(bytes, bytes[0] >> 1, 1);
    const bool last = (bytes[kStringSize - 1] & 0x80) == 0 &&
                      ShortStringReads(bytes, bytes[kStringSize - 1], 0);
    if (first == last) {
        return Layout::kUnknown;
    }
    return first ? Layout::kFirstByte : Layout::kLastByte;
}

// Settled once, since a build has one layout.
Layout g_layout = Layout::kUnknown;

// The text of a short std::string, for reading the locale back out. Empty for
// anything not in the short form, which a locale never is.
std::string ReadShortString(const void* at, const Layout layout)
{
    if (at == nullptr || layout == Layout::kUnknown) {
        return {};
    }
    const auto* bytes = static_cast<const unsigned char*>(at);
    const size_t size = layout == Layout::kFirstByte ? bytes[0] >> 1
                                                     : bytes[kStringSize - 1];
    if (size > kShortMax) {
        return {};
    }
    const size_t at_byte = layout == Layout::kFirstByte ? 1 : 0;
    return {reinterpret_cast<const char*>(bytes) + at_byte, size};
}


// A short std::string of our own, in the layout the build uses, living on the
// caller's stack. Only the tagged locale is built this way and it never
// outgrows the short form.
bool WriteShortString(void* at, const char* text, const Layout layout)
{
    const size_t size = std::strlen(text);
    if (size > kShortMax || layout == Layout::kUnknown) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(at);
    std::memset(bytes, 0, kStringSize);
    if (layout == Layout::kFirstByte) {
        bytes[0] = static_cast<unsigned char>(size << 1);
        std::memcpy(bytes + 1, text, size + 1);
    } else {
        std::memcpy(bytes, text, size + 1);
        bytes[kStringSize - 1] = static_cast<unsigned char>(size);
    }
    return true;
}

bool Hook(const int marked, const void* locale, void* out)
{
    if (g_original == nullptr) {
        return false;
    }
    if (std::getenv("DWC_FALLBACK_LOG") != nullptr) {
        static bool said = false;
        if (!said) {
            said = true;
            Say("answering here, first character U+%04X", static_cast<unsigned>(marked));
        }
    }
    // The renderer marks the character when the run asking is bold, since the
    // weight does not survive the mojo call and the character is the only
    // thing that reaches here per query. See bold_weight.cpp.
    const bool bold = (marked & bold_weight::kBoldMark) != 0;
    const int c = marked & ~bold_weight::kBoldMark;
    if (!chromium_patch::ParityWanted()) {
        return g_original(c, locale, out);
    }
    // No font claims the C1 controls, so the run keeps its own font and draws
    // its .notdef box. Declining here says that: font_cache_linux.cc's
    // PlatformFallbackFontForCharacter returns null as soon as
    // GetFontForCharacter fails, and the fallback iterator carries on with the
    // family already in hand. Without this, fontconfig answers Webdings, whose
    // charset claims the range.
    if (c >= 0x0080 && c <= 0x009F) {
        return false;
    }
    // A measured DirectWrite row is asked first, because the pass-through
    // below relies on a sorted set this build never makes. FcFontSort is what
    // puts Times New Roman and the run's Han family in front, and a static
    // build has no sort to watch, so a character those rows cover would keep
    // whatever the configuration's own order happened to answer.
    const int dwrite = fallback_order::DWriteRowFor(static_cast<unsigned>(c));
    // Times New Roman fronts the Latin, Greek and Cyrillic ranges, which the
    // sort does on the other route and nothing does here. Naming it keeps a
    // character the run's own family lacks off a second face of that family:
    // Microsoft JhengHei has no Cyrillic and the Light face does, so zh-TW
    // answered Cyrillic from it instead.
    const char* set_front =
        fallback_order::SetOrderFront(static_cast<unsigned>(c));
    // Unified Han's family follows the run's language, which FcFontSort
    // answers on the other route by putting it in front of the set. A static
    // build has no sort to watch, so the language is read here and its Han
    // family named outright. Without this the configuration's own order
    // answers, which reaches Microsoft Tai Le for the fullwidth forms where
    // Windows reaches Microsoft YaHei.
    if (dwrite < 0 && set_front == nullptr &&
        fallback_order::SetOrderAnswers(static_cast<unsigned>(c))) {
        if (g_layout == Layout::kUnknown) {
            g_layout = LayoutOf(locale);
        }
        int han_tag = -1;
        if (g_layout != Layout::kUnknown) {
            const std::string spoken = ReadShortString(locale, g_layout);
            unsigned han_count = 0;
            const char* const* han =
                fallback_order::HanCandidates(spoken.c_str(), &han_count);
            if (han == nullptr) {
                // OrderForHan takes the simplified list when the locale
                // settles no language, and the subtag is what names it.
                han = fallback_order::HanCandidates("zh-Hans", &han_count);
            }
            // FirstAvailableFont takes the first installed candidate, and a
            // family with no block is one the survey did not find.
            for (unsigned i = 0; han != nullptr && i < han_count && han_tag < 0; ++i) {
                han_tag = static_fontconfig::TagFor(han[i]);
            }
        }
        if (han_tag < 0) {
            return g_original(c, locale, out);
        }
        char han_tagged[64];
        (void)std::snprintf(han_tagged, sizeof(han_tagged), "%s%02d%s",
                            static_fontconfig::kFamilyTag, han_tag,
                            bold ? static_fontconfig::kBoldTag : "");
        alignas(16) unsigned char han_held[kStringSize];
        if (!WriteShortString(han_held, han_tagged, g_layout)) {
            return g_original(c, locale, out);
        }
        return g_original(c, han_held, out);
    }
    unsigned count = 0;
    const char* const* families =
        fallback_order::FamiliesFor(static_cast<unsigned>(c), &count);
    int tag = set_front != nullptr ? static_fontconfig::TagFor(set_front) : -1;
    for (unsigned i = 0; families != nullptr && i < count && tag < 0; ++i) {
        tag = static_fontconfig::TagFor(families[i]);
    }
    // The tag stands alone. What follows the named family in its block is the
    // pan-Unicode list, which is what Windows walks next whatever the
    // language, so the locale would only multiply the sorted sets kept.
    // No row names a family means GetFallbackFamily names none either, and
    // then the list is the whole answer. A DirectWrite row wins over both,
    // and its block opens with the same families they would have named, so
    // nothing either of them answered moves.
    if (g_layout == Layout::kUnknown) {
        g_layout = LayoutOf(locale);
        if (g_layout == Layout::kUnknown) {
            return g_original(c, locale, out);
        }
        if (std::getenv("DWC_FALLBACK_LOG") != nullptr) {
            Say("the locale reads as the %s layout",
                g_layout == Layout::kFirstByte ? "first-byte" : "last-byte");
        }
    }
    char tagged[64];
    const char* const mark = bold ? static_fontconfig::kBoldTag : "";
    if (dwrite >= 0 && fallback_order::DWriteRowFront(dwrite) != nullptr) {
        // The row's front family follows the language, so the tag names both
        // the row and the language; 99 is a run that names none of them.
        const std::string spoken = ReadShortString(locale, g_layout);
        const int han = static_fontconfig::HanLocaleIndex(spoken.c_str());
        (void)std::snprintf(tagged, sizeof(tagged), "%s%02d%02d%s",
                            static_fontconfig::kDWriteHanTag, dwrite,
                            han < 0 ? 99 : han, mark);
    } else if (dwrite >= 0) {
        (void)std::snprintf(tagged, sizeof(tagged), "%s%02d%s",
                            static_fontconfig::kDWriteTag, dwrite, mark);
    } else if (tag < 0) {
        (void)std::snprintf(tagged, sizeof(tagged), "%s%s",
                            static_fontconfig::kPanTag, mark);
    } else {
        (void)std::snprintf(tagged, sizeof(tagged), "%s%02d%s",
                            static_fontconfig::kFamilyTag, tag, mark);
    }
    alignas(16) unsigned char held[kStringSize];
    if (!WriteShortString(held, tagged, g_layout)) {
        return g_original(c, locale, out);
    }
    if (bold && std::getenv("DWC_FALLBACK_LOG") != nullptr) {
        static bool said = false;
        if (!said) {
            said = true;
            Say("first bold answer, U+%04X under %s", static_cast<unsigned>(c), tagged);
        }
    }
    return g_original(c, held, out);
}

struct Image
{
    const unsigned char* text_begin;
    const unsigned char* text_end;
    const unsigned char* anchor;
};

// The module that carries the anchor, which is the one holding Chromium. This
// library writes the anchor down too, so its own image never counts.
int NoteImage(dl_phdr_info* info, size_t, void* data)
{
    static const uintptr_t self = [] {
        Dl_info me{};
        return dladdr(reinterpret_cast<const void*>(kAnchor), &me) != 0
                   ? reinterpret_cast<uintptr_t>(me.dli_fbase)
                   : 0;
    }();
    if (self != 0 && info->dlpi_addr == self) {
        return 0;
    }
    const unsigned char* found = nullptr;
    auto* image = static_cast<Image*>(data);
    Image seen{};
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD) {
            continue;
        }
        const auto* from =
            reinterpret_cast<const unsigned char*>(info->dlpi_addr + header.p_vaddr);
        const unsigned char* to = from + header.p_filesz;
        if ((header.p_flags & PF_X) != 0) {
            if (seen.text_begin == nullptr) {
                seen.text_begin = from;
                seen.text_end = to;
            }
            continue;
        }
        if (found == nullptr) {
            found = static_cast<const unsigned char*>(
                ::memmem(from, header.p_filesz, kAnchor, sizeof(kAnchor)));
        }
        (void)to;
    }
    if (found == nullptr || seen.text_begin == nullptr) {
        return 0;
    }
    seen.anchor = found;
    *image = seen;
    return 1;
}

// The one lea whose target is the anchor. A REX prefix, 0x8d, and a modrm
// naming RIP with no index.
const unsigned char* AnchorSite(const Image& image, const unsigned char* anchor)
{
    const unsigned char* found = nullptr;
    const unsigned char* at = image.text_begin;
    while (at + 7 <= image.text_end) {
        const auto* next =
            static_cast<const unsigned char*>(std::memchr(at, 0x8D, static_cast<size_t>(
                                                  image.text_end - at)));
        if (next == nullptr || next + 5 > image.text_end) {
            break;
        }
        at = next + 1;
        if (next == image.text_begin) {
            continue;
        }
        const unsigned char rex = next[-1];
        if ((rex & 0xF9) != 0x48 || (next[1] & 0xC7) != 0x05) {
            continue;
        }
        int32_t disp = 0;
        std::memcpy(&disp, next + 2, sizeof(disp));
        if (next - 1 + 7 + disp == anchor) {
            if (found != nullptr) {
                return nullptr;   // more than one, so nothing is named
            }
            found = next - 1;
        }
    }
    return found;
}

size_t RelocatableBytes(const unsigned char* at, size_t least);

// The function the site sits in, which is the nearest call target before it
// that a prologue starts at. A call is only ever made to a function's first
// byte, and the blocks a function is split into stay inside its own range
// here. The whole image is looked at, since the call can come from anywhere.
const unsigned char* EntryBefore(const Image& image, const unsigned char* site)
{
    const unsigned char* best = nullptr;
    const unsigned char* at = image.text_begin;
    while (at + 5 <= site) {
        const auto* next = static_cast<const unsigned char*>(
            std::memchr(at, 0xE8, static_cast<size_t>(site - at)));
        if (next == nullptr || next + 5 > site) {
            break;
        }
        at = next + 1;
        int32_t rel = 0;
        std::memcpy(&rel, next + 1, sizeof(rel));
        const unsigned char* target = next + 5 + rel;
        if (target > site || target < image.text_begin || target + 16 >= image.text_end) {
            continue;
        }
        if ((best == nullptr || target > best) && RelocatableBytes(target, 12) != 0) {
            best = target;
        }
    }
    return best;
}

// How many bytes of the prologue can be moved somewhere else and still mean
// the same thing. Only the forms a Chromium prologue is built from are
// recognized; anything else refuses rather than guessing, since a mistake here
// runs the wrong bytes.
size_t RelocatableBytes(const unsigned char* at, const size_t least)
{
    size_t taken = 0;
    while (taken < least) {
        const unsigned char* p = at + taken;
        if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA) {
            taken += 4;              // endbr64
        } else if (p[0] >= 0x50 && p[0] <= 0x57) {
            taken += 1;              // push rax..rdi
        } else if (p[0] == 0x41 && p[1] >= 0x50 && p[1] <= 0x57) {
            taken += 2;              // push r8..r15
        } else if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0xE5) {
            taken += 3;              // mov rbp, rsp
        } else if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) {
            taken += 4;              // sub rsp, imm8
        } else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) {
            taken += 7;              // sub rsp, imm32
        } else {
            return 0;
        }
    }
    return taken;
}

// The displaced bytes, then a jump back to what follows them.
void* BuildTrampoline(const unsigned char* entry, const size_t taken)
{
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return nullptr;
    }
    void* room = mmap(nullptr, static_cast<size_t>(page), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (room == MAP_FAILED) {
        return nullptr;
    }
    auto* out = static_cast<unsigned char*>(room);
    std::memcpy(out, entry, taken);
    const auto back = reinterpret_cast<uint64_t>(entry + taken);
    out[taken] = 0x48;
    out[taken + 1] = 0xB8;
    std::memcpy(out + taken + 2, &back, sizeof(back));
    out[taken + 10] = 0xFF;
    out[taken + 11] = 0xE0;
    if (mprotect(room, static_cast<size_t>(page), PROT_READ | PROT_EXEC) != 0) {
        (void)munmap(room, static_cast<size_t>(page));
        return nullptr;
    }
    return room;
}

}  // namespace

namespace fallback_hook {

void Install()
{
    static bool once = false;
    if (once || g_original != nullptr) {
        return;
    }
    once = true;
    if (const char* off = std::getenv("DWC_NO_FALLBACK_HOOK");
        off != nullptr && off[0] != '\0') {
        return;
    }
    Image image{};
    if (dl_iterate_phdr(NoteImage, &image) == 0 || image.text_begin == nullptr) {
        return;
    }
    const unsigned char* site = AnchorSite(image, image.anchor);
    if (site == nullptr) {
        Say("the trace name is in the image but nothing loads its address");
        return;
    }
    const unsigned char* entry = EntryBefore(image, site);
    if (entry == nullptr) {
        Say("no call reaches the function the trace name sits in");
        return;
    }
    constexpr size_t kPatch = 12;
    const size_t taken = RelocatableBytes(entry, kPatch);
    if (taken == 0) {
        Say("the prologue at %p is not one of the shapes that can be moved",
            static_cast<const void*>(entry));
        return;
    }
    void* trampoline = BuildTrampoline(entry, taken);
    if (trampoline == nullptr) {
        Say("no page for the trampoline");
        return;
    }
    g_original = reinterpret_cast<FallbackFn>(trampoline);
    const char* why = nullptr;
    if (!code_patch::WriteDetour(const_cast<unsigned char*>(entry),
                                 reinterpret_cast<void*>(&Hook), &why)) {
        g_original = nullptr;
        Say("gfx::GetFallbackFontForChar at %p stays as it was: %s",
            static_cast<const void*>(entry), why);
        return;
    }
    if (std::getenv("DWC_FALLBACK_LOG") != nullptr) {
        Say("gfx::GetFallbackFontForChar %p answered per character, %zu bytes moved",
            static_cast<const void*>(entry), taken);
    }
}

}  // namespace fallback_hook
