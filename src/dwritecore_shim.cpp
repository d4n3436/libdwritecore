//+--------------------------------------------------------------------------
//
//  dwritecore_shim.cpp - entry points and implementation loader.
//
//  The stock libdwritecore.so exports exactly two symbols:
//
//      DWriteCoreCreateFactory
//      DWriteCoreSetFeatureStagingCallback
//
//  This library re-exports both, and adds the DWriteCreateFactory spelling
//  that Office's libdwriteshim.so provides.
//
//  Because this library carries the same SONAME as the implementation it
//  loads, the implementation must be a SONAME-renamed copy - see
//  tools/make_impl.py. Otherwise the dynamic loader resolves the dlopen back
//  to this library and the forward recurses.
//
//----------------------------------------------------------------------------

#include "cleartype_version.h"
#include "dwrite_core.h"
#include "dwritecore_shim.h"
#include <stdlib.h>
#include "factory_proxy.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace
{

// ReSharper disable once CppDeclaratorNeverUsed
[[gnu::used]] constexpr char g_version[] = CLEARTYPE_VERSION_STRING;

// Default file name of the SONAME-renamed implementation copy. Chosen to be
// exactly as long as "libdwritecore.so" so tools/make_impl.py can rewrite
// DT_SONAME in place without moving any .dynstr offsets.
constexpr char kDefaultImplName[] = "libdwcoreimpl.so";
constexpr char kImplPathEnv[] = "DWRITECORE_IMPL_PATH";
constexpr char kBionicDirEnv[] = "DWRITECORE_BIONIC_DIR";

using PfnDWriteCoreCreateFactory = HRESULT (*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
using PfnSetFeatureStagingCallback = void (*)(void*, void*);

struct Impl
{
    void* handle = nullptr;
    PfnDWriteCoreCreateFactory createFactory = nullptr;
    PfnSetFeatureStagingCallback setFeatureStagingCallback = nullptr;
    std::string diagnostic;
};

Impl g_impl;
pthread_once_t g_once = PTHREAD_ONCE_INIT;

// Directory this shim was loaded from, so the implementation can sit beside it.
std::string ThisLibraryDirectory()
{
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&ThisLibraryDirectory), &info) != 0 &&
        info.dli_fname != nullptr)
    {
        if (const char* slash = strrchr(info.dli_fname, '/'); slash != nullptr)
        {
            return std::string(info.dli_fname,
                               static_cast<size_t>(slash - info.dli_fname) + 1);
        }
    }
    return std::string();
}

#ifdef DWRITECORE_EMBEDDED

// With DWRITECORE_EMBED_IMPL on, the implementation and the bionic stubs are
// carried inside this library as blobs (see tools/gen_blobs.py) so that only
// one file ships.
#define DWC_BLOB(name)                                    \
    extern "C" const unsigned char dwc_blob_##name##_start[]; \
    extern "C" const unsigned char dwc_blob_##name##_end[];

DWC_BLOB(impl)
DWC_BLOB(libc)
DWC_BLOB(libm)
DWC_BLOB(libdl)
DWC_BLOB(liblog)

