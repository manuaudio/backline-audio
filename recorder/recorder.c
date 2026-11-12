/* backline-recorder: record N PipeWire nodes to sample-aligned, timestamped files.
 * SPDX-License-Identifier: MIT
 *
 * Why it is one process with one node: every input port of a PipeWire node is
 * filled in the same graph cycle, so frame k of every track is the same graph
 * sample. Two separate recorders (two pw-record processes) start on different
 * cycles and can never be aligned after the fact without a reference.
 *
 *   backline-recorder -s NODE[:CHANNELS] [-s NODE...] -o DIR [-n TAKE]
 *                     [-f wav|wav24|flac] [-d SECONDS] [-a signal|graph]
 *                     [-P SECONDS]
 *
 * For a sink (Audio/Sink) the recorder takes its monitor ports, for anything
 * else its audio output ports. It makes its own links (they die with it).
 *
 * Alignment (-a). "graph" keeps frame k = the same graph cycle on every
 * track. "signal" (the default) also lines up the *signal*: the driver runs
 * first in each cycle, so a sink that drives the recorder's cycle carries on
 * its monitor what it was fed in the previous cycle, one quantum later than a
 * follower sink's monitor of the same signal (docs/proofs/recorder-proof.txt).
 * The recorder drops that many leading frames from such a track; the driver
 * is spa_io_position clock.id. All tracks keep the same length. The sinks'
 * reported playback latency (SPA_PARAM_Latency) is logged in the sidecar but
 * not used: PipeWire reports the same figure for drivers and followers, and 0
 * until a sink is fed (rec_align.h).
 *
 * Pre-roll (-P SECONDS): arm and keep the last SECONDS in memory; the take
 * starts on SIGUSR1 (or a line "record" on stdin) and includes them, so the
 * first notes are never lost. SIGINT/SIGTERM stop on a cycle boundary.
 *
 * Threads: main = PipeWire main loop (linking, arming, latency, stop); data =
 * process callback (copies into one lock-free ring per track, never blocks);
 * writer = drains rings into libsndfile. Files are opened once the take's
 * first frame is known, so the BWF time reference is exact.
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
#include "rec_align.h"

#define MAX_TRACKS 32
#define MAX_CH 16
#define RING_SECONDS 8 /* at 192 kHz; 32 s at 48 kHz */

struct track {
	char node[256];
	int channels;
	void *ports[MAX_CH];
	struct spa_ringbuffer ring;
	float *ring_data;
	uint32_t ring_size;             /* bytes, power of two */
	float scratch[8192 * MAX_CH];
	SNDFILE *sf;
	char path[1024];
	uint64_t written;               /* frames written to the file */
	uint64_t consumed;              /* frames taken from the ring (writer) */

	/* set by main before arming */
	uint32_t src_node_id;
	bool sink_monitor;              /* recording a sink's monitor */
	struct bl_latency playback;     /* the sink's input ports, output direction */
	uint32_t playback_ports[MAX_CH];
	int n_playback_ports;

	/* data thread */
	float *prev;                    /* previous cycle, channels x 8192, for signal alignment */
	bool is_driver;                 /* drove the first cycle of the take */
	uint64_t driver_cycles;         /* cycles of the take in which it drove the graph */
};

struct start_info {
	uint64_t position;    /* graph clock position (frames) of the take's first frame */
	uint64_t cycle_nsec;  /* CLOCK_MONOTONIC of that frame */
	uint64_t real_nsec;   /* CLOCK_REALTIME of that frame (correlated) */
	uint64_t trigger_position, trigger_nsec, trigger_real_nsec;
	uint32_t rate;
	uint32_t quantum;
	uint32_t clock_id;    /* the driver's node id */
	uint64_t pre_roll_frames;
};

static struct {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_filter *filter;
	struct spa_hook filter_listener;
	struct bl_graph *graph;
	struct spa_source *done_event, *tick, *stdin_src;
	uint32_t node_id;

