/* backline-probe: load ONE plugin binary (or one LV2 plugin) and report on it.
 * SPDX-License-Identifier: MIT
 *
 * The registry runs one probe process per binary or plugin, inside its own
 * cgroup scope with a time and memory limit, so a plugin that crashes, hangs
 * or leaves children behind while being scanned costs only its probe.
 *
 *   backline-probe clap-list FILE        CLAP descriptors (factory only)
 *   backline-probe clap-check FILE ID    create, activate, process, state round-trip
 *   backline-probe vst3-list BUNDLE      load the module, list its audio classes
 *   backline-probe lv2-list              every LV2 plugin via lilv (TTL only, no code)
 *   backline-probe lv2-check URI [BUNDLE] instantiate, run, check outputs
 *
 * Output: exactly one JSON object on the original stdout. Before any plugin
 * code is loaded, stdout is moved to a private descriptor and fd 1 is pointed
 * at stderr, so whatever a plugin prints can't corrupt the report.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <clap/clap.h>

#ifdef BL_HAVE_LILV
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#endif

#define PROBE_VERSION "1"

static FILE *J; /* the report stream */

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---------------------------------------------------------------- JSON out */
static void jstr(const char *s)
{
	fputc('"', J);
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') fprintf(J, "\\%c", c);
		else if (c == '\n') fputs("\\n", J);
		else if (c == '\t') fputs("\\t", J);
		else if (c < 0x20) fprintf(J, "\\u%04x", c);
		else fputc(c, J);
	}
	fputc('"', J);
}
static void jnum(double v)
{
	if (isfinite(v)) fprintf(J, "%.6g", v);
	else fputs("null", J);
}

/* Findings: conformance problems a host survives (they make a grade "warn"). */
#define MAX_FINDINGS 32
static char findings[MAX_FINDINGS][256];
static int n_findings;
static void finding(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void finding(const char *fmt, ...)
{
	if (n_findings == MAX_FINDINGS) return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(findings[n_findings++], sizeof(findings[0]), fmt, ap);
	va_end(ap);
}
static void jfindings(void)
{
	fputs(",\"findings\":[", J);
	for (int i = 0; i < n_findings; i++) {
		if (i) fputc(',', J);
		jstr(findings[i]);
	}
	fputc(']', J);
}

static int fail_report(const char *stage, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail_report(const char *stage, const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fprintf(J, "{\"probe_version\":\"%s\",\"status\":\"fail\",\"stage\":", PROBE_VERSION);
	jstr(stage);
	fputs(",\"error\":", J);
	jstr(msg);
	jfindings();
	fputs("}\n", J);
	fflush(J);
	return 1;
}

/* ===================================================================== CLAP */
static void *clap_open(const char *path, const clap_plugin_entry_t **entry, char *err, size_t errn)
{
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) { snprintf(err, errn, "dlopen: %s", dlerror()); return NULL; }
	*entry = dlsym(h, "clap_entry");
	if (!*entry) { snprintf(err, errn, "no clap_entry symbol"); return NULL; }
	if (!clap_version_is_compatible((*entry)->clap_version)) {
		snprintf(err, errn, "incompatible CLAP version %u.%u.%u", (*entry)->clap_version.major,
			 (*entry)->clap_version.minor, (*entry)->clap_version.revision);
		return NULL;
	}
	if (!(*entry)->init(path)) { snprintf(err, errn, "clap_entry.init() returned false"); return NULL; }
	return h;
}

static int clap_list(const char *path)
{
	const clap_plugin_entry_t *e;
	char err[512];
	double t0 = now_ms();
	if (!clap_open(path, &e, err, sizeof(err)))
		return fail_report("load", "%s", err);
	double t_load = now_ms() - t0;
	const clap_plugin_factory_t *f = e->get_factory(CLAP_PLUGIN_FACTORY_ID);
	bool presets = e->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID) ||
		       e->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT);
	fprintf(J, "{\"probe_version\":\"%s\",\"status\":\"ok\",\"clap_version\":\"%u.%u.%u\",\"load_ms\":",
		PROBE_VERSION, e->clap_version.major, e->clap_version.minor, e->clap_version.revision);
	jnum(t_load);
	fprintf(J, ",\"preset_discovery\":%s,\"plugins\":[", presets ? "true" : "false");
	uint32_t n = f ? f->get_plugin_count(f) : 0;
	int out = 0;
	for (uint32_t i = 0; i < n; i++) {
		const clap_plugin_descriptor_t *d = f->get_plugin_descriptor(f, i);
		if (!d || !d->id) continue;
		fprintf(J, "%s{\"id\":", out++ ? "," : "");
		jstr(d->id);
		fputs(",\"name\":", J); jstr(d->name);
		fputs(",\"vendor\":", J); jstr(d->vendor);
		fputs(",\"version\":", J); jstr(d->version);
		fputs(",\"url\":", J); jstr(d->url);
		fputs(",\"description\":", J); jstr(d->description);
		fputs(",\"features\":[", J);
		for (int k = 0; d->features && d->features[k]; k++) {
			if (k) fputc(',', J);
			jstr(d->features[k]);
		}
		fputs("]}", J);
	}
	fputs("]}\n", J);
	fflush(J);
	e->deinit();
	return 0;
}

/* ---- a minimal, honest host: everything on one thread, which is "main"
 * outside process() and "audio" inside it. */
