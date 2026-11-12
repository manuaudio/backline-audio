/* backline-test-beatclick.lv2: the beat-click test plugin (beatclick.h) as
 * LV2. It follows time:Position objects on its control port the way LV2
 * tempo-synced plugins do: each object sets bar, barBeat, beatsPerBar,
 * beatsPerMinute and speed; between objects the plugin advances by itself.
 * Beats are LV2's: of time:beatUnit.
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include "beatclick.h"

#define BC_URI "https://github.com/manuaudio/backline-audio/test/beatclick"

typedef struct {
	const LV2_Atom_Sequence *control;
	float *click, *beat;
	double rate;
	LV2_URID u_object, u_blank, u_position, u_bar, u_barbeat, u_bpb, u_bpm, u_speed, u_long, u_float, u_double, u_int;
	/* the transport as last told */
	bool rolling, known;
	double beat_now, bpm;  /* beat at the next sample */
	struct beatclick bc;
} plug_t;

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f)
{
	(void)d; (void)path;
	LV2_URID_Map *map = NULL;
	for (int i = 0; f[i]; i++)
		if (!strcmp(f[i]->URI, LV2_URID__map))
			map = f[i]->data;
	if (!map)
		return NULL;
	plug_t *s = calloc(1, sizeof(*s));
	s->rate = rate;
	s->u_object = map->map(map->handle, LV2_ATOM__Object);
	s->u_blank = map->map(map->handle, LV2_ATOM__Blank);
	s->u_position = map->map(map->handle, LV2_TIME__Position);
	s->u_bar = map->map(map->handle, LV2_TIME__bar);
	s->u_barbeat = map->map(map->handle, LV2_TIME__barBeat);
	s->u_bpb = map->map(map->handle, LV2_TIME__beatsPerBar);
	s->u_bpm = map->map(map->handle, LV2_TIME__beatsPerMinute);
	s->u_speed = map->map(map->handle, LV2_TIME__speed);
	s->u_long = map->map(map->handle, LV2_ATOM__Long);
	s->u_float = map->map(map->handle, LV2_ATOM__Float);
	s->u_double = map->map(map->handle, LV2_ATOM__Double);
	s->u_int = map->map(map->handle, LV2_ATOM__Int);
	s->bpm = 120;
	return s;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
	plug_t *s = h;
	if (port == 0) s->control = data;
	else if (port == 1) s->click = data;
	else if (port == 2) s->beat = data;
}

static double num(plug_t *s, const LV2_Atom *a)
{
	if (a->type == s->u_float) return ((const LV2_Atom_Float *)a)->body;
	if (a->type == s->u_double) return ((const LV2_Atom_Double *)a)->body;
	if (a->type == s->u_long) return (double)((const LV2_Atom_Long *)a)->body;
	if (a->type == s->u_int) return ((const LV2_Atom_Int *)a)->body;
	return 0;
}

static void on_position(plug_t *s, const LV2_Atom_Object *o)
{
	const LV2_Atom *bar = NULL, *barbeat = NULL, *bpb = NULL, *bpm = NULL, *speed = NULL;
	lv2_atom_object_get(o, s->u_bar, &bar, s->u_barbeat, &barbeat, s->u_bpb, &bpb, s->u_bpm, &bpm,
			    s->u_speed, &speed, 0);
	if (speed) s->rolling = num(s, speed) > 0;
	if (bpm) s->bpm = num(s, bpm);
	if (bar && barbeat && bpb) {
		s->beat_now = num(s, bar) * num(s, bpb) + num(s, barbeat);
		s->known = true;
	}
}

/* run a stretch of samples on the current transport */
static void run_span(plug_t *s, uint32_t from, uint32_t to)
{
	if (to <= from) return;
	double step = s->bpm / 60.0 / s->rate;
	beatclick_run(&s->bc, s->rolling && s->known, s->beat_now, step, s->click + from, s->beat + from, to - from);
	if (s->rolling)
		s->beat_now += step * (to - from);
}

static void run(LV2_Handle h, uint32_t n)
{
	plug_t *s = h;
	uint32_t at = 0;
	LV2_ATOM_SEQUENCE_FOREACH(s->control, ev) {
		const LV2_Atom_Object *o = (const LV2_Atom_Object *)&ev->body;
		if ((o->atom.type == s->u_object || o->atom.type == s->u_blank) && o->body.otype == s->u_position) {
			uint32_t t = (uint32_t)ev->time.frames;
			if (t > n) t = n;
			run_span(s, at, t);
			at = t;
			on_position(s, o);
		}
	}
	run_span(s, at, n);
}

static void activate(LV2_Handle h) { plug_t *s = h; beatclick_reset(&s->bc); s->known = false; s->rolling = false; }
static void cleanup(LV2_Handle h) { free(h); }

static const LV2_Descriptor desc = { BC_URI, instantiate, connect_port, activate, run, NULL, cleanup, NULL };
LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t i) { return i ? NULL : &desc; }
