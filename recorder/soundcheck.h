/* backline virtual soundcheck: a take played back as one node, all tracks in lockstep.
 * SPDX-License-Identifier: MIT
 *
 * The player is one pw_filter, `backline.soundcheck`, with an output port per
 * track channel (t<track>_<channel>, like the recorder's inputs). Frame k of
 * every track goes out in the same graph cycle, so a multitrack take comes
 * back as aligned as it was recorded. A disk thread reads ahead into one ring
 * per track; the data thread only copies, and if any ring runs short it plays
 * silence on every track for that cycle and counts it, so they never drift.
 *
 * Which consumer hears the playback instead of the live input is the
 * daemon's job (recorderd.c): it moves links. This file only plays. Main
 * thread only, like rec_engine.h.
 */
#ifndef BACKLINE_SOUNDCHECK_H
#define BACKLINE_SOUNDCHECK_H

#include <stdbool.h>
#include <stdint.h>
#include <pipewire/pipewire.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC_MAX_TRACKS 32
#define SC_MAX_CH 16

struct sc_track_info {
	char node[256];     /* the input the track was recorded from */
	char file[1024];
	int channels;
	char source[16];    /* "monitor" or "output", from the sidecar */
};

struct sc_take {
	char take[128];
	char sidecar[1100];
	uint32_t rate;
	uint64_t frames;
	int n_tracks;
	struct sc_track_info tracks[SC_MAX_TRACKS];
};

/* Read a backline.take/1 or /2 sidecar. Returns 0 or -errno; err says why. */
int sc_take_load(const char *sidecar, struct sc_take *out, char *err, size_t err_size);

struct sc_player;

struct sc_player *sc_player_new(struct pw_core *core, struct pw_loop *loop, const struct sc_take *take,
		bool loop_play, uint64_t start_frame, char *err, size_t err_size);
void sc_player_destroy(struct sc_player *p);

uint32_t sc_player_node_id(const struct sc_player *p);
uint64_t sc_player_position(const struct sc_player *p);   /* take frame being played */
uint64_t sc_player_underruns(const struct sc_player *p);  /* cycles played as silence for want of data */
uint64_t sc_player_cycles(const struct sc_player *p);
bool sc_player_ended(const struct sc_player *p);          /* not looping, and past the end */
/* Whether the graph rate matches the take (no resampling is done). */
uint32_t sc_player_graph_rate(const struct sc_player *p);

#ifdef __cplusplus
}
#endif
#endif
