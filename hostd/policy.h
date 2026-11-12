/* backline-hostd: the pure parts of the supervisor (no I/O), unit-tested.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_HOSTD_POLICY_H
#define BACKLINE_HOSTD_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BL_NSEC 1000000000ull
#define BL_RESTART_GIVE_UP UINT64_MAX

/* Restart policy for one instance.
 *
 *  - The first crash after a healthy run restarts at once (delay 0): a synth
 *    that falls over mid-set should be back within a cycle or two.
 *  - Crashes in a row (the worker dies again within `healthy_ns` of starting)
 *    back off: base, 2 x base, 4 x base ... up to max.
 *  - More than `max_crashes` crashes inside `window_ns` gives up: the instance
 *    is marked failed and stays down until it is loaded or restarted again. */
struct bl_restart_policy {
	unsigned max_crashes;   /* default 5 */
	uint64_t window_ns;     /* default 60 s */
	uint64_t healthy_ns;    /* default 10 s: a run this long resets the backoff */
	uint64_t base_ns;       /* default 250 ms */
	uint64_t max_ns;        /* default 5 s */

	/* state */
	uint64_t crash_ns[16];  /* ring of recent crash times */
	unsigned n_crashes;     /* total */
	unsigned consecutive;   /* crashes since the last healthy run */
};

void bl_restart_policy_init(struct bl_restart_policy *p);
/* A crash at `now` after the worker ran for `ran_ns`. Returns the delay before
 * the restart, or BL_RESTART_GIVE_UP. */
uint64_t bl_restart_policy_crash(struct bl_restart_policy *p, uint64_t now, uint64_t ran_ns);
/* Forget the crash history (an explicit Restart or Load). */
void bl_restart_policy_reset(struct bl_restart_policy *p);

/* Instance ids double as systemd unit name parts and D-Bus-safe node names:
 * lower-case [a-z0-9-], no leading/trailing '-', at most `n - 1` chars.
 * "Surge XT" -> "surge-xt". Returns the length (0 if nothing usable). */
size_t bl_slug(const char *in, char *out, size_t n);

/* The worker's D-Bus name for a node name: the same rule the worker uses
 * (io.github.manuaudio.Backline.Instance.<node, non-alnum -> '_'>). */
void bl_instance_bus_name(const char *node, char *out, size_t n);

/* Parse the worker's ready line: {"node": ..., "osc_port": N, "dbus": ..., "pid": N}.
 * Minimal and strict on purpose (the worker writes it). Returns 0 or -1. */
struct bl_ready {
	char node[256];
	char plugin[256];
	char plugin_path[1024];
	char dbus[256];
	unsigned osc_port;
	int pid;
};
int bl_parse_ready(const char *line, struct bl_ready *out);

#ifdef __cplusplus
}
#endif
#endif
