// dwrite_fallback_probe.cpp - what DWriteCore's own fallback answers.
//
//   g++ -std=c++17 -O1 -o /tmp/fallback_probe \
//       tools/testing/dwrite_fallback_probe.cpp -Iinclude \
//       -Lbuild -ldwritecore -Wl,-rpath,build
//   /tmp/fallback_probe <codepoint-hex>...
//
// Chromium on Windows answers a character no hardcoded list covers with
// FontCache::GetDWriteFallbackFamily, which is IDWriteFontFallback::
// MapCharacters through Skia, and the answer follows the locale. This asks
// DWriteCore the same question directly, with no browser in the way, which is
// how the fallback rows are built.
#include "dwrite_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

size_t Length(const char16_t* s)
{
    size_t n = 0;
    while (s[n] != u'\0') {
        ++n;
    }
    return n;
}

// The little the fallback reads: one run of text, its locale, and a reading
// direction.
class OneRun final : public IDWriteTextAnalysisSource
{
public:
    OneRun(const char16_t* text, const char16_t* locale, IDWriteNumberSubstitution* sub)
        : text_(text), locale_(locale), sub_(sub)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = riid == __uuidof(IDWriteTextAnalysisSource) ? this : nullptr;
        return *out != nullptr ? S_OK : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 at, WCHAR const** text,
                                                UINT32* length) override
    {
        const size_t size = Length(text_);
        if (at >= size) {
            *text = nullptr;
            *length = 0;
            return S_OK;
        }
        *text = text_ + at;
        *length = static_cast<UINT32>(size - at);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 at, WCHAR const** text,
                                                    UINT32* length) override
    {
        if (at == 0) {
            *text = nullptr;
            *length = 0;
            return S_OK;
        }
        *text = text_;
        *length = at;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override
    {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32, UINT32* length,
                                            WCHAR const** name) override
    {
        *name = locale_;
        *length = static_cast<UINT32>(Length(text_));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32, UINT32* length,
                                                    IDWriteNumberSubstitution** sub) override
    {
        *sub = sub_;
        if (sub_ != nullptr) {
            sub_->AddRef();
        }
        *length = static_cast<UINT32>(Length(text_));
        return S_OK;
    }

private:
    const char16_t* text_;
    const char16_t* locale_;
    IDWriteNumberSubstitution* sub_ = nullptr;
};

std::string FamilyOf(IDWriteFont* font)
{
    IDWriteFontFamily* group = nullptr;
    IDWriteLocalizedStrings* names = nullptr;
    std::string out;
    if (SUCCEEDED(font->GetFontFamily(&group)) && group != nullptr &&
        SUCCEEDED(group->GetFamilyNames(&names)) && names != nullptr &&
        names->GetCount() > 0) {
        UINT32 length = 0;
        if (SUCCEEDED(names->GetStringLength(0, &length)) && length > 0) {
            std::u16string wide(length + 1, u'\0');
            if (SUCCEEDED(names->GetString(0, wide.data(), length + 1))) {
                for (const char16_t ch : wide) {
                    if (ch == u'\0') {
                        break;
                    }
                    out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
                }
            }
        }
    }
    if (names != nullptr) {
        names->Release();
    }
    if (group != nullptr) {
        group->Release();
    }
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    IUnknown* unknown = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory2),
                                   &unknown)) ||
        unknown == nullptr) {
        std::fprintf(stderr, "no factory\n");
        return 1;
    }
    auto* factory = static_cast<IDWriteFactory2*>(unknown);
    IDWriteFontFallback* fallback = nullptr;
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(factory->GetSystemFontFallback(&fallback)) || fallback == nullptr ||
        FAILED(factory->GetSystemFontCollection(&collection, FALSE))) {
        std::fprintf(stderr, "no fallback\n");
        return 1;
    }

    // The locales the census asks under, and the generic Blink resolves first.
    const char16_t* locales[] = {u"", u"ja", u"ko", u"zh-CN", u"ar", u"hi", u"th"};
    // Blink hands matchFamilyStyleCharacter the family the run already
    // resolved to, so the probe has to be told which one rather than assume.
    const char* base_utf8 = std::getenv("DWC_PROBE_BASE");
    char16_t base_buf[64] = {};
    for (size_t i = 0; base_utf8 != nullptr && base_utf8[i] != '\0' && i < 63; ++i) {
        base_buf[i] = static_cast<char16_t>(static_cast<unsigned char>(base_utf8[i]));
    }
    const char16_t* base = base_utf8 != nullptr ? base_buf : nullptr;
    std::printf("base family: %s\n", base_utf8 != nullptr ? base_utf8 : "(none)");
    std::printf("%-8s", "cp");
    for (const char16_t* locale : locales) {
        char narrow[16] = {};
        for (size_t i = 0; locale[i] != u'\0' && i < sizeof(narrow) - 1; ++i) {
            narrow[i] = static_cast<char>(locale[i]);
        }
        std::printf(" %-22s", narrow[0] != '\0' ? narrow : "-");
    }
    std::printf("\n");

    for (int i = 1; i < argc; ++i) {
        const auto cp = static_cast<unsigned>(std::strtoul(argv[i], nullptr, 16));
        char16_t text[3] = {};
        if (cp > 0xFFFF) {
            text[0] = static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10));
            text[1] = static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            text[0] = static_cast<char16_t>(cp);
        }
        std::printf("U+%04X  ", cp);
        for (const char16_t* locale : locales) {
            IDWriteNumberSubstitution* sub = nullptr;
            (void)factory->CreateNumberSubstitution(DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE,
                                                    locale, TRUE, &sub);
            OneRun run(text, locale, sub);
            UINT32 mapped = 0;
            IDWriteFont* font = nullptr;
            FLOAT scale = 1;
            std::string name = "(none)";
            const HRESULT hr = fallback->MapCharacters(&run, 0, static_cast<UINT32>(Length(text)),
                                                  collection, base, DWRITE_FONT_WEIGHT_NORMAL,
                                                  DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, &mapped, &font,
                                                  &scale);
            if (SUCCEEDED(hr) && font != nullptr) {
                name = FamilyOf(font);
                font->Release();
            } else {
                char why[32];
                (void)std::snprintf(why, sizeof(why), "hr=%08x len=%u",
                                    static_cast<unsigned>(hr), mapped);
                name = why;
            }
            if (sub != nullptr) {
                sub->Release();
            }
            std::printf(" %-22s", name.c_str());
        }
        std::printf("\n");
    }
    return 0;
}
