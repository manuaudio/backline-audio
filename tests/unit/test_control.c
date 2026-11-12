/* Unit tests for the control service's OSC codec, network policy and
 * mapping model (control/). SPDX-License-Identifier: MIT */
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mapping.h"
#include "osc.h"
#include "policy.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct seen { int n; char addr[4][64]; double v[4]; char t[4]; };
static void cb(void *d, const struct osc_msg *m)
{
	struct seen *s = d;
	if (s->n < 4) {
		snprintf(s->addr[s->n], 64, "%s", m->address);
		s->t[s->n] = m->argc ? m->argv[0].type : 0;
		if (!m->argc || !osc_arg_number(&m->argv[0], &s->v[s->n]))
			s->v[s->n] = NAN;
	}
	s->n++;
}

static void test_osc(void)
{
	uint8_t b[256];
	size_t n = osc_build(b, sizeof(b), "/fader/1", "f", 0.25);
	CHECK(n == 20);
	struct seen s = { 0 };
	CHECK(osc_parse(b, n, cb, &s) == 1 && s.n == 1 && !strcmp(s.addr[0], "/fader/1") && s.v[0] == 0.25 && s.t[0] == 'f');
	n = osc_build(b, sizeof(b), "/x", "isdTF", 7, "hi", 2.5);
	memset(&s, 0, sizeof(s));
	CHECK(osc_parse(b, n, cb, &s) == 1 && s.v[0] == 7);
	/* a bundle of two messages, one nested */
	uint8_t m1[64], m2[64], inner[128], bun[256];
	size_t n1 = osc_build(m1, sizeof(m1), "/a", "i", 1), n2 = osc_build(m2, sizeof(m2), "/b", "f", 0.5);
	size_t k = 0;
	memcpy(inner, "#bundle\0\0\0\0\0\0\0\0\1", 16); k = 16;
	inner[k++] = 0; inner[k++] = 0; inner[k++] = 0; inner[k++] = (uint8_t)n2; memcpy(inner + k, m2, n2); k += n2;
	size_t ni = k;
	k = 0;
	memcpy(bun, "#bundle\0\0\0\0\0\0\0\0\1", 16); k = 16;
	bun[k++] = 0; bun[k++] = 0; bun[k++] = 0; bun[k++] = (uint8_t)n1; memcpy(bun + k, m1, n1); k += n1;
	bun[k++] = 0; bun[k++] = 0; bun[k++] = 0; bun[k++] = (uint8_t)ni; memcpy(bun + k, inner, ni); k += ni;
	memset(&s, 0, sizeof(s));
	CHECK(osc_parse(bun, k, cb, &s) == 2 && !strcmp(s.addr[0], "/a") && !strcmp(s.addr[1], "/b") && s.v[1] == 0.5);
	/* malformed: never delivered, even partly */
	memset(&s, 0, sizeof(s));
	uint8_t bad[256];
	memcpy(bad, bun, k);
	bad[k - 1] ^= 0; bad[16 + 3] = 200; /* first element's size runs past the end */
	CHECK(osc_parse(bad, k, cb, &s) < 0 && s.n == 0);
	CHECK(osc_parse(b, n - 4, cb, &s) < 0);            /* truncated argument */
	CHECK(osc_parse(b, 3, cb, &s) < 0);                /* not a multiple of 4 */
	CHECK(osc_parse((const uint8_t *)"abc\0", 4, cb, &s) < 0); /* address without '/' */
	uint8_t nonul[8] = { '/', 'a', 'b', 'c', 'd', 'e', 'f', 'g' };
	CHECK(osc_parse(nonul, 8, cb, &s) < 0);            /* string without NUL */
	n = osc_build(b, sizeof(b), "/x", "i", 1);
	b[5] = 'q';                                         /* ",q": unknown type */
	CHECK(osc_parse(b, n, cb, &s) < 0);
	uint8_t blob[] = { '/', 'b', 0, 0, ',', 'b', 0, 0, 0x7f, 0xff, 0xff, 0xff };
	CHECK(osc_parse(blob, sizeof(blob), cb, &s) < 0);  /* huge blob size */
	/* bundle nesting limit: 4 levels parse, 5 don't */
	uint8_t lvl[2][512];
	size_t ln = osc_build(lvl[0], 512, "/d", "i", 1);
	for (int d = 1; d <= 5; d++) {
		uint8_t *src = lvl[(d - 1) % 2], *dst = lvl[d % 2];
		memmove(dst + 20, src, ln);
		memcpy(dst, "#bundle\0\0\0\0\0\0\0\0\1", 16);
		dst[16] = 0; dst[17] = 0; dst[18] = (uint8_t)(ln >> 8); dst[19] = (uint8_t)ln;
		ln += 20;
		CHECK((osc_parse(dst, ln, NULL, NULL) > 0) == (d <= 4));
	}
	CHECK(s.n == 0);
	/* fuzz: random bytes never crash and rarely parse */
	srand(1);
	for (int i = 0; i < 200000; i++) {
		size_t len = (size_t)(rand() % 64) * 4;
		uint8_t r[256];
		for (size_t j = 0; j < len; j++)
			r[j] = (uint8_t)rand();
		if (rand() % 2 && len >= 8) { r[0] = '/'; }
		if (rand() % 4 == 0 && len >= 16) memcpy(r, "#bundle\0", 8);
		osc_parse(r, len, NULL, NULL);
	}
	CHECK(osc_build(b, 8, "/too/long/for/this", "") == 0);
}

