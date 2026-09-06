//+--------------------------------------------------------------------------
//
//  system_fonts.cpp - the CSS system font keywords, answered as Windows does.
//
//  LayoutThemeFontProvider answers `font: menu` and its siblings, and the
//  Windows and default builds of it disagree:
//
//    layout_theme_font_provider_win.cc      kMenu, kSmallCaption and
//                                           kStatusBar take the family and
//                                           height the browser process read
//                                           from the OS and sent over
//                                           RendererPreferences; everything
//                                           else takes DefaultGUIFont and the
//                                           default font size.
//    layout_theme_font_provider_default.cc  every keyword takes DefaultGUIFont
//                                           and the default font size, with
//                                           the three -webkit control
//                                           keywords two points smaller.
//
//  web_view_impl.cc sends those three families only under BUILDFLAG(IS_WIN),
//  so a Linux renderer holds no value to read and the difference cannot be
//  configured away. The two answers are patched here instead.
//
//  Both answers are compiled two ways and both are handled.
//
//  The size always ends in the same arithmetic, whether SystemFontSize stands
//  on its own or CSSPendingSystemFontValue::ResolveFontSize inlined it: the
//  keyword is biased by -18 and DefaultFontSize is called with the document.
//  Every such call is pointed at a stub that answers the three keywords and
//  defers to DefaultFontSize for the rest.
//
//  The family is either inlined into StyleBuilderConverterBase::ConvertFontFamily,
//  where the branch is entered with the CSSValue in rsi and holds room for a
//  call, or left as an out-of-line SystemFontFamily that takes the keyword in
//  edi and tail jumps to the getter. The inlined form stands in for a load and
//  answers with the string an AtomicString holds; the out-of-line form is
//  replaced whole and answers with the AtomicString itself.
//
//----------------------------------------------------------------------------

#include "system_fonts.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "code_patch.h"
#include "parity_gate.h"

extern "C" void chromium_system_family_thunk();
extern "C" void chromium_system_size_thunk();

