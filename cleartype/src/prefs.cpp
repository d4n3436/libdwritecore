//+--------------------------------------------------------------------------
//
//  prefs.cpp - supplying Firefox's Windows preferences.
//
//  Firefox parity needs about 150 preferences that Firefox on Windows has and
//  Firefox on Linux does not: the per-language font.name-list values, the
//  monospace sizes, the chrome ui.font.* families, the two FreeType gamma
//  settings that stand in for DirectWrite's, and the widget accent color, which
//  Windows takes from Firefox and every other platform from the desktop.
//
//  MOZ_DEFAULT_PREFS carries them. Preferences::GetInstanceForService passes
//  it to parsePrefData, which takes pref *data* and not a file name, so the
//  whole set travels in the environment: no profile to find, nothing written,
//  nothing to undo. It loads as defaults, which is why the generated text says
//  `pref` and not `user_pref`.
//
//  Defaults means anything the profile already sets as a user value wins.
//  That matters for the eight prefs Firefox's own font dialog writes under
//  these names: someone who chose a font size there meant it.
//
//----------------------------------------------------------------------------

#if CLEARTYPE_FIREFOX_PARITY

#include "firefox_parity_data.h"
#include "parity_mode.h"
#include "windows_fonts.h"

#include <dlfcn.h>
#include <strings.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// Every function here is reached from the constructor at the foot of the file,
// which the loader calls and static analysis does not see.
// ReSharper disable CppDFAUnreachableFunctionCall

bool Disabled(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && (std::strcmp(v, "0") == 0 || strcasecmp(v, "off") == 0 ||
                            strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0);
}

// fontconfig/fontconfig.h FC_WEIGHT_BOLD.
constexpr int kFcWeightBold = 200;

// Whether this machine has the family itself, in the weights these prefs
// assume, and not something fontconfig offered in its place. A match that
// comes back under another name is a substitution, and one that comes back
// lighter than asked is a family missing the face it was asked for.
bool FamilyInstalled(const char* name, const bool require_bold = true)
{
    static void* const fc = [] {
        void* h = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
        return h != nullptr ? h : dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
    }();
    if (fc == nullptr) {
        return false;
    }
    auto sym = [](const char* n) { return dlsym(fc, n); };
    const auto init = reinterpret_cast<int (*)()>(sym("FcInit"));
    const auto create = reinterpret_cast<void* (*)()>(sym("FcPatternCreate"));
    const auto add = reinterpret_cast<int (*)(void*, const char*, const unsigned char*)>(
        sym("FcPatternAddString"));
    const auto substitute = reinterpret_cast<int (*)(void*, void*, int)>(sym("FcConfigSubstitute"));
    const auto defaults = reinterpret_cast<void (*)(void*)>(sym("FcDefaultSubstitute"));
    const auto match = reinterpret_cast<void* (*)(void*, void*, int*)>(sym("FcFontMatch"));
    const auto get = reinterpret_cast<int (*)(void*, const char*, int, unsigned char**)>(
        sym("FcPatternGetString"));
    const auto add_int = reinterpret_cast<int (*)(void*, const char*, int)>(
        sym("FcPatternAddInteger"));
    const auto get_int = reinterpret_cast<int (*)(void*, const char*, int, int*)>(
        sym("FcPatternGetInteger"));
    const auto destroy = reinterpret_cast<void (*)(void*)>(sym("FcPatternDestroy"));
    if (init == nullptr || create == nullptr || add == nullptr || substitute == nullptr ||
        defaults == nullptr || match == nullptr || get == nullptr || add_int == nullptr ||
        get_int == nullptr || destroy == nullptr || init() == 0) {
        return false;
    }

    void* pattern = create();
    if (pattern == nullptr) {
        return false;
    }
    add(pattern, "family", reinterpret_cast<const unsigned char*>(name));
    // Asking at bold weight, because the sizes carried with these families
    // were measured on a Windows that has a designed bold to draw. A family
    // present as its regular face alone answers this match with that face,
    // and fontconfig reports the weight it really found. A caller that only
    // wants to know whether the family is here at all asks without it.
    if (require_bold) {
        add_int(pattern, "weight", kFcWeightBold);
    }
    substitute(nullptr, pattern, 0 /* FcMatchPattern */);
    defaults(pattern);
    int result = 0;
    void* matched = match(nullptr, pattern, &result);
    unsigned char* found = nullptr;
    int weight = 0;
    const bool installed = matched != nullptr && get(matched, "family", 0, &found) == 0 &&
                           found != nullptr &&
                           strcasecmp(reinterpret_cast<const char*>(found), name) == 0 &&
                           (!require_bold || (get_int(matched, "weight", 0, &weight) == 0 &&
                                              weight >= kFcWeightBold));
    if (matched != nullptr) {
        destroy(matched);
    }
    destroy(pattern);
    return installed;
}

// Whether any family in a comma-separated chain is on this machine.
bool AnyInstalled(const std::string& list);

