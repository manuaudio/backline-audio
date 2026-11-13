// backline-clockd: the system transport, tempo, LTC/MTC and Ableton Link
// (docs/api/Clock1.md). SPDX-License-Identifier: MIT (sources; a build with
// Ableton Link makes the binary GPL-2.0-or-later, see clock/CMakeLists.txt).
//
// Why a JACK client. PipeWire's transport and musical position live in the
// driver's spa_io_position (state, segment, segment.bar). A client becomes
// the owner of that segment and moves the transport through its node
// activation, which pipewire-jack implements as the JACK timebase master and
// jack_transport_*: so clockd is a JACK client of pipewire-jack, and every
// PipeWire node (native or JACK) sees its transport in spa_io_position. No
// second clock and no private protocol.
//
// Threads. *Process* (PipeWire's data thread, through pipewire-jack): LTC
// and MTC generation from the transport frame, LTC/MTC decoding and the
// chase loop (locates are realtime-safe), the timebase callback (BBT from
// tempo, or from the Link session). *Main*: sd-event with D-Bus, OSC and a
// 50 ms housekeeping timer that publishes what the process thread measured.
// They share atomics only. *Link*: Link's own threads; its callbacks only
// store atomics.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <getopt.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <ltc.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "../host/osc.h"
#include "timecode.h"

#ifdef BL_HAVE_LINK
#include <ableton/Link.hpp>
#endif

namespace {

constexpr const char *kBusName = "io.github.manuaudio.Backline.Clock";
constexpr const char *kPath = "/io/github/manuaudio/Backline/Clock";
constexpr const char *kIface = "io.github.manuaudio.Backline.Clock1";
constexpr const char *kErrState = "io.github.manuaudio.Backline.Error.State";
constexpr const char *kErrNotFound = "io.github.manuaudio.Backline.Error.NotFound";
constexpr const char *kErrUnsupported = "io.github.manuaudio.Backline.Error.Unsupported";
constexpr double kTicksPerBeat = 1920.0;

enum Source { SRC_INTERNAL, SRC_LINK, SRC_LTC, SRC_MTC };
enum Mode { MODE_OFF, MODE_GENERATE, MODE_CHASE };
const char *source_name(int s) { return s == SRC_LINK ? "link" : s == SRC_LTC ? "ltc" : s == SRC_MTC ? "mtc" : "internal"; }
const char *mode_name(int m) { return m == MODE_GENERATE ? "generate" : m == MODE_CHASE ? "chase" : "off"; }

// One stretch of the internal source's tempo map: from frame0 on,
// beat = beat0 + (frame - frame0) x bpm / 60 / rate.
struct TempoAnchor {
	double bpm = 120, bpb = 4, beat_type = 4;
	int64_t frame0 = 0;
	double beat0 = 0;
};

// The tempo map: frame 0 is always beat 0 (bar 1, beat 1). A tempo or meter
// change while stopped sets the song's tempo from the start (one stretch); a
// change while rolling adds a stretch at the transport's frame, so the beat
// doesn't jump, and the map keeps it: locating back before it plays the
// earlier tempo, as heard. A change replaces every stretch after its frame.
constexpr int kMaxStretches = 64;
struct TempoMap {
	int n = 1;
	TempoAnchor seg[kMaxStretches];
	const TempoAnchor &at_frame(int64_t f) const
	{
		int i = n - 1;
		while (i > 0 && seg[i].frame0 > f)
			i--;
		return seg[i];
	}
	const TempoAnchor &at_beat(double b) const
	{
		int i = n - 1;
		while (i > 0 && seg[i].beat0 > b)
			i--;
		return seg[i];
	}
	double beat(int64_t f, uint32_t rate) const
	{
		const TempoAnchor &a = at_frame(f);
		return a.beat0 + (double)(f - a.frame0) * a.bpm / 60.0 / rate;
	}
	int64_t frame(double b, uint32_t rate) const
	{
		const TempoAnchor &a = at_beat(b);
		return a.frame0 + (int64_t)std::llround((b - a.beat0) * 60.0 / a.bpm * rate);
	}
};

struct Chase {
	// process thread only
	bool pending = false;     // a locate/start was requested; skip refs until it lands
	int skip_cycles = 0;
	int good = 0;
	int64_t last_ref_time = -1;
};

struct Clock {
	jack_client_t *client = nullptr;
	jack_port_t *ltc_out = nullptr, *ltc_in = nullptr, *mtc_out = nullptr, *mtc_in = nullptr;
	uint32_t rate = 48000;

	// published by main, read by process (double-buffered)
	TempoMap maps[2];
	std::atomic<int> map_idx{0};
	struct tc_rate fps_slots[2];
	std::atomic<int> fps_idx{0};
	std::atomic<int> ltc_mode{MODE_OFF}, mtc_mode{MODE_OFF}, source{SRC_INTERNAL};
	std::atomic<float> ltc_level{0.125f};   // -18 dBFS
	std::atomic<bool> link_on{false};
	std::atomic<int> tolerance{2};          // chase: samples of error accepted as locked

	// measured by process, read by main
	std::atomic<bool> rolling{false};
	std::atomic<int64_t> frame{0};
	std::atomic<bool> locked{false};
	std::atomic<int64_t> chase_error{0};
	std::atomic<uint64_t> refs{0}, relocates{0};
	std::atomic<uint64_t> cycles{0};
	std::atomic<bool> want_full_frame{false};

	// process thread state
	struct ltc_gen gen;
	struct tc_rate gen_rate{};
	LTCDecoder *dec = nullptr;
	struct mtc_rx mrx;
	Chase chase;
	int64_t last_frame = -1;
	bool last_rolling = false;
	int64_t counter = 0;      // samples processed by clockd (decoder time base)

#ifdef BL_HAVE_LINK
	ableton::Link *link = nullptr;
	std::atomic<double> link_tempo{0};
	std::atomic<uint32_t> link_peers{0};
	std::atomic<int> link_playing{-1};
	std::atomic<bool> link_tempo_changed{false}, link_play_changed{false};
#endif

