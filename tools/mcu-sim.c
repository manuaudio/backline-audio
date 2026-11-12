/* backline-test-mcu-sim: a software X-Touch / MCU on PipeWire, for testing
 * surfaced without hardware. It is a MIDI node with a port to the host and a
 * port from it, named like the device, that speaks the device side of the
 * protocol (tools/mcusim.c). The test drives the "engineer" through
 * commands on stdin and reads what the device shows with "dump".
 *
 *   touch N 1|0        fader N (1-8, 9 = master) touched / released
 *   move N V           fader N to V (0-16383, or 0.0-1.0)
 *   press NAME|ID      a button down (NAME as in Surface1.md: mute-3, flip, bank-right)
 *   release NAME|ID    and up
 *   tap NAME|ID        down and up
 *   pot N TICKS        V-Pot N turned (negative: counter-clockwise)
 *   jog TICKS
 *   hello              the device's own connection query (as at power-up)
 *   raw HEX            any MIDI bytes (b0071fb02705: two CCs), as a generic controller would send
 *   unplug | plug      the USB cable: the node goes away / comes back powered up blank
 *   dump PATH          the device's state as JSON to PATH (written atomically)
 *   quit
 *
 * Throwaway: nothing is linked by this tool; whoever wants the device links
 * to it, as with a real one.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include "bl_midibuf.h"
#include "bl_midi_pw.h"
#include "mcusim.h"

static struct {
	struct pw_main_loop *loop;
	struct pw_loop *l;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_filter *filter;
	struct spa_hook listener;
	void *in_port, *out_port;
	struct spa_source *stdin_src, *rx_ev;
	struct bl_midi_ring rx, tx;
	struct bl_midi_parser parser;
	struct mcusim sim;
	char name[128], alias[160], group[96];
	char line[1024];
	size_t line_len;
	bool verbose;
} M;

static void out_cb(void *data, const uint8_t *m, size_t n)
{
	bl_midi_ring_push(&M.tx, m, n);
}

static void rx_push(void *data, const uint8_t *m, size_t n)
{
	bl_midi_ring_push(&M.rx, m, n);
}

static void on_process(void *data, struct spa_io_position *pos)
{
	if (M.out_port) {
		struct pw_buffer *b = pw_filter_dequeue_buffer(M.out_port);
		if (b) {
			bl_midi_pw_write(&b->buffer->datas[0], &M.tx);
			pw_filter_queue_buffer(M.out_port, b);
		}
	}
	if (M.in_port) {
		struct pw_buffer *b = pw_filter_dequeue_buffer(M.in_port);
		if (b) {
			uint32_t before = atomic_load_explicit(&M.rx.w, memory_order_relaxed);
			bl_midi_pw_read(&b->buffer->datas[0], rx_push, NULL);
			pw_filter_queue_buffer(M.in_port, b);
			if (atomic_load_explicit(&M.rx.w, memory_order_relaxed) != before)
				pw_loop_signal_event(M.l, M.rx_ev);
		}
	}
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
};

static void plug(void)
{
	if (M.filter)
		return;
	mcusim_reset(&M.sim);
	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Midi", PW_KEY_MEDIA_CATEGORY, "Duplex", PW_KEY_MEDIA_CLASS, "Midi/Bridge",
		PW_KEY_NODE_NAME, M.name, PW_KEY_NODE_DESCRIPTION, "Backline test: software MCU", PW_KEY_NODE_AUTOCONNECT, "false",
		NULL);
	if (M.group[0])
		pw_properties_set(props, PW_KEY_NODE_GROUP, M.group);
	M.filter = pw_filter_new(M.core, M.name, props);
	pw_filter_add_listener(M.filter, &M.listener, &filter_events, NULL);
	/* a device's capture side (it sends) and playback side (it receives),
	 * with one alias, as PipeWire's MIDI bridge names a USB device's ports */
	M.out_port = pw_filter_add_port(M.filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
					pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME, "capture",
							  PW_KEY_PORT_ALIAS, M.alias, NULL), NULL, 0);
	M.in_port = pw_filter_add_port(M.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
				       pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME, "playback",
							 PW_KEY_PORT_ALIAS, M.alias, NULL), NULL, 0);
	pw_filter_connect(M.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
	printf("plugged %s\n", M.name);
	fflush(stdout);
}

