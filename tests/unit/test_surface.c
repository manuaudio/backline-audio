/* The surface layer end to end, without hardware and without PipeWire: the
 * MCU protocol, the shipped profiles, and the engine driving two software
 * devices (an X-Touch Extender and an X-Touch, tools/mcusim.c) as one
 * 16-strip group against a fake mixer's Params1 tree, in simulated time.
 * SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "bl_midibuf.h"
#include "engine.h"
#include "mcu.h"
#include "mcusim.h"
#include "params.h"
#include "profile.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define MS 1000000ull

/* ------------------------------------------------------------ protocol */
static void test_mcu(void)
{
	uint8_t m[160];
	CHECK(mcu_device_query(0x14, m, sizeof(m)) == 7 && !memcmp(m, "\xf0\x00\x00\x66\x14\x00\xf7", 7));
	CHECK(mcu_lcd(0x15, 56, "AB", 2, m, sizeof(m)) == 10 && m[4] == 0x15 && m[5] == 0x12 && m[6] == 56 && m[7] == 'A');
	CHECK(mcu_lcd(0x14, 111, "AB", 2, m, sizeof(m)) == 0); /* past the end */
	uint8_t c[8] = { 1, 2, 3, 4, 5, 6, 7, 0 };
	CHECK(mcu_colours(0x14, c, m, sizeof(m)) == 15 && m[5] == 0x72 && m[6] == 1 && m[13] == 0);
	CHECK(mcu_fader(8, 16383, m) == 3 && m[0] == 0xe8 && m[1] == 0x7f && m[2] == 0x7f);
	CHECK(mcu_led(0x32, 1, m) == 3 && m[2] == 0x01);
	CHECK(mcu_meter(3, 11, m) == 2 && m[0] == 0xd0 && m[1] == 0x3b);
	CHECK(mcu_ring_byte(MCU_RING_DOT, 0, false, false) == 0x01 && mcu_ring_byte(MCU_RING_DOT, 1, false, false) == 0x0b);
	CHECK(mcu_ring_byte(MCU_RING_SPREAD, 1, true, false) == (0x40 | 0x30 | 6));
	CHECK(mcu_ring_byte(MCU_RING_WRAP, 0.5, false, true) == 0x20);
	CHECK(mcu_7seg('a', false) == 0x01 && mcu_7seg('1', true) == (0x31 | 0x40) && mcu_7seg(' ', false) == 0x20);
	CHECK(mcu_meter_level(-200) == 0 && mcu_meter_level(-60) == 1 && mcu_meter_level(0) == 11 && mcu_meter_level(6) == 12);
	CHECK(mcu_colour_nearest(255, 0, 0) == 1 && mcu_colour_nearest(0, 0, 255) == 4 && mcu_colour_nearest(250, 250, 240) == 7 &&
	      mcu_colour_nearest(0, 0, 0) == 0 && mcu_colour_nearest(200, 180, 20) == 3);
	char ab[8];
	mcu_abbrev("Kick In", ab, 6);
	CHECK(!strcmp(ab, "KickIn"));
	mcu_abbrev("Snare Top", ab, 6);
	CHECK(!strcmp(ab, "SnrTp") || strlen(ab) <= 6);
	mcu_abbrev("Vox", ab, 6);
	CHECK(!strcmp(ab, "Vox"));
	/* decode */
	struct mcu_in in;
	uint8_t touch[3] = { 0x90, 0x6a, 0x7f }, rel[3] = { 0x90, 0x6a, 0x00 }, pb[3] = { 0xe3, 0x00, 0x40 }, pot[3] = { 0xb0, 0x12, 0x43 };
	CHECK(mcu_decode(touch, 3, &in) && in.kind == MCU_IN_TOUCH && in.index == 2 && in.down);
	CHECK(mcu_decode(rel, 3, &in) && in.kind == MCU_IN_TOUCH && !in.down);
	CHECK(mcu_decode(pb, 3, &in) && in.kind == MCU_IN_FADER && in.index == 3 && in.value == 8192);
	CHECK(mcu_decode(pot, 3, &in) && in.kind == MCU_IN_VPOT && in.index == 2 && in.delta == -3);
	uint8_t q[] = { 0xf0, 0, 0, 0x66, 0x14, 0x01, 'A', 'B', 'C', '1', '2', '3', '4', 1, 2, 3, 4, 0xf7 };
	CHECK(mcu_decode(q, sizeof(q), &in) && in.kind == MCU_IN_HOST_QUERY && in.serial[0] == 'A' && in.challenge[3] == 4);
	char st[32];
	mcu_serial_text(in.serial, st, sizeof(st));
	CHECK(!strcmp(st, "ABC1234"));
	/* every MCU note id has a name, and names round-trip */
	int named = 0;
	for (int id = 0; id <= 0x76; id++)
		if (mcu_control_name(id)) {
			named++;
			CHECK(mcu_control_id(mcu_control_name(id)) == id);
		}
	CHECK(named == 0x74 + 1); /* 00-73 and 76 */
	/* Logic Control challenge: a known vector from the formula */
	uint8_t ch[4] = { 0x10, 0x20, 0x30, 0x01 }, r[4];
	mcu_lc_response(ch, r);
	CHECK(r[0] == (0x7f & (0x10 + (0x20 ^ 0x0a) - 0x01)) && r[3] == (0x7f & (0x20 - 0x30 + (0xf0 ^ (0x01 << 4)))));
}

