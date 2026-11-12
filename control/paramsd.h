/* Params1 inside controld (docs/api/Params1.md): the D-Bus object, the
 * parameter store, OSC get/set/touch/subscribe/meters, roles and gestures.
 * controld owns the sockets and calls in here; this module never touches a
 * socket itself.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_PARAMSD_H
#define BACKLINE_PARAMSD_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include <pipewire/loop.h>
#include <systemd/sd-bus.h>

#include "osc.h"
#include "params.h"

#define PD_OBJ_PATH "/io/github/manuaudio/Backline/Params"
#define PD_IFACE "io.github.manuaudio.Backline.Params1"

/* Who an OSC message came from, and where a reply goes. */
struct pd_peer {
	int listener;                 /* controld's listener index */
	void *conn;                   /* the TCP connection, NULL for UDP */
	struct sockaddr_storage addr; /* UDP: where replies go */
	socklen_t alen;
	bool tcp;
	char role[32];                /* the listener's params role */
	char label[96];               /* "osc:udp:127.0.0.1:9000" */
};

struct pd_hooks {
	/* Send one OSC packet (a message or a bundle) to a peer. */
	void (*osc_send)(void *data, const struct pd_peer *peer, const uint8_t *pkt, size_t n);
	/* A parameter's confirmed value changed (Update/UpdateText). */
	void (*changed)(void *data, struct prm *p);
	void *data;
};

int pd_init(sd_bus *bus, struct pw_loop *loop, const struct pd_hooks *hooks, bool verbose);
void pd_fini(void);

/* An OSC message from a listener that serves Params1. true: it was ours
 * (a parameter path under a registered root, or /backline/get|sub|unsub|
 * meters|renew). */
bool pd_osc(const struct pd_peer *peer, const struct osc_msg *m);
/* A TCP connection closed (conn) or a listener closed (conn NULL, listener):
 * drop its subscriptions, end its gestures. */
void pd_peer_gone(int listener, void *conn);
/* Called by controld's housekeeping timer (expiry of UDP subscriptions and
 * gestures). */
void pd_house(uint64_t now_ns);

/* Requests from inside controld (Control1 param: targets). 0, or -1 with a
 * reason. norm: value is 0..1. */
int pd_set(const char *path, bool norm, double value, const char *client, const char *role, char *why, size_t n);
int pd_gesture(const char *path, bool begin, const char *client, const char *role, char *why, size_t n);
struct prm *pd_get(const char *path);

bool pd_busy(void);              /* roots registered or subscriptions open */
uint32_t pd_subscriptions(void);

#endif
