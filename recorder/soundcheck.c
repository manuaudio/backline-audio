/* backline virtual soundcheck player (soundcheck.h).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "soundcheck.h"
#include "sc_plan.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <json-c/json.h>
#include <pipewire/filter.h>
#include <sndfile.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/string.h>

#define CYCLE_MAX 8192
#define CHUNK 4096
#define RING_SECONDS 4

int sc_take_load(const char *sidecar, struct sc_take *out, char *err, size_t err_size)
{
	memset(out, 0, sizeof(*out));
	json_object *j = json_object_from_file(sidecar);
	if (!j) {
		snprintf(err, err_size, "%s: not readable JSON", sidecar);
		return -ENOENT;
	}
	json_object *v;
	const char *schema = json_object_object_get_ex(j, "schema", &v) ? json_object_get_string(v) : "";
	if (strncmp(schema, "backline.take/", 14)) {
		snprintf(err, err_size, "%s: not a Backline take", sidecar);
		json_object_put(j);
		return -EINVAL;
	}
	snprintf(out->sidecar, sizeof(out->sidecar), "%s", sidecar);
	if (json_object_object_get_ex(j, "take", &v))
		snprintf(out->take, sizeof(out->take), "%s", json_object_get_string(v));
	if (json_object_object_get_ex(j, "rate", &v))
		out->rate = (uint32_t)json_object_get_int64(v);
	if (json_object_object_get_ex(j, "frames", &v))
		out->frames = (uint64_t)json_object_get_int64(v);
	json_object *tracks;
	if (!json_object_object_get_ex(j, "tracks", &tracks) || !json_object_is_type(tracks, json_type_array)) {
		snprintf(err, err_size, "%s: no tracks", sidecar);
		json_object_put(j);
		return -EINVAL;
	}
	size_t n = json_object_array_length(tracks);
	for (size_t i = 0; i < n && i < SC_MAX_TRACKS; i++) {
		json_object *t = json_object_array_get_idx(tracks, i);
		struct sc_track_info *ti = &out->tracks[out->n_tracks++];
		if (json_object_object_get_ex(t, "node", &v))
			snprintf(ti->node, sizeof(ti->node), "%s", json_object_get_string(v));
		if (json_object_object_get_ex(t, "file", &v))
			snprintf(ti->file, sizeof(ti->file), "%s", json_object_get_string(v));
		if (json_object_object_get_ex(t, "source", &v))
			snprintf(ti->source, sizeof(ti->source), "%s", json_object_get_string(v));
		ti->channels = json_object_object_get_ex(t, "channels", &v) ? json_object_get_int(v) : 2;
		ti->channels = SPA_CLAMP(ti->channels, 1, SC_MAX_CH);
	}
	json_object_put(j);
	if (out->n_tracks == 0 || out->rate == 0) {
		snprintf(err, err_size, "%s: empty take", sidecar);
		return -EINVAL;
	}
	return 0;
}

struct sc_ptrack {
	int channels;
	void *ports[SC_MAX_CH];
	SNDFILE *sf;
	struct spa_ringbuffer ring;
	float *ring_data;
	uint32_t ring_size;
	float *rbuf;   /* data thread: CYCLE_MAX x channels */
	float *dbuf;   /* disk thread: CHUNK x channels */
};

struct sc_player {
	struct pw_loop *loop;
	struct pw_filter *filter;
	struct spa_hook listener;
	struct sc_take take;
	struct sc_ptrack t[SC_MAX_TRACKS];
	bool loop_play;
	uint64_t start;
	pthread_t disk;
	atomic_int quit, primed, ended;
	atomic_uint_fast64_t position, underruns, cycles;
	atomic_uint graph_rate;
	uint64_t read_pos;  /* disk thread: next take frame to read */
	uint32_t node_id;
};

