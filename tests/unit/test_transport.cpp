// Unit test: the transport the worker derives from spa_io_position, and its
// CLAP form. SPDX-License-Identifier: MIT
#include <clap/clap.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "transport.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

static spa_io_position make(uint64_t clock_pos, uint32_t state, uint64_t seg_start, uint64_t seg_pos)
{
    spa_io_position p;
    std::memset(&p, 0, sizeof(p));
    p.clock.position = clock_pos;
    p.clock.duration = 256;
    p.clock.rate = SPA_FRACTION(1, 48000);
    p.clock.nsec = 123456789;
    p.state = state;
    p.n_segments = 1;
    p.segments[0].start = seg_start;
    p.segments[0].rate = 1.0;
    p.segments[0].position = seg_pos;
    return p;
}

int main()
{
    // rolling: frame = running - start + position
    spa_io_position p = make(10000, SPA_IO_POSITION_STATE_RUNNING, 8000, 96000);
    bl::Transport prev, t;
    bl::transport_from_position(&p, prev, 0, t);
    CHECK(t.valid && t.rolling && !t.bar_valid);
    CHECK(t.frame == 98000);
    CHECK(t.changed);            // first cycle
    CHECK(t.nsec == 123456789);

    // the bar: beat = bar.beat + offset x bpm / 60 / rate (clockd's bbt_offset)
    auto &bar = p.segments[0].bar;
    bar.flags = SPA_IO_SEGMENT_BAR_FLAG_VALID;
    bar.bpm = 120;
    bar.signature_num = 7;
    bar.signature_denom = 8;
    bar.beat = 15.0 + 3.0 / 1920;   // tick-floored
    bar.offset = 6;                 // frames past that tick
    bl::transport_from_position(&p, prev, 0, t);
    CHECK(t.bar_valid);
    // pipewire-jack's bar.beat is one bar ahead (JACK's 1-based bar x bpb)
    double beat = 15.0 + 3.0 / 1920 + 6.0 * 120 / 60 / 48000 - 7;
    CHECK(near(t.beat, beat));
    CHECK(t.bar == 1 && near(t.bar_start, 7));   // JACK bar 2, beat 2
    CHECK(near(t.qn(), beat * 4 / 8) && near(t.qn_tempo(), 60) && near(t.qn_bar_start(), 3.5));
    // bar 1, beat 1, tick 0: song beat 0
    bar.beat = 7;
    bar.offset = 0;
    bl::transport_from_position(&p, prev, 0, t);
    CHECK(near(t.beat, 0) && t.bar == 0 && near(t.bar_start, 0));
    bar.beat = 15.0 + 3.0 / 1920;
    bar.offset = 6;
    bl::transport_from_position(&p, prev, 0, t);

    // continuation: not changed; a jump, a stop or a tempo change is
    prev = t;
    spa_io_position q = make(10256, SPA_IO_POSITION_STATE_RUNNING, 8000, 96000);
    q.segments[0].bar = bar;
    bl::Transport t2;
    bl::transport_from_position(&q, prev, 256, t2);
    CHECK(t2.frame == 98256 && !t2.changed);
    q.segments[0].position = 0;                         // a locate
    bl::transport_from_position(&q, prev, 256, t2);
    CHECK(t2.changed);
    q = make(10256, SPA_IO_POSITION_STATE_STOPPED, 8000, 96000);
    q.segments[0].bar = bar;
    bl::transport_from_position(&q, prev, 256, t2);
    CHECK(!t2.rolling && t2.changed);
    q = make(10256, SPA_IO_POSITION_STATE_RUNNING, 8000, 96000);
    q.segments[0].bar = bar;
    q.segments[0].bar.bpm = 90;
    bl::transport_from_position(&q, prev, 256, t2);
    CHECK(t2.changed);

    // stopped: the owner gives the segment rate 0, so the frame holds still;
    // a stopped continuation isn't a change
    spa_io_position s1 = make(20000, SPA_IO_POSITION_STATE_STOPPED, 8000, 4800);
    s1.segments[0].rate = 0;
    bl::Transport a, b;
    bl::transport_from_position(&s1, bl::Transport{}, 0, a);
    spa_io_position s2 = make(20256, SPA_IO_POSITION_STATE_STOPPED, 8000, 4800);
    s2.segments[0].rate = 0;
    bl::transport_from_position(&s2, a, 256, b);
    CHECK(!b.rolling && b.frame == a.frame && !b.changed);

    // no position: not valid
    p.segments[0].flags = SPA_IO_SEGMENT_FLAG_NO_POSITION;
    bl::transport_from_position(&p, prev, 0, t);
    CHECK(!t.valid && !t.rolling);

    // CLAP: quarter notes, fixed point 2^31, flags
    clap_event_transport e;
    bl::Transport c;
    c.valid = c.rolling = c.bar_valid = true;
    c.frame = 72000;
    c.rate = 48000;
    c.bpm = 90;
    c.sig_num = 3;
    c.sig_den = 4;
    c.beat = 7.25;
    c.bar = 2;
    c.bar_start = 6;
    bl::clap_transport_from(c, e);
    CHECK(e.header.type == CLAP_EVENT_TRANSPORT && e.header.size == sizeof(e));
    CHECK((e.flags & CLAP_TRANSPORT_IS_PLAYING) && (e.flags & CLAP_TRANSPORT_HAS_TEMPO) &&
          (e.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && (e.flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) &&
          (e.flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE));
    CHECK(e.song_pos_beats == (clap_beattime)(7.25 * CLAP_BEATTIME_FACTOR));
    CHECK(e.song_pos_seconds == (clap_sectime)(1.5 * CLAP_SECTIME_FACTOR));
    CHECK(e.bar_start == 6 * CLAP_BEATTIME_FACTOR && e.bar_number == 2 && e.tsig_num == 3 && e.tsig_denom == 4);
    CHECK(near(e.tempo, 90));
    c.sig_den = 8;                  // 6/8: beats are eighths; CLAP gets quarters
    bl::clap_transport_from(c, e);
    CHECK(e.song_pos_beats == (clap_beattime)(3.625 * CLAP_BEATTIME_FACTOR) && near(e.tempo, 45));
    c.rolling = false;
    c.bar_valid = false;
    bl::clap_transport_from(c, e);
    CHECK(!(e.flags & CLAP_TRANSPORT_IS_PLAYING) && !(e.flags & CLAP_TRANSPORT_HAS_TEMPO) &&
          (e.flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE));

    if (failures)
        return 1;
    std::puts("transport: all checks passed");
    return 0;
}
