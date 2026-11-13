/* backline-test-offload-console: the console's side of a network offload
 * slot, against one or two backline-offloadd servers (primary, mirror), for
 * offload/tests/failover-proof.
 *
 * Every period (64 frames at 48 kHz by default) it sends one stereo block of
 * a known signal x[n] to every server and takes the block back
 * (offload1.h). The servers run the slot test effect (gain −6 dB, a 37-frame
 * latency), so every returned sample must be g·x[n − L]. Optionally, at a
 * given time, it kills the primary server (--kill-pid) or makes the
 * primary's plugin crash (--crash-primary) and checks the mirror took over
 * with no gap: no block missing, every sample still right.
 *
 *   backline-test-offload-console --server IP:PORT [--server IP:PORT] [--seconds S] [--frames N]
 *       [--budget-pct P] [--kill-pid PID --at S] [--crash-primary --at S]
 * Prints one JSON object; exit 0 when the checks hold.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "backline/offload1.h"

#define CH 2

static float sig(int64_t n, int ch) { return (float)(((n * 7919 + ch * 104729) % 2003) - 1001) / 1024.0f; }

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static void sleep_until(uint64_t t)
{
	struct timespec ts = { (time_t)(t / 1000000000ull), (long)(t % 1000000000ull) };
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

int main(int argc, char **argv)
{
	const char *hosts[BLO1_MAX_PEERS];
	uint16_t ports[BLO1_MAX_PEERS];
	char hostbuf[BLO1_MAX_PEERS][64];
	int n_peers = 0, kill_pid = 0, crash = 0, pct = 50, spin = 0;
	double seconds = 10, at = -1;
	uint32_t frames = 64, rate = 48000;
	for (int i = 1; i < argc; i++) {
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(argv[i], "--server") && v && n_peers < BLO1_MAX_PEERS) {
			snprintf(hostbuf[n_peers], sizeof(hostbuf[0]), "%s", v);
			char *colon = strrchr(hostbuf[n_peers], ':');
			ports[n_peers] = colon ? (uint16_t)atoi(colon + 1) : BLO1_DEFAULT_PORT;
			if (colon)
				*colon = 0;
			hosts[n_peers] = hostbuf[n_peers];
			n_peers++;
			i++;
		} else if (!strcmp(argv[i], "--seconds") && v) seconds = atof(v), i++;
		else if (!strcmp(argv[i], "--frames") && v) frames = (uint32_t)atoi(v), i++;
		else if (!strcmp(argv[i], "--budget-pct") && v) pct = atoi(v), i++;
		else if (!strcmp(argv[i], "--kill-pid") && v) kill_pid = atoi(v), i++;
		else if (!strcmp(argv[i], "--crash-primary")) crash = 1;
		else if (!strcmp(argv[i], "--spin")) spin = 1;
		else if (!strcmp(argv[i], "--at") && v) at = atof(v), i++;
		else {
			fprintf(stderr, "usage: see the header of tests/tools/offload-console.c\n");
			return 2;
		}
	}
	if (!n_peers)
		return 2;
	static struct blo1_client c;
	if (blo1_client_open(&c, 0, CH, (uint16_t)frames, hosts, ports, n_peers, 0) < 0) {
		fprintf(stderr, "offload-console: cannot open the client\n");
		return 1;
	}
	c.spin = spin;
	int lat = blo1_client_hello(&c, 3000);
	if (lat < 0) {
		fprintf(stderr, "offload-console: %s\n", lat == -2 ? "the servers report different latencies" : "no server answered");
		return 1;
	}
	uint32_t L = (uint32_t)lat;
	/* gain −6 dB on insert 0, everywhere (its range: −60..+24 dB) */
	struct blo1_event g = { 0, 2, 0, 0, (-6.0 + 60.0) / 84.0 };
	blo1_client_params(&c, -1, &g, 1);
	double gain = pow(10.0, -6.0 / 20.0);

	struct sched_param sp = { .sched_priority = 80 };
	int rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
	uint64_t period = (uint64_t)frames * 1000000000ull / rate;
	int blocks = (int)(seconds * rate / frames);
	int warm = 200; /* the gain lands and both chains' delay lines fill with the same signal */
	int at_block = at >= 0 ? (int)(at * rate / frames) : -1;
	uint64_t *rtt = calloc((size_t)blocks, 8);
	mlock(rtt, (size_t)blocks * 8);
	static float in0[BLO1_MAX_FRAMES], in1[BLO1_MAX_FRAMES], o0[BLO1_MAX_FRAMES], o1[BLO1_MAX_FRAMES];
	float *in[CH] = { in0, in1 }, *out[CH] = { o0, o1 };
	int served[BLO1_MAX_PEERS] = { 0 }, takeovers = 0, dry = 0, first_takeover = -1, nr = 0;
	int window_gaps = 0, primary_after = 0, takeover_in_window = -1;
	float worst = 0;
	uint64_t t = blo1_now_ns();
	for (int k = -warm; k < blocks; k++) {
		int64_t n0 = (int64_t)k * frames;
		for (uint32_t i = 0; i < frames; i++) {
			in0[i] = k < 0 ? 0.f : sig(n0 + i, 0);
			in1[i] = k < 0 ? 0.f : sig(n0 + i, 1);
		}
		if (k == at_block) {
			if (kill_pid)
				kill(kill_pid, SIGKILL);
			if (crash) {
				struct blo1_event e = { 0, 2, 0, 1, 1.0 }; /* insert 0, param 1 (crash), on */
				blo1_client_params(&c, 0, &e, 1);
			}
		}
		blo1_client_send(&c, (const float *const *)in);
		int who;
		enum blo1_result r = blo1_client_wait(&c, out, t + period * (uint64_t)pct / 100, &who);
		if (k >= 0) {
			if (r == BLO1_DRY)
				dry++;
			else
				served[who]++;
			/* the failure window: from 2 blocks before the failure to 50 after */
			if (at_block >= 0 && k >= at_block - 2 && k <= at_block + 50) {
				if (r == BLO1_DRY)
					window_gaps++;
				if (r == BLO1_TAKEOVER && takeover_in_window < 0 && k >= at_block)
					takeover_in_window = k;
			}
			if (at_block >= 0 && k > at_block + 2 && r != BLO1_DRY && who == 0)
				primary_after++;
			if (r == BLO1_TAKEOVER) {
				takeovers++;
				if (first_takeover < 0)
					first_takeover = k;
			}
			if (r != BLO1_DRY)
				rtt[nr++] = c.peer[who].rtt_ns_last;
			for (uint32_t i = 0; i < frames; i++) {
				int64_t n = n0 + i - (int64_t)L;
				for (int ch = 0; ch < CH; ch++) {
					float x = n >= 0 ? sig(n, ch) : 0.f;
					float want = r == BLO1_DRY ? x : (float)(gain * x);
					float e = fabsf(out[ch][i] - want);
					if (e > worst)
						worst = e;
				}
			}
		}
		t += period;
		sleep_until(t);
	}
	qsort(rtt, (size_t)nr, 8, cmp_u64);