	// main thread
	sd_event *event = nullptr;
	sd_bus *bus = nullptr;
	int osc_fd = -1;
	uint16_t osc_port = 0;
	bool last_pub_rolling = false, last_pub_locked = false;
	int64_t last_pub_frame = -1;
	uint64_t last_transport_emit = 0;
	double last_pub_bpm = 0, last_pub_bpb = 0, last_pub_bt = 0;
	uint32_t last_pub_peers = 0;
	std::string ltc_peer, mtc_peer;
	int last_source = SRC_INTERNAL;
	int idle_exit = 0;          // seconds; 0 = never
	uint64_t busy_since = 0;    // last time something needed the clock
};

Clock C;

uint64_t now_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

const TempoMap &tempo_map() { return C.maps[C.map_idx.load(std::memory_order_acquire)]; }
// The latest stretch: the tempo and meter clockd is set to.
const TempoAnchor &anchor() { const TempoMap &m = tempo_map(); return m.seg[m.n - 1]; }
const tc_rate &fps() { return C.fps_slots[C.fps_idx.load(std::memory_order_acquire)]; }

void set_map(const TempoMap &m)
{
	int next = C.map_idx.load() ^ 1;
	C.maps[next] = m;
	C.map_idx.store(next, std::memory_order_release);
}

#ifdef BL_HAVE_LINK
// Change the latest stretch in place (Link's tempo: the beat comes from Link).
void set_anchor(const TempoAnchor &a)
{
	TempoMap m = tempo_map();
	m.seg[m.n - 1] = a;
	set_map(m);
}
#endif

void set_fps(const tc_rate &r)
{
	int next = C.fps_idx.load() ^ 1;
	C.fps_slots[next] = r;
	C.fps_idx.store(next, std::memory_order_release);
}

// ------------------------------------------------------------- process thread
// A reference from incoming timecode: transport sample `x_in` was at clockd
// sample time `t`. Chase the transport to it.
void chase_ref(int64_t x_in, int64_t t, int64_t cycle_t, int64_t frame, bool rolling, jack_nframes_t n)
{
	Chase &ch = C.chase;
	ch.last_ref_time = t;
	C.refs.fetch_add(1, std::memory_order_relaxed);
	if (ch.pending)
		return;
	int64_t ours = rolling ? frame + (t - cycle_t) : frame;
	int64_t err = ours - x_in;
	C.chase_error.store(err, std::memory_order_relaxed);
	// A locate requested in cycle k lands at the start of cycle k + 2 (JACK's
	// rule, and measured here: one cycle later than k + 1 leaves the
	// transport exactly one quantum behind). Aim there.
	int64_t land = cycle_t + 2 * (int64_t)n;
	int64_t next_in = x_in + (land - t);
	if (!rolling) {
		jack_transport_locate(C.client, (jack_nframes_t)std::max<int64_t>(0, next_in));
		jack_transport_start(C.client);
		ch.pending = true;
		ch.skip_cycles = 4;
		ch.good = 0;
		C.relocates.fetch_add(1);
		C.locked.store(false);
		return;
	}
	if (std::llabs(err) > C.tolerance.load()) {
		jack_transport_locate(C.client, (jack_nframes_t)std::max<int64_t>(0, frame + (land - cycle_t) - err));
		ch.pending = true;
		ch.skip_cycles = 4;
		ch.good = 0;
		C.relocates.fetch_add(1);
		C.locked.store(false);
		return;
	}
	if (++ch.good >= 3)
		C.locked.store(true, std::memory_order_relaxed);
}

void chase_cycle_end(bool chasing, bool rolling, int64_t cycle_t, jack_nframes_t n)
{
	Chase &ch = C.chase;
	if (ch.pending && --ch.skip_cycles <= 0)
		ch.pending = false;
	if (!chasing)
		return;
	// incoming timecode stopped: stop with it (after ~4 LTC frames' worth)
	if (rolling && !ch.pending && ch.last_ref_time >= 0 && cycle_t + n - ch.last_ref_time > (int64_t)C.rate / 6) {
		jack_transport_stop(C.client);
		C.locked.store(false);
		ch.good = 0;
		ch.last_ref_time = -1;
	}
}

int on_process(jack_nframes_t n, void *)
{
	jack_position_t pos;
	jack_transport_state_t st = jack_transport_query(C.client, &pos);
	bool rolling = st == JackTransportRolling;
	int64_t frame = pos.frame;
	int64_t t = C.counter;
	C.cycles.fetch_add(1, std::memory_order_relaxed);
	const tc_rate &r = fps();
	int lm = C.ltc_mode.load(std::memory_order_relaxed), mm = C.mtc_mode.load(std::memory_order_relaxed);

	// ---- LTC out
	float *lo = (float *)jack_port_get_buffer(C.ltc_out, n);
	if (lm == MODE_GENERATE && rolling) {
		if (memcmp(&C.gen_rate, &r, sizeof(r)) || C.gen.rate != C.rate) {
			ltc_gen_init(&C.gen, &r, C.rate, C.ltc_level.load());
			C.gen_rate = r;
		}
		C.gen.amp = C.ltc_level.load(std::memory_order_relaxed);
		for (jack_nframes_t i = 0; i < n; i++)
			lo[i] = ltc_gen_sample(&C.gen, frame + i);
	} else {
		memset(lo, 0, n * sizeof(float));
		C.gen.next = -1;
	}

	// ---- MTC out
	void *mo = jack_port_get_buffer(C.mtc_out, n);
	jack_midi_clear_buffer(mo);
	if (mm == MODE_GENERATE) {
		bool moved = frame != C.last_frame + (C.last_rolling ? (int64_t)n : 0) && C.last_frame >= 0;
		if ((!rolling && (moved || C.last_rolling)) || C.want_full_frame.exchange(false)) {
			uint8_t ff[10];
			mtc_full_frame(&r, tc_frame_at(&r, C.rate, frame), ff);
			jack_midi_event_write(mo, 0, ff, 10);
		}
		if (rolling) {
			// quarter frames whose start falls in [frame, frame + n)
			int64_t q = (int64_t)(((__int128)frame * 4 * r.num) / ((__int128)C.rate * r.den));
			if (mtc_qf_start(&r, C.rate, q) < frame)
				q++;
			for (; mtc_qf_start(&r, C.rate, q) < frame + (int64_t)n; q++) {
				uint8_t m[2] = { 0xF1, mtc_qf_byte(&r, q) };
				jack_midi_event_write(mo, (jack_nframes_t)(mtc_qf_start(&r, C.rate, q) - frame), m, 2);
			}
		}
	}

	// ---- LTC in (chase)
	float *li = (float *)jack_port_get_buffer(C.ltc_in, n);
	if (lm == MODE_CHASE) {
		ltc_decoder_write_float(C.dec, li, n, t);
		LTCFrameExt fx;
		while (ltc_decoder_read(C.dec, &fx)) {
			SMPTETimecode stc;
			ltc_frame_to_time(&stc, &fx.ltc, 0);
			tc_label l{ stc.hours, stc.mins, stc.secs, stc.frame };
			int64_t k = tc_label_to_frames(&r, &l);
			if (fx.reverse)
				continue;
			// the end of a frame is where libltc is exact (its off_start is an
			// estimate, up to 8 samples off at 24 fps / 44.1 kHz:
			// tests/unit/test_timecode.c): frame k + 1 starts after it
			chase_ref(tc_frame_start(&r, C.rate, k + 1), fx.off_end + 1, t, frame, rolling, n);
		}
	}

	// ---- MTC in (chase)
	void *mi = jack_port_get_buffer(C.mtc_in, n);
	if (mm == MODE_CHASE) {
		uint32_t ne = jack_midi_get_event_count(mi);
		for (uint32_t e = 0; e < ne; e++) {
			jack_midi_event_t ev;
			if (jack_midi_event_get(&ev, mi, e))
				continue;
			if (ev.size == 2 && ev.buffer[0] == 0xF1) {
				int64_t q4;
				mtc_rx_quarter(&C.mrx, ev.buffer[1], t + ev.time, &q4);
				tc_rate rr;
				if (q4 >= 0 && tc_rate_from_mtc_code(C.mrx.rate_code, &rr))
					chase_ref(mtc_qf_start(&rr, C.rate, q4), t + ev.time, t, frame, rolling, n);
			} else {
				int code;
				int64_t f;
				tc_rate rr;
				if (mtc_rx_full(ev.buffer, (int)ev.size, &code, &f) && tc_rate_from_mtc_code(code, &rr) && !rolling) {
					jack_transport_locate(C.client, (jack_nframes_t)tc_frame_start(&rr, C.rate, f));
					C.relocates.fetch_add(1);
				}
			}
		}
	}
	chase_cycle_end(lm == MODE_CHASE || mm == MODE_CHASE, rolling, t, n);

	C.rolling.store(rolling, std::memory_order_relaxed);
	C.frame.store(frame, std::memory_order_relaxed);
	C.last_frame = frame;
	C.last_rolling = rolling;
	C.counter += n;
	return 0;
}

// BBT for the PipeWire segment (bar.bpm, signature, beat).
void on_timebase(jack_transport_state_t st, jack_nframes_t n, jack_position_t *pos, int new_pos, void *)
{
	// pipewire-jack publishes what this callback writes with the *next*
	// cycle (measured: the bar lagged the frame by one cycle), so describe
	// the frame and time that cycle will have. A new position is exact.
	int64_t at_frame = (int64_t)pos->frame;
	uint64_t at_usecs = pos->usecs;
	if (st == JackTransportRolling && !new_pos) {
		at_frame += n;
		at_usecs += (uint64_t)n * 1000000ull / C.rate;
	}
	const TempoAnchor &a = tempo_map().at_frame(at_frame);
	double bpm = a.bpm, bpb = a.bpb, beat;
#ifdef BL_HAVE_LINK
	if (C.link_on.load(std::memory_order_relaxed) && C.link) {
		auto s = C.link->captureAudioSessionState();
		auto at = std::chrono::microseconds((int64_t)at_usecs);
		bpm = s.tempo();
		beat = s.beatAtTime(at, bpb);
		if (beat < 0)
			beat = std::fmod(beat, bpb) + bpb;   // keep the bar grid, before the session's beat 0
	} else
#endif
	{
		beat = a.beat0 + (double)(at_frame - a.frame0) * bpm / 60.0 / C.rate;
	}
	double bar0 = std::floor(beat / bpb);
	double in_bar = beat - bar0 * bpb;
	pos->valid = (jack_position_bits_t)(pos->valid | JackPositionBBT);
	pos->bar = (int32_t)bar0 + 1;
	pos->beat = (int32_t)std::floor(in_bar) + 1;
	pos->tick = std::min((int32_t)kTicksPerBeat - 1, (int32_t)std::floor((in_bar - std::floor(in_bar)) * kTicksPerBeat + 1e-6));
	pos->bar_start_tick = bar0 * bpb * kTicksPerBeat;
	pos->beats_per_bar = (float)bpb;
	pos->beat_type = (float)a.beat_type;
	pos->ticks_per_beat = kTicksPerBeat;
	pos->beats_per_minute = bpm;
	// The ticks are the beat floored to 1/1920 (12.5 frames at 120 BPM and
	// 48 kHz). bbt_offset says how many frames before the cycle that tick
	// boundary was, so a reader gets the beat at the cycle's first frame to
	// within one frame: beat + offset x bpm / 60 / rate. pipewire-jack
	// carries it as the segment bar's offset.
	double tick_beat = bar0 * bpb + std::floor(in_bar) + pos->tick / kTicksPerBeat;
	double off = (beat - tick_beat) * 60.0 / bpm * C.rate;
	pos->valid = (jack_position_bits_t)(pos->valid | JackBBTFrameOffset);
	pos->bbt_offset = off > 0 ? (jack_nframes_t)off : 0;
}

// ---------------------------------------------------------------- main thread
void props_changed(const char *first, ...)
{
	const char *names[16];
	int n = 0;
	va_list ap;
	va_start(ap, first);
	for (const char *s = first; s && n < 15; s = va_arg(ap, const char *))
		names[n++] = s;
	va_end(ap);
	names[n] = nullptr;
	sd_bus_emit_properties_changed_strv(C.bus, kPath, kIface, (char **)names);
}

std::string timecode_now(int64_t frame)
{
	char s[32];
	tc_format(&fps(), tc_frame_at(&fps(), C.rate, std::max<int64_t>(0, frame)), s, sizeof(s));
	return s;
}

std::string bbt_now()
{
	jack_position_t pos;
	jack_transport_query(C.client, &pos);
	char s[64];
	if (pos.valid & JackPositionBBT)
		snprintf(s, sizeof(s), "%d|%d|%d", pos.bar, pos.beat, pos.tick);
	else
		snprintf(s, sizeof(s), "1|1|0");
	return s;
}

void emit_transport()
{
	int64_t f = jack_get_current_transport_frame(C.client);
	sd_bus_emit_signal(C.bus, kPath, kIface, "Transport", "btss", (int)C.rolling.load(), (uint64_t)f,
			   timecode_now(f).c_str(), bbt_now().c_str());
	C.last_transport_emit = now_ns();
}

int set_source(int s)
{
	if (C.source.exchange(s) != s)
		props_changed("Source", nullptr);
	return 0;
}

// Re-anchor the internal tempo map at the current position (so changing the
// tempo while rolling doesn't jump the beat).
void reanchor(double bpm, double bpb, double bt)
{
	TempoMap m = tempo_map();
	TempoAnchor a;
	a.bpm = bpm;
	a.bpb = bpb;
	a.beat_type = bt;
	jack_position_t pos;
	if (jack_transport_query(C.client, &pos) != JackTransportRolling || pos.frame == 0) {
		m.n = 1;            // stopped: the song's tempo, from frame 0
		m.seg[0] = a;
	} else {
		int64_t f = pos.frame;
		a.frame0 = f;
		a.beat0 = m.beat(f, C.rate);
		while (m.n > 1 && m.seg[m.n - 1].frame0 >= f)
			m.n--;
		if (m.n == 1 && m.seg[0].frame0 >= f)
			m.seg[0] = a;
		else if (m.n < kMaxStretches)
			m.seg[m.n++] = a;
		else
			m.seg[m.n - 1] = a;
	}
	set_map(m);
}

bool chasing() { return C.source.load() == SRC_LTC || C.source.load() == SRC_MTC; }

int m_play(sd_bus_message *m, void *, sd_bus_error *e)
{
	if (chasing())
		return sd_bus_error_setf(e, kErrState, "the transport is chasing %s", source_name(C.source.load()));
	jack_transport_start(C.client);
#ifdef BL_HAVE_LINK
	if (C.link_on && C.link) {
		auto s = C.link->captureAppSessionState();
		s.setIsPlaying(true, C.link->clock().micros());
		C.link->commitAppSessionState(s);
	}
#endif
	return sd_bus_reply_method_return(m, "");
}

int m_stop(sd_bus_message *m, void *, sd_bus_error *e)
{
	if (chasing())
		return sd_bus_error_setf(e, kErrState, "the transport is chasing %s", source_name(C.source.load()));
	jack_transport_stop(C.client);
#ifdef BL_HAVE_LINK
	if (C.link_on && C.link) {
		auto s = C.link->captureAppSessionState();
		s.setIsPlaying(false, C.link->clock().micros());
		C.link->commitAppSessionState(s);
	}
#endif
	return sd_bus_reply_method_return(m, "");
}

bool parse_position(const char *p, int64_t *frame)
{
	if (!strncmp(p, "frame:", 6))
		p += 6;
	else if (!strncmp(p, "tc:", 3))
		return tc_parse(&fps(), p + 3, frame) && (*frame = tc_frame_start(&fps(), C.rate, *frame), true);
	else if (!strncmp(p, "bbt:", 4)) {
		int bar, beat, tick = 0;
		if (sscanf(p + 4, "%d|%d|%d", &bar, &beat, &tick) < 2 || bar < 1 || beat < 1 || tick < 0)
			return false;
		const TempoAnchor &a = anchor();
		double b = (bar - 1) * a.bpb + (beat - 1) + tick / kTicksPerBeat;
		*frame = tempo_map().frame(b, C.rate);
		return *frame >= 0;
	}
	char *end;
	long long v = strtoll(p, &end, 10);
	if (*p == 0 || *end || v < 0 || v > UINT32_MAX)
		return false;
	*frame = v;
	return true;
}

// The frame a position names, with the current tempo map and frame rate,
// without moving the transport (the player places clips with it).
int m_resolve(sd_bus_message *m, void *, sd_bus_error *e)
{
	const char *p;
	int r = sd_bus_message_read(m, "s", &p);
	if (r < 0)
		return r;
	int64_t f;
	if (!parse_position(p, &f) || f > UINT32_MAX)
		return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "position \"%s\": frames, tc:HH:MM:SS:FF or bbt:BAR|BEAT|TICK", p);
	return sd_bus_reply_method_return(m, "t", (uint64_t)f);
}

