/*
 * libc_compat.c - bionic libc.so stand-in for loading libdwritecore.so on glibc.
 *
 * libdwritecore.so imports 84 symbols, all but two versioned @LIBC, and 76 of
 * them from "libc.so". glibc defines GLIBC_2.x versions and never LIBC, so the
 * host libc cannot satisfy those references at all; this library declares the
 * LIBC version (libc.map) and provides the whole set, forwarding the ones that
 * are ABI-compatible.
 *
 * What must NOT be forwarded is the pthread surface. DWriteCore embeds mutexes,
 * condition variables and rwlocks inside its own structures, sized for bionic.
 * bionic's pthread_cond_t is 32 bytes where glibc's is 48, so handing one to
 * glibc would write past the end of it. Those primitives are implemented here
 * on futexes, using only the leading bytes of each object and treating all-zero
 * as "initialized and unlocked" - exactly what bionic's static initializers
 * produce, so DWriteCore's statically initialized locks work untouched.
 *
 * Two re-entrancy hazards shape the rest:
 *   - This library defines malloc, so calling malloc from within it would
 *     recurse. The allocator forwards to glibc's __libc_malloc family by name
 *     instead of through dlsym, which also avoids allocating during startup.
 *   - It defines memset/memcpy too, so it must be built with -fno-builtin or
 *     the compiler turns their bodies into calls to themselves.
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

#include "bionic_abi.h"

/* A va_list is initialized by va_start and by nothing else, which the
   analysis does not model, so every varargs forwarder here reads as
   uninitialized. */
// ReSharper disable CppLocalVariableMightNotBeInitialized
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <locale.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define EXPORT __attribute__((visibility("default")))

/*
 * Recent glibc defines these as function-like _Generic macros (see
 * __glibc_const_generic in sys/cdefs.h) so that the return type tracks the
 * constness of the argument. That makes them unusable as definition names,
 * so drop the macros and keep the plain external functions underneath.
 */
#undef memchr
#undef wmemchr

/* ------------------------------------------------------------------------ */
/* Forwarding to the host libc                                              */
/* ------------------------------------------------------------------------ */

/* glibc's public allocator aliases, referenced directly - see header comment. */
extern void* __libc_malloc(size_t);
extern void  __libc_free(void*);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern int   __xpg_strerror_r(int, char*, size_t);

static void* g_libc;

/*
 * The raw syscall entry, cached at load time. The locks below are a hot path
 * and this file also *defines* syscall(), so going through the normal
 * forwarder would mean a dlsym on every futex operation.
 */
static long (*g_syscall)(long, ...);

__attribute__((constructor(101)))
static void bionic_compat_init(void)
{
    /* Already mapped in every process; this only takes a handle to it. */
    g_libc = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
    if (g_libc != NULL && g_syscall == NULL)
    {
        *(void**)&g_syscall = dlsym(g_libc, "syscall");
    }
}

/* Counted here instead of with strlen(): this file exports its own strlen as
 * a forwarding shim (FWD1 below), and fatal() is what runs when that
 * forwarding is what broke, so resolving strlen through it could come straight
 * back here. The reads go through a volatile pointer so that no compiler can
 * recognize the loop and rewrite it back into the strlen call this exists to
 * avoid. */
/* Two callers, two different messages; the analysis sees only one path here. */
// ReSharper disable once CppDFAConstantParameter
static size_t message_length(const char* msg)
{
    const volatile char* p = msg;
    size_t n = 0;
    while (p[n] != '\0')
    {
        n++;
    }
    return n;
}

// ReSharper disable once CppDFAConstantParameter
_Noreturn static void fatal(const char* msg)
{
    if (g_syscall)
    {
        g_syscall(SYS_write, 2, msg, (long)message_length(msg));
    }
    _exit(127);
}

