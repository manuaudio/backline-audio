/* Offload1's data plane: the BLO1 frame and the console's side of the
 * lockstep loop with a primary DSP server and a mirror (docs/api/Offload1.md).
 *
 * Header-only C, MIT, libc only. The console sends every block to every
 * server of a slot (the primary and its mirror run the same chain on the
 * same input), then waits until its deadline for the block to come back. It
 * listens to one server, the active one: when the active server's block is
 * missing or marked dry at the deadline, the mirror's block is used and the
 * mirror becomes active. A takeover therefore costs no block. Only when no
 * server answers does the console play the dry input delayed by the slot's
 * latency.
 *
 * Realtime: blo_client_send() and blo_client_wait() never allocate or lock;
 * the only syscalls are sendto, ppoll and recvfrom.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_OFFLOAD1_H
#define BACKLINE_OFFLOAD1_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLO1_MAGIC 0x314F4C42u      /* "BLO1" little-endian */
#define BLO1_ETHERTYPE 0x88B5       /* IEEE local experimental (the L2 transport) */
#define BLO1_DEFAULT_PORT 49800     /* UDP: slot k listens on port + k */
#define BLO1_MAX_CHANNELS 8
#define BLO1_MAX_FRAMES 256
#define BLO1_MAX_PEERS 2            /* primary and mirror */
#define BLO1_MAX_EVENTS 32          /* parameter events per control frame */

/* flags */
#define BLO1_F_DRY 0x1u      /* server → console: the server could not run the chain; the samples are its delayed dry */
#define BLO1_F_PARAM 0x2u    /* console → server: a control frame, the payload is events, not audio */
#define BLO1_F_HELLO 0x4u    /* console → server: hello; the reply carries the slot's latency in server_us */

/* 32 bytes, then the payload: planar float32, channels × frames. */
struct blo1_hdr {
	uint32_t magic;
	uint32_t seq;          /* the console's block number */
	uint16_t frames, channels;
	uint32_t flags;
	uint64_t t_send_ns;    /* the console's clock, echoed */
	uint32_t server_us;    /* the server's time for this block (HELLO: the slot's latency, frames) */
	uint16_t slot;         /* the server's slot index */
	uint16_t part;         /* 0 (a slot wider than one frame splits; not in v1) */
};

/* A parameter event in a control frame (BLO1_F_PARAM): the Slot1 event
 * layout (slot1.h), `slot` being the insert in the chain. */
struct blo1_event {
	uint8_t insert, type;  /* type: 1 PARAM (plugin units), 2 PARAM_NORM (0..1), 3 MIDI */
	uint16_t offset;
	uint32_t id;
	double value;
};

#ifdef __cplusplus
static_assert(sizeof(struct blo1_hdr) == 32, "BLO1 header");
static_assert(sizeof(struct blo1_event) == 16, "BLO1 event");
#else
_Static_assert(sizeof(struct blo1_hdr) == 32, "BLO1 header");
_Static_assert(sizeof(struct blo1_event) == 16, "BLO1 event");
#endif

#define BLO1_MAX_PAYLOAD (BLO1_MAX_CHANNELS * BLO1_MAX_FRAMES * 4)
#define BLO1_MAX_PACKET (32 + BLO1_MAX_PAYLOAD)

static inline uint64_t blo1_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ console side */

enum blo1_result {
	BLO1_ACTIVE = 0,     /* the active server's block */
	BLO1_TAKEOVER = 1,   /* the active server missed; the mirror's block, and the mirror is active now */
	BLO1_DRY = 2,        /* no server answered in time: the console's dry, delayed by the slot latency */
};

struct blo1_peer {
	int fd;
	struct sockaddr_in addr;
	uint64_t served, late, dry_marked;
	uint64_t rtt_ns_last, rtt_ns_max;
};

struct blo1_client {
	struct blo1_peer peer[BLO1_MAX_PEERS];
	int n_peers;
	int active;
	int spin;                     /* busy-poll until the deadline instead of sleeping in ppoll */
	uint16_t slot, channels, frames;
	uint32_t latency;             /* frames: the console's PDC and dry path use it */
	uint32_t seq;
	uint64_t t_send;
	uint64_t takeovers, dry_blocks, blocks;
	/* the console's dry delay line (channels × dly_len) */
	float *dly;
	uint32_t dly_len, dly_pos;
	uint8_t tx[BLO1_MAX_PACKET] __attribute__((aligned(16)));
	uint8_t rx[BLO1_MAX_PACKET] __attribute__((aligned(16)));
	uint8_t spare[BLO1_MAX_PACKET] __attribute__((aligned(16)));
	uint8_t junk[BLO1_MAX_PACKET] __attribute__((aligned(16)));
};