int m_locate(sd_bus_message *m, void *, sd_bus_error *e)
{
	const char *p;
	int r = sd_bus_message_read(m, "s", &p);
	if (r < 0)
		return r;
	if (chasing())
		return sd_bus_error_setf(e, kErrState, "the transport is chasing %s", source_name(C.source.load()));
	int64_t f;
	if (!parse_position(p, &f) || f > UINT32_MAX)
		return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "position \"%s\": frames, tc:HH:MM:SS:FF or bbt:BAR|BEAT|TICK", p);
	jack_transport_locate(C.client, (jack_nframes_t)f);
	C.want_full_frame = true;
	return sd_bus_reply_method_return(m, "t", (uint64_t)f);
}

void apply_tempo(double bpm)
{
	const TempoAnchor &a = anchor();
	reanchor(bpm, a.bpb, a.beat_type);
#ifdef BL_HAVE_LINK
	if (C.link_on && C.link) {
		auto s = C.link->captureAppSessionState();
		s.setTempo(bpm, C.link->clock().micros());
		C.link->commitAppSessionState(s);
	}
#endif
}

int m_set_tempo(sd_bus_message *m, void *, sd_bus_error *e)
{
	double bpm;
	int r = sd_bus_message_read(m, "d", &bpm);
	if (r < 0)
		return r;
	if (!(bpm >= 20 && bpm <= 999))
		return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "tempo must be 20 to 999 BPM");
	apply_tempo(bpm);
	return sd_bus_reply_method_return(m, "");
}

