/* The core of the Backline beat-click test plugins (CLAP and LV2): a plugin
 * that follows the host's transport the way tempo-synced plugins do, and
 * shows exactly what it was told.
 * SPDX-License-Identifier: MIT
 *
 * Output 1: a unit impulse on the first sample at or after every beat the
 *           host's transport crosses (a beat exactly on a cycle's first
 *           sample after a start or locate counts).
 * Output 2: the beat position at every sample (0 while stopped).
 *
 * Per cycle the wrapper gives the beat at the cycle's first sample and the
 * beats per sample; the core keeps the last sample's beat to find crossings,
 * and treats anything that isn't the continuation (a locate, a tempo
 * change's re-anchor is continuous) as a fresh start.
 */
#ifndef BACKLINE_BEATCLICK_H
#define BACKLINE_BEATCLICK_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

struct beatclick {
	bool have_last;
	double last_beat;    /* beat of the previous output sample */
	double last_step;
};

static inline void beatclick_reset(struct beatclick *c) { c->have_last = false; }

static inline void beatclick_run(struct beatclick *c, bool rolling, double beat0, double step,
				 float *click, float *beat_out, uint32_t n)
{
	if (!rolling) {
		c->have_last = false;
		for (uint32_t i = 0; i < n; i++)
			click[i] = beat_out[i] = 0.0f;
		return;
	}
	/* a continuation lands on last + step (within float noise of the host's
	 * own arithmetic, 1e-6 beat = 0.024 samples at 120 BPM / 48 kHz) */
	if (c->have_last && fabs(beat0 - (c->last_beat + c->last_step)) > 1e-6)
		c->have_last = false;
	for (uint32_t i = 0; i < n; i++) {
		double b = beat0 + (double)i * step;
		bool hit;
		if (c->have_last)
			hit = floor(b + 1e-9) > floor(c->last_beat + 1e-9);
		else
			hit = fabs(b - floor(b + 1e-9)) < 1e-9;
		click[i] = hit ? 1.0f : 0.0f;
		beat_out[i] = (float)b;
		c->have_last = true;
		c->last_beat = b;
		c->last_step = step;
	}
}

#endif
