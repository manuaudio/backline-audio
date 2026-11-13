/* Counts heap calls made on a realtime thread: link this file into a program
 * and it wraps glibc's malloc family (forwarding to glibc's own __libc_*
 * entry points, so the allocator doesn't change). A thread that calls
 * bl_rtalloc_enter() counts from then on; every other thread costs one
 * thread-local test per call. Plugins loaded with dlopen() resolve malloc to
 * these wrappers too, so a plugin that allocates in process() is counted.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bl_rtalloc.h"

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);
extern void *__libc_valloc(size_t);
extern void *__libc_pvalloc(size_t);
extern void __libc_free(void *);

static __thread int t_rt __attribute__((tls_model("initial-exec")));
static uint32_t *g_allocs, *g_frees;
static uint32_t g_allocs_local, g_frees_local;

void bl_rtalloc_counters(uint32_t *allocs, uint32_t *frees)
{
	__atomic_store_n(&g_allocs, allocs ? allocs : &g_allocs_local, __ATOMIC_RELEASE);
	__atomic_store_n(&g_frees, frees ? frees : &g_frees_local, __ATOMIC_RELEASE);
}

void bl_rtalloc_enter(void) { t_rt = 1; }
void bl_rtalloc_leave(void) { t_rt = 0; }

uint32_t bl_rtalloc_allocs(void)
{
	uint32_t *p = __atomic_load_n(&g_allocs, __ATOMIC_ACQUIRE);
	return p ? __atomic_load_n(p, __ATOMIC_RELAXED) : __atomic_load_n(&g_allocs_local, __ATOMIC_RELAXED);
}

uint32_t bl_rtalloc_frees(void)
{
	uint32_t *p = __atomic_load_n(&g_frees, __ATOMIC_ACQUIRE);
	return p ? __atomic_load_n(p, __ATOMIC_RELAXED) : __atomic_load_n(&g_frees_local, __ATOMIC_RELAXED);
}

static inline void count_alloc(void)
{
	if (__builtin_expect(t_rt, 0)) {
		uint32_t *p = __atomic_load_n(&g_allocs, __ATOMIC_RELAXED);
		__atomic_fetch_add(p ? p : &g_allocs_local, 1, __ATOMIC_RELAXED);
	}
}

static inline void count_free(void *ptr)
{
	if (__builtin_expect(t_rt, 0) && ptr) {
		uint32_t *p = __atomic_load_n(&g_frees, __ATOMIC_RELAXED);
		__atomic_fetch_add(p ? p : &g_frees_local, 1, __ATOMIC_RELAXED);
	}
}

void *malloc(size_t n)
{
	count_alloc();
	return __libc_malloc(n);
}

void *calloc(size_t a, size_t b)
{
	count_alloc();
	return __libc_calloc(a, b);
}

void *realloc(void *p, size_t n)
{
	count_alloc();
	return __libc_realloc(p, n);
}

void free(void *p)
{
	count_free(p);
	__libc_free(p);
}

void *memalign(size_t al, size_t n)
{
	count_alloc();
	return __libc_memalign(al, n);
}

void *aligned_alloc(size_t al, size_t n)
{
	count_alloc();
	return __libc_memalign(al, n);
}

int posix_memalign(void **out, size_t al, size_t n)
{
	if (al < sizeof(void *) || (al & (al - 1)))
		return EINVAL;
	count_alloc();
	void *p = __libc_memalign(al, n);
	if (!p && n)
		return ENOMEM;
	*out = p;
	return 0;
}

void *valloc(size_t n)
{
	count_alloc();
	return __libc_valloc(n);
}

void *pvalloc(size_t n)
{
	count_alloc();
	return __libc_pvalloc(n);
}
