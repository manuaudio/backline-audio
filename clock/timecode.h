/* Backline timecode math: frame rates, labels, LTC bits, MTC messages.
 * SPDX-License-Identifier: MIT
 *
 * Pure functions, no I/O, safe on the realtime thread, unit-tested
 * (tests/unit/test_timecode.c). Timecode frame k starts at audio sample
 * ceil(k x rate / fps) of the transport, with fps = num/den exactly (30000/1001
 * for 29.97), so timecode is locked to transport frames with no drift.
 */
#ifndef BACKLINE_TIMECODE_H
#define BACKLINE_TIMECODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tc_rate {
	uint32_t num, den;  /* frames per second = num / den */
	uint32_t nominal;   /* 24, 25 or 30: frames per labelled second */
	bool drop;          /* 29.97 drop-frame labels */
};

/* 24, 25, 29.97 (drop-frame), 30. Returns false for anything else. */
bool tc_rate_from_fps(double fps, struct tc_rate *r);
double tc_rate_fps(const struct tc_rate *r);

struct tc_label { int h, m, s, f; };

/* Timecode frame count (from 00:00:00:00) <-> label, with drop-frame. */
void tc_frames_to_label(const struct tc_rate *r, int64_t frames, struct tc_label *l);
int64_t tc_label_to_frames(const struct tc_rate *r, const struct tc_label *l);
/* "HH:MM:SS:FF" (';' before FF for drop-frame); returns false if malformed. */
bool tc_parse(const struct tc_rate *r, const char *s, int64_t *frames);
void tc_format(const struct tc_rate *r, int64_t frames, char *out, int n);

/* First audio sample of timecode frame k, and the timecode frame that
 * contains audio sample x (x >= 0). */
int64_t tc_frame_start(const struct tc_rate *r, uint32_t rate, int64_t k);
int64_t tc_frame_at(const struct tc_rate *r, uint32_t rate, int64_t x);

/* ---- LTC (SMPTE 12M): 80 bits per frame, biphase mark ------------------- */
/* The frame's 80 bits, bit 0 first, sync word in bits 64..79, the parity
 * bit set so every frame has an even number of zeros (polarity-neutral). */
void ltc_bits(const struct tc_rate *r, int64_t frames, uint8_t bits[80]);
/* Parse 80 bits back to a frame count; false if the sync word is wrong. */
bool ltc_parse_bits(const struct tc_rate *r, const uint8_t bits[80], int64_t *frames);

/* A biphase-mark generator locked to transport samples. Feed it consecutive
 * transport samples; a jump (locate) restarts it at the new frame. */
struct ltc_gen {
	struct tc_rate r;
	uint32_t rate;
	int64_t next;        /* transport sample expected next (-1: none) */
	int64_t frame;       /* current timecode frame */
	int64_t frame_start, frame_len;
	uint8_t bits[80];
	int level;           /* +1 / -1 */
	int last_cell;       /* half-cell index last output, within the frame */
	float amp;
};
void ltc_gen_init(struct ltc_gen *g, const struct tc_rate *r, uint32_t rate, float amp);
/* The sample for transport sample x (rolling). */
float ltc_gen_sample(struct ltc_gen *g, int64_t x);

/* ---- MTC ---------------------------------------------------------------- */
/* Quarter frame q (from transport sample 0: q = 4 x timecode frame + quarter)
 * starts at sample ceil(q x rate / (4 x fps)). */
int64_t mtc_qf_start(const struct tc_rate *r, uint32_t rate, int64_t q);
/* The MIDI data byte of quarter frame q (status 0xF1 is sent before it).
 * Pieces 0..7 of q - (q mod 8) .. carry the frame of piece 0. */
uint8_t mtc_qf_byte(const struct tc_rate *r, int64_t q);
/* Full-frame SysEx: F0 7F 7F 01 01 hh mm ss ff F7 (10 bytes). */
void mtc_full_frame(const struct tc_rate *r, int64_t frames, uint8_t out[10]);
/* The MTC rate code of hh bits 5-6. */
int mtc_rate_code(const struct tc_rate *r);

/* MTC receiver. Feed each quarter-frame byte (and full frames) with the
 * transport-independent sample time it arrived at. */
struct mtc_rx {
	uint8_t pieces[8];
	uint8_t have;            /* bit mask of pieces received in order */
	int next_piece;
	int64_t piece0_time;
	int rate_code;           /* from piece 7 */
	bool valid;              /* a full sequence was decoded */
	int64_t frames;          /* the frame of the last sequence's piece 0 */
	int64_t frames_time;     /* the sample time of that piece 0 */
};
void mtc_rx_init(struct mtc_rx *m);
/* Returns true when piece 7 completed a sequence whose time differs from
 * the one predicted (the first one, or after a jump). Once a sequence is
 * known, every in-order piece gives *ref_q4 = the quarter frame (4 x frame
 * + piece) that started at time t; otherwise *ref_q4 = -1. */
bool mtc_rx_quarter(struct mtc_rx *m, uint8_t data, int64_t t, int64_t *ref_q4);
/* Full frame: returns true and the frame count if the SysEx is one. */
bool mtc_rx_full(const uint8_t *msg, int n, int *rate_code, int64_t *frames);
/* The rate a code means (for chase: the incoming fps). */
bool tc_rate_from_mtc_code(int code, struct tc_rate *r);

#ifdef __cplusplus
}
#endif
#endif
