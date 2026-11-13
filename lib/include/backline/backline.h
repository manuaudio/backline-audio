/* libbackline: the C client library for the Backline audio services.
 * SPDX-License-Identifier: MIT
 *
 * A thin, typed wrapper over Backline's D-Bus interfaces on the session bus:
 *
 *   Host1      io.github.manuaudio.Backline.Host       the plugin-host supervisor
 *   Instance1  io.github.manuaudio.Backline.Instance.* one hosted plugin
 *   Registry1  io.github.manuaudio.Backline.Registry   the plugin index
 *   Recorder1  io.github.manuaudio.Backline.Recorder   the recorder daemon
 *   Clock1     io.github.manuaudio.Backline.Clock      transport, tempo, timecode, Link
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
#define BACKLINE_VERSION_MINOR 4
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
	int force;               /* -1/0: refuse a plugin the registry grades fail; 1: run it anyway */
} bl_load_options;
#define BL_LOAD_OPTIONS_INIT { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, -1, -1, -1 }

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

/* Start a plugin instance; returns when its node is up. `id_out` may be NULL.
 * `plugin`: a .clap file, a .vst3 bundle, an LV2 URI or .lv2 bundle, or a
 * name in the standard directories (CLAP first, then VST3, then LV2). */
int bl_host_load(bl_client *c, const char *plugin, const bl_load_options *opt, char *id_out, size_t id_size);
/* The same, looking only for `format` ("clap", "vst3", "lv2"; NULL = any). */
int bl_host_load_format(bl_client *c, const char *plugin, const char *format, const bl_load_options *opt,
			char *id_out, size_t id_size);
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
/* Presets: what the plugin offers (CLAP preset-discovery, .vstpreset files
 * and VST3 program lists, LV2 pset:Preset). `id` is what load_preset takes;
 * an exact name or a unique part of one works too. */
typedef struct bl_preset {
	char id[1024];
	char name[256];
	char location[1024];     /* a file, or where inside the plugin it lives */
} bl_preset;
int bl_instance_presets(bl_client *c, const char *instance, bl_preset **out, size_t *n);
int bl_instance_load_preset(bl_client *c, const char *instance, const char *id);
/* Saves the current state as a preset file (VST3: .vstpreset in the standard
 * user directory, or at `name_or_path` if it has a '/'). */
int bl_instance_save_preset(bl_client *c, const char *instance, const char *name_or_path, char *path_out, size_t n);
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

/* -------------------------------------------------------------- Recorder1 */
/* docs/api/Recorder1.md. NULL strings and negative numbers mean the
 * service's default. */
typedef struct bl_rec_options {
	const char *dir, *name, *format, *align, *latency;
	double pre_roll, duration, ring_seconds;
} bl_rec_options;
#define BL_REC_OPTIONS_INIT { NULL, NULL, NULL, NULL, NULL, -1, -1, -1 }

typedef struct bl_rec_status {
	char state[16];          /* idle, armed, recording */
	char take[128];
	char soundcheck[128];
	char last_take[1100];
	uint32_t rate;
	uint64_t frames, overruns;
} bl_rec_status;

/* sources: n strings NODE[:CHANNELS]. Returns once armed. */
int bl_recorder_arm(bl_client *c, const char *const *sources, size_t n, const bl_rec_options *opt, char *take, size_t take_size);
int bl_recorder_record(bl_client *c, char *take, size_t take_size);
/* *sidecar (release with bl_free) is "" when it only disarmed. */
int bl_recorder_stop(bl_client *c, char **sidecar);
int bl_recorder_marker(bl_client *c, const char *name, uint64_t *frame);
int bl_recorder_takes(bl_client *c, char **json);
int bl_recorder_take(bl_client *c, const char *take, char **json);
/* loop: -1 default (on), 0, 1. */
int bl_recorder_soundcheck(bl_client *c, const char *take, int loop, uint64_t start);
/* mode: "live" or "playback" */
int bl_recorder_set_input(bl_client *c, const char *node, const char *mode);
/* Per channel (docs/api/Recorder1.md, SetChannelInput): channel is the input's
 * 1-based source channel (0 = every channel); consumer a node.name, or NULL /
 * "" for every consumer. Only that channel's links into that consumer move. */
