/* Backline OSC 1.0 codec (osc.h).
 * SPDX-License-Identifier: MIT
 */
#include "osc.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static size_t pad4(size_t n)
{
	return (n + 3) & ~(size_t)3;
}

/* A NUL-terminated, 4-byte padded string at *pos. */
static const char *read_str(const uint8_t *buf, size_t len, size_t *pos)
{
	if (*pos >= len)
		return NULL;
	const uint8_t *end = memchr(buf + *pos, 0, len - *pos);
	if (!end)
		return NULL;
	const char *s = (const char *)buf + *pos;
	size_t next = *pos + pad4((size_t)(end - (buf + *pos)) + 1);
	if (next > len)
		return NULL;
	*pos = next;
	return s;
}

static int parse_message(const uint8_t *buf, size_t len, uint64_t timetag, osc_msg_cb cb, void *data)
{
	struct osc_msg m;
	size_t pos = 0;
	memset(&m, 0, sizeof(m));
	m.timetag = timetag;
	m.address = read_str(buf, len, &pos);
	if (!m.address || m.address[0] != '/')
		return -EINVAL;
	if (pos == len) {  /* no type tag string: OSC 1.0 allows it, no arguments */
		if (cb)
			cb(data, &m);
		return 1;
	}
	const char *tags = read_str(buf, len, &pos);
	if (!tags || tags[0] != ',')
		return -EINVAL;
	for (const char *t = tags + 1; *t; t++) {
		if (m.argc >= OSC_MAX_ARGS)
			return -EINVAL;
		struct osc_arg *a = &m.argv[m.argc];
		a->type = *t;
		switch (*t) {
		case 'i':
		case 'f':
			if (pos + 4 > len)
				return -EINVAL;
			if (*t == 'i') {
				a->v.i = (int32_t)be32(buf + pos);
			} else {
				uint32_t u = be32(buf + pos);
				memcpy(&a->v.f, &u, 4);
			}
			pos += 4;
			break;
		case 'd':
		case 'h':
		case 't':
			if (pos + 8 > len)
				return -EINVAL;
			if (*t == 'd') {
				uint64_t u = be64(buf + pos);
				memcpy(&a->v.d, &u, 8);
			} else {
				a->v.h = (int64_t)be64(buf + pos);
				a->type = 'h';
			}
			pos += 8;
			break;
		case 's':
		case 'S':
			a->s = read_str(buf, len, &pos);
			if (!a->s)
				return -EINVAL;
			a->type = 's';
			break;
		case 'b': {
			if (pos + 4 > len)
				return -EINVAL;
			uint32_t n = be32(buf + pos);
			pos += 4;
			if (n > len - pos || pad4(n) > len - pos)
				return -EINVAL;
			a->blob = buf + pos;
			a->blob_size = n;
			pos += pad4(n);
			break;
		}
		case 'T':
		case 'F':
		case 'N':
		case 'I':
			break;
		default:
			return -EINVAL;
		}
		m.argc++;
	}
	if (pos != len)
		return -EINVAL;
	if (cb)
		cb(data, &m);
	return 1;
}

static int parse_packet(const uint8_t *buf, size_t len, uint64_t timetag, int depth, osc_msg_cb cb, void *data)
{
	if (len < 4 || len % 4)
		return -EINVAL;
	if (buf[0] != '#')
		return parse_message(buf, len, timetag, cb, data);
	if (len < 16 || memcmp(buf, "#bundle", 8) || depth >= OSC_MAX_DEPTH)
		return -EINVAL;
	uint64_t tt = be64(buf + 8);
	size_t pos = 16;
	int count = 0;
	while (pos < len) {
		if (pos + 4 > len)
			return -EINVAL;
		uint32_t n = be32(buf + pos);
		pos += 4;
		if (n == 0 || n > len - pos)
			return -EINVAL;
		int r = parse_packet(buf + pos, n, tt, depth + 1, cb, data);
		if (r < 0)
			return r;
		count += r;
		pos += n;
	}
	return count;
}

int osc_parse(const uint8_t *buf, size_t len, osc_msg_cb cb, void *data)
{
	if (!buf)
		return -EINVAL;
	int r = parse_packet(buf, len, 1, 0, NULL, NULL);
	if (r < 0 || !cb)
		return r;
	return parse_packet(buf, len, 1, 0, cb, data);
}

bool osc_arg_number(const struct osc_arg *a, double *out)
{
	switch (a->type) {
	case 'i': *out = a->v.i; return true;
	case 'f': *out = a->v.f; return true;
	case 'd': *out = a->v.d; return true;
	case 'h': *out = (double)a->v.h; return true;
	case 'T':
	case 'I': *out = 1; return true;
	case 'F':
	case 'N': *out = 0; return true;
	default: return false;
	}
}

static bool put(uint8_t *buf, size_t cap, size_t *pos, const void *src, size_t n)
{
	if (n > cap - *pos)
		return false;
	memcpy(buf + *pos, src, n);
	*pos += n;
	return true;
}

static bool put_str(uint8_t *buf, size_t cap, size_t *pos, const char *s)
{
	size_t n = strlen(s) + 1, p = pad4(n);
	if (p > cap - *pos)
		return false;
	memcpy(buf + *pos, s, n);
	memset(buf + *pos + n, 0, p - n);
	*pos += p;
	return true;
}

static bool put32(uint8_t *buf, size_t cap, size_t *pos, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
	return put(buf, cap, pos, b, 4);
}

size_t osc_build(uint8_t *buf, size_t cap, const char *address, const char *types, ...)
{
	char tags[OSC_MAX_ARGS + 2] = ",";
	size_t nt = strlen(types);
	if (nt > OSC_MAX_ARGS)
		return 0;
	memcpy(tags + 1, types, nt + 1);
	size_t pos = 0;
	if (!put_str(buf, cap, &pos, address) || !put_str(buf, cap, &pos, tags))
		return 0;
	va_list ap;
	va_start(ap, types);
	bool ok = true;
	for (const char *t = types; *t && ok; t++) {
		switch (*t) {
		case 'i': ok = put32(buf, cap, &pos, (uint32_t)va_arg(ap, int)); break;
		case 'f': {
			float f = (float)va_arg(ap, double);
			uint32_t u;
			memcpy(&u, &f, 4);
			ok = put32(buf, cap, &pos, u);
			break;
		}
		case 'd': {
			double d = va_arg(ap, double);
			uint64_t u;
			memcpy(&u, &d, 8);
			ok = put32(buf, cap, &pos, (uint32_t)(u >> 32)) && put32(buf, cap, &pos, (uint32_t)u);
			break;
		}
		case 's': ok = put_str(buf, cap, &pos, va_arg(ap, const char *)); break;
		case 'T':
		case 'F': break;
		default: ok = false;
		}
	}
	va_end(ap);
	return ok ? pos : 0;
}

size_t osc_bundle_begin(uint8_t *buf, size_t cap)
{
	if (cap < 16)
		return 0;
	memcpy(buf, "#bundle\0\0\0\0\0\0\0\0\1", 16); /* timetag 1: immediately */
	return 16;
}

size_t osc_bundle_add(uint8_t *buf, size_t len, size_t cap, const uint8_t *msg, size_t n)
{
	if (len < 16 || n == 0 || (n & 3) || len + 4 + n > cap)
		return 0;
	buf[len] = (uint8_t)(n >> 24);
	buf[len + 1] = (uint8_t)(n >> 16);
	buf[len + 2] = (uint8_t)(n >> 8);
	buf[len + 3] = (uint8_t)n;
	memcpy(buf + len + 4, msg, n);
	return len + 4 + n;
}