// Firefox's own Windows chains, with the families a stock Windows 11 does not
// ship taken out. The chains are Mozilla's, and their order is Windows' answer
// *given Windows' font set*. A Linux machine carries fonts no Windows has -
// Latin Modern Math, Noto, Liberation - and one of those takes a run Windows
// would have drawn with something else.
//
// Moving them down the chain is not enough, because Firefox does not stay
// within one chain. For a CJK character gfxPlatformFontList::GetLangPrefs
// discards the character's own language and walks the cached CJK order, which
// ends zh-CN, zh-HK, zh-TW, ja, ko. Hangul therefore meets the *Chinese* chain
// first, and the pan-CJK Noto Sans CJK SC sitting at the end of it covers
// Hangul, so it answers for text Windows draws with Malgun Gothic - which is
// never reached. Nothing ahead of Noto in that chain has the character either,
// so only removal helps.
//
// Removal is conditional. If none of the Windows families in a chain is on
// this machine the chain is left as Mozilla wrote it, so a machine without the
// Windows fonts still falls back somewhere instead of nowhere.
std::string WindowsFirst(const std::string& list)
{
    std::string windows;
    std::string rest;
    for (size_t at = 0; at <= list.size();) {
        const size_t comma = list.find(',', at);
        const size_t end = comma == std::string::npos ? list.size() : comma;
        std::string name = list.substr(at, end - at);
        const size_t first = name.find_first_not_of(" \t\r\n");
        const size_t last = name.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) {
            name = name.substr(first, last - first + 1);
        }
        at = end + 1;
        if (name.empty()) {
            if (comma == std::string::npos) {
                break;
            }
            continue;
        }
        bool ships = false;
        for (const char* known : windows_fonts::kBaseInstall) {
            if (strcasecmp(known, name.c_str()) == 0) {
                ships = true;
                break;
            }
        }
        std::string& into = ships ? windows : rest;
        into += into.empty() ? "" : ", ";
        into += name;
        if (comma == std::string::npos) {
            break;
        }
    }
    if (windows.empty() || rest.empty()) {
        return list;
    }
    // Both arms are std::string values and the return type is one too, so
    // nothing here outlives the call.
    // ReSharper disable once CppDFALocalValueEscapesFunction
    return AnyInstalled(windows) ? windows : list;
}

bool AnyInstalled(const std::string& list)
{
    for (size_t at = 0; at <= list.size();) {
        const size_t comma = list.find(',', at);
        const size_t end = comma == std::string::npos ? list.size() : comma;
        std::string name = list.substr(at, end - at);
        const size_t first = name.find_first_not_of(" \t\r\n");
        if (const size_t last = name.find_last_not_of(" \t\r\n");
            first != std::string::npos &&
            FamilyInstalled(name.substr(first, last - first + 1).c_str(), false)) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        at = end + 1;
    }
    return false;
}

// The UI font and the size measured with it have to travel together. A size
// taken for Segoe UI, applied to whatever fontconfig substitutes, is worse than
// applying neither. Selawik is metric-compatible, so it keeps that size honest.
// With neither installed the ui.font.* block goes, and Gecko's own
// look-and-feel answers with family and size from one source.
// A name-list line, with its families put in the order Windows would answer in.
std::string ReorderNameList(const std::string& line)
{
    if (line.compare(0, 21, "pref(\"font.name-list.") != 0) {
        return line;
    }
    const size_t open = line.find("\", \"");
    const size_t close = line.rfind("\");");
    if (open == std::string::npos || close == std::string::npos || close <= open + 3) {
        return line;
    }
    const size_t value = open + 4;
    return line.substr(0, value) + WindowsFirst(line.substr(value, close - value)) +
           line.substr(close);
}

std::string PrefsForThisMachine()
{
    const bool segoe = FamilyInstalled("Segoe UI");
    const bool selawik = !segoe && FamilyInstalled("Selawik");
    // The other three name MS Shell Dlg 2, which is Windows' alias for Tahoma
    // and which fontconfig follows; without Tahoma it lands somewhere else, so
    // the same rule decides them.
    const bool shell_dlg = FamilyInstalled("Tahoma");

    std::string out;
    const std::string all = firefox_parity::kWindowsPrefs;
    for (size_t at = 0; at < all.size();) {
        const size_t end = all.find('\n', at);
        std::string line = all.substr(at, end == std::string::npos ? end : end - at + 1);
        at = end == std::string::npos ? all.size() : end + 1;
        if (line.compare(0, 13, "pref(\"ui.font") != 0) {
            out += ReorderNameList(line);
            continue;
        }
        // The id prefix covers each family line and the size, weight and italic
        // taken with it, which stand or fall together.
        const bool shell = line.find("ui.font.-moz-button") != std::string::npos ||
                           line.find("ui.font.-moz-list") != std::string::npos ||
                           line.find("ui.font.-moz-field") != std::string::npos;
        if (shell ? !shell_dlg : !(segoe || selawik)) {
            continue;
        }
        if (selawik && !shell) {
            if (const size_t name = line.find("\"Segoe UI\""); name != std::string::npos) {
                line.replace(name, 10, "\"Selawik\"");
            }
        }
        out += line;
    }
    return out;
}

// Before main, so it is in place long before Preferences::GetInstanceForService
// looks, and inherited by every content process this one starts.
__attribute__((constructor)) void SupplyWindowsPrefs()
{
    // Gecko or nothing: the variable is inherited by every child, so a shell
    // that has it exported would hand Windows font preferences to a later
    // Firefox started without this library - preferences with no rasterizer
    // behind them.
    if (!dwcft::ParityActive() || Disabled("CLEARTYPE_PREFS") ||
        std::getenv("MOZ_DEFAULT_PREFS") != nullptr) {
        return;
    }
    setenv("MOZ_DEFAULT_PREFS", PrefsForThisMachine().c_str(), 0 /* keep any existing */);
}

}  // namespace

#endif  // CLEARTYPE_FIREFOX_PARITY
