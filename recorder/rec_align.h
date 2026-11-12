/* backline-recorder: per-cycle signal alignment (pure, unit-tested).
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_REC_ALIGN_H
#define BACKLINE_REC_ALIGN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PipeWire's scheduling, measured in docs/proofs/recorder-proof.txt: the
 * driver runs first in every cycle, before the nodes that feed it. So a sink
 * that drives cycle k carries on its monitor what it was fed in cycle k-1,
 * while a follower sink's monitor carries cycle k. The offset is one cycle,
 * whatever the quantum is, and it moves with the driver.
 *
 * In signal mode the recorder therefore writes, in cycle k, the driver
 * sink's monitor as it arrives and every other track as it was in cycle k-1
 * (a one-cycle copy). Frame j of every track is then the same input sample,
 * through quantum changes and driver changes. The take's timestamps refer to
 * cycle k-1.
 *
 * (The ports' reported SPA_PARAM_Latency cannot decide this: a null sink
 * reports 1 quantum of playback latency whether it drives or follows, and 0
 * until something feeds it. The recorder logs it in the sidecar instead.) */

struct rec_align_track {
	bool sink_monitor;     /* the track records a sink's monitor ports */
	uint32_t src_node_id;  /* the recorded node */
};

/* Does this set of tracks need the one-cycle pairing at all? Only in signal
 * mode, and only if some track is a sink's monitor (which may drive). */
bool rec_align_pairing(const struct rec_align_track *t, int n, bool signal);

/* For cycle with driver `driver_node_id`: use_current[i] = true when track i
 * is written from this cycle's buffers (the driver sink's monitor, or every
 * track when pairing is off), false when from the previous cycle's copy.
 * Returns the number of tracks written from the current cycle. */
int rec_align_select(const struct rec_align_track *t, int n, bool pairing, uint32_t driver_node_id,
		     bool *use_current);

#ifdef __cplusplus
}
#endif
#endif
