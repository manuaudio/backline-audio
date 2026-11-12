/* Backline registry: run probes in parallel, each in its own sandbox.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_RUNNER_H
#define BACKLINE_RUNNER_H

#include <stdbool.h>
#include <json-c/json.h>

#define JOB_MAX_ARGS 8

struct job {
	const char *argv[JOB_MAX_ARGS]; /* probe arguments (argv[0] is the mode) */
	const char *env_lv2_path;       /* LV2_PATH for the probe, or NULL */
	void *user;                     /* caller's tag */
	/* results */
	json_object *result;            /* owned: the probe's report or a synthesized one */
	double ms;                      /* wall time */
	int cwd_files;                  /* files the plugin left in its working directory */
	bool cacheable;                 /* false for timeouts and infrastructure errors */
};

struct runner_opts {
	const char *probe;      /* path to backline-probe */
	int jobs;               /* parallel probes */
	double timeout;         /* seconds per probe */
	const char *memory_max; /* e.g. "2G" */
	int tasks_max;
	bool sandbox;           /* systemd user scope per probe (falls back if unavailable) */
};

struct runner_stats {
	int run, crashed, timed_out, errors;
	const char *sandbox; /* "systemd-scope" or "fork" */
	int leftover_units;  /* scopes still alive after their probe (should stay 0) */
};

/* True if `systemd-run --user --scope` works here. */
bool runner_scope_available(void);
void runner_run(struct job *jobs, int n, const struct runner_opts *o, struct runner_stats *st);

#endif
