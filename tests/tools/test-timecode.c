/* backline-test-timecode: an independent native PipeWire node for the clock
 * proofs. SPDX-License-Identifier: MIT
 *
 *   backline-test-timecode -n NAME -o LOG [--observe] [--ltc-read] [--mtc-read]
 *                          [--ltc-gen TC] [--mtc-gen TC] [--fps FPS]
 *
 * --observe    log the transport this node sees in spa_io_position every
 *              cycle: graph position, state, transport frame (computed from
 *              the segment as pipewire-jack does), and the segment's bar
 *   C  pos rolling frame bar_valid bpm beat sig_num sig_denom nsec
 * --ltc-read   decode LTC on input port ltc_in with libltc:
 *   L  k start_graph_pos transport_frame_at_start rolling   (k: the frame
 *      that starts right after a decoded frame ends)
 * --mtc-read   decode MTC quarter frames on MIDI port mtc_in:
 *   Q  q4 graph_pos transport_frame rolling
 * --ltc-gen TC / --mtc-gen TC   generate LTC (port ltc_out) / MTC (mtc_out)
 *              from timecode TC, starting at the first cycle after SIGUSR1;
 *              SIGUSR2 jumps to --jump TC; SIGHUP stops (silence). Logs
 *   G  graph_pos_of_start start_frame_samples
 * The log is written by the main thread from a lock-free ring.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ltc.h>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/control/control.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/ringbuffer.h>

#include "timecode.h"

#define HIST 256

struct rec { char kind; int64_t a, b, c, d; double e, f; int64_t g, h; uint64_t nsec; };

static struct {
	struct pw_main_loop *loop;
	struct pw_filter *filter;
	struct spa_hook listener;
	void *ltc_in, *ltc_out, *mtc_in, *mtc_out;
	bool observe;
	struct tc_rate r;
	uint32_t rate;
	int64_t gen_tc, jump_tc;
	atomic_int go, jump, halt;
	bool gen_on;
	int64_t gen_origin;   /* graph position where timecode sample gen_base plays */
	int64_t gen_base;
	struct ltc_gen gen;
	LTCDecoder *dec;
	struct mtc_rx mrx;
	/* history of cycles: graph position -> transport frame */
	int64_t h_pos[HIST], h_frame[HIST];
	bool h_roll[HIST];
	uint32_t h_n[HIST];
	int h_i;
	struct spa_ringbuffer ring;
	struct rec recs[8192];
	FILE *log;
} T;

static void push(struct rec r)
{
	uint32_t idx;
	int32_t fill = spa_ringbuffer_get_write_index(&T.ring, &idx);
	if (fill >= 8192)
		return;
	T.recs[idx & 8191] = r;
	spa_ringbuffer_write_update(&T.ring, idx + 1);
}

static int64_t transport_frame(const struct spa_io_position *p, bool *rolling)
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

/* transport frame at graph position g, from the cycle history */
static bool frame_at(int64_t g, int64_t *frame, bool *rolling)
{
	for (int k = 0; k < HIST; k++) {
		int i = (T.h_i - 1 - k + HIST) % HIST;
		if (T.h_n[i] && g >= T.h_pos[i] && g < T.h_pos[i] + T.h_n[i] + 0) {
			*rolling = T.h_roll[i];
			*frame = T.h_frame[i] + (T.h_roll[i] ? g - T.h_pos[i] : 0);
			return true;
		}
	}
	return false;
}

static void write_midi(void *port, uint32_t n, const uint8_t *const *msgs, const uint32_t *lens, const uint32_t *offs, int count)
{
	struct pw_buffer *b = pw_filter_dequeue_buffer(port);
	if (!b)
		return;
	struct spa_data *d = &b->buffer->datas[0];
	struct spa_pod_builder bld;
	struct spa_pod_frame f;
	spa_pod_builder_init(&bld, d->data, d->maxsize);
	spa_pod_builder_push_sequence(&bld, &f, 0);
	for (int i = 0; i < count; i++) {
		spa_pod_builder_control(&bld, offs[i], SPA_CONTROL_Midi);
		spa_pod_builder_bytes(&bld, msgs[i], lens[i]);
	}
	spa_pod_builder_pop(&bld, &f);
	d->chunk->offset = 0;
	d->chunk->size = bld.state.offset;
	d->chunk->stride = 1;
	pw_filter_queue_buffer(port, b);
	(void)n;
}

