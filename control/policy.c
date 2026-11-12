/* The control service's network policy (policy.h).
 * SPDX-License-Identifier: MIT
 */
#include "policy.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool v4_loopback(const uint8_t *a) { return a[0] == 127; }

static bool v6_loopback(const uint8_t *a)
{
	static const uint8_t lo[16] = { [15] = 1 };
	if (!memcmp(a, lo, 16))
		return true;
	/* ::ffff:127.x.x.x */
	static const uint8_t mapped[12] = { [10] = 0xff, [11] = 0xff };
	return !memcmp(a, mapped, 12) && a[12] == 127;
}

bool ctl_is_loopback(const struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET)
		return v4_loopback((const uint8_t *)&((const struct sockaddr_in *)sa)->sin_addr);
	if (sa->sa_family == AF_INET6)
		return v6_loopback(((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr);
	return false;
}

int ctl_parse_address(const char *s, uint16_t port, struct sockaddr_storage *ss, socklen_t *len, bool *loopback,
		      char *err, size_t err_size)
{
	memset(ss, 0, sizeof(*ss));
	struct sockaddr_in *v4 = (struct sockaddr_in *)ss;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ss;
	if (!s || !*s) {
		snprintf(err, err_size, "no address given: name the IP address to listen on");
		return -1;
	}
	if (inet_pton(AF_INET, s, &v4->sin_addr) == 1) {
		v4->sin_family = AF_INET;
		v4->sin_port = htons(port);
		*len = sizeof(*v4);
		if (v4->sin_addr.s_addr == htonl(INADDR_ANY)) {
			snprintf(err, err_size, "%s is the wildcard address; name the one interface address to listen on", s);
			return -1;
		}
		if (v4->sin_addr.s_addr == htonl(INADDR_BROADCAST) || (ntohl(v4->sin_addr.s_addr) >> 28) == 0xe) {
			snprintf(err, err_size, "%s is a broadcast or multicast address, not an interface address", s);
			return -1;
		}
		*loopback = v4_loopback((const uint8_t *)&v4->sin_addr);
		return 0;
	}
	if (inet_pton(AF_INET6, s, &v6->sin6_addr) == 1) {
		v6->sin6_family = AF_INET6;
		v6->sin6_port = htons(port);
		*len = sizeof(*v6);
		if (IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr)) {
			snprintf(err, err_size, "%s is the wildcard address; name the one interface address to listen on", s);
			return -1;
		}
		if (IN6_IS_ADDR_MULTICAST(&v6->sin6_addr)) {
			snprintf(err, err_size, "%s is a multicast address, not an interface address", s);
			return -1;
		}
		if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr) && !memcmp(v6->sin6_addr.s6_addr + 12, "\0\0\0\0", 4)) {
			snprintf(err, err_size, "%s is the wildcard address; name the one interface address to listen on", s);
			return -1;
		}
		*loopback = v6_loopback(v6->sin6_addr.s6_addr);
		return 0;
	}
	snprintf(err, err_size, "'%s' is not a literal IP address (host names are not resolved, so a DNS answer can't "
		 "move the listener)", s);
	return -1;
}

int ctl_parse_cidr(const char *s, struct ctl_cidr *out)
{
	char a[80];
	memset(out, 0, sizeof(*out));
	if (!s || strlen(s) >= sizeof(a))
		return -1;
	strcpy(a, s);
	char *slash = strchr(a, '/');
	int prefix = -1;
	if (slash) {
		*slash = 0;
		char *end;
		long p = strtol(slash + 1, &end, 10);
		if (!slash[1] || *end || p < 0 || p > 128)
			return -1;
		prefix = (int)p;
	}
	if (inet_pton(AF_INET, a, out->addr) == 1) {
		out->family = AF_INET;
		if (prefix > 32)
			return -1;
		out->prefix = prefix < 0 ? 32 : prefix;
	} else if (inet_pton(AF_INET6, a, out->addr) == 1) {
		out->family = AF_INET6;
		out->prefix = prefix < 0 ? 128 : prefix;
	} else {
		return -1;
	}
	return 0;
}

static bool prefix_match(const uint8_t *a, const uint8_t *b, int bits)
{
	int full = bits / 8, rest = bits % 8;
	if (memcmp(a, b, (size_t)full))
		return false;
	if (rest) {
		uint8_t mask = (uint8_t)(0xff << (8 - rest));
		if ((a[full] & mask) != (b[full] & mask))
			return false;
	}
	return true;
}