	struct track tracks[MAX_TRACKS];
	int n_tracks;
	char dir[512], take[128], format[16];
	double duration;
	bool align_signal;
	double pre_roll;
	uint64_t pre_roll_frames;   /* at the graph rate, set at arm time */

	atomic_int armed;           /* main -> data: links are up */
	atomic_int record_req;      /* main -> data: start the take now */
	atomic_int stop_req;        /* main -> data: stop at the next cycle */
	atomic_int started;         /* data -> writer: the take has a first frame */
	atomic_int finished;        /* data -> writer/main */
	int skip_cycles;            /* data: settle cycles after arming */
	atomic_uint_fast64_t ring_frames;  /* data: frames written to every ring since arming */
	uint64_t take_begin;        /* data: ring frame index where the take begins */
	uint64_t target_frames;     /* data: ring frames needed (0 = until stopped) */
	bool pairing;               /* signal mode with a sink monitor: one-cycle pairing (rec_align.h) */
	bool prev_valid;            /* data: the previous cycle was copied */
	uint32_t prev_n;            /* data: its length */
	uint64_t prev_position, prev_nsec;
	uint32_t last_driver;
	uint32_t quantum_changes, driver_changes;  /* data: during the take */
	uint32_t q_min, q_max;      /* data: quantum range seen while recording */
	atomic_uint_fast64_t overruns;
	atomic_uint_fast64_t discarded;    /* writer: frames dropped in pre-roll standby */
	struct start_info start;
	uint32_t rate_seen;
	pthread_t writer;
	int exit_code;
} R;

static uint64_t ts_ns(clockid_t c)
{
	struct timespec ts;
	clock_gettime(c, &ts);
	return (uint64_t)ts.tv_sec * SPA_NSEC_PER_SEC + ts.tv_nsec;
}

/* ------------------------------------------------------------ data thread */
static struct rec_align_track align_tracks[MAX_TRACKS];

/* The take starts with the frames written in this cycle. With pairing, those
 * are the previous cycle's input, so the timestamps are that cycle's. */
static void start_take(struct spa_io_position *pos, uint32_t n)
{
	uint64_t mono = ts_ns(CLOCK_MONOTONIC), real = ts_ns(CLOCK_REALTIME);
	uint32_t rate = pos->clock.rate.denom;
	uint64_t now_frames = atomic_load(&R.ring_frames);  /* this cycle is not written yet */
	uint64_t keep = R.pre_roll_frames;
	uint64_t oldest = atomic_load(&R.discarded);         /* the writer stops discarding before `begin` */
	uint64_t begin = now_frames > keep ? now_frames - keep : 0;
	if (begin < oldest)
		begin = oldest;

	uint64_t cyc_pos = R.pairing ? R.prev_position : pos->clock.position;
	uint64_t cyc_nsec = R.pairing ? R.prev_nsec : pos->clock.nsec;
	for (int t = 0; t < R.n_tracks; t++)
		R.tracks[t].is_driver = R.tracks[t].src_node_id == pos->clock.id;
	R.start.trigger_position = cyc_pos;
	R.start.trigger_nsec = cyc_nsec;
	R.start.trigger_real_nsec = real - (mono - cyc_nsec);
	R.start.pre_roll_frames = now_frames - begin;
	uint64_t back_ns = R.start.pre_roll_frames * SPA_NSEC_PER_SEC / rate;
	R.start.position = cyc_pos - R.start.pre_roll_frames;
	R.start.cycle_nsec = cyc_nsec - back_ns;
	R.start.real_nsec = R.start.trigger_real_nsec - back_ns;
	R.start.rate = rate;
	R.start.quantum = n;
	R.start.clock_id = pos->clock.id;
	R.take_begin = begin;
	R.q_min = R.q_max = n;
	R.last_driver = pos->clock.id;
	if (R.duration > 0)
		R.target_frames = begin + (uint64_t)(R.duration * rate + 0.5);
	atomic_store(&R.started, 1);
}

