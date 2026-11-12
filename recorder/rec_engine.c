/* backline recorder engine (rec_engine.h).
 * SPDX-License-Identifier: MIT
 *
 * Why one node: every input port of a PipeWire node is filled in the same
 * graph cycle, so frame k of every track is the same graph sample. Two
 * separate recorders start on different cycles and can never be aligned
 * after the fact without a reference (docs/proofs/recorder-proof.txt).
 *
 * Threads:
 *   main  PipeWire main loop: linking, arming, latency, stop, sidecar.
 *   data  the process callback: copies each cycle into one lock-free ring per
 *         track. Never blocks, never allocates. Either every track gets a
 *         cycle or none does, so a full ring can't skew them.
 *   disk  the writer: drains the rings to libsndfile. A slow or stalled disk
 *         only fills the rings; with the default 30 s of ring per track the
 *         disk may stall for that long without losing a frame.
 *
 * Input-latency compensation. A capture port reports its capture latency
 * (SPA_PARAM_Latency, output direction: PipeWire's and JACK's convention): a sample that reaches the graph at
 * frame f was at the converter at frame f - L. Track i (latency L_i) is
 * written from ring frame begin + L_i, so frame j of every track is the same
 * moment at the inputs. The take runs L_max frames past the stop so every
 * track has the same length, and its timestamps are the inputs' time.
 */
#define _GNU_SOURCE
#include "rec_engine.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/filter.h>
#include <sndfile.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/string.h>

#include "bl_graph.h"
#include "rec_align.h"

#define CYCLE_MAX 8192
#define RING_MAX_BYTES (1u << 30)

struct track {
	struct rec_source src;
	int channels;
	void *ports[REC_MAX_CH];
	struct spa_ringbuffer ring;
	float *ring_data;
	uint32_t ring_size;             /* bytes, power of two */
	float *scratch;                 /* CYCLE_MAX x channels, data thread */
	float *wbuf;                    /* CYCLE_MAX x channels, disk thread */
	SNDFILE *sf;
	char path[1024];
	char container[8];              /* wav, wavex, rf64, flac */
	uint64_t written;               /* frames written to the file */
	uint64_t consumed;              /* frames taken from the ring (disk thread) */
	atomic_uint peak_bits;          /* data -> main: float bits of the peak */

	/* main, before arming */
	uint32_t src_node_id;
	bool sink_monitor;
	bool physical;
	struct bl_latency playback;
	uint32_t playback_ports[REC_MAX_CH];
	int n_playback_ports;
	uint32_t src_ports[REC_MAX_CH];
	int n_src_ports;
	int64_t latency;                /* compensation applied, frames */
	char latency_from[16];          /* off, reported, manual */
	int64_t reported_capture;       /* reported capture latency (frames), -1 unknown */

	/* data thread */
	float *prev;                    /* previous cycle, channels x CYCLE_MAX */
	bool is_driver;
	uint64_t driver_cycles;
};

struct start_info {
	uint64_t position, cycle_nsec, real_nsec;
	uint64_t trigger_position, trigger_nsec, trigger_real_nsec;
	uint32_t rate, quantum, clock_id;
	uint64_t pre_roll_frames;
};

struct rec_session {
	struct pw_core *core;
	struct pw_loop *loop;
	struct pw_filter *filter;
	struct spa_hook filter_listener;
	struct bl_graph *graph;
	struct spa_source *done_event, *writer_event, *tick;
	uint32_t node_id;
	const struct rec_events *events;
	void *data;

	struct track tracks[REC_MAX_TRACKS];
	int n_tracks;
	struct rec_options o;
	enum rec_state state;
	uint64_t pre_roll_frames;
	uint64_t lat_max;
	uint64_t arm_started_ns;

	atomic_int armed, record_req, stop_req, started, finished, writer_done;
	int skip_cycles;
	atomic_uint_fast64_t ring_frames;
	uint64_t take_begin;
	uint64_t target_frames;            /* data: ring frames needed (0 = until stopped) */
	bool pairing, prev_valid;
	uint32_t prev_n;
	uint64_t prev_position, prev_nsec;
	uint32_t last_driver, quantum_changes, driver_changes, q_min, q_max;
	atomic_uint_fast64_t overruns, discarded;
	uint64_t next_position;            /* data: the graph position the next cycle should have */
	uint64_t gap_frames, gaps;         /* data: cycles this node was not run for (graph xruns) */
	uint32_t gap_driver;
	struct start_info start;
	struct rec_align_track align[REC_MAX_TRACKS];
	atomic_uint rate_seen, quantum_seen;
	pthread_t writer;
	bool writer_running;

	struct rec_marker markers[REC_MAX_MARKERS];
	int n_markers;
	char sidecar[1100];
};

static uint64_t ts_ns(clockid_t c)
{
	struct timespec ts;
	clock_gettime(c, &ts);
	return (uint64_t)ts.tv_sec * SPA_NSEC_PER_SEC + ts.tv_nsec;
}

void rec_options_defaults(struct rec_options *o)
{
	memset(o, 0, sizeof(*o));
	snprintf(o->dir, sizeof(o->dir), ".");
	snprintf(o->format, sizeof(o->format), "wav");
	o->align_signal = true;
	o->ring_seconds = 30;
	o->latency = REC_LATENCY_HARDWARE;
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	strftime(o->take, sizeof(o->take), "take-%Y%m%d-%H%M%S", &lt);
}