static void* host(const char* name)
{
    if (g_libc == NULL)
    {
        bionic_compat_init();
    }
    void* fn = g_libc ? dlsym(g_libc, name) : NULL;
    if (fn == NULL)
    {
        fatal("bionic-compat: missing host libc symbol\n");
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

/* ------------------------------------------------------------------------ */
/* futex primitives                                                         */
/* ------------------------------------------------------------------------ */

/* Six arguments because that is how many a Linux syscall takes, not because
   every caller here needs them: the ones present pass zero for the tail. */
// ReSharper disable CppDFAConstantParameter
static long raw_syscall6(const long n, const long a, const long b, const long c, const long d, const long e, const long f)
{
    if (g_syscall == NULL)
    {
        bionic_compat_init();
        if (g_syscall == NULL)
        {
            fatal("bionic-compat: cannot resolve syscall()\n");
        }
    }
    return g_syscall(n, a, b, c, d, e, f);
}
// ReSharper restore CppDFAConstantParameter

static int futex_wait(volatile int32_t* addr, const int32_t expected)
{
    return (int)raw_syscall6(SYS_futex, (long)addr, FUTEX_WAIT_PRIVATE,
                             expected, 0, 0, 0);
}

static int futex_wake(volatile int32_t* addr, const int count)
{
    return (int)raw_syscall6(SYS_futex, (long)addr, FUTEX_WAKE_PRIVATE,
                             count, 0, 0, 0);
}

static int32_t this_tid(void)
{
    /* Cached per thread: the value never changes and the syscall is not free. */
    static __thread int32_t tid;
    if (tid == 0)
    {
        tid = (int32_t)raw_syscall6(SYS_gettid, 0, 0, 0, 0, 0, 0);
    }
    return tid;
}

/* ------------------------------------------------------------------------ */
/* pthread: mutex                                                           */
/* ------------------------------------------------------------------------ */

EXPORT int pthread_mutexattr_init(long* attr)
{
    *attr = BIONIC_MUTEX_NORMAL;
    return 0;
}

/* The pointee stays non-const: these are the POSIX prototypes this library
   exports, and const there would be a different declaration. */
// ReSharper disable once CppParameterMayBeConstPtrOrRef
EXPORT int pthread_mutexattr_destroy(long* attr)
{
    (void)attr;
    return 0;
}

EXPORT int pthread_mutexattr_settype(long* attr, const int type)
{
    if (type != BIONIC_MUTEX_NORMAL && type != BIONIC_MUTEX_RECURSIVE &&
        type != BIONIC_MUTEX_ERRORCHECK)
    {
        return EINVAL;
    }
    *attr = type;
    return 0;
}

/* Returns int because POSIX says so. Nothing here can fail. */
// ReSharper disable once CppDFAConstantFunctionResult
EXPORT int pthread_mutex_init(bionic_mutex_t* m, const long* attr)
{
    /* Written field-wise, because this file defines memset. */
    m->futex = 0;
    m->owner = 0;
    m->recursion = 0;
    m->type = attr ? (int32_t)*attr : BIONIC_MUTEX_NORMAL;
    return 0;
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
EXPORT int pthread_mutex_destroy(bionic_mutex_t* m)
{
    return __atomic_load_n(&m->futex, __ATOMIC_ACQUIRE) != 0 ? EBUSY : 0;
}

EXPORT int pthread_mutex_lock(bionic_mutex_t* m)
{
    int32_t self = 0;

    if (m->type != BIONIC_MUTEX_NORMAL)
    {
        self = this_tid();
        if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == self)
        {
            if (m->type == BIONIC_MUTEX_RECURSIVE)
            {
                ++m->recursion;
                return 0;
            }
            return EDEADLK; /* errorcheck */
        }
    }

    /* Uncontended fast path: 0 -> 1. */
    int32_t expected = 0;
    if (!__atomic_compare_exchange_n(&m->futex, &expected, 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        /* Contended: publish "locked with waiters" (2), then sleep. */
        do
        {
            if (expected == 2 ||
                __atomic_compare_exchange_n(&m->futex, &expected, 2, 0,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            {
                futex_wait(&m->futex, 2);
            }
            expected = 0;
        } while (!__atomic_compare_exchange_n(&m->futex, &expected, 2, 0,
                                              __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    }

    if (m->type != BIONIC_MUTEX_NORMAL)
    {
        __atomic_store_n(&m->owner, self, __ATOMIC_RELAXED);
        m->recursion = 0;
    }
    return 0;
}

/* Returns int because POSIX says so. Nothing here can fail. */
// ReSharper disable once CppDFAConstantFunctionResult
EXPORT int pthread_mutex_unlock(bionic_mutex_t* m)
{
    if (m->type != BIONIC_MUTEX_NORMAL)
    {
        if (m->recursion > 0)
        {
            --m->recursion;
            return 0;
        }
        __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);
    }

    if (__atomic_exchange_n(&m->futex, 0, __ATOMIC_RELEASE) == 2)
    {
        futex_wake(&m->futex, 1);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* pthread: condition variable                                              */
/*                                                                          */
/* Paired with the mutex above: a condvar must release the mutex atomically */
/* with the wait, so both have to be the same implementation.               */
/* ------------------------------------------------------------------------ */

// ReSharper disable once CppParameterMayBeConstPtrOrRef
EXPORT int pthread_cond_destroy(bionic_cond_t* c)
{
    (void)c;
    return 0;
}

EXPORT int pthread_cond_wait(bionic_cond_t* c, bionic_mutex_t* m)
{
    const int32_t seq = __atomic_load_n(&c->seq, __ATOMIC_RELAXED);
    pthread_mutex_unlock(m);
    futex_wait(&c->seq, seq);
    pthread_mutex_lock(m);
    return 0;
}

EXPORT int pthread_cond_signal(bionic_cond_t* c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    futex_wake(&c->seq, 1);
    return 0;
}

EXPORT int pthread_cond_broadcast(bionic_cond_t* c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    futex_wake(&c->seq, INT_MAX);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* pthread: rwlock                                                            */
/*                                                                            */
/* Only rdlock/wrlock/unlock are imported, so every rwlock DWriteCore uses is */
/* statically initialized - all zero, which is our unlocked state.            */
/* -------------------------------------------------------------------------- */

EXPORT int pthread_rwlock_rdlock(bionic_rwlock_t* rw)
{
    for (;;)
    {
        int32_t s = __atomic_load_n(&rw->state, __ATOMIC_RELAXED);
        if (s >= 0)
        {
            if (__atomic_compare_exchange_n(&rw->state, &s, s + 1, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            {
                return 0;
            }
        }
        else
        {
            futex_wait(&rw->state, s);
        }
    }
}

EXPORT int pthread_rwlock_wrlock(bionic_rwlock_t* rw)
{
    for (;;)
    {
        int32_t s = 0;
        if (__atomic_compare_exchange_n(&rw->state, &s, -1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        {
            return 0;
        }
        futex_wait(&rw->state, s);
    }
}

EXPORT int pthread_rwlock_unlock(bionic_rwlock_t* rw)
{
    if (__atomic_load_n(&rw->state, __ATOMIC_RELAXED) < 0)
    {
        __atomic_store_n(&rw->state, 0, __ATOMIC_RELEASE);
        futex_wake(&rw->state, INT_MAX);
    }
    else if (__atomic_sub_fetch(&rw->state, 1, __ATOMIC_RELEASE) == 0)
    {
        futex_wake(&rw->state, INT_MAX);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* pthread: once                                                            */
/* ------------------------------------------------------------------------ */

#define ONCE_PENDING 0
#define ONCE_RUNNING 1
#define ONCE_DONE    2

/* Returns int because POSIX says so. Nothing here can fail. */
// ReSharper disable once CppDFAConstantFunctionResult
EXPORT int pthread_once(int32_t* once, void (*init)(void))
{
    for (;;)
    {
        const int32_t s = __atomic_load_n(once, __ATOMIC_ACQUIRE);
        if (s == ONCE_DONE)
        {
            return 0;
        }
        if (s == ONCE_PENDING)
        {
            int32_t expected = ONCE_PENDING;
            if (__atomic_compare_exchange_n(once, &expected, ONCE_RUNNING, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            {
                init();
                __atomic_store_n(once, ONCE_DONE, __ATOMIC_RELEASE);
                futex_wake(once, INT_MAX);
                return 0;
            }
        }
        else
        {
            futex_wait(once, ONCE_RUNNING);
        }
    }
}

/*
 * Threads and TLS keys are handle-based rather than embedded in DWriteCore's
 * structures, and bionic's pthread_t and pthread_key_t are the same width as
 * glibc's, so these forward safely. DWriteCore never imports pthread_attr_init,
 * so the attribute pointer it passes is always NULL.
 */
EXPORT int pthread_create(unsigned long* t, const void* attr,
                          void* (*fn)(void*), void* arg)
{
    return HOST(pthread_create)((pthread_t*)t, (const pthread_attr_t*)attr, fn, arg);
}

EXPORT int pthread_join(const unsigned long t, void** ret)
{
    return HOST(pthread_join)((pthread_t)t, ret);
}

EXPORT int pthread_detach(const unsigned long t)
{
    return HOST(pthread_detach)((pthread_t)t);
}

EXPORT int pthread_key_create(unsigned int* key, void (*dtor)(void*))
{
    return HOST(pthread_key_create)((pthread_key_t*)key, dtor);
}

EXPORT int pthread_key_delete(const unsigned int key)
{
    return HOST(pthread_key_delete)((pthread_key_t)key);
}

EXPORT void* pthread_getspecific(const unsigned int key)
{
    return HOST(pthread_getspecific)((pthread_key_t)key);
}

EXPORT int pthread_setspecific(const unsigned int key, const void* value)
{
    return HOST(pthread_setspecific)((pthread_key_t)key, value);
}

/* ------------------------------------------------------------------------ */
/* stdio                                                                    */
/*                                                                          */
/* DWriteCore takes stderr as &__sF[2] using bionic's 152-byte FILE. We own */
/* both the array and every function that consumes it, so the pointer only  */
/* has to round-trip through this translation.                              */
/* ------------------------------------------------------------------------ */

EXPORT unsigned char __sF[3 * BIONIC_FILE_SIZE];

static FILE* real_file(FILE* f)
{
    const ptrdiff_t off = (unsigned char*)f - __sF;
    if (off < 0 || off >= (ptrdiff_t)sizeof(__sF))
    {
        return f; /* not one of ours - pass through untouched */
    }
    switch (off / BIONIC_FILE_SIZE)
    {
    case 0:  return stdin;
    case 1:  return stdout;
    default: return stderr;
    }
}

/* No const on `ap`: va_list is an array type on this ABI, so the qualifier
   lands on the element and changes the function's type - the compiler
   rejects the redeclaration. */
// ReSharper disable once CppParameterMayBeConst
EXPORT int vfprintf(FILE* f, const char* fmt, va_list ap)
{
    return HOST(vfprintf)(real_file(f), fmt, ap);
}

EXPORT int fprintf(FILE* f, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

EXPORT size_t fwrite(const void* p, const size_t size, const size_t n, FILE* f)
{
    return HOST(fwrite)(p, size, n, real_file(f));
}

EXPORT int fputc(const int c, FILE* f)
{
    return HOST(fputc)(c, real_file(f));
}

EXPORT int fflush(FILE* f)
{
    return HOST(fflush)(f ? real_file(f) : NULL);
}

EXPORT int snprintf(char* buf, const size_t n, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int r = HOST(vsnprintf)(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* No const on `ap`; see vfprintf above. */
// ReSharper disable once CppParameterMayBeConst
EXPORT int vasprintf(char** out, const char* fmt, va_list ap)
{
    return HOST(vasprintf)(out, fmt, ap);
}

/* ------------------------------------------------------------------------ */
/* bionic-specific entry points                                             */
/* ------------------------------------------------------------------------ */

EXPORT int* __errno(void)
{
    return __errno_location();
}

EXPORT _Noreturn void __assert2(const char* file, const int line, const char* fn, const char* expr)
{
    (void)fprintf(stderr, "bionic-compat: assertion failed: %s (%s:%d in %s)\n",
            expr ? expr : "?", file ? file : "?", line, fn ? fn : "?");
    abort();
}

EXPORT _Noreturn void __stack_chk_fail(void)
{
    (void)fprintf(stderr, "bionic-compat: stack smashing detected\n");
    abort();
}

/* _FORTIFY_SOURCE variants; the trailing argument is the known buffer size. */
EXPORT void* __memset_chk(void* d, const int c, const size_t n, const size_t buflen)
{
    if (n > buflen)
    {
        __assert2(__FILE__, __LINE__, "__memset_chk", "buffer overflow");
    }
    return memset(d, c, n);
}

EXPORT size_t __strlen_chk(const char* s, const size_t buflen)
{
    const size_t n = strlen(s);
    if (n >= buflen)
    {
        __assert2(__FILE__, __LINE__, "__strlen_chk", "buffer overflow");
    }
    return n;
}

EXPORT int __open_2(const char* path, const int flags)
{
    return HOST(open)(path, flags);
}

EXPORT pid_t gettid(void)
{
    return this_tid();
}

EXPORT void android_set_abort_message(const char* msg)
{
    if (msg)
    {
        (void)fprintf(stderr, "bionic-compat: abort message: %s\n", msg);
    }
}

/* ------------------------------------------------------------------------ */
/* Straight forwards                                                        */
/*                                                                          */
/* ABI-identical between bionic and glibc on x86-64: struct stat and struct */
/* iovec both follow the kernel layout here, so no translation is needed.   */
/* ------------------------------------------------------------------------ */

EXPORT void* malloc(const size_t n)                 { return __libc_malloc(n); }
EXPORT void  free(void* p)                          { __libc_free(p); }
EXPORT void* calloc(const size_t n, const size_t s) { return __libc_calloc(n, s); }
EXPORT void* realloc(void* p, const size_t n)       { return __libc_realloc(p, n); }

/*
 * bionic's strerror_r is the XSI flavor returning int, while glibc's visible
 * declaration under _GNU_SOURCE returns char*. Defining it under an asm label
 * sidesteps the prototype clash; __xpg_strerror_r supplies XSI semantics.
 */
EXPORT int bionic_strerror_r(int e, char* buf, size_t n) __asm__("strerror_r");
EXPORT int bionic_strerror_r(const int e, char* buf, const size_t n)
{
    return __xpg_strerror_r(e, buf, n);
}

EXPORT _Noreturn void abort(void)
{
    HOST(abort)();
    /* abort() does not return; this states it for the caller's benefit. */
    // ReSharper disable once CppDFAUnreachableCode
    __builtin_unreachable();
}

EXPORT int open(const char* path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    const mode_t mode = (mode_t)va_arg(ap, int);
    va_end(ap);
    return HOST(open)(path, flags, mode);
}

EXPORT long syscall(long number, ...)
{
    va_list ap;
    va_start(ap, number);
    const long a = va_arg(ap, long), b = va_arg(ap, long), c = va_arg(ap, long);
    const long d = va_arg(ap, long), e = va_arg(ap, long), f = va_arg(ap, long);
    va_end(ap);
    return HOST(syscall)(number, a, b, c, d, e, f);
}

EXPORT void syslog(const int prio, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    HOST(vsyslog)(prio, fmt, ap);
    va_end(ap);
}

#define FWD1(ret, fn, T1)                      EXPORT ret fn(T1 a) { return HOST(fn)(a); }
#define FWD2(ret, fn, T1, T2)                  EXPORT ret fn(T1 a, T2 b) { return HOST(fn)(a, b); }
#define FWD3(ret, fn, T1, T2, T3)              EXPORT ret fn(T1 a, T2 b, T3 c) { return HOST(fn)(a, b, c); }
#define FWD6(ret, fn, T1, T2, T3, T4, T5, T6)  \
    EXPORT ret fn(T1 a, T2 b, T3 c, T4 d, T5 e, T6 f) { return HOST(fn)(a, b, c, d, e, f); }

#define FWD0V(fn)          EXPORT void fn(void) { HOST(fn)(); }
#define FWD1V(fn, T1)      EXPORT void fn(T1 a) { HOST(fn)(a); }
#define FWD3V(fn, T1, T2, T3) EXPORT void fn(T1 a, T2 b, T3 c) { HOST(fn)(a, b, c); }

/* Written out, not macro-generated: the parameter is a function pointer. */
EXPORT int __cxa_atexit(void (*fn)(void*), void* arg, void* dso)
{
    return HOST(__cxa_atexit)(fn, arg, dso);
}

FWD1(int, close, int)
FWD0V(closelog)
FWD1V(__cxa_finalize, void*)
FWD2(int, clock_gettime, clockid_t, struct timespec*)
FWD2(int, fstat, int, struct stat*)
FWD2(char*, getcwd, char*, size_t)
FWD1(char*, getenv, const char*)
FWD3(off64_t, lseek64, int, off64_t, int)
FWD3(void*, memchr, const void*, int, size_t)
FWD3(int, memcmp, const void*, const void*, size_t)
FWD3(void*, memcpy, void*, const void*, size_t)
FWD3(void*, memmove, void*, const void*, size_t)
FWD3(void*, memset, void*, int, size_t)
FWD6(void*, mmap, void*, size_t, int, int, int, off_t)
FWD2(int, munmap, void*, size_t)
FWD3V(openlog, const char*, int, int)
FWD3(int, posix_memalign, void**, size_t, size_t)
FWD3(ssize_t, read, int, void*, size_t)
FWD3(ssize_t, readlink, const char*, char*, size_t)
FWD2(char*, realpath, const char*, char*)
FWD2(char*, setlocale, int, const char*)
FWD2(int, stat, const char*, struct stat*)
FWD2(int, strcmp, const char*, const char*)
FWD1(size_t, strlen, const char*)
FWD3(wchar_t*, wmemchr, const wchar_t*, wchar_t, size_t)
FWD3(ssize_t, write, int, const void*, size_t)
FWD3(ssize_t, writev, int, const struct iovec*, int)
FWD3(ssize_t, getrandom, void*, size_t, unsigned int)
