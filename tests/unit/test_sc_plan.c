/* Per-channel virtual soundcheck, offline: the link plan (recorder/sc_plan.h).
 * SPDX-License-Identifier: MIT
 *
 * A fake graph shaped like the recorder's: an 8-channel interface recorded as
 * one 8-channel track, a stereo synth, and a sink recorded from its monitor.
 * Consumers are a mixer ("console.engine"), a DAW ("daw") and the recorder
 * itself ("backline.recorder", never moved). The plan is carried out on the
 * fake graph the way recorderd.c does, so idempotence and restore are checked
 * end to end without PipeWire.
 */
#include "sc_plan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- a fake graph: links by name */
struct glink { char out_node[64], out_port[64], in_node[64], in_port[64]; uint32_t id; bool alive; };
static struct glink G[512];
static int n_g;
static uint32_t next_id = 100;

static void glink_add(const char *on, const char *op, const char *in, const char *ip)
{
	for (int i = 0; i < n_g; i++)
		if (G[i].alive && !strcmp(G[i].out_node, on) && !strcmp(G[i].out_port, op) && !strcmp(G[i].in_node, in) &&
		    !strcmp(G[i].in_port, ip))
			return;   /* bl_graph_link is idempotent too */
	struct glink *l = &G[n_g++];
	snprintf(l->out_node, 64, "%s", on);
	snprintf(l->out_port, 64, "%s", op);
	snprintf(l->in_node, 64, "%s", in);
	snprintf(l->in_port, 64, "%s", ip);
	l->id = next_id++;
	l->alive = true;
}

static void glink_del_id(uint32_t id)
{
	for (int i = 0; i < n_g; i++)
		if (G[i].alive && G[i].id == id)
			G[i].alive = false;
}

static void glink_del(const char *on, const char *op, const char *in, const char *ip)
{
	for (int i = 0; i < n_g; i++)
		if (G[i].alive && !strcmp(G[i].out_node, on) && !strcmp(G[i].out_port, op) && !strcmp(G[i].in_node, in) &&
		    !strcmp(G[i].in_port, ip))
			G[i].alive = false;
}

static bool linked(const char *on, const char *op, const char *in, const char *ip)
{
	for (int i = 0; i < n_g; i++)
		if (G[i].alive && !strcmp(G[i].out_node, on) && !strcmp(G[i].out_port, op) && !strcmp(G[i].in_node, in) &&
		    !strcmp(G[i].in_port, ip))
			return true;
	return false;
}

static int alive(void)
{
	int n = 0;
	for (int i = 0; i < n_g; i++)
		n += G[i].alive;
	return n;
}

/* The input's port name for channel c (1-based), as the graph names them. */
static const char *IFACE_PORTS[] = { "capture_AUX0", "capture_AUX1", "capture_AUX2", "capture_AUX3",
				     "capture_AUX4", "capture_AUX5", "capture_AUX6", "capture_AUX7" };
static const char *SYNTH_PORTS[] = { "output_FL", "output_FR" };
static const char *SINK_PORTS[] = { "monitor_FL", "monitor_FR" };
static const char *NODES[] = { "alsa_input.usb-iface", "synth", "playback-sink" };
static const int CHANS[] = { 8, 2, 2 };
static const char **PORTS[] = { IFACE_PORTS, SYNTH_PORTS, SINK_PORTS };
static const int NPORTS[] = { 8, 2, 2 };

static void build_world(void)
{
	n_g = 0;
	for (int n = 0; n < 3; n++)
		for (int c = 0; c < NPORTS[n]; c++) {
			char p[32];
			snprintf(p, sizeof(p), "in_%c_%d", 'a' + n, c + 1);
			glink_add(NODES[n], PORTS[n][c], "console.engine", p);
			snprintf(p, sizeof(p), "in_%d", n * 8 + c + 1);
			glink_add(NODES[n], PORTS[n][c], "daw", p);
			snprintf(p, sizeof(p), "t%d_%d", n + 1, c + 1);
			glink_add(NODES[n], PORTS[n][c], "backline.recorder", p);   /* the recorder itself */
		}
}

/* ---- recorderd's side: snapshot, plan, carry out */
static struct scp_move M[512];
static int n_m;

static int snapshot(const struct scp_state *s, struct scp_link *out)
{
	int n = 0;
	for (int i = 0; i < n_g; i++) {
		if (!G[i].alive)
			continue;
		for (int k = 0; k < 3; k++) {
			int in = scp_find_input(s, NODES[k]);
			if (in < 0 || strcmp(G[i].out_node, NODES[k]))
				continue;
			for (int c = 0; c < NPORTS[k]; c++)
				if (!strcmp(G[i].out_port, PORTS[k][c]))
					out[n++] = (struct scp_link){ G[i].id, in, c + 1, G[i].out_port, G[i].in_node, G[i].in_port };
		}
	}
	return n;
}