#define P(p) (nr ? rtt[(size_t)((nr - 1) * (p))] / 1000.0 : -1.0)
	int fails = 0;
	/* without realtime priority a rare block misses everywhere; the hard rule
	 * is the failure window below */
	if (dry > blocks / 1000) {
		fprintf(stderr, "FAIL: %d blocks had no server answer in time (a gap)\n", dry);
		fails++;
	}
	if (at_block >= 0 && n_peers > 1 && window_gaps) {
		fprintf(stderr, "FAIL: %d gap blocks around the failure\n", window_gaps);
		fails++;
	}
	if (at_block >= 0 && n_peers > 1 && primary_after) {
		fprintf(stderr, "FAIL: the failed primary still served %d blocks\n", primary_after);
		fails++;
	}
	if (worst > 1e-5f) {
		fprintf(stderr, "FAIL: an output sample was wrong by %g\n", worst);
		fails++;
	}
	if (at_block >= 0 && n_peers > 1 && c.active != 1) {
		fprintf(stderr, "FAIL: the mirror is not active at the end\n");
		fails++;
	}
	printf("{\"servers\": %d, \"frames\": %u, \"rate\": %u, \"budget_us\": %.0f, \"latency\": %u, \"blocks\": %d, "
	       "\"served\": [%d, %d], \"takeovers\": %d, \"first_takeover\": %d, \"failed_at\": %d, \"takeover_after_failure\": %d, \"gap_blocks\": %d, \"gaps_in_failure_window\": %d, "
	       "\"max_error\": %g, \"rtt_us\": {\"p50\": %.1f, \"p99\": %.1f, \"p999\": %.1f, \"max\": %.1f}, \"rt\": %s, \"spin\": %s, "
	       "\"primary_late\": %llu, \"primary_dry_marked\": %llu}\n",
	       n_peers, frames, rate, period * pct / 100 / 1000.0, L, blocks, served[0], n_peers > 1 ? served[1] : 0, takeovers,
	       first_takeover, at_block, takeover_in_window, dry, window_gaps, worst, P(0.5), P(0.99), P(0.999), P(1.0), rt ? "true" : "false", spin ? "true" : "false",
	       (unsigned long long)c.peer[0].late, (unsigned long long)c.peer[0].dry_marked);
	free(rtt);
	blo1_client_close(&c);
	return fails ? 1 : 0;
}