int bl_recorder_set_channel_input(bl_client *c, const char *node, uint32_t channel, const char *mode,
				  const char *consumer);
typedef struct bl_channel_input {
	const char *node;
	uint32_t channel;
	const char *mode;       /* "live" or "playback" */
	const char *consumer;   /* NULL or "" = every consumer */
} bl_channel_input;
/* Many switches at once: all are checked first, and either all apply, in one
 * pass, or none does. */
int bl_recorder_set_channel_inputs(bl_client *c, const bl_channel_input *v, size_t n);
int bl_recorder_stop_soundcheck(bl_client *c);
int bl_recorder_status(bl_client *c, bl_rec_status *st);

/* ------------------------------------------------------------------ Clock1 */
/* docs/api/Clock1.md. */
typedef struct bl_clock_status {
	bool rolling, link_enabled, locked;
	uint64_t frame;
	uint32_t rate, link_peers;
	double tempo, beats_per_bar, beat_type, fps;
	int64_t chase_error;
	char timecode[32], bbt[32], source[32], ltc_mode[32], mtc_mode[32];
} bl_clock_status;

int bl_clock_play(bl_client *c);
int bl_clock_stop(bl_client *c);
/* position: frames, "tc:HH:MM:SS:FF" or "bbt:BAR|BEAT|TICK" */
int bl_clock_locate(bl_client *c, const char *position, uint64_t *frame);
int bl_clock_set_tempo(bl_client *c, double bpm);
int bl_clock_set_meter(bl_client *c, double beats_per_bar, double beat_type);
int bl_clock_link(bl_client *c, bool enable);
/* ltc true: LTC, else MTC. mode: off, generate, chase. fps <= 0: keep. */
int bl_clock_timecode(bl_client *c, bool ltc, const char *mode, double fps, const char *target, const char *source);
int bl_clock_get(bl_client *c, bl_clock_status *st);

/* ----------------------------------------------------------------- Player1 */
/* docs/api/Player1.md. NULL strings and NAN numbers mean the default. */
typedef struct bl_play_options {
	const char *id;          /* default: from the file name */
	const char *at;          /* timeline position (as bl_clock_locate); default "0" */
	const char *target;      /* node to play into; default: the session manager links it */
	double gain_db;          /* default 0 */
	int autoconnect;         /* -1 default, 0, 1 (ignored with a target) */
} bl_play_options;
#define BL_PLAY_OPTIONS_INIT { NULL, NULL, NULL, 0.0, -1 }

/* file: an absolute path. Returns once the node is up (and linked). */
int bl_player_load(bl_client *c, const char *file, const bl_play_options *opt, char *id, size_t id_size);
int bl_player_unload(bl_client *c, const char *id);
int bl_player_set_gain(bl_client *c, const char *id, double db);
int bl_player_set_start(bl_client *c, const char *id, const char *position, uint64_t *frame);
/* JSON: backline.player/1 (List) and backline.clip/1 (Get); release with bl_free. */
int bl_player_list(bl_client *c, char **json);
int bl_player_get(bl_client *c, const char *id, char **json);

/* ---------------------------------------------------------------- Surface1 */
/* docs/api/Surface1.md: control surfaces (Mackie Control, USB/MIDI or the
 * network). JSON arrays of objects, released with bl_free: the units
 * (Units(): id, name, state, transport, address, group, ...) and the
 * network devices with their sessions (Peers(): name, address, state, unit). */
int bl_surface_units(bl_client *c, char **json);
int bl_surface_peers(bl_client *c, char **json);
/* A network device at an address ("10.0.0.5" or "10.0.0.5:5004"), kept. */
int bl_surface_add_network(bl_client *c, const char *address);
int bl_surface_remove_network(bl_client *c, const char *address);
/* A standby engine holds its sessions and drives nothing until active. */
int bl_surface_set_active(bl_client *c, bool active);
int bl_surface_status(bl_client *c, bool *active, uint16_t *network_port, bool *params_connected);