static void on_process(void *data, struct spa_io_position *pos)
{
	struct sc_player *p = data;
	uint32_t n = SPA_MIN(pos->clock.duration, (uint64_t)CYCLE_MAX);
	atomic_store_explicit(&p->graph_rate, pos->clock.rate.denom, memory_order_relaxed);
	atomic_fetch_add_explicit(&p->cycles, 1, memory_order_relaxed);
	float *out[SC_MAX_TRACKS][SC_MAX_CH];
	for (int k = 0; k < p->take.n_tracks; k++)
		for (int c = 0; c < p->t[k].channels; c++)
			out[k][c] = pw_filter_get_dsp_buffer(p->t[k].ports[c], pos->clock.duration);

	bool have = atomic_load(&p->primed);
	for (int k = 0; have && k < p->take.n_tracks; k++) {
		uint32_t idx;
		int32_t avail = spa_ringbuffer_get_read_index(&p->t[k].ring, &idx);
		if ((uint32_t)avail < n * p->t[k].channels * sizeof(float))
			have = false;
	}
	if (!have) {
		if (atomic_load(&p->primed) && !atomic_load(&p->ended))
			atomic_fetch_add(&p->underruns, 1);
		for (int k = 0; k < p->take.n_tracks; k++)
			for (int c = 0; c < p->t[k].channels; c++)
				if (out[k][c])
					memset(out[k][c], 0, n * sizeof(float));
		return;
	}
	for (int k = 0; k < p->take.n_tracks; k++) {
		struct sc_ptrack *tr = &p->t[k];
		uint32_t idx, bytes = n * tr->channels * sizeof(float);
		spa_ringbuffer_get_read_index(&tr->ring, &idx);
		spa_ringbuffer_read_data(&tr->ring, tr->ring_data, tr->ring_size, idx & (tr->ring_size - 1), tr->rbuf, bytes);
		spa_ringbuffer_read_update(&tr->ring, idx + bytes);
		for (int c = 0; c < tr->channels; c++) {
			if (!out[k][c])
				continue;
			for (uint32_t i = 0; i < n; i++)
				out[k][c][i] = tr->rbuf[i * tr->channels + c];
		}
	}
	uint64_t at = atomic_load(&p->position) + n;
	if (p->loop_play && p->take.frames)
		at %= p->take.frames;
	atomic_store(&p->position, at);
}

static void on_state_changed(void *data, enum pw_filter_state old, enum pw_filter_state state, const char *error)
{
	struct sc_player *p = data;
	if (state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING)
		p->node_id = pw_filter_get_node_id(p->filter);
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = on_state_changed,
	.process = on_process,
};

/* Read the next chunk of every track, the same length on all (EOF pads
 * with silence; looping seeks every track back together). */
static bool read_chunk(struct sc_player *p)
{
	uint64_t left = p->take.frames > p->read_pos ? p->take.frames - p->read_pos : 0;
	if (left == 0) {
		if (!p->loop_play)
			return false;
		p->read_pos = 0;
		for (int k = 0; k < p->take.n_tracks; k++)
			if (p->t[k].sf)
				sf_seek(p->t[k].sf, 0, SEEK_SET);
		left = p->take.frames;
	}
	uint32_t n = (uint32_t)SPA_MIN(left, (uint64_t)CHUNK);
	for (int k = 0; k < p->take.n_tracks; k++) {
		struct sc_ptrack *tr = &p->t[k];
		sf_count_t got = tr->sf ? sf_readf_float(tr->sf, tr->dbuf, n) : 0;
		if (got < (sf_count_t)n)
			memset(tr->dbuf + (size_t)SPA_MAX(got, 0) * tr->channels, 0,
			       (size_t)(n - SPA_MAX(got, 0)) * tr->channels * sizeof(float));
		uint32_t idx, bytes = n * tr->channels * sizeof(float);
		spa_ringbuffer_get_write_index(&tr->ring, &idx);
		spa_ringbuffer_write_data(&tr->ring, tr->ring_data, tr->ring_size, idx & (tr->ring_size - 1), tr->dbuf, bytes);
		spa_ringbuffer_write_update(&tr->ring, idx + bytes);
	}
	p->read_pos += n;
	return true;
}

static bool all_have_room(struct sc_player *p)
{
	for (int k = 0; k < p->take.n_tracks; k++) {
		uint32_t idx;
		int32_t filled = spa_ringbuffer_get_write_index(&p->t[k].ring, &idx);
		if ((uint32_t)filled + CHUNK * p->t[k].channels * sizeof(float) > p->t[k].ring_size)
			return false;
	}
	return true;
}

static void *disk_main(void *arg)
{
	struct sc_player *p = arg;
	struct timespec nap = { 0, 5 * 1000000L };
	uint64_t prime = p->take.rate / 2;
	uint64_t filled = 0;
	bool more = true;
	while (!atomic_load(&p->quit)) {
		bool did = false;
		while (more && all_have_room(p) && !atomic_load(&p->quit)) {
			uint64_t before = p->read_pos;
			more = read_chunk(p);
			filled += p->read_pos >= before ? p->read_pos - before : p->read_pos;
			did = true;
			if (!atomic_load(&p->primed) && filled >= prime)
				break;
		}
		if (!atomic_load(&p->primed) && (filled >= prime || !more))
			atomic_store(&p->primed, 1);
		if (!more) {
			/* not looping: done once the rings are empty */
			uint32_t idx;
			if (spa_ringbuffer_get_read_index(&p->t[0].ring, &idx) == 0)
				atomic_store(&p->ended, 1);
		}
		if (!did)
			nanosleep(&nap, NULL);
	}
	return NULL;
}