static int last_moves, last_restores;

static void carry_out(const struct scp_state *s)
{
	struct scp_link links[512];
	struct scp_action acts[1024];
	int nl = snapshot(s, links);
	int na = scp_plan(s, links, nl, M, n_m, acts, 1024);
	CHECK(na >= 0);
	last_moves = last_restores = 0;
	char sc[32];
	/* records first (recorderd saves them before touching the graph) */
	for (int a = 0; a < na; a++)
		if (acts[a].kind == SCP_MOVE && acts[a].add_record)
			M[n_m++] = acts[a].rec;
	for (int a = 0; a < na; a++) {
		const struct scp_action *x = &acts[a];
		if (x->kind == SCP_MOVE) {
			scp_port_name(x->rec.track, x->rec.sc_channel, sc, sizeof(sc));
			glink_add(SC_NODE_NAME, sc, x->rec.in_node, x->rec.in_port);
			glink_del_id(x->link_id);
			last_moves++;
		} else if (x->kind == SCP_KEEP) {
			const struct scp_move *mv = &M[x->move];
			scp_port_name(mv->track, mv->sc_channel, sc, sizeof(sc));
			glink_add(SC_NODE_NAME, sc, mv->in_node, mv->in_port);
		}
	}
	/* restores last, highest index first so indexes stay valid */
	for (int a = na - 1; a >= 0; a--) {
		if (acts[a].kind != SCP_RESTORE)
			continue;
		struct scp_move *mv = &M[acts[a].move];
		if (mv->track > 0) {
			scp_port_name(mv->track, mv->sc_channel, sc, sizeof(sc));
			glink_del(SC_NODE_NAME, sc, mv->in_node, mv->in_port);
		}
		glink_add(mv->input, mv->out_port, mv->in_node, mv->in_port);
		M[acts[a].move] = M[--n_m];
		last_restores++;
	}
}

static int set(struct scp_state *s, const char *node, uint32_t ch, const char *consumer, bool pb)
{
	struct scp_request r = { node, ch, consumer, pb };
	char err[200];
	int rc = scp_apply(s, &r, 1, err, sizeof(err));
	if (rc == 0)
		carry_out(s);
	return rc;
}

static void fresh(struct scp_state *s)
{
	build_world();
	n_m = 0;
	scp_init(s, 3, NODES, CHANS);
	for (int k = 0; k < 3; k++)
		s->inputs[k].ports = NPORTS[k];
}