bool ctl_cidr_match(const struct ctl_cidr *c, const struct sockaddr *sa)
{
	uint8_t addr[16];
	int fam = sa->sa_family;
	if (fam == AF_INET) {
		memcpy(addr, &((const struct sockaddr_in *)sa)->sin_addr, 4);
	} else if (fam == AF_INET6) {
		const uint8_t *a6 = ((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr;
		static const uint8_t mapped[12] = { [10] = 0xff, [11] = 0xff };
		if (c->family == AF_INET && !memcmp(a6, mapped, 12)) {
			memcpy(addr, a6 + 12, 4);
			fam = AF_INET;
		} else {
			memcpy(addr, a6, 16);
		}
	} else {
		return false;
	}
	if (fam != c->family)
		return false;
	return prefix_match(addr, c->addr, c->prefix);
}

int ctl_check_listen(const struct ctl_listen_spec *s, char *err, size_t err_size)
{
	struct sockaddr_storage ss;
	socklen_t len;
	bool loopback;
	if (ctl_parse_address(s->address, s->port, &ss, &len, &loopback, err, err_size) < 0)
		return -1;
	if (s->max_size < 64 || s->max_size > 65536) {
		snprintf(err, err_size, "max-size must be 64 to 65536 bytes");
		return -1;
	}
	if (s->max_rate < 1 || s->max_rate > 100000) {
		snprintf(err, err_size, "max-rate must be 1 to 100000 messages a second");
		return -1;
	}
	if (s->tcp && (s->max_clients < 1 || s->max_clients > 64)) {
		snprintf(err, err_size, "max-clients must be 1 to 64");
		return -1;
	}
	if (s->secret[0] && strlen(s->secret) < CTL_MIN_SECRET) {
		snprintf(err, err_size, "the secret must be at least %d characters", CTL_MIN_SECRET);
		return -1;
	}
	if (s->secret[0] && !s->tcp) {
		snprintf(err, err_size, "a secret needs TCP: UDP has no connection to authenticate");
		return -1;
	}
	if (loopback)
		return 0;
	if (s->n_allow == 0) {
		snprintf(err, err_size, "%s is not a loopback address, so it needs an allow-list of the peers that may "
			 "send (option allow, e.g. 192.168.1.20/32)", s->address);
		return -1;
	}
	if (s->tcp && !s->secret[0]) {
		snprintf(err, err_size, "TCP on %s (not loopback) needs a shared secret (option secret, %d+ characters)",
			 s->address, CTL_MIN_SECRET);
		return -1;
	}
	if (!s->tcp && !s->insecure_udp) {
		snprintf(err, err_size, "UDP on %s (not loopback) can't be authenticated and its source address can be "
			 "forged; use TCP with a secret, or set insecure-udp to accept that", s->address);
		return -1;
	}
	return 0;
}

bool ctl_peer_allowed(const struct ctl_listen_spec *s, bool listener_loopback, const struct sockaddr *peer)
{
	if (s->n_allow == 0)
		return listener_loopback && ctl_is_loopback(peer);
	for (int i = 0; i < s->n_allow; i++)
		if (ctl_cidr_match(&s->allow[i], peer))
			return true;
	return false;
}

bool ctl_secret_equal(const char *given, size_t given_len, const char *secret)
{
	size_t n = strlen(secret);
	volatile uint8_t diff = (uint8_t)(given_len != n);
	for (size_t i = 0; i < n; i++)
		diff |= (uint8_t)((i < given_len ? (uint8_t)given[i] : 0) ^ (uint8_t)secret[i]);
	return n > 0 && diff == 0;
}

bool ctl_bucket_take(struct ctl_bucket *b, uint32_t rate, uint64_t now_ns)
{
	if (b->last_ns == 0) {
		b->tokens = rate;
		b->last_ns = now_ns;
	}
	double add = (double)(now_ns - b->last_ns) / 1e9 * rate;
	b->last_ns = now_ns;
	b->tokens = b->tokens + add > rate ? rate : b->tokens + add;
	if (b->tokens < 1)
		return false;
	b->tokens -= 1;
	return true;
}