static void test_policy(void)
{
	struct sockaddr_storage ss;
	socklen_t sl;
	bool lo;
	char e[300];
	CHECK(ctl_parse_address("127.0.0.1", 9000, &ss, &sl, &lo, e, sizeof(e)) == 0 && lo);
	CHECK(ctl_parse_address("::1", 9000, &ss, &sl, &lo, e, sizeof(e)) == 0 && lo);
	CHECK(ctl_parse_address("10.77.0.1", 9000, &ss, &sl, &lo, e, sizeof(e)) == 0 && !lo);
	CHECK(ctl_parse_address("0.0.0.0", 9000, &ss, &sl, &lo, e, sizeof(e)) < 0 && strstr(e, "wildcard"));
	CHECK(ctl_parse_address("::", 9000, &ss, &sl, &lo, e, sizeof(e)) < 0 && strstr(e, "wildcard"));
	CHECK(ctl_parse_address("::ffff:0.0.0.0", 9000, &ss, &sl, &lo, e, sizeof(e)) < 0);
	CHECK(ctl_parse_address("localhost", 9000, &ss, &sl, &lo, e, sizeof(e)) < 0 && strstr(e, "not a literal"));
	CHECK(ctl_parse_address("mixer.local", 9000, &ss, &sl, &lo, e, sizeof(e)) < 0);
	CHECK(ctl_parse_address("239.1.1.1", 9000, &ss, &sl, &lo, e, sizeof(e)) < 0);
	CHECK(ctl_parse_address("255.255.255.255", 9000, &ss, &sl, &lo, e, sizeof(e)) < 0);

	struct ctl_cidr c;
	CHECK(ctl_parse_cidr("10.77.0.0/24", &c) == 0 && c.prefix == 24);
	CHECK(ctl_parse_cidr("10.77.0.0/33", &c) < 0);
	CHECK(ctl_parse_cidr("10.77.0.0/", &c) < 0);
	CHECK(ctl_parse_cidr("bogus", &c) < 0);
	struct sockaddr_in in = { .sin_family = AF_INET };
	inet_pton(AF_INET, "10.77.0.9", &in.sin_addr);
	ctl_parse_cidr("10.77.0.0/24", &c);
	CHECK(ctl_cidr_match(&c, (struct sockaddr *)&in));
	inet_pton(AF_INET, "10.77.1.9", &in.sin_addr);
	CHECK(!ctl_cidr_match(&c, (struct sockaddr *)&in));
	ctl_parse_cidr("10.77.1.9", &c);
	CHECK(ctl_cidr_match(&c, (struct sockaddr *)&in));
	struct sockaddr_in6 in6 = { .sin6_family = AF_INET6 };
	inet_pton(AF_INET6, "::ffff:10.77.1.9", &in6.sin6_addr);
	CHECK(ctl_cidr_match(&c, (struct sockaddr *)&in6)); /* v4-mapped peer on a v6 socket */
	ctl_parse_cidr("fd7a:115c::/48", &c);
	inet_pton(AF_INET6, "fd7a:115c:0:1::5", &in6.sin6_addr);
	CHECK(ctl_cidr_match(&c, (struct sockaddr *)&in6));

	struct ctl_listen_spec s;
	memset(&s, 0, sizeof(s));
	s.max_rate = 500; s.max_clients = 8; s.max_size = 8192;
	strcpy(s.address, "127.0.0.1");
	CHECK(ctl_check_listen(&s, e, sizeof(e)) == 0);           /* loopback UDP */
	strcpy(s.address, "10.77.0.1");
	CHECK(ctl_check_listen(&s, e, sizeof(e)) < 0 && strstr(e, "allow-list"));
	ctl_parse_cidr("10.77.0.0/24", &s.allow[0]); s.n_allow = 1;
	CHECK(ctl_check_listen(&s, e, sizeof(e)) < 0 && strstr(e, "insecure-udp"));
	s.insecure_udp = true;
	CHECK(ctl_check_listen(&s, e, sizeof(e)) == 0);
	s.tcp = true;
	CHECK(ctl_check_listen(&s, e, sizeof(e)) < 0 && strstr(e, "secret"));
	strcpy(s.secret, "short");
	CHECK(ctl_check_listen(&s, e, sizeof(e)) < 0 && strstr(e, "at least"));
	strcpy(s.secret, "0123456789abcdef-long-enough");
	CHECK(ctl_check_listen(&s, e, sizeof(e)) == 0);
	s.tcp = false;
	CHECK(ctl_check_listen(&s, e, sizeof(e)) < 0 && strstr(e, "needs TCP"));

	/* peers */
	struct ctl_listen_spec lo_spec = { .n_allow = 0 };
	inet_pton(AF_INET, "127.0.0.1", &in.sin_addr);
	CHECK(ctl_peer_allowed(&lo_spec, true, (struct sockaddr *)&in));
	inet_pton(AF_INET, "10.77.0.9", &in.sin_addr);
	CHECK(!ctl_peer_allowed(&lo_spec, true, (struct sockaddr *)&in));
	CHECK(ctl_peer_allowed(&s, false, (struct sockaddr *)&in));
	inet_pton(AF_INET, "10.78.0.9", &in.sin_addr);
	CHECK(!ctl_peer_allowed(&s, false, (struct sockaddr *)&in));

	CHECK(ctl_secret_equal("0123456789abcdef-long-enough", 28, s.secret));
	CHECK(!ctl_secret_equal("0123456789abcdef-long-enougX", 28, s.secret));
	CHECK(!ctl_secret_equal("0123456789abcdef-long-enough!", 29, s.secret));
	CHECK(!ctl_secret_equal("", 0, s.secret));
	CHECK(!ctl_secret_equal("", 0, ""));

	struct ctl_bucket bk = { 0 };
	int ok = 0;
	for (int i = 0; i < 1000; i++)
		ok += ctl_bucket_take(&bk, 100, 1000000000ull); /* all at once: the burst */
	CHECK(ok == 100);
	CHECK(ctl_bucket_take(&bk, 100, 1000000000ull + 20000000ull)); /* 20 ms later: 2 more */
	CHECK(ctl_bucket_take(&bk, 100, 1000000000ull + 20000000ull));
	CHECK(!ctl_bucket_take(&bk, 100, 1000000000ull + 20000000ull));
}

