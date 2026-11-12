/* Network MIDI for surfaced (rtpnet.h): RTP-MIDI sessions on UDP, devices
 * found by mDNS through avahi-daemon (the one mDNS stack on the machine;
 * surfaced never opens port 5353 itself).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "rtpnet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <pipewire/pipewire.h>

#ifdef BL_HAVE_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>
#include <avahi-common/watch.h>
#endif

#include "rtpmidi.h"

#define SERVICE "_apple-midi._udp"

enum found { F_MDNS, F_INVITED, F_ADDRESS };
static const char *found_names[] = { "mdns", "invited", "address" };

struct peer {
	bool used;
	enum found found;
	bool advertised;          /* mDNS still lists it */
	char name[RTPM_NAME_MAX]; /* the service name, then the session name it gave */
	char addr[64];
	struct sockaddr_in ctrl, data;
	struct rtpm_session s;
};

static struct {
	struct pw_loop *l;
	int fd[2];
	struct spa_source *src[2];
	uint16_t port;
	char name[RTPM_NAME_MAX];
	const struct rn_events *ev;
	void *data;
	struct peer peers[RN_MAX_PEERS];
	uint32_t seed;
	uint64_t last_refusal_log;
#ifdef BL_HAVE_AVAHI
	AvahiPoll poll;
	AvahiClient *client;
	AvahiServiceBrowser *browser;
#endif
} N = { .fd = { -1, -1 } };

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (N.ev && N.ev->log)
		N.ev->log(N.data, b);
}

static int idx(const struct peer *p) { return (int)(p - N.peers); }

static void changed(void)
{
	if (N.ev && N.ev->changed)
		N.ev->changed(N.data);
}

/* ================================================================ sessions */
static void p_send(void *data, enum rtpm_port port, const uint8_t *b, size_t n)
{
	struct peer *p = data;
	const struct sockaddr_in *to = port == RTPM_CONTROL ? &p->ctrl : &p->data;
	/* a failure (the network down, no route) is noticed by the silence timeout */
	(void)sendto(N.fd[port], b, n, 0, (const struct sockaddr *)to, sizeof(*to));
}

static void p_midi(void *data, const uint8_t *m, size_t n)
{
	struct peer *p = data;
	if (N.ev && N.ev->midi)
		N.ev->midi(N.data, idx(p), m, n);
}

static void p_state(void *data, enum rtpm_state old, enum rtpm_state st)
{
	struct peer *p = data;
	if (st == RTPM_OPEN) {
		if (p->s.peer_name[0] && p->found != F_MDNS)
			snprintf(p->name, sizeof(p->name), "%s", p->s.peer_name);
		logf_("%s (%s): session open", p->name, p->addr);
		if (N.ev && N.ev->opened)
			N.ev->opened(N.data, idx(p));
	} else if (old == RTPM_OPEN) {
		logf_("%s (%s): session %s", p->name, p->addr, st == RTPM_LOST ? "lost (no answer): asking again" : "closed");
		if (N.ev && N.ev->closed)
			N.ev->closed(N.data, idx(p), st == RTPM_LOST);
	} else if (st == RTPM_REFUSED) {
		if (p->s.peer_name[0] && p->found == F_ADDRESS)
			snprintf(p->name, sizeof(p->name), "%s", p->s.peer_name);
		logf_("%s (%s): the device refused the session (no free session on it?): asking again every %d s", p->name,
		      p->addr, RTPM_REFUSED_RETRY_MS / 1000);
	}
	changed();
}

static const struct rtpm_events peer_events = { p_send, p_midi, p_state };

static uint32_t new_ssrc(void)
{
	N.seed = N.seed * 1103515245u + 12345u + (uint32_t)now_ns();
	return N.seed ^ (uint32_t)getpid() << 16;
}

static void set_addr(struct peer *p, const struct sockaddr_in *ctrl)
{
	p->ctrl = *ctrl;
	p->data = *ctrl;
	p->data.sin_port = htons((uint16_t)(ntohs(ctrl->sin_port) + 1));
	char ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &ctrl->sin_addr, ip, sizeof(ip));
	snprintf(p->addr, sizeof(p->addr), "%s:%u", ip, ntohs(ctrl->sin_port));
}

static struct peer *peer_new(enum found f, const char *name, enum rtpm_role role)
{
	for (int i = 0; i < RN_MAX_PEERS; i++) {
		struct peer *p = &N.peers[i];
		if (p->used)
			continue;
		memset(p, 0, sizeof(*p));
		p->used = true;
		p->found = f;
		snprintf(p->name, sizeof(p->name), "%s", name);
		rtpm_init(&p->s, role, N.name, new_ssrc(), now_ns(), &peer_events, p);
		return p;
	}
	logf_("%s: no room for another network peer (%d)", name, RN_MAX_PEERS);
	return NULL;
}