static void keep_previous(struct spa_io_position *pos, float *const cur[MAX_TRACKS][MAX_CH], uint32_t n)
{
	if (!R.pairing)
		return;
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		for (int c = 0; c < tr->channels; c++) {
			float *dst = tr->prev + (size_t)c * 8192;
			if (cur[t][c])
				memcpy(dst, cur[t][c], n * sizeof(float));
			else
				memset(dst, 0, n * sizeof(float));
		}
	}
	R.prev_n = n;
	R.prev_position = pos->clock.position;
	R.prev_nsec = pos->clock.nsec;
	R.prev_valid = true;
}

static void on_process(void *data, struct spa_io_position *pos)
{
	uint32_t n = SPA_MIN(pos->clock.duration, 8192u);
	R.rate_seen = pos->clock.rate.denom;
	if (!atomic_load(&R.armed) || atomic_load(&R.finished))
		return;

	float *cur[MAX_TRACKS][MAX_CH];
	for (int t = 0; t < R.n_tracks; t++)
		for (int c = 0; c < R.tracks[t].channels; c++)
			cur[t][c] = pw_filter_get_dsp_buffer(R.tracks[t].ports[c], pos->clock.duration);

	if (R.skip_cycles > 0 || (R.pairing && !R.prev_valid)) {
		if (R.skip_cycles > 0)
			R.skip_cycles--;
		keep_previous(pos, cur, n);
		return;
	}
	bool started = atomic_load(&R.started);
	if (!started && (R.pre_roll_frames == 0 || atomic_load(&R.record_req))) {
		start_take(pos, n);
		started = true;
	}
	bool use_cur[MAX_TRACKS];
	rec_align_select(align_tracks, R.n_tracks, R.pairing, pos->clock.id, use_cur);
	/* frames this cycle: with pairing, the previous cycle's input */
	uint32_t w = R.pairing ? R.prev_n : n;
	if (started) {
		R.q_min = SPA_MIN(R.q_min, n);
		R.q_max = SPA_MAX(R.q_max, n);
		if (R.pairing && n != R.prev_n)
			R.quantum_changes++;
		if (pos->clock.id != R.last_driver) {
			R.driver_changes++;
			R.last_driver = pos->clock.id;
		}
		for (int t = 0; t < R.n_tracks; t++)
			R.tracks[t].driver_cycles += R.tracks[t].src_node_id == pos->clock.id;
	}
	uint64_t have = atomic_load(&R.ring_frames);
	if (started && R.target_frames && have + w > R.target_frames)
		w = (uint32_t)(R.target_frames - have);
	bool stop = started && atomic_load(&R.stop_req);

	/* All tracks get the same w frames, or none do. */
	bool room = true;
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		uint32_t idx;
		int32_t filled = spa_ringbuffer_get_write_index(&tr->ring, &idx);
		if ((uint32_t)filled + w * tr->channels * sizeof(float) > tr->ring_size)
			room = false;
	}
	if (!room) {
		atomic_fetch_add(&R.overruns, w);
	} else if (w > 0) {
		for (int t = 0; t < R.n_tracks; t++) {
			struct track *tr = &R.tracks[t];
			uint32_t avail = use_cur[t] ? n : R.prev_n;
			for (int c = 0; c < tr->channels; c++) {
				const float *in = use_cur[t] ? cur[t][c] : tr->prev + (size_t)c * 8192;
				for (uint32_t i = 0; i < w; i++)
					tr->scratch[i * tr->channels + c] = in && i < avail ? in[i] : 0.0f;
			}
			uint32_t idx, bytes = w * tr->channels * sizeof(float);
			spa_ringbuffer_get_write_index(&tr->ring, &idx);
			spa_ringbuffer_write_data(&tr->ring, tr->ring_data, tr->ring_size,
					idx & (tr->ring_size - 1), tr->scratch, bytes);
			spa_ringbuffer_write_update(&tr->ring, idx + bytes);
		}
		atomic_fetch_add(&R.ring_frames, w);
	}
	keep_previous(pos, cur, n);
	if (stop || (started && R.target_frames && atomic_load(&R.ring_frames) >= R.target_frames)) {
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
				"backline take=%s track=%d node=%s graph_pos=%" PRIu64 " clock_id=%u align=%s",
				R.take, t + 1, tr->node, R.start.position, R.start.clock_id, R.pairing ? "signal" : "graph");
			snprintf(bi.originator, sizeof(bi.originator), "Backline recorder 0.2");
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