namespace {

void Say(const char* what)
{
    if (std::getenv("DWC_SYSTEM_FONTS_LOG") == nullptr) {
        return;
    }
    (void)std::fprintf(stderr, "chromium-patch: system fonts: %s\n", what);
}

// CSSValueID, from core/css/css_value_keywords.json5 in declaration order.
// The system font keywords are kCaption through kStatusBar, which is the
// range CSSParserFastPaths::IsValidSystemFont accepts.
constexpr unsigned kCaption = 13;
constexpr unsigned kMenu = 15;
constexpr unsigned kSmallCaption = 17;
constexpr unsigned kStatusBar = 21;

// CSSValue::kPendingSystemFontValueClass, the class byte ConvertFontFamily
// branches on.
constexpr unsigned char kPendingSystemFontValueClass = 0x41;

// What Windows answers for the three keywords. The family is the Windows 11
// UI font and the height is the 9 point default at 96 dpi, which is what
// SystemParametersInfo reports and the browser process forwards.
constexpr char kUiFamily[] = "Segoe UI";
constexpr unsigned kUiFamilyLength = sizeof(kUiFamily) - 1;
constexpr float kUiFontSize = 12.0f;

bool WindowsUiKeyword(const unsigned id)
{
    return id == kMenu || id == kSmallCaption || id == kStatusBar;
}

struct Region
{
    const unsigned char* begin = nullptr;
    const unsigned char* end = nullptr;
};

// Everything read out of the image, so a replacement can defer to the code it
// replaced instead of restating it.
struct Found
{
    // LayoutThemeFontProvider::DefaultGUIFont, the guarded static holding the
    // Arial atom. It is left in place, since it is what every keyword but the
    // three answers with. It takes no argument and ignores the one passed.
    const void* (*family)(unsigned) = nullptr;
    // The AtomicString constructor it builds that static with, taking the
    // object, the characters and their count.
    void (*make_atom)(void*, const char*, unsigned) = nullptr;
    // DefaultFontSize, read out of the size function.
    float (*default_size)(const void*) = nullptr;
    unsigned char* family_site = nullptr;      // the inlined read to replace
};

Found g_found;

// The Segoe UI atom, built once through the same constructor Blink builds its
// own with, so the string is interned the way every other family name is. An
// AtomicString is one pointer wide, so the object's address serves where the
// code returns one by reference and its contents where the code loaded the
// string out of one.
void* g_ui_atom = nullptr;
std::once_flag g_ui_once;

void BuildUiAtom()
{
    std::call_once(g_ui_once,
                   [] { g_found.make_atom(static_cast<void*>(&g_ui_atom), kUiFamily,
                                       kUiFamilyLength); });
}

const void* UiFamilyString()
{
    BuildUiAtom();
    return g_ui_atom;
}

const void* UiFamilyAtom()
{
    BuildUiAtom();
    return static_cast<const void*>(&g_ui_atom);
}

// Runs in place of the inlined read in ConvertFontFamily's system-font
// branch, with the keyword in edi. The answer is the string an AtomicString
// holds, which is what the branch went on to store.
// ReSharper disable once CppDeclaratorNeverUsed
// Called from hook_thunk.S.
extern "C" const void* DwcSystemFamilyString(unsigned id);
const void* DwcSystemFamilyString(const unsigned id)
{
    // The keyword is read through the register the branch was entered with.
    // Anything outside the range CSSParserFastPaths::IsValidSystemFont
    // accepts means that register held something else, and the answer falls
    // back to what the code replaced here would have given.
    if (id >= kCaption && id <= kStatusBar && WindowsUiKeyword(id)) {
        return UiFamilyString();
    }
    // Everything else keeps Arial, taken through the untouched getter so its
    // one-time construction still happens exactly once.
    const auto* atom = static_cast<const void* const*>(g_found.family(id));
    return atom != nullptr ? *atom : nullptr;
}

// Runs in place of LayoutThemeFontProvider::SystemFontFamily where the build
// leaves it out of line, with the keyword in edi. It answers with the
// AtomicString itself, which is what that function returns by reference.
// Mirrors layout_theme_font_provider_win.cc, whose default arm is
// DefaultGUIFont and whose other three arms are the ones replaced here.
extern "C" const void* DwcSystemFamilyRef(unsigned id);
const void* DwcSystemFamilyRef(const unsigned id)
{
    if (id >= kCaption && id <= kStatusBar && WindowsUiKeyword(id)) {
        return UiFamilyAtom();
    }
    return g_found.family(id);
}

bool WriteBytes(unsigned char* at, const unsigned char* code, const size_t len)
{
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return false;
    }
    const auto start = reinterpret_cast<uintptr_t>(at) & ~static_cast<uintptr_t>(page - 1);
    const uintptr_t last =
        (reinterpret_cast<uintptr_t>(at) + len - 1) & ~static_cast<uintptr_t>(page - 1);
    const size_t span = last - start + static_cast<size_t>(page);
    auto* base = reinterpret_cast<void*>(start);
    if (mprotect(base, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    std::memcpy(at, code, len);
    (void)mprotect(base, span, PROT_READ | PROT_EXEC);
    return true;
}

// Every copy of the size arithmetic, inlined or not, computes the keyword
// less 18 into ebx and then calls DefaultFontSize, so that call is the one
// place they all share. The call sites are pointed here instead, with the
// keyword carried across in esi by the stub below. DefaultFontSize itself is
// left alone, which is what lets the answer defer to it.
extern "C" float DwcSystemFontSize(const void* document, unsigned biased);
float DwcSystemFontSize(const void* document, const unsigned biased)
{
    if (const unsigned id = biased + 18; id >= kCaption && id <= kStatusBar && WindowsUiKeyword(id)) {
        // The adjustment that follows the call subtracts two points only when
        // the biased keyword is below three unsigned, which none of these is.
        return kUiFontSize;
    }
    return g_found.default_size(document);
}

int32_t Rel32(const unsigned char* at)
{
    int32_t rel = 0;
    std::memcpy(&rel, at, sizeof(rel));
    return rel;
}

const unsigned char* RipTarget(const unsigned char* operand, const size_t instruction_end)
{
    return operand + instruction_end + Rel32(operand);
}

// The standalone getter, which every build compiles as a guarded static:
//   mov al, [guard] ; test al, al ; je init ; lea rax, [atom] ; ret
bool FindFamilyGetter(const Region& text, const unsigned char* arial)
{
    for (const unsigned char* p = text.begin; p + 32 < text.end; ++p) {
        if (p[0] != 0x8A || p[1] != 0x05 || p[6] != 0x84 || p[7] != 0xC0 ||
            p[8] != 0x74 || p[9] != 0x08 || p[10] != 0x48 || p[11] != 0x8D ||
            p[12] != 0x05 || p[17] != 0xC3) {
            continue;
        }
        // The constructor call sits in the branch this skips, naming the
        // literal and its length, which is what identifies the getter.
        for (const unsigned char* q = p + 18; q + 20 < p + 0x80 && q + 20 < text.end; ++q) {
            if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x35 ||
                q[7] != 0xBA || Rel32(q + 8) != 5 || q[12] != 0xE8) {
                continue;
            }
            if (RipTarget(q + 3, 4) != arial) {
                continue;
            }
            g_found.family = reinterpret_cast<const void* (*)(unsigned)>(
                const_cast<unsigned char*>(p));
            g_found.make_atom = reinterpret_cast<void (*)(void*, const char*, unsigned)>(
                const_cast<unsigned char*>(RipTarget(q + 13, 4)));
            return true;
        }
    }
    return false;
}

// SystemFontSize, which both builds compile the same way apart from where the
// keyword comes from:
//   push rbp ; mov rbp,rsp ; push rbx ; push rax ; mov ebx,<keyword>
//   add ebx,-18 ; mov rdi,rsi ; call DefaultFontSize ; cmp ebx,3 ; jae +8
// The keyword is a register when the function stands on its own and a field
// of the CSSValue where CSSPendingSystemFontValue::ResolveFontSize inlined
// it. What is wanted either way is DefaultFontSize, which the call names.
bool FindSizeFunction(const Region& text)
{
    for (const unsigned char* p = text.begin; p + 32 < text.end; ++p) {
        if (p[0] != 0x55 || p[1] != 0x48 || p[2] != 0x89 || p[3] != 0xE5 ||
            p[4] != 0x53 || p[5] != 0x50) {
            continue;
        }
        size_t load = 0;
        if (p[6] == 0x8B && p[7] == 0x5F && p[8] == 0x04) {
            load = 3;                                   // mov ebx,[rdi+4]
        } else if (p[6] == 0x89 && p[7] == 0xFB) {
            load = 2;                                   // mov ebx,edi
        } else {
            continue;
        }
        const unsigned char* q = p + 6 + load;
        if (q[0] != 0x83 || q[1] != 0xC3 || q[2] != 0xEE || q[3] != 0x48 ||
            q[4] != 0x89 || q[5] != 0xF7 || q[6] != 0xE8 || q[11] != 0x83 ||
            q[12] != 0xFB || q[13] != 0x03) {
            continue;
        }
        g_found.default_size = reinterpret_cast<float (*)(const void*)>(
            const_cast<unsigned char*>(RipTarget(q + 7, 4)));
        return true;
    }
    return false;
}

// ConvertFontFamily's system-font branch. The class check names it:
//   mov al,[rsi+3] ; cmp al,0x41 ; je branch
// and the branch opens with the inlined read of the same guard and atom the
// getter uses.
bool FindFamilySite(const Region& text, const unsigned char* guard,
                    const unsigned char* atom)
{
    for (const unsigned char* p = text.begin; p + 32 < text.end; ++p) {
        if (p[0] != 0x8A || p[1] != 0x46 || p[2] != 0x03 || p[3] != 0x3C ||
            p[4] != kPendingSystemFontValueClass) {
            continue;
        }
        // The jump is not always the next instruction; the compiler schedules
        // unrelated work between the test and the branch it feeds.
        const unsigned char* jump = nullptr;
        for (const unsigned char* q = p + 5; q + 6 <= p + 24 && q + 6 <= text.end; ++q) {
            if (q[0] == 0x0F && q[1] == 0x84) {
                jump = q;
                break;
            }
        }
        if (jump == nullptr) {
            continue;
        }
        const unsigned char* site = RipTarget(jump + 2, 4);
        if (site + 21 > text.end || site < text.begin) {
            continue;
        }
        // mov al,[guard] ; test al,al ; je init ; mov rax,[atom]
        if (site[0] != 0x8A || site[1] != 0x05 || site[6] != 0x84 || site[7] != 0xC0 ||
            site[8] != 0x0F || site[9] != 0x84 || site[14] != 0x48 ||
            site[15] != 0x8B || site[16] != 0x05) {
            continue;
        }
        if (RipTarget(site + 2, 4) != guard || RipTarget(site + 17, 4) != atom) {
            continue;
        }
        g_found.family_site = const_cast<unsigned char*>(site);
        return true;
    }
    return false;
}

// Bytes the out-of-line SystemFontFamily is replaced with outright.
constexpr size_t kFamilyJump = 12;

// SystemFontFamily where the build leaves it out of line, which is a frame
// pointer prologue and a tail call to the getter:
//   push rbp ; mov rbp,rsp ; pop rbp ; jmp DefaultGUIFont
// The padding after it has to be int3 and belong to no one, since the
// replacement is longer than the function.
unsigned char* FindSystemFamilyThunk(const Region& text, const unsigned char* getter)
{
    for (const unsigned char* p = text.begin; p + 32 < text.end; ++p) {
        if (p[0] != 0x55 || p[1] != 0x48 || p[2] != 0x89 || p[3] != 0xE5 ||
            p[4] != 0x5D || p[5] != 0xE9 || RipTarget(p + 6, 4) != getter) {
            continue;
        }
        bool padded = true;
        for (size_t i = 10; i < kFamilyJump; ++i) {
            padded = padded && p[i] == 0xCC;
        }
        if (padded) {
            return const_cast<unsigned char*>(p);
        }
    }
    return nullptr;
}

// The keyword is already in edi there, and the hook answers with the same
// reference the function it replaces returns.
bool WriteFamilyJump(unsigned char* thunk)
{
    unsigned char code[kFamilyJump];
    code[0] = 0x48; code[1] = 0xB8;                    // movabs rax, hook
    const auto hook = reinterpret_cast<uint64_t>(&DwcSystemFamilyRef);
    std::memcpy(code + 2, &hook, sizeof(hook));
    code[10] = 0xFF; code[11] = 0xE0;                  // jmp rax
    return WriteBytes(thunk, code, sizeof(code));
}

// The branch reads the atom in 21 bytes, which is room for the call that
// replaces it. rsi still holds the CSSValue, so the keyword is one load away.
bool WriteFamilyCall(unsigned char* site)
{
    constexpr size_t kRoom = 21;
    unsigned char code[kRoom];
    std::memset(code, 0x90, sizeof(code));                        // nop the remainder
    code[0] = 0x44; code[1] = 0x8B; code[2] = 0x5E; code[3] = 0x04;  // mov r11d,[rsi+4]
    code[4] = 0x48; code[5] = 0xB8;                               // movabs rax, thunk
    const auto hook = reinterpret_cast<uint64_t>(&chromium_system_family_thunk);
    std::memcpy(code + 6, &hook, sizeof(hook));
    code[14] = 0xFF; code[15] = 0xD0;                             // call rax

    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return false;
    }
    const auto start = reinterpret_cast<uintptr_t>(site) & ~static_cast<uintptr_t>(page - 1);
    const uintptr_t last =
        (reinterpret_cast<uintptr_t>(site) + kRoom - 1) & ~static_cast<uintptr_t>(page - 1);
    const size_t len = last - start + static_cast<size_t>(page);
    auto* base = reinterpret_cast<void*>(start);
    if (mprotect(base, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    std::memcpy(site, code, kRoom);
    (void)mprotect(base, len, PROT_READ | PROT_EXEC);
    return true;
}

// A stub inside the executable, so the call sites can reach it with the
// rel32 they already hold. Placed in the padding between functions, which is
// int3 and belongs to no one.
unsigned char* WriteSizeStub(const Region& text)
{
    constexpr size_t kStub = 14;
    unsigned char code[kStub] = {0x89, 0xDE, 0x48, 0xB8};   // mov esi,ebx ; movabs rax,hook
    const auto hook = reinterpret_cast<uint64_t>(&DwcSystemFontSize);
    std::memcpy(code + 4, &hook, sizeof(hook));
    code[12] = 0xFF;                                        // jmp rax
    code[13] = 0xE0;
    for (const unsigned char* p = text.begin; p + 64 < text.end; ++p) {
        if (*p != 0xCC) {
            continue;
        }
        size_t run = 0;
        while (p + run < text.end && p[run] == 0xCC) {
            ++run;
        }
        // Leave the first pad byte alone so nothing that falls through into
        // padding lands mid-instruction.
        if (run >= kStub + 1) {
            auto* at = const_cast<unsigned char*>(p) + 1;
            return WriteBytes(at, code, kStub) ? at : nullptr;
        }
        p += run;
    }
    return nullptr;
}

// Point every call that follows `add ebx,-18` at the stub. The keyword is
// already in ebx there, which is the whole reason this seam works.
unsigned RetargetSizeCalls(const Region& text, const unsigned char* default_size,
                           const unsigned char* stub)
{
    unsigned done = 0;
    for (const unsigned char* p = text.begin; p + 16 < text.end; ++p) {
        if (p[0] != 0x83 || p[1] != 0xC3 || p[2] != 0xEE) {
            continue;                                   // add ebx,-18
        }
        const unsigned char* q = p + 3;
        if (q[0] == 0x48 && q[1] == 0x89 && (q[2] & 0xC7) == 0xC7) {
            q += 3;                                     // mov rdi,<document>
        }
        if (q[0] != 0xE8 || RipTarget(q + 1, 4) != default_size) {
            continue;
        }
        const int64_t rel = stub - (q + 5);
        if (rel < INT32_MIN || rel > INT32_MAX) {
            continue;
        }
        const auto rel32 = static_cast<int32_t>(rel);
        unsigned char bytes[4];
        std::memcpy(bytes, &rel32, sizeof(bytes));
        if (WriteBytes(const_cast<unsigned char*>(q) + 1, bytes, sizeof(bytes))) {
            ++done;
        }
    }
    return done;
}

const unsigned char* FindArial(const Region* rodata, const unsigned count)
{
    static constexpr char kNeeded[] = "\0Arial";
    for (unsigned i = 0; i < count; ++i) {
        for (const unsigned char* p = rodata[i].begin; p + 7 < rodata[i].end; ++p) {
            if (std::memcmp(p, kNeeded, sizeof(kNeeded) - 1) == 0 && p[6] == '\0') {
                return p + 1;
            }
        }
    }
    return nullptr;
}

}  // namespace

namespace system_fonts {

void ApplyToImage(const uintptr_t base, const ElfW(Phdr)* phdr, const ElfW(Half) phnum)
{
    if (!chromium_patch::ParityWanted()) {
        return;
    }
    // Both answers or neither: a keyword drawn in the Windows family at the
    // Linux size agrees with neither platform, so every step below refuses
    // unless the code it is about to overwrite matches byte for byte, and the
    // family is only written once the size calls have been.
    if (const char* off = std::getenv("DWC_SYSTEM_FONTS");
        off != nullptr && (std::strcmp(off, "0") == 0 || std::strcmp(off, "off") == 0)) {
        return;
    }
    Region text;
    Region rodata[8];
    unsigned rodata_count = 0;
    for (ElfW(Half) i = 0; i < phnum; ++i) {
        const ElfW(Phdr)& p = phdr[i];
        if (p.p_type != PT_LOAD) {
            continue;
        }
        const auto* begin = reinterpret_cast<const unsigned char*>(base + p.p_vaddr);
        if ((p.p_flags & PF_X) != 0) {
            if (text.begin == nullptr || p.p_filesz > static_cast<size_t>(text.end - text.begin)) {
                text = {.begin = begin, .end = begin + p.p_filesz};
            }
        } else if ((p.p_flags & PF_W) == 0 && rodata_count < 8) {
            rodata[rodata_count++] = {.begin = begin, .end = begin + p.p_filesz};
        }
    }
    if (text.begin == nullptr || rodata_count == 0) {
        return;
    }
    const unsigned char* arial = FindArial(rodata, rodata_count);
    if (arial == nullptr) {
        Say("the default UI family name is not in the image");
        return;
    }
    if (!FindFamilyGetter(text, arial)) {
        Say("no guarded getter builds the default UI family name");
        return;
    }
    const auto* getter = reinterpret_cast<const unsigned char*>(g_found.family);
    const unsigned char* guard = RipTarget(getter + 2, 4);
    const unsigned char* atom = RipTarget(getter + 13, 4);
    // Where the converter inlined the read, the branch itself is replaced.
    // Where SystemFontFamily stayed a function, that function is.
    const bool inlined = FindFamilySite(text, guard, atom);
    unsigned char* thunk = inlined ? nullptr : FindSystemFamilyThunk(text, getter);
    if (!inlined && thunk == nullptr) {
        Say("the system font branch of the family converter is not recognized");
        return;
    }
    if (!FindSizeFunction(text)) {
        Say("the system font size resolver is not recognized");
        return;
    }
    const auto* default_size = reinterpret_cast<const unsigned char*>(g_found.default_size);
    const unsigned char* stub = WriteSizeStub(text);
    if (stub == nullptr) {
        Say("no room in the image for the size stub");
        return;
    }
    const unsigned retargeted = RetargetSizeCalls(text, default_size, stub);
    if (retargeted == 0) {
        Say("no call to the default size follows the keyword arithmetic");
        return;
    }
    // Both or neither: a family without its size, or the other way round,
    // renders the keyword at a size that never went with it.
    if (!(inlined ? WriteFamilyCall(g_found.family_site) : WriteFamilyJump(thunk))) {
        Say("the family branch would not open for writing");
        return;
    }
    if (std::getenv("DWC_SYSTEM_FONTS_LOG") != nullptr) {
        (void)std::fprintf(stderr,
                           "chromium-patch: system fonts: %u size calls retargeted\n",
                           retargeted);
    }
    Say("menu, small-caption and status-bar now answer as Windows does");
}

}  // namespace system_fonts
