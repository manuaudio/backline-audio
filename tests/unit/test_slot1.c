/* Slot1's contract (lib/include/backline/slot1.h), in one process: the
 * layout, the sealed memory, the rings, and the engine's cycle rule against a
 * sandbox thread that halves its input: wet in time, dry delayed by the
 * latency when late, stalled until caught up, and taken over by a new
 * generation.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "backline/slot1.h"
#include "bl_rtalloc.h"

static int fails;
#define OK(c, ...)                                                                                                    \
	do {                                                                                                          \
		if (!(c)) {                                                                                           \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                          \
			fprintf(stderr, __VA_ARGS__);                                                                 \
			fputc('\n', stderr);                                                                          \
			fails++;                                                                                      \
		}                                                                                                     \
	} while (0)

struct sb {
	struct bl_slot1_header *h;
	volatile int quit;
	volatile int delay_us;      /* make the next request slow */
	volatile int freeze;        /* stop serving (a hung or dead sandbox) */
	uint32_t seen;
};

static void *sandbox(void *arg)
{
	struct sb *s = arg;
	struct bl_slot1_header *h = s->h;
	bl_rtalloc_enter();
	for (;;) {
		uint32_t seq = bl_slot1_sandbox_wait(h, s->seen, &s->quit);
		if (s->quit)
			break;
		s->seen = seq;
		if (s->freeze) {
			while (s->freeze && !s->quit)
				usleep(100);
			continue; /* never answers this request */
		}
		if (s->delay_us) {
			usleep((useconds_t)s->delay_us);
			s->delay_us = 0;
		}
		struct bl_slot1_event e;
		while (bl_slot1_ring_pop(&h->in_head, &h->in_tail, bl_slot1_in_events(h), h->n_events, &e) == 0) {
			struct bl_slot1_event back = e;
			back.value *= 2;
			bl_slot1_ring_push(&h->out_head, &h->out_tail, bl_slot1_out_events(h), h->n_events, &back);
		}
		for (uint32_t c = 0; c < h->channels; c++) {
			float *a = bl_slot1_audio(h, c);
			for (uint32_t i = 0; i < h->frames; i++)
				a[i] *= 0.5f;
		}
		h->served++;
		bl_slot1_sandbox_done(h, seq);
	}
	bl_rtalloc_leave();
	return NULL;
}

static float x(int64_t n) { return (float)((n * 31) % 97) / 97.0f + 0.01f; }