static void read_mtc(uint32_t n, int64_t pos)
{
	struct pw_buffer *b = pw_filter_dequeue_buffer(T.mtc_in);
	if (!b)
		return;
	struct spa_data *d = &b->buffer->datas[0];
	struct spa_pod *pod = d->data && d->chunk ? spa_pod_from_data(d->data, d->maxsize, d->chunk->offset, d->chunk->size) : NULL;
	if (pod && spa_pod_is_sequence(pod)) {
		struct spa_pod_control *c;
		SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence *)pod, c) {
			const uint8_t *m = SPA_POD_BODY(&c->value);
			uint32_t sz = SPA_POD_BODY_SIZE(&c->value);
			uint8_t qf = 0;
			bool is_qf = false;
			if (c->type == SPA_CONTROL_Midi && sz == 2 && m[0] == 0xF1) {
				qf = m[1];
				is_qf = true;
			} else if (c->type == SPA_CONTROL_UMP && sz >= 4) {
				uint32_t w;
				memcpy(&w, m, 4);
				if ((w >> 28) == 0x1 && ((w >> 16) & 0xff) == 0xF1) {
					qf = (w >> 8) & 0x7f;
					is_qf = true;
				}
			}
			if (!is_qf)
				continue;
			int64_t t = pos + c->offset, q4, fr;
			bool roll;
			mtc_rx_quarter(&T.mrx, qf, t, &q4);
			if (q4 >= 0 && frame_at(t, &fr, &roll))
				push((struct rec){ 'Q', q4, t, fr, roll });
		}
	}
	pw_filter_queue_buffer(T.mtc_in, b);
}

static void on_process(void *data, struct spa_io_position *p)
{
	uint32_t n = p->clock.duration;
	int64_t pos = (int64_t)p->clock.position;
	bool roll;
	int64_t fr = transport_frame(p, &roll);
	T.h_pos[T.h_i] = pos;
	T.h_frame[T.h_i] = fr;
	T.h_roll[T.h_i] = roll;
	T.h_n[T.h_i] = n;
	T.h_i = (T.h_i + 1) % HIST;
	T.rate = p->clock.rate.denom;
	if (T.observe) {
		const struct spa_io_segment_bar *bar = &p->segments[0].bar;
		push((struct rec){ 'C', pos, roll, fr, (bar->flags & SPA_IO_SEGMENT_BAR_FLAG_VALID) != 0, bar->bpm, bar->beat,
				   (int64_t)bar->signature_num, (int64_t)bar->signature_denom, p->clock.nsec });
	}
	/* generators */
	if (atomic_exchange(&T.go, 0)) {
		T.gen_on = true;
		T.gen_origin = pos;
		T.gen_base = tc_frame_start(&T.r, T.rate, T.gen_tc);
		ltc_gen_init(&T.gen, &T.r, T.rate, 0.25f);
		push((struct rec){ 'G', pos, T.gen_base });
	}
	if (atomic_exchange(&T.jump, 0) && T.gen_on) {
		T.gen_origin = pos;
		T.gen_base = tc_frame_start(&T.r, T.rate, T.jump_tc);
		push((struct rec){ 'G', pos, T.gen_base });
	}
	if (atomic_exchange(&T.halt, 0))
		T.gen_on = false;
	if (T.ltc_out) {
		float *o = pw_filter_get_dsp_buffer(T.ltc_out, n);
		if (o)
			for (uint32_t i = 0; i < n; i++)
				o[i] = T.gen_on ? ltc_gen_sample(&T.gen, T.gen_base + (pos + i - T.gen_origin)) : 0.0f;
	}
	if (T.mtc_out) {
		const uint8_t *msgs[64];
		uint32_t lens[64], offs[64];
		static uint8_t buf[64][2];
		int cnt = 0;
		if (T.gen_on) {
			int64_t x0 = T.gen_base + (pos - T.gen_origin);
			int64_t q = (int64_t)(((__int128)x0 * 4 * T.r.num) / ((__int128)T.rate * T.r.den));
			if (mtc_qf_start(&T.r, T.rate, q) < x0)
				q++;
			for (; mtc_qf_start(&T.r, T.rate, q) < x0 + n && cnt < 64; q++, cnt++) {
				buf[cnt][0] = 0xF1;
				buf[cnt][1] = mtc_qf_byte(&T.r, q);
				msgs[cnt] = buf[cnt];
				lens[cnt] = 2;
				offs[cnt] = (uint32_t)(mtc_qf_start(&T.r, T.rate, q) - x0);
			}
		}
		write_midi(T.mtc_out, n, msgs, lens, offs, cnt);
	}
	/* readers */
	if (T.ltc_in) {
		float *in = pw_filter_get_dsp_buffer(T.ltc_in, n);
		if (in) {
			ltc_decoder_write_float(T.dec, in, n, pos);
			LTCFrameExt fx;
			while (ltc_decoder_read(T.dec, &fx)) {
				SMPTETimecode st;
				ltc_frame_to_time(&st, &fx.ltc, 0);
				struct tc_label l = { st.hours, st.mins, st.secs, st.frame };
				int64_t k = tc_label_to_frames(&T.r, &l), f2;
				bool r2;
				/* frame k + 1 starts right after libltc's exact off_end */
				if (frame_at(fx.off_end + 1, &f2, &r2))
					push((struct rec){ 'L', k + 1, fx.off_end + 1, f2, r2 });
			}
		}
	}
	if (T.mtc_in)
		read_mtc(n, pos);
}

static const struct pw_filter_events events = { PW_VERSION_FILTER_EVENTS, .process = on_process };

