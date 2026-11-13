/* Slot1: one strip's insert chain, run in another process inside the same
 * audio cycle, over shared memory, under a deadline.
 *
 * This header is the whole contract (docs/api/Slot1.md explains it). It is
 * header-only C, usable from C and C++, MIT, with no dependency beyond libc
 * and Linux futexes, so an engine can include it without linking anything.
 *
 *   engine  (the side that owns the audio cycle; creates the memory)
 *     bl_slot1_engine_create()   memfd, sealed, mapped, header filled in
 *     bl_slot1_engine_process()  one cycle: copy in, wake, wait until the
 *                                deadline, copy out or play the dry signal
 *                                delayed by the chain's latency
 *   sandbox (the plugin process; maps the memory)
 *     bl_slot1_map()             open the path or fd, check the header
 *     bl_slot1_sandbox_wait() / bl_slot1_sandbox_done()
 *
 * Realtime: bl_slot1_engine_process(), bl_slot1_engine_push(),
 * bl_slot1_engine_pop() and the sandbox wait/done calls never allocate, lock
 * or log. Create and destroy do, and run off the audio thread.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_SLOT1_H
#define BACKLINE_SLOT1_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ constants */

#define BL_SLOT1_MAGIC 0x31534c42u   /* "BLS1" little-endian */
#define BL_SLOT1_VERSION 1u
#define BL_SLOT1_HEADER_BYTES 4096u
#define BL_SLOT1_MAX_CHANNELS 32u
#define BL_SLOT1_MAX_SLOTS 16u
#define BL_SLOT1_DEFAULT_FRAMES 1024u
#define BL_SLOT1_MAX_FRAMES 8192u
#define BL_SLOT1_DEFAULT_EVENTS 512u  /* per direction, a power of two */
#define BL_SLOT1_DEFAULT_MAX_LATENCY 16384u
#define BL_SLOT1_MEMFD_NAME "backline-slot1"

/* header.state, written by the sandbox */
enum bl_slot1_state {
	BL_SLOT1_EMPTY = 0,     /* no sandbox has mapped it yet */
	BL_SLOT1_LOADING = 1,   /* a sandbox is loading plugins */
	BL_SLOT1_READY = 2,     /* serving requests */
	BL_SLOT1_FAILED = 3,    /* a sandbox gave up (load error); see the supervisor */
	BL_SLOT1_EXITED = 4,    /* a sandbox left cleanly */
};

/* header.flags: the transport for this request (from PipeWire's position) */
#define BL_SLOT1_TR_VALID 0x1u
#define BL_SLOT1_TR_ROLLING 0x2u
#define BL_SLOT1_TR_BAR 0x4u      /* bpm, beat, bar_start and the signature are set */
#define BL_SLOT1_TR_CHANGED 0x8u  /* not the continuation of the last request */

/* bl_slot1_slot_info.flags */
#define BL_SLOT1_SLOT_LOADED 0x1u
#define BL_SLOT1_SLOT_FAILED 0x2u
#define BL_SLOT1_SLOT_INSTRUMENT 0x4u

/* event types */
enum bl_slot1_event_type {
	BL_SLOT1_EV_PARAM = 1,       /* value in the plugin's own units (CLAP value, LV2 port value, VST3 plain) */
	BL_SLOT1_EV_PARAM_NORM = 2,  /* value 0..1 over the parameter's range */
	BL_SLOT1_EV_MIDI = 3,        /* id = status | data1 << 8 | data2 << 16 */
};

/* ------------------------------------------------------------------ layout */

/* 16 bytes. Engine → sandbox (in ring) and sandbox → engine (out ring: the
 * plugin moved a parameter; type PARAM, value in the plugin's own units). */
struct bl_slot1_event {
	uint8_t slot;       /* 0-based insert in the chain */
	uint8_t type;       /* enum bl_slot1_event_type */
	uint16_t offset;    /* frame in this request (PARAM events land at 0 in v1) */
	uint32_t id;        /* parameter id (CLAP param id, VST3 ParamID, LV2 port index) or MIDI bytes */
	double value;
};

