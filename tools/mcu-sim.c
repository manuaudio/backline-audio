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
 *                      (on the network: the power switch, sessions forgotten)
 *   netdrop | netback  the network cable: nothing in or out / back (the device stays on)
 *   dump PATH          the device's state as JSON to PATH (written atomically)
 *   quit
 *
 * Throwaway: nothing is linked by this tool; whoever wants the device links
 * to it, as with a real one.
 *
 * On the network (--net PORT) it is an X-Touch with Interface "Network": no
 * PipeWire node, an RTP-MIDI device on UDP PORT and PORT+1. As in the
 * X-Touch's "RTP SLAVE" mode it answers invitations, up to --sessions at once
 * (more get NO); with --invite HOST:PORT it is in "RTP MASTER" mode as well
 * and invites that host itself, again and again until it answers. Its
 * session name is --session-name (default: the model's, X-Touch or
 * X-Touch-Ext). Everything it sends goes to every open session.
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "bl_midibuf.h"
#include "bl_midi_pw.h"
#include "mcusim.h"
#include "rtpmidi.h"

#define NET_SESSIONS 4

struct nsess {
	bool used;
	struct rtpm_session s;
	struct sockaddr_in ctrl, data;
};

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
	/* the network */
	bool net, cut, powered;
	uint16_t net_port;
	int fd[2];
	struct spa_source *net_src[2], *net_tick;
	struct nsess sess[NET_SESSIONS];  /* [0]: our own invitation (RTP MASTER) */
	int max_sessions;
	bool invite;
	struct sockaddr_in invite_to;
	char sname[64];
	uint32_t seed;
} M;

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- the network side */
static bool sess_busy(const struct nsess *x)
{
	return x->used && (x->s.state == RTPM_OPEN || x->s.state == RTPM_INVITING_DATA);
}

static int n_busy(void)
{
	int n = 0;
	for (int i = 0; i < NET_SESSIONS; i++)
		n += sess_busy(&M.sess[i]);
	return n;
}

static void ns_send(void *data, enum rtpm_port port, const uint8_t *p, size_t n)
{
	struct nsess *x = data;
	if (M.cut || !M.powered)
		return;
	const struct sockaddr_in *to = port == RTPM_CONTROL ? &x->ctrl : &x->data;
	(void)sendto(M.fd[port], p, n, 0, (const struct sockaddr *)to, sizeof(*to));
}

static void ns_midi(void *data, const uint8_t *m, size_t n)
{
	if (m[0] < 0xf8)
		mcusim_receive(&M.sim, m, n);
}

static void ns_state(void *data, enum rtpm_state old, enum rtpm_state st)
{
	struct nsess *x = data;
	char ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &x->ctrl.sin_addr, ip, sizeof(ip));
	printf("session %s:%u %s (%s)\n", ip, ntohs(x->ctrl.sin_port), rtpm_state_name(st), x->s.peer_name);
	fflush(stdout);
}

static const struct rtpm_events ns_events = { ns_send, ns_midi, ns_state };

static uint32_t ssrc(void)
{
	M.seed = M.seed * 1664525u + 1013904223u;
	return M.seed;
}

static void net_out(const uint8_t *m, size_t n)
{
	uint64_t now = mono_ns();
	for (int i = 0; i < NET_SESSIONS; i++)
		if (M.sess[i].used)
			rtpm_send(&M.sess[i].s, m, n, now);
}

