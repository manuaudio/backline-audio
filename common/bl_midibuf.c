/* MIDI ring and parser (bl_midibuf.h).
 * SPDX-License-Identifier: MIT
 */
#include "bl_midibuf.h"

#include <stdlib.h>
#include <string.h>

int bl_midi_ring_init(struct bl_midi_ring *q, uint32_t cap)
{
	memset(q, 0, sizeof(*q));
	if (cap < 64 || (cap & (cap - 1)))
		return -1;
	q->buf = calloc(1, cap);
	if (!q->buf)
		return -1;
	q->cap = cap;
	return 0;
}

void bl_midi_ring_free(struct bl_midi_ring *q)
{
	free(q->buf);
	q->buf = NULL;
}

static void put(struct bl_midi_ring *q, uint32_t at, const uint8_t *src, size_t n)
{
	uint32_t m = q->cap - 1;
	for (size_t i = 0; i < n; i++)
		q->buf[(at + i) & m] = src[i];
}

static void get(struct bl_midi_ring *q, uint32_t at, uint8_t *dst, size_t n)
{
	uint32_t m = q->cap - 1;
	for (size_t i = 0; i < n; i++)
		dst[i] = q->buf[(at + i) & m];
}

uint32_t bl_midi_ring_used(struct bl_midi_ring *q)
{
	return atomic_load_explicit(&q->w, memory_order_acquire) - atomic_load_explicit(&q->r, memory_order_acquire);
}

bool bl_midi_ring_push(struct bl_midi_ring *q, const uint8_t *msg, size_t n)
{
	if (!q->buf || n == 0 || n > 65535)
		return false;
	uint32_t w = atomic_load_explicit(&q->w, memory_order_relaxed);
	uint32_t r = atomic_load_explicit(&q->r, memory_order_acquire);
	if (q->cap - (w - r) < n + 2) {
		atomic_fetch_add_explicit(&q->dropped, 1, memory_order_relaxed);
		return false;
	}
	uint8_t len[2] = { (uint8_t)(n >> 8), (uint8_t)n };
	put(q, w, len, 2);
	put(q, w + 2, msg, n);
	atomic_store_explicit(&q->w, w + 2 + (uint32_t)n, memory_order_release);
	return true;
}

size_t bl_midi_ring_next(struct bl_midi_ring *q)
{
	if (!q->buf)
		return 0;
	uint32_t r = atomic_load_explicit(&q->r, memory_order_relaxed);
	uint32_t w = atomic_load_explicit(&q->w, memory_order_acquire);
	if (w == r)
		return 0;
	uint8_t len[2];
	get(q, r, len, 2);
	return (size_t)len[0] << 8 | len[1];
}

size_t bl_midi_ring_pop(struct bl_midi_ring *q, uint8_t *out, size_t cap)
{
	size_t n = bl_midi_ring_next(q);
	if (!n)
		return 0;
	uint32_t r = atomic_load_explicit(&q->r, memory_order_relaxed);
	if (n <= cap)
		get(q, r + 2, out, n);
	atomic_store_explicit(&q->r, r + 2 + (uint32_t)n, memory_order_release);
	return n <= cap ? n : 0;
}

int bl_midi_data_len(uint8_t s)
{
	switch (s & 0xf0) {
	case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0: return 2;
	case 0xc0: case 0xd0: return 1;
	default: break;
	}
	switch (s) {
	case 0xf1: case 0xf3: return 1;
	case 0xf2: return 2;
	case 0xf6: case 0xf8: case 0xfa: case 0xfb: case 0xfc: case 0xfe: case 0xff: return 0;
	default: return -1;
	}
}

void bl_midi_parser_init(struct bl_midi_parser *p)
{
	memset(p, 0, sizeof(*p));
}

void bl_midi_parse(struct bl_midi_parser *p, const uint8_t *b, size_t n, bl_midi_cb cb, void *data)
{
	for (size_t i = 0; i < n; i++) {
		uint8_t c = b[i];
		if (c >= 0xf8) { /* realtime: anywhere, even inside SysEx */
			cb(data, &c, 1);
			continue;
		}
		if (p->sysex) {
			if (c == 0xf7 || c & 0x80) {
				if (c == 0xf7 && !p->overflow && p->n < sizeof(p->buf)) {
					p->buf[p->n++] = 0xf7;
					cb(data, p->buf, p->n);
				} else {
					p->sysex_dropped++;
				}
				p->sysex = false;
				p->n = 0;
				if (c == 0xf7)
					continue;
				/* another status byte ends the SysEx without F7: drop it and go on */
			} else {
				if (p->n < sizeof(p->buf) - 1)
					p->buf[p->n++] = c;
				else
					p->overflow = true;
				continue;
			}
		}
		if (c == 0xf0) {
			p->sysex = true;
			p->overflow = false;
			p->buf[0] = 0xf0;
			p->n = 1;
			p->running = 0;
			continue;
		}
		if (c & 0x80) {
			int dl = bl_midi_data_len(c);
			if (dl < 0) { /* F4, F5, stray F7 */
				p->running = 0;
				p->n = 0;
				continue;
			}
			p->buf[0] = c;
			p->n = 1;
			p->need = (size_t)dl;
			p->running = c < 0xf0 ? c : 0;
			if (dl == 0) {
				cb(data, p->buf, 1);
				p->n = 0;
			}
			continue;
		}
		/* a data byte */
		if (p->n == 0) {
			if (!p->running)
				continue;
			p->buf[0] = p->running;
			p->n = 1;
			p->need = (size_t)bl_midi_data_len(p->running);
		}
		p->buf[p->n++] = c;
		if (p->n == p->need + 1) {
			cb(data, p->buf, p->n);
			p->n = 0;
		}
	}
}