static bool in_audio;
static bool want_callback, want_restart;
static void h_request_restart(const clap_host_t *h) { (void)h; want_restart = true; }
static void h_request_process(const clap_host_t *h) { (void)h; }
static void h_request_callback(const clap_host_t *h) { (void)h; want_callback = true; }
static void h_log(const clap_host_t *h, clap_log_severity s, const char *m)
{
	(void)h;
	if (s == CLAP_LOG_HOST_MISBEHAVING)
		finding("plugin says the host misbehaved: %.180s", m ? m : "");
	fprintf(stderr, "plugin log %d: %s\n", (int)s, m ? m : "");
}
static const clap_host_log_t h_log_ext = { h_log };
static bool h_is_main(const clap_host_t *h) { (void)h; return !in_audio; }
static bool h_is_audio(const clap_host_t *h) { (void)h; return in_audio; }
static const clap_host_thread_check_t h_thread_ext = { h_is_main, h_is_audio };
static void h_params_rescan(const clap_host_t *h, clap_param_rescan_flags f) { (void)h; (void)f; }
static void h_params_clear(const clap_host_t *h, clap_id id, clap_param_clear_flags f) { (void)h; (void)id; (void)f; }
static void h_params_flush(const clap_host_t *h) { (void)h; want_callback = true; }
static const clap_host_params_t h_params_ext = { h_params_rescan, h_params_clear, h_params_flush };
static void h_state_dirty(const clap_host_t *h) { (void)h; }
static const clap_host_state_t h_state_ext = { h_state_dirty };
static void h_latency_changed(const clap_host_t *h) { (void)h; }
static const clap_host_latency_t h_latency_ext = { h_latency_changed };
static bool h_ap_flag(const clap_host_t *h, uint32_t f) { (void)h; (void)f; return false; }
static void h_ap_rescan(const clap_host_t *h, uint32_t f) { (void)h; (void)f; }
static const clap_host_audio_ports_t h_ap_ext = { h_ap_flag, h_ap_rescan };
static uint32_t h_np_dialects(const clap_host_t *h) { (void)h; return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; }
static void h_np_rescan(const clap_host_t *h, uint32_t f) { (void)h; (void)f; }
static const clap_host_note_ports_t h_np_ext = { h_np_dialects, h_np_rescan };
static clap_id next_timer = 1;
static bool h_timer_reg(const clap_host_t *h, uint32_t ms, clap_id *id) { (void)h; (void)ms; *id = next_timer++; return true; }
static bool h_timer_unreg(const clap_host_t *h, clap_id id) { (void)h; (void)id; return true; }
static const clap_host_timer_support_t h_timer_ext = { h_timer_reg, h_timer_unreg };
static const void *h_get_extension(const clap_host_t *h, const char *id)
{
	(void)h;
	if (!strcmp(id, CLAP_EXT_LOG)) return &h_log_ext;
	if (!strcmp(id, CLAP_EXT_THREAD_CHECK)) return &h_thread_ext;
	if (!strcmp(id, CLAP_EXT_PARAMS)) return &h_params_ext;
	if (!strcmp(id, CLAP_EXT_STATE)) return &h_state_ext;
	if (!strcmp(id, CLAP_EXT_LATENCY)) return &h_latency_ext;
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &h_ap_ext;
	if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &h_np_ext;
	if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &h_timer_ext;
	return NULL;
}
static const clap_host_t host = {
	CLAP_VERSION_INIT, NULL, "backline-probe", "Backline", "https://github.com/manuaudio/backline-audio",
	"0.1.0", h_get_extension, h_request_restart, h_request_process, h_request_callback,
};

/* ---- event lists */
#define MAX_EV 8
static union { clap_event_header_t h; clap_event_note_t note; clap_event_midi_t midi; } evs[MAX_EV];
static uint32_t n_evs;
static uint32_t ev_size(const clap_input_events_t *l) { (void)l; return n_evs; }
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) { (void)l; return &evs[i].h; }
static const clap_input_events_t in_events = { NULL, ev_size, ev_get };
static uint32_t out_count;
static bool out_push(const clap_output_events_t *l, const clap_event_header_t *e) { (void)l; (void)e; out_count++; return true; }
static const clap_output_events_t out_events = { NULL, out_push };

