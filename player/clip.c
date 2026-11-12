/* backline-playerd clip engine (clip.h).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "clip.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/filter.h>
#include <sndfile.h>
#include <spa/node/io.h>
#include <spa/utils/defs.h>

#define BLK 1024            /* frames per ring block */
#define CYCLE_MAX 8192

struct blk_hdr {
	uint32_t gen;
	uint32_t n;
	uint64_t off;       /* file frame of the block's first frame */
};

struct pl_clip {
	char id[128], path[1024], node_name[160];
	struct pw_filter *filter;
	struct spa_hook listener;
	void *ports[PL_MAX_CH];
	char chan[PL_MAX_CH][16];
	uint32_t channels, file_rate;
	uint64_t frames;
	atomic_uint_fast32_t node_id;

	/* in memory: file frames [0, head_frames) */
	float *head;
	uint64_t head_frames;

	/* ring of blocks: disk writes, data reads */
	struct blk_hdr *hdr;
	float *data;            /* nblk x BLK x channels */
	uint32_t nblk;
	atomic_uint_fast32_t wpos, rpos;

	/* seek requests: data -> disk */
	atomic_uint_fast32_t want_gen;
	atomic_uint_fast64_t want_off;
	sem_t sem;

	/* data thread only */
	uint32_t rgen;
	uint64_t reader_next;   /* the ring's stream (gen rgen) delivers this offset next */
	uint32_t blk_used;      /* frames already taken from the front block */
	float g_cur;

	/* main -> data */
	atomic_int_fast64_t start;
	atomic_uint_fast32_t gain_bits;  /* float */
	double gain_db;                  /* main only */

	/* data -> main (stats) */
	atomic_uint_fast32_t graph_rate;
	atomic_bool rolling;
	atomic_int_fast64_t transport;
	atomic_uint_fast64_t cycles, underruns, underrun_frames, seeks, played;

	/* disk thread */
	SNDFILE *sf;
	pthread_t disk;
	bool disk_started;
	atomic_bool quit;
	float *dbuf;
};

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

bool pl_span(int64_t start, uint64_t frames, int64_t t, uint32_t n,
	     uint32_t *out_off, uint64_t *file_off, uint32_t *len)
{
	int64_t o0 = t - start;                 /* file frame at output sample 0 */
	int64_t a = o0 < 0 ? -o0 : 0;           /* first output sample inside the file */
	if (a >= (int64_t)n)
		return false;
	int64_t f = o0 + a;
	if (f >= (int64_t)frames)
		return false;
	int64_t l = (int64_t)n - a;
	if (f + l > (int64_t)frames)
		l = (int64_t)frames - f;
	*out_off = (uint32_t)a;
	*file_off = (uint64_t)f;
	*len = (uint32_t)l;
	return true;
}

/* The transport frame of this cycle's first sample, as pipewire-jack
 * computes it from the segment (tests/tools/test-timecode.c does the same). */
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

/* ---------------------------------------------------------------- data */
static void request_seek(struct pl_clip *c, uint64_t off)
{
	c->rgen++;
	c->reader_next = off;
	c->blk_used = 0;
	atomic_store_explicit(&c->want_off, off, memory_order_relaxed);
	atomic_store_explicit(&c->want_gen, c->rgen, memory_order_release);
	atomic_fetch_add_explicit(&c->seeks, 1, memory_order_relaxed);
	sem_post(&c->sem);
}

/* Drop front blocks of an older generation. */
static void drop_stale(struct pl_clip *c)
{
	uint32_t r = atomic_load_explicit(&c->rpos, memory_order_relaxed);
	uint32_t w = atomic_load_explicit(&c->wpos, memory_order_acquire);
	bool dropped = false;
	while (r != w && c->hdr[r % c->nblk].gen != c->rgen) {
		r++;
		c->blk_used = 0;
		dropped = true;
	}
	if (dropped) {
		atomic_store_explicit(&c->rpos, r, memory_order_release);
		sem_post(&c->sem);
	}
}