static void net_flush(void)
{
	uint64_t now = mono_ns();
	for (int i = 0; i < NET_SESSIONS; i++)
		if (M.sess[i].used)
			rtpm_flush(&M.sess[i].s, now);
}

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void net_packet(int port, const uint8_t *b, size_t n, const struct sockaddr_in *from)
{
	uint64_t now = mono_ns();
	for (int i = 0; i < NET_SESSIONS; i++) {
		struct nsess *x = &M.sess[i];
		if (x->used && same_addr(from, port == RTPM_CONTROL ? &x->ctrl : &x->data)) {
			rtpm_input(&x->s, (enum rtpm_port)port, b, n, now);
			return;
		}
	}
	struct rtpm_cmd c;
	if (!rtpm_parse_command(b, n, &c) || c.kind != RTPM_CMD_IN)
		return;
	if (port == RTPM_DATA) {
		for (int i = 1; i < NET_SESSIONS; i++) {
			struct nsess *x = &M.sess[i];
			if (x->used && x->ctrl.sin_addr.s_addr == from->sin_addr.s_addr && x->s.peer_ssrc == c.ssrc) {
				x->data = *from;
				rtpm_input(&x->s, RTPM_DATA, b, n, now);
				return;
			}
		}
		return;
	}
	struct nsess *x = NULL;
	if (n_busy() < M.max_sessions)
		for (int i = 1; i < NET_SESSIONS && !x; i++)
			if (!M.sess[i].used)
				x = &M.sess[i];
	if (!x) { /* no free session: NO, as a device with its sessions taken */
		uint8_t no[96];
		size_t k = rtpm_build_session(RTPM_CMD_NO, c.token, ssrc(), M.sname, no, sizeof(no));
		(void)sendto(M.fd[RTPM_CONTROL], no, k, 0, (const struct sockaddr *)from, sizeof(*from));
		printf("refused %s (sessions taken: %d)\n", c.name, M.max_sessions);
		fflush(stdout);
		return;
	}
	memset(x, 0, sizeof(*x));
	x->used = true;
	x->ctrl = x->data = *from;
	x->data.sin_port = htons((uint16_t)(ntohs(from->sin_port) + 1));
	rtpm_init(&x->s, RTPM_RESPONDER, M.sname, ssrc(), now, &ns_events, x);
	rtpm_input(&x->s, RTPM_CONTROL, b, n, now);
}

static void on_net(void *data, int fd, uint32_t mask)
{
	int port = fd == M.fd[RTPM_DATA] ? RTPM_DATA : RTPM_CONTROL;
	uint8_t b[2048];
	for (int k = 0; k < 64; k++) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(fd, b, sizeof(b), 0, (struct sockaddr *)&from, &fl);
		if (n <= 0)
			break;
		if (!M.cut && M.powered)
			net_packet(port, b, (size_t)n, &from);
	}
}

static void on_net_tick(void *data, uint64_t exp)
{
	if (!M.powered)
		return;
	uint64_t now = mono_ns();
	for (int i = 0; i < NET_SESSIONS; i++) {
		struct nsess *x = &M.sess[i];
		if (!x->used)
			continue;
		/* our own invitation waits while every session is taken */
		if (i == 0 && !sess_busy(x) && n_busy() >= M.max_sessions)
			continue;
		rtpm_tick(&x->s, now);
		if (i > 0 && (x->s.state == RTPM_LOST || x->s.state == RTPM_CLOSED))
			x->used = false;
	}
}

static void net_power(bool on)
{
	uint64_t now = mono_ns();
	if (!on) {
		M.powered = false;
		memset(M.sess, 0, sizeof(M.sess));
		for (int i = 0; i < 9; i++)
			M.sim.touched[i] = false;
		printf("unplugged %s\n", M.sname);
		fflush(stdout);
		return;
	}
	mcusim_reset(&M.sim);
	M.powered = true;
	if (M.invite) {
		struct nsess *x = &M.sess[0];
		memset(x, 0, sizeof(*x));
		x->used = true;
		x->ctrl = M.invite_to;
		x->data = M.invite_to;
		x->data.sin_port = htons((uint16_t)(ntohs(M.invite_to.sin_port) + 1));
		rtpm_init(&x->s, RTPM_INITIATOR, M.sname, ssrc(), now, &ns_events, x);
		rtpm_start(&x->s, now);
	}
	printf("plugged %s (network, UDP %u)\n", M.sname, M.net_port);
	fflush(stdout);
}

static int net_bind(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "backline-test-mcu-sim: UDP %u: %s\n", port, strerror(errno));
		exit(1);
	}
	return fd;
}

