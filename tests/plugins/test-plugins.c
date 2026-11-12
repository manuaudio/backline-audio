/* Backline test plugins: small CLAP plugins with known behaviour.
 * SPDX-License-Identifier: MIT
 *
 * One source, built three times:
 *   BL_TEST_OK     backline-test-ok.clap: two well-behaved plugins
 *                    "backline.test.gain"  stereo gain effect, param "Gain" (dB), state
 *                    "backline.test.sine"  sine instrument (note port, CLAP + MIDI
 *                                          dialects), param "Volume" (dB), state
 *   BL_TEST_CRASH  backline-test-crash.clap: a gain effect whose activate() starts
 *                  a detached helper process (setsid), and which dereferences NULL
 *                  in its first process() call (SIGSEGV)
 *   BL_TEST_HANG   backline-test-hang.clap: activate() starts a detached child
 *                  that sleeps forever (setsid: it leaves the process group), then
 *                  spins forever itself. Only a cgroup kill cleans this up.
 */
#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <clap/clap.h>

#if !defined(BL_TEST_OK) && !defined(BL_TEST_CRASH) && !defined(BL_TEST_HANG)
#define BL_TEST_OK 1
#endif

enum { KIND_GAIN, KIND_SINE };

typedef struct {
	clap_plugin_t plugin;
	const clap_host_t *host;
	int kind;
	double rate;
	double param_db;          /* Gain (gain) or Volume (sine), in dB */
	/* sine voice */
	int key;                  /* -1 = silent */
	double phase, freq, vel, env;
} plug_t;

static const char *gain_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
	CLAP_PLUGIN_FEATURE_STEREO, NULL };
static const char *sine_features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
	CLAP_PLUGIN_FEATURE_STEREO, NULL };

#if defined(BL_TEST_OK)
#define GAIN_ID "backline.test.gain"
#define GAIN_NAME "Backline Test Gain"
#elif defined(BL_TEST_CRASH)
#define GAIN_ID "backline.test.crash"
#define GAIN_NAME "Backline Test Crash"
#else
#define GAIN_ID "backline.test.hang"
#define GAIN_NAME "Backline Test Hang"
#endif

static const clap_plugin_descriptor_t desc_gain = {
	CLAP_VERSION_INIT, GAIN_ID, GAIN_NAME, "Backline", "https://github.com/manuaudio/backline-audio",
	"", "", "0.1.0", "A test plugin for Backline", gain_features,
};
static const clap_plugin_descriptor_t desc_sine = {
	CLAP_VERSION_INIT, "backline.test.sine", "Backline Test Sine", "Backline",
	"https://github.com/manuaudio/backline-audio", "", "", "0.1.0", "A sine instrument for Backline tests",
	sine_features,
};

static double db_to_lin(double db) { return db <= -60.0 ? 0.0 : pow(10.0, db / 20.0); }

/* ------------------------------------------------------------ params */
static uint32_t params_count(const clap_plugin_t *p) { (void)p; return 1; }
static bool params_get_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info)
{
	plug_t *s = p->plugin_data;
	if (i != 0) return false;
	memset(info, 0, sizeof(*info));
	info->id = 0;
	info->flags = CLAP_PARAM_IS_AUTOMATABLE;
	snprintf(info->name, sizeof(info->name), "%s", s->kind == KIND_SINE ? "Volume" : "Gain");
	snprintf(info->module, sizeof(info->module), "%s", "");
	info->min_value = -60.0;
	info->max_value = 12.0;
	info->default_value = s->kind == KIND_SINE ? -12.0 : 0.0;
	return true;
}
static bool params_get_value(const clap_plugin_t *p, clap_id id, double *v)
{
	plug_t *s = p->plugin_data;
	if (id != 0) return false;
	*v = s->param_db;
	return true;
}
static bool params_value_to_text(const clap_plugin_t *p, clap_id id, double v, char *out, uint32_t n)
{
	(void)p;
	if (id != 0) return false;
	snprintf(out, n, "%.2f dB", v);
	return true;
}
static bool params_text_to_value(const clap_plugin_t *p, clap_id id, const char *t, double *v)
{
	(void)p;
	if (id != 0) return false;
	*v = strtod(t, NULL);
	return true;
}
static void handle_event(plug_t *s, const clap_event_header_t *h)
{
	if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
	switch (h->type) {
	case CLAP_EVENT_PARAM_VALUE: {
		const clap_event_param_value_t *e = (const void *)h;
		if (e->param_id == 0) s->param_db = fmin(12.0, fmax(-60.0, e->value));
		break;
	}
	case CLAP_EVENT_NOTE_ON: {
		const clap_event_note_t *e = (const void *)h;
		s->key = e->key; s->vel = e->velocity;
		s->freq = 440.0 * pow(2.0, (e->key - 69) / 12.0);
		break;
	}
	case CLAP_EVENT_NOTE_OFF: {
		const clap_event_note_t *e = (const void *)h;
		if (e->key == s->key || e->key < 0) s->key = -1;
		break;
	}
	case CLAP_EVENT_MIDI: {
		const clap_event_midi_t *e = (const void *)h;
		uint8_t st = e->data[0] & 0xf0;
		if (st == 0x90 && e->data[2]) {
			s->key = e->data[1]; s->vel = e->data[2] / 127.0;
			s->freq = 440.0 * pow(2.0, (s->key - 69) / 12.0);
		} else if ((st == 0x80 || (st == 0x90 && !e->data[2])) && e->data[1] == s->key) {
			s->key = -1;
		}
		break;
	}
	}
}
static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out)
{
	(void)out;
	plug_t *s = p->plugin_data;
	uint32_t n = in->size(in);
	for (uint32_t i = 0; i < n; i++)
		handle_event(s, in->get(in, i));
}
static const clap_plugin_params_t ext_params = {
	params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush,
};

