/* RTP-MIDI and AppleMIDI sessions (surface/rtpmidi.c) in simulated time: the
 * packet formats, SysEx split across packets, and two sessions (an initiator,
 * as surfaced is, and a responder, as a network X-Touch is) wired back to
 * back through a "cable" that can be cut: invite, sync, MIDI both ways, a
 * network drop noticed by the initiator, and the reconnect; a device that
 * refuses a second session.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtpmidi.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define MS 1000000ull

/* ------------------------------------------------------------ packets */
static uint8_t got[64][RTPM_SYSEX_MAX];
static size_t got_n[64];
static int n_got;
static void collect(void *data, const uint8_t *m, size_t n)
{
	(void)data;
	if (n_got < 64) {
		memcpy(got[n_got], m, n);
		got_n[n_got++] = n;
	}
}

static void test_packets(void)
{
	uint8_t b[RTPM_PACKET_MAX];
	struct rtpm_cmd c;
	size_t n = rtpm_build_session(RTPM_CMD_IN, 0x11223344, 0xaabbccdd, "Backline", b, sizeof(b));
	CHECK(n == 16 + 9 && b[0] == 0xff && b[2] == 'I' && b[3] == 'N' && b[7] == 2);
	CHECK(rtpm_parse_command(b, n, &c) && c.kind == RTPM_CMD_IN && c.token == 0x11223344 && c.ssrc == 0xaabbccdd &&
	      !strcmp(c.name, "Backline"));
	n = rtpm_build_session(RTPM_CMD_BY, 1, 2, "x", b, sizeof(b));
	CHECK(n == 16 && rtpm_parse_command(b, n, &c) && c.kind == RTPM_CMD_BY && c.name[0] == 0);
	uint64_t ts[3] = { 1, 0x0102030405060708ull, 3 };
	n = rtpm_build_ck(7, 1, ts, b, sizeof(b));
	CHECK(n == 36 && rtpm_parse_command(b, n, &c) && c.kind == RTPM_CMD_CK && c.count == 1 && c.ts[1] == ts[1]);
	CHECK(!rtpm_parse_command((const uint8_t *)"\xff\xff" "ZZ", 4, &c));

	/* three messages, one a 120-byte LCD SysEx, through encode and decode */
	uint8_t msgs[400];
	size_t ml = 0;
	uint8_t fader[3] = { 0xe0, 0x7f, 0x5f }, led[3] = { 0x90, 0x10, 0x7f }, sx[120];
	sx[0] = 0xf0;
	for (int i = 1; i < 119; i++)
		sx[i] = (uint8_t)(i & 0x7f);
	sx[119] = 0xf7;
	const uint8_t *ms[3] = { fader, sx, led };
	size_t ns[3] = { 3, 120, 3 };
	for (int i = 0; i < 3; i++) {
		msgs[ml++] = (uint8_t)ns[i];
		msgs[ml++] = 0;
		memcpy(msgs + ml, ms[i], ns[i]);
		ml += ns[i];
	}
	n = rtpm_encode(9, 1234, 0xdeadbeef, msgs, ml, b, sizeof(b));
	CHECK(n == 12 + 2 + 3 + 1 + 120 + 1 + 3 && b[1] == 0x61 && (b[12] & 0x80));
	struct rtpm_rx rx = { 0 };
	uint16_t seq;
	uint32_t ssrc;
	n_got = 0;
	CHECK(rtpm_decode(&rx, b, n, &seq, &ssrc, collect, NULL) && seq == 9 && ssrc == 0xdeadbeef);
	CHECK(n_got == 3 && got_n[0] == 3 && got_n[1] == 120 && !memcmp(got[1], sx, 120) && got[2][0] == 0x90);

	/* another sender's style: short header, running status, delta times,
	 * a SysEx split over two packets (F0 .. F0, then F7 .. F7) */
	uint8_t p1[] = { 0x80, 0x61, 0, 1, 0, 0, 0, 0, 1, 2, 3, 4,
			 0x0b, 0x90, 0x3c, 0x40, 0x81, 0x00, 0x3e, 0x40, 0x00, 0xf0, 0x00, 0x00, 0xf0 };
	uint8_t p2[] = { 0x80, 0x61, 0, 2, 0, 0, 0, 0, 1, 2, 3, 4, 0x04, 0xf7, 0x66, 0x14, 0xf7 };
	n_got = 0;
	CHECK(rtpm_decode(&rx, p1, sizeof(p1), NULL, NULL, collect, NULL));
	CHECK(n_got == 2 && got[1][0] == 0x90 && got[1][1] == 0x3e);
	CHECK(rtpm_decode(&rx, p2, sizeof(p2), NULL, NULL, collect, NULL));
	CHECK(n_got == 3 && got_n[2] == 6 && !memcmp(got[2], "\xf0\x00\x00\x66\x14\xf7", 6));
	/* a command packet is not a data packet */
	CHECK(!rtpm_decode(&rx, (const uint8_t *)"\xff\xffOK\0\0\0\2\0\0\0\0\0\0\0\0", 16, NULL, NULL, collect, NULL));
}

/* ------------------------------------------------------------ two sessions */
struct end {
	struct rtpm_session s;
	struct end *peer;
	bool cut;            /* the cable: nothing in or out */
	bool refuse;         /* a device with its one session taken */
	uint8_t midi[64][8];
	int n_midi;
	int opened, left;
};
static uint64_t now;

