/* backline-test-tap: a native PipeWire node with N audio inputs that keeps
 * what it receives, with the transport frame of every cycle as it sees it in
 * spa_io_position, and writes it all to a file when it is stopped (SIGINT or
 * SIGTERM). Test tool for tests/player-proof and tests/transport-proof.
 *
 *   backline-test-tap -n NODE -c CHANNELS -o FILE [-s MAX_SECONDS]
 *
 * File: per cycle a record
 *   int64 graph_position, int64 transport_frame, int32 rolling, uint32 frames,
 *   then frames x CHANNELS float32, channel by channel.
 * The data thread only copies into memory allocated up front.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/node/io.h>

#define MAXCH 16

static struct {
	struct pw_main_loop *loop;
	struct pw_filter *filter;
	struct spa_hook listener;
	void *in[MAXCH];
	int ch;
	char *mem;
	size_t cap;
	atomic_size_t used;
	atomic_ulong dropped;
} T;

static int64_t transport_frame(const struct spa_io_position *p, int *rolling)
{
	const struct spa_io_segment *seg = &p->segments[0];
	*rolling = p->state == SPA_IO_POSITION_STATE_RUNNING;
	if ((int64_t)p->clock.position < p->offset)
		return (int64_t)seg->position;
	uint64_t running = p->clock.position - p->offset;
	if (running >= seg->start && (seg->duration == 0 || running < seg->start + seg->duration))
		return (int64_t)((running - seg->start) * seg->rate + seg->position);
	return (int64_t)seg->position;
}

static void on_process(void *data, struct spa_io_position *pos)
{
	uint32_t n = (uint32_t)pos->clock.duration;
	size_t need = 24 + (size_t)n * T.ch * sizeof(float);
	size_t at = atomic_load_explicit(&T.used, memory_order_relaxed);
	if (at + need > T.cap) {
		atomic_fetch_add(&T.dropped, 1);
		return;
	}
	char *p = T.mem + at;
	int rolling;
	int64_t gp = (int64_t)pos->clock.position, tf = transport_frame(pos, &rolling);
	int32_t r = rolling;
	memcpy(p, &gp, 8);
	memcpy(p + 8, &tf, 8);
	memcpy(p + 16, &r, 4);
	memcpy(p + 20, &n, 4);
	float *o = (float *)(p + 24);
	for (int c = 0; c < T.ch; c++) {
		float *b = pw_filter_get_dsp_buffer(T.in[c], n);
		if (b)
			memcpy(o + (size_t)c * n, b, n * sizeof(float));
		else
			memset(o + (size_t)c * n, 0, n * sizeof(float));
	}
	atomic_store_explicit(&T.used, at + need, memory_order_release);
}

static const struct pw_filter_events events = { PW_VERSION_FILTER_EVENTS, .process = on_process };

static void on_signal(void *d, int s) { pw_main_loop_quit(T.loop); }

int main(int argc, char **argv)
{
	const char *name = "backline-test.tap", *outp = NULL;
	double secs = 120;
	T.ch = 2;
	int c;
	while ((c = getopt(argc, argv, "n:c:o:s:")) != -1) {
		switch (c) {
		case 'n': name = optarg; break;
		case 'c': T.ch = atoi(optarg); break;
		case 'o': outp = optarg; break;
		case 's': secs = atof(optarg); break;
		default: return 2;
		}
	}
	if (!outp || T.ch < 1 || T.ch > MAXCH) {
		fprintf(stderr, "usage: backline-test-tap -n NODE -c CHANNELS -o FILE [-s MAX_SECONDS]\n");
		return 2;
	}
	/* 48 kHz worst case, plus a header per 32-frame cycle */
	T.cap = (size_t)(secs * 48000) * T.ch * sizeof(float) + (size_t)(secs * 48000 / 32) * 24;
	T.mem = malloc(T.cap);
	if (!T.mem)
		return 1;
	for (size_t i = 0; i < T.cap; i += 4096)
		T.mem[i] = 0;   /* fault it in now, not on the data thread */
	pw_init(&argc, &argv);
	T.loop = pw_main_loop_new(NULL);
	struct pw_loop *l = pw_main_loop_get_loop(T.loop);
	pw_loop_add_signal(l, SIGINT, on_signal, NULL);
	pw_loop_add_signal(l, SIGTERM, on_signal, NULL);
	struct pw_context *ctx = pw_context_new(l, NULL, 0);
	struct pw_core *core = pw_context_connect(ctx, NULL, 0);
	if (!core) {
		fprintf(stderr, "no PipeWire\n");
		return 1;
	}
	T.filter = pw_filter_new(core, name, pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
		PW_KEY_NODE_NAME, name, PW_KEY_NODE_AUTOCONNECT, "false", PW_KEY_NODE_ALWAYS_PROCESS, "true",
		"node.dont-fallback", "true", NULL));
	pw_filter_add_listener(T.filter, &T.listener, &events, NULL);
	static const char *pn[] = { "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR" };
	for (int i = 0; i < T.ch; i++) {
		char port[32], ch[16];
		if (T.ch == 1)
			snprintf(ch, sizeof(ch), "MONO");
		else if (i < 8)
			snprintf(ch, sizeof(ch), "%s", pn[i]);
		else
			snprintf(ch, sizeof(ch), "AUX%d", i);
		snprintf(port, sizeof(port), "input_%s", ch);
		T.in[i] = pw_filter_add_port(T.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, port,
					  PW_KEY_AUDIO_CHANNEL, ch, NULL), NULL, 0);
	}
	pw_filter_connect(T.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
	pw_main_loop_run(T.loop);
	pw_filter_destroy(T.filter);
	FILE *f = fopen(outp, "wb");
	size_t used = atomic_load(&T.used);
	if (f) {
		fwrite(T.mem, 1, used, f);
		fclose(f);
	}
	fprintf(stderr, "backline-test-tap: %zu bytes, %lu cycles dropped (buffer full)\n", used, atomic_load(&T.dropped));
	pw_core_disconnect(core);
	pw_context_destroy(ctx);
	pw_main_loop_destroy(T.loop);
	pw_deinit();
	return 0;
}
