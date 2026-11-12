/* RTP-MIDI and AppleMIDI sessions, pure (rtpmidi.h).
 * SPDX-License-Identifier: MIT
 */
#include "rtpmidi.h"

#include <stdio.h>
#include <string.h>

#define MS 1000000ull
#define PROTOCOL_VERSION 2
#define RTP_PT_MIDI 0x61

static void be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void be64(uint8_t *p, uint64_t v) { be32(p, (uint32_t)(v >> 32)); be32(p + 4, (uint32_t)v); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

/* ================================================================ commands */
static const struct { char c[3]; enum rtpm_cmd_kind k; } cmds[] = {
	{ "IN", RTPM_CMD_IN }, { "OK", RTPM_CMD_OK }, { "NO", RTPM_CMD_NO },
	{ "BY", RTPM_CMD_BY }, { "CK", RTPM_CMD_CK }, { "RS", RTPM_CMD_RS },
};

bool rtpm_is_command(const uint8_t *p, size_t n)
{
	return n >= 4 && p[0] == 0xff && p[1] == 0xff;
}

bool rtpm_parse_command(const uint8_t *p, size_t n, struct rtpm_cmd *c)
{
	memset(c, 0, sizeof(*c));
	if (!rtpm_is_command(p, n))
		return false;
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		if (p[2] == (uint8_t)cmds[i].c[0] && p[3] == (uint8_t)cmds[i].c[1])
			c->kind = cmds[i].k;
	switch (c->kind) {
	case RTPM_CMD_IN: case RTPM_CMD_OK: case RTPM_CMD_NO: case RTPM_CMD_BY:
		if (n < 16)
			return false;
		c->version = rd32(p + 4);
		c->token = rd32(p + 8);
		c->ssrc = rd32(p + 12);
		if (n > 16) {
			size_t k = 0;
			for (size_t i = 16; i < n && p[i] && k + 1 < sizeof(c->name); i++)
				c->name[k++] = (char)p[i];
			c->name[k] = 0;
		}
		return true;
	case RTPM_CMD_CK:
		if (n < 36)
			return false;
		c->ssrc = rd32(p + 4);
		c->count = p[8];
		for (int i = 0; i < 3; i++)
			c->ts[i] = rd64(p + 12 + 8 * i);
		return c->count <= 2;
	case RTPM_CMD_RS:
		if (n < 10)
			return false;
		c->ssrc = rd32(p + 4);
		c->seq = rd16(p + 8);
		return true;
	default:
		return false;
	}
}

static size_t cmd_head(enum rtpm_cmd_kind kind, uint8_t *out)
{
	out[0] = out[1] = 0xff;
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		if (cmds[i].k == kind) {
			out[2] = (uint8_t)cmds[i].c[0];
			out[3] = (uint8_t)cmds[i].c[1];
		}
	return 4;
}

size_t rtpm_build_session(enum rtpm_cmd_kind kind, uint32_t token, uint32_t ssrc, const char *name, uint8_t *out,
			  size_t cap)
{
	size_t nl = (kind == RTPM_CMD_BY || !name) ? 0 : strnlen(name, RTPM_NAME_MAX - 1) + 1;
	if (cap < 16 + nl)
		return 0;
	cmd_head(kind, out);
	be32(out + 4, PROTOCOL_VERSION);
	be32(out + 8, token);
	be32(out + 12, ssrc);
	if (nl) {
		memcpy(out + 16, name, nl - 1);
		out[16 + nl - 1] = 0;
	}
	return 16 + nl;
}

size_t rtpm_build_ck(uint32_t ssrc, uint8_t count, const uint64_t ts[3], uint8_t *out, size_t cap)
{
	if (cap < 36)
		return 0;
	cmd_head(RTPM_CMD_CK, out);
	be32(out + 4, ssrc);
	out[8] = count;
	out[9] = out[10] = out[11] = 0;
	for (int i = 0; i < 3; i++)
		be64(out + 12 + 8 * i, ts[i]);
	return 36;
}

size_t rtpm_build_rs(uint32_t ssrc, uint16_t seq, uint8_t *out, size_t cap)
{
	if (cap < 12)
		return 0;
	cmd_head(RTPM_CMD_RS, out);
	be32(out + 4, ssrc);
	be16(out + 8, seq);
	out[10] = out[11] = 0;
	return 12;
}

/* ================================================================ payload */
static int data_len(uint8_t st)
{
	if (st < 0x80)
		return -1;
	if (st < 0xf0) {
		uint8_t hi = st & 0xf0;
		return (hi == 0xc0 || hi == 0xd0) ? 1 : 2;
	}
	switch (st) {
	case 0xf1: case 0xf3: return 1;
	case 0xf2: return 2;
	default: return 0;
	}
}

static void sysex_bytes(struct rtpm_rx *rx, const uint8_t *p, size_t *pos, size_t end, rtpm_midi_cb cb, void *data)
{
	while (*pos < end) {
		uint8_t c = p[(*pos)++];
		if (c == 0xf7) {           /* the end of this SysEx */
			if (rx->in_sysex && !rx->overflow && rx->n < RTPM_SYSEX_MAX) {
				rx->sysex[rx->n++] = 0xf7;
				cb(data, rx->sysex, rx->n);
			}
			rx->in_sysex = false;
			return;
		}
		if (c == 0xf0)             /* this segment ends; the next starts with F7 */
			return;
		if (c == 0xf4) {           /* cancelled */
			rx->in_sysex = false;
			return;
		}
		if (c >= 0xf8) {           /* realtime inside SysEx */
			cb(data, &c, 1);
			continue;
		}
		if (c & 0x80) {            /* malformed: drop what we have */
			rx->in_sysex = false;
			(*pos)--;
			return;
		}
		if (rx->n < RTPM_SYSEX_MAX - 1)
			rx->sysex[rx->n++] = c;
		else
			rx->overflow = true;
	}
}

bool rtpm_decode(struct rtpm_rx *rx, const uint8_t *p, size_t n, uint16_t *seq, uint32_t *ssrc, rtpm_midi_cb cb,
		 void *data)
{
	if (n < 13 || rtpm_is_command(p, n) || (p[0] >> 6) != 2)
		return false;
	if (seq)
		*seq = rd16(p + 2);
	if (ssrc)
		*ssrc = rd32(p + 8);
	size_t pos = 12 + 4u * (p[0] & 0x0f);
	if (p[0] & 0x10) {               /* header extension */
		if (pos + 4 > n)
			return false;
		pos += 4 + 4u * rd16(p + pos + 2);
	}
	if (pos >= n)
		return false;
	uint8_t h = p[pos++];
	size_t len = h & 0x0f;
	if (h & 0x80) {
		if (pos >= n)
			return false;
		len = len << 8 | p[pos++];
	}
	size_t end = pos + len;
	if (end > n)
		return false;
	bool first = true, z = h & 0x20;
	while (pos < end) {
		if (!first || z) {           /* delta time: up to 4 bytes */
			for (int k = 0; k < 4 && pos < end; k++)
				if (!(p[pos++] & 0x80))
					break;
		}
		first = false;
		if (pos >= end)
			break;
		uint8_t b = p[pos];
		if (b == 0xf0) {
			pos++;
			rx->n = 0;
			rx->overflow = false;
			rx->in_sysex = true;
			rx->sysex[rx->n++] = 0xf0;
			sysex_bytes(rx, p, &pos, end, cb, data);
			continue;
		}
		if (b == 0xf7) {             /* a continuation segment (or a stray end) */
			pos++;
			if (rx->in_sysex)
				sysex_bytes(rx, p, &pos, end, cb, data);
			continue;
		}
		uint8_t st;
		if (b & 0x80) {
			st = b;
			pos++;
			if (st >= 0xf8) {
				cb(data, &st, 1);
				continue;
			}
			rx->running = st < 0xf0 ? st : 0;
		} else {
			st = rx->running;
			if (!st)
				break;               /* no status to run on: give up on this list */
		}
		int dl = data_len(st);
		if (dl < 0 || pos + (size_t)dl > end)
			break;
		uint8_t m[3] = { st, 0, 0 };
		for (int k = 0; k < dl; k++) {
			if (p[pos] & 0x80)
				return true;     /* malformed */
			m[1 + k] = p[pos++];
		}
		cb(data, m, 1 + (size_t)dl);
	}
	return true;
}

size_t rtpm_encode(uint16_t seq, uint32_t ts, uint32_t ssrc, const uint8_t *msgs, size_t msgs_len, uint8_t *out,
		   size_t cap)
{
	if (cap < 14)
		return 0;
	out[0] = 0x80;
	out[1] = RTP_PT_MIDI;
	be16(out + 2, seq);
	be32(out + 4, ts);
	be32(out + 8, ssrc);
	size_t pos = 14, len = 0; /* the 2-byte (B=1) section header, filled below */
	bool first = true;
	for (size_t i = 0; i + 2 <= msgs_len;) {
		size_t mn = (size_t)msgs[i] | (size_t)msgs[i + 1] << 8;
		const uint8_t *m = msgs + i + 2;
		i += 2 + mn;
		if (!mn || i > msgs_len)
			break;
		size_t need = mn + (first ? 0 : 1);
		if (pos + need > cap || len + need > 0x0fff)
			break;
		if (!first)
			out[pos++] = 0;      /* delta time 0 */
		memcpy(out + pos, m, mn);
		pos += mn;
		len += need;
		first = false;
	}
	out[12] = 0x80 | (uint8_t)(len >> 8);  /* B=1, J=0, Z=0, P=0 */
	out[13] = (uint8_t)len;
	return pos;
}

/* ================================================================ session */
static const char *state_names[] = { "idle", "inviting", "inviting", "open", "refused", "lost", "closed" };

const char *rtpm_state_name(enum rtpm_state st)
{
	return (unsigned)st < sizeof(state_names) / sizeof(state_names[0]) ? state_names[st] : "?";
}

static uint64_t ts_now(const struct rtpm_session *s, uint64_t now)
{
	return (now - s->t0) / 100000ull; /* 100 us units */
}

static void set_state(struct rtpm_session *s, enum rtpm_state st)
{
	if (s->state == st)
		return;
	enum rtpm_state old = s->state;
	s->state = st;
	if (st == RTPM_LOST)
		s->lost_count++;
	if (st != RTPM_OPEN) {
		s->pending_len = 0;
		s->rx.in_sysex = false;
	}
	if (s->ev && s->ev->state)
		s->ev->state(s->data, old, st);
}

static void out_packet(struct rtpm_session *s, enum rtpm_port port, const uint8_t *p, size_t n)
{
	if (!n || !s->ev || !s->ev->send)
		return;
	s->packets_out++;
	s->ev->send(s->data, port, p, n);
}

static void send_session_cmd(struct rtpm_session *s, enum rtpm_port port, enum rtpm_cmd_kind kind)
{
	uint8_t b[16 + RTPM_NAME_MAX];
	out_packet(s, port, b, rtpm_build_session(kind, s->token, s->ssrc, s->name, b, sizeof(b)));
}

static void send_ck(struct rtpm_session *s, uint8_t count, uint64_t ts[3])
{
	uint8_t b[36];
	out_packet(s, RTPM_DATA, b, rtpm_build_ck(s->ssrc, count, ts, b, sizeof(b)));
}

void rtpm_init(struct rtpm_session *s, enum rtpm_role role, const char *name, uint32_t ssrc, uint64_t now,
	       const struct rtpm_events *ev, void *data)
{
	memset(s, 0, sizeof(*s));
	s->role = role;
	snprintf(s->name, sizeof(s->name), "%s", name ? name : "");
	s->ssrc = ssrc;
	s->t0 = now;
	s->seq = (uint16_t)(ssrc ^ (ssrc >> 16));
	s->ev = ev;
	s->data = data;
}

void rtpm_start(struct rtpm_session *s, uint64_t now)
{
	if (s->role != RTPM_INITIATOR)
		return;
	s->token = (uint32_t)(s->ssrc * 2654435761u) ^ (uint32_t)(now / 1000);
	s->tries = 0;
	s->have_peer_seq = false;
	set_state(s, RTPM_INVITING);
	send_session_cmd(s, RTPM_CONTROL, RTPM_CMD_IN);
	s->next = now + RTPM_RETRY_MS * MS;
}

static void deliver(void *data, const uint8_t *m, size_t n)
{
	struct rtpm_session *s = data;
	if (s->ev && s->ev->midi)
		s->ev->midi(s->data, m, n);
}

static void on_command(struct rtpm_session *s, enum rtpm_port port, const struct rtpm_cmd *c, uint64_t now)
{
	switch (c->kind) {
	case RTPM_CMD_IN:
		if (s->role != RTPM_RESPONDER)
			return;
		if (port == RTPM_CONTROL) {
			/* a new invitation (or the same one again: its OK was lost) */
			s->token = c->token;
			s->peer_ssrc = c->ssrc;
			snprintf(s->peer_name, sizeof(s->peer_name), "%s", c->name);
			s->have_peer_seq = false;
			s->probes_answered = false;
			set_state(s, RTPM_INVITING_DATA);
			send_session_cmd(s, RTPM_CONTROL, RTPM_CMD_OK);
		} else if (c->ssrc == s->peer_ssrc && c->token == s->token) {
			set_state(s, RTPM_OPEN);
			send_session_cmd(s, RTPM_DATA, RTPM_CMD_OK);
			s->next = now + RTPM_SYNC_MS * MS;
		}
		return;
	case RTPM_CMD_OK:
		if (s->role != RTPM_INITIATOR || c->token != s->token)
			return;
		if (port == RTPM_CONTROL && s->state == RTPM_INVITING) {
			s->peer_ssrc = c->ssrc;
			snprintf(s->peer_name, sizeof(s->peer_name), "%s", c->name);
			s->tries = 0;
			set_state(s, RTPM_INVITING_DATA);
			send_session_cmd(s, RTPM_DATA, RTPM_CMD_IN);
			s->next = now + RTPM_RETRY_MS * MS;
		} else if (port == RTPM_DATA && s->state == RTPM_INVITING_DATA) {
			set_state(s, RTPM_OPEN);
			uint64_t ts[3] = { ts_now(s, now), 0, 0 };
			send_ck(s, 0, ts);
			s->next = now + RTPM_SYNC_MS * MS;
		}
		return;
	case RTPM_CMD_NO:
		if (s->role == RTPM_INITIATOR && c->token == s->token &&
		    (s->state == RTPM_INVITING || s->state == RTPM_INVITING_DATA)) {
			if (c->name[0])
				snprintf(s->peer_name, sizeof(s->peer_name), "%s", c->name);
			set_state(s, RTPM_REFUSED);
			s->next = now + RTPM_REFUSED_RETRY_MS * MS;
		}
		return;
	case RTPM_CMD_BY:
		if (c->ssrc != s->peer_ssrc || s->state == RTPM_IDLE || s->state == RTPM_CLOSED)
			return;
		if (s->role == RTPM_INITIATOR) {
			/* the device ended it (switched off, another mode): ask again */
			set_state(s, RTPM_LOST);
			s->next = now + RTPM_RETRY_MS * MS;
		} else {
			set_state(s, RTPM_CLOSED);
		}
		return;
	case RTPM_CMD_CK: {
		if (s->state != RTPM_OPEN)
			return;
		uint64_t ts[3] = { c->ts[0], c->ts[1], c->ts[2] };
		if (c->count == 0) {
			ts[1] = ts_now(s, now);
			send_ck(s, 1, ts);
		} else if (c->count == 1) {
			if (s->role == RTPM_RESPONDER)
				s->probes_answered = true;
			ts[2] = ts_now(s, now);
			s->latency_100us = (ts[2] - ts[0]) / 2;
			send_ck(s, 2, ts);
		} else {
			s->latency_100us = ts_now(s, now) > ts[1] ? (ts_now(s, now) - ts[1]) / 2 : 0;
		}
		return;
	}
	default:
		return;
	}
}

void rtpm_input(struct rtpm_session *s, enum rtpm_port port, const uint8_t *p, size_t n, uint64_t now)
{
	struct rtpm_cmd c;
	if (rtpm_parse_command(p, n, &c)) {
		/* a session's own packets carry the peer's ssrc (IN carries a new one) */
		if (c.kind != RTPM_CMD_IN && c.kind != RTPM_CMD_OK && c.kind != RTPM_CMD_NO && s->peer_ssrc &&
		    c.ssrc != s->peer_ssrc)
			return;
		s->packets_in++;
		s->last_rx = now;
		on_command(s, port, &c, now);
		return;
	}
	if (port != RTPM_DATA || s->state != RTPM_OPEN)
		return;
	uint16_t seq;
	uint32_t ssrc;
	if (n >= 12 && (p[0] >> 6) == 2 && s->peer_ssrc && rd32(p + 8) != s->peer_ssrc)
		return;
	s->last_rx = now;
	s->packets_in++;
	if (rtpm_decode(&s->rx, p, n, &seq, &ssrc, deliver, s)) {
		s->peer_seq = seq;
		s->have_peer_seq = true;
		s->rs_due = true;
	}
}

void rtpm_flush(struct rtpm_session *s, uint64_t now)
{
	if (!s->pending_len)
		return;
	if (s->state == RTPM_OPEN) {
		uint8_t b[RTPM_PACKET_MAX];
		size_t n = rtpm_encode(s->seq++, (uint32_t)ts_now(s, now), s->ssrc, s->pending, s->pending_len, b, sizeof(b));
		out_packet(s, RTPM_DATA, b, n);
	}
	s->pending_len = 0;
}

bool rtpm_send(struct rtpm_session *s, const uint8_t *msg, size_t n, uint64_t now)
{
	if (s->state != RTPM_OPEN || !n || n > RTPM_SYSEX_MAX)
		return false;
	/* the packet: 12 RTP + 2 section header + each message and a delta byte */
	size_t enc = 14, k = 0, count = 0;
	while (k + 2 <= s->pending_len) {
		size_t mn = (size_t)s->pending[k] | (size_t)s->pending[k + 1] << 8;
		enc += mn + (count ? 1 : 0);
		count++;
		k += 2 + mn;
	}
	if (enc + n + 1 > RTPM_PACKET_MAX || s->pending_len + 2 + n > sizeof(s->pending))
		rtpm_flush(s, now);
	s->pending[s->pending_len++] = (uint8_t)n;
	s->pending[s->pending_len++] = (uint8_t)(n >> 8);
	memcpy(s->pending + s->pending_len, msg, n);
	s->pending_len += n;
	return true;
}

void rtpm_tick(struct rtpm_session *s, uint64_t now)
{
	switch (s->state) {
	case RTPM_INVITING:
	case RTPM_INVITING_DATA:
		/* a responder invited on its control port only: give up after a while */
		if (s->role == RTPM_RESPONDER) {
			if (now - s->last_rx > RTPM_LOST_MS * MS)
				set_state(s, RTPM_LOST);
			break;
		}
		if (now >= s->next) {
			s->tries++;
			if (s->state == RTPM_INVITING_DATA && s->tries > 3) {
				rtpm_start(s, now); /* start over on the control port */
				return;
			}
			send_session_cmd(s, s->state == RTPM_INVITING ? RTPM_CONTROL : RTPM_DATA, RTPM_CMD_IN);
			s->next = now + RTPM_RETRY_MS * MS;
		}
		break;
	case RTPM_REFUSED:
	case RTPM_LOST:
		if (s->role == RTPM_INITIATOR && now >= s->next)
			rtpm_start(s, now);
		break;
	case RTPM_OPEN: {
		uint64_t quiet = s->role == RTPM_INITIATOR || s->probes_answered ? RTPM_LOST_MS : RTPM_RESPONDER_LOST_MS;
		if (now - s->last_rx > quiet * MS) {
			set_state(s, RTPM_LOST);
			s->next = now + RTPM_RETRY_MS * MS;
			return;
		}
		if (now >= s->next) { /* the initiator's sync; a responder's probe */
			uint64_t ts[3] = { ts_now(s, now), 0, 0 };
			send_ck(s, 0, ts);
			s->next = now + RTPM_SYNC_MS * MS;
		}
		rtpm_flush(s, now);
		if (s->rs_due && now - s->last_rs > 1000 * MS) {
			uint8_t b[12];
			out_packet(s, RTPM_CONTROL, b, rtpm_build_rs(s->ssrc, s->peer_seq, b, sizeof(b)));
			s->rs_due = false;
			s->last_rs = now;
		}
		break;
	}
	default:
		break;
	}
}

void rtpm_bye(struct rtpm_session *s)
{
	if (s->state == RTPM_OPEN || s->state == RTPM_INVITING_DATA) {
		uint8_t b[16];
		out_packet(s, RTPM_CONTROL, b, rtpm_build_session(RTPM_CMD_BY, s->token, s->ssrc, NULL, b, sizeof(b)));
	}
	set_state(s, RTPM_CLOSED);
}