int m_set_meter(sd_bus_message *m, void *, sd_bus_error *e)
{
	double n, d;
	int r = sd_bus_message_read(m, "dd", &n, &d);
	if (r < 0)
		return r;
	if (!(n >= 1 && n <= 64) || !(d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32))
		return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "meter: 1-64 beats per bar, beat type 1, 2, 4, 8, 16 or 32");
	reanchor(anchor().bpm, n, d);
	return sd_bus_reply_method_return(m, "");
}

int m_link(sd_bus_message *m, void *, sd_bus_error *e)
{
	int on;
	int r = sd_bus_message_read(m, "b", &on);
	if (r < 0)
		return r;
#ifdef BL_HAVE_LINK
	if (on && !C.link_on) {
		auto s = C.link->captureAppSessionState();
		if (C.link->numPeers() == 0)
			s.setTempo(anchor().bpm, C.link->clock().micros());
		C.link->commitAppSessionState(s);
		C.link->enable(true);
		C.link_on = true;
		set_source(SRC_LINK);
	} else if (!on && C.link_on) {
		C.link->enable(false);
		C.link_on = false;
		reanchor(C.link_tempo.load() > 0 ? C.link_tempo.load() : anchor().bpm, anchor().bpb, anchor().beat_type);
		set_source(SRC_INTERNAL);
	}
	props_changed("LinkEnabled", "LinkPeers", nullptr);
	return sd_bus_reply_method_return(m, "");
#else
	(void)on;
	return sd_bus_error_set(e, kErrUnsupported, "this clockd was built without Ableton Link");
#endif
}