struct bl_slot1_slot_info {
	uint32_t latency;   /* frames this insert reports */
	uint32_t flags;
	char id[56];        /* plugin id, NUL-terminated (truncated) */
};

/* The first 4096 bytes of the memory. Fields are grouped by writer, one
 * writer per 64-byte line. Words named as futexes are 32-bit and naturally
 * aligned; everything else is plain data published by those words
 * (release/acquire). */
struct bl_slot1_header {
	/* 0: identity, written once by the engine before any sandbox maps it */
	uint32_t magic, version, header_bytes, total_bytes;
	uint32_t channels;           /* audio channels of the chain (the strip's width) */
	uint32_t max_frames;         /* largest request */
	uint32_t rate;               /* sample rate; a change means a new memory */
	uint32_t n_events;           /* capacity of each event ring (power of two) */
	uint32_t audio_offset;       /* byte offset of the audio: channels × max_frames float32, planar */
	uint32_t in_events_offset;   /* engine → sandbox ring */
	uint32_t out_events_offset;  /* sandbox → engine ring */
	uint32_t max_latency;        /* the engine's dry delay line holds this much */
	uint32_t _r0[4];

	/* 64: engine → sandbox, rewritten before each request */
	uint32_t req;                /* FUTEX: request sequence; the engine adds 1 per request */
	uint32_t frames;             /* frames in this request (≤ max_frames) */
	uint32_t bypass;             /* bit n: insert n bypassed (the sandbox passes its input, delayed by its latency) */
	uint32_t flags;              /* BL_SLOT1_TR_* */
	uint64_t steady;             /* frames since the engine started */
	int64_t tr_frame;            /* transport position, frames */
	double tr_bpm, tr_beat, tr_bar_start;  /* beats of the meter's own unit */
	uint64_t deadline_ns;        /* CLOCK_MONOTONIC: when the engine stops waiting */

	/* 128: engine writes (the in ring's head, the rest of the transport) */
	uint32_t in_head;
	float tr_sig_num, tr_sig_den;
	uint32_t _p1[13];
	/* 192: sandbox writes */
	uint32_t in_tail, _p2[15];

	/* 256: sandbox → engine */
	uint32_t done;               /* FUTEX: the last request served (= req when finished) */
	uint32_t state;              /* enum bl_slot1_state */
	uint32_t pid;                /* the sandbox */
	uint32_t generation;         /* sandboxes that have served this memory (a restart adds 1) */
	uint32_t latency;            /* frames, the whole chain: the sum of its inserts */
	uint32_t latency_serial;     /* +1 on every change of latency or slots[] */
	uint32_t n_slots;
	uint32_t audio_allocs;       /* allocations seen on the sandbox's audio thread (should stay 0) */
	uint64_t served;             /* requests served */
	uint64_t process_ns_last, process_ns_max;  /* the sandbox's own time per request */
	uint32_t process_errors;     /* a plugin's process returned an error */
	uint32_t audio_frees;        /* frees seen on the sandbox's audio thread */

	/* 320, 384: the out ring's indices */
	uint32_t out_head, _p3[15];  /* sandbox writes */
	uint32_t out_tail, _p4[15];  /* engine writes */

	/* 448: the engine's statistics, for anyone watching */
	uint64_t e_cycles;           /* bl_slot1_engine_process calls */
	uint64_t e_misses;           /* requests that missed the deadline */
	uint64_t e_dry;              /* blocks that played the delayed dry signal (misses + stalled + not ready) */
	uint64_t e_rtt_ns_last, e_rtt_ns_max;  /* request → done, as the engine saw it */
	uint64_t e_recoveries;       /* stalls that ended */
	uint64_t _r5[2];

	/* 512 */
	struct bl_slot1_slot_info slots[BL_SLOT1_MAX_SLOTS];