/* Opens one UDP socket per server ("a.b.c.d", port). Off the audio thread. */
static inline int blo1_client_open(struct blo1_client *c, uint16_t slot, uint16_t channels, uint16_t frames,
				   const char *const *hosts, const uint16_t *ports, int n, uint32_t max_latency)
{
	memset(c, 0, sizeof(*c));
	if (n < 1 || n > BLO1_MAX_PEERS || !channels || channels > BLO1_MAX_CHANNELS || !frames || frames > BLO1_MAX_FRAMES)
		return -EINVAL;
	c->slot = slot;
	c->channels = channels;
	c->frames = frames;
	c->n_peers = n;
	for (int i = 0; i < n; i++) {
		struct blo1_peer *p = &c->peer[i];
		p->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (p->fd < 0)
			return -errno;
		p->addr.sin_family = AF_INET;
		p->addr.sin_port = htons(ports[i]);
		if (inet_pton(AF_INET, hosts[i], &p->addr.sin_addr) != 1)
			return -EINVAL;
		int prio = 6; /* the interactive band: ahead of bulk traffic on a shared NIC */
		setsockopt(p->fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
		if (connect(p->fd, (struct sockaddr *)&p->addr, sizeof(p->addr)) < 0)
			return -errno;
	}
	c->dly_len = (max_latency ? max_latency : 16384) + frames;
	c->dly = (float *)calloc((size_t)channels * c->dly_len, sizeof(float));
	return c->dly ? 0 : -ENOMEM;
}

static inline void blo1_client_close(struct blo1_client *c)
{
	for (int i = 0; i < c->n_peers; i++)
		if (c->peer[i].fd > 0)
			close(c->peer[i].fd);
	free(c->dly);
	memset(c, 0, sizeof(*c));
}

/* Asks every server for the slot's latency (off the audio thread). Returns
 * the latency, or -1 when no server answered within timeout_ms. Servers that
 * disagree are an error (-2): a mirror must run the same chain. */
static inline int blo1_client_hello(struct blo1_client *c, int timeout_ms)
{
	struct blo1_hdr h = { BLO1_MAGIC, 0, c->frames, c->channels, BLO1_F_HELLO, blo1_now_ns(), 0, c->slot, 0 };
	int lat = -1, answered = 0;
	for (int i = 0; i < c->n_peers; i++)
		send(c->peer[i].fd, &h, sizeof(h), 0);
	uint64_t until = blo1_now_ns() + (uint64_t)timeout_ms * 1000000ull;
	while (answered < c->n_peers && blo1_now_ns() < until) {
		struct pollfd pf[BLO1_MAX_PEERS];
		for (int i = 0; i < c->n_peers; i++)
			pf[i] = (struct pollfd){ c->peer[i].fd, POLLIN, 0 };
		if (poll(pf, (nfds_t)c->n_peers, 20) <= 0)
			continue;
		for (int i = 0; i < c->n_peers; i++) {
			struct blo1_hdr r;
			if (!(pf[i].revents & POLLIN) || recv(c->peer[i].fd, &r, sizeof(r), 0) != (ssize_t)sizeof(r))
				continue;
			if (r.magic != BLO1_MAGIC || !(r.flags & BLO1_F_HELLO))
				continue;
			if (lat >= 0 && (int)r.server_us != lat)
				return -2;
			lat = (int)r.server_us;
			answered++;
		}
	}
	if (lat >= 0)
		c->latency = (uint32_t)lat;
	return lat;
}

/* Sends parameter events to one server (peer ≥ 0) or every server (-1). */
static inline int blo1_client_params(struct blo1_client *c, int peer, const struct blo1_event *ev, int n)
{
	if (n < 1 || n > BLO1_MAX_EVENTS)
		return -EINVAL;
	uint8_t buf[32 + BLO1_MAX_EVENTS * sizeof(struct blo1_event)];
	struct blo1_hdr h = { BLO1_MAGIC, c->seq, c->frames, c->channels, BLO1_F_PARAM, blo1_now_ns(), 0, c->slot, (uint16_t)n };
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + 32, ev, (size_t)n * sizeof(*ev));
	for (int i = 0; i < c->n_peers; i++)
		if (peer < 0 || peer == i)
			send(c->peer[i].fd, buf, 32 + (size_t)n * sizeof(*ev), 0);
	return 0;
}

/* Sends block `seq` (the next one) to every server. `in`: channels planar
 * buffers of `frames`. Feeds the dry delay line. */
