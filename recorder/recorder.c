/* backline-recorder: record N PipeWire nodes to sample-aligned, timestamped files.
 * SPDX-License-Identifier: MIT
 *
 * Why it is one process with one node: every input port of a PipeWire node is
 * filled in the same graph cycle, so frame k of every track is the same graph
 * sample. Two separate recorders (two pw-record processes) start on different
 * cycles and can never be aligned after the fact without a reference.
 *
 *   backline-recorder -s NODE[:CHANNELS] [-s NODE...] -o DIR [-n TAKE]
 *                     [-f wav|wav24|flac] [-d SECONDS]
 *
 * For a sink (Audio/Sink) the recorder takes its monitor ports, for anything
 * else its audio output ports. It makes its own links (they die with it).
 *
 * Threads: main = PipeWire main loop (linking, arming, stop); data = process
 * callback (copies into one lock-free ring per track, never blocks); writer =
 * drains rings into libsndfile. Files are opened once the first recorded cycle
 * is known, so the BWF time reference is exact.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <sndfile.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/string.h>

#include "bl_graph.h"

#define MAX_TRACKS 32
#define MAX_CH 16
#define RING_SECONDS 8

struct track {
	char node[256];
	int channels;
	void *ports[MAX_CH];
	uint32_t port_ids[MAX_CH];      /* our ports' global ids */
	uint32_t src_ids[MAX_CH];       /* linked source ports */
	int n_src;
	struct spa_ringbuffer ring;
	float *ring_data;
	uint32_t ring_size;             /* bytes, power of two */
	float scratch[8192 * MAX_CH];
	SNDFILE *sf;
	char path[1024];
	uint64_t written;
};

struct start_info {
	uint64_t position;    /* graph clock position (frames) of the first sample */
	uint64_t cycle_nsec;  /* CLOCK_MONOTONIC of that cycle */
	uint64_t real_nsec;   /* CLOCK_REALTIME of that cycle (correlated) */
	uint32_t rate;
	uint32_t quantum;
	uint32_t clock_id;
};

static struct {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_filter *filter;
	struct spa_hook filter_listener;
	struct bl_graph *graph;
	struct spa_source *done_event, *tick;
	uint32_t node_id;

	struct track tracks[MAX_TRACKS];
	int n_tracks;
	char dir[512], take[128], format[16];
	double duration;
	uint64_t target_frames;     /* 0 = until stopped */

	atomic_int armed;           /* main -> data: links are up */
	atomic_int stop_req;        /* main -> data: stop at the next cycle */
	atomic_int started;         /* data -> writer */
	atomic_int finished;        /* data -> writer/main */
	int skip_cycles;            /* data: settle cycles after arming */
	uint64_t frames;            /* data: frames recorded */
	uint32_t q_min, q_max;      /* data: quantum range seen while recording */
	atomic_uint_fast64_t overruns;
	struct start_info start;
	pthread_t writer;
	int writer_running;
	int exit_code;
} R;

static uint64_t ts_ns(clockid_t c)
{
	struct timespec ts;
	clock_gettime(c, &ts);
	return (uint64_t)ts.tv_sec * SPA_NSEC_PER_SEC + ts.tv_nsec;
}

