//+--------------------------------------------------------------------------
//
//  factory_proxy_overrides.cpp - the methods the proxy answers itself.
//
//  Everything else forwards; see src/factory_proxy_forward.cpp, which is
//  generated. The class declaration is generated too, so the members used here
//  are declared in src/factory_proxy.h.
//
//  Two groups:
//
//    * The system-font getters. The shipped library cannot enumerate fonts, so
//      these would otherwise answer S_OK with nothing in them.
//
//    * CreateTextFormat, whose fontCollection parameter is optional. A caller
//      passing null gets DWriteCore's own empty system collection, so text
//      layout resolves no family however well GetSystemFontCollection answers.
//      Substituting the collection there is what makes text actually render.
//
//  Every override falls back to the real method when no font list could be
//  built - fontconfig missing, or a machine with no outline fonts. That keeps
//  the failure shape callers already handle: S_OK with an empty collection,
//  never a new error code.
//
//----------------------------------------------------------------------------

// The casts to a particular IDWriteFactoryN are redundant for the reason the
// note at the top of src/factory_proxy_forward.cpp gives.
// ReSharper disable CppRedundantCastExpression

#include "factory_proxy.h"
#include "system_fonts.h"

namespace dwc
{
namespace
{
// COM's own IID, which no DirectWrite header declares.
constexpr GUID kIID_IUnknown =
    { .Data1 = 0x00000000, .Data2 = 0x0000, .Data3 = 0x0000,
      .Data4 = { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
}  // namespace

FactoryProxy::FactoryProxy(IDWriteFactory9* real)
    : real_(real), refs_(1), fonts_(SystemFontsCreate())
{
}

FactoryProxy::~FactoryProxy()
{
    SystemFontsDestroy(fonts_);
    if (real_ != nullptr)
    {
        real_->Release();
    }
}

// The factory interfaces are one inheritance chain, so this object is a valid
// pointer for every one of them and QueryInterface hands back the same address.
// Anything else is the real factory's business, and the object it returns is
// not wrapped, since only the factory has methods worth overriding.
HRESULT STDMETHODCALLTYPE FactoryProxy::QueryInterface(REFIID riid, void** object)
{
    if (object == nullptr)
    {
        return E_POINTER;
    }
    if (IsEqualGUID(riid, kIID_IUnknown) ||
        IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFactory)) ||
        IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFactory1)) ||
        IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFactory2)) ||
        IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFactory3)) ||
        IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFactory4)) ||
        IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFactory5)) ||
        IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFactory6)) ||
        IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFactory7)) ||
        IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFactory8)) ||
        IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFactory9)))
    {
        AddRef();
        *object = static_cast<IDWriteFactory9*>(this);
        return S_OK;
    }
    return real_->QueryInterface(riid, object);
}

ULONG STDMETHODCALLTYPE FactoryProxy::AddRef()
{
    return static_cast<ULONG>(__atomic_add_fetch(&refs_, 1, __ATOMIC_ACQ_REL));
}

ULONG STDMETHODCALLTYPE FactoryProxy::Release()
{
    const long remaining = __atomic_sub_fetch(&refs_, 1, __ATOMIC_ACQ_REL);
    if (remaining == 0)
    {
        // `this` is a FactoryProxy, not a base: the destructor is private and
        // this is the only line that may reach it, so the type is exact and the
        // delete is defined. Making the destructor virtual would put a slot in
        // the COM vtable and move every method after it.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdelete-non-abstract-non-virtual-dtor"
#else
#pragma GCC diagnostic ignored "-Wdelete-non-virtual-dtor"
#endif
        delete this;
    }
    return static_cast<ULONG>(remaining);
}

