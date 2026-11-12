/* libbackline: Surface1 (docs/api/Surface1.md), the parts a user needs:
 * what is connected, network devices at an address, standby.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backline/backline.h"
#include "internal.h"

#define SRF_NAME "io.github.manuaudio.Backline.Surface"
#define SRF_PATH "/io/github/manuaudio/Backline/Surface"
#define SRF_IFACE "io.github.manuaudio.Backline.Surface1"

struct buf {
	char *s;
	size_t n, cap;
};

static void put(struct buf *b, const char *s, size_t n)
{
	if (b->n + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 1024;
		while (cap < b->n + n + 1)
			cap *= 2;
		char *t = realloc(b->s, cap);
		if (!t)
			return;
		b->s = t;
		b->cap = cap;
	}
	memcpy(b->s + b->n, s, n);
	b->n += n;
	b->s[b->n] = 0;
}
static void puts_(struct buf *b, const char *s) { put(b, s, strlen(s)); }

static void put_str(struct buf *b, const char *s)
{
	puts_(b, "\"");
	for (; *s; s++) {
		char e[8];
		if (*s == '"' || *s == '\\') {
			e[0] = '\\';
			e[1] = *s;
			put(b, e, 2);
		} else if ((unsigned char)*s < 0x20) {
			snprintf(e, sizeof(e), "\\u%04x", (unsigned char)*s);
			puts_(b, e);
		} else {
			put(b, s, 1);
		}
	}
	puts_(b, "\"");
}

/* One variant as a JSON value: s, b, i, u, q, t, d, as. */
static int put_variant(struct buf *b, sd_bus_message *m)
{
	char type;
	const char *contents;
	int r = sd_bus_message_peek_type(m, &type, &contents);
	if (r <= 0)
		return r < 0 ? r : -EINVAL;
	if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0)
		return r;
	char num[64];
	switch (contents[0]) {
	case 's': { const char *s; r = sd_bus_message_read(m, "s", &s); if (r >= 0) put_str(b, s); break; }
	case 'b': { int v; r = sd_bus_message_read(m, "b", &v); if (r >= 0) puts_(b, v ? "true" : "false"); break; }
	case 'i': { int32_t v; r = sd_bus_message_read(m, "i", &v); snprintf(num, sizeof(num), "%d", v); puts_(b, num); break; }
	case 'u': { uint32_t v; r = sd_bus_message_read(m, "u", &v); snprintf(num, sizeof(num), "%u", v); puts_(b, num); break; }
	case 'q': { uint16_t v; r = sd_bus_message_read(m, "q", &v); snprintf(num, sizeof(num), "%u", v); puts_(b, num); break; }
	case 't': { uint64_t v; r = sd_bus_message_read(m, "t", &v); snprintf(num, sizeof(num), "%llu", (unsigned long long)v); puts_(b, num); break; }
	case 'd': { double v; r = sd_bus_message_read(m, "d", &v); snprintf(num, sizeof(num), "%.6g", v); puts_(b, num); break; }
	case 'a':
		if (!strcmp(contents, "as")) {
			if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0)
				break;
			const char *s;
			bool first = true;
			puts_(b, "[");
			while ((r = sd_bus_message_read(m, "s", &s)) > 0) {
				if (!first)
					puts_(b, ",");
				put_str(b, s);
				first = false;
			}
			puts_(b, "]");
			if (r >= 0)
				r = sd_bus_message_exit_container(m);
			break;
		}
		/* fall through */
	default:
		puts_(b, "null");
		r = sd_bus_message_skip(m, contents);
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(m);
}

/* aa{sv} → a JSON array of objects */
static int call_list(bl_client *c, const char *method, char **json)
{
	sd_bus_message *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	struct buf b = { 0 };
	int r = sd_bus_call_method(bl__bus(c), SRF_NAME, SRF_PATH, SRF_IFACE, method, &e, &reply, "");
	if (r >= 0)
		r = sd_bus_message_enter_container(reply, 'a', "a{sv}");
	puts_(&b, "[");
	bool first_o = true;
	while (r >= 0 && (r = sd_bus_message_enter_container(reply, 'a', "{sv}")) > 0) {
		puts_(&b, first_o ? "{" : ",{");
		first_o = false;
		bool first = true;
		while (r >= 0 && (r = sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
			const char *k;
			if ((r = sd_bus_message_read(reply, "s", &k)) < 0)
				break;
			if (!first)
				puts_(&b, ",");
			first = false;
			put_str(&b, k);
			puts_(&b, ":");
			if ((r = put_variant(&b, reply)) < 0)
				break;
			r = sd_bus_message_exit_container(reply);
		}
		if (r >= 0)
			r = sd_bus_message_exit_container(reply);
		puts_(&b, "}");
	}
	puts_(&b, "]");
	sd_bus_message_unref(reply);
	if (r < 0) {
		free(b.s);
		r = bl__fail_bus(c, r, &e, method);
		sd_bus_error_free(&e);
		return r;
	}
	*json = b.s;
	return 0;
}

int bl_surface_units(bl_client *c, char **json) { return call_list(c, "Units", json); }
int bl_surface_peers(bl_client *c, char **json) { return call_list(c, "Peers", json); }

static int call_s(bl_client *c, const char *method, const char *arg)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bl__bus(c), SRF_NAME, SRF_PATH, SRF_IFACE, method, &e, NULL, "s", arg);
	if (r < 0)
		r = bl__fail_bus(c, r, &e, method);
	sd_bus_error_free(&e);
	return r < 0 ? r : 0;
}

int bl_surface_add_network(bl_client *c, const char *address) { return call_s(c, "AddNetwork", address); }
int bl_surface_remove_network(bl_client *c, const char *address) { return call_s(c, "RemoveNetwork", address); }

int bl_surface_set_active(bl_client *c, bool active)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bl__bus(c), SRF_NAME, SRF_PATH, SRF_IFACE, "SetActive", &e, NULL, "b", (int)active);
	if (r < 0)
		r = bl__fail_bus(c, r, &e, "SetActive");
	sd_bus_error_free(&e);
	return r < 0 ? r : 0;
}

int bl_surface_status(bl_client *c, bool *active, uint16_t *network_port, bool *params_connected)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int a = 1, pc = 0;
	uint16_t port = 0;
	int r = sd_bus_get_property_trivial(bl__bus(c), SRF_NAME, SRF_PATH, SRF_IFACE, "Active", &e, 'b', &a);
	if (r >= 0)
		r = sd_bus_get_property_trivial(bl__bus(c), SRF_NAME, SRF_PATH, SRF_IFACE, "NetworkPort", &e, 'q', &port);
	if (r >= 0)
		r = sd_bus_get_property_trivial(bl__bus(c), SRF_NAME, SRF_PATH, SRF_IFACE, "ParamsConnected", &e, 'b', &pc);
	if (r < 0) {
		r = bl__fail_bus(c, r, &e, "Surface1 properties");
		sd_bus_error_free(&e);
		return r;
	}
	if (active)
		*active = a;
	if (network_port)
		*network_port = port;
	if (params_connected)
		*params_connected = pc;
	return 0;
}
