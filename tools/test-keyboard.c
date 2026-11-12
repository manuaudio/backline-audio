/* backline-test-keyboard: a scripted virtual MIDI keyboard.
 * SPDX-License-Identifier: MIT
 *
 * It creates an ALSA sequencer client with one readable port, exactly what a
 * USB keyboard looks like to PipeWire's MIDI bridge, waits until something
 * subscribes to it, then plays a script and logs when each event was sent
 * (CLOCK_MONOTONIC ns, the clock spa_io_position.clock.nsec uses).
 *
 *   backline-test-keyboard [--name NAME] [--wait SECS] [--linger SECS] STEP...
 *   --linger: stay connected this long after the last step (default 0.3;
 *             0 = "unplug" right away)
 *   STEP: on=KEY,VEL  off=KEY  cc=NUM,VAL  wait=SECONDS  (channel 1)
 */
#include <alsa/asoundlib.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int subscribers(snd_seq_t *seq, int client, int port)
{
	snd_seq_query_subscribe_t *q;
	snd_seq_addr_t addr = { .client = client, .port = port };
	int n = 0;
	snd_seq_query_subscribe_alloca(&q);
	snd_seq_query_subscribe_set_root(q, &addr);
	snd_seq_query_subscribe_set_type(q, SND_SEQ_QUERY_SUBS_READ);
	snd_seq_query_subscribe_set_index(q, 0);
	while (snd_seq_query_port_subscribers(seq, q) >= 0) {
		n++;
		snd_seq_query_subscribe_set_index(q, snd_seq_query_subscribe_get_index(q) + 1);
	}
	return n;
}

int main(int argc, char **argv)
{
	const char *name = "backline-test-keyboard";
	double wait_s = 10, linger_s = 0.3;
	static const struct option opts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "wait", required_argument, NULL, 'w' },
		{ "linger", required_argument, NULL, 'l' },
		{ 0 } };
	int c;
	while ((c = getopt_long(argc, argv, "n:w:l:", opts, NULL)) != -1) {
		if (c == 'n') name = optarg;
		else if (c == 'l') linger_s = atof(optarg);
		else if (c == 'w') wait_s = atof(optarg);
		else return 2;
	}

	snd_seq_t *seq;
	if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
		fprintf(stderr, "cannot open the ALSA sequencer\n");
		return 1;
	}
	snd_seq_set_client_name(seq, name);
	int port = snd_seq_create_simple_port(seq, "keys",
			SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
			SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
	int client = snd_seq_client_id(seq);
	printf("{\"event\": \"ready\", \"client\": %d, \"port\": %d, \"t_ns\": %llu}\n", client, port, now_ns());
	fflush(stdout);

	/* Wait for a subscriber: PipeWire's bridge, or anything else. */
	unsigned long long deadline = now_ns() + (unsigned long long)(wait_s * 1e9);
	while (subscribers(seq, client, port) == 0) {
		if (now_ns() > deadline) {
			fprintf(stderr, "nobody subscribed to %d:%d within %.1f s\n", client, port, wait_s);
			return 1;
		}
		struct timespec ts = { 0, 20 * 1000000L };
		nanosleep(&ts, NULL);
	}

	for (int i = optind; i < argc; i++) {
		snd_seq_event_t ev;
		int a = 0, b = 0;
		double s;
		snd_seq_ev_clear(&ev);
		snd_seq_ev_set_source(&ev, port);
		snd_seq_ev_set_subs(&ev);
		snd_seq_ev_set_direct(&ev);
		if (sscanf(argv[i], "on=%d,%d", &a, &b) == 2)
			snd_seq_ev_set_noteon(&ev, 0, a, b);
		else if (sscanf(argv[i], "off=%d", &a) == 1)
			snd_seq_ev_set_noteoff(&ev, 0, a, 0);
		else if (sscanf(argv[i], "cc=%d,%d", &a, &b) == 2)
			snd_seq_ev_set_controller(&ev, 0, a, b);
		else if (sscanf(argv[i], "wait=%lf", &s) == 1) {
			struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };
			nanosleep(&ts, NULL);
			continue;
		} else {
			fprintf(stderr, "bad step: %s\n", argv[i]);
			return 2;
		}
		/* t_ns is taken BEFORE sending: the event can be rendered before
		 * this process gets the CPU back. */
		unsigned long long t = now_ns();
		snd_seq_event_output_direct(seq, &ev);
		printf("{\"event\": \"%s\", \"a\": %d, \"b\": %d, \"t_ns\": %llu, \"sent_ns\": %llu}\n",
		       argv[i], a, b, t, now_ns());
		fflush(stdout);
	}
	snd_seq_drain_output(seq);
	/* Stay "plugged in" a moment, as a real keyboard does: closing the
	 * client removes the port, and an event still in flight with it. */
	struct timespec ls = { (time_t)linger_s, (long)((linger_s - (time_t)linger_s) * 1e9) };
	nanosleep(&ls, NULL);
	snd_seq_close(seq);
	return 0;
}
