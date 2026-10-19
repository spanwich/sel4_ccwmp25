/*
 * K0 heap for the Cedar engine (Rust std -> libc malloc family).
 *
 * seL4 musllibc backs large allocations with mmap but implements neither
 * munmap nor mremap, so Rust's realloc of large buffers aborts. This replaces
 * the malloc family with power-of-two size classes carved from a static arena
 * with per-class free lists: no system calls at all. Memory is never
 * returned to the system; freed blocks are reused by their class.
 * The arena size is a recorded experiment parameter (C5 footprint).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef K0_ARENA_BYTES
#define K0_ARENA_BYTES (48u << 20)
#endif
#define MIN_SHIFT 5  /* 32 B blocks, 16 B header => 16 B usable minimum */
#define MAX_SHIFT 26 /* 64 MiB */
#define HDR 16u

static uint8_t arena[K0_ARENA_BYTES] __attribute__((aligned(4096)));
static size_t brk_off;
static void *free_list[MAX_SHIFT + 1];
size_t k0_heap_high_water;

struct hdr {
    uint32_t shift;
    uint32_t magic;
    void *raw; /* aligned payloads: the block to free */
};
#define MAGIC 0x6b306d61u
#define MAGIC_ALIGNED 0x6b30616cu

static unsigned class_of(size_t n)
{
    unsigned s = MIN_SHIFT;
    while (s <= MAX_SHIFT && ((size_t)1 << s) < n + HDR) {
        s++;
    }
    return s;
}

void *malloc(size_t n)
{
    unsigned s = class_of(n);
    if (s > MAX_SHIFT) {
        errno = ENOMEM;
        return NULL;
    }
    struct hdr *h = free_list[s];
    if (h) {
        free_list[s] = *(void **)(h + 1);
    } else {
        size_t sz = (size_t)1 << s;
        if (brk_off + sz > sizeof(arena)) {
            errno = ENOMEM;
            return NULL;
        }
        h = (struct hdr *)(arena + brk_off);
        brk_off += sz;
        if (brk_off > k0_heap_high_water) {
            k0_heap_high_water = brk_off;
        }
    }
    h->shift = s;
    h->magic = MAGIC;
    h->raw = h + 1;
    return h + 1;
}

void free(void *p)
{
    if (!p) {
        return;
    }
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic == MAGIC_ALIGNED) {
        free(h->raw);
        return;
    }
    if (h->magic != MAGIC) {
        __builtin_trap();
    }
    *(void **)p = free_list[h->shift];
    free_list[h->shift] = h;
}

size_t malloc_usable_size(void *p)
{
    if (!p) {
        return 0;
    }
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic == MAGIC_ALIGNED) {
        return h->shift; /* exact requested size for aligned blocks */
    }
    return ((size_t)1 << h->shift) - HDR;
}

void *calloc(size_t a, size_t b)
{
    size_t n;
    if (__builtin_mul_overflow(a, b, &n)) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(n);
    if (p) {
        memset(p, 0, n);
    }
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) {
        return malloc(n);
    }
    size_t have = malloc_usable_size(p);
    if (n <= have) {
        return p;
    }
    void *q = malloc(n);
    if (q) {
        memcpy(q, p, have);
        free(p);
    }
    return q;
}

/* Alignment above 16 B: place the payload inside a larger block and keep a
 * shadow header that points back at it, so free() releases the raw block. */
int posix_memalign(void **out, size_t align, size_t n)
{
    if (align <= HDR) {
        *out = malloc(n);
        return *out ? 0 : ENOMEM;
    }
    uint8_t *raw = malloc(n + align + HDR);
    if (!raw) {
        return ENOMEM;
    }
    uintptr_t a = ((uintptr_t)raw + HDR + align - 1) & ~(uintptr_t)(align - 1);
    struct hdr *h = (struct hdr *)a - 1;
    h->shift = (uint32_t)n;
    h->magic = MAGIC_ALIGNED;
    h->raw = raw;
    *out = (void *)a;
    return 0;
}

void *aligned_alloc(size_t align, size_t n)
{
    void *p = NULL;
    return posix_memalign(&p, align, n) ? NULL : p;
}

void *memalign(size_t align, size_t n)
{
    return aligned_alloc(align, n);
}
