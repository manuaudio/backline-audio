/* backline-test-source: fake capture devices with a known input latency.
 * SPDX-License-Identifier: MIT
 *
 *   backline-test-source FILE NAME:LATENCY[:physical] [NAME:LATENCY...]
 *
 * Each NAME becomes an Audio/Source node with one mono output port that plays
 * FILE's first channel delayed by LATENCY frames, the way a converter delays
 * what it captures, and reports that delay as its capture latency
 * (SPA_PARAM_Latency, output direction, as an ALSA capture port does). ":physical" marks the port
 * port.physical, like a hardware input. All nodes live in this one process
 * and start the file in the same graph cycle, so the only difference between
 * them is the declared and real latency. Test tool only: it never links.
 *
 *   backline-test-source --sines BASE_HZ:CHANNELS NAME
 *
 * One node with CHANNELS outputs; channel c is a sine of (c + 1) x BASE_HZ
 * computed from the graph position (clock.position), so the signal itself
 * can never glitch: a recorder that drops or repeats a frame shows it.
 * The file plays once, 0.25 s after SIGUSR1 (on every node at the same
 * graph frame); then silence.
 */
#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <sndfile.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>

#define MAX_NODES 8

struct src {
	struct pw_filter *filter;
	struct spa_hook listener;
	void *port;
	uint32_t latency;
	bool physical;
	bool streaming;
};

static struct {
	struct pw_main_loop *loop;
	float *data;
	uint64_t frames;
	struct src s[MAX_NODES];
	int n;
	atomic_uint_fast64_t start_pos;   /* graph position where the file starts (0 = not yet) */
	atomic_int done, go;
} T;

static void on_process(void *data, struct spa_io_position *pos)
{
	struct src *s = data;
	uint32_t n = pos->clock.duration;
	float *out = pw_filter_get_dsp_buffer(s->port, n);
	if (!out)
		return;
	uint64_t start = atomic_load(&T.start_pos);
	for (uint32_t i = 0; i < n; i++) {
		float v = 0.0f;
		if (start) {
			int64_t k = (int64_t)(pos->clock.position + i) - (int64_t)start - (int64_t)s->latency;
			if (k >= 0 && (uint64_t)k < T.frames)
				v = T.data[k];
		}
		out[i] = v;
	}
	if (start && pos->clock.position > start + T.frames + 48000 + s->latency)
		atomic_store(&T.done, 1);
}

static void on_state(void *data, enum pw_filter_state old, enum pw_filter_state st, const char *err)
{
	struct src *s = data;
	s->streaming = st == PW_FILTER_STATE_STREAMING;
}

static const struct pw_filter_events events = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = on_state,
	.process = on_process,
};

static void on_quit(void *data, int sig)
{
	if (sig == SIGUSR1)
		atomic_store(&T.go, 1);
	else
		pw_main_loop_quit(T.loop);
}

/* Node 0 sets the start once SIGUSR1 asked for it: every node of the
 * driver group sees the same clock.position, so they all start together. */
static void arm_start(void *data, struct spa_io_position *pos)
{
	if (!atomic_load(&T.start_pos) && atomic_load(&T.go))
		atomic_store(&T.start_pos, pos->clock.position + pos->clock.rate.denom / 4);
	on_process(data, pos);
}

static const struct pw_filter_events events0 = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = on_state,
	.process = arm_start,
};

static struct { struct pw_filter *f; struct spa_hook l; void *ports[64]; int n; double base; } S;

static void on_sines(void *data, struct spa_io_position *pos)
{
	uint32_t n = pos->clock.duration, rate = pos->clock.rate.denom;
	for (int c = 0; c < S.n; c++) {
		float *o = pw_filter_get_dsp_buffer(S.ports[c], n);
		if (!o)
			continue;
		double f = S.base * (c + 1);
		for (uint32_t i = 0; i < n; i++) {
			uint64_t x = (pos->clock.position + i) % rate;   /* integer Hz: periodic in 1 s */
			o[i] = (float)(0.25 * sin(2 * M_PI * f * (double)x / rate));
		}
	}
}

static const struct pw_filter_events sine_events = { PW_VERSION_FILTER_EVENTS, .process = on_sines };

