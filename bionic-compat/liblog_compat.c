/*
 * liblog_compat.c - Android liblog.so stand-in.
 *
 * One unversioned import, __android_log_print. DWriteCore logs diagnostics
 * through it; off Android the sensible destination is stderr.
 */

#include <stdarg.h>
#include <stdio.h>

/* A va_list is initialized by va_start and by nothing else, which the
   analysis does not model, so every varargs forwarder here reads as
   uninitialized. */
// ReSharper disable CppLocalVariableMightNotBeInitialized
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

#define EXPORT __attribute__((visibility("default")))

EXPORT int __android_log_print(const int prio, const char* tag, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)fprintf(stderr, "[dwritecore:%s:%d] ", tag ? tag : "?", prio);
    // NOLINTNEXTLINE  -- va_start above initializes ap; the checker misses it
    const int n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
    return n;
}
