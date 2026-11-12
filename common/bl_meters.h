/* Backline: the Params1 meter table (docs/api/Params1.md, "Meters"). A
 * memfd the program writes and controld and surfaced map read-only; a
 * seqlock keeps readers from seeing half a write. Header-only.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_METERS_H
#define BACKLINE_METERS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define BL_METERS_MAGIC "BLMT"
#define BL_METERS_HDR 64u
#define BL_METERS_STRIDE 8u
#define BL_METER_CLIP 1u
#define BL_METER_SIGNAL 2u

struct bl_meters_hdr {
	char magic[4];
	uint32_t version;
	uint32_t count;
	uint32_t stride;
	_Atomic uint64_t seq;
	uint64_t updated_ns;
	uint8_t reserved[32];
};

struct bl_meter {
	float db;
	uint32_t flags;
};

static inline size_t bl_meters_size(uint32_t count)
{
	return BL_METERS_HDR + (size_t)count * BL_METERS_STRIDE;
}

/* The program's side: a sealed memfd of count meters, mapped read-write. */
static inline int bl_meters_create(uint32_t count, struct bl_meters_hdr **map)
{
	int fd = memfd_create("backline-meters", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0)
		return -errno;
	size_t len = bl_meters_size(count);
	if (ftruncate(fd, (off_t)len) < 0 || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		int e = errno;
		close(fd);
		return -e;
	}
	struct bl_meters_hdr *h = (struct bl_meters_hdr *)m;
	memcpy(h->magic, BL_METERS_MAGIC, 4);
	h->version = 1;
	h->count = count;
	h->stride = BL_METERS_STRIDE;
	struct bl_meter *e = (struct bl_meter *)((uint8_t *)m + BL_METERS_HDR);
	for (uint32_t i = 0; i < count; i++) {
		e[i].db = -INFINITY;
		e[i].flags = 0;
	}
	*map = h;
	return fd;
}

static inline void bl_meters_write_begin(struct bl_meters_hdr *h)
{
	atomic_fetch_add_explicit(&h->seq, 1, memory_order_acq_rel);
}

static inline void bl_meters_write_end(struct bl_meters_hdr *h, uint64_t now_ns)
{
	h->updated_ns = now_ns;
	atomic_fetch_add_explicit(&h->seq, 1, memory_order_release);
}

static inline struct bl_meter *bl_meters_entries(struct bl_meters_hdr *h)
{
	return (struct bl_meter *)((uint8_t *)h + BL_METERS_HDR);
}

/* A reader's side: checks an fd from Params1 (a sealed memfd big enough for
 * count meters) and maps it read-only. NULL on any problem. */
static inline const struct bl_meters_hdr *bl_meters_map(int fd, uint32_t count, size_t *len_out)
{
	int seals = fcntl(fd, F_GET_SEALS);
	if (seals < 0 || !(seals & F_SEAL_SHRINK))
		return NULL;
	struct stat st;
	if (fstat(fd, &st) < 0 || (size_t)st.st_size < bl_meters_size(count))
		return NULL;
	size_t len = bl_meters_size(count);
	void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED)
		return NULL;
	const struct bl_meters_hdr *h = (const struct bl_meters_hdr *)m;
	if (memcmp(h->magic, BL_METERS_MAGIC, 4) || h->version != 1 || h->stride != BL_METERS_STRIDE || h->count < count) {
		munmap(m, len);
		return NULL;
	}
	*len_out = len;
	return h;
}

/* Copies count meters out consistently; false if the writer kept writing. */
static inline bool bl_meters_read(const struct bl_meters_hdr *h, uint32_t count, struct bl_meter *out)
{
	const struct bl_meter *e = (const struct bl_meter *)((const uint8_t *)h + BL_METERS_HDR);
	for (int tries = 0; tries < 8; tries++) {
		uint64_t a = atomic_load_explicit((_Atomic uint64_t *)&h->seq, memory_order_acquire);
		if (a & 1)
			continue;
		memcpy(out, e, (size_t)count * sizeof(*out));
		atomic_thread_fence(memory_order_acquire);
		uint64_t b = atomic_load_explicit((_Atomic uint64_t *)&h->seq, memory_order_relaxed);
		if (a == b)
			return true;
	}
	return false;
}

#endif
