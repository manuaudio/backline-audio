/* backline virtual soundcheck: the per-channel, per-consumer link plan (sc_plan.h).
 * SPDX-License-Identifier: MIT
 */
#include "sc_plan.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void scp_port_name(int track, int channel, char *out, size_t n)
{
	snprintf(out, n, "t%d_%d", track, channel);
}

void scp_init(struct scp_state *s, int n_tracks, const char *const *nodes, const int *channels)
{
	memset(s, 0, sizeof(*s));
	for (int k = 0; k < n_tracks && k < SCP_MAX_TRACKS; k++) {
		if (scp_find_input(s, nodes[k]) >= 0)
			continue;   /* the first track from a node is the one its moves use */
		struct scp_input *in = &s->inputs[s->n_inputs++];
		snprintf(in->node, sizeof(in->node), "%s", nodes[k]);
		in->track = k;
		in->channels = channels[k] > 0 ? channels[k] : 1;
	}
}

int scp_find_input(const struct scp_state *s, const char *node)
{
	for (int i = 0; node && i < s->n_inputs; i++)
		if (!strcmp(s->inputs[i].node, node))
			return i;
	return -1;
}

static int find_consumer(const struct scp_state *s, const char *name)
{
	for (int k = 0; k < s->n_consumers; k++)
		if (!strcmp(s->consumers[k], name))
			return k;
	return -1;
}

static bool consumer_used(const struct scp_state *s, int k)
{
	for (int i = 0; i < s->n_inputs; i++)
		for (int c = 0; c < SCP_MAX_PORTS; c++)
			if (s->inputs[i].over_set[c] & (1ull << k))
				return true;
	return false;
}

/* Drop consumers no override names any more; renumber the bits. */
static void compact_consumers(struct scp_state *s)
{
	int to = 0;
	for (int k = 0; k < s->n_consumers; k++) {
		if (!consumer_used(s, k))
			continue;
		if (to != k) {
			memcpy(s->consumers[to], s->consumers[k], SCP_NAME);
			for (int i = 0; i < s->n_inputs; i++)
				for (int c = 0; c < SCP_MAX_PORTS; c++) {
					struct scp_input *in = &s->inputs[i];
					uint64_t from = 1ull << k, dst = 1ull << to;
					in->over_set[c] = (in->over_set[c] & ~dst) | ((in->over_set[c] & from) ? dst : 0);
					in->over_val[c] = (in->over_val[c] & ~dst) | ((in->over_val[c] & from) ? dst : 0);
					in->over_set[c] &= ~from;
					in->over_val[c] &= ~from;
				}
		}
		to++;
	}
	s->n_consumers = to;
}

static int consumer_slot(struct scp_state *s, const char *name)
{
	int k = find_consumer(s, name);
	if (k >= 0)
		return k;
	if (s->n_consumers == SCP_MAX_CONSUMERS)
		compact_consumers(s);
	if (s->n_consumers == SCP_MAX_CONSUMERS)
		return -1;
	k = s->n_consumers++;
	snprintf(s->consumers[k], SCP_NAME, "%s", name);
	return k;
}

static int max_channel(const struct scp_input *in)
{
	int m = in->ports > 0 ? in->ports : in->channels;
	return m > SCP_MAX_PORTS ? SCP_MAX_PORTS : m;
}

static int apply_one(struct scp_state *s, const struct scp_request *r, char *err, size_t n)
{
	int i = scp_find_input(s, r->node);
	if (i < 0) {
		snprintf(err, n, "\"%s\" is not an input of the take", r->node ? r->node : "");
		return -ENOENT;
	}
	struct scp_input *in = &s->inputs[i];
	int max = max_channel(in);
	if (r->channel > (uint32_t)max) {
		snprintf(err, n, "%s has %d channel%s, not %u", in->node, max, max == 1 ? "" : "s", r->channel);
		return -EINVAL;
	}
	bool every = !r->consumer || !*r->consumer;
	if (!every && strlen(r->consumer) >= SCP_NAME) {
		snprintf(err, n, "consumer name too long");
		return -EINVAL;
	}
	/* channel 0: every channel, also ports the input doesn't have yet */
	int lo = r->channel ? (int)r->channel - 1 : 0, hi = r->channel ? (int)r->channel : SCP_MAX_PORTS;
	if (every) {
		for (int c = lo; c < hi; c++) {
			in->def = r->playback ? in->def | (1ull << c) : in->def & ~(1ull << c);
			in->over_set[c] = in->over_val[c] = 0;
		}
		return 0;
	}
	int k = consumer_slot(s, r->consumer);
	if (k < 0) {
		snprintf(err, n, "more than %d consumers with their own soundcheck setting", SCP_MAX_CONSUMERS);
		return -ENOSPC;
	}
	uint64_t bit = 1ull << k;
	for (int c = lo; c < hi; c++) {
		bool def = in->def & (1ull << c);
		if (def == r->playback) {   /* same as the default: no override needed */
			in->over_set[c] &= ~bit;
			in->over_val[c] &= ~bit;
		} else {
			in->over_set[c] |= bit;
			in->over_val[c] = r->playback ? in->over_val[c] | bit : in->over_val[c] & ~bit;
		}
	}
	return 0;
}

