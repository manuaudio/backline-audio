/* backline recorder engine: one multitrack take as one PipeWire node.
 * SPDX-License-Identifier: MIT
 *
 * Used by the one-shot recorder (backline-recorder) and the daemon
 * (backline-recorderd). Every function here is main-thread only: the engine
 * owns a data thread (the filter's process callback) and a disk thread (the
 * writer), and talks to both through atomics and lock-free rings.
 *
 * Lifecycle: rec_session_new() creates the node and links it; `armed` fires
 * once every link is up. rec_session_record() starts the take (with any
 * pre-roll already in the rings), rec_session_stop() stops it on a cycle
 * boundary. `finished` fires once the disk thread has written everything and
 * the sidecar is on disk; then rec_session_destroy().
 */
#ifndef BACKLINE_REC_ENGINE_H
#define BACKLINE_REC_ENGINE_H

#include <stdbool.h>
#include <stdint.h>
#include <pipewire/pipewire.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REC_MAX_TRACKS 32
#define REC_MAX_CH 16
#define REC_MAX_MARKERS 1024

enum rec_latency_mode {
	REC_LATENCY_OFF,       /* no input-latency compensation */
	REC_LATENCY_HARDWARE,  /* physical capture ports: their reported capture latency */
	REC_LATENCY_REPORTED,  /* every source: its reported capture latency */
};

struct rec_source {
	char node[256];
	int channels;            /* 1..REC_MAX_CH */
	int64_t latency_frames;  /* manual capture latency; < 0 = by the latency mode */
};

struct rec_options {
	char dir[512];
	char take[128];
	char format[16];         /* wav (float, RF64 past 4 GB), wav24, flac */
	double duration;         /* seconds; 0 = until stopped */
	bool align_signal;
	double pre_roll;         /* seconds */
	bool wait_record;        /* start only on rec_session_record (else at once, unless pre-roll) */
	double ring_seconds;     /* per-track ring, seconds at the graph rate */
	enum rec_latency_mode latency;
};
void rec_options_defaults(struct rec_options *o);

enum rec_state {
	REC_ARMING,      /* node up, waiting for links */
	REC_ARMED,       /* standing by (pre-roll is being kept) */
	REC_RECORDING,
	REC_FINISHING,   /* stopped; the disk thread is writing the rest */
	REC_DONE,        /* files and sidecar complete (or disarmed) */
};

struct rec_events {
	void (*armed)(void *data);
	void (*started)(void *data);
	/* sidecar is NULL when the session ended without a take (disarmed) */
	void (*finished)(void *data, const char *sidecar);
};

struct rec_session;

struct rec_session *rec_session_new(struct pw_core *core, struct pw_loop *loop,
		const struct rec_source *sources, int n_sources, const struct rec_options *opts,
		const struct rec_events *events, void *data, char *err, size_t err_size);
void rec_session_destroy(struct rec_session *s);

int rec_session_record(struct rec_session *s);
/* Recording: stop at the next cycle. Armed: disarm (finished(NULL) follows). */
void rec_session_stop(struct rec_session *s);
/* A marker at the current take frame (accurate to the cycle). */
int rec_session_marker(struct rec_session *s, const char *name, uint64_t *frame);

enum rec_state rec_session_state(const struct rec_session *s);
const char *rec_session_take(const struct rec_session *s);
uint32_t rec_session_rate(const struct rec_session *s);
uint64_t rec_session_frames(const struct rec_session *s);
uint64_t rec_session_overruns(const struct rec_session *s);
int rec_session_n_tracks(const struct rec_session *s);
const char *rec_session_track_node(const struct rec_session *s, int track);
/* Input-latency compensation applied to a track, in frames (valid once armed). */
int64_t rec_session_track_latency(const struct rec_session *s, int track);
/* Per-track peak in dBFS since the last call (−200 for silence); resets. */
void rec_session_take_peaks(struct rec_session *s, double *peaks_db, int n);
/* The node id of the session's own filter (SPA_ID_INVALID until known). */
uint32_t rec_session_node_id(const struct rec_session *s);

/* Append a WAV `cue ` chunk and a LIST/adtl chunk with labels to a finished
 * WAV, WAVE_FORMAT_EXTENSIBLE or RF64 file. Pure file I/O, unit-tested. */
struct rec_marker { char name[128]; uint64_t frame; uint64_t realtime_ns; };
int rec_write_cues(const char *path, const struct rec_marker *m, int n);

#ifdef __cplusplus
}
#endif
#endif