/* Take up to `frames` frames off a track's ring; write them to its file if
 * `write`, else drop them. */
static void take_frames(struct track *tr, uint64_t frames, bool write)
{
	static float buf[8192 * MAX_CH];
	const uint32_t fsz = tr->channels * sizeof(float);
	while (frames > 0) {
		uint32_t idx;
		int32_t avail = spa_ringbuffer_get_read_index(&tr->ring, &idx);
		uint32_t chunk_frames = SPA_MIN((uint64_t)((uint32_t)avail / fsz), SPA_MIN(frames, (uint64_t)8192));
		if (chunk_frames == 0)
			return;
		uint32_t bytes = chunk_frames * fsz;
		if (write)
			spa_ringbuffer_read_data(&tr->ring, tr->ring_data, tr->ring_size, idx & (tr->ring_size - 1), buf, bytes);
		spa_ringbuffer_read_update(&tr->ring, idx + bytes);
		tr->consumed += chunk_frames;
		if (write) {
			if (tr->sf)
				sf_writef_float(tr->sf, buf, chunk_frames);
			tr->written += chunk_frames;
		}
		frames -= chunk_frames;
	}
}

/* Before the take: in pre-roll standby keep only the newest pre-roll
 * frames; every track drops the same number, so they stay aligned. */
static void discard_standby(void)
{
	uint64_t have = atomic_load(&R.ring_frames);
	uint64_t gone = atomic_load(&R.discarded);
	if (have <= gone + R.pre_roll_frames + 8192)
		return;
	uint64_t drop = have - gone - R.pre_roll_frames - 8192; /* keep a margin past the pre-roll */
	for (int t = 0; t < R.n_tracks; t++)
		take_frames(&R.tracks[t], drop, false);
	atomic_store(&R.discarded, gone + drop);
}

/* During and after the take: every track carries ring frames
 * [begin, begin + L), L = frames so far - begin. */
static void drain(void)
{
	uint64_t have = atomic_load(&R.ring_frames);
	uint64_t avail_len = have > R.take_begin ? have - R.take_begin : 0;
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		if (tr->consumed < R.take_begin) /* ring frames taken so far, standby discards included */
			take_frames(tr, R.take_begin - tr->consumed, false);
		if (tr->written < avail_len)
			take_frames(tr, avail_len - tr->written, true);
	}
}

