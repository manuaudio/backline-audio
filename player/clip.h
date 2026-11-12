/* backline-playerd: one clip = one audio file = one PipeWire node, placed on
 * the shared transport timeline (clockd's segment in spa_io_position).
 * SPDX-License-Identifier: MIT
 *
 * Threads:
 *  - main: create/destroy, gain and start changes (atomics), stats.
 *  - data (PipeWire's realtime loop): every cycle reads the transport frame
 *    T from spa_io_position and writes file frame (T - start + i) to output
 *    sample i. No locks, no allocation, no syscalls but sem_post.
 *  - disk: keeps a ring of blocks filled from the file, from the offset the
 *    data thread asks for. The first `preload` seconds are held in memory,
 *    so a start reached by rolling or by a locate before the clip is exact
 *    from its first sample.
 */
#ifndef BACKLINE_PLAYER_CLIP_H
#define BACKLINE_PLAYER_CLIP_H

#include <stdbool.h>
#include <stdint.h>
#include <pipewire/pipewire.h>

#define PL_MAX_CH 32

struct pl_clip;

struct pl_clip_info {
	uint32_t channels, file_rate;
	uint64_t frames;           /* file length */
	int64_t start;             /* timeline frame of file frame 0 */
	double gain_db;
	uint32_t node_id;
	uint32_t graph_rate;       /* last seen, 0 = not yet */
	bool rate_ok;              /* file rate == graph rate */
	bool rolling;              /* transport, last cycle */
	int64_t transport;         /* transport frame at the last cycle */
	uint64_t cycles, underruns, underrun_frames, seeks, played_frames;
};

struct pl_clip_params {
	const char *id;            /* node name is backline.player.<id> */
	const char *path;
	int64_t start;
	double gain_db;
	double preload_s;          /* default 4 */
	double ring_s;             /* default 8 */
	bool autoconnect;          /* let the session manager link it */
};

/* Opens the file and preloads its head (blocking, main thread). On error
 * returns NULL with a sentence in err; *errcode is -ENOENT (file), -EINVAL
 * (not audio, no frames), -ENOTSUP (too many channels). */
struct pl_clip *pl_clip_new(struct pw_core *core, const struct pl_clip_params *p,
			   char *err, size_t err_size, int *errcode);
void pl_clip_destroy(struct pl_clip *c);

void pl_clip_set_gain(struct pl_clip *c, double db);
void pl_clip_set_start(struct pl_clip *c, int64_t start);
void pl_clip_get_info(struct pl_clip *c, struct pl_clip_info *out);
const char *pl_clip_channel_name(const struct pl_clip *c, uint32_t ch); /* FL, FR, MONO, AUX<n> */
/* The node is in the graph (its id is known). */
bool pl_clip_ready(struct pl_clip *c);

/* Exposed for the unit test: how a clip maps a cycle onto the file. For a
 * cycle of n frames at transport frame t (rolling), the output samples
 * [*out_off, *out_off + *len) carry file frames [*file_off, *file_off + *len);
 * everything else is silence. Returns false if nothing of the file is in
 * the cycle. */
bool pl_span(int64_t start, uint64_t frames, int64_t t, uint32_t n,
	     uint32_t *out_off, uint64_t *file_off, uint32_t *len);

#endif
