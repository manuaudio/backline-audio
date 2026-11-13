/* backline-test-slotfx.clap: the slot test effect (slotfx.h) as CLAP, with
 * the params and latency extensions.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <clap/clap.h>

#include "slotfx.h"

typedef struct {
	clap_plugin_t plugin;
	struct slotfx fx;
	uint32_t latency;
} plug_t;

static const char *features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, NULL };
#define DESC(var, id, name, lat)                                                                                      \
	static const clap_plugin_descriptor_t var = { CLAP_VERSION_INIT, id, name, "Backline",                       \
		"https://github.com/manuaudio/backline-audio", "", "", "0.1.0",                                        \
		"Gain with a fixed reported latency, and switches that misbehave (a Backline test plugin)", features };
DESC(desc0, "backline.test.slotfx", "Backline Test Slot FX", 0)
DESC(desc37, "backline.test.slotfx-lat37", "Backline Test Slot FX (37 frames)", 37)
DESC(desc300, "backline.test.slotfx-lat300", "Backline Test Slot FX (300 frames)", 300)
static const clap_plugin_descriptor_t *descs[] = { &desc0, &desc37, &desc300 };
static const uint32_t lats[] = { 0, 37, 300 };

static uint32_t ap_count(const clap_plugin_t *p, bool in) { (void)p; (void)in; return 1; }
static bool ap_get(const clap_plugin_t *p, uint32_t i, bool in, clap_audio_port_info_t *info)
{
	(void)p;
	if (i)
		return false;
	memset(info, 0, sizeof(*info));
	info->id = in ? 1 : 2;
	snprintf(info->name, sizeof(info->name), in ? "in" : "out");
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = in ? 2 : 1;
	return true;
}
static const clap_plugin_audio_ports_t ext_audio_ports = { ap_count, ap_get };

static uint32_t lat_get(const clap_plugin_t *p) { return ((plug_t *)p->plugin_data)->latency; }
static const clap_plugin_latency_t ext_latency = { lat_get };

static uint32_t pr_count(const clap_plugin_t *p) { (void)p; return SLOTFX_N_PARAMS; }
static bool pr_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info)
{
	(void)p;
	if (i >= SLOTFX_N_PARAMS)
		return false;
	memset(info, 0, sizeof(*info));
	info->id = i;
	info->flags = CLAP_PARAM_IS_AUTOMATABLE | (i ? CLAP_PARAM_IS_STEPPED : 0);
	snprintf(info->name, sizeof(info->name), "%s", slotfx_param_name(i));
	slotfx_param_range(i, &info->min_value, &info->max_value, &info->default_value);
	if (i == 2)
		info->flags &= ~CLAP_PARAM_IS_STEPPED;
	return true;
}
static bool pr_value(const clap_plugin_t *p, clap_id id, double *v)
{
	double *x = slotfx_param(&((plug_t *)p->plugin_data)->fx, id);
	if (!x)
		return false;
	*v = *x;
	return true;
}
static bool pr_to_text(const clap_plugin_t *p, clap_id id, double v, char *out, uint32_t n)
{
	(void)p; (void)id;
	snprintf(out, n, "%g", v);
	return true;
}
static bool pr_from_text(const clap_plugin_t *p, clap_id id, const char *t, double *v)
{
	(void)p; (void)id;
	*v = atof(t);
	return true;
}
static void apply_events(plug_t *s, const clap_input_events_t *in)
{
	uint32_t n = in ? in->size(in) : 0;
	for (uint32_t i = 0; i < n; i++) {
		const clap_event_header_t *h = in->get(in, i);
		if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
			continue;
		const clap_event_param_value_t *pv = (const clap_event_param_value_t *)h;
		double *x = slotfx_param(&s->fx, pv->param_id);
		if (x)
			*x = pv->value;
	}
}
static void pr_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out)
{
	(void)out;
	apply_events(p->plugin_data, in);
}
static const clap_plugin_params_t ext_params = { pr_count, pr_info, pr_value, pr_to_text, pr_from_text, pr_flush };

/* state: the gain only (the misbehaving switches never persist) */
static bool st_save(const clap_plugin_t *p, const clap_ostream_t *os)
{
	double g = ((plug_t *)p->plugin_data)->fx.gain_db;
	return os->write(os, &g, sizeof(g)) == sizeof(g);
}
static bool st_load(const clap_plugin_t *p, const clap_istream_t *is)
{
	double g;
	if (is->read(is, &g, sizeof(g)) != sizeof(g))
		return false;
	((plug_t *)p->plugin_data)->fx.gain_db = g;
	return true;
}
static const clap_plugin_state_t ext_state = { st_save, st_load };

static bool p_init(const clap_plugin_t *p) { (void)p; return true; }
static void p_destroy(const clap_plugin_t *p) { free(p->plugin_data); }
static bool p_activate(const clap_plugin_t *p, double rate, uint32_t a, uint32_t b)
{
	(void)rate; (void)a; (void)b;
	slotfx_reset(&((plug_t *)p->plugin_data)->fx);
	return true;
}
static void p_deactivate(const clap_plugin_t *p) { (void)p; }
static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static void p_stop(const clap_plugin_t *p) { (void)p; }
static void p_reset(const clap_plugin_t *p) { slotfx_reset(&((plug_t *)p->plugin_data)->fx); }
static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *pr)
{
	plug_t *s = p->plugin_data;
	apply_events(s, pr->in_events);
	if (pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1)
		return CLAP_PROCESS_ERROR;
	const clap_audio_buffer_t *i = &pr->audio_inputs[0], *o = &pr->audio_outputs[0];
	slotfx_run(&s->fx, i->channel_count > 0 ? i->data32[0] : NULL, i->channel_count > 1 ? i->data32[1] : NULL,
		   o->channel_count > 0 ? o->data32[0] : NULL, o->channel_count > 1 ? o->data32[1] : NULL, pr->frames_count);
	return CLAP_PROCESS_CONTINUE;
}
static const void *p_get_extension(const clap_plugin_t *p, const char *id)
{
	(void)p;
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ext_audio_ports;
	if (!strcmp(id, CLAP_EXT_LATENCY)) return &ext_latency;
	if (!strcmp(id, CLAP_EXT_PARAMS)) return &ext_params;
	if (!strcmp(id, CLAP_EXT_STATE)) return &ext_state;
	return NULL;
}
static void p_on_main(const clap_plugin_t *p) { (void)p; }

static const clap_plugin_t *create(const clap_plugin_factory_t *f, const clap_host_t *h, const char *id)
{
	(void)f; (void)h;
	for (uint32_t k = 0; k < 3; k++) {
		if (strcmp(id, descs[k]->id))
			continue;
		plug_t *s = calloc(1, sizeof(*s));
		s->latency = lats[k];
		slotfx_init(&s->fx, lats[k]);
		s->plugin = (clap_plugin_t){ descs[k], s, p_init, p_destroy, p_activate, p_deactivate, p_start, p_stop,
			p_reset, p_process, p_get_extension, p_on_main };
		return &s->plugin;
	}
	return NULL;
}
static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return 3; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i < 3 ? descs[i] : NULL; }
static const clap_plugin_factory_t factory = { f_count, f_desc, create };
static bool e_init(const char *path) { (void)path; return true; }
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? NULL : &factory; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, e_init, e_deinit, e_factory };
