// backline-test-linkpeer: a second Ableton Link peer for the clock proof.
// SPDX-License-Identifier: MIT (source; the binary links Link, GPL-2.0-or-later)
//
// It joins the Link session and, as a JACK client in the same PipeWire graph,
// reads the transport clockd publishes. Every ~100 ms it prints
//   P <usecs> <peers> <link_bpm> <transport_bpm> <transport_beat> <link_beat> <diff_beats> <rolling>
// where link_beat is this peer's own Link session beat at the cycle time and
// transport_beat is the beat in PipeWire's segment (from JACK BBT), quantum =
// beats per bar. stdin: "tempo BPM" proposes a tempo to the session.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <ableton/Link.hpp>
#include <jack/jack.h>

static ableton::Link *L;
static jack_client_t *J;
static std::atomic<double> beat_t{0}, beat_l{0}, bpm_t{0}, bpm_l{0}, quantum{4};
static std::atomic<uint64_t> usecs{0};
static std::atomic<int> rolling{0}, have{0};

static int process(jack_nframes_t, void *)
{
	jack_position_t pos;
	jack_transport_state_t st = jack_transport_query(J, &pos);
	if (!(pos.valid & JackPositionBBT))
		return 0;
	double bpb = pos.beats_per_bar;
	double tb = (pos.bar - 1) * bpb + (pos.beat - 1) + pos.tick / pos.ticks_per_beat;
	auto s = L->captureAudioSessionState();
	double lb = s.beatAtTime(std::chrono::microseconds((int64_t)pos.usecs), bpb);
	beat_t = tb;
	quantum = bpb;
	beat_l = lb;
	bpm_t = pos.beats_per_minute;
	bpm_l = s.tempo();
	usecs = pos.usecs;
	rolling = st == JackTransportRolling;
	have = 1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "backline-test.linkpeer";
	L = new ableton::Link(120.0);
	L->enable(true);
	J = jack_client_open(name, JackNoStartServer, nullptr);
	if (!J) {
		fprintf(stderr, "no JACK/PipeWire\n");
		return 1;
	}
	jack_set_process_callback(J, process, nullptr);
	jack_activate(J);
	std::thread input([] {
		char line[128];
		while (fgets(line, sizeof(line), stdin)) {
			double t;
			if (sscanf(line, "tempo %lf", &t) == 1) {
				auto s = L->captureAppSessionState();
				s.setTempo(t, L->clock().micros());
				L->commitAppSessionState(s);
				printf("T set %.3f\n", t);
				fflush(stdout);
			}
		}
	});
	input.detach();
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!have)
			continue;
		double bpb = quantum, d = beat_t - beat_l;
		d -= std::round(d / bpb) * bpb;   // phase within the bar (Link quantum = beats per bar)
		printf("P %llu %zu %.6f %.6f %.6f %.6f %.9f %d\n", (unsigned long long)usecs.load(), L->numPeers(), bpm_l.load(),
		       bpm_t.load(), beat_t.load(), beat_l.load(), d, rolling.load());
		fflush(stdout);
	}
}
