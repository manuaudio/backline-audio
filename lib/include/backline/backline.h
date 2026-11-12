/* libbackline: the C client library for the Backline audio services.
 * SPDX-License-Identifier: MIT
 *
 * A thin, typed wrapper over Backline's D-Bus interfaces on the session bus:
 *
 *   Host1      io.github.manuaudio.Backline.Host       the plugin-host supervisor
 *   Instance1  io.github.manuaudio.Backline.Instance.* one hosted plugin
 *   Registry1  io.github.manuaudio.Backline.Registry   the plugin index
 *
 * It talks only D-Bus. It never loads plugin code, links no GPL code and has
 * no PipeWire dependency, so any application can use it.
 *
 * Conventions: functions return 0 on success or a negative errno. After a
 * failure, bl_client_error() gives a human-readable reason (usually the
 * service's D-Bus error message). Arrays and strings the library allocates
 * are released with bl_free(). A bl_client is not thread-safe; use one per
 * thread.
 *
 * Status: 0.x. The API follows the D-Bus interfaces and may change until they
 * reach 1.0 (see docs/DECISIONS.md).
 */
#ifndef BACKLINE_BACKLINE_H
#define BACKLINE_BACKLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKLINE_VERSION_MAJOR 0
#define BACKLINE_VERSION_MINOR 2
#define BACKLINE_VERSION_MICRO 0

const char *bl_version(void);

typedef struct bl_client bl_client;

/* Connect to the session bus. */
int bl_client_open(bl_client **out);
void bl_client_close(bl_client *c);
/* The reason for the last failure on this client ("" if none). */
const char *bl_client_error(const bl_client *c);
void bl_free(void *p);

/* ------------------------------------------------------------------ Host1 */

/* Options for bl_host_load. Start from BL_LOAD_OPTIONS_INIT; NULL strings and
 * -1 mean "the service's default". */
typedef struct bl_load_options {
	const char *id;          /* instance id, [a-z0-9-] (default: <plugin>-<n>) */
	const char *plugin_id;   /* plugin id inside the file (default: first instrument) */
	const char *name;        /* node.name (default: backline.<id>) */
	const char *target;      /* link the output to this node instead of the default output */
	const char *midi_follow; /* link every MIDI source matching this regex */
	const char *state;       /* start from this CLAP state file */
	const char *memory_max;  /* systemd MemoryMax for the worker (default 4G) */
	const char *cpu_quota;   /* systemd CPUQuota (default none) */
	int autoconnect;         /* -1 default (on unless target), 0, 1 */
	int restart;             /* -1 default (on), 0 = leave it down after a crash */
} bl_load_options;
#define BL_LOAD_OPTIONS_INIT { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, -1, -1 }

typedef struct bl_instance {
	char id[64];
	char plugin[1024];       /* as requested */
	char plugin_path[1024];  /* as resolved by the worker */
	char plugin_id[256];
	char node[256];
	char dbus[256];
	char target[256];
	char midi_follow[512];
	char state[16];          /* starting, running, restarting, stopping, failed */
	uint32_t pid;
	uint32_t osc_port;
	uint32_t restarts;
	int32_t last_signal;
	int32_t last_exit_status;
	bool restart;
	char unit[200];          /* systemd scope of the current worker */
	char state_file[1200];
	char log_file[1200];
	char fail_reason[256];
} bl_instance;

/* Start a plugin instance; returns when its node is up. `id_out` may be NULL. */
int bl_host_load(bl_client *c, const char *plugin, const bl_load_options *opt, char *id_out, size_t id_size);
int bl_host_unload(bl_client *c, const char *id);
int bl_host_restart(bl_client *c, const char *id);
int bl_host_get(bl_client *c, const char *id, bl_instance *out);
/* *out is an array of *n instances; release it with bl_free. */
int bl_host_list(bl_client *c, bl_instance **out, size_t *n);

typedef enum bl_event_type {
	BL_EVENT_LOADED,
	BL_EVENT_UNLOADED,
	BL_EVENT_CRASHED,
	BL_EVENT_RESTARTED,
	BL_EVENT_FAILED,
} bl_event_type;

typedef struct bl_event {
	bl_event_type type;
	const char *name;        /* "Loaded", "Crashed", ... */
	const char *id;
	int32_t signal;          /* Crashed */
	int32_t exit_status;     /* Crashed */
	uint32_t pid;            /* Restarted */
	uint32_t restarts;       /* Restarted */
	const char *reason;      /* Failed */
	uint64_t monotonic_ns;   /* when the client received it (CLOCK_MONOTONIC) */
} bl_event;

/* Return non-zero from the callback to stop watching. */
typedef int (*bl_event_cb)(const bl_event *e, void *userdata);
/* Block and deliver Host1 signals until the callback stops or `timeout_ms`
 * passes (-1: no timeout). Returns 0, or a negative errno. */
int bl_host_watch(bl_client *c, bl_event_cb cb, void *userdata, int timeout_ms);

/* -------------------------------------------------------------- Instance1 */
/* `instance` is a hostd instance id, a node name of any running worker, or a
 * full io.github.manuaudio.Backline.Instance.* bus name. Parameter keys are a
 * CLAP param id, an exact name ("Global Volume"), "Module/Name", or a unique
 * substring. */

typedef struct bl_param {
	uint32_t id;
	char name[256];
	char module[256];
	double value, min, max;
} bl_param;

int bl_instance_params(bl_client *c, const char *instance, bl_param **out, size_t *n);
int bl_instance_get_param(bl_client *c, const char *instance, const char *key, double *value);
int bl_instance_set_param(bl_client *c, const char *instance, const char *key, double value, double *applied);
int bl_instance_note(bl_client *c, const char *instance, bool on, uint8_t channel, uint8_t key, uint8_t velocity);
int bl_instance_save_state(bl_client *c, const char *instance, const char *path);
int bl_instance_load_state(bl_client *c, const char *instance, const char *path);
/* Counters: "Cycles", "MidiEvents", "DroppedEvents", "ReleasedNotes",
 * "StateSerial", "StateSaved", "StateSaves". */
int bl_instance_counter(bl_client *c, const char *instance, const char *name, uint64_t *out);
/* The instance's bus name, resolved as described above. */
int bl_instance_resolve(bl_client *c, const char *instance, char *bus_name, size_t n);

/* -------------------------------------------------------------- Registry1 */
/* Results are JSON (schema backline.plugins/1); release with bl_free.
 * filter_json: {"format": "clap", "kind": "instrument", "grade": "pass",
 * "text": "surge", "id": "..."}; NULL or "" for everything. */
int bl_plugins_query(bl_client *c, const char *filter_json, char **json_out);
int bl_plugins_show(bl_client *c, const char *id_or_name, char **json_out);
int bl_plugins_scan(bl_client *c, bool validate, char **summary_json_out);

/* --------------------------------------------------------------- helpers */
/* The worker's bus name for a node name (non-alphanumerics become '_'). */
void bl_bus_name_for_node(const char *node, char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