	/* 1536..4096 reserved, zero */
	uint8_t _reserved[BL_SLOT1_HEADER_BYTES - 1536];
};

#ifdef __cplusplus
static_assert(sizeof(struct bl_slot1_header) == BL_SLOT1_HEADER_BYTES, "Slot1 header size");
static_assert(offsetof(struct bl_slot1_header, req) == 64, "Slot1 req");
static_assert(offsetof(struct bl_slot1_header, done) == 256, "Slot1 done");
static_assert(offsetof(struct bl_slot1_header, e_cycles) == 448, "Slot1 stats");
static_assert(offsetof(struct bl_slot1_header, slots) == 512, "Slot1 slots");
#else
_Static_assert(sizeof(struct bl_slot1_header) == BL_SLOT1_HEADER_BYTES, "Slot1 header size");
_Static_assert(offsetof(struct bl_slot1_header, req) == 64, "Slot1 req");
_Static_assert(offsetof(struct bl_slot1_header, done) == 256, "Slot1 done");
_Static_assert(offsetof(struct bl_slot1_header, e_cycles) == 448, "Slot1 stats");
_Static_assert(offsetof(struct bl_slot1_header, slots) == 512, "Slot1 slots");
#endif

/* ------------------------------------------------------------------ atomics, futex, clock */

#define bl_slot1_load(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define bl_slot1_store(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

