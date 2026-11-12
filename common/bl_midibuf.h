/* Backline: MIDI 1.0 plumbing shared by controld, surfaced and the test
 * tools. A wait-free SPSC ring of whole messages (main thread to a process
 * callback, or back), and a byte-stream parser (running status, SysEx split
 * across packets, realtime bytes inside SysEx).
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_MIDIBUF_H
#define BACKLINE_MIDIBUF_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BL_MIDI_SYSEX_MAX 1024

/* Messages up to 65535 bytes, each stored as a 2-byte length and its bytes.
 * One writer thread, one reader thread. */
struct bl_midi_ring {
	uint8_t *buf;
	uint32_t cap;          /* power of two */
	atomic_uint w, r;
	atomic_uint_fast64_t dropped;
};

int bl_midi_ring_init(struct bl_midi_ring *q, uint32_t cap_pow2);
void bl_midi_ring_free(struct bl_midi_ring *q);
/* All or nothing; false (and counted in dropped) when there is no room. */
bool bl_midi_ring_push(struct bl_midi_ring *q, const uint8_t *msg, size_t n);
/* The next message's length, 0 when empty. */
size_t bl_midi_ring_next(struct bl_midi_ring *q);
/* Pops the next message into out (cap >= its length, or it is skipped and 0
 * returned). Returns its length, 0 when empty. */
size_t bl_midi_ring_pop(struct bl_midi_ring *q, uint8_t *out, size_t cap);
uint32_t bl_midi_ring_used(struct bl_midi_ring *q);

typedef void (*bl_midi_cb)(void *data, const uint8_t *msg, size_t n);

struct bl_midi_parser {
	uint8_t buf[BL_MIDI_SYSEX_MAX];
	size_t n;
	uint8_t running;      /* running status, 0 = none */
	size_t need;          /* data bytes of the current channel/common message */
	bool sysex;
	bool overflow;
	uint64_t sysex_dropped;
};

void bl_midi_parser_init(struct bl_midi_parser *p);
/* Feeds bytes; cb gets each complete message (SysEx with F0 and F7). */
void bl_midi_parse(struct bl_midi_parser *p, const uint8_t *bytes, size_t n, bl_midi_cb cb, void *data);
/* Data bytes after a status byte (-1 for SysEx/unknown). */
int bl_midi_data_len(uint8_t status);

#ifdef __cplusplus
}
#endif
#endif