static void on_drain(void *data, uint64_t exp)
{
	uint32_t idx;
	int32_t avail = spa_ringbuffer_get_read_index(&T.ring, &idx);
	for (int32_t i = 0; i < avail; i++) {
		struct rec *r = &T.recs[(idx + i) & 8191];
		switch (r->kind) {
		case 'C':
			fprintf(T.log, "C %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %.6f %.9f %" PRId64 " %" PRId64 " %" PRIu64 "\n",
				r->a, r->b, r->c, r->d, r->e, r->f, r->g, r->h, r->nsec);
			break;
		case 'G':
			fprintf(T.log, "G %" PRId64 " %" PRId64 "\n", r->a, r->b);
			break;
		default:
			fprintf(T.log, "%c %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 "\n", r->kind, r->a, r->b, r->c, r->d);
		}
	}
	spa_ringbuffer_read_update(&T.ring, idx + avail);
	fflush(T.log);
}

static void on_signal(void *data, int sig)
{
	if (sig == SIGUSR1) atomic_store(&T.go, 1);
	else if (sig == SIGUSR2) atomic_store(&T.jump, 1);
	else if (sig == SIGHUP) atomic_store(&T.halt, 1);
	else pw_main_loop_quit(T.loop);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "observe", no_argument, NULL, 'O' }, { "ltc-read", no_argument, NULL, 'l' },
		{ "mtc-read", no_argument, NULL, 'm' }, { "ltc-gen", required_argument, NULL, 'L' },
		{ "mtc-gen", required_argument, NULL, 'M' }, { "jump", required_argument, NULL, 'j' },
		{ "fps", required_argument, NULL, 'f' }, { NULL, 0, NULL, 0 },
	};
	const char *name = "backline-test.timecode", *logp = "/dev/stdout", *gen = NULL, *jump = "02:00:00:00";
	bool lr = false, mr = false, lg = false, mg = false;
	double fps = 25;
	int c;
	while ((c = getopt_long(argc, argv, "n:o:", opts, NULL)) != -1) {
		switch (c) {
		case 'n': name = optarg; break;
		case 'o': logp = optarg; break;
		case 'O': T.observe = true; break;
		case 'l': lr = true; break;
		case 'm': mr = true; break;
		case 'L': lg = true; gen = optarg; break;
		case 'M': mg = true; gen = optarg; break;
		case 'j': jump = optarg; break;
		case 'f': fps = atof(optarg); break;
		default: return 2;
		}
	}
	if (!tc_rate_from_fps(fps, &T.r))
		return 2;
	if (gen && !tc_parse(&T.r, gen, &T.gen_tc))
		return 2;
	if (!tc_parse(&T.r, jump, &T.jump_tc))
		return 2;
	T.log = fopen(logp, "w");
	spa_ringbuffer_init(&T.ring);
	mtc_rx_init(&T.mrx);
	T.dec = ltc_decoder_create(48000 / 25, 32);
	pw_init(&argc, &argv);
	T.loop = pw_main_loop_new(NULL);
	struct pw_loop *l = pw_main_loop_get_loop(T.loop);
	int sigs[] = { SIGINT, SIGTERM, SIGUSR1, SIGUSR2, SIGHUP };
	for (int i = 0; i < 5; i++)
		pw_loop_add_signal(l, sigs[i], on_signal, NULL);
	struct pw_context *ctx = pw_context_new(l, NULL, 0);
	struct pw_core *core = pw_context_connect(ctx, NULL, 0);
	if (!core)
		return 1;
	T.filter = pw_filter_new(core, name, pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Duplex",
		PW_KEY_NODE_NAME, name, PW_KEY_NODE_AUTOCONNECT, "false", PW_KEY_NODE_ALWAYS_PROCESS, "true", NULL));
	pw_filter_add_listener(T.filter, &T.listener, &events, NULL);
#define ADD(var, dir, fmt, pname) var = pw_filter_add_port(T.filter, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0, \
		pw_properties_new(PW_KEY_FORMAT_DSP, fmt, PW_KEY_PORT_NAME, pname, NULL), NULL, 0)
	if (lr) ADD(T.ltc_in, PW_DIRECTION_INPUT, "32 bit float mono audio", "ltc_in");
	if (lg) ADD(T.ltc_out, PW_DIRECTION_OUTPUT, "32 bit float mono audio", "ltc_out");
	if (mr) ADD(T.mtc_in, PW_DIRECTION_INPUT, "8 bit raw midi", "mtc_in");
	if (mg) ADD(T.mtc_out, PW_DIRECTION_OUTPUT, "8 bit raw midi", "mtc_out");
	/* an always-present dummy port keeps an observe-only node schedulable */
	void *dummy = NULL;
	ADD(dummy, PW_DIRECTION_INPUT, "32 bit float mono audio", "in");
	(void)dummy;
	pw_filter_connect(T.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
	struct spa_source *t = pw_loop_add_timer(l, on_drain, NULL);
	struct timespec ts = { 0, 20 * 1000000L };
	pw_loop_update_timer(l, t, &ts, &ts, false);
	pw_main_loop_run(T.loop);
	on_drain(NULL, 0);
	pw_filter_destroy(T.filter);
	pw_core_disconnect(core);
	pw_context_destroy(ctx);
	pw_main_loop_destroy(T.loop);
	fclose(T.log);
	return 0;
}
