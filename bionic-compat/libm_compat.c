/*
 * libm_compat.c - bionic libm.so stand-in.
 *
 * Six math imports, all versioned @LIBC against "libm.so". The functions
 * themselves are ABI-identical to glibc's, so these are pure forwards; the
 * stub exists only to supply a library with the right SONAME and a LIBC
 * version definition.
 *
 * Built with -fno-builtin so the compiler does not turn each body into a call
 * to itself.
 */

#define _GNU_SOURCE

/* dlsym() returns void*, and every symbol resolved here is a function. ISO C
   has no conversion between an object pointer and a function pointer, which is
   what -Wpedantic reports at each of these sites; POSIX requires the
   conversion to work and this whole file exists to perform it. gcc has no
   narrower switch for that one diagnostic, so -Wpedantic is off for the file. */
#pragma GCC diagnostic ignored "-Wpedantic"

/* Nothing declares these but the definitions themselves. They are the libc,
   libm, libdl and liblog entry points this library replaces, and their
   real declarations are in the host headers - which cannot be included
   here, because the bionic types they would bring in are not the host's.
   gcc and clang spell the same diagnostic differently. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#else
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#endif

#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

/* No constexpr in this file: it is C, and nothing here pins a C standard,
   so the compiler's default decides whether the keyword exists. */
// ReSharper disable CppVariableCanBeMadeConstexpr

#define EXPORT __attribute__((visibility("default")))

static void* g_libm;

/* Called by the loader through the constructor attribute, which data flow
   analysis has no model for, and again lazily below if a forwarder runs first. */
// ReSharper disable once CppDFAUnreachableFunctionCall
__attribute__((constructor(101)))
static void libm_compat_init(void)
{
    g_libm = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
}

static void* host(const char* name)
{
    if (g_libm == NULL)
    {
        libm_compat_init();
    }
    void* fn = g_libm ? dlsym(g_libm, name) : NULL;
    if (fn == NULL)
    {
        const char msg[] = "bionic-compat: missing host libm symbol\n";
        const ssize_t ignored = write(2, msg, sizeof(msg) - 1);
        (void)ignored;
        _exit(127);
    }
    return fn;
}

/* One resolution per call site, not one per call. host() ends in a dlsym, and
   these forward the C library calls DirectWrite makes on its hot path.
   The load and store are relaxed because the pointer is the whole payload: two
   threads arriving together resolve the same value, and neither publishes
   anything the other has to see in order. */
#define HOST(fn) (__extension__({                                      \
    static __typeof__(fn)* cached_fn;                                  \
    __typeof__(fn)* resolved_fn =                                      \
        __atomic_load_n(&cached_fn, __ATOMIC_RELAXED);                 \
    if (resolved_fn == NULL)                                           \
    {                                                                  \
        resolved_fn = (__typeof__(fn)*)host(#fn);                      \
        __atomic_store_n(&cached_fn, resolved_fn, __ATOMIC_RELAXED);   \
    }                                                                  \
    resolved_fn;                                                       \
}))

EXPORT float  atan2f(const float y, const float x) { return HOST(atan2f)(y, x); }
EXPORT float  expf(const float x)                  { return HOST(expf)(x); }
EXPORT float  logf(const float x)                  { return HOST(logf)(x); }
EXPORT float  tanf(const float x)                  { return HOST(tanf)(x); }
EXPORT double pow(const double x, const double y)  { return HOST(pow)(x, y); }

EXPORT void sincosf(const float x, float* s, float* c)
{
    HOST(sincosf)(x, s, c);
}