/* ------------------------------------------------------------ state */
#define STATE_MAGIC 0x424c5431u /* "BLT1" */
static bool state_save(const clap_plugin_t *p, const clap_ostream_t *os)
{
	plug_t *s = p->plugin_data;
	uint32_t magic = STATE_MAGIC;
	double v = s->param_db;
	return os->write(os, &magic, 4) == 4 && os->write(os, &v, 8) == 8;
}
static bool state_load(const clap_plugin_t *p, const clap_istream_t *is)
{
	plug_t *s = p->plugin_data;
	uint32_t magic = 0;
	double v = 0;
	if (is->read(is, &magic, 4) != 4 || magic != STATE_MAGIC || is->read(is, &v, 8) != 8)
		return false;
	s->param_db = fmin(12.0, fmax(-60.0, v));
	return true;
}
static const clap_plugin_state_t ext_state = { state_save, state_load };

/* ------------------------------------------------------------ ports */
static uint32_t ap_count(const clap_plugin_t *p, bool is_input)
{
	plug_t *s = p->plugin_data;
	return s->kind == KIND_SINE ? (is_input ? 0 : 1) : 1;
}
static bool ap_get(const clap_plugin_t *p, uint32_t i, bool is_input, clap_audio_port_info_t *info)
{
	if (i >= ap_count(p, is_input)) return false;
	memset(info, 0, sizeof(*info));
	info->id = is_input ? 0 : 1;
	snprintf(info->name, sizeof(info->name), "%s", is_input ? "in" : "out");
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}
static const clap_plugin_audio_ports_t ext_audio_ports = { ap_count, ap_get };

static uint32_t np_count(const clap_plugin_t *p, bool is_input)
{
	plug_t *s = p->plugin_data;
	return s->kind == KIND_SINE && is_input ? 1 : 0;
}
static bool np_get(const clap_plugin_t *p, uint32_t i, bool is_input, clap_note_port_info_t *info)
{
	if (i >= np_count(p, is_input)) return false;
	memset(info, 0, sizeof(*info));
	info->id = 0;
	info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
	info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
	snprintf(info->name, sizeof(info->name), "notes");
	return true;
}
static const clap_plugin_note_ports_t ext_note_ports = { np_count, np_get };

/* ------------------------------------------------------------ plugin */
static bool p_init(const clap_plugin_t *p) { (void)p; return true; }
static void p_destroy(const clap_plugin_t *p) { free(p->plugin_data); }
static bool p_activate(const clap_plugin_t *p, double rate, uint32_t minf, uint32_t maxf)
{
	(void)minf; (void)maxf;
	plug_t *s = p->plugin_data;
	s->rate = rate;
	s->phase = 0; s->env = 0;
#ifdef BL_TEST_CRASH
	/* a helper process that outlives the crash, as real plugins' helpers do */
	if (fork() == 0) {
		setsid();
		for (;;) pause();
	}
#endif
#ifdef BL_TEST_HANG
	pid_t c = fork();
	if (c == 0) {
		setsid();          /* escape the process group: only the cgroup still holds it */
		for (;;) pause();
	}
	for (volatile unsigned long spin = 0;; spin = spin + 1) {}
#endif
	return true;
}
static void p_deactivate(const clap_plugin_t *p) { (void)p; }
static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static void p_stop(const clap_plugin_t *p) { (void)p; }
static void p_reset(const clap_plugin_t *p) { plug_t *s = p->plugin_data; s->key = -1; s->env = 0; }