/* ------------------------------------------------------------ data thread */
static void on_process(void *data, struct spa_io_position *pos)
{
	uint32_t n = pos->clock.duration;
	if (!atomic_load(&R.armed) || atomic_load(&R.finished))
		return;
	if (R.skip_cycles > 0) {
		R.skip_cycles--;
		return;
	}
	if (!atomic_load(&R.started)) {
		uint64_t mono = ts_ns(CLOCK_MONOTONIC), real = ts_ns(CLOCK_REALTIME);
		R.start.position = pos->clock.position;
		R.start.cycle_nsec = pos->clock.nsec;
		R.start.real_nsec = real - (mono - pos->clock.nsec);
		R.start.rate = pos->clock.rate.denom;
		R.start.quantum = n;
		R.start.clock_id = pos->clock.id;
		R.frames = 0;
		R.q_min = R.q_max = n;
		if (R.duration > 0)
			R.target_frames = (uint64_t)(R.duration * R.start.rate + 0.5);
		atomic_store(&R.started, 1);
	}
	R.q_min = SPA_MIN(R.q_min, n);
	R.q_max = SPA_MAX(R.q_max, n);
	if (R.target_frames && R.frames + n > R.target_frames)
		n = (uint32_t)(R.target_frames - R.frames);
	bool stop = atomic_load(&R.stop_req);

	/* All tracks get the same n frames from the same cycle, or none do. */
	bool room = true;
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		uint32_t idx;
		int32_t filled = spa_ringbuffer_get_write_index(&tr->ring, &idx);
		if ((uint32_t)filled + n * tr->channels * sizeof(float) > tr->ring_size)
			room = false;
	}
	if (!room) {
		atomic_fetch_add(&R.overruns, n);
	} else {
		for (int t = 0; t < R.n_tracks; t++) {
			struct track *tr = &R.tracks[t];
			for (int c = 0; c < tr->channels; c++) {
				const float *in = pw_filter_get_dsp_buffer(tr->ports[c], pos->clock.duration);
				for (uint32_t i = 0; i < n; i++)
					tr->scratch[i * tr->channels + c] = in ? in[i] : 0.0f;
			}
			uint32_t idx, bytes = n * tr->channels * sizeof(float);
			spa_ringbuffer_get_write_index(&tr->ring, &idx);
			spa_ringbuffer_write_data(&tr->ring, tr->ring_data, tr->ring_size,
					idx & (tr->ring_size - 1), tr->scratch, bytes);
			spa_ringbuffer_write_update(&tr->ring, idx + bytes);
		}
	}
	R.frames += n;
	if (stop || (R.target_frames && R.frames >= R.target_frames)) {
		atomic_store(&R.finished, 1);
		pw_loop_signal_event(pw_main_loop_get_loop(R.loop), R.done_event);
	}
}

static void on_state_changed(void *data, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	if (state == PW_FILTER_STATE_ERROR)
		fprintf(stderr, "backline-recorder: filter error: %s\n", error ? error : "?");
	if (state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING)
		R.node_id = pw_filter_get_node_id(R.filter);
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = on_state_changed,
	.process = on_process,
};

/* ----------------------------------------------------------- writer thread */
static void open_files(void)
{
	time_t secs = (time_t)(R.start.real_nsec / SPA_NSEC_PER_SEC);
	struct tm lt;
	localtime_r(&secs, &lt);
	/* BWF time reference: samples since local midnight. */
	uint64_t midnight_ns = (uint64_t)(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec) * SPA_NSEC_PER_SEC +
		R.start.real_nsec % SPA_NSEC_PER_SEC;
	uint64_t tref = (uint64_t)((double)midnight_ns * R.start.rate / 1e9 + 0.5);

	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		SF_INFO info = { .samplerate = (int)R.start.rate, .channels = tr->channels };
		const char *ext = "wav";
		if (spa_streq(R.format, "flac")) {
			info.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
			ext = "flac";
		} else if (spa_streq(R.format, "wav24")) {
			info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
		} else {
			info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
		}
		char safe[256];
		snprintf(safe, sizeof(safe), "%s", tr->node);
		for (char *p = safe; *p; p++)
			if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.'))
				*p = '_';
		snprintf(tr->path, sizeof(tr->path), "%s/%s-%02d-%s.%s", R.dir, R.take, t + 1, safe, ext);
		tr->sf = sf_open(tr->path, SFM_WRITE, &info);
		if (!tr->sf) {
			fprintf(stderr, "backline-recorder: %s: %s\n", tr->path, sf_strerror(NULL));
			continue;
		}
		if (!spa_streq(ext, "flac")) {
			SF_BROADCAST_INFO bi;
			memset(&bi, 0, sizeof(bi));
			snprintf(bi.description, sizeof(bi.description),
				"backline take=%s track=%d node=%s graph_pos=%" PRIu64 " clock_id=%u",
				R.take, t + 1, tr->node, R.start.position, R.start.clock_id);
			snprintf(bi.originator, sizeof(bi.originator), "Backline recorder 0.1");
			snprintf(bi.originator_reference, sizeof(bi.originator_reference), "%.31s", R.take);
			/* fixed-width, not NUL-terminated fields */
			char tmp[32];
			strftime(tmp, sizeof(tmp), "%Y-%m-%d", &lt);
			memcpy(bi.origination_date, tmp, sizeof(bi.origination_date));
			strftime(tmp, sizeof(tmp), "%H:%M:%S", &lt);
			memcpy(bi.origination_time, tmp, sizeof(bi.origination_time));
			bi.time_reference_low = (uint32_t)tref;
			bi.time_reference_high = (uint32_t)(tref >> 32);
			bi.version = 1;
			bi.coding_history_size = (uint32_t)snprintf(bi.coding_history, sizeof(bi.coding_history),
				"A=PCM,F=%u,W=%s,M=%s,T=Backline recorder\r\n", R.start.rate,
				spa_streq(R.format, "wav24") ? "24" : "32", tr->channels == 1 ? "mono" : tr->channels == 2 ? "stereo" : "multi");
			if (sf_command(tr->sf, SFC_SET_BROADCAST_INFO, &bi, sizeof(bi)) != SF_TRUE)
				fprintf(stderr, "backline-recorder: could not write bext to %s\n", tr->path);
		} else {
			char c[256];
			snprintf(c, sizeof(c), "BACKLINE_TIME_REFERENCE=%" PRIu64 " BACKLINE_GRAPH_POSITION=%" PRIu64, tref, R.start.position);
			sf_set_string(tr->sf, SF_STR_COMMENT, c);
		}
	}
}