static void *writer_main(void *arg)
{
	struct timespec nap = { 0, 10 * 1000000L };
	while (!atomic_load(&R.started) && !atomic_load(&R.finished)) {
		if (atomic_load(&R.armed) && R.pre_roll_frames)
			discard_standby();
		nanosleep(&nap, NULL);
	}
	if (atomic_load(&R.started))
		open_files();
	while (!atomic_load(&R.finished)) {
		drain();
		nanosleep(&nap, NULL);
	}
	if (atomic_load(&R.started))
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

struct match_ctx { uint32_t node; bool monitor; bool want_playback; int found; uint32_t ids[MAX_CH]; };

static int match_src(void *data, const struct bl_port *p)
{
	struct match_ctx *m = data;
	if (p->node_id != m->node || p->midi)
		return 0;
	if (m->want_playback) {     /* a sink's input (playback) ports */
		if (p->output || p->monitor)
			return 0;
	} else if (!p->output || p->monitor != m->monitor) {
		return 0;
	}
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

/* Link every track whose source and our own ports are known; watch the
 * playback latency of every recorded sink. */
static void reconcile(void)
{
	if (R.node_id == SPA_ID_INVALID)
		return;
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		const struct bl_node *n = bl_graph_find_node(R.graph, tr->node);
		if (!n)
			continue;
		tr->src_node_id = n->id;
		tr->sink_monitor = strstr(n->media_class, "Sink") != NULL;
		struct match_ctx m = { n->id, tr->sink_monitor, false, 0, { 0 } };
		bl_graph_for_each_port(R.graph, match_src, &m);
		if (tr->sink_monitor) {
			struct match_ctx pb = { n->id, false, true, 0, { 0 } };
			bl_graph_for_each_port(R.graph, match_src, &pb);
			tr->n_playback_ports = pb.found;
			for (int k = 0; k < pb.found; k++) {
				tr->playback_ports[k] = pb.ids[k];
				bl_graph_watch_latency(R.graph, pb.ids[k]);
			}
		}
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

/* The largest playback latency over a sink's input ports, once all known. */
static bool latencies_known(void)
{
	bool all = true;
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		if (!tr->sink_monitor)
			continue;
		struct bl_latency best = { 0 };
		for (int k = 0; k < tr->n_playback_ports; k++) {
			const struct bl_port *p = bl_graph_port(R.graph, tr->playback_ports[k]);
			if (!p || !p->latency_known) {
				all = false;
				continue;
			}
			const struct bl_latency *l = &p->latency[1]; /* SPA_DIRECTION_OUTPUT: playback latency */
			if (l->valid && (!best.valid || l->max_quantum > best.max_quantum ||
					 l->max_rate > best.max_rate || l->max_ns > best.max_ns))
				best = *l;
		}
		tr->playback = best;
	}
	return all;
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
	if (atomic_load(&R.armed) || bl_graph_links_ready(R.graph) < count_links_needed())
		return;
	uint32_t rate = R.rate_seen ? R.rate_seen : 48000;
	R.pre_roll_frames = (uint64_t)(R.pre_roll * rate + 0.5);
	for (int t = 0; t < R.n_tracks; t++) {
		align_tracks[t].sink_monitor = R.tracks[t].sink_monitor;
		align_tracks[t].src_node_id = R.tracks[t].src_node_id;
	}
	R.pairing = rec_align_pairing(align_tracks, R.n_tracks, R.align_signal);
	R.skip_cycles = 2; /* let the new links carry data */
	atomic_store(&R.armed, 1);
	if (R.pre_roll_frames)
		fprintf(stderr, "backline-recorder: armed, %d links; standing by with %.2f s pre-roll (SIGUSR1 or 'record' on stdin to start)\n",
			count_links_needed(), R.pre_roll);
	else
		fprintf(stderr, "backline-recorder: armed, %d links\n", count_links_needed());
}

static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 0x20)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

static void iso_utc(uint64_t real_ns, char *out, size_t n)
{
	time_t secs = (time_t)(real_ns / SPA_NSEC_PER_SEC);
	struct tm ut;
	gmtime_r(&secs, &ut);
	char iso[64];
	strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &ut);
	snprintf(out, n, "%s.%09" PRIu64 "Z", iso, (uint64_t)(real_ns % SPA_NSEC_PER_SEC));
}