/* ------------------------------------------------------------ data thread */
static void start_take(struct rec_session *s, struct spa_io_position *pos, uint32_t n)
{
	uint64_t mono = ts_ns(CLOCK_MONOTONIC), real = ts_ns(CLOCK_REALTIME);
	uint32_t rate = pos->clock.rate.denom;
	uint64_t now_frames = atomic_load(&s->ring_frames);
	uint64_t oldest = atomic_load(&s->discarded);
	uint64_t begin = now_frames > s->pre_roll_frames ? now_frames - s->pre_roll_frames : 0;
	if (begin < oldest)
		begin = oldest;

	uint64_t cyc_pos = s->pairing ? s->prev_position : pos->clock.position;
	uint64_t cyc_nsec = s->pairing ? s->prev_nsec : pos->clock.nsec;
	for (int t = 0; t < s->n_tracks; t++)
		s->tracks[t].is_driver = s->tracks[t].src_node_id == pos->clock.id;
	s->start.trigger_position = cyc_pos;
	s->start.trigger_nsec = cyc_nsec;
	s->start.trigger_real_nsec = real - (mono - cyc_nsec);
	s->start.pre_roll_frames = now_frames - begin;
	uint64_t back_ns = s->start.pre_roll_frames * SPA_NSEC_PER_SEC / rate;
	s->start.position = cyc_pos - s->start.pre_roll_frames;
	s->start.cycle_nsec = cyc_nsec - back_ns;
	s->start.real_nsec = s->start.trigger_real_nsec - back_ns;
	s->start.rate = rate;
	s->start.quantum = n;
	s->start.clock_id = pos->clock.id;
	s->take_begin = begin;
	s->q_min = s->q_max = n;
	s->last_driver = pos->clock.id;
	if (s->o.duration > 0)
		s->target_frames = begin + (uint64_t)(s->o.duration * rate + 0.5) + s->lat_max;
	atomic_store(&s->started, 1);
	pw_loop_signal_event(s->loop, s->done_event);
}

static void keep_previous(struct rec_session *s, struct spa_io_position *pos,
		float *const cur[REC_MAX_TRACKS][REC_MAX_CH], uint32_t n)
{
	if (!s->pairing)
		return;
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		for (int c = 0; c < tr->channels; c++) {
			float *dst = tr->prev + (size_t)c * CYCLE_MAX;
			if (cur[t][c])
				memcpy(dst, cur[t][c], n * sizeof(float));
			else
				memset(dst, 0, n * sizeof(float));
		}
	}
	s->prev_n = n;
	s->prev_position = pos->clock.position;
	s->prev_nsec = pos->clock.nsec;
	s->prev_valid = true;
}

static void on_process(void *data, struct spa_io_position *pos)
{
	struct rec_session *s = data;
	uint32_t n = SPA_MIN(pos->clock.duration, (uint64_t)CYCLE_MAX);
	atomic_store_explicit(&s->rate_seen, pos->clock.rate.denom, memory_order_relaxed);
	atomic_store_explicit(&s->quantum_seen, n, memory_order_relaxed);
	if (!atomic_load(&s->armed) || atomic_load(&s->finished))
		return;

	/* a cycle the graph didn't run this node for: its input is gone before
	 * the recorder could see it (a scheduling xrun, not the disk) */
	/* (only under the same driver: a new driver brings its own position) */
	if (atomic_load(&s->started) && s->next_position && pos->clock.id == s->gap_driver &&
	    pos->clock.position > s->next_position) {
		s->gap_frames += pos->clock.position - s->next_position;
		s->gaps++;
	}
	s->next_position = pos->clock.position + pos->clock.duration;
	s->gap_driver = pos->clock.id;

	float *cur[REC_MAX_TRACKS][REC_MAX_CH];
	for (int t = 0; t < s->n_tracks; t++)
		for (int c = 0; c < s->tracks[t].channels; c++)
			cur[t][c] = pw_filter_get_dsp_buffer(s->tracks[t].ports[c], pos->clock.duration);

	if (s->skip_cycles > 0 || (s->pairing && !s->prev_valid)) {
		if (s->skip_cycles > 0)
			s->skip_cycles--;
		keep_previous(s, pos, cur, n);
		return;
	}
	bool started = atomic_load(&s->started);
	if (!started && ((!s->o.wait_record && s->pre_roll_frames == 0) || atomic_load(&s->record_req))) {
		start_take(s, pos, n);
		started = true;
	}
	bool use_cur[REC_MAX_TRACKS];
	rec_align_select(s->align, s->n_tracks, s->pairing, pos->clock.id, use_cur);
	uint32_t w = s->pairing ? s->prev_n : n;
	if (started) {
		s->q_min = SPA_MIN(s->q_min, n);
		s->q_max = SPA_MAX(s->q_max, n);
		if (s->pairing && n != s->prev_n)
			s->quantum_changes++;
		if (pos->clock.id != s->last_driver) {
			s->driver_changes++;
			s->last_driver = pos->clock.id;
		}
		for (int t = 0; t < s->n_tracks; t++)
			s->tracks[t].driver_cycles += s->tracks[t].src_node_id == pos->clock.id;
		/* stop: run on for the largest latency so every track ends together */
		if (atomic_load(&s->stop_req) && s->target_frames == 0)
			s->target_frames = atomic_load(&s->ring_frames) + s->lat_max;
	}
	uint64_t have = atomic_load(&s->ring_frames);
	if (started && s->target_frames && have + w > s->target_frames)
		w = (uint32_t)(s->target_frames - have);

	bool room = true;
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		uint32_t idx;
		int32_t filled = spa_ringbuffer_get_write_index(&tr->ring, &idx);
		if ((uint64_t)(uint32_t)filled + (uint64_t)w * tr->channels * sizeof(float) > tr->ring_size)
			room = false;
	}
	if (!room) {
		atomic_fetch_add(&s->overruns, w);
	} else if (w > 0) {
		for (int t = 0; t < s->n_tracks; t++) {
			struct track *tr = &s->tracks[t];
			uint32_t avail = use_cur[t] ? n : s->prev_n;
			float peak = 0.0f;
			for (int c = 0; c < tr->channels; c++) {
				const float *in = use_cur[t] ? cur[t][c] : tr->prev + (size_t)c * CYCLE_MAX;
				for (uint32_t i = 0; i < w; i++) {
					float v = in && i < avail ? in[i] : 0.0f;
					tr->scratch[i * tr->channels + c] = v;
					float a = fabsf(v);
					if (a > peak)
						peak = a;
				}
			}
			union { float f; unsigned u; } pk, old;
			old.u = atomic_load_explicit(&tr->peak_bits, memory_order_relaxed);
			pk.f = peak;
			if (peak > old.f)
				atomic_store_explicit(&tr->peak_bits, pk.u, memory_order_relaxed);
			uint32_t idx, bytes = w * tr->channels * sizeof(float);
			spa_ringbuffer_get_write_index(&tr->ring, &idx);
			spa_ringbuffer_write_data(&tr->ring, tr->ring_data, tr->ring_size,
					idx & (tr->ring_size - 1), tr->scratch, bytes);
			spa_ringbuffer_write_update(&tr->ring, idx + bytes);
		}
		atomic_fetch_add(&s->ring_frames, w);
	}
	keep_previous(s, pos, cur, n);
	bool stop_now = started && s->target_frames && atomic_load(&s->ring_frames) >= s->target_frames;
	if (stop_now) {
		atomic_store(&s->finished, 1);
		pw_loop_signal_event(s->loop, s->done_event);
	}
}