static void unplug(void)
{
	if (!M.filter)
		return;
	spa_hook_remove(&M.listener);
	pw_filter_destroy(M.filter);
	M.filter = NULL;
	M.in_port = M.out_port = NULL;
	while (bl_midi_ring_next(&M.tx)) {
		uint8_t b[BL_MIDI_SYSEX_MAX];
		bl_midi_ring_pop(&M.tx, b, sizeof(b));
	}
	for (int i = 0; i < 9; i++)
		M.sim.touched[i] = false;
	printf("unplugged %s\n", M.name);
	fflush(stdout);
}

static void sim_msg(void *data, const uint8_t *m, size_t n)
{
	if (m[0] >= 0xf8)
		return;
	mcusim_receive(&M.sim, m, n);
}

static void on_rx(void *data, uint64_t count)
{
	uint8_t b[BL_MIDI_SYSEX_MAX];
	size_t n;
	while ((n = bl_midi_ring_pop(&M.rx, b, sizeof(b))) > 0)
		if (M.filter)
			bl_midi_parse(&M.parser, b, n, sim_msg, NULL);
}

static int control_id(const char *s)
{
	if (!strncmp(s, "0x", 2))
		return (int)strtol(s, NULL, 16);
	int id = mcu_control_id(s);
	return id;
}

static void command(char *line)
{
	char *argv[4] = { 0 };
	int argc = 0;
	for (char *save = NULL, *t = strtok_r(line, " \t", &save); t && argc < 4; t = strtok_r(NULL, " \t", &save))
		argv[argc++] = t;
	if (!argc)
		return;
	const char *c = argv[0];
	bool ok = true;
	if (!strcmp(c, "quit")) {
		pw_main_loop_quit(M.loop);
	} else if (!strcmp(c, "plug")) {
		plug();
	} else if (!strcmp(c, "unplug")) {
		unplug();
	} else if (!M.filter && strcmp(c, "dump")) {
		ok = false; /* unplugged: the engineer's hands do nothing */
	} else if (!strcmp(c, "touch") && argc == 3) {
		mcusim_touch(&M.sim, atoi(argv[1]) - 1, atoi(argv[2]) != 0);
	} else if (!strcmp(c, "move") && argc == 3) {
		double v = atof(argv[2]);
		mcusim_move(&M.sim, atoi(argv[1]) - 1, strchr(argv[2], '.') ? (int)(v * 16383 + 0.5) : (int)v);
	} else if ((!strcmp(c, "press") || !strcmp(c, "release") || !strcmp(c, "tap")) && argc == 2) {
		int id = control_id(argv[1]);
		if (id < 0) {
			ok = false;
		} else {
			if (strcmp(c, "release"))
				mcusim_press(&M.sim, id, true);
			if (strcmp(c, "press"))
				mcusim_press(&M.sim, id, false);
		}
	} else if (!strcmp(c, "pot") && argc == 3) {
		mcusim_pot(&M.sim, atoi(argv[1]) - 1, atoi(argv[2]));
	} else if (!strcmp(c, "jog") && argc == 2) {
		mcusim_jog(&M.sim, atoi(argv[1]));
	} else if (!strcmp(c, "hello")) {
		mcusim_hello(&M.sim);
	} else if (!strcmp(c, "raw") && argc >= 2) {
		/* raw HEXBYTES (one message, e.g. b0071f): a generic controller */
		uint8_t b[256];
		size_t n = 0;
		for (int a = 1; a < argc; a++)
			for (const char *h = argv[a]; h[0] && h[1] && n < sizeof(b); h += 2) {
				char t[3] = { h[0], h[1], 0 };
				b[n++] = (uint8_t)strtol(t, NULL, 16);
			}
		/* a run of channel messages goes out one by one */
		for (size_t i = 0; i < n;) {
			int dl = bl_midi_data_len(b[i]);
			size_t len = dl < 0 ? n - i : (size_t)dl + 1;
			if (i + len > n)
				break;
			mcusim_raw(&M.sim, b + i, len);
			i += len;
		}
	} else if (!strcmp(c, "dump") && argc == 2) {
		char tmp[1100];
		snprintf(tmp, sizeof(tmp), "%s.tmp", argv[1]);
		FILE *f = fopen(tmp, "w");
		if (f) {
			char *js = mcusim_state_json(&M.sim);
			fprintf(f, "%s\n", js);
			free(js);
			fclose(f);
			rename(tmp, argv[1]);
		} else {
			ok = false;
		}
	} else {
		ok = false;
	}
	if (!ok) {
		fprintf(stderr, "backline-test-mcu-sim: '%s': not done\n", c);
		return;
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("done %s t=%llu\n", c, (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec);
	fflush(stdout);
}

static void on_stdin(void *data, int fd, uint32_t mask)
{
	char buf[512];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n <= 0) {
		if (n == 0 || (errno != EAGAIN && errno != EINTR))
			pw_main_loop_quit(M.loop);
		return;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == '\n' || M.line_len >= sizeof(M.line) - 1) {
			M.line[M.line_len] = 0;
			command(M.line);
			M.line_len = 0;
		} else {
			M.line[M.line_len++] = buf[i];
		}
	}
}