int scp_apply(struct scp_state *s, const struct scp_request *reqs, int n, char *err, size_t err_size)
{
	char dummy[8];
	if (!err) {
		err = dummy;
		err_size = sizeof(dummy);
	}
	struct scp_state *w = malloc(sizeof(*w));   /* main thread: work on a copy */
	if (!w) {
		snprintf(err, err_size, "out of memory");
		return -ENOMEM;
	}
	memcpy(w, s, sizeof(*w));
	for (int i = 0; i < n; i++) {
		if (apply_one(w, &reqs[i], err, err_size) < 0) {
			free(w);
			return -(i + 1);
		}
	}
	memcpy(s, w, sizeof(*s));
	free(w);
	return 0;
}

bool scp_wants_playback(const struct scp_state *s, int input, int channel, const char *consumer)
{
	if (input < 0 || input >= s->n_inputs || channel < 1 || channel > SCP_MAX_PORTS)
		return false;
	const struct scp_input *in = &s->inputs[input];
	int c = channel - 1;
	int k = consumer ? find_consumer(s, consumer) : -1;
	if (k >= 0 && (in->over_set[c] & (1ull << k)))
		return in->over_val[c] & (1ull << k);
	return in->def & (1ull << c);
}

const char *scp_input_mode(const struct scp_state *s, int input)
{
	if (input < 0 || input >= s->n_inputs)
		return "live";
	const struct scp_input *in = &s->inputs[input];
	int max = max_channel(in);
	uint64_t all = max >= 64 ? ~0ull : (1ull << max) - 1;
	bool overrides = false;
	for (int c = 0; c < SCP_MAX_PORTS; c++)
		overrides |= in->over_set[c] != 0;
	if (!overrides && (in->def & all) == 0)
		return "live";
	if (!overrides && (in->def & all) == all)
		return "playback";
	return "mixed";
}

int scp_for_each_row(const struct scp_state *s,
		     void (*fn)(void *data, const char *node, int channel, const char *consumer, bool playback),
		     void *data)
{
	int rows = 0;
	for (int i = 0; i < s->n_inputs; i++) {
		const struct scp_input *in = &s->inputs[i];
		for (int c = 0; c < max_channel(in); c++) {
			if (in->def & (1ull << c)) {
				if (fn) fn(data, in->node, c + 1, "", true);
				rows++;
			}
			for (int k = 0; k < s->n_consumers; k++)
				if (in->over_set[c] & (1ull << k)) {
					if (fn) fn(data, in->node, c + 1, s->consumers[k], in->over_val[c] & (1ull << k));
					rows++;
				}
		}
	}
	return rows;
}

static bool ours(const char *node)
{
	return node && !strncmp(node, SC_OURS_PREFIX, strlen(SC_OURS_PREFIX));
}

static bool same_target(const struct scp_move *m, const char *input, const char *out_port, const char *node, const char *port)
{
	return !strcmp(m->input, input) && !strcmp(m->out_port, out_port) && !strcmp(m->in_node, node) &&
	       !strcmp(m->in_port, port);
}

int scp_plan(const struct scp_state *s, const struct scp_link *links, int n_links,
	     const struct scp_move *moves, int n_moves, struct scp_action *out, int max)
{
	int n = 0;
	bool full = false;
#define EMIT(a) do { if (n < max) out[n++] = (a); else full = true; } while (0)

	/* the records: each one stands (KEEP) or goes back (RESTORE) */
	for (int m = 0; m < n_moves; m++) {
		const struct scp_move *mv = &moves[m];
		int i = scp_find_input(s, mv->input);
		bool keep = i >= 0 && mv->channel > 0 && mv->track > 0 &&
			    scp_wants_playback(s, i, mv->channel, mv->in_node);
		struct scp_action a = { .kind = keep ? SCP_KEEP : SCP_RESTORE, .move = m };
		EMIT(a);
	}
	/* live links that should carry the take and don't yet */
	for (int l = 0; l < n_links; l++) {
		const struct scp_link *lk = &links[l];
		if (lk->input < 0 || lk->input >= s->n_inputs || ours(lk->consumer))
			continue;
		if (!scp_wants_playback(s, lk->input, lk->channel, lk->consumer))
			continue;
		const struct scp_input *in = &s->inputs[lk->input];
		bool recorded = false;   /* already moved by a standing record, or by an earlier action here */
		for (int m = 0; m < n_moves && !recorded; m++)
			recorded = same_target(&moves[m], in->node, lk->out_port, lk->consumer, lk->consumer_port);
		for (int a = 0; a < n && !recorded; a++)
			recorded = out[a].kind == SCP_MOVE &&
				   same_target(&out[a].rec, in->node, lk->out_port, lk->consumer, lk->consumer_port);
		struct scp_action a = { .kind = SCP_MOVE, .link_id = lk->id };
		struct scp_move *r = &a.rec;
		snprintf(r->input, sizeof(r->input), "%s", in->node);
		snprintf(r->out_port, sizeof(r->out_port), "%s", lk->out_port);
		snprintf(r->in_node, sizeof(r->in_node), "%s", lk->consumer);
		snprintf(r->in_port, sizeof(r->in_port), "%s", lk->consumer_port);
		r->channel = lk->channel;
		r->track = in->track + 1;
		r->sc_channel = (lk->channel - 1 < in->channels - 1 ? lk->channel - 1 : in->channels - 1) + 1;
		r->active = true;
		/* recorded: the input→consumer link came back (a session manager
		 * relinked it). Move it again, but don't add a second record. */
		a.add_record = !recorded;
		EMIT(a);
	}
#undef EMIT
	return full ? -ENOSPC : n;
}