static bool same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

#define MS 1000000ull

/* Is this address on one of this computer's own IPv4 networks (or itself)? */
static bool on_local_net(in_addr_t a)
{
	struct ifaddrs *ifs = NULL;
	bool ok = false;
	if (getifaddrs(&ifs) < 0)
		return false;
	for (struct ifaddrs *i = ifs; i && !ok; i = i->ifa_next) {
		if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP))
			continue;
		in_addr_t ia = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
		in_addr_t m = ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr;
		ok = (ia & m) == (a & m);
	}
	freeifaddrs(ifs);
	return ok;
}

static void on_packet(int port, const uint8_t *b, size_t n, const struct sockaddr_in *from)
{
	uint64_t now = now_ns();
	for (int i = 0; i < RN_MAX_PEERS; i++) {
		struct peer *p = &N.peers[i];
		if (p->used && same(from, port == RTPM_CONTROL ? &p->ctrl : &p->data)) {
			struct rtpm_cmd in;
			/* the device we invite invites us itself (RTP MASTER as well):
			 * answer it, unless our own session with it is open */
			if (port == RTPM_CONTROL && p->s.role == RTPM_INITIATOR && p->s.state != RTPM_OPEN &&
			    rtpm_parse_command(b, n, &in) && in.kind == RTPM_CMD_IN)
				p->s.role = RTPM_RESPONDER;
			rtpm_input(&p->s, (enum rtpm_port)port, b, n, now);
			return;
		}
	}
	struct rtpm_cmd c;
	if (!rtpm_parse_command(b, n, &c) || c.kind != RTPM_CMD_IN)
		return;
	if (port == RTPM_DATA) {
		/* the data-port invitation of a session invited on its control port */
		for (int i = 0; i < RN_MAX_PEERS; i++) {
			struct peer *p = &N.peers[i];
			if (p->used && p->s.role == RTPM_RESPONDER && p->ctrl.sin_addr.s_addr == from->sin_addr.s_addr &&
			    p->s.peer_ssrc == c.ssrc) {
				p->data = *from;
				rtpm_input(&p->s, RTPM_DATA, b, n, now);
				return;
			}
		}
		return;
	}
	/* a device invites us: only from a network this computer is on (a
	 * surface is on the LAN; nothing routed from further away), and only one
	 * whose name matches a profile */
	char ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &from->sin_addr, ip, sizeof(ip));
	if (!on_local_net(from->sin_addr.s_addr)) {
		if (now - N.last_refusal_log > 10000 * MS) {
			logf_("'%s' (%s) invited us from outside this computer's networks: ignored", c.name, ip);
			N.last_refusal_log = now;
		}
		return;
	}
	bool want = N.ev && N.ev->want && N.ev->want(N.data, c.name);
	if (!want) {
		uint8_t no[16 + RTPM_NAME_MAX];
		size_t k = rtpm_build_session(RTPM_CMD_NO, c.token, new_ssrc(), N.name, no, sizeof(no));
		sendto(N.fd[RTPM_CONTROL], no, k, 0, (const struct sockaddr *)from, sizeof(*from));
		if (now - N.last_refusal_log > 10000 * MS) {
			logf_("'%s' (%s) invited us: not a surface we know, refused", c.name, ip);
			N.last_refusal_log = now;
		}
		return;
	}
	/* the same device calling again from a new port (it restarted): the old
	 * session is over */
	for (int i = 0; i < RN_MAX_PEERS; i++) {
		struct peer *o = &N.peers[i];
		if (o->used && o->found == F_INVITED && o->ctrl.sin_addr.s_addr == from->sin_addr.s_addr &&
		    !strcmp(o->name, c.name)) {
			rtpm_bye(&o->s);
			o->used = false;
		}
	}
	struct peer *p = peer_new(F_INVITED, c.name, RTPM_RESPONDER);
	if (!p)
		return;
	set_addr(p, from);
	logf_("%s (%s) invites us", c.name, p->addr);
	rtpm_input(&p->s, RTPM_CONTROL, b, n, now);
}

static void on_sock(void *data, int fd, uint32_t mask)
{
	int port = fd == N.fd[RTPM_DATA] ? RTPM_DATA : RTPM_CONTROL;
	uint8_t b[2048];
	for (int k = 0; k < 64; k++) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(fd, b, sizeof(b), 0, (struct sockaddr *)&from, &fl);
		if (n <= 0)
			break;
		if (from.sin_family == AF_INET)
			on_packet(port, b, (size_t)n, &from);
	}
}

