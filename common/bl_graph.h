/* Backline: a small live view of the PipeWire graph, plus owned links.
 * SPDX-License-Identifier: MIT
 *
 * Every Backline service needs the same two things from the graph: "tell me
 * about ports as they come and go" (hotplug: a USB keyboard appears, a device
 * returns) and "link these two ports, and let the link die with me". This is
 * that, on top of the registry. Main-thread only; never touch it from a
 * process callback.
 */
#ifndef BACKLINE_GRAPH_H
#define BACKLINE_GRAPH_H

#include <stdbool.h>
#include <stdint.h>
#include <pipewire/pipewire.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bl_port {
	uint32_t id;
	uint32_t node_id;
	bool output;          /* port.direction == out */
	bool monitor;         /* port.monitor */
	bool physical;        /* port.physical */
	bool midi;            /* format.dsp is raw MIDI or UMP */
	char name[128];       /* port.name */
	char alias[256];      /* port.alias */
	char channel[16];     /* audio.channel */
	char node_name[256];  /* node.name of the owning node, if known */
};

struct bl_node {
	uint32_t id;
	char name[256];
	char media_class[64];
};

struct bl_graph_events {
	void (*port_added)(void *data, const struct bl_port *port);
	void (*port_removed)(void *data, uint32_t id);
};

struct bl_graph;

struct bl_graph *bl_graph_new(struct pw_core *core,
		const struct bl_graph_events *events, void *data);
void bl_graph_destroy(struct bl_graph *g);

const struct bl_node *bl_graph_find_node(struct bl_graph *g, const char *name);
const struct bl_node *bl_graph_node(struct bl_graph *g, uint32_t id);

/* Iterate ports (stops when cb returns non-zero). */
int bl_graph_for_each_port(struct bl_graph *g,
		int (*cb)(void *data, const struct bl_port *port), void *data);

/* Create a link owned by this client (object.linger = false): it goes away
 * when the link is destroyed, the ports go, or this process exits or crashes.
 * Duplicate requests for the same pair are ignored. Returns 0 or -errno. */
int bl_graph_link(struct bl_graph *g, uint32_t out_port, uint32_t in_port);
bool bl_graph_is_linked(struct bl_graph *g, uint32_t out_port, uint32_t in_port);
/* Number of links created by bl_graph_link that the server has confirmed. */
int bl_graph_links_ready(struct bl_graph *g);

#ifdef __cplusplus
}
#endif
#endif
