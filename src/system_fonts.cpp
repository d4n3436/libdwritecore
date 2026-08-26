//+--------------------------------------------------------------------------
//
//  system_fonts.cpp - the font list the shipped library cannot produce itself.
//
//  libdwritecore.so imports open, read, stat, fstat and mmap, and no opendir,
//  readdir, scandir or glob. It can open a font file it is handed and cannot
//  discover one, so its system font collection comes back S_OK with zero
//  families unless a host supplies the list. Everything after discovery works:
//  CreateFontFileReference -> IDWriteFontSetBuilder1::AddFontFile ->
//  CreateFontSet -> CreateFontCollectionFromFontSet builds a real collection
//  that resolves family names and creates font faces.
//
//  So this supplies the list, from fontconfig, which is where every other
//  Linux text stack gets it. One enumeration produces one IDWriteFontSet, and
//  the collection flavors are views onto that set - which is also how the
//  DWRITE_FONT_FAMILY_MODEL parameter comes to mean something, since the two
//  models group the same set differently.
//
//  fontconfig is loaded with dlopen, not linked. Linking it would put a
//  DT_NEEDED on libfontconfig into a library whose whole packaging story is
//  that it ships as one file with nothing to install beside it. Absent
//  fontconfig, every entry point here fails softly and the proxy forwards to
//  the real method, which still returns S_OK with an empty collection. That is
//  the behavior callers had before, so none of them meets a new error.
//
//----------------------------------------------------------------------------

#include "system_fonts.h"

#include <dlfcn.h>
#include <pthread.h>

#include <set>
#include <string>
#include <vector>