static void on_state_changed(void *data, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	struct rec_session *s = data;
	if (state == PW_FILTER_STATE_ERROR)
		pw_log_warn("backline recorder: filter error: %s", error ? error : "?");
	if (state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING)
		s->node_id = pw_filter_get_node_id(s->filter);
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = on_state_changed,
	.process = on_process,
};

/* ------------------------------------------------------------ disk thread */
static void safe_name(const char *in, char *out, size_t n)
{
	snprintf(out, n, "%s", in);
	for (char *p = out; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.'))
			*p = '_';
}

static void open_files(struct rec_session *s)
{
	time_t secs = (time_t)(s->start.real_nsec / SPA_NSEC_PER_SEC);
	struct tm lt;
	localtime_r(&secs, &lt);
	uint64_t midnight_ns = (uint64_t)(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec) * SPA_NSEC_PER_SEC +
		s->start.real_nsec % SPA_NSEC_PER_SEC;
	uint64_t tref = (uint64_t)((double)midnight_ns * s->start.rate / 1e9 + 0.5);

	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		SF_INFO info = { .samplerate = (int)s->start.rate, .channels = tr->channels };
		const char *ext = "wav";
		bool rf64 = false;
		if (spa_streq(s->o.format, "flac")) {
			info.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
			ext = "flac";
		} else if (spa_streq(s->o.format, "wav24")) {
			info.format = SF_FORMAT_RF64 | SF_FORMAT_PCM_24;
			rf64 = true;
		} else {
			info.format = SF_FORMAT_RF64 | SF_FORMAT_FLOAT;
			rf64 = true;
		}
		char safe[256];
		safe_name(tr->src.node, safe, sizeof(safe));
		snprintf(tr->path, sizeof(tr->path), "%s/%s-%02d-%s.%s", s->o.dir, s->o.take, t + 1, safe, ext);
		tr->sf = sf_open(tr->path, SFM_WRITE, &info);
		if (!tr->sf) {
			pw_log_warn("backline recorder: %s: %s", tr->path, sf_strerror(NULL));
			continue;
		}
		/* RF64 that writes a plain (extensible) WAV with a reserved JUNK
		 * chunk, and becomes RF64 only if the file passes 4 GB (EBU 3306). */
		if (rf64)
			sf_command(tr->sf, SFC_RF64_AUTO_DOWNGRADE, NULL, SF_TRUE);
		if (!spa_streq(ext, "flac")) {
			SF_BROADCAST_INFO bi;
			memset(&bi, 0, sizeof(bi));
			snprintf(bi.description, sizeof(bi.description),
				"backline take=%s track=%d node=%s graph_pos=%" PRIu64 " clock_id=%u align=%s latency=%" PRId64,
				s->o.take, t + 1, tr->src.node, s->start.position, s->start.clock_id,
				s->pairing ? "signal" : "graph", tr->latency);
			snprintf(bi.originator, sizeof(bi.originator), "Backline recorder 0.3");
			snprintf(bi.originator_reference, sizeof(bi.originator_reference), "%.31s", s->o.take);
			char tmp[32];
			strftime(tmp, sizeof(tmp), "%Y-%m-%d", &lt);
			memcpy(bi.origination_date, tmp, sizeof(bi.origination_date));
			strftime(tmp, sizeof(tmp), "%H:%M:%S", &lt);
			memcpy(bi.origination_time, tmp, sizeof(bi.origination_time));
			bi.time_reference_low = (uint32_t)tref;
			bi.time_reference_high = (uint32_t)(tref >> 32);
			bi.version = 1;
			bi.coding_history_size = (uint32_t)snprintf(bi.coding_history, sizeof(bi.coding_history),
				"A=PCM,F=%u,W=%s,M=%s,T=Backline recorder\r\n", s->start.rate,
				spa_streq(s->o.format, "wav24") ? "24" : "32",
				tr->channels == 1 ? "mono" : tr->channels == 2 ? "stereo" : "multi");
			if (sf_command(tr->sf, SFC_SET_BROADCAST_INFO, &bi, sizeof(bi)) != SF_TRUE)
				pw_log_warn("backline recorder: could not write bext to %s", tr->path);
		} else {
			char c[256];
			snprintf(c, sizeof(c), "BACKLINE_TIME_REFERENCE=%" PRIu64 " BACKLINE_GRAPH_POSITION=%" PRIu64, tref, s->start.position);
			sf_set_string(tr->sf, SF_STR_COMMENT, c);
		}
	}
}

