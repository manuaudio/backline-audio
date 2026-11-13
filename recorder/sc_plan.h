/* backline virtual soundcheck: which links move, per channel and per consumer.
 * SPDX-License-Identifier: MIT
 *
 * PipeWire-free and pure, so the decisions are unit-tested offline
 * (tests/unit/test_sc_plan.c). recorderd.c fills a snapshot of the graph,
 * asks for a plan, and carries it out.
 *
 * The state. For every input of the loaded take (a node the take was recorded
 * from) and every one of its source ports ("channels", 1-based, in port
 * order, as the recorder took them), the mode is:
 *   - a default for every consumer, live or playback; plus
 *   - overrides for named consumers (a consumer is a node.name), each live or
 *     playback.
 * A link from input channel c to consumer N carries the take iff N's override
 * says playback, or there is none and the default says playback. So:
 *   SetInput(node, playback)                  = every channel, every consumer
 *   SetChannelInput(node, 3, playback, "")    = channel 3, every consumer
 *   SetChannelInput(node, 3, playback, "daw") = channel 3, only into "daw"
 * and nothing else moves: one channel's switch never touches another channel
 * of the same device, and a consumer's switch never touches another consumer.
 *
 * Several tracks may come from the same node. The state is per node, and its
 * moved links use the first such track's soundcheck ports.
 */
#ifndef BACKLINE_SC_PLAN_H
#define BACKLINE_SC_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Names in one place (a later rename touches only these). */
#define SC_NODE_NAME "backline.soundcheck"   /* the playback node */
#define SC_OURS_PREFIX "backline."           /* links into our own nodes are never moved */

#define SCP_MAX_TRACKS 32       /* = SC_MAX_TRACKS */
#define SCP_MAX_PORTS 64        /* source ports per input */
#define SCP_MAX_CONSUMERS 64    /* distinct consumers named in overrides */
#define SCP_NAME 256
#define SCP_PORT 128

/* "t<track>_<channel>", the soundcheck (and recorder) port name, 1-based. */
void scp_port_name(int track, int channel, char *out, size_t n);

struct scp_input {
	char node[SCP_NAME];
	int track;              /* first track (0-based) recorded from this node */
	int channels;           /* that track's channels */
	int ports;              /* source ports seen in the graph, 0 = unknown */
	uint64_t def;           /* bit c: channel c+1 defaults to playback */
	uint64_t over_set[SCP_MAX_PORTS];  /* bit k: consumer k has an override on this channel */
	uint64_t over_val[SCP_MAX_PORTS];  /* bit k: ... and it says playback */
};

struct scp_state {
	int n_inputs;
	struct scp_input inputs[SCP_MAX_TRACKS];
	int n_consumers;
	char consumers[SCP_MAX_CONSUMERS][SCP_NAME];
};

/* From the take's tracks (node and channels of each, in track order): every
 * channel live, no overrides. */
void scp_init(struct scp_state *s, int n_tracks, const char *const *nodes, const int *channels);

int scp_find_input(const struct scp_state *s, const char *node);

struct scp_request {
	const char *node;       /* an input of the take */
	uint32_t channel;       /* 1-based source channel; 0 = every channel (also ports that appear later) */
	const char *consumer;   /* a consumer node.name; NULL or "" = every consumer */
	bool playback;
};

/* Validate every request, then apply them all, or none: on an error the
 * state is untouched, the return is -(index + 1) of the first bad request and
 * err says why. Returns 0. Idempotent: applying the same requests again
 * changes nothing. */
int scp_apply(struct scp_state *s, const struct scp_request *reqs, int n, char *err, size_t err_size);

/* The decision for one link: input channel (1-based) into consumer. */
bool scp_wants_playback(const struct scp_state *s, int input, int channel, const char *consumer);

/* "live", "playback" or "mixed" (some channels or consumers differ). */
const char *scp_input_mode(const struct scp_state *s, int input);

/* The explicit table, for the SoundcheckChannels property: every channel's
 * default that is playback (consumer ""), and every override, for the
 * channels the input has. Calls fn per row (fn may be NULL); returns the
 * number of rows. */
int scp_for_each_row(const struct scp_state *s,
		     void (*fn)(void *data, const char *node, int channel, const char *consumer, bool playback),
		     void *data);

/* ---- the graph, as recorderd sees it */

/* One live link out of an input's source port. */
struct scp_link {
	uint32_t id;            /* the link's global id */
	int input;              /* index into scp_state.inputs */
	int channel;            /* 1-based source port position */
	const char *out_port;   /* the input's port name */
	const char *consumer;   /* consumer node.name */
	const char *consumer_port;
};

/* A link we moved (the record in $XDG_RUNTIME_DIR/backline/soundcheck.json). */
struct scp_move {
	char input[SCP_NAME];       /* the take's input node */
	char out_port[SCP_PORT];    /* its port name */
	char in_node[SCP_NAME];     /* consumer node and port */
	char in_port[SCP_PORT];
	int channel;                /* 1-based source channel; 0 = unknown (from a crashed run) */
	int track, sc_channel;      /* soundcheck port t<track>_<sc_channel>, 1-based; 0 = none */
	bool active;
};

enum scp_kind {
	SCP_MOVE,       /* link sc port → consumer port, destroy link `link_id`, record the move */
	SCP_KEEP,       /* a move that stands: make sure sc port → consumer port exists */
	SCP_RESTORE,    /* undo move `move`: unlink sc port → consumer, relink input → consumer */
};

struct scp_action {
	enum scp_kind kind;
	int move;                   /* KEEP, RESTORE: index into the moves */
	uint32_t link_id;           /* MOVE: the live link */
	bool add_record;            /* MOVE: false when a record for it stands already */
	struct scp_move rec;        /* MOVE: the record (to add when add_record) */
};

/* The plan that makes the graph match the state. Links into our own nodes
 * (SC_OURS_PREFIX) are never in it. A link already moved by a record
 * (same input port and consumer port) is not moved twice. Returns the number
 * of actions written (at most max), or -ENOSPC if more were needed. */
int scp_plan(const struct scp_state *s, const struct scp_link *links, int n_links,
	     const struct scp_move *moves, int n_moves, struct scp_action *out, int max);

#ifdef __cplusplus
}
#endif
#endif
