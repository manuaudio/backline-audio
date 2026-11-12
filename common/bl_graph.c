/* SPDX-License-Identifier: MIT */
#include "bl_graph.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <spa/utils/list.h>
#include <spa/utils/string.h>
#include <pipewire/extensions/metadata.h>

struct gport {
	struct spa_list link;
	struct bl_port info;
};

struct gnode {
	struct spa_list link;
	struct bl_node info;
};

struct glink {
	struct spa_list link;
	struct bl_graph *g;
	uint32_t out, in;
	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	bool ready;
};

struct bl_graph {
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	const struct bl_graph_events *events;
	void *data;
	struct spa_list ports;
	struct spa_list nodes;
	struct spa_list links;
};

static void copy(char *dst, size_t n, const char *src)
{
	snprintf(dst, n, "%s", src ? src : "");
}

static void on_global(void *data, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version, const struct spa_dict *props)
{
	struct bl_graph *g = data;
	(void)permissions; (void)version;
	if (props == NULL)
		return;

	if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		struct gnode *n = calloc(1, sizeof(*n));
		if (!n)
			return;
		n->info.id = id;
		copy(n->info.name, sizeof(n->info.name), spa_dict_lookup(props, PW_KEY_NODE_NAME));
		copy(n->info.media_class, sizeof(n->info.media_class), spa_dict_lookup(props, PW_KEY_MEDIA_CLASS));
		spa_list_append(&g->nodes, &n->link);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
		struct gport *p = calloc(1, sizeof(*p));
		const char *s;
		if (!p)
			return;
		p->info.id = id;
		if ((s = spa_dict_lookup(props, PW_KEY_NODE_ID)))
			p->info.node_id = (uint32_t)atoi(s);
		p->info.output = spa_streq(spa_dict_lookup(props, PW_KEY_PORT_DIRECTION), "out");
		p->info.monitor = spa_atob(spa_dict_lookup(props, PW_KEY_PORT_MONITOR));
		p->info.physical = spa_atob(spa_dict_lookup(props, PW_KEY_PORT_PHYSICAL));
		s = spa_dict_lookup(props, PW_KEY_FORMAT_DSP);
		p->info.midi = s && (strstr(s, "midi") || strstr(s, "UMP"));
		copy(p->info.name, sizeof(p->info.name), spa_dict_lookup(props, PW_KEY_PORT_NAME));
		copy(p->info.alias, sizeof(p->info.alias), spa_dict_lookup(props, PW_KEY_PORT_ALIAS));
		copy(p->info.channel, sizeof(p->info.channel), spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL));
		const struct bl_node *n = bl_graph_node(g, p->info.node_id);
		if (n)
			copy(p->info.node_name, sizeof(p->info.node_name), n->name);
		spa_list_append(&g->ports, &p->link);
		if (g->events && g->events->port_added)
			g->events->port_added(g->data, &p->info);
	}
}