static void take_frames(struct track *tr, uint64_t frames, bool write)
{
	const uint32_t fsz = tr->channels * sizeof(float);
	while (frames > 0) {
		uint32_t idx;
		int32_t avail = spa_ringbuffer_get_read_index(&tr->ring, &idx);
		uint32_t chunk = (uint32_t)SPA_MIN((uint64_t)((uint32_t)avail / fsz), SPA_MIN(frames, (uint64_t)CYCLE_MAX));
		if (chunk == 0)
			return;
		uint32_t bytes = chunk * fsz;
		if (write)
			spa_ringbuffer_read_data(&tr->ring, tr->ring_data, tr->ring_size, idx & (tr->ring_size - 1), tr->wbuf, bytes);
		spa_ringbuffer_read_update(&tr->ring, idx + bytes);
		tr->consumed += chunk;
		if (write) {
			if (tr->sf)
				sf_writef_float(tr->sf, tr->wbuf, chunk);
			tr->written += chunk;
		}
		frames -= chunk;
	}
}

static void discard_standby(struct rec_session *s)
{
	uint64_t have = atomic_load(&s->ring_frames);
	uint64_t gone = atomic_load(&s->discarded);
	uint64_t keep = s->pre_roll_frames + s->lat_max + CYCLE_MAX;
	if (have <= gone + keep)
		return;
	uint64_t drop = have - gone - keep;
	for (int t = 0; t < s->n_tracks; t++)
		take_frames(&s->tracks[t], drop, false);
	atomic_store(&s->discarded, gone + drop);
}

/* Track t carries ring frames [begin + L_t, begin + L_t + len), with len =
 * frames so far - begin - L_max: the same length on every track. */
static void drain(struct rec_session *s)
{
	uint64_t have = atomic_load(&s->ring_frames);
	uint64_t len = have > s->take_begin + s->lat_max ? have - s->take_begin - s->lat_max : 0;
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		uint64_t first = s->take_begin + (uint64_t)tr->latency;
		if (tr->consumed < first)
			take_frames(tr, first - tr->consumed, false);
		if (tr->written < len)
			take_frames(tr, len - tr->written, true);
	}
}

static void *writer_main(void *arg)
{
	struct rec_session *s = arg;
	struct timespec nap = { 0, 10 * 1000000L };
	while (!atomic_load(&s->started) && !atomic_load(&s->finished)) {
		if (atomic_load(&s->armed))
			discard_standby(s);
		nanosleep(&nap, NULL);
	}
	if (atomic_load(&s->started))
		open_files(s);
	while (!atomic_load(&s->finished)) {
		drain(s);
		nanosleep(&nap, NULL);
	}
	if (atomic_load(&s->started))
		drain(s);
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		if (!tr->sf)
			continue;
		sf_close(tr->sf);
		tr->sf = NULL;
		/* what libsndfile wrote: RF64 only past 4 GB */
		char hdr[4] = { 0 };
		int fd = open(tr->path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			if (read(fd, hdr, 4) == 4)
				snprintf(tr->container, sizeof(tr->container), "%s",
					!memcmp(hdr, "RF64", 4) ? "rf64" : !memcmp(hdr, "RIFF", 4) ? "wav" : "flac");
			close(fd);
		}
	}
	atomic_store(&s->writer_done, 1);
	pw_loop_signal_event(s->loop, s->writer_event);
	return NULL;
}

/* ------------------------------------------------------------ main thread */
static int count_links_needed(struct rec_session *s)
{
	int n = 0;
	for (int t = 0; t < s->n_tracks; t++)
		n += s->tracks[t].channels;
	return n;
}

struct match_ctx { uint32_t node; bool monitor; bool want_playback; int found; uint32_t ids[REC_MAX_CH]; uint32_t idx[REC_MAX_CH]; bool physical; };

static int match_src(void *data, const struct bl_port *p)
{
	struct match_ctx *m = data;
	if (p->node_id != m->node || p->midi)
		return 0;
	if (m->want_playback) {
		if (p->output || p->monitor)
			return 0;
	} else if (!p->output || p->monitor != m->monitor) {
		return 0;
	}
	if (m->found < REC_MAX_CH) {
		/* in port order (port.id), not registry order: the registry
		 * announces a node's ports in no promised order, and channel c
		 * of the track must be the node's channel c */
		int k = m->found++;
		while (k > 0 && m->idx[k - 1] > p->index) {
			m->ids[k] = m->ids[k - 1];
			m->idx[k] = m->idx[k - 1];
			k--;
		}
		m->ids[k] = p->id;
		m->idx[k] = p->index;
		m->physical |= p->physical;
	}
	return 0;
}

struct own_ctx { uint32_t node; const char *name; uint32_t id; };
static int match_own(void *data, const struct bl_port *p)
{
	struct own_ctx *o = data;
	if (p->node_id == o->node && spa_streq(p->name, o->name)) {
		o->id = p->id;
		return 1;
	}
	return 0;
}

static bool wants_reported_latency(struct rec_session *s, struct track *tr)
{
	if (tr->src.latency_frames >= 0 || tr->sink_monitor)
		return false;
	return s->o.latency == REC_LATENCY_REPORTED || (s->o.latency == REC_LATENCY_HARDWARE && tr->physical);
}