static inline uint64_t bl_slot1_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void bl_slot1_futex_wake(uint32_t *w)
{
	syscall(SYS_futex, w, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

/* Sleeps while *w == val, until abs_ns (CLOCK_MONOTONIC; 0 = forever).
 * Returns 0 when woken or the value differs, -ETIMEDOUT at the deadline. */
static inline int bl_slot1_futex_wait(uint32_t *w, uint32_t val, uint64_t abs_ns)
{
	struct timespec ts, *tp = NULL;
	if (abs_ns) {
		ts.tv_sec = (time_t)(abs_ns / 1000000000ull);
		ts.tv_nsec = (long)(abs_ns % 1000000000ull);
		tp = &ts;
	}
	long r = syscall(SYS_futex, w, FUTEX_WAIT_BITSET, val, tp, NULL, FUTEX_BITSET_MATCH_ANY);
	if (r == 0)
		return 0;
	return errno == ETIMEDOUT ? -ETIMEDOUT : 0; /* EAGAIN (value differs) and EINTR: look again */
}

static inline size_t bl_slot1_total_bytes(uint32_t channels, uint32_t max_frames, uint32_t n_events)
{
	size_t ev = (size_t)n_events * sizeof(struct bl_slot1_event);
	return BL_SLOT1_HEADER_BYTES + 2 * ev + (size_t)channels * max_frames * sizeof(float);
}

static inline float *bl_slot1_audio(struct bl_slot1_header *h, uint32_t channel)
{
	return (float *)((uint8_t *)h + h->audio_offset) + (size_t)channel * h->max_frames;
}

static inline struct bl_slot1_event *bl_slot1_in_events(struct bl_slot1_header *h)
{
	return (struct bl_slot1_event *)((uint8_t *)h + h->in_events_offset);
}

static inline struct bl_slot1_event *bl_slot1_out_events(struct bl_slot1_header *h)
{
	return (struct bl_slot1_event *)((uint8_t *)h + h->out_events_offset);
}

/* One-producer one-consumer ring over (head, tail, events). */
static inline int bl_slot1_ring_push(uint32_t *head, uint32_t *tail, struct bl_slot1_event *ev, uint32_t cap,
				     const struct bl_slot1_event *e)
{
	uint32_t h = __atomic_load_n(head, __ATOMIC_RELAXED);
	if (h - bl_slot1_load(tail) >= cap)
		return -1;
	ev[h & (cap - 1)] = *e;
	bl_slot1_store(head, h + 1);
	return 0;
}

static inline int bl_slot1_ring_pop(uint32_t *head, uint32_t *tail, struct bl_slot1_event *ev, uint32_t cap,
				    struct bl_slot1_event *e)
{
	uint32_t t = __atomic_load_n(tail, __ATOMIC_RELAXED);
	if (t == bl_slot1_load(head))
		return -1;
	*e = ev[t & (cap - 1)];
	bl_slot1_store(tail, t + 1);
	return 0;
}

/* Checks a mapped header (either side). Returns NULL or a reason. */
static inline const char *bl_slot1_check(const struct bl_slot1_header *h, size_t mapped)
{
	if (mapped < BL_SLOT1_HEADER_BYTES || h->magic != BL_SLOT1_MAGIC)
		return "not Slot1 memory";
	if (h->version != BL_SLOT1_VERSION)
		return "a Slot1 version this side does not speak";
	if (h->header_bytes != BL_SLOT1_HEADER_BYTES || h->total_bytes > mapped)
		return "Slot1 memory smaller than its header says";
	if (!h->channels || h->channels > BL_SLOT1_MAX_CHANNELS || !h->max_frames || h->max_frames > BL_SLOT1_MAX_FRAMES)
		return "Slot1 channels or frames out of range";
	if (!h->n_events || (h->n_events & (h->n_events - 1)))
		return "Slot1 event ring capacity is not a power of two";
	if (bl_slot1_total_bytes(h->channels, h->max_frames, h->n_events) != h->total_bytes)
		return "Slot1 layout does not add up";
	return NULL;
}

/* ------------------------------------------------------------------ engine side */

enum bl_slot1_result {
	BL_SLOT1_WET = 0,        /* the chain's output, this cycle */
	BL_SLOT1_LATE = 1,       /* missed the deadline: dry, delayed by the chain's latency */
	BL_SLOT1_STALLED = 2,    /* still waiting for a late request: dry, delayed */
	BL_SLOT1_NOT_READY = 3,  /* no sandbox serving (starting, crashed, restarting): dry, delayed */
};

struct bl_slot1_engine {
	struct bl_slot1_header *h;
	int fd;                  /* the memfd; /proc/<pid>/fd/<fd> is its path */
	size_t size;
	uint32_t seq;            /* last request sent */
	int stalled;
	uint32_t stall_generation;
	uint64_t spin_ns;        /* spin this long before sleeping on the futex (default 20 µs) */
	/* the dry delay line: channels × dly_len, so a fallback block keeps the
	 * alignment the chain's latency set */
	float *dly;
	uint32_t dly_len, dly_pos;
	uint32_t latency;        /* the latency the dry delay uses (the header's, clamped) */
	/* between begin and end */
	int pending;
	enum bl_slot1_result result;
	uint32_t frames;
	uint64_t t0, deadline_ns;
};

/* Creates the memory (memfd, sealed against resizing), maps and locks it, and
 * fills in the header. Off the audio thread. Returns 0 or -errno. */
static inline int bl_slot1_engine_create(struct bl_slot1_engine *e, uint32_t channels, uint32_t max_frames, uint32_t rate,
					 uint32_t max_latency)
{
	memset(e, 0, sizeof(*e));
	e->fd = -1;
	if (!channels || channels > BL_SLOT1_MAX_CHANNELS || !max_frames || max_frames > BL_SLOT1_MAX_FRAMES || !rate)
		return -EINVAL;
	if (!max_latency)
		max_latency = BL_SLOT1_DEFAULT_MAX_LATENCY;
	uint32_t n_events = BL_SLOT1_DEFAULT_EVENTS;
	size_t size = bl_slot1_total_bytes(channels, max_frames, n_events);
	int fd = memfd_create(BL_SLOT1_MEMFD_NAME, MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0)
		return -errno;
	if (ftruncate(fd, (off_t)size) < 0 || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) {
		int r = -errno;
		close(fd);
		return r;
	}
	void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		int r = -errno;
		close(fd);
		return r;
	}
	mlock(m, size); /* best effort: RLIMIT_MEMLOCK may refuse; the pages are touched below anyway */
	memset(m, 0, size);
	struct bl_slot1_header *h = (struct bl_slot1_header *)m;
	h->magic = BL_SLOT1_MAGIC;
	h->version = BL_SLOT1_VERSION;
	h->header_bytes = BL_SLOT1_HEADER_BYTES;
	h->total_bytes = (uint32_t)size;
	h->channels = channels;
	h->max_frames = max_frames;
	h->rate = rate;
	h->n_events = n_events;
	h->in_events_offset = BL_SLOT1_HEADER_BYTES;
	h->out_events_offset = BL_SLOT1_HEADER_BYTES + n_events * (uint32_t)sizeof(struct bl_slot1_event);
	h->audio_offset = h->out_events_offset + n_events * (uint32_t)sizeof(struct bl_slot1_event);
	h->max_latency = max_latency;
	e->dly_len = max_latency + max_frames;
	e->dly = (float *)calloc((size_t)channels * e->dly_len, sizeof(float));
	if (!e->dly) {
		munmap(m, size);
		close(fd);
		return -ENOMEM;
	}
	mlock(e->dly, (size_t)channels * e->dly_len * sizeof(float));
	e->h = h;
	e->fd = fd;
	e->size = size;
	e->spin_ns = 20000;
	return 0;
}

static inline void bl_slot1_engine_destroy(struct bl_slot1_engine *e)
{
	if (e->h)
		munmap(e->h, e->size);
	if (e->fd >= 0)
		close(e->fd);
	free(e->dly);
	memset(e, 0, sizeof(*e));
	e->fd = -1;
}

/* The path a sandbox opens: "/proc/<pid>/fd/<fd>". */
static inline void bl_slot1_engine_path(const struct bl_slot1_engine *e, char *out, size_t n)
{
	snprintf(out, n, "/proc/%d/fd/%d", (int)getpid(), e->fd);
}

/* Queues an event for the next request. Realtime-safe. -1: the ring is full. */
static inline int bl_slot1_engine_push(struct bl_slot1_engine *e, const struct bl_slot1_event *ev)
{
	struct bl_slot1_header *h = e->h;
	return bl_slot1_ring_push(&h->in_head, &h->in_tail, bl_slot1_in_events(h), h->n_events, ev);
}

/* Takes one event the plugins sent back. Realtime-safe. -1: none. */
static inline int bl_slot1_engine_pop(struct bl_slot1_engine *e, struct bl_slot1_event *ev)
{
	struct bl_slot1_header *h = e->h;
	return bl_slot1_ring_pop(&h->out_head, &h->out_tail, bl_slot1_out_events(h), h->n_events, ev);
}

/* The transport for the next request; all optional (flags 0 = none). */
struct bl_slot1_transport {
	uint32_t flags;          /* BL_SLOT1_TR_* */
	int64_t frame;
	double bpm, beat, bar_start;
	float sig_num, sig_den;
};

static inline void bl_slot1__dry(struct bl_slot1_engine *e, float *const *out, uint32_t frames)
{
	uint32_t ch = e->h->channels, len = e->dly_len, L = e->latency;
	for (uint32_t c = 0; c < ch; c++) {
		const float *d = e->dly + (size_t)c * len;
		float *o = out[c];
		uint32_t r = (e->dly_pos + len - L) % len;
		for (uint32_t i = 0; i < frames; i++) {
			o[i] = d[r];
			if (++r == len)
				r = 0;
		}
	}
}

/* One cycle, in two halves, so an engine with several chains can send every
 * request first and collect them after (one chain's wait never eats another
 * chain's deadline):
 *
 *   bl_slot1_engine_begin()  feeds the dry delay line, copies `in` into the
 *                            memory and wakes the sandbox (or decides this
 *                            block is dry: stalled or not ready)
 *   bl_slot1_engine_end()    waits for `done` until the deadline, then copies
 *                            the chain's output to `out`, or the dry input
 *                            delayed by the chain's latency
 *
 * in and out are `channels` planar buffers of `frames` (they may be the same
 * buffers: `in` is read in begin, `out` written in end). deadline_ns:
 * CLOCK_MONOTONIC time to stop waiting (typically the cycle's start + 50 % of
 * the period). Realtime-safe.
 *
 * The rule: a request that is not done by the deadline plays the dry input
 * delayed by the chain's reported latency, so alignment holds, and the chain
 * is stalled: it gets no new request (the engine never writes audio the
 * sandbox may still read) until the sandbox has caught up (done == req) or a
 * new sandbox has taken the memory over (generation changed, ready). A
 * request that is done when end() looks is wet, even past the deadline. */
static inline enum bl_slot1_result bl_slot1_engine_begin(struct bl_slot1_engine *e, const float *const *in, uint32_t frames,
							 uint64_t deadline_ns, const struct bl_slot1_transport *tr)
{
	struct bl_slot1_header *h = e->h;
	uint32_t ch = h->channels;
	if (frames > h->max_frames)
		frames = h->max_frames;
	h->e_cycles++;
	e->frames = frames;
	e->deadline_ns = deadline_ns;
	e->pending = 0;

	/* the latency the dry path follows */
	uint32_t L = bl_slot1_load(&h->latency);
	e->latency = L > h->max_latency ? h->max_latency : L;

	/* feed the dry delay line every cycle, before anything may overwrite `in` */
	uint32_t len = e->dly_len;
	for (uint32_t c = 0; c < ch; c++) {
		float *d = e->dly + (size_t)c * len;
		const float *x = in[c];
		uint32_t w = e->dly_pos;
		for (uint32_t i = 0; i < frames; i++) {
			d[w] = x[i];
			if (++w == len)
				w = 0;
		}
	}

	uint32_t state = bl_slot1_load(&h->state);
	if (e->stalled) {
		uint32_t done = bl_slot1_load(&h->done);
		uint32_t gen = bl_slot1_load(&h->generation);
		if (done == e->seq || (gen != e->stall_generation && state == BL_SLOT1_READY)) {
			e->stalled = 0;
			e->seq = bl_slot1_load(&h->req);
			h->e_recoveries++;
		}
	}
	if (state != BL_SLOT1_READY)
		return e->result = BL_SLOT1_NOT_READY;
	if (e->stalled)
		return e->result = BL_SLOT1_STALLED;

	for (uint32_t c = 0; c < ch; c++)
		memcpy(bl_slot1_audio(h, c), in[c], frames * sizeof(float));
	h->frames = frames;
	if (tr) {
		h->flags = tr->flags;
		h->tr_frame = tr->frame;
		h->tr_bpm = tr->bpm;
		h->tr_beat = tr->beat;
		h->tr_bar_start = tr->bar_start;
		h->tr_sig_num = tr->sig_num;
		h->tr_sig_den = tr->sig_den;
	} else {
		h->flags = 0;
	}
	h->deadline_ns = deadline_ns;
	e->t0 = bl_slot1_now_ns();
	e->seq++;
	bl_slot1_store(&h->req, e->seq);
	bl_slot1_futex_wake(&h->req);
	e->pending = 1;
	return e->result = BL_SLOT1_WET;
}

static inline enum bl_slot1_result bl_slot1_engine_end(struct bl_slot1_engine *e, float *const *out)
{
	struct bl_slot1_header *h = e->h;
	uint32_t ch = h->channels, frames = e->frames, len = e->dly_len;
	enum bl_slot1_result res = e->result;
	if (e->pending) {
		uint32_t seq = e->seq;
		uint64_t deadline_ns = e->deadline_ns;
		uint64_t spin_until = e->t0 + e->spin_ns;
		if (spin_until > deadline_ns)
			spin_until = deadline_ns;
		uint32_t done = bl_slot1_load(&h->done);
		while (done != seq && bl_slot1_now_ns() < spin_until)
			done = bl_slot1_load(&h->done);
		while (done != seq) {
			if (bl_slot1_futex_wait(&h->done, done, deadline_ns) == -ETIMEDOUT) {
				done = bl_slot1_load(&h->done);
				break;
			}
			done = bl_slot1_load(&h->done);
			if (done != seq && bl_slot1_now_ns() >= deadline_ns)
				break;
		}
		uint64_t rtt = bl_slot1_now_ns() - e->t0;
		e->pending = 0;
		if (done == seq) {
			h->e_rtt_ns_last = rtt;
			if (rtt > h->e_rtt_ns_max)
				h->e_rtt_ns_max = rtt;
			for (uint32_t c = 0; c < ch; c++)
				memcpy(out[c], bl_slot1_audio(h, c), frames * sizeof(float));
			h->steady += frames;
			e->dly_pos = (e->dly_pos + frames) % len;
			return BL_SLOT1_WET;
		}
		e->stalled = 1;
		e->stall_generation = bl_slot1_load(&h->generation);
		h->e_misses++;
		res = BL_SLOT1_LATE;
	}
	bl_slot1__dry(e, out, frames);
	h->steady += frames;
	h->e_dry++;
	e->dly_pos = (e->dly_pos + frames) % len;
	return res;
}

/* begin + end, for one chain. */
static inline enum bl_slot1_result bl_slot1_engine_process(struct bl_slot1_engine *e, const float *const *in,
							   float *const *out, uint32_t frames, uint64_t deadline_ns,
							   const struct bl_slot1_transport *tr)
{
	bl_slot1_engine_begin(e, in, frames, deadline_ns, tr);
	return bl_slot1_engine_end(e, out);
}

/* ------------------------------------------------------------------ sandbox side */

/* Maps Slot1 memory from a path ("/proc/<pid>/fd/<n>") or, with path NULL,
 * from fd. Returns the header or NULL with *why set. */
static inline struct bl_slot1_header *bl_slot1_map(const char *path, int fd, size_t *size_out, const char **why)
{
	int own = -1;
	if (path) {
		own = fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			*why = "cannot open the Slot1 memory";
			return NULL;
		}
	}
	struct stat st;
	if (fstat(fd, &st) < 0 || (size_t)st.st_size < BL_SLOT1_HEADER_BYTES) {
		*why = "Slot1 memory is too small";
		if (own >= 0)
			close(own);
		return NULL;
	}
	void *m = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (own >= 0)
		close(own);
	if (m == MAP_FAILED) {
		*why = "cannot map the Slot1 memory";
		return NULL;
	}
	struct bl_slot1_header *h = (struct bl_slot1_header *)m;
	const char *bad = bl_slot1_check(h, (size_t)st.st_size);
	if (bad) {
		munmap(m, (size_t)st.st_size);
		*why = bad;
		return NULL;
	}
	mlock(m, (size_t)st.st_size);
	*size_out = (size_t)st.st_size;
	return h;
}

/* Waits for a request after `seen` (blocking; quit is checked on every
 * wake-up). Returns the new request's sequence, or `seen` if *quit was set. */
static inline uint32_t bl_slot1_sandbox_wait(struct bl_slot1_header *h, uint32_t seen, const volatile int *quit)
{
	for (;;) {
		uint32_t r = bl_slot1_load(&h->req);
		if (r != seen)
			return r;
		if (quit && *quit)
			return seen;
		bl_slot1_futex_wait(&h->req, seen, 0);
	}
}

static inline void bl_slot1_sandbox_done(struct bl_slot1_header *h, uint32_t seq)
{
	bl_slot1_store(&h->done, seq);
	bl_slot1_futex_wake(&h->done);
}

#ifdef __cplusplus
}
#endif
#endif