/* ================================================================ mDNS */
#ifdef BL_HAVE_AVAHI
struct AvahiWatch {
	struct spa_source *src;
	AvahiWatchCallback cb;
	void *userdata;
	int fd;
	AvahiWatchEvent last;
};
struct AvahiTimeout {
	struct spa_source *src;
	AvahiTimeoutCallback cb;
	void *userdata;
};

static void w_io(void *data, int fd, uint32_t mask)
{
	AvahiWatch *w = data;
	w->last = (AvahiWatchEvent)mask;
	w->cb(w, fd, (AvahiWatchEvent)mask, w->userdata);
}
static AvahiWatch *w_new(const AvahiPoll *api, int fd, AvahiWatchEvent ev, AvahiWatchCallback cb, void *userdata)
{
	AvahiWatch *w = calloc(1, sizeof(*w));
	w->cb = cb;
	w->userdata = userdata;
	w->fd = fd;
	w->src = pw_loop_add_io(N.l, fd, (uint32_t)ev, false, w_io, w);
	return w;
}
static void w_update(AvahiWatch *w, AvahiWatchEvent ev) { pw_loop_update_io(N.l, w->src, (uint32_t)ev); }
static AvahiWatchEvent w_events(AvahiWatch *w) { return w->last; }
static void w_free(AvahiWatch *w)
{
	pw_loop_destroy_source(N.l, w->src);
	free(w);
}
static void t_fire(void *data, uint64_t exp)
{
	AvahiTimeout *t = data;
	t->cb(t, t->userdata);
}
static void t_update(AvahiTimeout *t, const struct timeval *tv)
{
	if (!tv) {
		pw_loop_update_timer(N.l, t->src, NULL, NULL, false);
		return;
	}
	AvahiUsec us = -avahi_age(tv); /* microseconds from now */
	if (us < 1)
		us = 1;
	struct timespec v = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
	pw_loop_update_timer(N.l, t->src, &v, NULL, false);
}
static AvahiTimeout *t_new(const AvahiPoll *api, const struct timeval *tv, AvahiTimeoutCallback cb, void *userdata)
{
	AvahiTimeout *t = calloc(1, sizeof(*t));
	t->cb = cb;
	t->userdata = userdata;
	t->src = pw_loop_add_timer(N.l, t_fire, t);
	t_update(t, tv);
	return t;
}
static void t_free(AvahiTimeout *t)
{
	pw_loop_destroy_source(N.l, t->src);
	free(t);
}

static struct peer *peer_named(enum found f, const char *name)
{
	for (int i = 0; i < RN_MAX_PEERS; i++)
		if (N.peers[i].used && N.peers[i].found == f && !strcmp(N.peers[i].name, name))
			return &N.peers[i];
	return NULL;
}

static void on_resolve(AvahiServiceResolver *r, AvahiIfIndex iface, AvahiProtocol proto, AvahiResolverEvent ev,
		       const char *name, const char *type, const char *domain, const char *host, const AvahiAddress *a,
		       uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags flags, void *userdata)
{
	if (ev == AVAHI_RESOLVER_FOUND && a && a->proto == AVAHI_PROTO_INET && !(flags & AVAHI_LOOKUP_RESULT_OUR_OWN)) {
		struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
		sa.sin_addr.s_addr = a->data.ipv4.address;
		struct peer *p = peer_named(F_MDNS, name);
		if (!p)
			for (int i = 0; i < RN_MAX_PEERS && !p; i++)
				if (N.peers[i].used && same(&sa, &N.peers[i].ctrl))
					p = &N.peers[i]; /* added by address, or it called us: the same device */
		if (p && p->found != F_MDNS) {
			p->advertised = true;
			goto out;
		}
		bool fresh = !p;
		if (!p && !(p = peer_new(F_MDNS, name, RTPM_INITIATOR)))
			goto out;
		p->advertised = true;
		if (fresh || (!same(&sa, &p->ctrl) && p->s.state != RTPM_OPEN)) {
			set_addr(p, &sa);
			logf_("found %s at %s (mDNS)", name, p->addr);
			rtpm_start(&p->s, now_ns());
			changed();
		} else if (p->s.state == RTPM_LOST || p->s.state == RTPM_REFUSED) {
			rtpm_start(&p->s, now_ns()); /* announced again: ask now */
		}
	}
out:
	avahi_service_resolver_free(r);
}