int main(void)
{
	/* layout */
	OK(sizeof(struct bl_slot1_header) == 4096, "header size");
	OK(offsetof(struct bl_slot1_header, req) % 64 == 0 && offsetof(struct bl_slot1_header, done) % 64 == 0, "futex lines");
	OK(offsetof(struct bl_slot1_header, in_head) == 128 && offsetof(struct bl_slot1_header, in_tail) == 192, "in ring lines");
	OK(offsetof(struct bl_slot1_header, out_head) == 320 && offsetof(struct bl_slot1_header, out_tail) == 384, "out ring lines");
	OK(sizeof(struct bl_slot1_event) == 16, "event size");

	struct bl_slot1_engine e;
	OK(bl_slot1_engine_create(&e, 0, 64, 48000, 0) == -EINVAL, "0 channels refused");
	OK(bl_slot1_engine_create(&e, 2, 9000, 48000, 0) == -EINVAL, "frames above 8192 refused");
	OK(bl_slot1_engine_create(&e, 2, 1024, 48000, 512) == 0, "create");
	int seals = fcntl(e.fd, F_GET_SEALS);
	OK((seals & (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) == (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL), "sealed: %x", seals);
	OK(ftruncate(e.fd, 4096) < 0, "the memory can't be shrunk");
	OK(e.h->total_bytes == bl_slot1_total_bytes(2, 1024, BL_SLOT1_DEFAULT_EVENTS), "total bytes");
	OK(bl_slot1_check(e.h, e.size) == NULL, "own header checks");

	/* the sandbox's view, through the path */
	char path[64];
	bl_slot1_engine_path(&e, path, sizeof(path));
	size_t sz = 0;
	const char *why = NULL;
	struct bl_slot1_header *h = bl_slot1_map(path, -1, &sz, &why);
	OK(h != NULL, "map %s: %s", path, why ? why : "");
	OK(sz == e.size, "mapped size");
	{
		struct bl_slot1_header bad = *e.h;
		bad.magic = 0;
		OK(bl_slot1_check(&bad, sizeof(bad)) != NULL, "bad magic refused");
		bad = *e.h;
		bad.version = 2;
		OK(bl_slot1_check(&bad, e.size) != NULL, "another version refused");
		bad = *e.h;
		bad.n_events = 300;
		OK(bl_slot1_check(&bad, e.size) != NULL, "ring capacity not a power of two refused");
		bad = *e.h;
		bad.channels = 3;
		OK(bl_slot1_check(&bad, e.size) != NULL, "a layout that doesn't add up refused");
	}

	/* rings wrap and refuse when full */
	{
		int pushed = 0;
		struct bl_slot1_event ev = { .slot = 1, .type = BL_SLOT1_EV_PARAM, .id = 7 };
		while (bl_slot1_engine_push(&e, &ev) == 0)
			pushed++, ev.value = pushed;
		OK(pushed == (int)BL_SLOT1_DEFAULT_EVENTS, "in ring holds %d, not %u", pushed, BL_SLOT1_DEFAULT_EVENTS);
		struct bl_slot1_event got;
		int popped = 0;
		while (bl_slot1_ring_pop(&h->in_head, &h->in_tail, bl_slot1_in_events(h), h->n_events, &got) == 0) {
			OK(got.value == popped, "ring order");
			popped++;
		}
		OK(popped == pushed, "popped all");
	}

	/* a sandbox: ready, generation 1 */
	struct sb s = { .h = h };
	s.seen = bl_slot1_load(&h->req);
	pthread_t th;
	pthread_create(&th, NULL, sandbox, &s);
	h->pid = (uint32_t)getpid();
	bl_slot1_store(&h->latency, 5u);
	bl_slot1_store(&h->generation, 1u);
	bl_slot1_store(&h->state, (uint32_t)BL_SLOT1_READY);

	enum { N = 64 };
	float in0[N], in1[N], o0[N], o1[N];
	float *in[2] = { in0, in1 }, *out[2] = { o0, o1 };
	int64_t n0 = 0;
	uint32_t L = 5;
	bl_rtalloc_counters(NULL, NULL);
	bl_rtalloc_enter();
	/* events both ways */
	struct bl_slot1_event pe = { .slot = 0, .type = BL_SLOT1_EV_PARAM_NORM, .id = 3, .value = 0.25 };
	bl_slot1_engine_push(&e, &pe);
	int wet = 0, late = 0, stalled = 0, notready = 0;
	float worst = 0;
	for (int k = 0; k < 60; k++, n0 += N) {
		for (int i = 0; i < N; i++)
			in0[i] = in1[i] = x(n0 + i);
		if (k == 20)
			s.delay_us = 20000;   /* one slow request: a miss, then stalled until it catches up */
		if (k == 40)
			s.freeze = 1;         /* stops answering: like a dead sandbox */
		uint64_t dl = bl_slot1_now_ns() + 2000000; /* 2 ms */
		enum bl_slot1_result r = bl_slot1_engine_process(&e, (const float *const *)in, out, N, dl, NULL);
		for (int i = 0; i < N; i++) {
			int64_t n = n0 + i;
			float dry = n - (int64_t)L >= 0 ? x(n - L) : 0.f;
			float want = r == BL_SLOT1_WET ? 0.5f * x(n) : dry;
			float err = fabsf(o0[i] - want) + fabsf(o1[i] - want);
			if (err > worst)
				worst = err;
		}
		if (r == BL_SLOT1_WET) wet++;
		else if (r == BL_SLOT1_LATE) late++;
		else if (r == BL_SLOT1_STALLED) stalled++;
		else notready++;
		if (k == 20)
			OK(r == BL_SLOT1_LATE, "the slow request is late (%d)", r);
		if (k == 50) {
			/* a new sandbox takes the memory over: done catches up to req and
			 * the generation changes */
			s.freeze = 0;
			s.quit = 1;
			bl_slot1_futex_wake(&h->req);
			bl_rtalloc_leave();
			pthread_join(th, NULL);
			s.quit = 0;
			s.seen = bl_slot1_load(&h->req);
			bl_slot1_store(&h->done, s.seen);
			pthread_create(&th, NULL, sandbox, &s);
			bl_slot1_store(&h->generation, 2u);
			bl_rtalloc_enter();
		}
		if (k == 55)
			OK(r == BL_SLOT1_WET, "the new generation serves (%d)", r);
		usleep(1000);
	}
	bl_rtalloc_leave();
	OK(worst < 1e-6f, "an output block was wrong by %g", worst);
	OK(late == 2, "two misses (slow, frozen): %d", late);
	OK(stalled >= 10, "stalled blocks while frozen: %d", stalled);
	OK(notready == 0, "not-ready blocks: %d", notready);
	OK(e.h->e_recoveries == 2, "two recoveries: %llu", (unsigned long long)e.h->e_recoveries);
	OK(bl_rtalloc_allocs() == 0 && bl_rtalloc_frees() == 0, "heap calls in the engine's cycle: %u/%u", bl_rtalloc_allocs(),
	   bl_rtalloc_frees());
	struct bl_slot1_event back;
	OK(bl_slot1_engine_pop(&e, &back) == 0 && back.value == 0.5 && back.id == 3, "an event came back");

	/* not ready: a sandbox that left */
	bl_slot1_store(&h->state, (uint32_t)BL_SLOT1_EXITED);
	for (int i = 0; i < N; i++)
		in0[i] = in1[i] = x(n0 + i);
	OK(bl_slot1_engine_process(&e, (const float *const *)in, out, N, bl_slot1_now_ns() + 1000000, NULL) == BL_SLOT1_NOT_READY,
	   "not ready after exit");
	OK(fabsf(o0[0] - x(n0 - L)) < 1e-7f, "not-ready plays delayed dry");

	s.quit = 1;
	bl_slot1_futex_wake(&h->req);
	pthread_join(th, NULL);
	printf("slot1: wet %d, late %d, stalled %d, recoveries %llu, worst error %g\n", wet, late, stalled,
	       (unsigned long long)e.h->e_recoveries, worst);
	munmap(h, sz);
	bl_slot1_engine_destroy(&e);
	if (fails)
		fprintf(stderr, "%d failures\n", fails);
	return fails ? 1 : 0;
}
