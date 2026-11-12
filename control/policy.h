/* The control service's network policy: which listeners may open, which
 * peers may talk to them, and how fast. Pure functions, unit-tested.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_CTL_POLICY_H
#define BACKLINE_CTL_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CTL_MAX_ALLOW 16
#define CTL_MIN_SECRET 16

struct ctl_cidr {
	int family;         /* AF_INET or AF_INET6 */
	uint8_t addr[16];
	int prefix;
};

struct ctl_listen_spec {
	bool tcp;
	char address[64];   /* as given: a literal IPv4 or IPv6 address */
	uint16_t port;
	struct ctl_cidr allow[CTL_MAX_ALLOW];
	int n_allow;
	char secret[256];
	bool insecure_udp;
	uint32_t max_rate;     /* messages per second per peer */
	uint32_t max_clients;  /* TCP */
	uint32_t max_size;     /* bytes per packet / frame */
	char params[32];       /* Params1 over OSC: the role of this listener's peers; "" = not served */
};

/* A literal IP address (no host names, no wildcard). Fills ss/len and
 * *loopback. Returns 0, or -1 with a sentence in err. */
int ctl_parse_address(const char *s, uint16_t port, struct sockaddr_storage *ss, socklen_t *len, bool *loopback,
		      char *err, size_t err_size);
/* "10.0.0.0/8", "192.168.1.20", "fd7a:115c::/48", "::1". 0 or -1. */
int ctl_parse_cidr(const char *s, struct ctl_cidr *out);
bool ctl_cidr_match(const struct ctl_cidr *c, const struct sockaddr *sa);
bool ctl_is_loopback(const struct sockaddr *sa);

/* The rules of docs/api/Control1.md "Security". 0, or -1 with a sentence. */
int ctl_check_listen(const struct ctl_listen_spec *s, char *err, size_t err_size);

/* May this peer talk to a listener with this spec? (Loopback listener with
 * no allow-list: loopback peers only; otherwise the allow-list.) */
bool ctl_peer_allowed(const struct ctl_listen_spec *s, bool listener_loopback, const struct sockaddr *peer);

/* Constant-time comparison of a presented secret with the configured one. */
bool ctl_secret_equal(const char *given, size_t given_len, const char *secret);

/* Token bucket, one per peer: rate tokens per second, a burst of rate. */
struct ctl_bucket {
	double tokens;
	uint64_t last_ns;
};
bool ctl_bucket_take(struct ctl_bucket *b, uint32_t rate, uint64_t now_ns);

#ifdef __cplusplus
}
#endif
#endif