struct got {
	int n;
	size_t len[16];
	uint8_t first[16];
};

static void got_cb(void *d, const uint8_t *msg, size_t n)
{
	struct got *g = d;
	if (g->n < 16) {
		g->len[g->n] = n;
		g->first[g->n] = msg[0];
	}
	g->n++;
}

static void test_midi(void)
{
	struct bl_midi_parser p;
	bl_midi_parser_init(&p);
	struct got g = { 0 };
	uint8_t a[] = { 0xf0, 0x00, 0x00, 0x66 }, b[] = { 0x14, 0xf8, 0x12, 0x00, 'H', 'i', 0xf7, 0x90, 0x10, 0x7f, 0x11, 0x00 };
	bl_midi_parse(&p, a, sizeof(a), got_cb, &g);
	CHECK(g.n == 0);
	bl_midi_parse(&p, b, sizeof(b), got_cb, &g);
	CHECK(g.n == 4 && g.first[0] == 0xf8 && g.len[1] == 10 && g.first[1] == 0xf0 && g.len[2] == 3 && g.len[3] == 3 && g.first[3] == 0x90);
	struct bl_midi_ring q;
	CHECK(bl_midi_ring_init(&q, 64) == 0);
	uint8_t m[3] = { 1, 2, 3 }, out[8];
	int pushed = 0;
	while (bl_midi_ring_push(&q, m, 3))
		pushed++;
	CHECK(pushed == 12 && q.dropped == 1); /* 64 bytes / (2 + 3) */
	CHECK(bl_midi_ring_pop(&q, out, sizeof(out)) == 3 && out[2] == 3);
	CHECK(bl_midi_ring_push(&q, m, 3));
	bl_midi_ring_free(&q);
}

/* ------------------------------------------------------------ the rig */
#define NCH 24
static struct prm_store *store;
static struct sf_engine *eng;
static struct mcusim sims[2];
static int unit_of_sim[2] = { -1, -1 }, sim_of_unit[16];
static bool plugged[2];
static uint64_t now = 1000 * MS;
static int sets, gestures_begin, gestures_end;
static char last_gesture[256], last_action[128];
static float meter_db[NCH];
static uint8_t pending[2][4096];
static size_t pending_len[2];

static void put_param(const char *js)
{
	char err[256];
	json_object *o = json_tokener_parse(js);
	struct prm *p;
	if (prm_from_json(o, "/mix", &p, err, sizeof(err)) < 0) {
		fprintf(stderr, "test param: %s\n", err);
		exit(2);
	}
	json_object_put(o);
	prm_store_put(store, p);
}

static int attaching = -1; /* the sim whose unit is being attached (its index isn't known yet) */

static void h_send(void *d, int unit, const uint8_t *msg, size_t n)
{
	int s = sim_of_unit[unit] >= 0 ? sim_of_unit[unit] : attaching;
	if (s >= 0 && plugged[s])
		mcusim_receive(&sims[s], msg, n);
}

static void h_set(void *d, const char *path, double norm, const char *role, const char *client)
{
	struct prm *p = prm_store_get(store, path);
	if (!p)
		return;
	sets++;
	p->value = prm_from_norm(p, norm); /* the mixer applies it and confirms */
	prm_refresh_text(p);
}

static void h_gesture(void *d, const char *path, bool begin, const char *role, const char *client)
{
	if (begin) gestures_begin++;
	else gestures_end++;
	snprintf(last_gesture, sizeof(last_gesture), "%s %s", begin ? "begin" : "end", path);
}

static void h_action(void *d, const char *group, const char *name, bool down)
{
	if (down)
		snprintf(last_action, sizeof(last_action), "%s", name);
}

static bool h_meter(void *d, const char *root, const char *id, float *db, bool *clip)
{
	int n;
	if (strcmp(root, "/mix") || sscanf(id, "in.%d", &n) != 1 || n < 1 || n > NCH)
		return false;
	*db = meter_db[n - 1];
	*clip = meter_db[n - 1] > 0;
	return true;
}