struct sc_player *sc_player_new(struct pw_core *core, struct pw_loop *loop, const struct sc_take *take,
		bool loop_play, uint64_t start_frame, char *err, size_t err_size)
{
	struct sc_player *p = calloc(1, sizeof(*p));
	if (!p) {
		snprintf(err, err_size, "out of memory");
		return NULL;
	}
	p->loop = loop;
	p->take = *take;
	p->loop_play = loop_play;
	p->node_id = SPA_ID_INVALID;
	if (start_frame >= take->frames)
		start_frame = 0;
	p->start = start_frame;
	p->read_pos = start_frame;
	atomic_store(&p->position, start_frame);

	for (int k = 0; k < take->n_tracks; k++) {
		struct sc_ptrack *tr = &p->t[k];
		SF_INFO info = { 0 };
		tr->channels = take->tracks[k].channels;
		tr->sf = sf_open(take->tracks[k].file, SFM_READ, &info);
		if (!tr->sf) {
			snprintf(err, err_size, "%s: %s", take->tracks[k].file, sf_strerror(NULL));
			sc_player_destroy(p);
			return NULL;
		}
		if (info.channels != tr->channels) {
			snprintf(err, err_size, "%s: %d channels, the take says %d", take->tracks[k].file, info.channels, tr->channels);
			sc_player_destroy(p);
			return NULL;
		}
		if (start_frame)
			sf_seek(tr->sf, (sf_count_t)start_frame, SEEK_SET);
		uint64_t want = (uint64_t)take->rate * RING_SECONDS * tr->channels * sizeof(float);
		tr->ring_size = 1;
		while (tr->ring_size < want)
			tr->ring_size <<= 1;
		tr->ring_data = calloc(1, tr->ring_size);
		memset(tr->ring_data, 0, tr->ring_size);
		tr->rbuf = calloc((size_t)CYCLE_MAX * tr->channels, sizeof(float));
		tr->dbuf = calloc((size_t)CHUNK * tr->channels, sizeof(float));
		spa_ringbuffer_init(&tr->ring);
	}

	p->filter = pw_filter_new(core, "backline-soundcheck",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Production",
			PW_KEY_NODE_NAME, SC_NODE_NAME,
			PW_KEY_NODE_DESCRIPTION, "Backline soundcheck",
			PW_KEY_NODE_AUTOCONNECT, "false",
			PW_KEY_NODE_ALWAYS_PROCESS, "true",
			"backline.take", take->take,
			NULL));
	pw_filter_add_listener(p->filter, &p->listener, &filter_events, p);
	for (int k = 0; k < take->n_tracks; k++) {
		for (int c = 0; c < p->t[k].channels; c++) {
			char name[64];
			scp_port_name(k + 1, c + 1, name, sizeof(name));
			p->t[k].ports[c] = pw_filter_add_port(p->filter, PW_DIRECTION_OUTPUT,
				PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
				pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
						  PW_KEY_PORT_NAME, name,
						  "backline.soundcheck.input", take->tracks[k].node, NULL), NULL, 0);
		}
	}
	pthread_create(&p->disk, NULL, disk_main, p);
	if (pw_filter_connect(p->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		snprintf(err, err_size, "cannot connect the soundcheck node");
		sc_player_destroy(p);
		return NULL;
	}
	return p;
}

void sc_player_destroy(struct sc_player *p)
{
	if (!p)
		return;
	if (p->filter)
		pw_filter_destroy(p->filter);
	atomic_store(&p->quit, 1);
	if (p->disk)
		pthread_join(p->disk, NULL);
	for (int k = 0; k < p->take.n_tracks; k++) {
		struct sc_ptrack *tr = &p->t[k];
		if (tr->sf)
			sf_close(tr->sf);
		free(tr->ring_data);
		free(tr->rbuf);
		free(tr->dbuf);
	}
	free(p);
}

uint32_t sc_player_node_id(const struct sc_player *p) { return p->node_id; }
uint64_t sc_player_position(const struct sc_player *p) { return atomic_load(&((struct sc_player *)p)->position); }
uint64_t sc_player_underruns(const struct sc_player *p) { return atomic_load(&((struct sc_player *)p)->underruns); }
uint64_t sc_player_cycles(const struct sc_player *p) { return atomic_load(&((struct sc_player *)p)->cycles); }
bool sc_player_ended(const struct sc_player *p) { return atomic_load(&((struct sc_player *)p)->ended); }
uint32_t sc_player_graph_rate(const struct sc_player *p) { return atomic_load(&((struct sc_player *)p)->graph_rate); }