/* Copy `len` file frames from offset `off` out of the ring into the outputs
 * at `out_off`, with gain ramp g0 -> g1 over the cycle (sample index i of n).
 * Returns the frames delivered (stops at the first frame not in the ring). */
static uint32_t ring_copy(struct pl_clip *c, uint64_t off, uint32_t len, float **outs,
			  uint32_t out_off, float g0, float dg)
{
	uint32_t done = 0;
	uint32_t r = atomic_load_explicit(&c->rpos, memory_order_relaxed);
	bool freed = false;
	while (done < len) {
		uint32_t w = atomic_load_explicit(&c->wpos, memory_order_acquire);
		if (r == w)
			break;
		struct blk_hdr *h = &c->hdr[r % c->nblk];
		if (h->gen != c->rgen) { r++; c->blk_used = 0; freed = true; continue; }
		uint64_t b0 = h->off + c->blk_used, b1 = h->off + h->n;
		uint64_t want = off + done;
		if (b1 <= want) {           /* entirely behind: skip it */
			r++; c->blk_used = 0; freed = true;
			continue;
		}
		if (b0 > want)              /* a gap: not ours (yet) */
			break;
		uint32_t k = (uint32_t)(want - h->off);
		uint32_t take = (uint32_t)SPA_MIN((uint64_t)(len - done), b1 - want);
		const float *src = c->data + ((size_t)(r % c->nblk) * BLK + k) * c->channels;
		for (uint32_t ch = 0; ch < c->channels; ch++) {
			float *o = outs[ch];
			if (!o)
				continue;
			for (uint32_t i = 0; i < take; i++) {
				uint32_t s = out_off + done + i;
				o[s] = src[(size_t)i * c->channels + ch] * (g0 + dg * (float)s);
			}
		}
		done += take;
		c->blk_used = k + take;
		if (c->blk_used >= h->n) { r++; c->blk_used = 0; freed = true; }
	}
	atomic_store_explicit(&c->rpos, r, memory_order_release);
	if (freed)
		sem_post(&c->sem);
	c->reader_next = off + done;
	return done;
}

static void on_process(void *data, struct spa_io_position *pos)
{
	struct pl_clip *c = data;
	uint32_t n = (uint32_t)SPA_MIN(pos->clock.duration, (uint64_t)CYCLE_MAX);
	uint32_t rate = pos->clock.rate.denom;
	float *outs[PL_MAX_CH];
	for (uint32_t ch = 0; ch < c->channels; ch++) {
		outs[ch] = pw_filter_get_dsp_buffer(c->ports[ch], n);
		if (outs[ch])
			memset(outs[ch], 0, n * sizeof(float));
	}
	atomic_fetch_add_explicit(&c->cycles, 1, memory_order_relaxed);
	atomic_store_explicit(&c->graph_rate, rate, memory_order_relaxed);
	bool rolling;
	int64_t t = transport_frame(pos, &rolling);
	atomic_store_explicit(&c->rolling, rolling, memory_order_relaxed);
	atomic_store_explicit(&c->transport, t, memory_order_relaxed);
	int64_t start = atomic_load_explicit(&c->start, memory_order_relaxed);
	float g1 = u2f(atomic_load_explicit(&c->gain_bits, memory_order_relaxed));

	drop_stale(c);
	/* Where the ring must stand: at the first frame past the head that the
	 * transport will need (now if rolling, at the next start if stopped). */
	int64_t o0 = t - start;
	uint64_t need = (uint64_t)SPA_MAX(o0, (int64_t)c->head_frames);
	if (need < c->frames) {
		uint64_t cap = (uint64_t)c->nblk * BLK;
		if (need < c->reader_next || need > c->reader_next + cap)
			request_seek(c, need);
	}

	if (rate != c->file_rate || !rolling) {
		c->g_cur = g1;
		return;
	}
	uint32_t out_off, len;
	uint64_t f;
	if (!pl_span(start, c->frames, t, n, &out_off, &f, &len)) {
		c->g_cur = g1;
		return;
	}
	float g0 = c->g_cur, dg = (g1 - g0) / (float)n;
	uint32_t done = 0;
	/* head part, from memory */
	if (f < c->head_frames) {
		uint32_t hl = (uint32_t)SPA_MIN((uint64_t)len, c->head_frames - f);
		const float *src = c->head + (size_t)f * c->channels;
		for (uint32_t ch = 0; ch < c->channels; ch++) {
			float *o = outs[ch];
			if (!o)
				continue;
			for (uint32_t i = 0; i < hl; i++) {
				uint32_t s = out_off + i;
				o[s] = src[(size_t)i * c->channels + ch] * (g0 + dg * (float)s);
			}
		}
		done = hl;
	}
	if (done < len) {
		uint32_t got = ring_copy(c, f + done, len - done, outs, out_off + done, g0, dg);
		if (got < len - done) {
			atomic_fetch_add_explicit(&c->underruns, 1, memory_order_relaxed);
			atomic_fetch_add_explicit(&c->underrun_frames, len - done - got, memory_order_relaxed);
		}
		done += got;
	}
	atomic_fetch_add_explicit(&c->played, done, memory_order_relaxed);
	c->g_cur = g1;
}