static void on_browse(AvahiServiceBrowser *b, AvahiIfIndex iface, AvahiProtocol proto, AvahiBrowserEvent ev,
		      const char *name, const char *type, const char *domain, AvahiLookupResultFlags flags, void *userdata)
{
	if (ev == AVAHI_BROWSER_NEW) {
		if (flags & AVAHI_LOOKUP_RESULT_OUR_OWN)
			return;
		if (!N.ev || !N.ev->want || !N.ev->want(N.data, name))
			return;
		if (!avahi_service_resolver_new(N.client, iface, proto, name, type, domain, AVAHI_PROTO_INET, 0, on_resolve,
						NULL))
			logf_("mDNS: cannot resolve %s: %s", name, avahi_strerror(avahi_client_errno(N.client)));
	} else if (ev == AVAHI_BROWSER_REMOVE) {
		struct peer *p = peer_named(F_MDNS, name);
		if (p) {
			p->advertised = false;
			changed();
		}
	} else if (ev == AVAHI_BROWSER_FAILURE) {
		logf_("mDNS browsing failed: %s", avahi_strerror(avahi_client_errno(N.client)));
	}
}

static void on_client(AvahiClient *c, AvahiClientState st, void *userdata)
{
	N.client = c;
	if (st == AVAHI_CLIENT_S_RUNNING && !N.browser) {
		N.browser = avahi_service_browser_new(c, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, SERVICE, NULL, 0, on_browse, NULL);
		if (N.browser)
			logf_("looking for network surfaces (mDNS %s)", SERVICE);
	} else if (st == AVAHI_CLIENT_CONNECTING || st == AVAHI_CLIENT_FAILURE) {
		if (N.browser) {
			avahi_service_browser_free(N.browser);
			N.browser = NULL;
		}
		if (st == AVAHI_CLIENT_FAILURE) {
			logf_("avahi-daemon: %s; waiting for it", avahi_strerror(avahi_client_errno(c)));
			avahi_client_free(c);
			N.client = NULL;
			int err;
			avahi_client_new(&N.poll, AVAHI_CLIENT_NO_FAIL, on_client, NULL, &err);
		}
	}
}
#endif

/* ================================================================ API */
static int bind_udp(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int e = -errno;
		close(fd);
		return e;
	}
	return fd;
}

int rn_init(struct pw_loop *l, const char *name, const struct rn_events *ev, void *data)
{
	N.l = l;
	N.ev = ev;
	N.data = data;
	N.seed = (uint32_t)now_ns() ^ (uint32_t)getpid();
	snprintf(N.name, sizeof(N.name), "%s", name);
	/* the AppleMIDI default pair, or the next free one */
	for (uint16_t p = 5004; p < 5100 && N.fd[1] < 0; p += 2) {
		int a = bind_udp(p);
		if (a < 0)
			continue;
		int b = bind_udp((uint16_t)(p + 1));
		if (b < 0) {
			close(a);
			continue;
		}
		N.fd[0] = a;
		N.fd[1] = b;
		N.port = p;
	}
	if (N.fd[1] < 0)
		return -EADDRINUSE;
	for (int i = 0; i < 2; i++)
		N.src[i] = pw_loop_add_io(l, N.fd[i], SPA_IO_IN, false, on_sock, NULL);
#ifdef BL_HAVE_AVAHI
	N.poll = (AvahiPoll){ NULL, w_new, w_update, w_events, w_free, t_new, t_update, t_free };
	int err;
	if (!avahi_client_new(&N.poll, AVAHI_CLIENT_NO_FAIL, on_client, NULL, &err))
		logf_("mDNS: %s: network surfaces are not looked for; they can still call in or be added",
		      avahi_strerror(err));
#else
	logf_("built without avahi: network surfaces are not discovered");
#endif
	return 0;
}

void rn_tick(uint64_t now)
{
	for (int i = 0; i < RN_MAX_PEERS; i++) {
		struct peer *p = &N.peers[i];
		if (!p->used)
			continue;
		rtpm_tick(&p->s, now);
		bool ended = p->s.state == RTPM_LOST || p->s.state == RTPM_CLOSED;
		/* a device that invited us invites again after a drop: free its slot */
		if (p->found == F_INVITED && ended) {
			p->used = false;
			changed();
		} else if (p->found != F_INVITED && p->s.role == RTPM_RESPONDER && ended) {
			/* one we found that had called us: we invite it again */
			p->s.role = RTPM_INITIATOR;
			rtpm_start(&p->s, now);
		} else if (p->found == F_MDNS && !p->advertised && p->s.state != RTPM_OPEN) {
			/* withdrawn from mDNS and not connected: forget it (an announcement
			 * brings it back) */
			p->used = false;
			changed();
		}
	}
}