static void drain(void)
{
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		uint32_t idx;
		int32_t avail = spa_ringbuffer_get_read_index(&tr->ring, &idx);
		while (avail > 0) {
			uint32_t chunk = SPA_MIN((uint32_t)avail, (uint32_t)sizeof(tr->scratch) / 2);
			chunk -= chunk % (tr->channels * sizeof(float));
			static float buf[8192 * MAX_CH];
			spa_ringbuffer_read_data(&tr->ring, tr->ring_data, tr->ring_size,
					idx & (tr->ring_size - 1), buf, chunk);
			spa_ringbuffer_read_update(&tr->ring, idx + chunk);
			uint32_t frames = chunk / (tr->channels * sizeof(float));
			if (tr->sf)
				sf_writef_float(tr->sf, buf, frames);
			tr->written += frames;
			avail = spa_ringbuffer_get_read_index(&tr->ring, &idx);
		}
	}
}

static void *writer_main(void *arg)
{
	struct timespec nap = { 0, 10 * 1000000L };
	while (!atomic_load(&R.started) && !atomic_load(&R.finished))
		nanosleep(&nap, NULL);
	if (atomic_load(&R.started))
		open_files();
	while (!atomic_load(&R.finished)) {
		drain();
		nanosleep(&nap, NULL);
	}
	drain();
	for (int t = 0; t < R.n_tracks; t++)
		if (R.tracks[t].sf)
			sf_close(R.tracks[t].sf);
	return NULL;
}

/* ------------------------------------------------------------- main thread */
static int count_links_needed(void)
{
	int n = 0;
	for (int t = 0; t < R.n_tracks; t++)
		n += R.tracks[t].channels;
	return n;
}

struct match_ctx { struct track *tr; uint32_t node; bool monitor; int found; uint32_t ids[MAX_CH]; };

static int match_src(void *data, const struct bl_port *p)
{
	struct match_ctx *m = data;
	if (p->node_id != m->node || !p->output || p->midi || p->monitor != m->monitor)
		return 0;
	if (m->found < MAX_CH)
		m->ids[m->found++] = p->id;
	return 0;
}

struct own_ctx { const char *name; uint32_t id; };
static int match_own(void *data, const struct bl_port *p)
{
	struct own_ctx *o = data;
	if (p->node_id == R.node_id && spa_streq(p->name, o->name)) {
		o->id = p->id;
		return 1;
	}
	return 0;
}