static void on_state_changed(void *data, enum pw_filter_state old, enum pw_filter_state state, const char *error)
{
	struct pl_clip *c = data;
	if (state == PW_FILTER_STATE_ERROR)
		fprintf(stderr, "backline-playerd: %s: %s\n", c->node_name, error ? error : "error");
	if (state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING)
		atomic_store(&c->node_id, pw_filter_get_node_id(c->filter));
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = on_state_changed,
	.process = on_process,
};

/* ---------------------------------------------------------------- disk */
static void *disk_main(void *arg)
{
	struct pl_clip *c = arg;
	uint32_t gen = 0;
	uint64_t off = c->head_frames;
	sf_seek(c->sf, (sf_count_t)off, SEEK_SET);
	while (!atomic_load(&c->quit)) {
		uint32_t g = atomic_load_explicit(&c->want_gen, memory_order_acquire);
		if (g != gen) {
			gen = g;
			off = atomic_load_explicit(&c->want_off, memory_order_relaxed);
			sf_seek(c->sf, (sf_count_t)off, SEEK_SET);
		}
		uint32_t w = atomic_load_explicit(&c->wpos, memory_order_relaxed);
		uint32_t r = atomic_load_explicit(&c->rpos, memory_order_acquire);
		if (off < c->frames && w - r < c->nblk) {
			uint32_t want = (uint32_t)SPA_MIN((uint64_t)BLK, c->frames - off);
			float *dst = c->data + (size_t)(w % c->nblk) * BLK * c->channels;
			sf_count_t got = sf_readf_float(c->sf, dst, want);
			if (got <= 0) { /* a short file: treat the rest as silence */
				memset(dst, 0, (size_t)want * c->channels * sizeof(float));
				got = want;
			}
			struct blk_hdr *h = &c->hdr[w % c->nblk];
			h->gen = gen;
			h->off = off;
			h->n = (uint32_t)got;
			atomic_store_explicit(&c->wpos, w + 1, memory_order_release);
			off += (uint64_t)got;
			continue;
		}
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 20 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
		sem_timedwait(&c->sem, &ts);
	}
	return NULL;
}

