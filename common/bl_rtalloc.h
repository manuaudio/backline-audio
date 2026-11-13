/* Heap calls on a realtime thread, counted (bl_rtalloc.c).
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_RTALLOC_H
#define BACKLINE_RTALLOC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the counts go (e.g. into shared memory); NULL: a private counter. */
void bl_rtalloc_counters(uint32_t *allocs, uint32_t *frees);
/* The calling thread counts from now on (or stops). */
void bl_rtalloc_enter(void);
void bl_rtalloc_leave(void);
uint32_t bl_rtalloc_allocs(void);
uint32_t bl_rtalloc_frees(void);

#ifdef __cplusplus
}
#endif
#endif