static inline void blo1_client_send(struct blo1_client *c, const float *const *in)
{
	uint32_t ch = c->channels, n = c->frames, len = c->dly_len;
	for (uint32_t k = 0; k < ch; k++) {
		float *d = c->dly + (size_t)k * len;
		uint32_t w = c->dly_pos;
		for (uint32_t i = 0; i < n; i++) {
			d[w] = in[k][i];
			if (++w == len)
				w = 0;
		}
	}
	c->seq++;
	c->t_send = blo1_now_ns();
	struct blo1_hdr *h = (struct blo1_hdr *)c->tx;
	*h = (struct blo1_hdr){ BLO1_MAGIC, c->seq, (uint16_t)n, (uint16_t)ch, 0, c->t_send, 0, c->slot, 0 };
	float *s = (float *)(c->tx + 32);
	for (uint32_t k = 0; k < ch; k++)
		memcpy(s + (size_t)k * n, in[k], n * sizeof(float));
	size_t bytes = 32 + (size_t)ch * n * 4;
	for (int i = 0; i < c->n_peers; i++)
		send(c->peer[i].fd, c->tx, bytes, MSG_DONTWAIT);
}

static inline void blo1__out(struct blo1_client *c, const uint8_t *pkt, float *const *out)
{
	const float *s = (const float *)(pkt + 32);
	for (uint32_t k = 0; k < c->channels; k++)
		memcpy(out[k], s + (size_t)k * c->frames, c->frames * sizeof(float));
}

/* Waits until deadline_ns (CLOCK_MONOTONIC) for block `seq` and writes it to
 * `out`. *served: the peer whose block was used (-1: the console's dry). */
static inline enum blo1_result blo1_client_wait(struct blo1_client *c, float *const *out, uint64_t deadline_ns, int *served)
{
	uint32_t ch = c->channels, n = c->frames, len = c->dly_len;
	size_t bytes = 32 + (size_t)ch * n * 4;
	int have_spare = -1; /* a non-active peer's valid block */
	int got_active = 0;
	c->blocks++;
	for (;;) {
		struct pollfd pf[BLO1_MAX_PEERS];
		for (int i = 0; i < c->n_peers; i++)
			pf[i] = (struct pollfd){ c->peer[i].fd, POLLIN, 0 };
		uint64_t now = blo1_now_ns();
		if (now >= deadline_ns)
			break;
		if (c->spin) {
			for (int i = 0; i < c->n_peers; i++)
				pf[i].revents = POLLIN; /* try every socket, without sleeping */
		} else {
			uint64_t left = deadline_ns - now;
			struct timespec ts = { (time_t)(left / 1000000000ull), (long)(left % 1000000000ull) };
			if (ppoll(pf, (nfds_t)c->n_peers, &ts, NULL) <= 0)
				continue;
		}
		for (int i = 0; i < c->n_peers; i++) {
			if (!(pf[i].revents & POLLIN))
				continue;
			for (;;) {
				/* the active server's block, the first valid spare; the rest is read and dropped */
				uint8_t *buf = i == c->active ? c->rx : have_spare < 0 ? c->spare : c->junk;
				ssize_t r = recv(c->peer[i].fd, buf, BLO1_MAX_PACKET, MSG_DONTWAIT);
				if (r < 0)
					break;
				const struct blo1_hdr *h = (const struct blo1_hdr *)buf;
				if ((size_t)r != bytes || h->magic != BLO1_MAGIC || h->seq != c->seq || (h->flags & (BLO1_F_PARAM | BLO1_F_HELLO)))
					continue; /* stale, or not ours */
				uint64_t rtt = blo1_now_ns() - h->t_send_ns;
				c->peer[i].rtt_ns_last = rtt;
				if (rtt > c->peer[i].rtt_ns_max)
					c->peer[i].rtt_ns_max = rtt;
				if (h->flags & BLO1_F_DRY) {
					c->peer[i].dry_marked++;
					continue;
				}
				if (i == c->active)
					got_active = 1;
				else if (have_spare < 0)
					have_spare = i;
				break;
			}
		}
		if (got_active)
			break;
	}
	enum blo1_result res;
	if (got_active) {
		blo1__out(c, c->rx, out);
		c->peer[c->active].served++;
		*served = c->active;
		res = BLO1_ACTIVE;
	} else if (have_spare >= 0) {
		c->peer[c->active].late++;
		blo1__out(c, c->spare, out);
		c->active = have_spare;
		c->peer[have_spare].served++;
		c->takeovers++;
		*served = have_spare;
		res = BLO1_TAKEOVER;
	} else {
		c->peer[c->active].late++;
		uint32_t L = c->latency < len - n ? c->latency : len - n;
		for (uint32_t k = 0; k < ch; k++) {
			const float *d = c->dly + (size_t)k * len;
			uint32_t r = (c->dly_pos + len - L) % len;
			for (uint32_t i = 0; i < n; i++) {
				out[k][i] = d[r];
				if (++r == len)
					r = 0;
			}
		}
		c->dry_blocks++;
		*served = -1;
		res = BLO1_DRY;
	}
	c->dly_pos = (c->dly_pos + n) % len;
	return res;
}

#ifdef __cplusplus
}
#endif
#endif