static int logs;
static void h_log(void *d, const char *m)
{
	logs++;
	if (getenv("SURFACE_TEST_VERBOSE"))
		fprintf(stderr, "  surface: %s\n", m);
}

/* the device's bytes reach the engine on the next pump */
static void sim_out(void *d, const uint8_t *m, size_t n)
{
	int s = (int)(intptr_t)d;
	if (pending_len[s] + n + 2 > sizeof(pending[s]))
		return;
	pending[s][pending_len[s]++] = (uint8_t)n;
	memcpy(pending[s] + pending_len[s], m, n);
	pending_len[s] += n;
}

static void pump(int ms)
{
	for (int t = 0; t <= ms; t += 5) {
		for (int s = 0; s < 2; s++) {
			size_t k = 0;
			while (k < pending_len[s]) {
				size_t n = pending[s][k++];
				if (plugged[s] && unit_of_sim[s] >= 0)
					sf_unit_input(eng, unit_of_sim[s], pending[s] + k, n, now);
				k += n;
			}
			pending_len[s] = 0;
		}
		sf_tick(eng, now);
		now += 5 * MS;
	}
}

static void plug(int s, const char *port)
{
	const struct sf_profile *p = sf_profile_for_port(eng, port);
	CHECK(p != NULL);
	if (!p)
		return;
	plugged[s] = true;
	mcusim_reset(&sims[s]);
	attaching = s;
	unit_of_sim[s] = sf_unit_attach(eng, p, port, now);
	attaching = -1;
	sim_of_unit[unit_of_sim[s]] = s;
}

static void unplug(int s)
{
	plugged[s] = false;
	sf_unit_detach(eng, unit_of_sim[s], now);
	sim_of_unit[unit_of_sim[s]] = -1;
	unit_of_sim[s] = -1;
}

static const char *cell(int s, int line, int strip)
{
	static char c[8];
	mcusim_lcd_cell(&sims[s], line, strip, c);
	return c;
}

static double norm_of(const char *path)
{
	struct prm *p = prm_store_get(store, path);
	return p ? prm_to_norm(p, p->value) : -1;
}

static const char *VIEWS =
	"{\"schema\":\"backline.views/1\",\"views\":["
	"{\"id\":\"inputs\",\"label\":\"Inputs\",\"code\":\"In\",\"layer\":2,\"slots\":[%s],"
	" \"master\":{\"fader\":\"/mix/main/fader\",\"name\":\"Main\"}},"
	"{\"id\":\"aux\",\"label\":\"Aux\",\"code\":\"Au\",\"layer\":5,\"slots\":["
	"  {\"id\":\"aux.1\",\"name\":\"Wedge 1\",\"fader\":\"/mix/aux.1/fader\",\"colour\":\"#20ff20\"},"
	"  {\"id\":\"aux.2\",\"name\":\"Wedge 2\",\"fader\":\"/mix/aux.2/fader\",\"colour\":\"#20ff20\"}]}],"
	"\"pot-modes\":["
	"{\"id\":\"pan\",\"label\":\"Pan\",\"binding\":\"pan\",\"ring\":\"dot\",\"push\":\"reset\"},"
	"{\"id\":\"send\",\"label\":\"Send\",\"binding\":\"send\",\"ring\":\"wrap\"},"
	"{\"id\":\"eq\",\"label\":\"EQ\",\"strip\":true,\"pages\":[[{\"path\":\"/mix/{slot}/eq.lf\",\"label\":\"LF G\",\"ring\":\"boost-cut\"},"
	"  {\"path\":\"/mix/{slot}/pan\",\"label\":\"Pan\"}]]}],"
	"\"home\":{\"view\":\"inputs\",\"pot-mode\":\"pan\"}}";

static const char *names[NCH] = { "Kick In", "Kick Out", "Snare", "Snare Bot", "HiHat", "Tom 1", "Tom 2", "Floor",
				  "OH L", "OH R", "Bass DI", "Bass Mic", "Gtr L", "Gtr R", "Keys L", "Keys R",
				  "Vox Lead", "Vox BV1", "Vox BV2", "Sax", "Tpt", "Trb", "Click", "TB" };
static const char *colours[] = { "#e02020", "#2020e0", "#e0e020", "#20e0e0" };