// ---------------------------------------------------------------------------
// The system font set
//
// One enumeration, one IDWriteFontSet, and the newer getters ask it for the
// interface they promise instead of fabricating one. If this set does not
// support IDWriteFontSet1 or IDWriteFontSet2, the real method answers.
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE FactoryProxy::GetSystemFontSet(IDWriteFontSet** fontSet)
{
    if (fontSet == nullptr)
    {
        return E_POINTER;
    }
    IDWriteFontSet* set = SystemFontSet(fonts_, real_);
    if (set == nullptr)
    {
        return static_cast<IDWriteFactory3*>(real_)->GetSystemFontSet(fontSet);
    }
    set->AddRef();
    *fontSet = set;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FactoryProxy::GetSystemFontSet(const BOOL includeDownloadableFonts,
                                                        IDWriteFontSet1** fontSet)
{
    IDWriteFontSet* set = SystemFontSet(fonts_, real_);
    if (set == nullptr || fontSet == nullptr)
    {
        return static_cast<IDWriteFactory6*>(real_)->GetSystemFontSet(includeDownloadableFonts, fontSet);
    }
    IDWriteFontSet1* set1 = nullptr;
    if (FAILED(set->QueryInterface(DWRITE_UUIDOF(IDWriteFontSet1),
                                   reinterpret_cast<void**>(&set1))))
    {
        return static_cast<IDWriteFactory6*>(real_)->GetSystemFontSet(
            includeDownloadableFonts, fontSet);
    }
    *fontSet = set1;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FactoryProxy::GetSystemFontSet(const BOOL includeDownloadableFonts,
                                                        IDWriteFontSet2** fontSet)
{
    IDWriteFontSet* set = SystemFontSet(fonts_, real_);
    if (set == nullptr || fontSet == nullptr)
    {
        return static_cast<IDWriteFactory7*>(real_)->GetSystemFontSet(includeDownloadableFonts, fontSet);
    }
    IDWriteFontSet2* set2 = nullptr;
    if (FAILED(set->QueryInterface(DWRITE_UUIDOF(IDWriteFontSet2),
                                   reinterpret_cast<void**>(&set2))))
    {
        return static_cast<IDWriteFactory7*>(real_)->GetSystemFontSet(
            includeDownloadableFonts, fontSet);
    }
    *fontSet = set2;
    return S_OK;
}

// ---------------------------------------------------------------------------
// The system font collection
//
// CreateFontCollectionFromFontSet produces the weight-stretch-style grouping,
// which is what the legacy IDWriteFontCollection means. The model-taking
// getters go through the real factory's own conversion so the caller's
// DWRITE_FONT_FAMILY_MODEL is honored, since the two models group a machine's
// faces into different numbers of families.
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE FactoryProxy::GetSystemFontCollection(
    IDWriteFontCollection** fontCollection, const BOOL checkForUpdates)
{
    if (fontCollection == nullptr)
    {
        return E_POINTER;
    }
    IDWriteFontCollection* collection = SystemCollectionLegacy(fonts_, real_);
    if (collection == nullptr)
    {
        return static_cast<IDWriteFactory*>(real_)->GetSystemFontCollection(fontCollection, checkForUpdates);
    }
    collection->AddRef();
    *fontCollection = collection;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FactoryProxy::GetSystemFontCollection(
    const BOOL includeDownloadableFonts, IDWriteFontCollection1** fontCollection,
    const BOOL checkForUpdates)
{
    if (fontCollection == nullptr)
    {
        return E_POINTER;
    }
    IDWriteFontCollection1* collection = SystemCollection1(fonts_, real_);
    if (collection == nullptr)
    {
        return static_cast<IDWriteFactory3*>(real_)->GetSystemFontCollection(
            includeDownloadableFonts, fontCollection, checkForUpdates);
    }
    collection->AddRef();
    *fontCollection = collection;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FactoryProxy::GetSystemFontCollection(
    const BOOL includeDownloadableFonts, const DWRITE_FONT_FAMILY_MODEL fontFamilyModel,
    IDWriteFontCollection2** fontCollection)
{
    IDWriteFontCollection2* collection = SystemCollection2(fonts_, real_, fontFamilyModel);
    if (collection == nullptr || fontCollection == nullptr)
    {
        return static_cast<IDWriteFactory6*>(real_)->GetSystemFontCollection(
            includeDownloadableFonts, fontFamilyModel, fontCollection);
    }
    collection->AddRef();
    *fontCollection = collection;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FactoryProxy::GetSystemFontCollection(
    const BOOL includeDownloadableFonts, const DWRITE_FONT_FAMILY_MODEL fontFamilyModel,
    IDWriteFontCollection3** fontCollection)
{
    if (const IDWriteFontSet* set = SystemFontSet(fonts_, real_);
        set == nullptr || fontCollection == nullptr)
    {
        return static_cast<IDWriteFactory7*>(real_)->GetSystemFontCollection(
            includeDownloadableFonts, fontFamilyModel, fontCollection);
    }
    IDWriteFontCollection2* collection2 = SystemCollection2(fonts_, real_, fontFamilyModel);
    HRESULT hr = collection2 != nullptr ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
    {
        collection2->AddRef();
    }
    if (FAILED(hr))
    {
        return hr;
    }
    hr = collection2->QueryInterface(DWRITE_UUIDOF(IDWriteFontCollection3),
                                     reinterpret_cast<void**>(fontCollection));
    collection2->Release();
    return hr;
}

// ---------------------------------------------------------------------------
// Fallback
//
// A different question from the collection: not which fonts exist, but which
// one to reach for when a run has no glyph. Built from per-script Unicode
// ranges resolved through fontconfig; see src/system_fallback.cpp.
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE FactoryProxy::GetSystemFontFallback(
    IDWriteFontFallback** fontFallback)
{
    if (fontFallback == nullptr)
    {
        return E_POINTER;
    }
    IDWriteFontFallback* fallback = SystemFallback(fonts_, real_);
    if (fallback == nullptr)
    {
        return static_cast<IDWriteFactory2*>(real_)->GetSystemFontFallback(fontFallback);
    }
    fallback->AddRef();
    *fontFallback = fallback;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Text format
//
// Only the null case is substituted. A caller that names a collection means it.
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE FactoryProxy::CreateTextFormat(
    const WCHAR* fontFamilyName, IDWriteFontCollection* fontCollection,
    const DWRITE_FONT_WEIGHT fontWeight, const DWRITE_FONT_STYLE fontStyle,
    const DWRITE_FONT_STRETCH fontStretch, const FLOAT fontSize, const WCHAR* localeName,
    IDWriteTextFormat** textFormat)
{
    if (fontCollection == nullptr)
    {
        fontCollection = SystemCollectionLegacy(fonts_, real_);
    }
    return static_cast<IDWriteFactory*>(real_)->CreateTextFormat(
        fontFamilyName, fontCollection, fontWeight, fontStyle, fontStretch, fontSize,
        localeName, textFormat);
}

HRESULT STDMETHODCALLTYPE FactoryProxy::CreateTextFormat(
    const WCHAR* fontFamilyName, IDWriteFontCollection* fontCollection,
    const DWRITE_FONT_AXIS_VALUE* fontAxisValues, const UINT32 fontAxisValueCount,
    const FLOAT fontSize, const WCHAR* localeName, IDWriteTextFormat3** textFormat)
{
    if (fontCollection == nullptr)
    {
        fontCollection = SystemCollectionLegacy(fonts_, real_);
    }
    return static_cast<IDWriteFactory6*>(real_)->CreateTextFormat(
        fontFamilyName, fontCollection, fontAxisValues, fontAxisValueCount, fontSize,
        localeName, textFormat);
}

}  // namespace dwc
