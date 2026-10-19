/*
 * libc symbols referenced by the Rust std inside the Cedar staticlib that the
 * seL4 musllibc does not provide. Cedar's decision path never calls them;
 * each stub fails like an unsupported syscall would.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>

/* glibc >= 2.35 hook used by the cross libgcc unwinder; "not found" makes it
 * fall back to dl_iterate_phdr (musl). Cedar is built with panic=abort. */
struct dl_find_object;
int _dl_find_object(void *pc, struct dl_find_object *result)
{
    (void)pc;
    (void)result;
    return -1;
}

/* compiler-builtins probes AT_HWCAP for LSE atomics from a constructor that
 * runs before musl's auxv is set up under sel4runtime (NULL deref). The
 * Cortex-A35 is ARMv8.0 (no LSE), so "no hwcaps" is the truth here. Strong
 * definition overrides musl's weak alias. */
unsigned long getauxval(unsigned long type)
{
    (void)type;
    return 0;
}

/* Rust std seeds HashMap hashing from getrandom(); seL4 musllibc has no such
 * syscall and its /dev/urandom fallback asserts. As in the paper's Microkit
 * Cedar build, use deterministic benchmark-only entropy (NOT a production
 * random source; recorded in the evidence). */
#include <stddef.h>
#include <sys/types.h>
ssize_t getrandom(void *buf, size_t len, unsigned flags)
{
    static unsigned long long x = 0x243f6a8885a308d3ull;
    unsigned char *p = buf;
    (void)flags;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        p[i] = (unsigned char)x;
    }
    return (ssize_t)len;
}

/* musl finds the main thread's stack by probing with mremap/munmap, which
 * seL4 musllibc does not implement (assert). Rust std only uses the result
 * for stack-guard bookkeeping and tolerates failure. */
#include <pthread.h>
int pthread_getattr_np(pthread_t t, pthread_attr_t *a)
{
    (void)t;
    (void)a;
    return ENOSYS;
}
