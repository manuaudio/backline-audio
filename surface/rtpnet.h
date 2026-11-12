/* Network MIDI for surfaced: RTP-MIDI sessions (rtpmidi.c) on a pair of UDP
 * sockets, and the devices found by mDNS (_apple-midi._udp, through
 * avahi-daemon) or that invite us. One instance, on the daemon's PipeWire
 * main loop. A peer becomes a unit when its session is open, exactly like a
 * USB device's MIDI port.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_RTPNET_H
#define BACKLINE_RTPNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pw_loop;

#define RN_MAX_PEERS 16

struct rn_events {
	/* Is a device with this session or service name one we drive? */
	bool (*want)(void *data, const char *name);
	void (*opened)(void *data, int peer);
	/* The session closed or the peer went silent (lost: a drop, not a BY). */
	void (*closed)(void *data, int peer, bool lost);
	void (*midi)(void *data, int peer, const uint8_t *msg, size_t n);
	void (*changed)(void *data);  /* a peer was found, refused, ... (for listings) */
	void (*log)(void *data, const char *msg);
};

/* Binds the sockets (the first free pair from 5004) and starts browsing.
 * name: our session name. 0, or a negative errno. */
int rn_init(struct pw_loop *l, const char *name, const struct rn_events *ev, void *data);
void rn_tick(uint64_t now);
bool rn_send(int peer, const uint8_t *msg, size_t n);
/* Packs what rn_send queued into packets now (once per engine tick). */
void rn_flush(uint64_t now);
/* A device at a fixed address (no mDNS): its control port (5004 if 0). */
int rn_add(const char *host, uint16_t port);
/* Ends the session with the device added at that address, and forgets it. */
int rn_remove(const char *host, uint16_t port);
/* BY to every peer. */
void rn_bye_all(void);
void rn_close(void);

const char *rn_peer_name(int peer);
const char *rn_peer_address(int peer);    /* "10.0.0.5:5004" */
bool rn_peer_open(int peer);
/* Every peer as JSON (malloc'd): name, address, state, found ("mdns",
 * "invited", "address"), latency-ms, lost (count). */
char *rn_peers_json(void);
uint16_t rn_port(void);                   /* our control port, 0 if not up */
bool rn_discovery(void);                  /* browsing mDNS */

#endif