int main(void)
{
	static struct scp_state s;
	char name[32];

	scp_port_name(3, 2, name, sizeof(name));
	CHECK(!strcmp(name, "t3_2"));
	CHECK(!strcmp(SC_NODE_NAME, "backline.soundcheck"));

	/* 1. one channel, every consumer: only that channel's consumer links move */
	fresh(&s);
	int before = alive();
	CHECK(set(&s, "alsa_input.usb-iface", 3, "", true) == 0);
	CHECK(last_moves == 2);   /* console.engine and daw; never the recorder */
	CHECK(!linked("alsa_input.usb-iface", "capture_AUX2", "console.engine", "in_a_3"));
	CHECK(!linked("alsa_input.usb-iface", "capture_AUX2", "daw", "in_3"));
	CHECK(linked(SC_NODE_NAME, "t1_3", "console.engine", "in_a_3"));
	CHECK(linked(SC_NODE_NAME, "t1_3", "daw", "in_3"));
	CHECK(linked("alsa_input.usb-iface", "capture_AUX2", "backline.recorder", "t1_3"));
	for (int c = 0; c < 8; c++)   /* every other channel of the same device untouched */
		if (c != 2) {
			char p[16];
			snprintf(p, sizeof(p), "in_a_%d", c + 1);
			CHECK(linked("alsa_input.usb-iface", IFACE_PORTS[c], "console.engine", p));
		}
	CHECK(alive() == before);
	CHECK(!strcmp(scp_input_mode(&s, 0), "mixed"));
	CHECK(!strcmp(scp_input_mode(&s, 1), "live"));

	/* 2. idempotent: the same switch again changes nothing */
	int g = n_g, m = n_m;
	CHECK(set(&s, "alsa_input.usb-iface", 3, "", true) == 0);
	CHECK(last_moves == 0 && last_restores == 0 && n_g == g && n_m == m);

	/* 3. one channel, one consumer: the other consumer keeps the live input */
	fresh(&s);
	CHECK(set(&s, "alsa_input.usb-iface", 5, "console.engine", true) == 0);
	CHECK(last_moves == 1);
	CHECK(linked(SC_NODE_NAME, "t1_5", "console.engine", "in_a_5"));
	CHECK(linked("alsa_input.usb-iface", "capture_AUX4", "daw", "in_5"));
	CHECK(!linked(SC_NODE_NAME, "t1_5", "daw", "in_5"));
	CHECK(scp_wants_playback(&s, 0, 5, "console.engine"));
	CHECK(!scp_wants_playback(&s, 0, 5, "daw"));
	CHECK(!scp_wants_playback(&s, 0, 4, "console.engine"));
	/* and back: exactly the original link returns */
	CHECK(set(&s, "alsa_input.usb-iface", 5, "console.engine", false) == 0);
	CHECK(last_restores == 1 && n_m == 0);
	CHECK(linked("alsa_input.usb-iface", "capture_AUX4", "console.engine", "in_a_5"));
	CHECK(!linked(SC_NODE_NAME, "t1_5", "console.engine", "in_a_5"));
	CHECK(!strcmp(scp_input_mode(&s, 0), "live"));

	/* 4. whole input (SetInput) then one channel back live for one consumer */
	fresh(&s);
	CHECK(set(&s, "synth", 0, NULL, true) == 0);
	CHECK(last_moves == 4);
	CHECK(!strcmp(scp_input_mode(&s, 1), "playback"));
	CHECK(set(&s, "synth", 2, "daw", false) == 0);
	CHECK(last_restores == 1 && last_moves == 0);
	CHECK(linked("synth", "output_FR", "daw", "in_10"));
	CHECK(linked(SC_NODE_NAME, "t2_2", "console.engine", "in_b_2"));
	CHECK(linked(SC_NODE_NAME, "t2_1", "daw", "in_9"));
	CHECK(!strcmp(scp_input_mode(&s, 1), "mixed"));
	/* a whole-input live clears every override */
	CHECK(set(&s, "synth", 0, "", false) == 0);
	CHECK(n_m == 0 && !strcmp(scp_input_mode(&s, 1), "live"));
	CHECK(linked("synth", "output_FL", "console.engine", "in_b_1"));

	/* 5. a sink's monitor ports (the recorder records a sink from its monitor) */
	fresh(&s);
	CHECK(set(&s, "playback-sink", 1, "", true) == 0);
	CHECK(linked(SC_NODE_NAME, "t3_1", "daw", "in_17"));
	CHECK(linked("playback-sink", "monitor_FR", "daw", "in_18"));

	/* 6. batch: all or nothing */
	fresh(&s);
	struct scp_request batch[] = {
		{ "alsa_input.usb-iface", 1, "", true },
		{ "alsa_input.usb-iface", 2, "", true },
		{ "not-an-input", 1, "", true },        /* bad: the whole batch is refused */
	};
	char err[200];
	static struct scp_state copy;
	memcpy(&copy, &s, sizeof(s));
	int rc = scp_apply(&s, batch, 3, err, sizeof(err));
	CHECK(rc == -3);
	CHECK(strstr(err, "not-an-input") != NULL);
	CHECK(!memcmp(&copy, &s, sizeof(s)));    /* state untouched */
	struct scp_request batch2[] = {
		{ "alsa_input.usb-iface", 1, "", true },
		{ "alsa_input.usb-iface", 9, "", true },   /* bad: 8 channels */
	};
	CHECK(scp_apply(&s, batch2, 2, err, sizeof(err)) == -2);
	CHECK(strstr(err, "8 channels") != NULL);
	CHECK(!memcmp(&copy, &s, sizeof(s)));
	/* a good batch lands in one plan */
	struct scp_request batch3[16];
	for (int c = 0; c < 8; c++)
		batch3[c] = (struct scp_request){ "alsa_input.usb-iface", (uint32_t)c + 1, "console.engine", true };
	batch3[8] = (struct scp_request){ "synth", 1, "console.engine", true };
	CHECK(scp_apply(&s, batch3, 9, err, sizeof(err)) == 0);
	carry_out(&s);
	CHECK(last_moves == 9);
	for (int c = 0; c < 8; c++) {
		char p[16], d[16], t[16];
		snprintf(p, sizeof(p), "in_a_%d", c + 1);
		snprintf(d, sizeof(d), "in_%d", c + 1);
		snprintf(t, sizeof(t), "t1_%d", c + 1);
		CHECK(linked(SC_NODE_NAME, t, "console.engine", p));
		CHECK(linked("alsa_input.usb-iface", IFACE_PORTS[c], "daw", d));   /* the DAW never noticed */
	}
	CHECK(!strcmp(scp_input_mode(&s, 0), "mixed"));
	/* the table lists exactly the overrides */
	CHECK(scp_for_each_row(&s, NULL, NULL) == 9);

	/* 7. a session manager relinks the input to a consumer in playback: moved
	 *    again, no second record */
	glink_add("alsa_input.usb-iface", "capture_AUX0", "console.engine", "in_a_1");
	int recs = n_m;
	carry_out(&s);
	CHECK(last_moves == 1 && n_m == recs);
	CHECK(!linked("alsa_input.usb-iface", "capture_AUX0", "console.engine", "in_a_1"));

	/* 8. a consumer that disappears and comes back gets the take again (KEEP) */
	glink_del(SC_NODE_NAME, "t1_2", "console.engine", "in_a_2");
	carry_out(&s);
	CHECK(linked(SC_NODE_NAME, "t1_2", "console.engine", "in_a_2"));

	/* 9. records from a crashed run (channel 0, track 0) all go back */
	fresh(&s);
	glink_del("synth", "output_FL", "daw", "in_9");
	n_m = 1;
	memset(&M[0], 0, sizeof(M[0]));
	snprintf(M[0].input, SCP_NAME, "synth");
	snprintf(M[0].out_port, SCP_PORT, "output_FL");
	snprintf(M[0].in_node, SCP_NAME, "daw");
	snprintf(M[0].in_port, SCP_PORT, "in_9");
	carry_out(&s);
	CHECK(last_restores == 1 && n_m == 0 && linked("synth", "output_FL", "daw", "in_9"));

	/* 10. a mono source in a 2-channel track: channel 1 → t<k>_1; channel > ports refused */
	{
		static struct scp_state m;
		const char *nodes[] = { "mono-mic" };
		int ch[] = { 2 };
		scp_init(&m, 1, nodes, ch);
		m.inputs[0].ports = 1;
		struct scp_request r = { "mono-mic", 2, "", true };
		CHECK(scp_apply(&m, &r, 1, err, sizeof(err)) == -1);
		r.channel = 1;
		CHECK(scp_apply(&m, &r, 1, err, sizeof(err)) == 0);
		struct scp_link l = { 7, 0, 1, "capture_MONO", "console.engine", "in_x_1" };
		struct scp_action a[4];
		CHECK(scp_plan(&m, &l, 1, NULL, 0, a, 4) == 1);
		CHECK(a[0].kind == SCP_MOVE && a[0].rec.track == 1 && a[0].rec.sc_channel == 1 && a[0].link_id == 7);
		/* too small an action buffer is reported, not truncated silently */
		CHECK(scp_plan(&m, &l, 1, NULL, 0, a, 0) < 0);
	}

	/* 11. two tracks from one node share one state and the first track's ports */
	{
		static struct scp_state d;
		const char *nodes[] = { "synth", "synth" };
		int ch[] = { 2, 2 };
		scp_init(&d, 2, nodes, ch);
		CHECK(d.n_inputs == 1 && d.inputs[0].track == 0);
	}

	/* 12. more consumers than slots: old unused ones are reclaimed */
	{
		static struct scp_state c;
		const char *nodes[] = { "synth" };
		int ch[] = { 2 };
		scp_init(&c, 1, nodes, ch);
		char cn[32];
		for (int i = 0; i < 200; i++) {
			snprintf(cn, sizeof(cn), "consumer-%d", i);
			struct scp_request on = { "synth", 1, cn, true }, off = { "synth", 1, cn, false };
			CHECK(scp_apply(&c, &on, 1, err, sizeof(err)) == 0);
			CHECK(scp_apply(&c, &off, 1, err, sizeof(err)) == 0);
		}
		CHECK(c.n_consumers <= SCP_MAX_CONSUMERS);
		int held = 0;
		for (int i = 0; i < SCP_MAX_CONSUMERS + 1; i++) {
			snprintf(cn, sizeof(cn), "held-%d", i);
			struct scp_request on = { "synth", 1, cn, true };
			held += scp_apply(&c, &on, 1, err, sizeof(err)) == 0;
		}
		CHECK(held == SCP_MAX_CONSUMERS);   /* the 65th live override is refused, cleanly */
		CHECK(scp_wants_playback(&c, 0, 1, "held-0") && !scp_wants_playback(&c, 0, 1, "consumer-3"));
	}

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("sc_plan: per-channel soundcheck plan OK\n");
	return 0;
}