static void reconcile(struct rec_session *s)
{
	if (s->node_id == SPA_ID_INVALID)
		return;
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		const struct bl_node *n = bl_graph_find_node(s->graph, tr->src.node);
		if (!n)
			continue;
		tr->src_node_id = n->id;
		tr->sink_monitor = strstr(n->media_class, "Sink") != NULL;
		struct match_ctx m = { n->id, tr->sink_monitor, false, 0, { 0 }, { 0 }, false };
		bl_graph_for_each_port(s->graph, match_src, &m);
		if (m.found == 0 && !tr->sink_monitor) {
			/* a virtual source (null sink as Audio/Source/Virtual, what
			 * pactl's module-null-sink makes) has only monitor outputs */
			m.monitor = true;
			bl_graph_for_each_port(s->graph, match_src, &m);
		}
		tr->n_src_ports = m.found;
		memcpy(tr->src_ports, m.ids, sizeof(m.ids));
		tr->physical = m.physical;
		if (!tr->sink_monitor)
			for (int k = 0; k < m.found; k++)
				bl_graph_watch_latency(s->graph, m.ids[k]);
		if (tr->sink_monitor) {
			struct match_ctx pb = { n->id, false, true, 0, { 0 }, { 0 }, false };
			bl_graph_for_each_port(s->graph, match_src, &pb);
			tr->n_playback_ports = pb.found;
			for (int k = 0; k < pb.found; k++) {
				tr->playback_ports[k] = pb.ids[k];
				bl_graph_watch_latency(s->graph, pb.ids[k]);
			}
		}
		for (int c = 0; c < tr->channels; c++) {
			char name[64];
			snprintf(name, sizeof(name), "t%d_%d", t + 1, c + 1);
			struct own_ctx o = { s->node_id, name, SPA_ID_INVALID };
			bl_graph_for_each_port(s->graph, match_own, &o);
			if (o.id == SPA_ID_INVALID || m.found == 0)
				continue;
			uint32_t src = m.ids[SPA_MIN(c, m.found - 1)];
			if (!bl_graph_is_linked(s->graph, src, o.id))
				bl_graph_link(s->graph, src, o.id);
		}
	}
}

/* Sinks' playback latency (logged only) and sources' capture latency. Returns
 * true once every port whose capture latency is needed has answered. */
static bool latencies_known(struct rec_session *s, uint32_t quantum, uint32_t rate)
{
	bool all = true;
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		if (tr->sink_monitor) {
			struct bl_latency best = { 0 };
			for (int k = 0; k < tr->n_playback_ports; k++) {
				const struct bl_port *p = bl_graph_port(s->graph, tr->playback_ports[k]);
				if (!p || !p->latency_known)
					continue;
				const struct bl_latency *l = &p->latency[BL_LATENCY_PLAYBACK];
				if (l->valid && (!best.valid || l->max_quantum > best.max_quantum ||
						 l->max_rate > best.max_rate || l->max_ns > best.max_ns))
					best = *l;
			}
			tr->playback = best;
			continue;
		}
		int64_t cap = -1;
		bool known = true;
		for (int k = 0; k < tr->n_src_ports; k++) {
			const struct bl_port *p = bl_graph_port(s->graph, tr->src_ports[k]);
			if (!p || !p->latency_known) {
				known = false;
				continue;
			}
			int64_t f = p->latency[BL_LATENCY_CAPTURE].valid ? bl_latency_frames(&p->latency[BL_LATENCY_CAPTURE], quantum, rate) : 0;
			if (f > cap)
				cap = f;
		}
		tr->reported_capture = known ? SPA_MAX(cap, (int64_t)0) : -1;
		if (!known && wants_reported_latency(s, tr))
			all = false;
	}
	return all;
}

static void on_port_added(void *data, const struct bl_port *p)
{
	struct rec_session *s = data;
	if (s->node_id == SPA_ID_INVALID && s->filter)
		s->node_id = pw_filter_get_node_id(s->filter);
	reconcile(s);
}

static const struct bl_graph_events graph_events = { .port_added = on_port_added };

static int alloc_rings(struct rec_session *s, uint32_t rate)
{
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		uint64_t want = (uint64_t)(s->o.ring_seconds * rate) * tr->channels * sizeof(float);
		uint32_t size = 1;
		while (size < want && size < RING_MAX_BYTES)
			size <<= 1;
		float *d = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
		if (d == MAP_FAILED)
			return -errno;
		mlock(d, size); /* best effort: RLIMIT_MEMLOCK is often small */
		tr->ring_data = d;
		tr->ring_size = size;
		spa_ringbuffer_init(&tr->ring);
	}
	return 0;
}

static void on_tick(void *data, uint64_t expirations)
{
	struct rec_session *s = data;
	reconcile(s);
	if (atomic_load(&s->armed))
		return;
	uint32_t rate = atomic_load(&s->rate_seen);
	if (bl_graph_links_ready(s->graph) < count_links_needed(s) || rate == 0)
		return;
	uint32_t quantum = atomic_load(&s->quantum_seen);
	if (quantum == 0)
		quantum = 1024;
	/* capture latency: wait up to 1 s for the ports to answer */
	bool lat_ok = latencies_known(s, quantum, rate);
	if (!lat_ok && ts_ns(CLOCK_MONOTONIC) - s->arm_started_ns < SPA_NSEC_PER_SEC)
		return;
	s->lat_max = 0;
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		if (tr->src.latency_frames >= 0) {
			tr->latency = tr->src.latency_frames;
			snprintf(tr->latency_from, sizeof(tr->latency_from), "manual");
		} else if (wants_reported_latency(s, tr) && tr->reported_capture >= 0) {
			tr->latency = tr->reported_capture;
			snprintf(tr->latency_from, sizeof(tr->latency_from), "reported");
		} else {
			tr->latency = 0;
			snprintf(tr->latency_from, sizeof(tr->latency_from), "off");
		}
		s->lat_max = SPA_MAX(s->lat_max, (uint64_t)tr->latency);
	}
	if (alloc_rings(s, rate) < 0) {
		pw_log_warn("backline recorder: cannot allocate rings");
		return;
	}
	s->pre_roll_frames = (uint64_t)(s->o.pre_roll * rate + 0.5);
	for (int t = 0; t < s->n_tracks; t++) {
		s->align[t].sink_monitor = s->tracks[t].sink_monitor;
		s->align[t].src_node_id = s->tracks[t].src_node_id;
	}
	s->pairing = rec_align_pairing(s->align, s->n_tracks, s->o.align_signal);
	s->skip_cycles = 2;
	pthread_create(&s->writer, NULL, writer_main, s);
	s->writer_running = true;
	atomic_store(&s->armed, 1);
	s->state = REC_ARMED;
	if (s->events && s->events->armed)
		s->events->armed(s->data);
}

