//+--------------------------------------------------------------------------
//
//  system_fallback.cpp - supplying the per-script fallback table.
//
//  GetSystemFontFallback is built from the system font set, so on this build it
//  comes back as empty as the collection did. The collection is now supplied
//  from fontconfig (src/system_fonts.cpp); this supplies the fallback, which is
//  a different question: not "which fonts exist" but "which one to reach for
//  when a run has no glyph".
//
//  The ranges come from the generated table (src/fallback_data.cpp); the
//  family is whatever fontconfig resolves for that script's language on this
//  machine, which is the same charset-backed answer fc-match would give.
//
//  It names no font of its own. This library stands in for DWriteCore on
//  Linux, and a Linux DirectWrite asked what should render Devanagari answers
//  with what this host has. Preferring Windows' families is a parity question
//  that libcleartype answers in its Firefox parity mode, from DirectWrite's own
//  extracted table, so no second list is kept here.
//
//  Failure stays soft. No fontconfig, no builder, or no mapping that could be
//  added means the caller gets whatever the real GetSystemFontFallback returned
//  before - empty, but valid, and S_OK.
//
//----------------------------------------------------------------------------

#include "fallback_data.h"
#include "system_fonts.h"

#include <string>
#include <vector>

namespace dwc
{

namespace
{

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

// Builds the fallback once. Returns nullptr when nothing could be built, in
// which case the caller forwards to the real method.
IDWriteFontFallback* BuildSystemFallback(IDWriteFactory9* real,
                                         IDWriteFontCollection* collection)
{
    if (real == nullptr)
    {
        return nullptr;
    }

    IDWriteFontFallbackBuilder* builder = nullptr;
    // Cast to the interface that declared the method, for the reason
    // src/factory_proxy_forward.cpp gives: which of IDWriteFactory9's inherited
    // overloads stay visible is an SDK header detail.
    // ReSharper disable CppRedundantCastExpression
    if (FAILED(static_cast<IDWriteFactory2*>(real)->CreateFontFallbackBuilder(&builder)) ||
        builder == nullptr)
    // ReSharper restore CppRedundantCastExpression
    {
        return nullptr;
    }

    unsigned mapped = 0;
    for (unsigned i = 0; i < kFallbackEntryCount; ++i)
    {
        const FallbackEntry& entry = kFallbackEntries[i];

        std::vector<DWRITE_UNICODE_RANGE> ranges;
        ranges.reserve(entry.range_count);
        for (unsigned r = 0; r < entry.range_count; ++r)
        {
            DWRITE_UNICODE_RANGE range;
            range.first = entry.ranges[r].first;
            range.last = entry.ranges[r].last;
            ranges.push_back(range);
        }

        // Whatever this machine covers the script with, and nothing else.
        // Naming a family here would be answering for a different platform:
        // see the note above FallbackEntry.
        std::string resolved;
        std::vector<WCHAR> resolved_wide;
        if (entry.fc_lang == nullptr || !FontconfigFamilyForLang(entry.fc_lang, &resolved))
        {
            continue;
        }
        resolved_wide = Widen(resolved);
        const WCHAR* names[] = { resolved_wide.data() };
        if (SUCCEEDED(builder->AddMapping(ranges.data(), static_cast<UINT32>(ranges.size()),
                                          names, 1,
                                          collection, nullptr, nullptr, 1.0f)))
        {
            ++mapped;
        }
    }

    IDWriteFontFallback* fallback = nullptr;
    if (mapped == 0 || FAILED(builder->CreateFontFallback(&fallback)))
    {
        fallback = nullptr;
    }
    builder->Release();
    return fallback;
}

}  // namespace dwc