static void on_signal(void *data, int sig)
{
	pw_main_loop_quit(M.loop);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "model", required_argument, NULL, 'm' },
		{ "serial", required_argument, NULL, 's' },
		{ "name", required_argument, NULL, 'n' },
		{ "alias", required_argument, NULL, 'a' },
		{ "node-group", required_argument, NULL, 'g' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *model = "xtouch", *serial = "SIM0001";
	snprintf(M.name, sizeof(M.name), "backline-test.mcu-sim");
	int c;
	while ((c = getopt_long(argc, argv, "m:s:n:a:g:h", opts, NULL)) != -1) {
		switch (c) {
		case 'm': model = optarg; break;
		case 's': serial = optarg; break;
		case 'n': snprintf(M.name, sizeof(M.name), "%s", optarg); break;
		case 'a': snprintf(M.alias, sizeof(M.alias), "%s", optarg); break;
		case 'g': snprintf(M.group, sizeof(M.group), "%s", optarg); break;
		case 'h':
			puts("usage: backline-test-mcu-sim [--model xtouch|xtouch-ext|mcu-pro|mcu-xt] [--serial S] [--name NODE]\n"
			     "                             [--alias ALIAS] [--node-group GROUP]   (commands on stdin)");
			return 0;
		default: return 2;
		}
	}
	if (!M.alias[0])
		snprintf(M.alias, sizeof(M.alias), "%s:%s MIDI 1", !strcmp(model, "xtouch-ext") ? "X-Touch-Ext" : !strcmp(model, "xtouch") ? "X-Touch"
				   : !strcmp(model, "mcu-xt") ? "MCU XT Pro" : "MCU Pro", !strcmp(model, "xtouch-ext") ? "X-Touch-Ext" : !strcmp(model, "xtouch") ? "X-Touch"
				   : !strcmp(model, "mcu-xt") ? "MCU XT Pro" : "MCU Pro");
	if (mcusim_init(&M.sim, model, serial, out_cb, NULL) < 0) {
		fprintf(stderr, "backline-test-mcu-sim: model is xtouch, xtouch-ext, mcu-pro or mcu-xt\n");
		return 2;
	}
	bl_midi_ring_init(&M.rx, 1 << 16);
	bl_midi_ring_init(&M.tx, 1 << 15);
	bl_midi_parser_init(&M.parser);
	pw_init(&argc, &argv);
	M.loop = pw_main_loop_new(NULL);
	M.l = pw_main_loop_get_loop(M.loop);
	pw_loop_add_signal(M.l, SIGINT, on_signal, NULL);
	pw_loop_add_signal(M.l, SIGTERM, on_signal, NULL);
	M.context = pw_context_new(M.l, NULL, 0);
	M.core = pw_context_connect(M.context, NULL, 0);
	if (!M.core) {
		fprintf(stderr, "backline-test-mcu-sim: cannot connect to PipeWire\n");
		return 1;
	}
	M.rx_ev = pw_loop_add_event(M.l, on_rx, NULL);
	M.stdin_src = pw_loop_add_io(M.l, 0, SPA_IO_IN | SPA_IO_HUP, false, on_stdin, NULL);
	plug();
	pw_main_loop_run(M.loop);
	unplug();
	pw_core_disconnect(M.core);
	pw_context_destroy(M.context);
	pw_main_loop_destroy(M.loop);
	pw_deinit();
	return 0;
}
