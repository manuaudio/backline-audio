/* Backline: MIDI in and out of a PipeWire control port, for process
 * callbacks. Header-only (it needs the SPA headers and nothing else).
 * Realtime-safe: no allocation, no locks, no syscalls.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_MIDI_PW_H
#define BACKLINE_MIDI_PW_H

#include <spa/buffer/buffer.h>
#include <spa/control/control.h>
#include <spa/control/ump-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>

#include "bl_midibuf.h"

/* Calls cb with the MIDI 1.0 bytes of every control in the buffer, in order
 * (SPA_CONTROL_Midi as is; UMP converted, SysEx possibly in pieces: feed a
 * bl_midi_parser). */
static inline void bl_midi_pw_read(struct spa_data *d, bl_midi_cb cb, void *data)
{
	if (!d->data || !d->chunk || d->chunk->size == 0)
		return;
#ifdef BL_HAVE_SPA_PARSER_BODY
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	struct spa_pod_sequence seq;
	const void *seq_body, *c_body;
	struct spa_pod_control c;
	spa_pod_parser_init_from_data(&parser, d->data, d->maxsize, d->chunk->offset, d->chunk->size);
	if (spa_pod_parser_push_sequence_body(&parser, &frame, &seq, &seq_body) < 0)
		return;
	uint64_t state = 0;
	while (spa_pod_parser_get_control_body(&parser, &c, &c_body) >= 0) {
		if (c.type == SPA_CONTROL_Midi) {
			cb(data, (const uint8_t *)c_body, c.value.size);
		} else if (c.type == SPA_CONTROL_UMP) {
			const uint32_t *u = (const uint32_t *)c_body;
			size_t size = c.value.size;
			while (size > 0) {
				uint8_t midi[16];
				int sz = spa_ump_to_midi(&u, &size, midi, sizeof(midi), &state);
				if (sz < 0)
					break;
				if (sz > 0)
					cb(data, midi, (size_t)sz);
			}
		}
	}
#else
	struct spa_pod *pod = (struct spa_pod *)spa_pod_from_data(d->data, d->maxsize, d->chunk->offset, d->chunk->size);
	if (!pod || !spa_pod_is_sequence(pod))
		return;
	struct spa_pod_control *c;
	SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence *)pod, c) {
		if (c->type == SPA_CONTROL_Midi) {
			cb(data, (const uint8_t *)SPA_POD_BODY(&c->value), SPA_POD_BODY_SIZE(&c->value));
		} else if (c->type == SPA_CONTROL_UMP) {
			uint8_t midi[16];
			int sz = spa_ump_to_midi((uint32_t *)SPA_POD_BODY(&c->value), SPA_POD_BODY_SIZE(&c->value), midi, sizeof(midi));
			if (sz > 0)
				cb(data, midi, (size_t)sz);
		}
	}
#endif
}

/* Writes the ring's messages that fit into the buffer as one control
 * sequence (an empty sequence when there are none). Returns the bytes of
 * MIDI written. */
static inline size_t bl_midi_pw_write(struct spa_data *d, struct bl_midi_ring *q)
{
	if (!d->data || !d->chunk)
		return 0;
	struct spa_pod_builder b;
	struct spa_pod_frame f;
	spa_pod_builder_init(&b, d->data, d->maxsize);
	spa_pod_builder_push_sequence(&b, &f, 0);
	uint8_t tmp[BL_MIDI_SYSEX_MAX];
	size_t n, total = 0;
	while ((n = bl_midi_ring_next(q)) > 0) {
		if (b.state.offset + n + 64 > d->maxsize)
			break;
		n = bl_midi_ring_pop(q, tmp, sizeof(tmp));
		if (!n)
			continue; /* larger than any MIDI message we send: dropped */
		spa_pod_builder_control(&b, 0, SPA_CONTROL_Midi);
		spa_pod_builder_bytes(&b, tmp, (uint32_t)n);
		total += n;
	}
	spa_pod_builder_pop(&b, &f);
	d->chunk->offset = 0;
	d->chunk->size = b.state.offset;
	d->chunk->stride = 1;
	return total;
}

#endif
