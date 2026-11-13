/* backline-test-slotfx.lv2: the slot test effect (slotfx.h) as LV2. Three
 * URIs (latency 0, 37, 300) in one bundle; the latency is an output control
 * port designated lv2:latency.
 * Ports: 0 in_l, 1 in_r, 2 out_l, 3 out_r, 4 gain, 5 crash, 6 sleep_us,
 * 7 alloc, 8 latency (out).
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#include "slotfx.h"

#define URI0 "https://github.com/manuaudio/backline-audio/test/slotfx"
#define URI37 URI0 "-lat37"
#define URI300 URI0 "-lat300"

typedef struct {
	const float *in[2];
	float *out[2];
	const float *ctl[SLOTFX_N_PARAMS];
	float *lat;
	struct slotfx fx;
} plug_t;

static LV2_Handle inst(uint32_t lat)
{
	plug_t *s = calloc(1, sizeof(*s));
	if (s)
		slotfx_init(&s->fx, lat);
	return s;
}
static LV2_Handle inst0(const LV2_Descriptor *d, double r, const char *p, const LV2_Feature *const *f)
{
	(void)d; (void)r; (void)p; (void)f;
	return inst(0);
}
static LV2_Handle inst37(const LV2_Descriptor *d, double r, const char *p, const LV2_Feature *const *f)
{
	(void)d; (void)r; (void)p; (void)f;
	return inst(37);
}
static LV2_Handle inst300(const LV2_Descriptor *d, double r, const char *p, const LV2_Feature *const *f)
{
	(void)d; (void)r; (void)p; (void)f;
	return inst(300);
}
static void connect(LV2_Handle h, uint32_t port, void *data)
{
	plug_t *s = h;
	if (port < 2) s->in[port] = data;
	else if (port < 4) s->out[port - 2] = data;
	else if (port < 4 + SLOTFX_N_PARAMS) s->ctl[port - 4] = data;
	else if (port == 8) s->lat = data;
}
static void activate(LV2_Handle h) { slotfx_reset(&((plug_t *)h)->fx); }
static void run(LV2_Handle h, uint32_t n)
{
	plug_t *s = h;
	for (uint32_t i = 0; i < SLOTFX_N_PARAMS; i++)
		if (s->ctl[i])
			*slotfx_param(&s->fx, i) = *s->ctl[i];
	if (s->lat)
		*s->lat = (float)s->fx.latency;
	slotfx_run(&s->fx, s->in[0], s->in[1], s->out[0], s->out[1], n);
}
static void deactivate(LV2_Handle h) { (void)h; }
static void cleanup(LV2_Handle h) { free(h); }
static const void *ext(const char *uri) { (void)uri; return NULL; }

static const LV2_Descriptor descs[] = {
	{ URI0, inst0, connect, activate, run, deactivate, cleanup, ext },
	{ URI37, inst37, connect, activate, run, deactivate, cleanup, ext },
	{ URI300, inst300, connect, activate, run, deactivate, cleanup, ext },
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t i) { return i < 3 ? &descs[i] : NULL; }