static void e_send(void *data, enum rtpm_port port, const uint8_t *p, size_t n)
{
	struct end *e = data;
	if (e->cut || e->peer->cut)
		return;
	if (e->peer->refuse) {
		struct rtpm_cmd c;
		if (rtpm_parse_command(p, n, &c) && c.kind == RTPM_CMD_IN) {
			uint8_t b[64];
			size_t k = rtpm_build_session(RTPM_CMD_NO, c.token, 99, "X-Touch", b, sizeof(b));
			rtpm_input(&e->s, port, b, k, now);
		}
		return;
	}
	rtpm_input(&e->peer->s, port, p, n, now);
}

static void e_midi(void *data, const uint8_t *m, size_t n)
{
	struct end *e = data;
	if (e->n_midi < 64 && n <= 8)
		memcpy(e->midi[e->n_midi++], m, n);
}

static void e_state(void *data, enum rtpm_state old, enum rtpm_state st)
{
	struct end *e = data;
	if (st == RTPM_OPEN)
		e->opened++;
	if (old == RTPM_OPEN)
		e->left++;
}

static const struct rtpm_events ev = { e_send, e_midi, e_state };

static void run(struct end *a, struct end *b, uint64_t ms)
{
	for (uint64_t t = 0; t < ms; t += 5) {
		now += 5 * MS;
		rtpm_tick(&a->s, now);
		rtpm_tick(&b->s, now);
	}
}

static void test_sessions(void)
{
	static struct end host, dev;
	now = 1000 * MS;
	host.peer = &dev;
	dev.peer = &host;
	rtpm_init(&host.s, RTPM_INITIATOR, "Backline", 0x1111, now, &ev, &host);
	rtpm_init(&dev.s, RTPM_RESPONDER, "X-Touch", 0x2222, now, &ev, &dev);
	rtpm_start(&host.s, now);
	run(&host, &dev, 20);
	CHECK(host.s.state == RTPM_OPEN && dev.s.state == RTPM_OPEN);
	CHECK(!strcmp(host.s.peer_name, "X-Touch") && !strcmp(dev.s.peer_name, "Backline"));
	CHECK(host.opened == 1 && dev.opened == 1);

	/* MIDI both ways, batched per tick */
	uint8_t f[3] = { 0xe2, 0x10, 0x40 }, t[3] = { 0x90, 0x6a, 0x7f };
	CHECK(rtpm_send(&host.s, f, 3, now));
	CHECK(rtpm_send(&dev.s, t, 3, now));
	run(&host, &dev, 10);
	CHECK(dev.n_midi == 1 && !memcmp(dev.midi[0], f, 3));
	CHECK(host.n_midi == 1 && !memcmp(host.midi[0], t, 3));

	/* quiet but connected: CK every second keeps it open */
	run(&host, &dev, 10000);
	CHECK(host.s.state == RTPM_OPEN && host.left == 0 && dev.left == 0);

	/* the network drops: the initiator notices within RTPM_LOST_MS (+ a tick) */
	dev.cut = true;
	uint64_t cut_at = now;
	while (host.s.state == RTPM_OPEN && now - cut_at < 10000 * MS)
		run(&host, &dev, 5);
	uint64_t noticed = (now - cut_at) / MS;
	printf("drop noticed after %llu ms\n", (unsigned long long)noticed);
	CHECK(host.s.state == RTPM_LOST && noticed <= RTPM_LOST_MS + 1000);
	/* the responder probes with CK too; the initiator answered, so it is as quick */
	CHECK(dev.s.probes_answered && dev.s.state == RTPM_LOST);
	CHECK(!rtpm_send(&host.s, f, 3, now));
	run(&host, &dev, 5000);
	CHECK(host.s.state == RTPM_INVITING);   /* inviting again, every second */

	/* back: the next invitation opens it, and MIDI flows again */
	dev.cut = false;
	run(&host, &dev, 1100);
	CHECK(host.s.state == RTPM_OPEN && dev.s.state == RTPM_OPEN && host.opened == 2);
	dev.n_midi = 0;
	rtpm_send(&host.s, f, 3, now);
	run(&host, &dev, 10);
	CHECK(dev.n_midi == 1);

	/* a clean end: BY */
	rtpm_bye(&host.s);
	CHECK(host.s.state == RTPM_CLOSED && dev.s.state == RTPM_CLOSED);

	/* a device with no free session says NO: asked again every 5 s */
	static struct end h2, busy;
	h2.peer = &busy;
	busy.peer = &h2;
	busy.refuse = true;
	rtpm_init(&h2.s, RTPM_INITIATOR, "Backline standby", 0x3333, now, &ev, &h2);
	rtpm_init(&busy.s, RTPM_RESPONDER, "X-Touch", 0x4444, now, &ev, &busy);
	rtpm_start(&h2.s, now);
	run(&h2, &busy, 20);
	CHECK(h2.s.state == RTPM_REFUSED);
	run(&h2, &busy, RTPM_REFUSED_RETRY_MS + 50);
	CHECK(h2.s.state == RTPM_REFUSED && h2.s.packets_out >= 2);
	busy.refuse = false;
	run(&h2, &busy, RTPM_REFUSED_RETRY_MS + 50);
	CHECK(h2.s.state == RTPM_OPEN);
}

int main(void)
{
	test_packets();
	test_sessions();
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
