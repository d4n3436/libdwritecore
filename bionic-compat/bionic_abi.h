/*
 * bionic_abi.h - the Android/bionic ABI facts this compatibility layer depends on.
 *
 * Every constant here was pinned against a source of truth, never assumed,
 * because a wrong size corrupts memory silently instead of failing to build:
 *
 *   pthread object sizes   AOSP bionic libc/include/bits/pthread_types.h
 *   FILE size / __sF       the shipped libdwritecore.so itself (see below)
 *
 * libdwritecore.so is built for Android 21, where bionic still defines
 * stderr as &__sF[2]. The binary computes that address as a constant offset
 * from the imported __sF symbol, and every such site loads 0x130 = 304:
 *
 *   mov  $0x130,%ebx
 *   add  __sF(%rip),%rbx        # -> &__sF[2]
 *
 * 304 / 2 gives a 152-byte bionic FILE, which is what the layout below encodes.
 */

#ifndef BIONIC_ABI_H
#define BIONIC_ABI_H

#include <stdint.h>

/* --- pthread opaque object sizes, LP64 (AOSP bits/pthread_types.h) ---------
 *
 * These are the sizes DWriteCore reserved inside its own structures. Our
 * implementations must fit within them; each uses far less. Note the layouts
 * differ from glibc's even where the sizes happen to coincide, which is why
 * these primitives are reimplemented rather than forwarded.
 *
 *   bionic mutex   int32_t[10] = 40 bytes   (glibc: 40)
 *   bionic cond    int64_t[4]  = 32 bytes   (glibc: 48  <-- would overflow)
 *   bionic rwlock  int32_t[14] = 56 bytes   (glibc: 56)
 *   bionic once    int         =  4 bytes
 */
#define BIONIC_MUTEX_SIZE   40
#define BIONIC_COND_SIZE    32
#define BIONIC_RWLOCK_SIZE  56
#define BIONIC_ONCE_SIZE     4

/* bionic: typedef long pthread_mutexattr_t; */
#define BIONIC_MUTEXATTR_SIZE 8

/* bionic pthread_mutex type values, identical numbering to glibc. */
#define BIONIC_MUTEX_NORMAL      0
#define BIONIC_MUTEX_RECURSIVE   1
#define BIONIC_MUTEX_ERRORCHECK  2

/* --- stdio -----------------------------------------------------------------
 * Derived from the binary as described above. __sF must be at least
 * 3 * BIONIC_FILE_SIZE bytes so &__sF[2] lands inside our array.
 */
#define BIONIC_FILE_SIZE 152

/* Defined in libc_compat.c. Declared here so the definition has a prior
 * declaration to match, which is what -Wmissing-variable-declarations asks
 * for; the array is exported, so it cannot be static. */
extern unsigned char __sF[3 * BIONIC_FILE_SIZE];

/*
 * Our view of each pthread object. Each is declared as a full-size bionic
 * object so sizeof() checks hold, with the live state in the leading fields.
 *
 * Zero is the valid initial state for all of them, which matters: bionic's
 * PTHREAD_MUTEX_INITIALIZER and friends are all-zero, so a statically
 * initialized lock inside DWriteCore is already correctly initialized for us.
 */

typedef struct {
    int32_t futex;      /* 0 unlocked, 1 locked, 2 locked with waiters */
    int32_t type;       /* BIONIC_MUTEX_* */
    int32_t owner;      /* owning tid, for recursive and errorcheck mutexes */
    int32_t recursion;  /* recursive acquisition depth */
    int32_t _pad[BIONIC_MUTEX_SIZE / 4 - 4];
} bionic_mutex_t;

typedef struct {
    int32_t seq;        /* bumped on every signal/broadcast; the futex word */
    int32_t _pad[BIONIC_COND_SIZE / 4 - 1];
} bionic_cond_t;

typedef struct {
    int32_t state;      /* -1 write-locked, 0 free, >0 reader count */
    int32_t _pad[BIONIC_RWLOCK_SIZE / 4 - 1];
} bionic_rwlock_t;

_Static_assert(sizeof(bionic_mutex_t)  == BIONIC_MUTEX_SIZE,  "mutex size");
_Static_assert(sizeof(bionic_cond_t)   == BIONIC_COND_SIZE,   "cond size");
_Static_assert(sizeof(bionic_rwlock_t) == BIONIC_RWLOCK_SIZE, "rwlock size");

#endif /* BIONIC_ABI_H */
