/* backline-test-beatclick.clap: the beat-click test plugin (beatclick.h) as
 * CLAP. It reads clap_process.transport: song_pos_beats (quarter notes,
 * fixed point), tempo and IS_PLAYING.
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <clap/clap.h>

#include "beatclick.h"

typedef struct {
	clap_plugin_t plugin;
	double rate;
	struct beatclick bc;
} plug_t;

static const char *features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_UTILITY, NULL };
static const clap_plugin_descriptor_t desc = {
	CLAP_VERSION_INIT, "backline.test.beatclick", "Backline Test Beat Click", "Backline",
	"https://github.com/manuaudio/backline-audio", "", "", "0.1.0",
	"Clicks on every beat of the host transport (a Backline test plugin)", features,
};

static uint32_t ap_count(const clap_plugin_t *p, bool in) { (void)p; return in ? 0 : 1; }
static bool ap_get(const clap_plugin_t *p, uint32_t i, bool in, clap_audio_port_info_t *info)
{
	if (in || i) return false;
	memset(info, 0, sizeof(*info));
	info->id = 1;
	snprintf(info->name, sizeof(info->name), "out");
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}
static const clap_plugin_audio_ports_t ext_audio_ports = { ap_count, ap_get };

static bool p_init(const clap_plugin_t *p) { (void)p; return true; }
static void p_destroy(const clap_plugin_t *p) { free(p->plugin_data); }
static bool p_activate(const clap_plugin_t *p, double rate, uint32_t a, uint32_t b)
{
	(void)a; (void)b;
	plug_t *s = p->plugin_data;
	s->rate = rate;
	beatclick_reset(&s->bc);
	return true;
}
static void p_deactivate(const clap_plugin_t *p) { (void)p; }
static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static void p_stop(const clap_plugin_t *p) { (void)p; }
static void p_reset(const clap_plugin_t *p) { beatclick_reset(&((plug_t *)p->plugin_data)->bc); }
static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *pr)
{
	plug_t *s = p->plugin_data;
	if (pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count < 2)
		return CLAP_PROCESS_ERROR;
	float *click = pr->audio_outputs[0].data32[0], *beat = pr->audio_outputs[0].data32[1];
	const clap_event_transport_t *t = pr->transport;
	bool rolling = t && (t->flags & CLAP_TRANSPORT_IS_PLAYING) && (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) &&
		       (t->flags & CLAP_TRANSPORT_HAS_TEMPO);
	double b0 = rolling ? (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR : 0;
	double step = rolling ? t->tempo / 60.0 / s->rate : 0;
	beatclick_run(&s->bc, rolling, b0, step, click, beat, pr->frames_count);
	return CLAP_PROCESS_CONTINUE;
}
static const void *p_get_extension(const clap_plugin_t *p, const char *id)
{
	(void)p;
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ext_audio_ports;
	return NULL;
}
static void p_on_main(const clap_plugin_t *p) { (void)p; }

static const clap_plugin_t *create(const clap_plugin_factory_t *f, const clap_host_t *h, const char *id)
{
	(void)f; (void)h;
	if (strcmp(id, desc.id)) return NULL;
	plug_t *s = calloc(1, sizeof(*s));
	s->plugin = (clap_plugin_t){ &desc, s, p_init, p_destroy, p_activate, p_deactivate, p_start, p_stop, p_reset,
		p_process, p_get_extension, p_on_main };
	return &s->plugin;
}
static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i ? NULL : &desc; }
static const clap_plugin_factory_t factory = { f_count, f_desc, create };
static bool e_init(const char *path) { (void)path; return true; }
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? NULL : &factory; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, e_init, e_deinit, e_factory };