static void write_sidecar(void)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.json", R.dir, R.take);
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	latencies_known(); /* the sinks' reported playback latency, as of now */
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		fprintf(stderr, "backline-recorder: track %d: %s %s, node %u, drove %" PRIu64 " cycles of the take; reported playback latency %.1f quantum + %d frames + %" PRId64 " ns\n",
			t + 1, tr->node, tr->sink_monitor ? "monitor" : "output", tr->src_node_id, tr->driver_cycles,
			tr->playback.max_quantum, tr->playback.max_rate, tr->playback.max_ns);
	}
	char utc[96], trig[96];
	iso_utc(R.start.real_nsec, utc, sizeof(utc));
	iso_utc(R.start.trigger_real_nsec, trig, sizeof(trig));
	uint64_t frames = R.tracks[0].written;
	fprintf(f, "{\n  \"schema\": \"backline.take/2\",\n  \"take\": ");
	json_str(f, R.take);
	fprintf(f, ",\n  \"start\": {\n    \"utc\": \"%s\",\n    \"realtime_ns\": %" PRIu64 ",\n"
		"    \"monotonic_ns\": %" PRIu64 ",\n    \"graph_position\": %" PRIu64 ",\n    \"clock_id\": %u\n  },\n",
		utc, R.start.real_nsec, R.start.cycle_nsec, R.start.position, R.start.clock_id);
	fprintf(f, "  \"trigger\": {\n    \"utc\": \"%s\",\n    \"monotonic_ns\": %" PRIu64 ",\n    \"graph_position\": %" PRIu64
		",\n    \"pre_roll_frames\": %" PRIu64 ",\n    \"pre_roll_requested_frames\": %" PRIu64 "\n  },\n",
		trig, R.start.trigger_nsec, R.start.trigger_position, R.start.pre_roll_frames, R.pre_roll_frames);
	fprintf(f, "  \"rate\": %u,\n  \"quantum_at_start\": %u,\n  \"quantum_range\": [%u, %u],\n  \"frames\": %" PRIu64
		",\n  \"overrun_frames\": %" PRIu64 ",\n",
		R.start.rate, R.start.quantum, R.q_min, R.q_max, frames, (uint64_t)atomic_load(&R.overruns));
	fprintf(f, "  \"alignment\": {\n    \"mode\": \"%s\",\n    \"one_cycle_pairing\": %s,\n    \"driver_at_start\": %u,\n"
		"    \"driver_changes\": %u,\n    \"quantum_changes\": %u\n  },\n",
		R.align_signal ? "signal" : "graph", R.pairing ? "true" : "false", R.start.clock_id, R.driver_changes,
		R.quantum_changes);
	fprintf(f, "  \"tracks\": [\n");
	for (int t = 0; t < R.n_tracks; t++) {
		struct track *tr = &R.tracks[t];
		fprintf(f, "    {\"index\": %d, \"node\": ", t + 1);
		json_str(f, tr->node);
		fprintf(f, ", \"node_id\": %u, \"source\": \"%s\", \"driver_at_start\": %s, \"driver_cycles\": %" PRIu64
			", \"reported_playback_latency\": {\"quantum\": %.3f, \"frames\": %d, \"ns\": %" PRId64 "}, "
			"\"channels\": %d, \"file\": ",
			tr->src_node_id, tr->sink_monitor ? "monitor" : "output", tr->is_driver ? "true" : "false",
			tr->driver_cycles, tr->playback.max_quantum, tr->playback.max_rate, tr->playback.max_ns, tr->channels);
		json_str(f, tr->path);
		fprintf(f, ", \"frames\": %" PRIu64 "}%s\n", tr->written, t + 1 < R.n_tracks ? "," : "");
	}
	fprintf(f, "  ]\n}\n");
	fclose(f);
	printf("%s\n", path);
}

static void on_done(void *data, uint64_t count)
{
	pw_main_loop_quit(R.loop);
}

static void request_record(void)
{
	if (!atomic_exchange(&R.record_req, 1))
		fprintf(stderr, "backline-recorder: record\n");
}

static void on_signal(void *data, int sig)
{
	if (sig == SIGUSR1) {
		request_record();
		return;
	}
	if (atomic_load(&R.started))
		atomic_store(&R.stop_req, 1); /* stop on a cycle boundary */
	else {
		atomic_store(&R.finished, 1);
		pw_main_loop_quit(R.loop);
	}
}

static void on_stdin(void *data, int fd, uint32_t mask)
{
	char buf[256];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0) {
		pw_loop_destroy_source(pw_main_loop_get_loop(R.loop), R.stdin_src);
		R.stdin_src = NULL;
		return;
	}
	buf[n] = 0;
	if (strstr(buf, "record"))
		request_record();
	if (strstr(buf, "stop"))
		on_signal(NULL, SIGINT);
}