// The JACK names of a node's ports. pipewire-jack names a port after the
// node's description or nick ("Built-in Audio Analog Stereo:capture_FL"),
// not its node.name, so match the aliases too: alias1 is the object path,
// alias2 the port.alias, "node.name:port.name". `peer` is a node.name (all
// its ports of that type and direction: a sink's outputs are its monitor) or
// one full port name or alias.
std::vector<std::string> ports_of(const std::string &peer, bool midi, unsigned long flags)
{
	std::vector<std::string> out;
	const char **all = jack_get_ports(C.client, nullptr, midi ? JACK_DEFAULT_MIDI_TYPE : JACK_DEFAULT_AUDIO_TYPE, flags);
	if (!all)
		return out;
	bool exact = peer.find(':') != std::string::npos;
	std::string prefix = peer + ":";
	std::vector<char> a1(jack_port_name_size()), a2(jack_port_name_size());
	for (int i = 0; all[i]; i++) {
		jack_port_t *p = jack_port_by_name(C.client, all[i]);
		if (!p || jack_port_is_mine(C.client, p))
			continue;
		char *al[2] = { a1.data(), a2.data() };
		a1[0] = a2[0] = 0;
		jack_port_get_aliases(p, al);
		std::string names[3] = { all[i], a1.data(), a2.data() };
		for (auto &nm : names) {
			if (nm.empty())
				continue;
			if (exact ? nm == peer : nm.compare(0, prefix.size(), prefix) == 0) {
				out.push_back(all[i]);
				break;
			}
		}
	}
	jack_free(all);
	return out;
}

// Connect our port to a node's ports: an output to all of them, an input
// (chase) to the first one.
bool connect_port(jack_port_t *ours, const std::string &peer, bool ours_is_output, bool midi)
{
	auto ports = ports_of(peer, midi, ours_is_output ? JackPortIsInput : JackPortIsOutput);
	for (auto &p : ports) {
		if (ours_is_output)
			jack_connect(C.client, jack_port_name(ours), p.c_str());
		else {
			jack_connect(C.client, p.c_str(), jack_port_name(ours));
			break;
		}
	}
	return !ports.empty();
}

bool node_exists(const std::string &peer, bool midi, bool want_outputs)
{
	return !ports_of(peer, midi, want_outputs ? JackPortIsOutput : JackPortIsInput).empty();
}

int timecode_call(sd_bus_message *m, sd_bus_error *e, bool ltc)
{
	const char *mode;
	int r = sd_bus_message_read(m, "s", &mode);
	if (r < 0)
		return r;
	int md = !strcmp(mode, "off") ? MODE_OFF : !strcmp(mode, "generate") ? MODE_GENERATE : !strcmp(mode, "chase") ? MODE_CHASE : -1;
	if (md < 0)
		return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "mode is off, generate or chase");
	double fpsv = tc_rate_fps(&fps()), level = -18;
	std::string target, source;
	if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *k, *s;
		sd_bus_message_read(m, "s", &k);
		if (!strcmp(k, "fps") || (ltc && !strcmp(k, "level"))) {
			double d;
			if (sd_bus_message_read(m, "v", "d", &d) < 0)
				return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "%s must be a double", k);
			(!strcmp(k, "fps") ? fpsv : level) = d;
		} else if (!strcmp(k, "target") || !strcmp(k, "source")) {
			if (sd_bus_message_read(m, "v", "s", &s) < 0)
				return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "%s must be a string", k);
			(!strcmp(k, "target") ? target : source) = s;
		} else {
			return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "unknown option \"%s\"", k);
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	tc_rate rr;
	if (!tc_rate_from_fps(fpsv, &rr))
		return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "fps is 24, 25, 29.97 or 30");
	if (md == MODE_CHASE && source.empty())
		return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "chase needs a source");
	if (!target.empty() && !node_exists(target, !ltc, false))
		return sd_bus_error_setf(e, kErrNotFound, "no %s input port on \"%s\"", ltc ? "audio" : "MIDI", target.c_str());
	if (!source.empty() && !node_exists(source, !ltc, true))
		return sd_bus_error_setf(e, kErrNotFound, "no %s output or monitor port on \"%s\"", ltc ? "audio" : "MIDI", source.c_str());

	jack_port_t *out = ltc ? C.ltc_out : C.mtc_out, *in = ltc ? C.ltc_in : C.mtc_in;
	jack_port_disconnect(C.client, out);
	jack_port_disconnect(C.client, in);
	set_fps(rr);
	if (ltc)
		C.ltc_level = (float)std::pow(10.0, level / 20.0);
	std::atomic<int> &mode_ref = ltc ? C.ltc_mode : C.mtc_mode;
	// only one chase at a time: the other one stops chasing
	if (md == MODE_CHASE) {
		std::atomic<int> &other = ltc ? C.mtc_mode : C.ltc_mode;
		if (other.load() == MODE_CHASE)
			other.store(MODE_OFF);
		if (ltc) {
			if (C.dec)
				ltc_decoder_free(C.dec);
			C.dec = ltc_decoder_create((int)(C.rate / tc_rate_fps(&rr)), 32);
		} else {
			mtc_rx_init(&C.mrx);
		}
		C.locked = false;
		C.chase = Chase{};
	}
	mode_ref.store(md);
	if (md == MODE_GENERATE && !target.empty())
		connect_port(out, target, true, !ltc);
	if (md == MODE_CHASE)
		connect_port(in, source, false, !ltc);
	if (md == MODE_CHASE)
		set_source(ltc ? SRC_LTC : SRC_MTC);
	else if (C.source.load() == (ltc ? SRC_LTC : SRC_MTC))
		set_source(
#ifdef BL_HAVE_LINK
			C.link_on ? SRC_LINK :
#endif
			SRC_INTERNAL);
	if (md == MODE_GENERATE && !ltc)
		C.want_full_frame = true;
	props_changed(ltc ? "LtcMode" : "MtcMode", "Fps", "Locked", nullptr);
	return sd_bus_reply_method_return(m, "");
}