static void build(void)
{
	store = prm_store_new();
	char js[512];
	for (int i = 1; i <= NCH; i++) {
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/fader\",\"type\":\"float\",\"min\":-144,\"max\":10,\"unit\":\"dB\","
			 "\"taper\":\"db-fader\",\"flags\":[\"automatable\"],\"value\":%d}", i, -i);
		put_param(js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/pan\",\"type\":\"float\",\"min\":-1,\"max\":1,\"default\":0,\"flags\":[\"automatable\"]}", i);
		put_param(js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/send\",\"type\":\"float\",\"min\":-144,\"max\":10,\"taper\":\"db-fader\"}", i);
		put_param(js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/eq.lf\",\"type\":\"float\",\"min\":-15,\"max\":15,\"default\":0,\"unit\":\"dB\",\"short\":\"LF G\"}", i);
		put_param(js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/mute\",\"type\":\"bool\"}", i);
		put_param(js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/solo\",\"type\":\"bool\"}", i);
		put_param(js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/name\",\"type\":\"string\",\"value\":\"%s\"}", i, names[i - 1]);
		put_param(js);
	}
	put_param("{\"path\":\"main/fader\",\"type\":\"float\",\"min\":-144,\"max\":10,\"taper\":\"db-fader\",\"flags\":[\"automatable\"]}");
	put_param("{\"path\":\"aux.1/fader\",\"type\":\"float\",\"min\":-144,\"max\":10,\"taper\":\"db-fader\",\"value\":0}");
	put_param("{\"path\":\"aux.2/fader\",\"type\":\"float\",\"min\":-144,\"max\":10,\"taper\":\"db-fader\",\"value\":-10}");
	put_param("{\"path\":\"clock/tc\",\"type\":\"string\",\"value\":\"01:02:03.04\"}");
}

static char *views_json(void)
{
	static char slots[16384];
	size_t k = 0;
	for (int i = 1; i <= NCH; i++)
		k += (size_t)snprintf(slots + k, sizeof(slots) - k,
				      "%s{\"id\":\"in.%d\",\"name\":\"/mix/in.%d/name\",\"colour\":\"%s\",\"fader\":\"/mix/in.%d/fader\","
				      "\"pots\":{\"pan\":\"/mix/in.%d/pan\",\"send\":\"/mix/in.%d/send\"},\"mute\":\"/mix/in.%d/mute\","
				      "\"solo\":\"/mix/in.%d/solo\",\"select\":{\"action\":\"select\"},\"meter\":\"/mix:in.%d\"}",
				      i > 1 ? "," : "", i, i, colours[i % 4], i, i, i, i, i, i);
	char *out = malloc(32768);
	snprintf(out, 32768, VIEWS, slots);
	return out;
}