static void usage(FILE *f)
{
	fputs("usage: backline-recorder -s NODE[:CHANNELS] [-s ...] -o DIR [-n TAKE] [-f wav|wav24|flac]\n"
	      "                         [-d SECONDS] [-a signal|graph] [-P SECONDS]\n"
	      "  -s NODE[:CH]  a track: a sink's monitor, or any node's audio outputs (default 2 ch)\n"
	      "  -d SECONDS    take length (default: until SIGINT/SIGTERM or 'stop' on stdin)\n"
	      "  -a signal     line up the signal across tracks: a sink that drives the graph is one\n"
	      "                quantum late on its monitor; drop that quantum from its track (default)\n"
	      "  -a graph      frame k of every track is the same graph cycle, no compensation\n"
	      "  -P SECONDS    pre-roll: stand by and keep the last SECONDS; start on SIGUSR1 or 'record' on stdin\n",
	      f);
}

int main(int argc, char **argv)
{
	int c;
	R.node_id = SPA_ID_INVALID;
	R.align_signal = true;
	snprintf(R.dir, sizeof(R.dir), ".");
	snprintf(R.format, sizeof(R.format), "wav");
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	strftime(R.take, sizeof(R.take), "take-%Y%m%d-%H%M%S", &lt);

	while ((c = getopt(argc, argv, "s:o:n:f:d:a:P:h")) != -1) {
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
		case 'a':
			if (spa_streq(optarg, "signal")) R.align_signal = true;
			else if (spa_streq(optarg, "graph")) R.align_signal = false;
			else { usage(stderr); return 2; }
			break;
		case 'P': R.pre_roll = atof(optarg); break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (R.n_tracks == 0) { usage(stderr); return 2; }
	if (R.pre_roll < 0 || R.pre_roll > RING_SECONDS - 1) {
		fprintf(stderr, "backline-recorder: pre-roll must be 0..%d s\n", RING_SECONDS - 1);
		return 2;
	}
	mkdir(R.dir, 0755);

	pw_init(&argc, &argv);
	R.loop = pw_main_loop_new(NULL);
	struct pw_loop *l = pw_main_loop_get_loop(R.loop);
	pw_loop_add_signal(l, SIGINT, on_signal, NULL);
	pw_loop_add_signal(l, SIGTERM, on_signal, NULL);
	pw_loop_add_signal(l, SIGUSR1, on_signal, NULL);
	if (R.pre_roll > 0 || R.duration <= 0)
		R.stdin_src = pw_loop_add_io(l, 0, SPA_IO_IN | SPA_IO_HUP, false, on_stdin, NULL);
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
		tr->prev = calloc((size_t)tr->channels * 8192, sizeof(float));
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

	pw_main_loop_run(R.loop);

	atomic_store(&R.finished, 1);
	pthread_join(R.writer, NULL);
	if (atomic_load(&R.started))
		write_sidecar();
	else {
		fprintf(stderr, "backline-recorder: never started (sources missing, or no record request)\n");
		R.exit_code = 1;
	}
	if (atomic_load(&R.overruns))
		fprintf(stderr, "backline-recorder: WARNING %" PRIu64 " frames lost to overruns\n", (uint64_t)atomic_load(&R.overruns));
	if (R.pairing && (R.quantum_changes || R.driver_changes))
		fprintf(stderr, "backline-recorder: note: %u quantum change(s), %u driver change(s) during the take; "
			"alignment holds across them, but the cycle of a change may glitch on the driver's track\n",
			R.quantum_changes, R.driver_changes);

	pw_filter_destroy(R.filter);
	bl_graph_destroy(R.graph);
	pw_core_disconnect(R.core);
	pw_context_destroy(R.context);
	pw_main_loop_destroy(R.loop);
	pw_deinit();
	return R.exit_code;
}
