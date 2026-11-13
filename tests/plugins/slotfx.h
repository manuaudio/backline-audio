/* The core of the Backline slot test effect (CLAP, VST3 and LV2): a stereo
 * effect with a fixed, reported latency and switches that misbehave on
 * purpose, for tests/slot-proof.
 * SPDX-License-Identifier: MIT
 *
 *   out = gain × in, delayed by exactly `latency` frames (what it reports)
 *
 * Parameters (the same ids in every format; LV2 port indices differ):
 *   0 gain      dB −60..+24, default 0
 *   1 crash     0/1: the next process() dereferences NULL (SIGSEGV)
 *   2 sleep_us  0..200000: process() busy-waits this long (a deadline miss)
 *   3 alloc     0/1: process() mallocs and frees (an RT violation, counted)
 *
 * Variants (ids): backline.test.slotfx (latency 0), backline.test.slotfx-lat37
 * (37 frames), backline.test.slotfx-lat300 (300 frames).
 */
#ifndef BACKLINE_SLOTFX_H
#define BACKLINE_SLOTFX_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOTFX_MAX_LATENCY 4096
#define SLOTFX_N_PARAMS 4

struct slotfx {
	uint32_t latency;
	double gain_db, crash, sleep_us, alloc;
	float dly[2][SLOTFX_MAX_LATENCY + 1];
	uint32_t pos;
};

static inline void slotfx_init(struct slotfx *s, uint32_t latency)
{
	memset(s, 0, sizeof(*s));
	s->latency = latency > SLOTFX_MAX_LATENCY ? SLOTFX_MAX_LATENCY : latency;
}

static inline void slotfx_reset(struct slotfx *s)
{
	memset(s->dly, 0, sizeof(s->dly));
	s->pos = 0;
}

static inline const char *slotfx_param_name(uint32_t i)
{
	static const char *n[] = { "gain", "crash", "sleep_us", "alloc" };
	return i < SLOTFX_N_PARAMS ? n[i] : "";
}

static inline void slotfx_param_range(uint32_t i, double *mn, double *mx, double *def)
{
	static const double r[][3] = { { -60, 24, 0 }, { 0, 1, 0 }, { 0, 200000, 0 }, { 0, 1, 0 } };
	*mn = r[i][0];
	*mx = r[i][1];
	*def = r[i][2];
}

static inline double *slotfx_param(struct slotfx *s, uint32_t i)
{
	switch (i) {
	case 0: return &s->gain_db;
	case 1: return &s->crash;
	case 2: return &s->sleep_us;
	case 3: return &s->alloc;
	}
	return NULL;
}

static inline void slotfx_run(struct slotfx *s, const float *in0, const float *in1, float *out0, float *out1, uint32_t n)
{
	if (s->crash > 0.5) {
		volatile int *nil = NULL;
		*nil = 1;
	}
	if (s->alloc > 0.5) {
		void *volatile p = malloc(64);
		free(p);
	}
	if (s->sleep_us > 0) {
		struct timespec t0, t;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		do
			clock_gettime(CLOCK_MONOTONIC, &t);
		while ((t.tv_sec - t0.tv_sec) * 1e6 + (t.tv_nsec - t0.tv_nsec) / 1e3 < s->sleep_us);
	}
	float g = (float)pow(10.0, s->gain_db / 20.0);
	const float *in[2] = { in0, in1 };
	float *out[2] = { out0, out1 };
	uint32_t L = s->latency, len = L + 1;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t w = (s->pos + i) % len, r = (s->pos + i + 1) % len; /* r: written L frames ago */
		for (int c = 0; c < 2; c++) {
			float x = in[c] ? in[c][i] : 0.f;
			s->dly[c][w] = x;
			float y = L ? s->dly[c][r] : x;
			if (out[c])
				out[c][i] = g * y;
		}
	}
	s->pos = (s->pos + n) % len;
}

#endif