/* Link every track whose source and our own ports are known. */
static void reconcile(void)
{
	if (R.node_id == SPA_ID_INVALID)
		return;
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		const struct bl_node *n = bl_graph_find_node(R.graph, tr->node);
		if (!n)
			continue;
		struct match_ctx m = { tr, n->id, strstr(n->media_class, "Sink") != NULL, 0, { 0 } };
		bl_graph_for_each_port(R.graph, match_src, &m);
		for (int c = 0; c < tr->channels; c++) {
			char name[64];
			snprintf(name, sizeof(name), "t%d_%d", t + 1, c + 1);
			struct own_ctx o = { name, SPA_ID_INVALID };
			bl_graph_for_each_port(R.graph, match_own, &o);
			if (o.id == SPA_ID_INVALID || m.found == 0)
				continue;
			uint32_t src = m.ids[SPA_MIN(c, m.found - 1)]; /* mono source -> all channels */
			if (!bl_graph_is_linked(R.graph, src, o.id)) {
				bl_graph_link(R.graph, src, o.id);
				fprintf(stderr, "backline-recorder: track %d ch %d <- %s port %u\n", t + 1, c + 1, tr->node, src);
			}
		}
	}
}

static void on_port_added(void *data, const struct bl_port *p)
{
	if (R.node_id == SPA_ID_INVALID && R.filter)
		R.node_id = pw_filter_get_node_id(R.filter);
	reconcile();
}

static const struct bl_graph_events graph_events = { .port_added = on_port_added };

static void on_tick(void *data, uint64_t expirations)
{
	reconcile();
	if (!atomic_load(&R.armed) && bl_graph_links_ready(R.graph) >= count_links_needed()) {
		R.skip_cycles = 2; /* let the new links carry data */
		atomic_store(&R.armed, 1);
		fprintf(stderr, "backline-recorder: armed, %d links\n", count_links_needed());
	}
}

static void write_sidecar(void)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.json", R.dir, R.take);
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	time_t secs = (time_t)(R.start.real_nsec / SPA_NSEC_PER_SEC);
	struct tm ut;
	gmtime_r(&secs, &ut);
	char iso[64];
	strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &ut);
	fprintf(f, "{\n  \"schema\": \"backline.take/1\",\n  \"take\": \"%s\",\n", R.take);
	fprintf(f, "  \"start\": {\n    \"utc\": \"%s.%09" PRIu64 "Z\",\n    \"realtime_ns\": %" PRIu64 ",\n"
		"    \"monotonic_ns\": %" PRIu64 ",\n    \"graph_position\": %" PRIu64 ",\n    \"clock_id\": %u\n  },\n",
		iso, (uint64_t)(R.start.real_nsec % SPA_NSEC_PER_SEC), R.start.real_nsec, R.start.cycle_nsec,
		R.start.position, R.start.clock_id);
	fprintf(f, "  \"rate\": %u,\n  \"quantum_at_start\": %u,\n  \"quantum_range\": [%u, %u],\n  \"frames\": %" PRIu64 ",\n  \"overrun_frames\": %" PRIu64 ",\n",
		R.start.rate, R.start.quantum, R.q_min, R.q_max, R.frames, (uint64_t)atomic_load(&R.overruns));
	fprintf(f, "  \"tracks\": [\n");
	for (int t = 0; t < R.n_tracks; t++)
		fprintf(f, "    {\"index\": %d, \"node\": \"%s\", \"channels\": %d, \"file\": \"%s\", \"frames\": %" PRIu64 "}%s\n",
			t + 1, R.tracks[t].node, R.tracks[t].channels, R.tracks[t].path, R.tracks[t].written,
			t + 1 < R.n_tracks ? "," : "");
	fprintf(f, "  ]\n}\n");
	fclose(f);
	printf("%s\n", path);
}

static void on_done(void *data, uint64_t count)
{
	pw_main_loop_quit(R.loop);
}

static void on_signal(void *data, int sig)
{
	if (atomic_load(&R.started))
		atomic_store(&R.stop_req, 1); /* stop on a cycle boundary */
	else {
		atomic_store(&R.finished, 1);
		pw_main_loop_quit(R.loop);
	}
}

static void usage(FILE *f)
{
	fputs("usage: backline-recorder -s NODE[:CHANNELS] [-s ...] -o DIR [-n TAKE] [-f wav|wav24|flac] [-d SECONDS]\n", f);
}