static void json_str(FILE *f, const char *str)
{
	fputc('"', f);
	for (; *str; str++) {
		unsigned char c = (unsigned char)*str;
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

static const char *latency_mode_name(enum rec_latency_mode m)
{
	return m == REC_LATENCY_OFF ? "off" : m == REC_LATENCY_REPORTED ? "reported" : "hardware";
}

static void write_sidecar(struct rec_session *s)
{
	snprintf(s->sidecar, sizeof(s->sidecar), "%s/%s.json", s->o.dir, s->o.take);
	char tmp[1200];
	snprintf(tmp, sizeof(tmp), "%s.tmp", s->sidecar);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		s->sidecar[0] = 0;
		return;
	}
	latencies_known(s, s->start.quantum, s->start.rate);
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		fprintf(stderr, "backline-recorder: track %d: %s %s, node %u, drove %" PRIu64 " cycles of the take; "
			"reported playback latency %.1f quantum + %d frames + %" PRId64 " ns; capture latency compensation %" PRId64 " frames (%s)\n",
			t + 1, tr->src.node, tr->sink_monitor ? "monitor" : "output", tr->src_node_id, tr->driver_cycles,
			tr->playback.max_quantum, tr->playback.max_rate, tr->playback.max_ns, tr->latency, tr->latency_from);
	}
	char utc[96], trig[96];
	iso_utc(s->start.real_nsec, utc, sizeof(utc));
	iso_utc(s->start.trigger_real_nsec, trig, sizeof(trig));
	uint64_t frames = s->tracks[0].written;
	fprintf(f, "{\n  \"schema\": \"backline.take/2\",\n  \"take\": ");
	json_str(f, s->o.take);
	fprintf(f, ",\n  \"start\": {\n    \"utc\": \"%s\",\n    \"realtime_ns\": %" PRIu64 ",\n"
		"    \"monotonic_ns\": %" PRIu64 ",\n    \"graph_position\": %" PRIu64 ",\n    \"clock_id\": %u\n  },\n",
		utc, s->start.real_nsec, s->start.cycle_nsec, s->start.position, s->start.clock_id);
	fprintf(f, "  \"trigger\": {\n    \"utc\": \"%s\",\n    \"monotonic_ns\": %" PRIu64 ",\n    \"graph_position\": %" PRIu64
		",\n    \"pre_roll_frames\": %" PRIu64 ",\n    \"pre_roll_requested_frames\": %" PRIu64 "\n  },\n",
		trig, s->start.trigger_nsec, s->start.trigger_position, s->start.pre_roll_frames, s->pre_roll_frames);
	fprintf(f, "  \"rate\": %u,\n  \"quantum_at_start\": %u,\n  \"quantum_range\": [%u, %u],\n  \"frames\": %" PRIu64
		",\n  \"overrun_frames\": %" PRIu64 ",\n  \"graph_gap_frames\": %" PRIu64 ",\n  \"graph_gaps\": %" PRIu64
		",\n  \"format\": \"%s\",\n  \"ring_seconds\": %.1f,\n",
		s->start.rate, s->start.quantum, s->q_min, s->q_max, frames, (uint64_t)atomic_load(&s->overruns), s->gap_frames, s->gaps,
		s->o.format, s->o.ring_seconds);
	fprintf(f, "  \"alignment\": {\n    \"mode\": \"%s\",\n    \"one_cycle_pairing\": %s,\n    \"driver_at_start\": %u,\n"
		"    \"driver_changes\": %u,\n    \"quantum_changes\": %u\n  },\n",
		s->o.align_signal ? "signal" : "graph", s->pairing ? "true" : "false", s->start.clock_id, s->driver_changes,
		s->quantum_changes);
	fprintf(f, "  \"latency_compensation\": {\n    \"mode\": \"%s\",\n    \"max_frames\": %" PRIu64 "\n  },\n",
		latency_mode_name(s->o.latency), s->lat_max);
	fprintf(f, "  \"markers\": [");
	for (int i = 0; i < s->n_markers; i++) {
		char mu[96];
		iso_utc(s->markers[i].realtime_ns, mu, sizeof(mu));
		fprintf(f, "%s\n    {\"name\": ", i ? "," : "");
		json_str(f, s->markers[i].name);
		fprintf(f, ", \"frame\": %" PRIu64 ", \"utc\": \"%s\"}", s->markers[i].frame, mu);
	}
	fprintf(f, "%s],\n  \"tracks\": [\n", s->n_markers ? "\n  " : "");
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		fprintf(f, "    {\"index\": %d, \"node\": ", t + 1);
		json_str(f, tr->src.node);
		fprintf(f, ", \"node_id\": %u, \"source\": \"%s\", \"physical\": %s, \"driver_at_start\": %s, \"driver_cycles\": %" PRIu64
			", \"reported_playback_latency\": {\"quantum\": %.3f, \"frames\": %d, \"ns\": %" PRId64 "}, "
			"\"reported_capture_latency_frames\": %" PRId64 ", \"capture_latency_frames\": %" PRId64
			", \"capture_latency_from\": \"%s\", \"channels\": %d, \"container\": \"%s\", \"file\": ",
			tr->src_node_id, tr->sink_monitor ? "monitor" : "output", tr->physical ? "true" : "false",
			tr->is_driver ? "true" : "false", tr->driver_cycles, tr->playback.max_quantum, tr->playback.max_rate,
			tr->playback.max_ns, tr->reported_capture, tr->latency, tr->latency_from, tr->channels,
			tr->container[0] ? tr->container : "wav");
		json_str(f, tr->path);
		fprintf(f, ", \"frames\": %" PRIu64 "}%s\n", tr->written, t + 1 < s->n_tracks ? "," : "");
	}
	fprintf(f, "  ]\n}\n");
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	rename(tmp, s->sidecar);
}