namespace dwc
{
namespace
{

// The fontconfig ABI this uses, declared here instead of included: four calls,
// one struct and two result codes. FcFontSet's layout is part of fontconfig's
// public ABI and has been stable since 2.x.
struct FcFontSetAbi
{
    int nfont;
    // Unread, and kept: it is a field of fontconfig's struct, and dropping it
    // would move `fonts`.
    // ReSharper disable once CppDeclaratorNeverUsed
    int sfont;
    void** fonts;   // FcPattern**
};

using fn_FcPatternCreate = void* (*)();
using fn_FcPatternAddBool = int (*)(void*, const char*, int);
using fn_FcPatternDestroy = void (*)(void*);
using fn_FcObjectSetBuild = void* (*)(const char*, ...);
using fn_FcObjectSetDestroy = void (*)(void*);
using fn_FcFontList = FcFontSetAbi* (*)(void*, void*, void*);
using fn_FcFontSetDestroy = void (*)(FcFontSetAbi*);
using fn_FcPatternGetString = int (*)(void*, const char*, int, unsigned char**);
using fn_FcInit = int (*)();
using fn_FcPatternAddString = int (*)(void*, const char*, const unsigned char*);
using fn_FcConfigSubstitute = int (*)(void*, void*, int);
using fn_FcDefaultSubstitute = void (*)(void*);
using fn_FcFontMatch = void* (*)(void*, void*, int*);

struct Fontconfig
{
    void* handle;
    fn_FcInit Init;
    fn_FcPatternCreate PatternCreate;
    fn_FcPatternAddBool PatternAddBool;
    fn_FcPatternDestroy PatternDestroy;
    fn_FcObjectSetBuild ObjectSetBuild;
    fn_FcObjectSetDestroy ObjectSetDestroy;
    fn_FcFontList FontList;
    fn_FcFontSetDestroy FontSetDestroy;
    fn_FcPatternGetString PatternGetString;
    fn_FcPatternAddString PatternAddString;
    fn_FcConfigSubstitute ConfigSubstitute;
    fn_FcDefaultSubstitute DefaultSubstitute;
    fn_FcFontMatch FontMatch;
};

Fontconfig g_fc;
pthread_once_t g_fc_once = PTHREAD_ONCE_INIT;

void LoadFontconfig()
{
    // RTLD_LOCAL: nothing here should become visible to the host process, which
    // may have its own fontconfig loaded at a different version.
    g_fc.handle = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
    if (g_fc.handle == nullptr)
    {
        g_fc.handle = dlopen("libfontconfig.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (g_fc.handle == nullptr)
    {
        return;
    }

// The handle doubles as the "fontconfig is usable" flag, so a missing symbol
// closes it as well as clearing it.
#define DWC_FC_SYM(field, name)                                               \
    g_fc.field = reinterpret_cast<fn_##name>(dlsym(g_fc.handle, #name));      \
    if (g_fc.field == nullptr) { dlclose(g_fc.handle); g_fc.handle = nullptr; return; }

    DWC_FC_SYM(Init, FcInit)
    DWC_FC_SYM(PatternCreate, FcPatternCreate)
    DWC_FC_SYM(PatternAddBool, FcPatternAddBool)
    DWC_FC_SYM(PatternDestroy, FcPatternDestroy)
    DWC_FC_SYM(ObjectSetBuild, FcObjectSetBuild)
    DWC_FC_SYM(ObjectSetDestroy, FcObjectSetDestroy)
    DWC_FC_SYM(FontList, FcFontList)
    DWC_FC_SYM(FontSetDestroy, FcFontSetDestroy)
    DWC_FC_SYM(PatternGetString, FcPatternGetString)
    DWC_FC_SYM(PatternAddString, FcPatternAddString)
    DWC_FC_SYM(ConfigSubstitute, FcConfigSubstitute)
    DWC_FC_SYM(DefaultSubstitute, FcDefaultSubstitute)
    DWC_FC_SYM(FontMatch, FcFontMatch)
#undef DWC_FC_SYM

    if (g_fc.Init() == 0)
    {
        g_fc.handle = nullptr;
    }
}

// Every scalable outline font fontconfig knows, by path, deduplicated: a
// collection file yields one pattern per face, and AddFontFile takes the whole
// file at once.
std::vector<std::string> EnumerateFontFiles()
{
    std::vector<std::string> out;
    pthread_once(&g_fc_once, LoadFontconfig);
    if (g_fc.handle == nullptr)
    {
        return out;
    }

    void* pattern = g_fc.PatternCreate();
    if (pattern == nullptr)
    {
        return out;
    }
    g_fc.PatternAddBool(pattern, "outline", 1);
    g_fc.PatternAddBool(pattern, "scalable", 1);

    void* objects = g_fc.ObjectSetBuild("file", static_cast<const char*>(nullptr));
    if (FcFontSetAbi* fonts = g_fc.FontList(nullptr, pattern, objects); fonts != nullptr)
    {
        std::set<std::string> seen;
        for (int i = 0; i < fonts->nfont; ++i)
        {
            unsigned char* file = nullptr;
            if (g_fc.PatternGetString(fonts->fonts[i], "file", 0, &file) != 0 ||
                file == nullptr)
            {
                continue;
            }
            if (const std::string path(reinterpret_cast<const char*>(file)); !path.empty() && seen.insert(path).second)
            {
                out.push_back(path);
            }
        }
        g_fc.FontSetDestroy(fonts);
    }
    if (objects != nullptr) { g_fc.ObjectSetDestroy(objects); }
    g_fc.PatternDestroy(pattern);
    return out;
}

// DirectWrite takes UTF-16 paths and compat.h pins WCHAR at 2 bytes. Font file
// paths that are not ASCII are passed through as UTF-8 bytes widened one for
// one, which is wrong for non-ASCII and is why the result is checked. A path
// DWriteCore cannot open is skipped, and the enumeration carries on.
// UTF-8 to UTF-16, which is what DWriteCore's WCHAR strings are. Stops at the
// first malformed sequence, since a half-decoded path or family name is no
// more usable than none.
std::vector<WCHAR> Widen(const std::string& s)
{
    std::vector<WCHAR> w;
    w.reserve(s.size() + 1);
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        size_t extra = 0;
        uint32_t lowest = 0;
        if (c < 0x80) { cp = c; extra = 0; lowest = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; lowest = 0x80; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; lowest = 0x800; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; lowest = 0x10000; }
        else { break; }                      // a continuation or an invalid lead
        if (i + extra >= s.size()) { break; }   // truncated at the end
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k)
        {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok || cp < lowest || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { break; }
        i += extra + 1;
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            w.push_back(static_cast<WCHAR>(0xD800 + (cp >> 10)));
            w.push_back(static_cast<WCHAR>(0xDC00 + (cp & 0x3FF)));
        }
        else
        {
            w.push_back(static_cast<WCHAR>(cp));
        }
    }
    w.push_back(0);
    return w;
}

}  // namespace

// The family fontconfig would use for a language here. FcFontMatch always
// answers, substituting where it has to, so this is the family the system
// would actually pick and not a promise that the language is covered. It is
// the whole answer the fallback gives for that script - see src/fallback_data.h
// for why no font is named there.
bool FontconfigFamilyForLang(const char* lang, std::string* out)
{
    pthread_once(&g_fc_once, LoadFontconfig);
    if (g_fc.handle == nullptr || lang == nullptr || out == nullptr)
    {
        return false;
    }
    void* pattern = g_fc.PatternCreate();
    if (pattern == nullptr)
    {
        return false;
    }
    g_fc.PatternAddString(pattern, "lang", reinterpret_cast<const unsigned char*>(lang));
    g_fc.PatternAddBool(pattern, "outline", 1);
    g_fc.PatternAddBool(pattern, "scalable", 1);
    g_fc.ConfigSubstitute(nullptr, pattern, 0 /* FcMatchPattern */);
    g_fc.DefaultSubstitute(pattern);

    int result = 0;
    void* matched = g_fc.FontMatch(nullptr, pattern, &result);
    bool found = false;
    if (matched != nullptr)
    {
        unsigned char* family = nullptr;
        if (g_fc.PatternGetString(matched, "family", 0, &family) == 0 && family != nullptr)
        {
            *out = reinterpret_cast<const char*>(family);
            // fontconfig returns every name a family answers to, comma joined -
            // "Microsoft YaHei,\u5fae\u8f6f\u96c5\u9ed1". DirectWrite wants one name.
            if (const size_t comma = out->find(','); comma != std::string::npos)
            {
                out->erase(comma);
            }
            found = !out->empty();
        }
        g_fc.PatternDestroy(matched);
    }
    g_fc.PatternDestroy(pattern);
    return found;
}

struct SystemFonts
{
    pthread_mutex_t lock;
    bool attempted;
    bool fallback_attempted;
    IDWriteFontSet* set;
    IDWriteFontCollection* legacy;
    IDWriteFontCollection1* collection1;
    // One per DWRITE_FONT_FAMILY_MODEL, since the two group families
    // differently and callers ask for both.
    IDWriteFontCollection2* collection2[2];
    IDWriteFontFallback* fallback;
};

SystemFonts* SystemFontsCreate()
{
    auto* f = new (std::nothrow) SystemFonts();
    if (f == nullptr)
    {
        return nullptr;
    }
    pthread_mutex_init(&f->lock, nullptr);
    f->attempted = false;
    f->fallback_attempted = false;
    f->set = nullptr;
    f->legacy = nullptr;
    f->collection1 = nullptr;
    f->fallback = nullptr;
    return f;
}

void SystemFontsDestroy(SystemFonts* fonts)
{
    if (fonts == nullptr)
    {
        return;
    }
    if (fonts->fallback != nullptr) { fonts->fallback->Release(); }
    if (fonts->collection1 != nullptr) { fonts->collection1->Release(); }
    for (IDWriteFontCollection2* c : fonts->collection2)
    {
        if (c != nullptr) { c->Release(); }
    }
    if (fonts->legacy != nullptr) { fonts->legacy->Release(); }
    if (fonts->set != nullptr) { fonts->set->Release(); }
    pthread_mutex_destroy(&fonts->lock);
    delete fonts;
}

// Builds the set once. Returns nullptr when fontconfig is unavailable or the
// machine genuinely has no outline fonts. Callers forward to the real method
// in that case, and no error is manufactured here.
IDWriteFontSet* SystemFontSet(SystemFonts* fonts, IDWriteFactory9* real)
{
    if (fonts == nullptr || real == nullptr)
    {
        return nullptr;
    }
    pthread_mutex_lock(&fonts->lock);
    if (!fonts->attempted)
    {
        fonts->attempted = true;

        IDWriteFontSetBuilder* builder0 = nullptr;
        IDWriteFontSetBuilder1* builder = nullptr;
        if (SUCCEEDED(real->CreateFontSetBuilder(&builder0)) && builder0 != nullptr &&
            SUCCEEDED(builder0->QueryInterface(DWRITE_UUIDOF(IDWriteFontSetBuilder1),
                                               reinterpret_cast<void**>(&builder))))
        {
            unsigned added = 0;
            const std::vector<std::string> files = EnumerateFontFiles();
            for (size_t i = 0; i < files.size(); ++i)
            {
                IDWriteFontFile* file = nullptr;
                std::vector<WCHAR> path = Widen(files[i]);
                if (FAILED(real->CreateFontFileReference(path.data(), nullptr, &file)) ||
                    file == nullptr)
                {
                    continue;
                }
                if (SUCCEEDED(builder->AddFontFile(file)))
                {
                    ++added;
                }
                file->Release();
            }
            if (added != 0)
            {
                IDWriteFontSet* set = nullptr;
                if (SUCCEEDED(builder->CreateFontSet(&set)))
                {
                    fonts->set = set;
                }
            }
        }
        if (builder != nullptr) { builder->Release(); }
        if (builder0 != nullptr) { builder0->Release(); }
    }
    IDWriteFontSet* set = fonts->set;
    pthread_mutex_unlock(&fonts->lock);
    return set;
}

IDWriteFontCollection1* SystemCollection1(SystemFonts* fonts, IDWriteFactory9* real)
{
    IDWriteFontSet* set = SystemFontSet(fonts, real);
    if (set == nullptr)
    {
        return nullptr;
    }
    pthread_mutex_lock(&fonts->lock);
    if (fonts->collection1 == nullptr)
    {
        IDWriteFontCollection1* collection = nullptr;
        if (SUCCEEDED(real->CreateFontCollectionFromFontSet(set, &collection)))
        {
            fonts->collection1 = collection;
        }
    }
    IDWriteFontCollection1* out = fonts->collection1;
    pthread_mutex_unlock(&fonts->lock);
    return out;
}

IDWriteFontCollection2* SystemCollection2(SystemFonts* fonts, IDWriteFactory9* real,
                                          const DWRITE_FONT_FAMILY_MODEL model)
{
    IDWriteFontSet* set = SystemFontSet(fonts, real);
    if (set == nullptr)
    {
        return nullptr;
    }
    const unsigned slot = model == DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC ? 0u : 1u;
    pthread_mutex_lock(&fonts->lock);
    if (fonts->collection2[slot] == nullptr)
    {
        IDWriteFontCollection2* collection = nullptr;
        if (SUCCEEDED(real->CreateFontCollectionFromFontSet(set, model, &collection)))
        {
            fonts->collection2[slot] = collection;
        }
    }
    IDWriteFontCollection2* out = fonts->collection2[slot];
    pthread_mutex_unlock(&fonts->lock);
    return out;
}

// Built once, from the collection - a mapping naming a family the collection
// does not have is simply skipped by DirectWrite, so the collection has to
// exist first for any mapping to mean anything.
IDWriteFontFallback* SystemFallback(SystemFonts* fonts, IDWriteFactory9* real)
{
    if (fonts == nullptr || real == nullptr)
    {
        return nullptr;
    }
    IDWriteFontCollection1* collection = SystemCollection1(fonts, real);
    if (collection == nullptr)
    {
        return nullptr;
    }
    pthread_mutex_lock(&fonts->lock);
    if (!fonts->fallback_attempted)
    {
        fonts->fallback_attempted = true;
        fonts->fallback = BuildSystemFallback(real, collection);
    }
    IDWriteFontFallback* out = fonts->fallback;
    pthread_mutex_unlock(&fonts->lock);
    return out;
}

IDWriteFontCollection* SystemCollectionLegacy(SystemFonts* fonts, IDWriteFactory9* real)
{
    // IDWriteFontCollection1 derives from IDWriteFontCollection, and the legacy
    // interface is the weight-stretch-style view, which is what
    // CreateFontCollectionFromFontSet produces.
    return SystemCollection1(fonts, real);
}

}  // namespace dwc