static int run_sines(int argc, char **argv)
{
	if (argc < 4 || sscanf(argv[2], "%lf:%d", &S.base, &S.n) != 2 || S.n < 1 || S.n > 64)
		return 2;
	pw_init(&argc, &argv);
	T.loop = pw_main_loop_new(NULL);
	struct pw_loop *l = pw_main_loop_get_loop(T.loop);
	pw_loop_add_signal(l, SIGINT, on_quit, NULL);
	pw_loop_add_signal(l, SIGTERM, on_quit, NULL);
	struct pw_context *ctx = pw_context_new(l, NULL, 0);
	struct pw_core *core = pw_context_connect(ctx, NULL, 0);
	if (!core)
		return 1;
	S.f = pw_filter_new(core, argv[3], pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CLASS, "Audio/Source",
		PW_KEY_NODE_NAME, argv[3], PW_KEY_NODE_AUTOCONNECT, "false", PW_KEY_NODE_ALWAYS_PROCESS, "true",
		"node.virtual", "true", "priority.session", "1", NULL));
	pw_filter_add_listener(S.f, &S.l, &sine_events, NULL);
	for (int c = 0; c < S.n; c++) {
		char name[32];
		snprintf(name, sizeof(name), "out_%02d", c);
		S.ports[c] = pw_filter_add_port(S.f, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, name, NULL), NULL, 0);
	}
	pw_filter_connect(S.f, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
	pw_main_loop_run(T.loop);
	pw_filter_destroy(S.f);
	pw_core_disconnect(core);
	pw_context_destroy(ctx);
	pw_main_loop_destroy(T.loop);
	pw_deinit();
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--sines"))
		return run_sines(argc, argv);
	if (argc < 3) {
		fprintf(stderr, "usage: backline-test-source FILE NAME:LATENCY[:physical] ...\n");
		return 2;
	}
	SF_INFO info = { 0 };
	SNDFILE *sf = sf_open(argv[1], SFM_READ, &info);
	if (!sf) {
		fprintf(stderr, "%s: %s\n", argv[1], sf_strerror(NULL));
		return 1;
	}
	float *inter = calloc((size_t)info.frames * info.channels, sizeof(float));
	sf_readf_float(sf, inter, info.frames);
	sf_close(sf);
	T.frames = (uint64_t)info.frames;
	T.data = calloc(T.frames, sizeof(float));
	for (uint64_t i = 0; i < T.frames; i++)
		T.data[i] = inter[i * info.channels];
	free(inter);

	pw_init(&argc, &argv);
	T.loop = pw_main_loop_new(NULL);
	struct pw_loop *l = pw_main_loop_get_loop(T.loop);
	pw_loop_add_signal(l, SIGINT, on_quit, NULL);
	pw_loop_add_signal(l, SIGTERM, on_quit, NULL);
	pw_loop_add_signal(l, SIGUSR1, on_quit, NULL);
	struct pw_context *ctx = pw_context_new(l, NULL, 0);
	struct pw_core *core = pw_context_connect(ctx, NULL, 0);
	if (!core)
		return 1;

	for (int a = 2; a < argc && T.n < MAX_NODES; a++) {
		struct src *s = &T.s[T.n];
		char name[256];
		snprintf(name, sizeof(name), "%s", argv[a]);
		char *c1 = strchr(name, ':');
		if (!c1)
			return 2;
		*c1++ = 0;
		s->latency = (uint32_t)atoi(c1);
		s->physical = strstr(c1, ":physical") != NULL;
		s->filter = pw_filter_new(core, name,
			pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CLASS, "Audio/Source",
					  PW_KEY_NODE_NAME, name, PW_KEY_NODE_DESCRIPTION, name,
					  PW_KEY_NODE_AUTOCONNECT, "false", PW_KEY_NODE_ALWAYS_PROCESS, "true",
					  "node.virtual", "true", "priority.session", "1", NULL));
		pw_filter_add_listener(s->filter, &s->listener, T.n == 0 ? &events0 : &events, s);
		uint8_t buf[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		/* The port's capture latency, kept as declared: CUSTOM_LATENCY
		 * stops pw_filter's default algorithm from recomputing it (to 0,
		 * from no inputs) once the port is linked, the way a device node
		 * keeps reporting its converter's delay. */
		struct spa_latency_info li = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT, .min_rate = (int32_t)s->latency,
							      .max_rate = (int32_t)s->latency);
		const struct spa_pod *params[1] = { spa_latency_build(&b, SPA_PARAM_Latency, &li) };
		s->port = pw_filter_add_port(s->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, "capture_MONO",
					  PW_KEY_PORT_PHYSICAL, s->physical ? "true" : "false",
					  PW_KEY_PORT_TERMINAL, s->physical ? "true" : "false", NULL),
			params, 1);
		pw_filter_connect(s->filter, PW_FILTER_FLAG_RT_PROCESS | PW_FILTER_FLAG_CUSTOM_LATENCY, NULL, 0);
		fprintf(stderr, "backline-test-source: %s latency %u frames%s\n", name, s->latency, s->physical ? " (physical)" : "");
		T.n++;
	}
	pw_main_loop_run(T.loop);
	for (int i = 0; i < T.n; i++)
		pw_filter_destroy(T.s[i].filter);
	pw_core_disconnect(core);
	pw_context_destroy(ctx);
	pw_main_loop_destroy(T.loop);
	pw_deinit();
	return 0;
}