static void test_engine(void)
{
	struct sf_host host = { h_send, h_set, h_gesture, h_action, NULL, NULL, h_meter, NULL, h_log };
	for (int i = 0; i < 16; i++)
		sim_of_unit[i] = -1;
	build();
	eng = sf_new(&host, NULL, store);
	CHECK(sf_load_profile_dir(eng, BL_SURFACE_SRCDIR) == 4);
	CHECK(sf_profile_by_id(eng, "behringer-xtouch") && sf_profile_by_id(eng, "behringer-xtouch-ext") &&
	      sf_profile_by_id(eng, "mackie-mcu-pro") && sf_profile_by_id(eng, "mackie-mcu-xt"));
	/* detection by port name */
	const struct sf_profile *xt = sf_profile_for_port(eng, "Midi-Bridge:X-Touch:(capture_0) X-Touch MIDI 1 | X-Touch:X-Touch MIDI 1");
	const struct sf_profile *xe = sf_profile_for_port(eng, "Midi-Bridge:X-Touch-Ext:(capture_0) X-Touch-Ext MIDI 1 | X-Touch-Ext:X-Touch-Ext MIDI 1");
	CHECK(xt && !strcmp(xt->id, "behringer-xtouch") && xe && !strcmp(xe->id, "behringer-xtouch-ext"));
	CHECK(!sf_profile_for_port(eng, "Midi-Bridge:Midi Through:(capture_0) Midi Through Port-0"));
	CHECK(!sf_profile_for_port(eng, "Midi-Bridge:X-Touch One:(capture_0) X-Touch One MIDI 1"));
	const struct sf_profile *mp = sf_profile_for_port(eng, "Midi-Bridge:MCU Pro USB v3.1:(capture_0) MCU Pro USB v3.1 MIDI 1");
	const struct sf_profile *mx = sf_profile_for_port(eng, "Midi-Bridge:MCU XT Pro USB:(capture_0) MCU XT Pro USB MIDI 1");
	CHECK(mp && !strcmp(mp->id, "mackie-mcu-pro") && mx && !strcmp(mx->id, "mackie-mcu-xt"));
	char e2[256];
	CHECK(sf_set_views(eng, "*", views_json(), e2, sizeof(e2)) == 0);

	/* 1. two units, extender first: one group of 16, [ext, x-touch] */
	mcusim_init(&sims[0], "xtouch-ext", "EXT0001", sim_out, (void *)0);
	mcusim_init(&sims[1], "xtouch", "XT00002", sim_out, (void *)1);
	plug(0, "test:X-Touch-Ext MIDI 1");
	plug(1, "test:X-Touch MIDI 1");
	pump(20);
	CHECK(sf_unit_state(eng, unit_of_sim[0]) == SF_ONLINE && sf_unit_state(eng, unit_of_sim[1]) == SF_ONLINE);
	CHECK(!strcmp(sf_unit_id(eng, unit_of_sim[0]), "behringer-xtouch-ext@EXT0001"));
	CHECK(!strcmp(sf_unit_id(eng, unit_of_sim[1]), "behringer-xtouch@XT00002"));
	const char *g = sf_unit_group(eng, unit_of_sim[1]);
	char gid[64];
	snprintf(gid, sizeof(gid), "%s", g);
	CHECK(!strcmp(gid, sf_unit_group(eng, unit_of_sim[0])) && sf_group_width(eng, gid) == 16);
	CHECK(sims[0].connected && sims[1].connected && sims[0].rx_query == 1);
	/* the greeting, then the full redraw */
	pump(300);
	CHECK(strstr(sims[1].lcd, "Backline") != NULL);
	pump(1200);
	CHECK(!strcmp(cell(0, 0, 0), "KickIn") && !strcmp(cell(0, 0, 1), "KickOt") && !strcmp(cell(1, 0, 0), "OH L"));
	CHECK(!strcmp(cell(1, 0, 7), "KeysR") || !strcmp(cell(1, 0, 7), "Keys R") || strlen(cell(1, 0, 7)) > 0);
	CHECK(!strcmp(cell(0, 1, 0), "0.0")); /* line 2: the pan value */
	/* colours to the X-Touch's 7: in.1 blue, in.2 yellow, in.3 cyan, in.4 red; in.9 blue */
	CHECK(sims[0].colour[0] == 4 && sims[0].colour[1] == 3 && sims[0].colour[2] == 6 && sims[0].colour[3] == 1 && sims[1].colour[0] == 4);
	/* faders where the parameters are (10-bit devices: within 16) */
	struct prm *f3 = prm_store_get(store, "/mix/in.3/fader");
	CHECK(abs(sims[0].fader[2] - (int)lround(prm_to_norm(f3, f3->value) * 16383)) <= 16);
	CHECK(sims[1].fader[8] == (int)lround(norm_of("/mix/main/fader") * 16383)); /* master fader */
	char as[3];
	mcusim_assign_text(&sims[1], as);
	CHECK(!strcmp(as, "IN"));

	/* 2. a fader move, both ways: touch -> gesture, move -> parameter, release -> motor holds the value */
	int s0 = sets;
	mcusim_move(&sims[1], 2, 12000); /* untouched: never applied (motor overshoot rule) */
	pump(20);
	CHECK(sets == s0);
	mcusim_touch(&sims[1], 2, true);
	pump(10);
	CHECK(gestures_begin == 1 && !strcmp(last_gesture, "begin /mix/in.11/fader"));
	for (int v = 9000; v <= 12000; v += 500) {
		mcusim_move(&sims[1], 2, v);
		pump(10);
	}
	CHECK(sets > s0 && fabs(norm_of("/mix/in.11/fader") - (12000 & ~0x0f) / 16383.0) < 1e-9);
	CHECK(sims[1].host_fader[2] == (12000 & ~0x0f)); /* echoed back while held: the servo holds */
	CHECK(!strncmp(cell(1, 1, 2), "-", 1) || strstr(cell(1, 1, 2), "."));  /* line 2 shows the fader value while touched */
	mcusim_touch(&sims[1], 2, false);
	pump(50);
	CHECK(gestures_end == 1 && !strcmp(last_gesture, "end /mix/in.11/fader"));
	CHECK(abs(sims[1].fader[2] - (12000 & ~0x0f)) <= 1);
	/* the mixer moves a parameter: the motor follows */
	struct prm *f5 = prm_store_get(store, "/mix/in.5/fader");
	f5->value = 0; /* 0 dB = 0.75 */
	pump(50);
	CHECK(abs(sims[0].fader[4] - (int)lround(0.75 * 16383)) <= 1);

	/* 3. LEDs: mute on the extender's strip 2 */
	mcusim_press(&sims[0], MCU_MUTE + 1, true);
	mcusim_press(&sims[0], MCU_MUTE + 1, false);
	pump(20);
	CHECK(norm_of("/mix/in.2/mute") == 1 && sims[0].led[MCU_MUTE + 1] == 2);
	mcusim_press(&sims[0], MCU_MUTE + 1, true);
	mcusim_press(&sims[0], MCU_MUTE + 1, false);
	pump(20);
	CHECK(norm_of("/mix/in.2/mute") == 0 && sims[0].led[MCU_MUTE + 1] == 0);
	/* select is an action for the program */
	mcusim_press(&sims[1], MCU_SELECT + 0, true);
	pump(10);
	CHECK(!strcmp(last_action, "select:in.9"));
	sf_set_selected(eng, gid, "in.9", e2, sizeof(e2));
	pump(20);
	CHECK(sims[1].led[MCU_SELECT] == 2);

	/* 4. meters: levels and clip */
	meter_db[0] = -6;
	meter_db[9] = 3;
	pump(200);
	CHECK(sims[0].meter[0] == mcu_meter_level(-6) && sims[1].meter[1] == mcu_meter_level(3) && sims[1].clip[1]);
	meter_db[9] = -100;
	pump(2200);
	CHECK(!sims[1].clip[1]); /* clip held 2 s, then cleared */

	/* 5. banking across the 16-wide group (24 slots: last bank starts at 8) */
	mcusim_press(&sims[1], 0x2f, true); /* bank right */
	mcusim_press(&sims[1], 0x2f, false);
	pump(200);
	CHECK(sf_group_offset(eng, gid) == 8 && !strcmp(cell(0, 0, 0), "OH L") && !strcmp(cell(1, 0, 7), "TB"));
	mcusim_assign_text(&sims[1], as);
	CHECK(as[0] == 'I' && as[1] == '2'); /* the bank number for a second */
	mcusim_press(&sims[1], 0x30, true); /* channel left */
	pump(100);
	CHECK(sf_group_offset(eng, gid) == 7 && !strcmp(cell(0, 0, 0), "Floor"));
	mcusim_press(&sims[1], 0x46, true); /* shift + bank left: the first bank */
	mcusim_press(&sims[1], 0x2e, true);
	mcusim_press(&sims[1], 0x2e, false);
	mcusim_press(&sims[1], 0x46, false);
	pump(200);
	CHECK(sf_group_offset(eng, gid) == 0 && !strcmp(cell(0, 0, 0), "KickIn"));

	/* 6. pots: pan on the x-touch's strip 1 (slot in.9) */
	double pan0 = norm_of("/mix/in.9/pan");
	mcusim_pot(&sims[1], 0, 5);
	pump(40);
	CHECK(fabs(norm_of("/mix/in.9/pan") - (pan0 + 0.05)) < 1e-9 && sims[1].ring[0] == mcu_ring_byte(MCU_RING_DOT, norm_of("/mix/in.9/pan"), false, false));
	CHECK(!strcmp(cell(1, 1, 0), "0.1")); /* line 2: the new value */
	CHECK(!strcmp(last_gesture, "begin /mix/in.9/pan"));
	pump(400);
	CHECK(!strcmp(last_gesture, "end /mix/in.9/pan")); /* 300 ms after the last tick */
	mcusim_press(&sims[1], MCU_VPOT + 0, true); /* push: reset to default */
	pump(40);
	CHECK(fabs(prm_store_get(store, "/mix/in.9/pan")->value) < 1e-9); /* the default, centre */

	/* 7. flip: the faders take the pots' function */
	mcusim_press(&sims[1], 0x32, true);
	mcusim_press(&sims[1], 0x32, false);
	pump(100);
	CHECK(sims[1].led[0x32] == 2);
	CHECK(abs(sims[0].fader[0] - (int)lround(norm_of("/mix/in.1/pan") * 16383)) <= 1);
	mcusim_press(&sims[1], 0x32, true);
	mcusim_press(&sims[1], 0x32, false);
	pump(100);
	CHECK(sims[1].led[0x32] == 0 && abs(sims[0].fader[0] - (int)lround(norm_of("/mix/in.1/fader") * 16383)) <= 1);

	/* 8. the view layers: Aux on the layer-5 button, its LED lit */
	mcusim_press(&sims[1], 0x42, true);
	mcusim_press(&sims[1], 0x42, false);
	pump(200);
	CHECK(!strcmp(sf_group_view(eng, gid), "aux") && sims[1].led[0x42] == 2 && sims[1].led[0x3f] == 0);
	CHECK(!strcmp(cell(0, 0, 0), "Wedge1") && cell(0, 0, 2)[0] == 0 && sims[0].fader[2] == 0 && sims[0].colour[0] == 2);
	mcusim_press(&sims[1], 0x3f, true); /* layer 2: inputs */
	pump(200);
	CHECK(!strcmp(sf_group_view(eng, gid), "inputs") && !strcmp(cell(0, 0, 0), "KickIn"));

	/* 9. spill primitives: push a view, pop back to exactly the window */
	sf_set_window(eng, gid, 5, e2, sizeof(e2));
	CHECK(sf_push_view(eng, gid, "aux", e2, sizeof(e2)) == 0);
	pump(100);
	CHECK(!strcmp(cell(0, 0, 1), "Wedge2"));
	CHECK(sf_pop_view(eng, gid, e2, sizeof(e2)) == 0 && sf_group_offset(eng, gid) == 5);
	pump(100);
	CHECK(!strcmp(cell(0, 0, 0), "Tom1") || !strcmp(cell(0, 0, 0), "Tom 1"));
	CHECK(sf_pop_view(eng, gid, e2, sizeof(e2)) == -3);
	sf_set_window(eng, gid, 0, e2, sizeof(e2));

	/* 10. a strip pot mode on the selected channel (in.3) */
	sf_set_selected(eng, gid, "in.3", e2, sizeof(e2));
	CHECK(sf_set_pot_mode(eng, gid, "eq", 0, e2, sizeof(e2)) == 0);
	pump(200);
	CHECK(!strcmp(cell(0, 0, 0), "LF G") && !strcmp(cell(0, 0, 1), "Pan") && cell(0, 0, 2)[0] == 0);
	mcusim_pot(&sims[0], 0, 10);
	pump(50);
	CHECK(fabs(norm_of("/mix/in.3/eq.lf") - 0.6) < 1e-9);
	CHECK(sf_set_pot_mode(eng, gid, "pan", 0, e2, sizeof(e2)) == 0);
	CHECK(sf_set_pot_mode(eng, gid, "nope", 0, e2, sizeof(e2)) == -2);

	/* 11. displays: a message, the timecode */
	CHECK(sf_text(eng, gid, "", "LOCKED", 500, now, e2, sizeof(e2)) == 0);
	pump(100);
	CHECK(!strncmp(sims[0].lcd, "LOCKED", 6) && !strncmp(sims[1].lcd, "LOCKED", 6));
	pump(600);
	CHECK(strncmp(sims[1].lcd, "LOCKED", 6));
	CHECK(sf_set_binding(eng, gid, "timecode", "param:/mix/clock/tc", e2, sizeof(e2)) == 0);
	pump(50);
	char tc[32];
	mcusim_tc_text(&sims[1], tc, sizeof(tc));
	CHECK(strstr(tc, "01.02.03.04") != NULL);
	CHECK(sf_set_binding(eng, gid, "f1", "action:talkback", e2, sizeof(e2)) == 0);
	mcusim_press(&sims[1], 0x36, true);
	pump(10);
	CHECK(!strcmp(last_action, "talkback"));
	mcusim_press(&sims[1], 0x37, true); /* unbound: an action with its name */
	pump(10);
	CHECK(!strcmp(last_action, "f2"));
	CHECK(sf_set_binding(eng, gid, "wobble", "flip", e2, sizeof(e2)) < 0);

	/* 12. hot-plug: unplug the X-Touch while a fader is held */
	mcusim_touch(&sims[1], 4, true);
	pump(10);
	int ends = gestures_end;
	unplug(1);
	CHECK(gestures_end == ends + 1 && !strcmp(last_gesture, "end /mix/in.13/fader")); /* at once */
	pump(50);
	CHECK(sf_group_width(eng, gid) == 8 && sf_units_online(eng) == 1);
	char *units = sf_units_json(eng);
	CHECK(strstr(units, "\"gone\"") != NULL);
	free(units);
	/* plug it back: same group, same place, redrawn from scratch */
	sf_set_window(eng, gid, 0, e2, sizeof(e2));
	plug(1, "test:X-Touch MIDI 1");
	pump(1500);
	CHECK(sf_unit_state(eng, unit_of_sim[1]) == SF_ONLINE && !strcmp(sf_unit_group(eng, unit_of_sim[1]), gid));
	CHECK(sf_group_width(eng, gid) == 16 && !strcmp(cell(1, 0, 0), "OH L") && !strcmp(cell(0, 0, 0), "KickIn"));
	CHECK(sims[1].colour[0] == 4 && sims[1].fader[2] == (int)lround(norm_of("/mix/in.11/fader") * 16383));

	/* 13. groups by hand: the extender alone, locked to the aux view */
	const char *only_ext[] = { sf_unit_id(eng, unit_of_sim[0]) };
	CHECK(sf_set_group(eng, "mon", only_ext, 1, "monitor", "aux", 0, true, e2, sizeof(e2)) == 0);
	pump(200);
	CHECK(sf_group_width(eng, "mon") == 8 && sf_group_width(eng, gid) == 8 && !strcmp(cell(0, 0, 0), "Wedge1"));
	CHECK(sf_set_window(eng, "mon", 5, e2, sizeof(e2)) == 0 && sf_group_offset(eng, "mon") == 0); /* locked */
	char *state = sf_state_json(eng);
	CHECK(strstr(state, "\"mon\"") && strstr(state, "behringer-xtouch-ext@EXT0001") && strstr(state, "lock-view"));
	free(state);

	/* 14. shutdown: everything dark, then offline */
	sf_unit_goodbye(eng, unit_of_sim[1]);
	CHECK(sims[1].offline && sims[1].fader[8] == 0 && !strncmp(sims[1].lcd, "        ", 8));

	/* 15. views that don't parse change nothing */
	CHECK(sf_set_views(eng, gid, "{\"schema\":\"backline.views/1\",\"views\":[{\"id\":\"x\",\"slots\":[{\"wobble\":1}]}]}", e2, sizeof(e2)) < 0);
	CHECK(sf_set_views(eng, "nope", views_json(), e2, sizeof(e2)) == -2);
	sf_free(eng);
	prm_store_free(store);
}