static void test_mapping(void)
{
	struct ctl_source s, e;
	struct ctl_target t;
	char err[300], buf[160];
	CHECK(ctl_parse_source("midi:cc:1:7", &s, err, sizeof(err)) == 0 && s.kind == CTL_SRC_CC && s.channel == 1 && s.number == 7);
	ctl_format_source(&s, buf, sizeof(buf));
	CHECK(!strcmp(buf, "midi:cc:1:7"));
	CHECK(ctl_parse_source("midi:note:*:60", &s, err, sizeof(err)) == 0 && s.channel == 0);
	CHECK(ctl_parse_source("midi:cc:17:7", &s, err, sizeof(err)) < 0);
	CHECK(ctl_parse_source("midi:cc:1:128", &s, err, sizeof(err)) < 0);
	CHECK(ctl_parse_source("osc:/fader/1", &s, err, sizeof(err)) == 0 && s.kind == CTL_SRC_OSC);
	CHECK(ctl_parse_source("osc:/fader/*", &s, err, sizeof(err)) < 0);
	CHECK(ctl_parse_source("osc:fader", &s, err, sizeof(err)) < 0);
	CHECK(ctl_parse_target("instance:backline.surge-a/Global Volume", &t, err, sizeof(err)) == 0 &&
	      !strcmp(t.node, "backline.surge-a") && !strcmp(t.key, "Global Volume"));
	CHECK(ctl_parse_target("instance:backline.surge-a/FX/Return 1", &t, err, sizeof(err)) == 0 && !strcmp(t.key, "FX/Return 1"));
	CHECK(ctl_parse_target("instance:backline.surge-a", &t, err, sizeof(err)) < 0);
	CHECK(ctl_parse_target("transport:locate:bbt:5|1|0", &t, err, sizeof(err)) == 0 && !strcmp(t.arg, "bbt:5|1|0"));
	CHECK(ctl_parse_target("transport:rewind", &t, err, sizeof(err)) < 0);
	CHECK(ctl_parse_target("volume:a/b", &t, err, sizeof(err)) < 0);
	CHECK(ctl_parse_target("recorder:marker", &t, err, sizeof(err)) == 0 && ctl_target_is_trigger(&t));
	CHECK(ctl_parse_target("transport:tempo", &t, err, sizeof(err)) == 0 && !ctl_target_is_trigger(&t));

	uint8_t cc[3] = { 0xb0, 7, 127 }, on[3] = { 0x91, 60, 64 }, off0[3] = { 0x91, 60, 0 }, pb[3] = { 0xe0, 0x7f, 0x7f };
	double v;
	CHECK(ctl_midi_event(cc, 3, &e, &v) && e.kind == CTL_SRC_CC && e.channel == 1 && e.number == 7 && v == 1);
	CHECK(ctl_midi_event(on, 3, &e, &v) && e.kind == CTL_SRC_NOTE && e.channel == 2 && fabs(v - 64 / 127.0) < 1e-12);
	CHECK(ctl_midi_event(off0, 3, &e, &v) && v == 0);
	CHECK(ctl_midi_event(pb, 3, &e, &v) && e.kind == CTL_SRC_PITCHBEND && v == 1);
	uint8_t clk[1] = { 0xf8 };
	CHECK(!ctl_midi_event(clk, 1, &e, &v));
	ctl_parse_source("midi:note:*:60", &s, err, sizeof(err));
	ctl_midi_event(on, 3, &e, &v);
	CHECK(ctl_source_matches(&s, &e));
	ctl_parse_source("midi:note:1:60", &s, err, sizeof(err));
	CHECK(!ctl_source_matches(&s, &e));

	CHECK(ctl_scale(0.5, false, 0, 10) == 5);
	CHECK(ctl_scale(0.25, true, 0, 1) == 0.75);
	CHECK(ctl_scale(2, false, -60, 0) == 0);
	CHECK(ctl_scale(NAN, false, -60, 0) == -60);
	CHECK(ctl_normalize(64, 0, 127) > 0.50 && ctl_normalize(64, 0, 127) < 0.51);
	CHECK(ctl_normalize(-5, 0, 1) == 0);
	CHECK(fabs(ctl_db_to_gain(-6.0206) - 0.5) < 1e-4);
	CHECK(ctl_db_to_gain(-INFINITY) == 0 && ctl_db_to_gain(-200) == 0);
}