static void on_global_remove(void *data, uint32_t id)
{
	struct bl_graph *g = data;
	struct gport *p;
	struct gnode *n;

	spa_list_for_each(p, &g->ports, link) {
		if (p->info.id == id) {
			spa_list_remove(&p->link);
			if (g->events && g->events->port_removed)
				g->events->port_removed(g->data, id);
			free(p);
			return;
		}
	}
	spa_list_for_each(n, &g->nodes, link) {
		if (n->info.id == id) {
			spa_list_remove(&n->link);
			free(n);
			return;
		}
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
	.global_remove = on_global_remove,
};

struct bl_graph *bl_graph_new(struct pw_core *core,
		const struct bl_graph_events *events, void *data)
{
	struct bl_graph *g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->core = core;
	g->events = events;
	g->data = data;
	spa_list_init(&g->ports);
	spa_list_init(&g->nodes);
	spa_list_init(&g->links);
	g->registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(g->registry, &g->registry_listener, &registry_events, g);
	return g;
}

static void link_free(struct glink *l)
{
	spa_list_remove(&l->link);
	if (l->proxy) {
		spa_hook_remove(&l->proxy_listener);
		pw_proxy_destroy(l->proxy);
	}
	free(l);
}

void bl_graph_destroy(struct bl_graph *g)
{
	struct gport *p, *tp;
	struct gnode *n, *tn;
	struct glink *l, *tl;
	if (!g)
		return;
	spa_list_for_each_safe(l, tl, &g->links, link)
		link_free(l);
	spa_list_for_each_safe(p, tp, &g->ports, link)
		free(p);
	spa_list_for_each_safe(n, tn, &g->nodes, link)
		free(n);
	spa_hook_remove(&g->registry_listener);
	pw_proxy_destroy((struct pw_proxy *)g->registry);
	free(g);
}

const struct bl_node *bl_graph_find_node(struct bl_graph *g, const char *name)
{
	struct gnode *n;
	spa_list_for_each(n, &g->nodes, link)
		if (spa_streq(n->info.name, name))
			return &n->info;
	return NULL;
}

const struct bl_node *bl_graph_node(struct bl_graph *g, uint32_t id)
{
	struct gnode *n;
	spa_list_for_each(n, &g->nodes, link)
		if (n->info.id == id)
			return &n->info;
	return NULL;
}

int bl_graph_for_each_port(struct bl_graph *g,
		int (*cb)(void *data, const struct bl_port *port), void *data)
{
	struct gport *p, *t;
	int r;
	spa_list_for_each_safe(p, t, &g->ports, link)
		if ((r = cb(data, &p->info)) != 0)
			return r;
	return 0;
}

static void link_bound(void *data, uint32_t global_id)
{
	struct glink *l = data;
	(void)global_id;
	l->ready = true;
}

static void link_removed(void *data)
{
	struct glink *l = data;
	/* The server removed the link (a port went away). Forget it, so the
	 * same pair can be linked again when the port comes back. */
	link_free(l);
}

static void link_error(void *data, int seq, int res, const char *message)
{
	struct glink *l = data;
	(void)seq;
	pw_log_warn("link %u -> %u failed: %s (%d)", l->out, l->in, message, res);
}

static const struct pw_proxy_events link_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.bound = link_bound,
	.removed = link_removed,
	.error = link_error,
};

bool bl_graph_is_linked(struct bl_graph *g, uint32_t out_port, uint32_t in_port)
{
	struct glink *l;
	spa_list_for_each(l, &g->links, link)
		if (l->out == out_port && l->in == in_port)
			return true;
	return false;
}

int bl_graph_links_ready(struct bl_graph *g)
{
	struct glink *l;
	int n = 0;
	spa_list_for_each(l, &g->links, link)
		n += l->ready;
	return n;
}

int bl_graph_link(struct bl_graph *g, uint32_t out_port, uint32_t in_port)
{
	struct glink *l;
	char o[16], i[16];

	if (bl_graph_is_linked(g, out_port, in_port))
		return 0;
	l = calloc(1, sizeof(*l));
	if (!l)
		return -ENOMEM;
	snprintf(o, sizeof(o), "%u", out_port);
	snprintf(i, sizeof(i), "%u", in_port);
	struct pw_properties *props = pw_properties_new(
			PW_KEY_LINK_OUTPUT_PORT, o,
			PW_KEY_LINK_INPUT_PORT, i,
			PW_KEY_OBJECT_LINGER, "false",
			NULL);
	l->g = g;
	l->out = out_port;
	l->in = in_port;
	l->proxy = pw_core_create_object(g->core, "link-factory",
			PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0);
	pw_properties_free(props);
	if (!l->proxy) {
		free(l);
		return -errno;
	}
	pw_proxy_add_listener(l->proxy, &l->proxy_listener, &link_proxy_events, l);
	spa_list_append(&g->links, &l->link);
	return 0;
}
