/* RTP-MIDI: the AppleMIDI session protocol and RFC 6295 MIDI payloads, pure.
 * No sockets and no clock: the daemon (surface/rtpnet.c), the software
 * device (tools/mcu-sim.c) and the unit tests pass packets and the time in,
 * and get packets out through a callback.
 *
 * Written from RFC 6295 (RTP Payload Format for MIDI) and Apple's
 * "RTP-MIDI" session description (the IN/OK/NO/BY/CK/RS exchange on a
 * control port and the data port next to it).
 *
 * A session has two ends: the initiator invites (control port, then data
 * port), then keeps the clocks in sync with CK every second; the responder
 * answers. Either end may send MIDI once the session is open. Here an
 * initiator also notices a silent peer (nothing heard for RTPM_LOST_MS) and
 * invites again until it answers, so a network drop ends in the same place.
 * A responder sends CK probes as well; when the initiator answers them, a
 * silent initiator is noticed as fast, otherwise only after
 * RTPM_RESPONDER_LOST_MS (the initiator invites again when it is back).
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_RTPMIDI_H
#define BACKLINE_RTPMIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTPM_NAME_MAX 64
#define RTPM_PACKET_MAX 1400        /* stays under a 1500-byte Ethernet MTU */
#define RTPM_SYSEX_MAX 1024
#define RTPM_SYNC_MS 1000           /* CK from the initiator */
#define RTPM_LOST_MS 3000           /* nothing heard for this long: the peer is gone */
#define RTPM_RETRY_MS 1000          /* invitations while the peer doesn't answer */
#define RTPM_REFUSED_RETRY_MS 5000  /* after NO: the device may free a session later */
#define RTPM_RESPONDER_LOST_MS 30000 /* a responder whose CK probes go unanswered */

enum rtpm_port { RTPM_CONTROL = 0, RTPM_DATA = 1 };
enum rtpm_role { RTPM_INITIATOR, RTPM_RESPONDER };
enum rtpm_state {
	RTPM_IDLE,        /* not started */
	RTPM_INVITING,    /* IN sent on the control port, waiting for OK */
	RTPM_INVITING_DATA,
	RTPM_OPEN,        /* both ports accepted: MIDI flows */
	RTPM_REFUSED,     /* the peer said NO (it has no free session); asked again later */
	RTPM_LOST,        /* the peer went silent; invited again (initiator) */
	RTPM_CLOSED,      /* BY sent or received */
};

/* ---- command packets (FF FF + two letters) */
enum rtpm_cmd_kind { RTPM_CMD_NONE, RTPM_CMD_IN, RTPM_CMD_OK, RTPM_CMD_NO, RTPM_CMD_BY, RTPM_CMD_CK, RTPM_CMD_RS };

struct rtpm_cmd {
	enum rtpm_cmd_kind kind;
	uint32_t version, token, ssrc;
	char name[RTPM_NAME_MAX];
	uint8_t count;              /* CK: 0, 1, 2 */
	uint64_t ts[3];             /* CK timestamps, 100 us units */
	uint16_t seq;               /* RS */
};

bool rtpm_is_command(const uint8_t *p, size_t n);
bool rtpm_parse_command(const uint8_t *p, size_t n, struct rtpm_cmd *out);
/* IN, OK, NO (with name), BY. Returns the length. */
size_t rtpm_build_session(enum rtpm_cmd_kind kind, uint32_t token, uint32_t ssrc, const char *name, uint8_t *out,
			  size_t cap);
size_t rtpm_build_ck(uint32_t ssrc, uint8_t count, const uint64_t ts[3], uint8_t *out, size_t cap);
size_t rtpm_build_rs(uint32_t ssrc, uint16_t seq, uint8_t *out, size_t cap);

/* ---- RTP-MIDI data packets */
/* Reassembles SysEx that a sender split across commands or packets. */
struct rtpm_rx {
	uint8_t sysex[RTPM_SYSEX_MAX];
	size_t n;
	bool in_sysex, overflow;
	uint8_t running;
};
typedef void (*rtpm_midi_cb)(void *data, const uint8_t *msg, size_t n);
/* An RTP packet: its MIDI command section (the recovery journal is skipped).
 * Each complete message goes to cb (SysEx with F0 and F7). Returns false if
 * it is not an RTP-MIDI packet. *seq and *ssrc get the RTP header's. */
bool rtpm_decode(struct rtpm_rx *rx, const uint8_t *p, size_t n, uint16_t *seq, uint32_t *ssrc, rtpm_midi_cb cb,
		 void *data);
/* Builds an RTP-MIDI packet of whole messages (each with its status byte,
 * delta times 0, no journal). msgs: the messages back to back, each a
 * 2-byte little-endian length and its bytes. Returns the length. */
size_t rtpm_encode(uint16_t seq, uint32_t ts, uint32_t ssrc, const uint8_t *msgs, size_t msgs_len, uint8_t *out,
		   size_t cap);

/* ---- a session */
struct rtpm_session;
struct rtpm_events {
	/* a packet to the peer's control or data port */
	void (*send)(void *data, enum rtpm_port port, const uint8_t *p, size_t n);
	void (*midi)(void *data, const uint8_t *msg, size_t n);
	void (*state)(void *data, enum rtpm_state old, enum rtpm_state now);
};

struct rtpm_session {
	enum rtpm_role role;
	enum rtpm_state state;
	char name[RTPM_NAME_MAX], peer_name[RTPM_NAME_MAX];
	uint32_t ssrc, peer_ssrc, token;
	uint16_t seq, peer_seq;
	bool have_peer_seq, rs_due;
	bool probes_answered;       /* responder: the initiator answers our CK too */
	uint64_t t0, last_rx, next, last_rs;
	uint64_t latency_100us;     /* one-way, from the last CK exchange */
	int tries;
	uint64_t packets_in, packets_out, lost_count;
	struct rtpm_rx rx;
	uint8_t pending[RTPM_PACKET_MAX];  /* the next data packet's messages (rtpm_encode's msgs) */
	size_t pending_len;
	const struct rtpm_events *ev;
	void *data;
};

void rtpm_init(struct rtpm_session *s, enum rtpm_role role, const char *name, uint32_t ssrc, uint64_t now,
	       const struct rtpm_events *ev, void *data);
/* Initiator: start inviting (again). */
void rtpm_start(struct rtpm_session *s, uint64_t now);
/* A packet from the peer on one of its ports. */
void rtpm_input(struct rtpm_session *s, enum rtpm_port port, const uint8_t *p, size_t n, uint64_t now);
/* Timers: invitations, CK, the silence timeout; sends what is pending. */
void rtpm_tick(struct rtpm_session *s, uint64_t now);
/* Queues one MIDI message (sent at the next rtpm_flush or rtpm_tick, or now
 * if the packet is full). False if the session is not open. */
bool rtpm_send(struct rtpm_session *s, const uint8_t *msg, size_t n, uint64_t now);
void rtpm_flush(struct rtpm_session *s, uint64_t now);
/* Ends the session: BY to the peer. */
void rtpm_bye(struct rtpm_session *s);
const char *rtpm_state_name(enum rtpm_state st);

#ifdef __cplusplus
}
#endif
#endif