int m_ltc(sd_bus_message *m, void *, sd_bus_error *e) { return timecode_call(m, e, true); }
int m_mtc(sd_bus_message *m, void *, sd_bus_error *e) { return timecode_call(m, e, false); }

int append_state(sd_bus_message *reply)
{
	jack_position_t pos;
	jack_transport_state_t st = jack_transport_query(C.client, &pos);
	char bbt[64];
	snprintf(bbt, sizeof(bbt), "%d|%d|%d", pos.bar, pos.beat, pos.tick);
	const TempoAnchor &a = anchor();
	double bpm = (pos.valid & JackPositionBBT) ? pos.beats_per_minute : a.bpm;
	sd_bus_message_open_container(reply, 'a', "{sv}");
	sd_bus_message_append(reply, "{sv}", "Rolling", "b", (int)(st == JackTransportRolling));
	sd_bus_message_append(reply, "{sv}", "Frame", "t", (uint64_t)pos.frame);
	sd_bus_message_append(reply, "{sv}", "Rate", "u", C.rate);
	sd_bus_message_append(reply, "{sv}", "Tempo", "d", bpm);
	sd_bus_message_append(reply, "{sv}", "BeatsPerBar", "d", a.bpb);
	sd_bus_message_append(reply, "{sv}", "BeatType", "d", a.beat_type);
	sd_bus_message_append(reply, "{sv}", "Fps", "d", tc_rate_fps(&fps()));
	sd_bus_message_append(reply, "{sv}", "Timecode", "s", timecode_now(pos.frame).c_str());
	sd_bus_message_append(reply, "{sv}", "Bbt", "s", bbt);
	sd_bus_message_append(reply, "{sv}", "Source", "s", source_name(C.source.load()));
	sd_bus_message_append(reply, "{sv}", "LinkEnabled", "b", (int)C.link_on.load());
#ifdef BL_HAVE_LINK
	sd_bus_message_append(reply, "{sv}", "LinkPeers", "u", C.link_peers.load());
#else
	sd_bus_message_append(reply, "{sv}", "LinkPeers", "u", 0u);
#endif
	sd_bus_message_append(reply, "{sv}", "LtcMode", "s", mode_name(C.ltc_mode.load()));
	sd_bus_message_append(reply, "{sv}", "MtcMode", "s", mode_name(C.mtc_mode.load()));
	sd_bus_message_append(reply, "{sv}", "Locked", "b", (int)C.locked.load());
	sd_bus_message_append(reply, "{sv}", "ChaseError", "x", C.chase_error.load());
	sd_bus_message_append(reply, "{sv}", "Node", "s", jack_get_client_name(C.client));
	sd_bus_message_append(reply, "{sv}", "OscPort", "u", (uint32_t)C.osc_port);
	return sd_bus_message_close_container(reply);
}

int m_get(sd_bus_message *m, void *, sd_bus_error *)
{
	sd_bus_message *reply;
	int r = sd_bus_message_new_method_return(m, &reply);
	if (r < 0)
		return r;
	append_state(reply);
	r = sd_bus_send(nullptr, reply, nullptr);
	sd_bus_message_unref(reply);
	return r;
}

// One getter for every property: the same snapshot as Get.
int p_get(sd_bus *, const char *, const char *, const char *prop, sd_bus_message *reply, void *, sd_bus_error *)
{
	jack_position_t pos;
	jack_transport_state_t st = jack_transport_query(C.client, &pos);
	const TempoAnchor &a = anchor();
	std::string p = prop;
	if (p == "Rolling") return sd_bus_message_append(reply, "b", (int)(st == JackTransportRolling));
	if (p == "Frame") return sd_bus_message_append(reply, "t", (uint64_t)pos.frame);
	if (p == "Rate") return sd_bus_message_append(reply, "u", C.rate);
	if (p == "Tempo") return sd_bus_message_append(reply, "d", (pos.valid & JackPositionBBT) ? pos.beats_per_minute : a.bpm);
	if (p == "BeatsPerBar") return sd_bus_message_append(reply, "d", a.bpb);
	if (p == "BeatType") return sd_bus_message_append(reply, "d", a.beat_type);
	if (p == "Fps") return sd_bus_message_append(reply, "d", tc_rate_fps(&fps()));
	if (p == "Timecode") return sd_bus_message_append(reply, "s", timecode_now(pos.frame).c_str());
	if (p == "Bbt") return sd_bus_message_append(reply, "s", bbt_now().c_str());
	if (p == "Source") return sd_bus_message_append(reply, "s", source_name(C.source.load()));
	if (p == "LinkEnabled") return sd_bus_message_append(reply, "b", (int)C.link_on.load());
#ifdef BL_HAVE_LINK
	if (p == "LinkPeers") return sd_bus_message_append(reply, "u", C.link_peers.load());
#else
	if (p == "LinkPeers") return sd_bus_message_append(reply, "u", 0u);
#endif
	if (p == "LtcMode") return sd_bus_message_append(reply, "s", mode_name(C.ltc_mode.load()));
	if (p == "MtcMode") return sd_bus_message_append(reply, "s", mode_name(C.mtc_mode.load()));
	if (p == "Locked") return sd_bus_message_append(reply, "b", (int)C.locked.load());
	if (p == "ChaseError") return sd_bus_message_append(reply, "x", C.chase_error.load());
	if (p == "Node") return sd_bus_message_append(reply, "s", jack_get_client_name(C.client));
	if (p == "OscPort") return sd_bus_message_append(reply, "u", (uint32_t)C.osc_port);
	return -EINVAL;
}

