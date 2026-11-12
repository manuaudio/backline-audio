/* SPDX-License-Identifier: MIT */
#include "rec_align.h"

bool rec_align_pairing(const struct rec_align_track *t, int n, bool signal)
{
	if (!signal)
		return false;
	for (int i = 0; i < n; i++)
		if (t[i].sink_monitor)
			return true;
	return false;
}

int rec_align_select(const struct rec_align_track *t, int n, bool pairing, uint32_t driver_node_id,
		     bool *use_current)
{
	int cur = 0;
	for (int i = 0; i < n; i++) {
		use_current[i] = !pairing || (t[i].sink_monitor && t[i].src_node_id == driver_node_id);
		cur += use_current[i];
	}
	return cur;
}