static void on_writer_done(void *data, uint64_t count)
{
	struct rec_session *s = data;
	if (s->writer_running) {
		pthread_join(s->writer, NULL);
		s->writer_running = false;
	}
	if (atomic_load(&s->started)) {
		if (s->n_markers && !spa_streq(s->o.format, "flac"))
			for (int t = 0; t < s->n_tracks; t++)
				if (s->tracks[t].path[0])
					rec_write_cues(s->tracks[t].path, s->markers, s->n_markers);
		write_sidecar(s);
	}
	s->state = REC_DONE;
	if (s->events && s->events->finished)
		s->events->finished(s->data, atomic_load(&s->started) && s->sidecar[0] ? s->sidecar : NULL);
}

static void on_done(void *data, uint64_t count)
{
	struct rec_session *s = data;
	if (atomic_load(&s->started) && s->state == REC_ARMED) {
		s->state = REC_RECORDING;
		if (s->events && s->events->started)
			s->events->started(s->data);
	}
	if (atomic_load(&s->finished) && s->state != REC_DONE)
		s->state = REC_FINISHING;
}

struct rec_session *rec_session_new(struct pw_core *core, struct pw_loop *loop,
		const struct rec_source *sources, int n_sources, const struct rec_options *opts,
		const struct rec_events *events, void *data, char *err, size_t err_size)
{
	if (n_sources < 1 || n_sources > REC_MAX_TRACKS) {
		snprintf(err, err_size, "need 1 to %d sources", REC_MAX_TRACKS);
		return NULL;
	}
	if (opts->ring_seconds < 2 || opts->ring_seconds > 120) {
		snprintf(err, err_size, "ring-seconds must be 2 to 120");
		return NULL;
	}
	if (opts->pre_roll < 0 || opts->pre_roll > opts->ring_seconds - 1) {
		snprintf(err, err_size, "pre-roll must be 0 to %.0f s (ring-seconds - 1)", opts->ring_seconds - 1);
		return NULL;
	}
	if (!spa_streq(opts->format, "wav") && !spa_streq(opts->format, "wav24") && !spa_streq(opts->format, "flac")) {
		snprintf(err, err_size, "format must be wav, wav24 or flac");
		return NULL;
	}
	struct rec_session *s = calloc(1, sizeof(*s));
	if (!s) {
		snprintf(err, err_size, "out of memory");
		return NULL;
	}
	s->core = core;
	s->loop = loop;
	s->o = *opts;
	s->events = events;
	s->data = data;
	s->node_id = SPA_ID_INVALID;
	s->n_tracks = n_sources;
	s->state = REC_ARMING;
	s->arm_started_ns = ts_ns(CLOCK_MONOTONIC);
	if (mkdir(s->o.dir, 0755) < 0 && errno != EEXIST) {
		snprintf(err, err_size, "%s: %s", s->o.dir, strerror(errno));
		free(s);
		return NULL;
	}
	s->done_event = pw_loop_add_event(loop, on_done, s);
	s->writer_event = pw_loop_add_event(loop, on_writer_done, s);
	s->graph = bl_graph_new(core, &graph_events, s);

	s->filter = pw_filter_new(core, "backline-recorder",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Production",
			PW_KEY_NODE_NAME, "backline.recorder",
			PW_KEY_NODE_DESCRIPTION, "Backline recorder",
			PW_KEY_NODE_AUTOCONNECT, "false",
			PW_KEY_NODE_ALWAYS_PROCESS, "true",
			"backline.take", s->o.take,
			NULL));
	pw_filter_add_listener(s->filter, &s->filter_listener, &filter_events, s);
	for (int t = 0; t < n_sources; t++) {
		struct track *tr = &s->tracks[t];
		tr->src = sources[t];
		tr->channels = SPA_CLAMP(sources[t].channels, 1, REC_MAX_CH);
		tr->reported_capture = -1;
		snprintf(tr->latency_from, sizeof(tr->latency_from), "off");
		for (int ch = 0; ch < tr->channels; ch++) {
			char name[64];
			snprintf(name, sizeof(name), "t%d_%d", t + 1, ch + 1);
			tr->ports[ch] = pw_filter_add_port(s->filter, PW_DIRECTION_INPUT,
				PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
				pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
						  PW_KEY_PORT_NAME, name, NULL), NULL, 0);
		}
		tr->prev = calloc((size_t)tr->channels * CYCLE_MAX, sizeof(float));
		tr->scratch = calloc((size_t)tr->channels * CYCLE_MAX, sizeof(float));
		tr->wbuf = calloc((size_t)tr->channels * CYCLE_MAX, sizeof(float));
	}
	if (pw_filter_connect(s->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		snprintf(err, err_size, "cannot connect the recorder node");
		rec_session_destroy(s);
		return NULL;
	}
	s->tick = pw_loop_add_timer(loop, on_tick, s);
	struct timespec tick = { 0, 20 * 1000000L };
	pw_loop_update_timer(loop, s->tick, &tick, &tick, false);
	return s;
}

void rec_session_destroy(struct rec_session *s)
{
	if (!s)
		return;
	atomic_store(&s->finished, 1);
	if (s->writer_running) {
		pthread_join(s->writer, NULL);
		s->writer_running = false;
	}
	if (s->tick)
		pw_loop_destroy_source(s->loop, s->tick);
	if (s->filter)
		pw_filter_destroy(s->filter);
	bl_graph_destroy(s->graph);
	pw_loop_destroy_source(s->loop, s->done_event);
	pw_loop_destroy_source(s->loop, s->writer_event);
	for (int t = 0; t < s->n_tracks; t++) {
		struct track *tr = &s->tracks[t];
		if (tr->sf)
			sf_close(tr->sf);
		if (tr->ring_data)
			munmap(tr->ring_data, tr->ring_size);
		free(tr->prev);
		free(tr->scratch);
		free(tr->wbuf);
	}
	free(s);
}

int rec_session_record(struct rec_session *s)
{
	if (s->state != REC_ARMED)
		return -EBUSY;
	atomic_store(&s->record_req, 1);
	return 0;
}

