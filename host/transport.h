// The transport of one cycle, read from PipeWire's spa_io_position: what the
// worker hands every plugin format. Header-only and realtime-safe (no
// allocation, no locks), so the unit test can run it without a daemon.
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <cstdint>

#include <spa/node/io.h>

#include "plugin.h"

namespace bl {

// The transport frame at the cycle's first sample, as pipewire-jack computes
// it from segment 0 (the segment owner's mapping of running time to
// position).
inline int64_t segment_frame(const spa_io_position *p)
{
    const spa_io_segment *seg = &p->segments[0];
    if ((int64_t)p->clock.position < p->offset)
        return (int64_t)seg->position;
    uint64_t running = p->clock.position - p->offset;
    if (running >= seg->start && (seg->duration == 0 || running < seg->start + seg->duration))
        return (int64_t)((double)(running - seg->start) * seg->rate + (double)seg->position);
    return (int64_t)seg->position;
}

// Fill `t` for this cycle. `prev` is last cycle's result and `prev_frames`
// its length, for `changed`.
inline void transport_from_position(const spa_io_position *p, const Transport &prev, uint32_t prev_frames, Transport &t)
{
    const spa_io_segment *seg = &p->segments[0];
    const spa_io_segment_bar *bar = &seg->bar;
    t.valid = !(seg->flags & SPA_IO_SEGMENT_FLAG_NO_POSITION);
    t.rolling = t.valid && p->state == SPA_IO_POSITION_STATE_RUNNING;
    t.rate = p->clock.rate.denom ? p->clock.rate.denom : 48000;
    t.nsec = p->clock.nsec;
    t.frame = t.valid ? segment_frame(p) : 0;
    t.bar_valid = t.valid && (bar->flags & SPA_IO_SEGMENT_BAR_FLAG_VALID) && bar->bpm > 0 &&
                  bar->signature_num > 0 && bar->signature_denom > 0;
    if (t.bar_valid) {
        t.bpm = bar->bpm;
        t.sig_num = bar->signature_num;
        t.sig_den = bar->signature_denom;
        // bar.beat is the beat at `offset` frames before the cycle (a JACK
        // timebase master's bbt_offset; clockd sets it so the beat is exact
        // to the frame, not only to the tick). pipewire-jack writes it as
        // bar x beats_per_bar + (beat - 1) + tick with JACK's 1-based bar
        // and reads it back as bar = beat / beats_per_bar, so the segment's
        // beat is one bar ahead of the song's: song beat 0 (bar 1, beat 1)
        // is bar.beat = beats_per_bar. This is its exact inverse.
        t.beat = bar->beat - t.sig_num + (double)bar->offset * t.bpm / 60.0 / t.rate;
        if (t.beat < 0)
            t.beat = 0;
        double b = std::floor(t.beat / t.sig_num + 1e-9);
        t.bar = (int32_t)b;
        t.bar_start = b * t.sig_num;
    } else {
        t.bpm = 120;
        t.sig_num = t.sig_den = 4;
        t.beat = t.bar_start = 0;
        t.bar = 0;
    }
    int64_t expect = prev.frame + (prev.rolling ? (int64_t)prev_frames : 0);
    t.changed = !prev.valid || t.valid != prev.valid || t.rolling != prev.rolling || t.bar_valid != prev.bar_valid ||
                t.bpm != prev.bpm || t.sig_num != prev.sig_num || t.sig_den != prev.sig_den || t.frame != expect;
    // The offset is whole frames, floored, so each cycle's beat is at or up
    // to one frame below the true one. While the transport just continues,
    // the previous cycle's beat carried forward is exact; keep the larger of
    // the two when they are within that frame (a bigger difference, such as
    // a Link phase correction, is taken as it comes).
    if (t.bar_valid && !t.changed && prev.rolling) {
        double per_frame = t.bpm / 60.0 / t.rate;
        double carried = prev.beat + (double)prev_frames * per_frame;
        if (carried > t.beat && carried - t.beat < per_frame * 1.000001) {
            t.beat = carried;
            double b = std::floor(t.beat / t.sig_num + 1e-9);
            t.bar = (int32_t)b;
            t.bar_start = b * t.sig_num;
        }
    }
}

} // namespace bl

#ifdef CLAP_VERSION_MAJOR
namespace bl {

// CLAP's transport: beats are quarter notes, as in the hosts CLAP plugins
// are written against (and VST3's projectTimeMusic), fixed point 1 << 31.
inline void clap_transport_from(const Transport &t, clap_event_transport &e)
{
    e = {};
    e.header.size = sizeof(e);
    e.header.time = 0;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_TRANSPORT;
    e.header.flags = 0;
    e.flags = CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
    if (t.rolling)
        e.flags |= CLAP_TRANSPORT_IS_PLAYING;
    e.song_pos_seconds = (clap_sectime)std::llround((double)t.frame / t.rate * (double)CLAP_SECTIME_FACTOR);
    if (t.bar_valid) {
        e.flags |= CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        e.tempo = t.qn_tempo();
        e.tempo_inc = 0;
        e.song_pos_beats = (clap_beattime)std::llround(t.qn() * (double)CLAP_BEATTIME_FACTOR);
        e.bar_start = (clap_beattime)std::llround(t.qn_bar_start() * (double)CLAP_BEATTIME_FACTOR);
        e.bar_number = t.bar;
        e.tsig_num = (uint16_t)std::lround(t.sig_num);
        e.tsig_denom = (uint16_t)std::lround(t.sig_den);
    }
}

} // namespace bl
#endif