#define PROP(name, sig, flags) SD_BUS_PROPERTY(name, sig, p_get, 0, flags)
const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Play", "", "", m_play, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Stop", "", "", m_stop, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Locate", SD_BUS_ARGS("s", position), SD_BUS_RESULT("t", frame), m_locate, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Resolve", SD_BUS_ARGS("s", position), SD_BUS_RESULT("t", frame), m_resolve, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetTempo", SD_BUS_ARGS("d", bpm), SD_BUS_NO_RESULT, m_set_tempo, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetMeter", SD_BUS_ARGS("d", beats_per_bar, "d", beat_type), SD_BUS_NO_RESULT, m_set_meter, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Link", SD_BUS_ARGS("b", enable), SD_BUS_NO_RESULT, m_link, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Ltc", SD_BUS_ARGS("s", mode, "a{sv}", options), SD_BUS_NO_RESULT, m_ltc, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Mtc", SD_BUS_ARGS("s", mode, "a{sv}", options), SD_BUS_NO_RESULT, m_mtc, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Get", SD_BUS_NO_ARGS, SD_BUS_RESULT("a{sv}", state), m_get, SD_BUS_VTABLE_UNPRIVILEGED),
	PROP("Rolling", "b", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("Frame", "t", 0),
	PROP("Rate", "u", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("Tempo", "d", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("BeatsPerBar", "d", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("BeatType", "d", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("Fps", "d", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("Timecode", "s", 0),
	PROP("Bbt", "s", 0),
	PROP("Source", "s", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("LinkEnabled", "b", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("LinkPeers", "u", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("LtcMode", "s", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("MtcMode", "s", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("Locked", "b", SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	PROP("ChaseError", "x", 0),
	PROP("Node", "s", SD_BUS_VTABLE_PROPERTY_CONST),
	PROP("OscPort", "u", SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_SIGNAL_WITH_ARGS("Transport", SD_BUS_ARGS("b", rolling, "t", frame, "s", timecode, "s", bbt), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Tempo", SD_BUS_ARGS("d", bpm, "d", beats_per_bar, "d", beat_type), 0),
	SD_BUS_SIGNAL_WITH_ARGS("LinkPeers", SD_BUS_ARGS("u", peers), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Lock", SD_BUS_ARGS("s", source, "b", locked), 0),
	SD_BUS_VTABLE_END
};

// Housekeeping: publish what the process thread and Link changed.
int on_tick(sd_event_source *s, uint64_t, void *)
{
	// Re-arm 50 ms from now, not from the last deadline: the source starts at time 0, so
	// "deadline + 50 ms" stays in the past for the whole uptime and the loop spins at 100% CPU.
	uint64_t now = 0;
	sd_event_now(sd_event_source_get_event(s), CLOCK_MONOTONIC, &now);
	sd_event_source_set_time(s, now + 50000);
	bool rolling = C.rolling.load();
	int64_t f = C.frame.load();
	bool moved = C.last_pub_frame >= 0 && !rolling && f != C.last_pub_frame;
	if (rolling != C.last_pub_rolling || moved) {
		props_changed("Rolling", nullptr);
		emit_transport();
	} else if (rolling && now_ns() - C.last_transport_emit > 250000000ull) {
		emit_transport();
	}
	C.last_pub_rolling = rolling;
	C.last_pub_frame = f;
#ifdef BL_HAVE_LINK
	if (C.link_on && C.link_tempo_changed.exchange(false)) {
		TempoAnchor a = anchor();
		a.bpm = C.link_tempo.load();
		set_anchor(a);
	}
	if (C.link_on && C.link_play_changed.exchange(false) && C.source.load() == SRC_LINK) {
		if (C.link_playing.load() == 1)
			jack_transport_start(C.client);
		else if (C.link_playing.load() == 0)
			jack_transport_stop(C.client);
	}
	uint32_t peers = C.link_peers.load();
	if (peers != C.last_pub_peers) {
		C.last_pub_peers = peers;
		sd_bus_emit_signal(C.bus, kPath, kIface, "LinkPeers", "u", peers);
		props_changed("LinkPeers", nullptr);
	}
#endif
	const TempoAnchor &a = anchor();
	if (a.bpm != C.last_pub_bpm || a.bpb != C.last_pub_bpb || a.beat_type != C.last_pub_bt) {
		C.last_pub_bpm = a.bpm;
		C.last_pub_bpb = a.bpb;
		C.last_pub_bt = a.beat_type;
		sd_bus_emit_signal(C.bus, kPath, kIface, "Tempo", "ddd", a.bpm, a.bpb, a.beat_type);
		props_changed("Tempo", "BeatsPerBar", "BeatType", nullptr);
	}
	// idle: stopped, no Link, no timecode
	if (rolling || C.link_on.load() || C.ltc_mode.load() != MODE_OFF || C.mtc_mode.load() != MODE_OFF || !C.busy_since)
		C.busy_since = now_ns();
	else if (C.idle_exit > 0 && now_ns() - C.busy_since > (uint64_t)C.idle_exit * 1000000000ull)
		sd_event_exit(C.event, 0);
	bool locked = C.locked.load();
	if (locked != C.last_pub_locked) {
		C.last_pub_locked = locked;
		sd_bus_emit_signal(C.bus, kPath, kIface, "Lock", "sb", source_name(C.source.load()), (int)locked);
		props_changed("Locked", nullptr);
	}
	return 0;
}

int on_osc(sd_event_source *, int fd, uint32_t, void *)
{
	uint8_t buf[2048];
	ssize_t n = recv(fd, buf, sizeof(buf), 0);
	osc::Message msg;
	if (n <= 0 || !osc::parse(buf, (size_t)n, msg))
		return 0;
	double d;
	if (msg.address == "/backline/transport/play" && !chasing())
		jack_transport_start(C.client);
	else if (msg.address == "/backline/transport/stop" && !chasing())
		jack_transport_stop(C.client);
	else if (msg.address == "/backline/transport/locate" && !chasing() && !msg.args.empty()) {
		int64_t f;
		if (auto s = std::get_if<std::string>(&msg.args[0]); s && parse_position(s->c_str(), &f))
			jack_transport_locate(C.client, (jack_nframes_t)f);
		else if (osc::as_double(msg.args[0], d) && d >= 0)
			jack_transport_locate(C.client, (jack_nframes_t)d);
	} else if (msg.address == "/backline/transport/tempo" && !msg.args.empty() && osc::as_double(msg.args[0], d) && d >= 20 && d <= 999)
		apply_tempo(d);
	return 0;
}

int on_signal(sd_event_source *, const struct signalfd_siginfo *, void *)
{
	sd_event_exit(C.event, 0);
	return 0;
}

void on_jack_shutdown(void *)
{
	// the PipeWire server went away: nothing to clock
	_exit(1);
}

void usage(FILE *f)
{
	fputs("usage: backline-clockd [--name NAME] [--replace] [--tolerance SAMPLES] [--idle-exit SECONDS]\n"
	      "  The Backline clock on the session bus (io.github.manuaudio.Backline.Clock, Clock1):\n"
	      "  transport, tempo and meter as PipeWire's position segment (the JACK timebase master),\n"
	      "  LTC and MTC generate and chase"
#ifdef BL_HAVE_LINK
	      ", Ableton Link"
#endif
	      ".\n", f);
}

} // namespace

int main(int argc, char **argv)
{
	static const option opts[] = {
		{ "name", required_argument, nullptr, 'n' },
		{ "replace", no_argument, nullptr, 'r' },
		{ "tolerance", required_argument, nullptr, 't' },
		{ "idle-exit", required_argument, nullptr, 'i' },
		{ "help", no_argument, nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 },
	};
	std::string name = "backline.clock";
	int c, replace = 0;
	while ((c = getopt_long(argc, argv, "h", opts, nullptr)) != -1) {
		switch (c) {
		case 'n': name = optarg; break;
		case 'r': replace = 1; break;
		case 't': C.tolerance = atoi(optarg); break;
		case 'i': C.idle_exit = atoi(optarg); break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}

	// OSC on localhost; the port goes into the node's properties
	C.osc_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t sl = sizeof(sa);
	if (C.osc_fd >= 0 && bind(C.osc_fd, (sockaddr *)&sa, sizeof(sa)) == 0 && getsockname(C.osc_fd, (sockaddr *)&sa, &sl) == 0)
		C.osc_port = ntohs(sa.sin_port);
	{
		// pipewire-jack reads node properties from PIPEWIRE_PROPS: add ours
		std::string props = getenv("PIPEWIRE_PROPS") ? getenv("PIPEWIRE_PROPS") : "";
		size_t close = props.rfind('}');
		std::string ours = " backline.service = clock backline.dbus = io.github.manuaudio.Backline.Clock backline.osc.port = " +
				   std::to_string(C.osc_port) + " node.always-process = true jack.fill-aliases = true ";
		props = close == std::string::npos ? "{" + ours + "}" : props.substr(0, close) + ours + "}";
		setenv("PIPEWIRE_PROPS", props.c_str(), 1);
	}

	set_map(TempoMap{});
	tc_rate r25;
	tc_rate_from_fps(25, &r25);
	set_fps(r25);
	mtc_rx_init(&C.mrx);

	jack_status_t status;
	C.client = jack_client_open(name.c_str(), JackNoStartServer, &status);
	if (!C.client) {
		fprintf(stderr, "backline-clockd: cannot connect to PipeWire (pipewire-jack), status 0x%x\n", status);
		return 1;
	}
	C.rate = jack_get_sample_rate(C.client);
	C.dec = ltc_decoder_create((int)(C.rate / 25), 32);
	C.ltc_out = jack_port_register(C.client, "ltc_out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	C.ltc_in = jack_port_register(C.client, "ltc_in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	C.mtc_out = jack_port_register(C.client, "mtc_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
	C.mtc_in = jack_port_register(C.client, "mtc_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
	jack_set_process_callback(C.client, on_process, nullptr);
	jack_on_shutdown(C.client, on_jack_shutdown, nullptr);
	if (jack_set_timebase_callback(C.client, 0, on_timebase, nullptr) != 0)
		fprintf(stderr, "backline-clockd: could not become the timebase master (another client is)\n");

#ifdef BL_HAVE_LINK
	C.link = new ableton::Link(anchor().bpm);
	C.link->setNumPeersCallback([](std::size_t n) { C.link_peers = (uint32_t)n; });
	C.link->setTempoCallback([](double bpm) { C.link_tempo = bpm; C.link_tempo_changed = true; });
	C.link->enableStartStopSync(true);
	C.link->setStartStopCallback([](bool playing) { C.link_playing = playing ? 1 : 0; C.link_play_changed = true; });
#endif

	if (jack_activate(C.client)) {
		fprintf(stderr, "backline-clockd: cannot activate\n");
		return 1;
	}

	sd_event_default(&C.event);
	sigset_t ss;
	sigemptyset(&ss);
	sigaddset(&ss, SIGINT);
	sigaddset(&ss, SIGTERM);
	sigprocmask(SIG_BLOCK, &ss, nullptr);
	sd_event_add_signal(C.event, nullptr, SIGINT, on_signal, nullptr);
	sd_event_add_signal(C.event, nullptr, SIGTERM, on_signal, nullptr);
	int rr = sd_bus_open_user(&C.bus);
	if (rr >= 0)
		rr = sd_bus_add_object_vtable(C.bus, nullptr, kPath, kIface, vtable, nullptr);
	if (rr >= 0)
		rr = sd_bus_request_name(C.bus, kBusName, replace ? SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT
								 : SD_BUS_NAME_ALLOW_REPLACEMENT);
	if (rr < 0) {
		fprintf(stderr, "backline-clockd: session bus: %s\n", strerror(-rr));
		jack_client_close(C.client);
		return 1;
	}
	sd_bus_attach_event(C.bus, C.event, 0);
	sd_event_source *tick;
	sd_event_add_time(C.event, &tick, CLOCK_MONOTONIC, 0, 1000, on_tick, nullptr);
	sd_event_source_set_enabled(tick, SD_EVENT_ON);
	if (C.osc_fd >= 0)
		sd_event_add_io(C.event, nullptr, C.osc_fd, EPOLLIN, on_osc, nullptr);
	fprintf(stderr, "backline-clockd: %s at %u Hz, OSC 127.0.0.1:%u%s\n", jack_get_client_name(C.client), C.rate, C.osc_port,
#ifdef BL_HAVE_LINK
		", Link available"
#else
		", without Link"
#endif
	);
	sd_event_loop(C.event);

	// leave the transport stopped and the segment unowned
	jack_transport_stop(C.client);
	jack_release_timebase(C.client);
#ifdef BL_HAVE_LINK
	C.link->enable(false);
#endif
	jack_deactivate(C.client);
	jack_client_close(C.client);
#ifdef BL_HAVE_LINK
	delete C.link;
#endif
	if (C.dec)
		ltc_decoder_free(C.dec);
	sd_bus_flush_close_unref(C.bus);
	sd_event_unref(C.event);
	return 0;
}