static void add_note(bool on, bool midi, uint16_t port, uint32_t time)
{
	if (n_evs >= MAX_EV) return;
	if (midi) {
		clap_event_midi_t *e = &evs[n_evs++].midi;
		memset(e, 0, sizeof(*e));
		e->header = (clap_event_header_t){ sizeof(*e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
		e->port_index = port;
		e->data[0] = on ? 0x90 : 0x80;
		e->data[1] = 60;
		e->data[2] = on ? 100 : 0;
	} else {
		clap_event_note_t *e = &evs[n_evs++].note;
		memset(e, 0, sizeof(*e));
		e->header = (clap_event_header_t){ sizeof(*e), time, CLAP_CORE_EVENT_SPACE_ID,
			(uint16_t)(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
		e->note_id = -1;
		e->port_index = (int16_t)port;
		e->channel = 0;
		e->key = 60;
		e->velocity = on ? 100 / 127.0 : 0;
	}
}

/* ---- state streams in memory */
typedef struct { uint8_t *data; size_t len, cap, pos; } membuf_t;
static int64_t ms_write(const clap_ostream_t *s, const void *buf, uint64_t n)
{
	membuf_t *m = s->ctx;
	if (m->len + n > m->cap) {
		size_t c = m->cap ? m->cap : 4096;
		while (c < m->len + n) c *= 2;
		if (c > (256u << 20)) return -1; /* 256 MB of state: refuse */
		uint8_t *d = realloc(m->data, c);
		if (!d) return -1;
		m->data = d; m->cap = c;
	}
	memcpy(m->data + m->len, buf, n);
	m->len += n;
	return (int64_t)n;
}
static int64_t ms_read(const clap_istream_t *s, void *buf, uint64_t n)
{
	membuf_t *m = s->ctx;
	size_t left = m->len - m->pos;
	if (n > left) n = left;
	memcpy(buf, m->data + m->pos, n);
	m->pos += n;
	return (int64_t)n;
}

#define BLOCK 512
#define BLOCKS 16
#define MAXPORTS 256
#define MAXCH 1024

static int clap_check(const char *path, const char *id)
{
	const clap_plugin_entry_t *e;
	char err[512];
	double t0 = now_ms();
	if (!clap_open(path, &e, err, sizeof(err)))
		return fail_report("load", "%s", err);
	double t_load = now_ms() - t0;
	const clap_plugin_factory_t *f = e->get_factory(CLAP_PLUGIN_FACTORY_ID);
	if (!f) return fail_report("factory", "no plugin factory");

	t0 = now_ms();
	const clap_plugin_t *p = f->create_plugin(f, &host, id);
	if (!p) return fail_report("create", "create_plugin(%s) returned NULL", id);
	if (!p->init(p)) return fail_report("init", "plugin init() returned false");
	double t_create = now_ms() - t0;

	const clap_plugin_params_t *pp = p->get_extension(p, CLAP_EXT_PARAMS);
	const clap_plugin_audio_ports_t *ap = p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
	const clap_plugin_note_ports_t *np = p->get_extension(p, CLAP_EXT_NOTE_PORTS);
	const clap_plugin_state_t *st = p->get_extension(p, CLAP_EXT_STATE);

	/* params: ids unique, ranges sane, values readable */
	uint32_t n_params = pp ? pp->count(p) : 0, bad_params = 0;
	clap_id *ids = calloc(n_params ? n_params : 1, sizeof(clap_id));
	for (uint32_t i = 0; i < n_params; i++) {
		clap_param_info_t info;
		memset(&info, 0, sizeof(info));
		if (!pp->get_info(p, i, &info)) { bad_params++; continue; }
		ids[i] = info.id;
		double v;
		bool range_ok = info.min_value <= info.max_value && isfinite(info.min_value) && isfinite(info.max_value) &&
				info.default_value >= info.min_value - 1e-9 && info.default_value <= info.max_value + 1e-9;
		bool value_ok = pp->get_value(p, info.id, &v) && isfinite(v);
		if (!range_ok || !value_ok || !info.name[0]) {
			if (bad_params < 3)
				finding("param %u '%.60s': %s", info.id, info.name,
					!range_ok ? "bad range/default" : !value_ok ? "get_value failed or not finite" : "empty name");
			bad_params++;
		}
		for (uint32_t k = 0; k < i; k++)
			if (ids[k] == info.id) { finding("duplicate param id %u", info.id); break; }
	}
	free(ids);
	if (bad_params > 3) finding("%u params with problems in total", bad_params);

	/* audio ports */
	uint32_t ports[2] = { 0, 0 }, chans[2] = { 0, 0 };
	static clap_audio_buffer_t bufs[2][MAXPORTS];
	static float data[2][MAXCH][BLOCK];
	static float *ptrs[2][MAXCH];
	for (int dir = 0; dir < 2; dir++) { /* 0 = input, 1 = output */
		uint32_t n = ap ? ap->count(p, dir == 0) : 0;
		if (n > MAXPORTS) return fail_report("audio-ports", "%u audio ports (more than %d)", n, MAXPORTS);
		uint32_t used = 0;
		for (uint32_t i = 0; i < n; i++) {
			clap_audio_port_info_t info;
			memset(&info, 0, sizeof(info));
			if (!ap->get(p, i, dir == 0, &info)) { finding("audio_ports.get(%u) failed", i); info.channel_count = 0; }
			uint32_t c = info.channel_count;
			if (used + c > MAXCH) return fail_report("audio-ports", "more than %d channels", MAXCH);
			bufs[dir][i] = (clap_audio_buffer_t){ .data32 = &ptrs[dir][used], .channel_count = c };
			for (uint32_t k = 0; k < c; k++) ptrs[dir][used + k] = data[dir][used + k];
			used += c;
		}
		ports[dir] = n;
		chans[dir] = used;
	}

	/* note input */
	bool has_notes = false, midi = false;
	uint16_t note_port = 0;
	if (np && np->count(p, true) > 0) {
		clap_note_port_info_t ni;
		memset(&ni, 0, sizeof(ni));
		if (np->get(p, 0, true, &ni)) {
			has_notes = true;
			midi = !(ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP) && (ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI);
		}
	}

	t0 = now_ms();
	if (!p->activate(p, 48000.0, 32, BLOCK)) return fail_report("activate", "activate(48000, 32, %d) returned false", BLOCK);
	double t_activate = now_ms() - t0;

	in_audio = true;
	bool started = p->start_processing(p);
	in_audio = false;
	if (!started) finding("start_processing returned false");

	uint64_t nonfinite = 0;
	double peak = 0, t_proc = 0;
	int errors = 0;
	double phase = 0;
	for (int b = 0; b < BLOCKS && started; b++) {
		for (uint32_t c = 0; c < chans[0]; c++)
			for (int s = 0; s < BLOCK; s++)
				data[0][c][s] = 0.25f * (float)sin(phase + 2 * M_PI * 1000 * s / 48000.0);
		phase = fmod(phase + 2 * M_PI * 1000 * BLOCK / 48000.0, 2 * M_PI);
		for (uint32_t c = 0; c < chans[1]; c++)
			memset(data[1][c], 0, sizeof(data[1][c]));
		n_evs = 0;
		if (has_notes && b == 1) add_note(true, midi, note_port, 17);
		if (has_notes && b == BLOCKS - 4) add_note(false, midi, note_port, 0);
		clap_process_t pr = {
			.steady_time = (int64_t)b * BLOCK, .frames_count = BLOCK, .transport = NULL,
			.audio_inputs = bufs[0], .audio_outputs = bufs[1],
			.audio_inputs_count = ports[0], .audio_outputs_count = ports[1],
			.in_events = &in_events, .out_events = &out_events,
		};
		double tp = now_ms();
		in_audio = true;
		clap_process_status r = p->process(p, &pr);
		in_audio = false;
		t_proc += now_ms() - tp;
		if (r == CLAP_PROCESS_ERROR) errors++;
		for (uint32_t c = 0; c < chans[1]; c++)
			for (int s = 0; s < BLOCK; s++) {
				float v = data[1][c][s];
				if (!isfinite(v)) nonfinite++;
				else if (fabsf(v) > peak) peak = fabsf(v);
			}
		if (want_callback) { want_callback = false; p->on_main_thread(p); }
	}
	if (started) {
		in_audio = true;
		p->stop_processing(p);
		in_audio = false;
	}
	if (errors) finding("process() returned CLAP_PROCESS_ERROR in %d of %d blocks", errors, BLOCKS);
	p->deactivate(p);

	/* state: save, load, save again; the two saves should match */
	const char *state = "none";
	size_t state_bytes = 0;
	double t_state = 0;
	if (st) {
		membuf_t a = { 0 }, b = { 0 };
		clap_ostream_t oa = { &a, ms_write }, ob = { &b, ms_write };
		t0 = now_ms();
		if (!st->save(p, &oa)) {
			finding("state.save returned false");
			state = "save-failed";
		} else {
			clap_istream_t is = { &a, ms_read };
			if (!st->load(p, &is)) {
				finding("state.load of its own saved state returned false");
				state = "load-failed";
			} else if (!st->save(p, &ob)) {
				finding("state.save after load returned false");
				state = "save-failed";
			} else if (a.len != b.len || memcmp(a.data, b.data, a.len)) {
				finding("state differs after a save/load/save round-trip (%zu vs %zu bytes)", a.len, b.len);
				state = "differs";
			} else {
				state = "stable";
			}
		}
		t_state = now_ms() - t0;
		state_bytes = a.len;
		free(a.data);
		free(b.data);
	}
	if (t_load + t_create > 5000) finding("slow to load: %.0f ms", t_load + t_create);

	t0 = now_ms();
	p->destroy(p);
	e->deinit();
	double t_destroy = now_ms() - t0;

	bool ok = nonfinite == 0;
	fprintf(J, "{\"probe_version\":\"%s\",\"status\":\"%s\",\"checked\":\"run\"", PROBE_VERSION, ok ? "ok" : "fail");
	if (!ok) fprintf(J, ",\"stage\":\"process\",\"error\":\"%llu non-finite output samples\"", (unsigned long long)nonfinite);
	fprintf(J, ",\"params\":%u,\"audio_in\":%u,\"audio_out\":%u,\"note_input\":%s,\"blocks\":%d,\"block\":%d",
		n_params, chans[0], chans[1], has_notes ? (midi ? "\"midi\"" : "\"clap\"") : "null", started ? BLOCKS : 0, BLOCK);
	fputs(",\"peak\":", J); jnum(peak);
	fprintf(J, ",\"nonfinite\":%llu,\"out_events\":%u,\"state\":\"%s\",\"state_bytes\":%zu,\"ms\":{\"load\":",
		(unsigned long long)nonfinite, out_count, state, state_bytes);
	jnum(t_load); fputs(",\"create\":", J); jnum(t_create);
	fputs(",\"activate\":", J); jnum(t_activate);
	fputs(",\"process_per_block\":", J); jnum(started ? t_proc / BLOCKS : 0);
	fputs(",\"state\":", J); jnum(t_state);
	fputs(",\"destroy\":", J); jnum(t_destroy);
	fputc('}', J);
	jfindings();
	fputs("}\n", J);
	fflush(J);
	return ok ? 0 : 1;
}

/* ===================================================================== VST3 */
/* The non-COM vtable layout VST3 uses on Linux (pluginterfaces/base). */
typedef int32_t tresult;
typedef struct { char vendor[64], url[256], email[128]; int32_t flags; } PFactoryInfo;
typedef struct { uint8_t cid[16]; int32_t cardinality; char category[32], name[64]; } PClassInfo;
typedef struct {
	uint8_t cid[16]; int32_t cardinality; char category[32], name[64];
	uint32_t classFlags; char subCategories[128], vendor[64], version[64], sdkVersion[64];
} PClassInfo2;
typedef struct IPluginFactory2 IPluginFactory2;
struct IPluginFactory2Vtbl {
	tresult (*queryInterface)(void *, const uint8_t iid[16], void **);
	uint32_t (*addRef)(void *);
	uint32_t (*release)(void *);
	tresult (*getFactoryInfo)(void *, PFactoryInfo *);
	int32_t (*countClasses)(void *);
	tresult (*getClassInfo)(void *, int32_t, PClassInfo *);
	tresult (*createInstance)(void *, const char *, const char *, void **);
	tresult (*getClassInfo2)(void *, int32_t, PClassInfo2 *);
};
struct IPluginFactory2 { const struct IPluginFactory2Vtbl *v; };
/* IPluginFactory2 IID 0007B650-F24B4C0B-A464EDB9-F00B2ABB (non-COM byte order) */
static const uint8_t IID_IPluginFactory2[16] = { 0x00, 0x07, 0xB6, 0x50, 0xF2, 0x4B, 0x4C, 0x0B,
						 0xA4, 0x64, 0xED, 0xB9, 0xF0, 0x0B, 0x2A, 0xBB };

static void jfixed(const char *s, size_t n) /* fixed-size, maybe unterminated */
{
	char tmp[256];
	if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
	memcpy(tmp, s, n);
	tmp[n] = 0;
	jstr(tmp);
}

static int vst3_list(const char *bundle)
{
	struct utsname u;
	uname(&u);
	const char *base = strrchr(bundle, '/');
	char b2[4096];
	snprintf(b2, sizeof b2, "%s", bundle);
	size_t bl = strlen(b2);
	while (bl > 1 && b2[bl - 1] == '/') b2[--bl] = 0;
	base = strrchr(b2, '/');
	base = base ? base + 1 : b2;
	char name[512], so[8192], mi[8192];
	snprintf(name, sizeof name, "%s", base);
	char *dot = strstr(name, ".vst3");
	if (dot) *dot = 0;
	snprintf(so, sizeof so, "%s/Contents/%s-linux/%s.so", b2, u.machine, name);
	snprintf(mi, sizeof mi, "%s/Contents/Resources/moduleinfo.json", b2);
	struct stat sst;
	bool moduleinfo = stat(mi, &sst) == 0;
	double t0 = now_ms();
	void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
	if (!h) return fail_report("load", "dlopen: %s", dlerror());
	bool (*entry)(void *) = (bool (*)(void *))dlsym(h, "ModuleEntry");
	bool (*exitf)(void) = (bool (*)(void))dlsym(h, "ModuleExit");
	void *(*getf)(void) = (void *(*)(void))dlsym(h, "GetPluginFactory");
	if (!getf) return fail_report("load", "no GetPluginFactory symbol");
	if (entry && !entry(h)) return fail_report("load", "ModuleEntry() returned false");
	IPluginFactory2 *f1 = getf(), *f2 = NULL;
	if (!f1) return fail_report("factory", "GetPluginFactory() returned NULL");
	f1->v->queryInterface(f1, IID_IPluginFactory2, (void **)&f2);
	PFactoryInfo fi;
	memset(&fi, 0, sizeof(fi));
	f1->v->getFactoryInfo(f1, &fi);
	double t_load = now_ms() - t0;
	fprintf(J, "{\"probe_version\":\"%s\",\"status\":\"ok\",\"checked\":\"load\",\"binary\":", PROBE_VERSION);
	jstr(so);
	fprintf(J, ",\"moduleinfo\":%s,\"load_ms\":", moduleinfo ? "true" : "false");
	jnum(t_load);
	fputs(",\"factory_vendor\":", J);
	jfixed(fi.vendor, sizeof fi.vendor);
	fputs(",\"classes\":[", J);
	int32_t n = f1->v->countClasses(f1), out = 0;
	for (int32_t i = 0; i < n; i++) {
		PClassInfo2 c;
		memset(&c, 0, sizeof(c));
		if (f2) {
			if (f2->v->getClassInfo2(f2, i, &c) != 0) continue;
		} else {
			PClassInfo c1;
			memset(&c1, 0, sizeof(c1));
			if (f1->v->getClassInfo(f1, i, &c1) != 0) continue;
			memcpy(&c, &c1, sizeof c1);
		}
		if (strncmp(c.category, "Audio Module Class", sizeof c.category)) continue; /* skip controllers */
		fprintf(J, "%s{\"cid\":\"", out++ ? "," : "");
		for (int k = 0; k < 16; k++) fprintf(J, "%02X", c.cid[k]);
		fputs("\",\"name\":", J); jfixed(c.name, sizeof c.name);
		fputs(",\"subcategories\":", J); jfixed(c.subCategories, sizeof c.subCategories);
		fputs(",\"vendor\":", J);
		if (c.vendor[0]) jfixed(c.vendor, sizeof c.vendor); else jfixed(fi.vendor, sizeof fi.vendor);
		fputs(",\"version\":", J); jfixed(c.version, sizeof c.version);
		fputs(",\"sdk\":", J); jfixed(c.sdkVersion, sizeof c.sdkVersion);
		fputc('}', J);
	}
	fputs("]}\n", J);
	fflush(J);
	if (f2) f2->v->release(f2);
	f1->v->release(f1);
	if (exitf) exitf();
	return 0;
}

/* ====================================================================== LV2 */
#ifdef BL_HAVE_LILV
static LilvWorld *world;
static LilvNode *n_audio, *n_control, *n_input, *n_output, *n_atom, *n_cv, *n_midi_ev, *n_preset,
	*n_inst_class, *n_midi_class, *n_analyser_class, *n_ui, *n_conn_optional;

static void lv2_init(const char *bundle)
{
	world = lilv_world_new();
	if (bundle) {
		/* Only this bundle: much faster than parsing every installed TTL. */
		lilv_world_load_specifications(world);
		lilv_world_load_plugin_classes(world);
		char uri[8192];
		size_t n = strlen(bundle);
		snprintf(uri, sizeof uri, "file://%s%s", bundle, n && bundle[n - 1] == '/' ? "" : "/");
		LilvNode *b = lilv_new_uri(world, uri);
		lilv_world_load_bundle(world, b);
		lilv_node_free(b);
	} else {
		lilv_world_load_all(world);
	}
#define U(v, s) v = lilv_new_uri(world, s)
	U(n_audio, LV2_CORE__AudioPort); U(n_control, LV2_CORE__ControlPort);
	U(n_input, LV2_CORE__InputPort); U(n_output, LV2_CORE__OutputPort);
	U(n_atom, LV2_ATOM__AtomPort); U(n_cv, LV2_CORE__CVPort);
	U(n_midi_ev, "http://lv2plug.in/ns/ext/midi#MidiEvent");
	U(n_preset, "http://lv2plug.in/ns/ext/presets#Preset");
	U(n_inst_class, LV2_CORE__InstrumentPlugin); U(n_midi_class, LV2_CORE_PREFIX "MIDIPlugin");
	U(n_analyser_class, LV2_CORE__AnalyserPlugin);
	U(n_ui, "http://lv2plug.in/ns/extensions/ui#ui");
	U(n_conn_optional, LV2_CORE__connectionOptional);
#undef U
}

static bool has_class(const LilvPlugin *p, const LilvNode *cls)
{
	const LilvPluginClass *c = lilv_plugin_get_class(p);
	const LilvPluginClasses *all = lilv_world_get_plugin_classes(world);
	for (int depth = 0; c && depth < 8; depth++) {
		if (lilv_node_equals(lilv_plugin_class_get_uri(c), cls)) return true;
		const LilvNode *parent = lilv_plugin_class_get_parent_uri(c);
		c = parent ? lilv_plugin_classes_get_by_uri(all, parent) : NULL;
	}
	return false;
}

static int lv2_list(void)
{
	lv2_init(NULL);
	const LilvPlugins *ps = lilv_world_get_all_plugins(world);
	fprintf(J, "{\"probe_version\":\"%s\",\"status\":\"ok\",\"plugins\":[", PROBE_VERSION);
	int out = 0;
	LILV_FOREACH (plugins, it, ps) {
		const LilvPlugin *p = lilv_plugins_get(ps, it);
		int ain = 0, aout = 0, min = 0, mout = 0, ctl = 0;
		for (uint32_t i = 0; i < lilv_plugin_get_num_ports(p); i++) {
			const LilvPort *pt = lilv_plugin_get_port_by_index(p, i);
			bool in = lilv_port_is_a(p, pt, n_input);
			if (lilv_port_is_a(p, pt, n_audio)) in ? ain++ : aout++;
			else if (lilv_port_is_a(p, pt, n_control)) ctl += in;
			else if (lilv_port_is_a(p, pt, n_atom) && lilv_port_supports_event(p, pt, n_midi_ev))
				in ? min++ : mout++;
		}
		LilvNodes *presets = lilv_plugin_get_related(p, n_preset);
		LilvNodes *req = lilv_plugin_get_required_features(p);
		LilvNodes *uis = lilv_plugin_get_value(p, n_ui);
		LilvNode *name = lilv_plugin_get_name(p), *author = lilv_plugin_get_author_name(p);
		const LilvPluginClass *cls = lilv_plugin_get_class(p);
		fprintf(J, "%s{\"uri\":", out++ ? "," : "");
		jstr(lilv_node_as_uri(lilv_plugin_get_uri(p)));
		fputs(",\"name\":", J); jstr(name ? lilv_node_as_string(name) : "");
		fputs(",\"vendor\":", J); jstr(author ? lilv_node_as_string(author) : "");
		fputs(",\"class\":", J); jstr(cls ? lilv_node_as_string(lilv_plugin_class_get_label(cls)) : "");
		fprintf(J, ",\"instrument\":%s,\"midi_plugin\":%s,\"analyser\":%s",
			has_class(p, n_inst_class) ? "true" : "false", has_class(p, n_midi_class) ? "true" : "false",
			has_class(p, n_analyser_class) ? "true" : "false");
		fputs(",\"bundle\":", J);
		char *bp = lilv_file_uri_parse(lilv_node_as_uri(lilv_plugin_get_bundle_uri(p)), NULL);
		jstr(bp);
		lilv_free(bp);
		fputs(",\"library\":", J);
		const LilvNode *lib = lilv_plugin_get_library_uri(p);
		char *lp = lib ? lilv_file_uri_parse(lilv_node_as_uri(lib), NULL) : NULL;
		jstr(lp ? lp : "");
		lilv_free(lp);
		fprintf(J, ",\"io\":{\"audio_in\":%d,\"audio_out\":%d,\"midi_in\":%d,\"midi_out\":%d,\"controls\":%d}",
			ain, aout, min, mout, ctl);
		fprintf(J, ",\"presets\":%u,\"has_ui\":%s,\"required_features\":[", presets ? lilv_nodes_size(presets) : 0,
			uis && lilv_nodes_size(uis) ? "true" : "false");
		int k = 0;
		LILV_FOREACH (nodes, ri, req) {
			if (k++) fputc(',', J);
			jstr(lilv_node_as_uri(lilv_nodes_get(req, ri)));
		}
		fputs("]}", J);
		lilv_nodes_free(presets);
		lilv_nodes_free(req);
		lilv_nodes_free(uis);
		lilv_node_free(name);
		lilv_node_free(author);
	}
	fputs("]}\n", J);
	fflush(J);
	return 0;
}

static char *urids[4096];
static uint32_t nurids;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char *uri)
{
	(void)h;
	for (uint32_t i = 0; i < nurids; i++)
		if (!strcmp(urids[i], uri)) return i + 1;
	if (nurids == 4096) return 0;
	urids[nurids] = strdup(uri);
	return ++nurids;
}
static const char *unmap_uri(LV2_URID_Unmap_Handle h, LV2_URID u)
{
	(void)h;
	return u && u <= nurids ? urids[u - 1] : NULL;
}
/* A synchronous worker: enough for a check run, where nothing is realtime. */
static const LV2_Worker_Interface *worker_iface;
static LV2_Handle worker_handle;
static LV2_Worker_Status worker_respond(LV2_Worker_Respond_Handle h, uint32_t size, const void *data)
{
	(void)h;
	return worker_iface && worker_iface->work_response ? worker_iface->work_response(worker_handle, size, data)
							    : LV2_WORKER_ERR_UNKNOWN;
}
static LV2_Worker_Status worker_schedule(LV2_Worker_Schedule_Handle h, uint32_t size, const void *data)
{
	(void)h;
	if (!worker_iface) return LV2_WORKER_ERR_UNKNOWN;
	return worker_iface->work(worker_handle, worker_respond, NULL, size, data);
}

#define LV2_BLOCK 512
static int lv2_check(const char *uri_s, const char *bundle)
{
	double t0 = now_ms();
	lv2_init(bundle);
	LilvNode *uri = lilv_new_uri(world, uri_s);
	const LilvPlugin *p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri);
	if (!p) return fail_report("load", "plugin %s not found", uri_s);
	double t_ttl = now_ms() - t0;

	static LV2_URID_Map map = { NULL, map_uri };
	static LV2_URID_Unmap unmap = { NULL, unmap_uri };
	int32_t block = LV2_BLOCK, seq = 8192;
	LV2_Options_Option opts[] = {
		{ LV2_OPTIONS_INSTANCE, 0, map_uri(NULL, LV2_BUF_SIZE__minBlockLength), sizeof(int32_t),
		  map_uri(NULL, LV2_ATOM__Int), &block },
		{ LV2_OPTIONS_INSTANCE, 0, map_uri(NULL, LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t),
		  map_uri(NULL, LV2_ATOM__Int), &block },
		{ LV2_OPTIONS_INSTANCE, 0, map_uri(NULL, LV2_BUF_SIZE__sequenceSize), sizeof(int32_t),
		  map_uri(NULL, LV2_ATOM__Int), &seq },
		{ LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL },
	};
	LV2_Feature f_map = { LV2_URID__map, &map }, f_unmap = { LV2_URID__unmap, &unmap },
		    f_opts = { LV2_OPTIONS__options, opts }, f_bbl = { LV2_BUF_SIZE__boundedBlockLength, NULL };
	static LV2_Worker_Schedule sched = { NULL, worker_schedule };
	LV2_Feature f_work = { LV2_WORKER__schedule, &sched };
	const LV2_Feature *features[] = { &f_map, &f_unmap, &f_opts, &f_bbl, &f_work, NULL };
	const char *have[] = { LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options, LV2_BUF_SIZE__boundedBlockLength,
			       LV2_WORKER__schedule, LV2_CORE__isLive, LV2_CORE__inPlaceBroken, LV2_CORE__hardRTCapable,
			       NULL };

	/* Like auval's "required features": say which host feature is missing. */
	LilvNodes *req = lilv_plugin_get_required_features(p);
	LILV_FOREACH (nodes, ri, req) {
		const char *f = lilv_node_as_uri(lilv_nodes_get(req, ri));
		bool ok = false;
		for (int i = 0; have[i]; i++) ok |= !strcmp(have[i], f);
		if (!ok) {
			fprintf(J, "{\"probe_version\":\"%s\",\"status\":\"skipped\",\"checked\":\"metadata\",\"error\":", PROBE_VERSION);
			char m[512];
			snprintf(m, sizeof m, "needs host feature %s", f);
			jstr(m);
			fputs("}\n", J);
			fflush(J);
			return 0;
		}
	}

	t0 = now_ms();
	LilvInstance *inst = lilv_plugin_instantiate(p, 48000.0, features);
	if (!inst) return fail_report("instantiate", "lilv_plugin_instantiate returned NULL");
	double t_inst = now_ms() - t0;
	uint32_t np = lilv_plugin_get_num_ports(p);
	float *mins = calloc(np, sizeof(float)), *maxs = calloc(np, sizeof(float)), *defs = calloc(np, sizeof(float));
	lilv_plugin_get_port_ranges_float(p, mins, maxs, defs);
	float(*bufs)[LV2_BLOCK] = calloc(np, sizeof *bufs);
	float *ctl = calloc(np, sizeof(float));
	uint8_t(*atoms)[16384] = calloc(np, sizeof *atoms);
	for (uint32_t i = 0; i < np; i++) {
		const LilvPort *pt = lilv_plugin_get_port_by_index(p, i);
		bool in = lilv_port_is_a(p, pt, n_input);
		if (lilv_port_is_a(p, pt, n_control)) {
			ctl[i] = isnan(defs[i]) ? (isnan(mins[i]) ? 0 : mins[i]) : defs[i];
			lilv_instance_connect_port(inst, i, &ctl[i]);
		} else if (lilv_port_is_a(p, pt, n_audio) || lilv_port_is_a(p, pt, n_cv)) {
			if (in)
				for (int s = 0; s < LV2_BLOCK; s++) bufs[i][s] = 0.25f * sinf(2 * M_PI * 1000 * s / 48000.0f);
			lilv_instance_connect_port(inst, i, bufs[i]);
		} else if (lilv_port_is_a(p, pt, n_atom)) {
			LV2_Atom_Sequence *sq = (LV2_Atom_Sequence *)atoms[i];
			if (in) {
				sq->atom.size = sizeof(LV2_Atom_Sequence_Body);
				sq->atom.type = map_uri(NULL, LV2_ATOM__Sequence);
			} else {
				sq->atom.size = sizeof atoms[i] - sizeof(LV2_Atom);
				sq->atom.type = map_uri(NULL, LV2_ATOM__Chunk);
			}
			lilv_instance_connect_port(inst, i, sq);
		} else if (!lilv_port_has_property(p, pt, n_conn_optional)) {
			lilv_instance_connect_port(inst, i, bufs[i]);
		}
	}
	worker_handle = lilv_instance_get_handle(inst);
	worker_iface = lilv_instance_get_extension_data(inst, LV2_WORKER__interface);
	lilv_instance_activate(inst);
	uint64_t bad = 0;
	double peak = 0;
	t0 = now_ms();
	for (int cycle = 0; cycle < 4; cycle++) {
		for (uint32_t i = 0; i < np; i++) { /* reset output sequences each cycle */
			const LilvPort *pt = lilv_plugin_get_port_by_index(p, i);
			if (lilv_port_is_a(p, pt, n_atom) && lilv_port_is_a(p, pt, n_output)) {
				LV2_Atom_Sequence *sq = (LV2_Atom_Sequence *)atoms[i];
				sq->atom.size = sizeof atoms[i] - sizeof(LV2_Atom);
				sq->atom.type = map_uri(NULL, LV2_ATOM__Chunk);
			}
		}
		lilv_instance_run(inst, LV2_BLOCK);
		for (uint32_t i = 0; i < np; i++) {
			const LilvPort *pt = lilv_plugin_get_port_by_index(p, i);
			if (!lilv_port_is_a(p, pt, n_output) || !lilv_port_is_a(p, pt, n_audio)) continue;
			for (int s = 0; s < LV2_BLOCK; s++) {
				if (!isfinite(bufs[i][s])) bad++;
				else if (fabs(bufs[i][s]) > peak) peak = fabs(bufs[i][s]);
			}
		}
	}
	double t_run = now_ms() - t0;
	lilv_instance_deactivate(inst);
	lilv_instance_free(inst);
	fprintf(J, "{\"probe_version\":\"%s\",\"status\":\"%s\",\"checked\":\"run\"", PROBE_VERSION, bad ? "fail" : "ok");
	if (bad) fprintf(J, ",\"stage\":\"process\",\"error\":\"%llu non-finite output samples\"", (unsigned long long)bad);
	fprintf(J, ",\"blocks\":4,\"block\":%d,\"nonfinite\":%llu,\"peak\":", LV2_BLOCK, (unsigned long long)bad);
	jnum(peak);
	fputs(",\"ms\":{\"ttl\":", J); jnum(t_ttl);
	fputs(",\"instantiate\":", J); jnum(t_inst);
	fputs(",\"run\":", J); jnum(t_run);
	fputc('}', J);
	jfindings();
	fputs("}\n", J);
	fflush(J);
	return bad ? 1 : 0;
}
#endif

int main(int argc, char **argv)
{
	/* The report goes to the original stdout; plugin chatter goes to stderr. */
	int jfd = dup(1);
	dup2(2, 1);
	J = fdopen(jfd, "w");
	if (!J) return 3;
	/* No core dumps for plugins we crash on purpose, no windows, no input. */
	prctl(PR_SET_DUMPABLE, 0);
	struct rlimit rl = { 0, 0 };
	setrlimit(RLIMIT_CORE, &rl);
	setenv("DISPLAY", "", 1);
	setenv("WAYLAND_DISPLAY", "", 1);

	if (argc >= 3 && !strcmp(argv[1], "clap-list")) return clap_list(argv[2]);
	if (argc >= 4 && !strcmp(argv[1], "clap-check")) return clap_check(argv[2], argv[3]);
	if (argc >= 3 && !strcmp(argv[1], "vst3-list")) return vst3_list(argv[2]);
#ifdef BL_HAVE_LILV
	if (argc >= 2 && !strcmp(argv[1], "lv2-list")) return lv2_list();
	if (argc >= 3 && !strcmp(argv[1], "lv2-check")) return lv2_check(argv[2], argc >= 4 ? argv[3] : NULL);
#else
	if (argc >= 2 && !strncmp(argv[1], "lv2-", 4)) return fail_report("unsupported", "built without lilv");
#endif
	if (argc >= 2 && !strcmp(argv[1], "--version")) { fprintf(J, "{\"probe_version\":\"%s\"}\n", PROBE_VERSION); return 0; }
	fprintf(stderr, "usage: backline-probe clap-list FILE | clap-check FILE ID | vst3-list BUNDLE | lv2-list | lv2-check URI [BUNDLE]\n");
	return 2;
}