// Load a shared object straight out of memory.
//
// The preferred route is an anonymous in-memory file, which never touches the
// filesystem. Loading by an explicit path also sidesteps the by-SONAME
// deduplication described above, though the copy embedded here is
// SONAME-renamed anyway so no two loaded objects ever claim the same name.
//
// Hardened systems can refuse to map a memfd executable (SELinux execmem
// policy, or seccomp filtering memfd_create), so there is a fallback that
// writes to a temporary file, opens it, and unlinks it immediately.
void* LoadFromMemory(const unsigned char* begin, const unsigned char* end,
                     const char* label, std::string* errors)
{
    // Subtracted as integers: begin and end are separate linker-provided
    // symbols bracketing one contiguous section, which the compiler cannot see
    // and a pointer subtraction between them is formally UB.
    const size_t size = reinterpret_cast<uintptr_t>(end) -
                        reinterpret_cast<uintptr_t>(begin);

    int fd = memfd_create(label, MFD_CLOEXEC);
    const bool from_memfd = fd >= 0;
    std::string temp_path;

    if (!from_memfd)
    {
        const char* dir = getenv("XDG_RUNTIME_DIR");
        temp_path = std::string(dir && *dir ? dir : "/tmp") + "/dwc-XXXXXX";
        std::vector tmpl(temp_path.begin(), temp_path.end());
        tmpl.push_back('\0');
        fd = mkstemp(tmpl.data());
        if (fd < 0)
        {
            *errors += std::string("\n  ") + label + ": no memfd and no temp file";
            return nullptr;
        }
        temp_path = tmpl.data();
        fchmod(fd, 0700);
    }

    size_t written = 0;
    while (written < size)
    {
        const ssize_t n = write(fd, begin + written, size - written);
        if (n <= 0)
        {
            close(fd);
            if (!temp_path.empty()) unlink(temp_path.c_str());
            *errors += std::string("\n  ") + label + ": short write";
            return nullptr;
        }
        written += static_cast<size_t>(n);
    }

    std::string path;
    if (from_memfd)
    {
        char proc[64];
        (void)snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
        path = proc;
    }
    else
    {
        path = temp_path;
    }

    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
        const char* e = dlerror();
        *errors += std::string("\n  ") + label + " (embedded): " + (e ? e : "unknown");
    }

    // The memfd descriptor stays open for the lifetime of the process. glibc
    // caches loaded objects by the path string it was given, and
    // /proc/self/fd/N names are only unique while N is: closing the descriptor
    // frees the number for reuse, and a later dlopen of the same path then
    // returns the *earlier* object's handle instead of loading anything - a
    // success that hands back the wrong library. MFD_CLOEXEC keeps it out of
    // child processes, and the mapping itself would keep the memory alive
    // regardless.
    if (!temp_path.empty())
    {
        // A mkstemp path is unique, so this descriptor can be released.
        close(fd);
        unlink(temp_path.c_str());
    }
    return handle;
}

#endif // DWRITECORE_EMBEDDED

// The implementation is an Android build: it needs liblog/libc/libm/libdl with
// bionic SONAMEs, and all its imports are versioned @LIBC, which glibc cannot
// satisfy. Pre-loading the stubs from bionic-compat/ registers those SONAMEs,
// and the loader then uses them to resolve the implementation's DT_NEEDED.
//
// RTLD_LOCAL matters here. These stubs define pthread_mutex_lock and friends,
// and in the global scope they would interpose on the host program's own
// threading. Local scope still satisfies the dependency while keeping the
// symbols invisible to everyone else.
//
// No conflict with the host libc, whose SONAMEs are libc.so.6 / libm.so.6 /
// libdl.so.2 - different names entirely.
void LoadBionicStubs(std::string* errors)
{
    static const char* kStubs[] = { "libc.so", "libm.so", "libdl.so", "liblog.so" };

    const char* dir_override = getenv(kBionicDirEnv);
    std::string dir = dir_override && *dir_override
                          ? std::string(dir_override)
                          : ThisLibraryDirectory() + "bionic";
    if (!dir.empty() && dir[dir.size() - 1] != '/')
    {
        dir += '/';
    }

    for (const char* stub : kStubs)
    {
        const std::string path = dir + stub;
        if (dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL) == nullptr)
        {
            const char* e = dlerror();
            *errors += "\n  bionic stub " + path + ": " + (e ? e : "unknown error");
        }
    }
}

