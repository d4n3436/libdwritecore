/*
 * libdl_compat.c - bionic libdl.so stand-in.
 *
 * A single import, dl_iterate_phdr, versioned @LIBC against "libdl.so".
 * DWriteCore's Rust backtrace support uses it; struct dl_phdr_info is the
 * standard ELF layout on both libcs, so this is a pure forward.
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
#include <link.h>
#include <stdlib.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

EXPORT int dl_iterate_phdr(int (*cb)(struct dl_phdr_info*, size_t, void*), void* data)
{
    static void* libc;
    if (libc == NULL)
    {
        libc = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
    }
    int (*real)(int (*)(struct dl_phdr_info*, size_t, void*), void*) =
        libc ? dlsym(libc, "dl_iterate_phdr") : NULL;
    if (real == NULL)
    {
        return 0; /* no modules enumerated; callers treat this as "no symbols" */
    }
    return real(cb, data);
}