int main(int argc, char **argv)
{
	int c;
	R.node_id = SPA_ID_INVALID;
	snprintf(R.dir, sizeof(R.dir), ".");
	snprintf(R.format, sizeof(R.format), "wav");
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	strftime(R.take, sizeof(R.take), "take-%Y%m%d-%H%M%S", &lt);

	while ((c = getopt(argc, argv, "s:o:n:f:d:h")) != -1) {
		switch (c) {
		case 's': {
			if (R.n_tracks == MAX_TRACKS) { fprintf(stderr, "too many tracks\n"); return 2; }
			struct track *tr = &R.tracks[R.n_tracks++];
			char *colon = strrchr(optarg, ':');
			tr->channels = 2;
			if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
				tr->channels = SPA_CLAMP(atoi(colon + 1), 1, MAX_CH);
				*colon = 0;
			}
			snprintf(tr->node, sizeof(tr->node), "%s", optarg);
			break;
		}
		case 'o': snprintf(R.dir, sizeof(R.dir), "%s", optarg); break;
		case 'n': snprintf(R.take, sizeof(R.take), "%s", optarg); break;
		case 'f': snprintf(R.format, sizeof(R.format), "%s", optarg); break;
		case 'd': R.duration = atof(optarg); break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (R.n_tracks == 0) { usage(stderr); return 2; }
	mkdir(R.dir, 0755);

	pw_init(&argc, &argv);
	R.loop = pw_main_loop_new(NULL);
	struct pw_loop *l = pw_main_loop_get_loop(R.loop);
	pw_loop_add_signal(l, SIGINT, on_signal, NULL);
	pw_loop_add_signal(l, SIGTERM, on_signal, NULL);
	R.done_event = pw_loop_add_event(l, on_done, NULL);
	R.context = pw_context_new(l, NULL, 0);
	R.core = pw_context_connect(R.context, NULL, 0);
	if (!R.core) { fprintf(stderr, "cannot connect to PipeWire\n"); return 1; }
	R.graph = bl_graph_new(R.core, &graph_events, NULL);

	R.filter = pw_filter_new(R.core, "backline-recorder",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Production",
			PW_KEY_NODE_NAME, "backline.recorder",
			PW_KEY_NODE_DESCRIPTION, "Backline recorder",
			PW_KEY_NODE_AUTOCONNECT, "false",
			PW_KEY_NODE_ALWAYS_PROCESS, "true",
			"backline.take", R.take,
			NULL));
	pw_filter_add_listener(R.filter, &R.filter_listener, &filter_events, NULL);

	uint32_t rate_guess = 192000;
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		for (int ch = 0; ch < tr->channels; ch++) {
			char name[64];
			snprintf(name, sizeof(name), "t%d_%d", t + 1, ch + 1);
			tr->ports[ch] = pw_filter_add_port(R.filter, PW_DIRECTION_INPUT,
				PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
				pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
						  PW_KEY_PORT_NAME, name, NULL), NULL, 0);
		}
		uint32_t bytes = (uint32_t)(rate_guess * RING_SECONDS * tr->channels * sizeof(float));
		tr->ring_size = 1;
		while (tr->ring_size < bytes)
			tr->ring_size <<= 1;
		tr->ring_data = calloc(1, tr->ring_size);
		spa_ringbuffer_init(&tr->ring);
	}

	if (pw_filter_connect(R.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		fprintf(stderr, "cannot connect the filter\n");
		return 1;
	}
	R.tick = pw_loop_add_timer(l, on_tick, NULL);
	struct timespec tick = { 0, 20 * 1000000L };
	pw_loop_update_timer(l, R.tick, &tick, &tick, false);

	pthread_create(&R.writer, NULL, writer_main, NULL);
	R.writer_running = 1;

	pw_main_loop_run(R.loop);

	atomic_store(&R.finished, 1);
	pthread_join(R.writer, NULL);
	if (atomic_load(&R.started))
		write_sidecar();
	else {
		fprintf(stderr, "backline-recorder: never started (sources missing?)\n");
		R.exit_code = 1;
	}
	if (atomic_load(&R.overruns))
		fprintf(stderr, "backline-recorder: WARNING %" PRIu64 " frames lost to overruns\n", (uint64_t)atomic_load(&R.overruns));

	pw_filter_destroy(R.filter);
	bl_graph_destroy(R.graph);
	pw_core_disconnect(R.core);
	pw_context_destroy(R.context);
	pw_main_loop_destroy(R.loop);
	pw_deinit();
	return R.exit_code;
}