void LoadImpl()
{
    const char* override_path = getenv(kImplPathEnv);

    std::string candidates[2];
    int count = 0;
    if (override_path != nullptr && *override_path != '\0')
    {
        candidates[count++] = override_path;
    }
    else
    {
        candidates[count++] = ThisLibraryDirectory() + kDefaultImplName;
        candidates[count++] = kDefaultImplName; // fall back to the search path
    }

    std::string errors;

#ifdef DWRITECORE_EMBEDDED
    // Everything needed is carried inside this library; nothing is read from
    // disk unless the embedded load fails and the search below takes over.
    LoadFromMemory(dwc_blob_libc_start, dwc_blob_libc_end, "libc.so", &errors);
    LoadFromMemory(dwc_blob_libm_start, dwc_blob_libm_end, "libm.so", &errors);
    LoadFromMemory(dwc_blob_libdl_start, dwc_blob_libdl_end, "libdl.so", &errors);
    LoadFromMemory(dwc_blob_liblog_start, dwc_blob_liblog_end, "liblog.so", &errors);
    if (override_path == nullptr || *override_path == '\0')
    {
        void* h = LoadFromMemory(dwc_blob_impl_start, dwc_blob_impl_end,
                                 "libdwritecore-impl", &errors);
        if (h != nullptr)
        {
            const auto cf =
                reinterpret_cast<PfnDWriteCoreCreateFactory>(
                    dlsym(h, "DWriteCoreCreateFactory"));
            if (cf != nullptr)
            {
                g_impl.handle = h;
                g_impl.createFactory = cf;
                g_impl.setFeatureStagingCallback =
                    reinterpret_cast<PfnSetFeatureStagingCallback>(
                        dlsym(h, "DWriteCoreSetFeatureStagingCallback"));
                return;
            }
            dlclose(h);
            errors += "\n  embedded implementation has no DWriteCoreCreateFactory";
        }
    }
#endif

    LoadBionicStubs(&errors);

    for (int i = 0; i < count; ++i)
    {
        void* h = dlopen(candidates[i].c_str(), RTLD_NOW | RTLD_LOCAL);
        if (h == nullptr)
        {
            const char* e = dlerror();
            errors += "\n  " + candidates[i] + ": " + (e ? e : "unknown error");
            continue;
        }

        // Guard against the SONAME collision described in the file header: if
        // the loader handed back this very library, the forward would recurse.
        Dl_info self;
        if (dladdr(reinterpret_cast<void*>(&LoadImpl), &self) != 0)
        {
            if (void* self_handle = dlopen(self.dli_fname, RTLD_NOW | RTLD_NOLOAD); self_handle != nullptr)
            {
                dlclose(self_handle);
                if (self_handle == h)
                {
                    dlclose(h);
                    errors += "\n  " + candidates[i] +
                              ": resolved to this shim itself (SONAME collision);"
                              " run tools/make_impl.py to produce a renamed copy";
                    continue;
                }
            }
        }

        const auto cf = reinterpret_cast<PfnDWriteCoreCreateFactory>(
            dlsym(h, "DWriteCoreCreateFactory"));
        if (cf == nullptr)
        {
            dlclose(h);
            errors += "\n  " + candidates[i] + ": no DWriteCoreCreateFactory export";
            continue;
        }

        g_impl.handle = h;
        g_impl.createFactory = cf;
        g_impl.setFeatureStagingCallback =
            reinterpret_cast<PfnSetFeatureStagingCallback>(
                dlsym(h, "DWriteCoreSetFeatureStagingCallback"));
        return;
    }

    g_impl.diagnostic = "libdwritecore shim: could not load the DWriteCore "
                        "implementation. Set " + std::string(kImplPathEnv) +
                        " to its path. Tried:" + errors;
}

// Data flow analysis has no model for dlopen/dlsym, so it reads every
// member below as null and concludes that nothing this shim loads is ever
// reachable. That verdict propagates: through DWriteCoreCreateFactory into
// GetFactories in cleartype/src/freetype.cpp, and from there over the whole
// ClearType rasterizer. The CppDFA suppressions in this file and that one
// all trace back here.
const Impl& GetImpl()
{
    pthread_once(&g_once, LoadImpl);
    return g_impl;
}

} // namespace