static void out_cb(void *data, const uint8_t *m, size_t n)
{
	if (M.net)
		net_out(m, n);
	else
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
	if (M.net) {
		if (!M.powered)
			net_power(true);
		return;
	}
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
	if (M.net) {
		if (M.powered)
			net_power(false);
		return;
	}
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
	} else if (!strcmp(c, "netdrop") && M.net) {
		M.cut = true;
	} else if (!strcmp(c, "netback") && M.net) {
		M.cut = false;
	} else if ((M.net ? !M.powered : !M.filter) && strcmp(c, "dump")) {
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
	if (M.net)
		net_flush();
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
		{ "net", required_argument, NULL, 'N' },
		{ "sessions", required_argument, NULL, 'k' },
		{ "invite", required_argument, NULL, 'I' },
		{ "session-name", required_argument, NULL, 'S' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *model = "xtouch", *serial = "SIM0001";
	snprintf(M.name, sizeof(M.name), "backline-test.mcu-sim");
	int c;
	M.max_sessions = 1;
	M.fd[0] = M.fd[1] = -1;
	while ((c = getopt_long(argc, argv, "m:s:n:a:g:N:k:I:S:h", opts, NULL)) != -1) {
		switch (c) {
		case 'm': model = optarg; break;
		case 's': serial = optarg; break;
		case 'n': snprintf(M.name, sizeof(M.name), "%s", optarg); break;
		case 'a': snprintf(M.alias, sizeof(M.alias), "%s", optarg); break;
		case 'g': snprintf(M.group, sizeof(M.group), "%s", optarg); break;
		case 'N': M.net = true; M.net_port = (uint16_t)atoi(optarg); break;
		case 'k': M.max_sessions = atoi(optarg) < 1 ? 1 : atoi(optarg) > NET_SESSIONS - 1 ? NET_SESSIONS - 1 : atoi(optarg); break;
		case 'S': snprintf(M.sname, sizeof(M.sname), "%s", optarg); break;
		case 'I': {
			char host[64];
			snprintf(host, sizeof(host), "%s", optarg);
			char *colon = strrchr(host, ':');
			M.invite_to.sin_family = AF_INET;
			M.invite_to.sin_port = htons(colon ? (uint16_t)atoi(colon + 1) : 5004);
			if (colon)
				*colon = 0;
			if (inet_pton(AF_INET, host, &M.invite_to.sin_addr) != 1) {
				fprintf(stderr, "backline-test-mcu-sim: --invite IPV4[:PORT]\n");
				return 2;
			}
			M.invite = true;
			break;
		}
		case 'h':
			puts("usage: backline-test-mcu-sim [--model xtouch|xtouch-ext|mcu-pro|mcu-xt] [--serial S] [--name NODE]\n"
			     "                             [--alias ALIAS] [--node-group GROUP]   (commands on stdin)\n"
			     "       on the network: --net PORT [--sessions N] [--invite HOST[:PORT]] [--session-name NAME]");
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
	if (M.net) {
		/* no PipeWire: a device on the network */
		if (!M.sname[0])
			snprintf(M.sname, sizeof(M.sname), "%s", !strcmp(model, "xtouch-ext") ? "X-Touch-Ext" : !strcmp(model, "xtouch") ? "X-Touch"
				 : !strcmp(model, "mcu-xt") ? "MCU XT Pro" : "MCU Pro");
		M.seed = (uint32_t)mono_ns() ^ (uint32_t)getpid();
		M.fd[0] = net_bind(M.net_port);
		M.fd[1] = net_bind((uint16_t)(M.net_port + 1));
		for (int i = 0; i < 2; i++)
			M.net_src[i] = pw_loop_add_io(M.l, M.fd[i], SPA_IO_IN, false, on_net, NULL);
		M.net_tick = pw_loop_add_timer(M.l, on_net_tick, NULL);
		struct timespec t5 = { 0, 5 * 1000000L };
		pw_loop_update_timer(M.l, M.net_tick, &t5, &t5, false);
		M.stdin_src = pw_loop_add_io(M.l, 0, SPA_IO_IN | SPA_IO_HUP, false, on_stdin, NULL);
		plug();
		pw_main_loop_run(M.loop);
		for (int i = 0; i < NET_SESSIONS; i++)
			if (M.sess[i].used)
				rtpm_bye(&M.sess[i].s);
		pw_main_loop_destroy(M.loop);
		pw_deinit();
		return 0;
	}
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