/* Control1 additions: cc14, NRPN/RPN, relative encoders, takeover, feedback */
static void test_additions(void)
{
	struct ctl_source s;
	struct ctl_target t;
	char err[300], buf[160];
	CHECK(ctl_parse_source("midi:cc14:1:7", &s, err, sizeof(err)) == 0 && s.kind == CTL_SRC_CC14 && s.number == 7);
	CHECK(ctl_parse_source("midi:cc14:1:32", &s, err, sizeof(err)) < 0);
	CHECK(ctl_parse_source("midi:nrpn:3:16383", &s, err, sizeof(err)) == 0 && s.kind == CTL_SRC_NRPN && s.channel == 3);
	ctl_format_source(&s, buf, sizeof(buf));
	CHECK(!strcmp(buf, "midi:nrpn:3:16383"));
	CHECK(ctl_parse_source("midi:rpn:*:0", &s, err, sizeof(err)) == 0 && s.kind == CTL_SRC_RPN && s.channel == 0);
	CHECK(ctl_parse_source("midi:nrpn:1:16384", &s, err, sizeof(err)) < 0);
	CHECK(ctl_parse_target("param:/console/in.1/fader", &t, err, sizeof(err)) == 0 && t.kind == CTL_T_PARAM &&
	      !strcmp(t.key, "/console/in.1/fader"));
	CHECK(ctl_parse_target("param:/console", &t, err, sizeof(err)) < 0);
	CHECK(ctl_parse_target("param:/console/in.*/fader", &t, err, sizeof(err)) < 0);

	enum ctl_encoding e;
	CHECK(ctl_parse_encoding("rel-sm", &e) == 0 && e == CTL_ENC_REL_SM && ctl_parse_encoding("rel", &e) < 0);
	CHECK(ctl_rel_ticks(CTL_ENC_REL_2C, 1) == 1 && ctl_rel_ticks(CTL_ENC_REL_2C, 127) == -1 && ctl_rel_ticks(CTL_ENC_REL_2C, 64) == -64);
	CHECK(ctl_rel_ticks(CTL_ENC_REL_SM, 0x03) == 3 && ctl_rel_ticks(CTL_ENC_REL_SM, 0x41) == -1 && ctl_rel_ticks(CTL_ENC_REL_SM, 0x45) == -5);
	CHECK(ctl_rel_ticks(CTL_ENC_REL_OFFSET, 65) == 1 && ctl_rel_ticks(CTL_ENC_REL_OFFSET, 63) == -1 && ctl_rel_ticks(CTL_ENC_REL_OFFSET, 64) == 0);
	CHECK(fabs(ctl_rel_apply(0.5, 3, 0.01, 1) - 0.53) < 1e-12 && ctl_rel_apply(0.99, 5, 0.01, 1) == 1 && ctl_rel_apply(0.01, -5, 0.01, 1) == 0);
	CHECK(fabs(ctl_rel_apply(0.5, -2, 0.01, 2) - 0.46) < 1e-12);

	/* 14-bit: a 7-bit device's MSB counts at once; after an LSB, pairs */
	struct ctl_hires h = { 0 };
	double n;
	CHECK(ctl_hires_feed(&h, 0, 127, 0, &n) && n == 1);
	CHECK(ctl_hires_feed(&h, 1, 0x7f, 1, &n) && n == 1);          /* first LSB: completes with the last MSB */
	CHECK(!ctl_hires_feed(&h, 0, 64, 1000, &n));                   /* now an MSB waits */
	CHECK(!ctl_hires_expire(&h, 1000 + CTL_HIRES_TIMEOUT_NS - 1, &n));
	CHECK(ctl_hires_feed(&h, 1, 1, 2000, &n) && fabs(n - (64 * 128 + 1) / 16383.0) < 1e-12);
	CHECK(!ctl_hires_feed(&h, 0, 10, 5000, &n));
	CHECK(ctl_hires_expire(&h, 5000 + CTL_HIRES_TIMEOUT_NS, &n) && fabs(n - 10 * 128 / 16383.0) < 1e-12);
	CHECK(!ctl_hires_expire(&h, 5000 + 2 * CTL_HIRES_TIMEOUT_NS, &n));

	/* NRPN 0x123 = 291 on channel 2, data 0x40 0x05, then increment */
	struct ctl_nrpn st;
	ctl_nrpn_init(&st);
	bool rpn;
	int prm, val;
	CHECK(ctl_nrpn_feed(&st, 1, 6, 10, &rpn, &prm, &val) == CTL_NRPN_NONE); /* nothing selected */
	CHECK(ctl_nrpn_feed(&st, 1, 99, 2, &rpn, &prm, &val) == CTL_NRPN_NONE);
	CHECK(ctl_nrpn_feed(&st, 1, 98, 0x23, &rpn, &prm, &val) == CTL_NRPN_NONE);
	CHECK(ctl_nrpn_feed(&st, 1, 6, 0x40, &rpn, &prm, &val) == CTL_NRPN_DATA_MSB && !rpn && prm == 291 && val == 0x40);
	CHECK(ctl_nrpn_feed(&st, 1, 38, 5, &rpn, &prm, &val) == CTL_NRPN_DATA_LSB && val == 5);
	CHECK(ctl_nrpn_feed(&st, 1, 96, 0, &rpn, &prm, &val) == CTL_NRPN_INC && val == 1);
	CHECK(ctl_nrpn_feed(&st, 1, 97, 3, &rpn, &prm, &val) == CTL_NRPN_DEC && val == 3);
	CHECK(ctl_nrpn_feed(&st, 0, 6, 1, &rpn, &prm, &val) == CTL_NRPN_NONE); /* per channel */
	CHECK(ctl_nrpn_feed(&st, 1, 101, 0, &rpn, &prm, &val) == CTL_NRPN_NONE);
	CHECK(ctl_nrpn_feed(&st, 1, 100, 0, &rpn, &prm, &val) == CTL_NRPN_NONE);
	CHECK(ctl_nrpn_feed(&st, 1, 6, 2, &rpn, &prm, &val) == CTL_NRPN_DATA_MSB && rpn && prm == 0); /* pitch-bend range */
	ctl_nrpn_feed(&st, 1, 101, 127, &rpn, &prm, &val);
	ctl_nrpn_feed(&st, 1, 100, 127, &rpn, &prm, &val);                   /* RPN null */
	CHECK(ctl_nrpn_feed(&st, 1, 6, 2, &rpn, &prm, &val) == CTL_NRPN_NONE);

	/* takeover: pickup ignores until crossing; scale converges; jump follows */
	struct ctl_takeover to = { 0 };
	double out;
	CHECK(!ctl_takeover_apply(CTL_TO_PICKUP, &to, 0.2, true, 0.6, &out));
	CHECK(!ctl_takeover_apply(CTL_TO_PICKUP, &to, 0.5, true, 0.6, &out));
	CHECK(ctl_takeover_apply(CTL_TO_PICKUP, &to, 0.7, true, 0.6, &out) && out == 0.7 && to.picked); /* crossed */
	CHECK(ctl_takeover_apply(CTL_TO_PICKUP, &to, 0.1, true, 0.7, &out) && out == 0.1);
	ctl_takeover_lost(&to);
	CHECK(!ctl_takeover_apply(CTL_TO_PICKUP, &to, 0.12, true, 0.9, &out));
	CHECK(ctl_takeover_apply(CTL_TO_PICKUP, &to, 0.89, true, 0.9, &out) && out == 0.89); /* within tolerance */
	struct ctl_takeover sc = { 0 };
	double tgt = 0.8, c = 0.2;
	CHECK(!ctl_takeover_apply(CTL_TO_SCALE, &sc, c, true, tgt, &out)); /* first value: no direction yet */
	int steps = 0;
	while (!sc.picked && steps < 100) {
		c += 0.01;
		if (ctl_takeover_apply(CTL_TO_SCALE, &sc, c, true, tgt, &out)) {
			CHECK(out >= tgt - 1e-12); /* moving up never moves the target down */
			tgt = out;
		}
		steps++;
	}
	CHECK(sc.picked && c < 1.0001 && fabs(c - tgt) <= CTL_TAKEOVER_TOLERANCE + 1e-9);
	struct ctl_takeover j = { 0 };
	CHECK(ctl_takeover_apply(CTL_TO_JUMP, &j, 0.3, true, 0.9, &out) && out == 0.3);
	struct ctl_takeover u = { 0 };
	CHECK(ctl_takeover_apply(CTL_TO_PICKUP, &u, 0.3, false, 0, &out) && out == 0.3); /* unknown target: follow */

	/* feedback bytes */
	uint8_t fb[16];
	ctl_parse_source("midi:cc:2:7", &s, err, sizeof(err));
	CHECK(ctl_feedback_midi(&s, 1, 7, fb, sizeof(fb)) == 3 && fb[0] == 0xb1 && fb[1] == 7 && fb[2] == 127);
	ctl_parse_source("midi:cc14:1:7", &s, err, sizeof(err));
	CHECK(ctl_feedback_midi(&s, 0.5, 14, fb, sizeof(fb)) == 6 && fb[1] == 7 && fb[2] == 64 && fb[4] == 39 && fb[5] == 0);
	ctl_parse_source("midi:nrpn:*:291", &s, err, sizeof(err));
	CHECK(ctl_feedback_midi(&s, 1, 14, fb, sizeof(fb)) == 12 && fb[0] == 0xb0 && fb[1] == 99 && fb[2] == 2 && fb[4] == 98 &&
	      fb[5] == 0x23 && fb[7] == 6 && fb[8] == 127 && fb[10] == 38 && fb[11] == 127);
	CHECK(ctl_feedback_midi(&s, 1, 7, fb, sizeof(fb)) == 9);
	ctl_parse_source("midi:note:1:16", &s, err, sizeof(err));
	CHECK(ctl_feedback_midi(&s, 1, 7, fb, sizeof(fb)) == 3 && fb[0] == 0x90 && fb[2] == 127);
	ctl_parse_source("midi:pitchbend:1", &s, err, sizeof(err));
	CHECK(ctl_feedback_midi(&s, 1, 7, fb, sizeof(fb)) == 3 && fb[1] == 0x7f && fb[2] == 0x7f);
	CHECK(ctl_feedback_midi(&s, 1, 7, fb, 2) == 0); /* no room: nothing */
	CHECK(fabs(ctl_gain_to_db(0.5) + 6.0206) < 1e-3 && isinf(ctl_gain_to_db(0)));
}

int main(void)
{
	test_osc();
	test_policy();
	test_mapping();
	test_additions();
	if (fails) {
		fprintf(stderr, "%d checks failed\n", fails);
		return 1;
	}
	puts("control: OSC codec, network policy and mappings ok");
	return 0;
}