namespace dwc
{
namespace
{

// One proxy per real factory. DWRITE_FACTORY_TYPE_SHARED hands back the same
// object every time, so without this every DWriteCoreCreateFactory call would
// build another proxy - each holding a reference and each enumerating the whole
// font list again. ISOLATED factories are distinct objects and get one each.
struct ProxyCacheEntry
{
    IDWriteFactory9* real;
    FactoryProxy* proxy;
};

pthread_mutex_t g_proxy_lock = PTHREAD_MUTEX_INITIALIZER;
std::vector<ProxyCacheEntry>* g_proxies = nullptr;

FactoryProxy* ProxyFor(IDWriteFactory9* real)
{
    pthread_mutex_lock(&g_proxy_lock);
    if (g_proxies == nullptr)
    {
        g_proxies = new (std::nothrow) std::vector<ProxyCacheEntry>();
    }
    FactoryProxy* found = nullptr;
    if (g_proxies != nullptr)
    {
        for (size_t i = 0; i < g_proxies->size(); ++i)
        {
            if ((*g_proxies)[i].real == real)
            {
                found = (*g_proxies)[i].proxy;
                break;
            }
        }
        if (found != nullptr)
        {
            // Caller's reference on `real` is surplus: the cached proxy already
            // holds one.
            real->Release();
        }
        else
        {
            // The proxy takes the reference this function was handed.
            found = new (std::nothrow) FactoryProxy(real);
            if (found != nullptr)
            {
                const ProxyCacheEntry entry = { .real = real, .proxy = found };
                g_proxies->push_back(entry);
                // The cache keeps a reference of its own, so the entry can never
                // outlive the object it names. Nothing releases it: like the
                // implementation handle, this lives for the life of the process.
                found->AddRef();
            }
        }
    }
    pthread_mutex_unlock(&g_proxy_lock);
    return found;
}

} // namespace

// Replace the factory the implementation returned with one that can answer the
// system-font getters. Any IID outside the IDWriteFactory chain is left alone -
// the caller asked for something the proxy has nothing to add to.
// Always S_OK by contract: every way of declining leaves the caller holding
// the factory the implementation returned, which is a success for it.
// ReSharper disable once CppDFAConstantFunctionResult
static HRESULT WrapFactory(REFIID iid, IUnknown** factory)
{
    // An off switch, because this changes what an existing call returns.
    if (const char* off = getenv("DWRITECORE_SYSTEM_FONTS"); off != nullptr && (strcmp(off, "0") == 0 || strcasecmp(off, "off") == 0))
    {
        return S_OK;
    }

    IDWriteFactory9* real = nullptr;
    if (FAILED((*factory)->QueryInterface(DWRITE_UUIDOF(IDWriteFactory9),
                                          reinterpret_cast<void**>(&real))))
    {
        return S_OK;   // not a factory, or too old a one to proxy: hand it back
    }

    FactoryProxy* proxy = ProxyFor(real);
    if (proxy == nullptr)
    {
        real->Release();
        return S_OK;
    }

    IUnknown* wrapped = nullptr;
    if (FAILED(proxy->QueryInterface(iid, reinterpret_cast<void**>(&wrapped))))
    {
        return S_OK;   // the proxy does not implement what was asked for
    }
    (*factory)->Release();
    *factory = wrapped;
    return S_OK;
}

} // namespace dwc

// ---------------------------------------------------------------------------
// Exported entry points
// ---------------------------------------------------------------------------

extern "C" {

DWRITECORE_API const char* DWriteCoreShimGetLastLoadError(void)
{
    const Impl& impl = GetImpl();
    // ReSharper disable once CppDFAConstantConditions
    // ReSharper disable once CppDFAUnreachableCode
    return impl.handle != nullptr ? nullptr : impl.diagnostic.c_str();
}

DWRITECORE_API HRESULT DWriteCoreCreateFactory(
    const DWRITE_FACTORY_TYPE factoryType,
    REFIID iid,
    IUnknown** factory)
{
    const Impl& impl = GetImpl();
    if (impl.createFactory == nullptr)
    {
        if (factory != nullptr)
        {
            *factory = nullptr;
        }
        (void)fprintf(stderr, "%s\n", impl.diagnostic.c_str());
        return E_FAIL;
    }
    if (const HRESULT hr = impl.createFactory(factoryType, iid, factory); FAILED(hr) || factory == nullptr || *factory == nullptr)
    {
        return hr;
    }
    return dwc::WrapFactory(iid, factory);
}

// The system DirectWrite spelling. Office reaches DWriteCore through this name
// via libdwriteshim.so, which is a pure rename of DWriteCoreCreateFactory.
DWRITECORE_API HRESULT DWriteCreateFactory(
    const DWRITE_FACTORY_TYPE factoryType,
    REFIID iid,
    IUnknown** factory)
{
    return DWriteCoreCreateFactory(factoryType, iid, factory);
}

// Undocumented; not declared in any Windows App SDK header. The signature was
// recovered from libdwriteshim.so, whose DWrite10Velocity_SetFeatureStagingCallback
// tail-calls this with both argument registers passed through untouched.
DWRITECORE_API void DWriteCoreSetFeatureStagingCallback(void* context, void* callback)
{
    if (const Impl& impl = GetImpl(); impl.setFeatureStagingCallback != nullptr)
    {
        impl.setFeatureStagingCallback(context, callback);
    }
}

DWRITECORE_API void DWrite10Velocity_SetFeatureStagingCallback(void* context, void* callback)
{
    DWriteCoreSetFeatureStagingCallback(context, callback);
}

// ---------------------------------------------------------------------------
} // extern "C"