/* pacing: a full redraw of one unit stays within its byte budget */
static size_t bytes_sent;
static void count_send(void *d, int u, const uint8_t *m, size_t n) { bytes_sent += n; mcusim_receive(&sims[0], m, n); }

static void test_pacing(void)
{
	build();
	struct sf_host host = { count_send, h_set, h_gesture, h_action, NULL, NULL, h_meter, NULL, NULL };
	struct sf_engine *e = sf_new(&host, NULL, store);
	sf_load_profile_dir(e, BL_SURFACE_SRCDIR);
	char e2[256];
	sf_set_views(e, "*", views_json(), e2, sizeof(e2));
	struct sf_profile slow;
	const struct sf_profile *x = sf_profile_by_id(e, "behringer-xtouch");
	slow = *x;
	snprintf(slow.id, sizeof(slow.id), "din-slow");
	slow.bytes_per_second = 3125; /* DIN MIDI */
	slow.n_match = slow.n_exclude = 0;
	sf_add_profile(e, &slow);
	mcusim_init(&sims[0], "xtouch", "DIN0001", NULL, NULL);
	for (int i = 0; i < NCH; i++)
		meter_db[i] = -200; /* silence: only the redraw is measured */
	int u = sf_unit_attach(e, sf_profile_by_id(e, "din-slow"), "din", now);
	uint8_t hello[32];
	uint8_t r[12] = { 0x01, 'D', 'I', 'N', '0', '0', '0', '1', 1, 2, 3, 4 };
	sf_unit_input(e, u, hello, mcu_sysex(0x14, r, 12, hello, sizeof(hello)), now);
	/* bytes per 5 ms tick for 3 s */
	size_t per[600], last = 0;
	for (int t = 0; t < 600; t++) {
		sf_tick(e, now);
		now += 5 * MS;
		per[t] = bytes_sent - last;
		last = bytes_sent;
	}
	size_t total = 0, peak = 0;
	int done = 0;
	for (int t = 0; t < 600; t++) {
		size_t ahead = 0;
		for (int k = t; k < t + 20 && k < 600; k++)
			ahead += per[k];
		if (!ahead && !done)
			done = t; /* nothing more for 100 ms: the redraw is complete */
		if (!done)
			total += per[t];
		size_t win = 0;
		for (int k = t; k < t + 20 && k < 600; k++) /* any 100 ms */
			win += per[k];
		if (win > peak)
			peak = win;
	}
	printf("  DIN-paced full redraw: %zu bytes, complete after %d ms, at most %zu bytes in any 100 ms (budget 313 + a 600-byte burst)\n",
	       total, done * 5, peak);
	CHECK(total > 400 && done * 5 <= 400 && peak <= 313 + 600 + 64);
	CHECK(!strncmp(sims[0].lcd, "KickIn", 6) || strstr(sims[0].lcd, "Backline"));
	sf_free(e);
	prm_store_free(store);
}

int main(void)
{
	test_mcu();
	test_midi();
	test_engine();
	test_pacing();
	if (fails) {
		fprintf(stderr, "%d of %d checks failed\n", fails, checks);
		return 1;
	}
	printf("surface: MCU protocol, profiles and the engine against two simulated devices: %d checks passed\n", checks);
	return 0;
}