void rn_flush(uint64_t now)
{
	for (int i = 0; i < RN_MAX_PEERS; i++)
		if (N.peers[i].used)
			rtpm_flush(&N.peers[i].s, now);
}

bool rn_send(int peer, const uint8_t *msg, size_t n)
{
	if (peer < 0 || peer >= RN_MAX_PEERS || !N.peers[peer].used)
		return false;
	return rtpm_send(&N.peers[peer].s, msg, n, now_ns());
}

/* An IPv4 address only: never a blocking name lookup on the main loop. */
static int parse_addr(const char *host, uint16_t port, struct sockaddr_in *sa)
{
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	sa->sin_port = htons(port ? port : 5004);
	return inet_pton(AF_INET, host, &sa->sin_addr) == 1 ? 0 : -EINVAL;
}

int rn_add(const char *host, uint16_t port)
{
	struct sockaddr_in sa;
	if (parse_addr(host, port, &sa) < 0)
		return -EINVAL;
	for (int i = 0; i < RN_MAX_PEERS; i++)
		if (N.peers[i].used && same(&sa, &N.peers[i].ctrl))
			return i;
	struct peer *p = peer_new(F_ADDRESS, host, RTPM_INITIATOR);
	if (!p)
		return -ENOSPC;
	set_addr(p, &sa);
	rtpm_start(&p->s, now_ns());
	changed();
	return idx(p);
}

int rn_remove(const char *host, uint16_t port)
{
	struct sockaddr_in sa;
	if (parse_addr(host, port, &sa) < 0)
		return -EINVAL;
	for (int i = 0; i < RN_MAX_PEERS; i++) {
		struct peer *p = &N.peers[i];
		if (p->used && p->found == F_ADDRESS && same(&sa, &p->ctrl)) {
			rtpm_bye(&p->s);
			p->used = false;
			changed();
			return 0;
		}
	}
	return -ENOENT;
}

void rn_bye_all(void)
{
	for (int i = 0; i < RN_MAX_PEERS; i++)
		if (N.peers[i].used) {
			rtpm_flush(&N.peers[i].s, now_ns());
			rtpm_bye(&N.peers[i].s);
		}
}

void rn_close(void)
{
#ifdef BL_HAVE_AVAHI
	if (N.browser)
		avahi_service_browser_free(N.browser);
	if (N.client)
		avahi_client_free(N.client);
	N.browser = NULL;
	N.client = NULL;
#endif
	for (int i = 0; i < 2; i++) {
		if (N.src[i])
			pw_loop_destroy_source(N.l, N.src[i]);
		if (N.fd[i] >= 0)
			close(N.fd[i]);
		N.src[i] = NULL;
		N.fd[i] = -1;
	}
}

const char *rn_peer_name(int peer) { return peer >= 0 && peer < RN_MAX_PEERS ? N.peers[peer].name : ""; }
const char *rn_peer_address(int peer) { return peer >= 0 && peer < RN_MAX_PEERS ? N.peers[peer].addr : ""; }
bool rn_peer_open(int peer)
{
	return peer >= 0 && peer < RN_MAX_PEERS && N.peers[peer].used && N.peers[peer].s.state == RTPM_OPEN;
}
uint16_t rn_port(void) { return N.port; }
bool rn_discovery(void)
{
#ifdef BL_HAVE_AVAHI
	return N.browser != NULL;
#else
	return false;
#endif
}

char *rn_peers_json(void)
{
	json_object *a = json_object_new_array();
	for (int i = 0; i < RN_MAX_PEERS; i++) {
		struct peer *p = &N.peers[i];
		if (!p->used)
			continue;
		json_object *o = json_object_new_object();
		json_object_object_add(o, "peer", json_object_new_int(i));
		json_object_object_add(o, "name", json_object_new_string(p->name));
		json_object_object_add(o, "address", json_object_new_string(p->addr));
		json_object_object_add(o, "state", json_object_new_string(rtpm_state_name(p->s.state)));
		json_object_object_add(o, "found", json_object_new_string(found_names[p->found]));
		json_object_object_add(o, "advertised", json_object_new_boolean(p->found != F_MDNS || p->advertised));
		json_object_object_add(o, "latency-ms", json_object_new_double((double)p->s.latency_100us / 10.0));
		json_object_object_add(o, "lost", json_object_new_int((int)p->s.lost_count));
		json_object_array_add(a, o);
	}
	char *s = strdup(json_object_to_json_string_ext(a, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(a);
	return s;
}