/* ---------------------------------------------------------------- main */
static void channel_names(struct pl_clip *c, SNDFILE *sf)
{
	int map[PL_MAX_CH];
	bool have = sf_command(sf, SFC_GET_CHANNEL_MAP_INFO, map, (int)(sizeof(int) * c->channels)) == SF_TRUE;
	for (uint32_t i = 0; i < c->channels; i++) {
		const char *n = NULL;
		if (have) {
			switch (map[i]) {
			case SF_CHANNEL_MAP_MONO: n = "MONO"; break;
			case SF_CHANNEL_MAP_LEFT: case SF_CHANNEL_MAP_FRONT_LEFT: n = "FL"; break;
			case SF_CHANNEL_MAP_RIGHT: case SF_CHANNEL_MAP_FRONT_RIGHT: n = "FR"; break;
			case SF_CHANNEL_MAP_CENTER: case SF_CHANNEL_MAP_FRONT_CENTER: n = "FC"; break;
			case SF_CHANNEL_MAP_LFE: n = "LFE"; break;
			case SF_CHANNEL_MAP_REAR_LEFT: n = "RL"; break;
			case SF_CHANNEL_MAP_REAR_RIGHT: n = "RR"; break;
			case SF_CHANNEL_MAP_SIDE_LEFT: n = "SL"; break;
			case SF_CHANNEL_MAP_SIDE_RIGHT: n = "SR"; break;
			default: break;
			}
		}
		if (!n && c->channels == 1) n = "MONO";
		if (!n && c->channels == 2) n = i == 0 ? "FL" : "FR";
		if (n)
			snprintf(c->chan[i], sizeof(c->chan[i]), "%s", n);
		else
			snprintf(c->chan[i], sizeof(c->chan[i]), "AUX%u", i);
	}
	/* duplicates (a map we don't follow) fall back to AUX<n> */
	for (uint32_t i = 0; i < c->channels; i++)
		for (uint32_t j = 0; j < i; j++)
			if (!strcmp(c->chan[i], c->chan[j]))
				snprintf(c->chan[i], sizeof(c->chan[i]), "AUX%u", i);
}

struct pl_clip *pl_clip_new(struct pw_core *core, const struct pl_clip_params *p,
			   char *err, size_t err_size, int *errcode)
{
	*errcode = 0;
	SF_INFO info = { 0 };
	SNDFILE *sf = sf_open(p->path, SFM_READ, &info);
	if (!sf) {
		snprintf(err, err_size, "%s: %s", p->path, sf_strerror(NULL));
		*errcode = access(p->path, R_OK) == 0 ? -EINVAL : -ENOENT;
		return NULL;
	}
	if (info.frames <= 0 || info.channels <= 0) {
		snprintf(err, err_size, "%s: no audio frames", p->path);
		sf_close(sf);
		*errcode = -EINVAL;
		return NULL;
	}
	if (info.channels > PL_MAX_CH) {
		snprintf(err, err_size, "%s: %d channels; at most %d", p->path, info.channels, PL_MAX_CH);
		sf_close(sf);
		*errcode = -ENOTSUP;
		return NULL;
	}
	struct pl_clip *c = calloc(1, sizeof(*c));
	snprintf(c->id, sizeof(c->id), "%s", p->id);
	snprintf(c->path, sizeof(c->path), "%s", p->path);
	snprintf(c->node_name, sizeof(c->node_name), "backline.player.%s", p->id);
	c->sf = sf;
	c->channels = (uint32_t)info.channels;
	c->file_rate = (uint32_t)info.samplerate;
	c->frames = (uint64_t)info.frames;
	atomic_store(&c->node_id, SPA_ID_INVALID);
	atomic_store(&c->start, p->start);
	c->gain_db = p->gain_db;
	c->g_cur = (float)pow(10.0, p->gain_db / 20.0);
	atomic_store(&c->gain_bits, f2u(c->g_cur));
	channel_names(c, sf);
	sem_init(&c->sem, 0, 0);

	double pre = p->preload_s > 0 ? p->preload_s : 4.0;
	c->head_frames = SPA_MIN(c->frames, (uint64_t)(pre * c->file_rate));
	c->head = malloc(SPA_MAX(c->head_frames, 1) * c->channels * sizeof(float));
	sf_count_t got = sf_readf_float(sf, c->head, (sf_count_t)c->head_frames);
	if (got < (sf_count_t)c->head_frames) {
		memset(c->head + (size_t)SPA_MAX(got, 0) * c->channels, 0,
		       (size_t)(c->head_frames - (uint64_t)SPA_MAX(got, 0)) * c->channels * sizeof(float));
	}
	double rs = p->ring_s > 0 ? p->ring_s : 8.0;
	c->nblk = (uint32_t)SPA_MAX(4.0, ceil(rs * c->file_rate / BLK));
	c->hdr = calloc(c->nblk, sizeof(*c->hdr));
	c->data = calloc((size_t)c->nblk * BLK * c->channels, sizeof(float));
	c->reader_next = c->head_frames;
	atomic_store(&c->want_off, c->head_frames);

	char chs[16], file_rate[16];
	snprintf(chs, sizeof(chs), "%u", c->channels);
	snprintf(file_rate, sizeof(file_rate), "%u", c->file_rate);
	char positions[PL_MAX_CH * 8] = "";
	for (uint32_t i = 0; i < c->channels; i++) {
		strcat(positions, i ? "," : "");
		strcat(positions, c->chan[i]);
	}
	char desc[300];
	const char *base = strrchr(p->path, '/');
	snprintf(desc, sizeof(desc), "Backline player: %s", base ? base + 1 : p->path);
	c->filter = pw_filter_new(core, c->node_name,
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Production",
			PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
			PW_KEY_NODE_NAME, c->node_name,
			PW_KEY_NODE_DESCRIPTION, desc,
			PW_KEY_NODE_AUTOCONNECT, p->autoconnect ? "true" : "false",
			PW_KEY_NODE_ALWAYS_PROCESS, "true",
			"node.dont-fallback", p->autoconnect ? "false" : "true",
			"audio.channels", chs,
			"audio.position", positions,
			"backline.player.id", c->id,
			"backline.player.file", c->path,
			"backline.player.rate", file_rate,
			NULL));
	pw_filter_add_listener(c->filter, &c->listener, &filter_events, c);
	for (uint32_t i = 0; i < c->channels; i++) {
		char name[32];
		snprintf(name, sizeof(name), "output_%s", c->chan[i]);
		c->ports[i] = pw_filter_add_port(c->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
					  PW_KEY_PORT_NAME, name,
					  PW_KEY_AUDIO_CHANNEL, c->chan[i], NULL), NULL, 0);
	}
	pthread_create(&c->disk, NULL, disk_main, c);
	c->disk_started = true;
	if (pw_filter_connect(c->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		snprintf(err, err_size, "cannot connect node %s", c->node_name);
		*errcode = -EIO;
		pl_clip_destroy(c);
		return NULL;
	}
	return c;
}