/* ---------------------------------------------------------------- Control1 */
/* docs/api/Control1.md: OSC and MIDI mapped to any parameter. */
typedef struct bl_map_options {
	double min, max;            /* output range (has_min / has_max) */
	bool has_min, has_max, invert;
	double in_min, in_max;      /* OSC input range (has_in) */
	bool has_in;
	const char *const *args;    /* recorder:arm sources, recorder:marker name */
	size_t n_args;
	const char *id;             /* NULL: the service picks one */
} bl_map_options;
#define BL_MAP_OPTIONS_INIT { 0, 0, false, false, false, 0, 1, false, NULL, 0, NULL }

/* source: midi:cc:CH:NUM, midi:cc14:CH:MSB, midi:nrpn:CH:PARAM,
 * midi:rpn:CH:PARAM, midi:note:CH:KEY, midi:pitchbend:CH, midi:program:CH,
 * osc:/ADDRESS. target: instance:NODE/PARAM,
 * transport:play|stop|toggle|tempo|locate:POS, recorder:arm|record|stop|marker,
 * volume:NODE, mute:NODE, control:NODE/NAME, param:PATH. The newer options
 * (encoding, takeover, feedback, ...) go through bl_control_add_json. */
int bl_control_map(bl_client *c, const char *source, const char *target, const bl_map_options *o, char *id, size_t n);
int bl_control_add_json(bl_client *c, const char *json, char *id, size_t n);
int bl_control_remove(bl_client *c, const char *id);
/* JSON, schema backline.control/1; release with bl_free. */
int bl_control_mappings(bl_client *c, char **json);
/* Learns the next MIDI or OSC input for target and waits for it (timeout_s:
 * 0 = 30 s). from: "midi", "osc" or NULL for either. Returns -ETIMEDOUT
 * if nothing came. */
int bl_control_learn(bl_client *c, const char *target, const bl_map_options *o, const char *from, unsigned timeout_s,
		     char *id, size_t id_size, char *source, size_t source_size);
int bl_control_cancel_learn(bl_client *c);

typedef struct bl_listen_options {
	const char *const *allow;   /* CIDRs; required for a non-loopback address */
	size_t n_allow;
	const char *secret;         /* TCP: required for a non-loopback address */
	bool insecure_udp;          /* UDP on a non-loopback address */
	unsigned max_rate, max_clients, max_size; /* 0: the defaults */
} bl_listen_options;
#define BL_LISTEN_OPTIONS_INIT { NULL, 0, NULL, false, 0, 0, 0 }
/* proto "udp" or "tcp"; address a literal IP; port 0 picks one. id is
 * "udp:127.0.0.1:9000". */
int bl_control_listen(bl_client *c, const char *proto, const char *address, uint16_t port, const bl_listen_options *o,
		      char *id, size_t n);
/* The same, also serving Params1 over OSC to its peers in params_role
 * ("all", "read-only" or a role the program defined; NULL: not served). */
int bl_control_listen_params(bl_client *c, const char *proto, const char *address, uint16_t port,
			     const bl_listen_options *o, const char *params_role, char *id, size_t n);
int bl_control_unlisten(bl_client *c, const char *id);
int bl_control_set_midi_follow(bl_client *c, const char *regex);
/* Link controld's MIDI output (feedback) to matching MIDI inputs ('' = none). */
int bl_control_set_midi_out(bl_client *c, const char *regex);
int bl_control_save(bl_client *c, char *path, size_t n);

typedef struct bl_control_status {
	uint32_t mappings;
	char learning[600], midi_follow[256], node[128], config_dir[600];
	char listeners[8][128];
	size_t n_listeners;
	uint64_t received, midi_events, rejected, auth_failures, malformed, dispatched, target_errors;
} bl_control_status;
int bl_control_get_status(bl_client *c, bl_control_status *st);

/* --------------------------------------------------------------- helpers */
/* The worker's bus name for a node name (non-alphanumerics become '_'). */
void bl_bus_name_for_node(const char *node, char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