static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *pr)
{
	plug_t *s = p->plugin_data;
#ifdef BL_TEST_CRASH
	volatile int *nowhere = NULL;
	*nowhere = 42; /* SIGSEGV, on purpose */
#endif
	uint32_t n = pr->frames_count, nev = pr->in_events->size(pr->in_events), ev = 0;
	float **out = pr->audio_outputs_count ? pr->audio_outputs[0].data32 : NULL;
	float **in = pr->audio_inputs_count ? pr->audio_inputs[0].data32 : NULL;
	for (uint32_t i = 0; i < n; i++) {
		while (ev < nev) {
			const clap_event_header_t *h = pr->in_events->get(pr->in_events, ev);
			if (h->time > i) break;
			handle_event(s, h);
			ev++;
		}
		double g = db_to_lin(s->param_db);
		if (s->kind == KIND_SINE) {
			double target = s->key >= 0 ? s->vel : 0.0;
			s->env += (target - s->env) * 0.002; /* ~10 ms smoothing at 48 kHz */
			double v = sin(s->phase) * s->env * g;
			s->phase += 2 * M_PI * s->freq / s->rate;
			if (s->phase > 2 * M_PI) s->phase -= 2 * M_PI;
			if (out) out[0][i] = out[1][i] = (float)v;
		} else if (out) {
			for (int c = 0; c < 2; c++)
				out[c][i] = in ? (float)(in[c][i] * g) : 0.0f;
		}
	}
	while (ev < nev)
		handle_event(s, pr->in_events->get(pr->in_events, ev++));
	return CLAP_PROCESS_CONTINUE;
}

static const void *p_get_extension(const clap_plugin_t *p, const char *id)
{
	plug_t *s = p->plugin_data;
	if (!strcmp(id, CLAP_EXT_PARAMS)) return &ext_params;
	if (!strcmp(id, CLAP_EXT_STATE)) return &ext_state;
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ext_audio_ports;
	if (!strcmp(id, CLAP_EXT_NOTE_PORTS) && s->kind == KIND_SINE) return &ext_note_ports;
	return NULL;
}
static void p_on_main_thread(const clap_plugin_t *p) { (void)p; }

static const clap_plugin_t *create(const clap_host_t *host, const clap_plugin_descriptor_t *d, int kind)
{
	plug_t *s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->host = host;
	s->kind = kind;
	s->key = -1;
	s->param_db = kind == KIND_SINE ? -12.0 : 0.0;
	s->freq = 440.0;
	s->rate = 48000;
	s->plugin = (clap_plugin_t){ d, s, p_init, p_destroy, p_activate, p_deactivate, p_start, p_stop,
		p_reset, p_process, p_get_extension, p_on_main_thread };
	return &s->plugin;
}

/* ------------------------------------------------------------ factory */
#ifdef BL_TEST_OK
#define N_PLUGINS 2
#else
#define N_PLUGINS 1
#endif
static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return N_PLUGINS; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i)
{
	(void)f;
	return i == 0 ? &desc_gain : (i == 1 && N_PLUGINS > 1) ? &desc_sine : NULL;
}
static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id)
{
	(void)f;
	if (!clap_version_is_compatible(host->clap_version)) return NULL;
	if (!strcmp(id, desc_gain.id)) return create(host, &desc_gain, KIND_GAIN);
	if (N_PLUGINS > 1 && !strcmp(id, desc_sine.id)) return create(host, &desc_sine, KIND_SINE);
	return NULL;
}
static const clap_plugin_factory_t factory = { f_count, f_desc, f_create };

static bool e_init(const char *path) { (void)path; return true; }
static void e_deinit(void) {}
static const void *e_get_factory(const char *id)
{
	return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, e_init, e_deinit, e_get_factory };