void rec_session_stop(struct rec_session *s)
{
	if (s->state == REC_DONE || s->state == REC_FINISHING)
		return;
	if (atomic_load(&s->started)) {
		atomic_store(&s->stop_req, 1);
		s->state = REC_FINISHING;
		return;
	}
	/* not started: disarm. The writer thread ends and reports. */
	atomic_store(&s->finished, 1);
	s->state = REC_FINISHING;
	if (!s->writer_running)
		pw_loop_signal_event(s->loop, s->writer_event);
}

int rec_session_marker(struct rec_session *s, const char *name, uint64_t *frame)
{
	if (!atomic_load(&s->started) || s->state != REC_RECORDING)
		return -EBUSY;
	if (s->n_markers == REC_MAX_MARKERS)
		return -ENOSPC;
	struct rec_marker *m = &s->markers[s->n_markers++];
	uint64_t have = atomic_load(&s->ring_frames);
	m->frame = have > s->take_begin ? have - s->take_begin : 0;
	m->realtime_ns = ts_ns(CLOCK_REALTIME);
	snprintf(m->name, sizeof(m->name), "%s", name && *name ? name : "marker");
	if (frame)
		*frame = m->frame;
	return 0;
}

enum rec_state rec_session_state(const struct rec_session *s) { return s->state; }
const char *rec_session_take(const struct rec_session *s) { return s->o.take; }
uint32_t rec_session_rate(const struct rec_session *s)
{
	return s->start.rate ? s->start.rate : atomic_load(&((struct rec_session *)s)->rate_seen);
}
uint64_t rec_session_frames(const struct rec_session *s)
{
	uint64_t have = atomic_load(&((struct rec_session *)s)->ring_frames);
	if (!atomic_load(&((struct rec_session *)s)->started))
		return 0;
	return have > s->take_begin + s->lat_max ? have - s->take_begin - s->lat_max : 0;
}
uint64_t rec_session_overruns(const struct rec_session *s) { return atomic_load(&((struct rec_session *)s)->overruns); }
int rec_session_n_tracks(const struct rec_session *s) { return s->n_tracks; }
const char *rec_session_track_node(const struct rec_session *s, int t) { return s->tracks[t].src.node; }
int64_t rec_session_track_latency(const struct rec_session *s, int t) { return s->tracks[t].latency; }
uint32_t rec_session_node_id(const struct rec_session *s) { return s->node_id; }

void rec_session_take_peaks(struct rec_session *s, double *out, int n)
{
	for (int t = 0; t < n && t < s->n_tracks; t++) {
		union { float f; unsigned u; } v;
		v.u = atomic_exchange_explicit(&s->tracks[t].peak_bits, 0u, memory_order_relaxed);
		out[t] = v.f > 1e-10f ? 20.0 * log10(v.f) : -200.0;
	}
}

/* ------------------------------------------------------------- cue chunks */
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

int rec_write_cues(const char *path, const struct rec_marker *m, int n)
{
	if (n <= 0)
		return 0;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	uint8_t hdr[48];
	struct stat st;
	if (pread(fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr) || fstat(fd, &st) < 0) {
		close(fd);
		return -EIO;
	}
	bool rf64 = !memcmp(hdr, "RF64", 4);
	if ((!rf64 && memcmp(hdr, "RIFF", 4)) || memcmp(hdr + 8, "WAVE", 4) || (rf64 && memcmp(hdr + 12, "ds64", 4))) {
		close(fd);
		return -EINVAL;
	}
	/* cue : count, then 24-byte points; LIST adtl: one labl per point */
	size_t cue_size = 4 + 24 * (size_t)n, adtl = 4;
	for (int i = 0; i < n; i++) {
		size_t l = 4 + strlen(m[i].name) + 1;
		adtl += 8 + l + (l & 1);
	}
	size_t total = 8 + cue_size + 8 + adtl;
	uint8_t *buf = calloc(1, total);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	uint8_t *p = buf;
	memcpy(p, "cue ", 4); put32(p + 4, (uint32_t)cue_size); put32(p + 8, (uint32_t)n); p += 12;
	for (int i = 0; i < n; i++) {
		uint32_t frame = m[i].frame > UINT32_MAX ? UINT32_MAX : (uint32_t)m[i].frame;
		put32(p, (uint32_t)(i + 1)); put32(p + 4, frame); memcpy(p + 8, "data", 4);
		put32(p + 12, 0); put32(p + 16, 0); put32(p + 20, frame); p += 24;
	}
	memcpy(p, "LIST", 4); put32(p + 4, (uint32_t)adtl); memcpy(p + 8, "adtl", 4); p += 12;
	for (int i = 0; i < n; i++) {
		size_t l = 4 + strlen(m[i].name) + 1;
		memcpy(p, "labl", 4); put32(p + 4, (uint32_t)l); put32(p + 8, (uint32_t)(i + 1));
		memcpy(p + 12, m[i].name, strlen(m[i].name) + 1);
		p += 8 + l + (l & 1);
	}
	off_t end = st.st_size;
	int r = 0;
	if (end & 1) { /* chunks start on even offsets */
		uint8_t z = 0;
		if (pwrite(fd, &z, 1, end) != 1) r = -EIO;
		end++;
	}
	if (!r && pwrite(fd, buf, total, end) != (ssize_t)total)
		r = -EIO;
	uint64_t riff = (uint64_t)end + total - 8;
	if (!r) {
		uint8_t sz[8];
		if (rf64 || riff > UINT32_MAX) {
			if (!rf64) {
				r = -EFBIG; /* a > 4 GB RIFF: cannot happen, libsndfile writes RF64 */
			} else {
				put64(sz, riff);
				if (pwrite(fd, sz, 8, 20) != 8) r = -EIO;
			}
		} else {
			put32(sz, (uint32_t)riff);
			if (pwrite(fd, sz, 4, 4) != 4) r = -EIO;
		}
	}
	free(buf);
	fsync(fd);
	close(fd);
	return r;
}
