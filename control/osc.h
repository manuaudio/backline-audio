/* Backline's OSC 1.0 codec for the control service: messages and bundles,
 * parsed in place, and a message builder.
 * SPDX-License-Identifier: MIT
 *
 * Parsing never reads past the packet and never allocates. A packet is
 * checked as a whole before any message in it is delivered, so a malformed
 * bundle never half-applies.
 */
#ifndef BACKLINE_OSC_H
#define BACKLINE_OSC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OSC_MAX_ARGS 16
#define OSC_MAX_DEPTH 4  /* nested bundles */

struct osc_arg {
	char type;            /* i f d h s (S is reported as s) T F N I b */
	union {
		int32_t i;
		float f;
		double d;
		int64_t h;
	} v;
	const char *s;        /* s: points into the packet, NUL-terminated */
	const uint8_t *blob;  /* b */
	uint32_t blob_size;
};

struct osc_msg {
	const char *address;  /* points into the packet */
	int argc;
	struct osc_arg argv[OSC_MAX_ARGS];
	uint64_t timetag;     /* of the enclosing bundle, 1 = immediately */
};

typedef void (*osc_msg_cb)(void *data, const struct osc_msg *m);

/* Checks the whole packet, then calls cb for each message in it (bundles
 * recursively, in order). Returns the number of messages, or -EINVAL if the
 * packet is malformed (then cb is never called). cb may be NULL (check
 * only). */
int osc_parse(const uint8_t *buf, size_t len, osc_msg_cb cb, void *data);

/* The numeric value of an argument (i f d h T F; I and N are 1 and 0).
 * false for strings and blobs. */
bool osc_arg_number(const struct osc_arg *a, double *out);

/* Builds one message. types: a string of i f d s T F (one va arg per i f d
 * s: int32_t as int, double for both f and d, const char *). Returns the
 * packet length, or 0 if it doesn't fit in cap. */
size_t osc_build(uint8_t *buf, size_t cap, const char *address, const char *types, ...);

/* A bundle (timetag "immediately"): begin writes its 16-byte header and
 * returns 16; add appends one message and returns the new length, or 0 if
 * it doesn't fit in cap (the bundle is unchanged). */
size_t osc_bundle_begin(uint8_t *buf, size_t cap);
size_t osc_bundle_add(uint8_t *buf, size_t len, size_t cap, const uint8_t *msg, size_t n);

#ifdef __cplusplus
}
#endif
#endif