void pl_clip_destroy(struct pl_clip *c)
{
	if (!c)
		return;
	if (c->filter)
		pw_filter_destroy(c->filter);   /* the data thread is done with us after this */
	atomic_store(&c->quit, true);
	sem_post(&c->sem);
	if (c->disk_started)
		pthread_join(c->disk, NULL);
	if (c->sf)
		sf_close(c->sf);
	sem_destroy(&c->sem);
	free(c->head);
	free(c->hdr);
	free(c->data);
	free(c);
}

void pl_clip_set_gain(struct pl_clip *c, double db)
{
	c->gain_db = db;
	atomic_store(&c->gain_bits, f2u((float)pow(10.0, db / 20.0)));
}

void pl_clip_set_start(struct pl_clip *c, int64_t start) { atomic_store(&c->start, start); }

bool pl_clip_ready(struct pl_clip *c) { return atomic_load(&c->node_id) != SPA_ID_INVALID; }

const char *pl_clip_channel_name(const struct pl_clip *c, uint32_t ch)
{
	return ch < c->channels ? c->chan[ch] : "";
}

void pl_clip_get_info(struct pl_clip *c, struct pl_clip_info *o)
{
	memset(o, 0, sizeof(*o));
	o->channels = c->channels;
	o->file_rate = c->file_rate;
	o->frames = c->frames;
	o->start = atomic_load(&c->start);
	o->gain_db = c->gain_db;
	o->node_id = (uint32_t)atomic_load(&c->node_id);
	o->graph_rate = (uint32_t)atomic_load(&c->graph_rate);
	o->rate_ok = o->graph_rate == 0 || o->graph_rate == c->file_rate;
	o->rolling = atomic_load(&c->rolling);
	o->transport = atomic_load(&c->transport);
	o->cycles = atomic_load(&c->cycles);
	o->underruns = atomic_load(&c->underruns);
	o->underrun_frames = atomic_load(&c->underrun_frames);
	o->seeks = atomic_load(&c->seeks);
	o->played_frames = atomic_load(&c->played);
}
