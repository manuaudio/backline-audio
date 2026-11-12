// backline-plugin-worker: one plugin instance, one PipeWire node, one process.
// SPDX-License-Identifier: MIT
//
// This is the unit of isolation of the Backline plugin host. A crash here
// takes down exactly one node: PipeWire drops this client, its node and the
// links it made, and every other node keeps running.
//
// Threads (and which CLAP calls happen on each):
//   main   = the PipeWire main loop. CLAP [main-thread] calls, D-Bus, OSC,
//            graph watching and linking. Never blocks on the audio thread
//            except for the documented stop handshake (pw_loop_invoke).
//   audio  = PipeWire's data loop (PW_FILTER_FLAG_RT_PROCESS). CLAP
//            start/stop_processing and process. No locks, no allocation,
//            no syscalls besides what PipeWire does itself.
// Control reaches the audio thread through a wait-free SPSC ring; parameter
// changes the plugin reports come back through another one.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdarg>
#include <strings.h>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <getopt.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <clap/clap.h>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/control/control.h>
#include <spa/control/ump-utils.h>
#include <spa/pod/parser.h>
#include <spa/utils/string.h>
#include <systemd/sd-bus.h>

#include "../common/bl_graph.h"
#include "osc.h"
#include "spsc.h"

namespace {

constexpr const char *kBusPrefix = "io.github.manuaudio.Backline.Instance.";
constexpr const char *kObjectPath = "/io/github/manuaudio/Backline/Instance";
constexpr const char *kInterface = "io.github.manuaudio.Backline.Instance1";
constexpr uint32_t kMaxFrames = 8192; // PipeWire's largest quantum
constexpr uint32_t kMaxEvents = 2048; // per cycle

thread_local bool t_audio_thread = false;

struct Param {
    clap_id id;
    std::string name, module;
    double min, max, def;
    uint32_t flags;
    void *cookie;
};

struct CtlEvent {
    enum Kind : uint8_t { ParamValue, Midi, ReleaseAll } kind;
    uint8_t midi[3];
    clap_id id;
    void *cookie;
    double value;
};

struct OutParam {
    clap_id id;
    double value;
};

struct PortGroup {   // one CLAP audio port, as N mono PipeWire DSP ports
    uint32_t clap_index;
    uint32_t channels;
    std::vector<void *> pw;          // pw_filter port data
    std::vector<std::string> names;  // port.name
    std::vector<std::string> pos;    // audio.channel
    std::vector<float *> ptrs;       // per-cycle buffer pointers (preallocated)
};

struct alignas(8) EvSlot { uint8_t raw[64]; };

struct Worker {
    // ---- options
    std::string plugin_path, plugin_id, node_name, midi_follow, audio_target, state_file;
    int osc_port = 0;
    int autoconnect = -1; // -1: default (on unless an audio target is given)
    bool use_dbus = true, list_only = false;

    // ---- PipeWire
    pw_main_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    spa_hook core_listener{};
    pw_filter *filter = nullptr;
    spa_hook filter_listener{};
    bl_graph *graph = nullptr;
    void *midi_port = nullptr;
    std::vector<PortGroup> ins, outs;
    spa_source *main_event = nullptr, *housekeeping = nullptr;
    uint32_t node_id = SPA_ID_INVALID;
    uint32_t midi_port_id = SPA_ID_INVALID;
    std::unordered_map<std::string, uint32_t> own_ports; // port.name -> global id
    std::optional<std::regex> midi_re;
    uint32_t server_rate = 48000;

    // ---- CLAP
    void *dl = nullptr;
    const clap_plugin_entry *entry = nullptr;
    const clap_plugin *plugin = nullptr;
    clap_host host{};
    const clap_plugin_params *ext_params = nullptr;
    const clap_plugin_audio_ports *ext_audio_ports = nullptr;
    const clap_plugin_note_ports *ext_note_ports = nullptr;
    const clap_plugin_state *ext_state = nullptr;
    const clap_plugin_timer_support *ext_timer = nullptr;
    std::string plugin_name;
    bool dialect_midi = false, dialect_clap = false;
    uint16_t note_port = 0;
    bool has_note_port = false;
    std::vector<Param> params;
    std::unordered_map<clap_id, size_t> param_index;
    std::unique_ptr<std::atomic<double>[]> values;
    std::unordered_map<clap_id, spa_source *> timers;
    clap_id next_timer = 1;

    // ---- realtime state
    std::atomic<bool> active{false};
    bool processing = false;               // audio thread only
    uint32_t active_rate = 0;
    uint64_t steady = 0;                   // audio thread only
    std::atomic<uint64_t> last_process_ns{0};
    std::atomic<uint32_t> graph_rate{0};
    std::atomic<bool> want_reactivate{false}, want_callback{false};
    Spsc<CtlEvent> to_audio{1024};
    Spsc<OutParam> from_audio{4096};
    std::vector<EvSlot> evs{kMaxEvents};
    uint32_t n_evs = 0;
    std::vector<float> zeros = std::vector<float>(kMaxFrames, 0.f);
    std::vector<float> trash = std::vector<float>(kMaxFrames, 0.f);
    std::vector<clap_audio_buffer> in_bufs, out_bufs;
    std::atomic<uint64_t> cycles{0}, midi_in{0}, dropped{0}, released{0};
    uint8_t held[16][128] = {};            // audio thread only: sounding notes
    std::vector<uint32_t> midi_sources;    // main thread: ports we linked to midi_in

    // ---- control
    std::thread::id main_tid;
    int osc_fd = -1;
    spa_source *osc_source = nullptr;
    sd_bus *bus = nullptr;
    sd_bus_slot *bus_slot = nullptr;

    // ---- state persistence (main thread)
    // serial counts changes a saved state would see: parameter sets, plugin
    // mark_dirty, parameter values the plugin reports, LoadState.
    uint64_t state_serial = 0, state_saved = 0, state_saves = 0;
    uint64_t dirty_since_ns = 0, last_save_ns = 0;
    uint64_t announced_serial = UINT64_MAX;
    // Control events pushed to the audio thread (main) and consumed by it
    // (audio, after process() returned). A save waits until the plugin has
    // seen every change, or it would capture the state before the last move.
    uint64_t ctl_pushed = 0, ctl_needed = 0;
    std::atomic<uint64_t> ctl_consumed{0};
    spa_source *bus_source = nullptr;
    std::string bus_name;
};

Worker *W = nullptr;

void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("backline-plugin-worker: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

#define LOG(...) fprintf(stderr, "backline-plugin-worker: " __VA_ARGS__)

uint64_t now_ns()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// ===================================================================== host
const void *host_get_extension(const clap_host *, const char *id);
void host_request_restart(const clap_host *)
{
    W->want_reactivate = true;
    pw_loop_signal_event(pw_main_loop_get_loop(W->loop), W->main_event);
}
void host_request_process(const clap_host *) {}
void host_request_callback(const clap_host *)
{
    W->want_callback = true;
    pw_loop_signal_event(pw_main_loop_get_loop(W->loop), W->main_event);
}

void host_log(const clap_host *, clap_log_severity sev, const char *msg)
{
    static const char *names[] = {"debug", "info", "warning", "error", "fatal", "host-misbehaving", "plugin-misbehaving"};
    if (sev >= CLAP_LOG_INFO)
        LOG("plugin %s: %s\n", sev <= CLAP_LOG_PLUGIN_MISBEHAVING ? names[sev] : "?", msg);
}
const clap_host_log host_log_ext{host_log};

bool host_is_main_thread(const clap_host *) { return std::this_thread::get_id() == W->main_tid; }
bool host_is_audio_thread(const clap_host *) { return t_audio_thread; }
const clap_host_thread_check host_thread_check_ext{host_is_main_thread, host_is_audio_thread};

void rescan_params();
void host_params_rescan(const clap_host *, clap_param_rescan_flags flags)
{
    if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_INFO))
        rescan_params();
}
void host_params_clear(const clap_host *, clap_id, clap_param_clear_flags) {}
void host_params_request_flush(const clap_host *) { host_request_callback(nullptr); }
const clap_host_params host_params_ext{host_params_rescan, host_params_clear, host_params_request_flush};

void mark_dirty();
void host_state_mark_dirty(const clap_host *) { mark_dirty(); }
const clap_host_state host_state_ext{host_state_mark_dirty};

void host_latency_changed(const clap_host *) { LOG("plugin latency changed\n"); }
const clap_host_latency host_latency_ext{host_latency_changed};

bool host_ap_is_rescan_flag_supported(const clap_host *, uint32_t) { return false; }
void host_ap_rescan(const clap_host *, uint32_t) { LOG("plugin asked for an audio port rescan (not supported yet)\n"); }
const clap_host_audio_ports host_audio_ports_ext{host_ap_is_rescan_flag_supported, host_ap_rescan};

uint32_t host_np_supported_dialects(const clap_host *) { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; }
void host_np_rescan(const clap_host *, uint32_t) {}
const clap_host_note_ports host_note_ports_ext{host_np_supported_dialects, host_np_rescan};

void on_plugin_timer(void *data, uint64_t)
{
    clap_id id = (clap_id)(uintptr_t)data;
    if (W->ext_timer)
        W->ext_timer->on_timer(W->plugin, id);
}
bool host_register_timer(const clap_host *, uint32_t period_ms, clap_id *timer_id)
{
    auto *l = pw_main_loop_get_loop(W->loop);
    clap_id id = W->next_timer++;
    spa_source *s = pw_loop_add_timer(l, on_plugin_timer, (void *)(uintptr_t)id);
    timespec v{period_ms / 1000, (long)(period_ms % 1000) * 1000000L};
    if (period_ms == 0)
        v.tv_nsec = 1000000;
    pw_loop_update_timer(l, s, &v, &v, false);
    W->timers[id] = s;
    *timer_id = id;
    return true;
}
bool host_unregister_timer(const clap_host *, clap_id id)
{
    auto it = W->timers.find(id);
    if (it == W->timers.end())
        return false;
    pw_loop_destroy_source(pw_main_loop_get_loop(W->loop), it->second);
    W->timers.erase(it);
    return true;
}
const clap_host_timer_support host_timer_ext{host_register_timer, host_unregister_timer};

const void *host_get_extension(const clap_host *, const char *id)
{
    if (!strcmp(id, CLAP_EXT_LOG)) return &host_log_ext;
    if (!strcmp(id, CLAP_EXT_THREAD_CHECK)) return &host_thread_check_ext;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &host_params_ext;
    if (!strcmp(id, CLAP_EXT_STATE)) return &host_state_ext;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &host_latency_ext;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &host_audio_ports_ext;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &host_note_ports_ext;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &host_timer_ext;
    return nullptr;
}

// ============================================================ input events
uint32_t in_size(const clap_input_events *) { return W->n_evs; }
const clap_event_header_t *in_get(const clap_input_events *, uint32_t i)
{
    return reinterpret_cast<const clap_event_header_t *>(W->evs[i].raw);
}
const clap_input_events in_events{nullptr, in_size, in_get};

bool out_try_push(const clap_output_events *, const clap_event_header_t *ev)
{
    if (ev->space_id == CLAP_CORE_EVENT_SPACE_ID && ev->type == CLAP_EVENT_PARAM_VALUE) {
        auto *pv = reinterpret_cast<const clap_event_param_value *>(ev);
        if (!W->from_audio.push({pv->param_id, pv->value}))
            W->dropped++;
    }
    return true;
}
const clap_output_events out_events{nullptr, out_try_push};

template <typename T> T *ev_alloc()
{
    static_assert(sizeof(T) <= sizeof(EvSlot));
    if (W->n_evs >= kMaxEvents) {
        W->dropped++;
        return nullptr;
    }
    auto *e = reinterpret_cast<T *>(W->evs[W->n_evs++].raw);
    memset(e, 0, sizeof(T));
    return e;
}

void push_midi_event(uint32_t time, const uint8_t *m, size_t n)
{
    if (n < 1)
        return;
    uint8_t status = m[0] & 0xf0;
    W->midi_in++;
    if (n >= 3 && status == 0x90)
        W->held[m[0] & 15][m[1] & 127] = m[2] > 0;
    else if (n >= 3 && status == 0x80)
        W->held[m[0] & 15][m[1] & 127] = 0;
    if (!W->has_note_port)
        return;
    bool is_note = (status == 0x90 || status == 0x80) && n >= 3;
    if (W->dialect_midi) {
        if (m[0] >= 0xf0)
            return; // system/sysex: not forwarded yet
        auto *e = ev_alloc<clap_event_midi>();
        if (!e) return;
        e->header = {sizeof(*e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
        e->port_index = W->note_port;
        for (size_t i = 0; i < 3 && i < n; i++)
            e->data[i] = m[i];
        return;
    }
    if (W->dialect_clap && is_note) {
        auto *e = ev_alloc<clap_event_note>();
        if (!e) return;
        bool on = status == 0x90 && m[2] > 0;
        e->header = {sizeof(*e), time, CLAP_CORE_EVENT_SPACE_ID,
                     (uint16_t)(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
        e->note_id = -1;
        e->port_index = (int16_t)W->note_port;
        e->channel = m[0] & 0x0f;
        e->key = m[1];
        e->velocity = on ? m[2] / 127.0 : 0.0;
    }
}

// ================================================================ process
void silence(uint32_t n)
{
    for (auto &g : W->outs)
        for (void *p : g.pw)
            if (auto *b = (float *)pw_filter_get_dsp_buffer(p, n))
                memset(b, 0, n * sizeof(float));
}

void drain_midi_port(uint32_t n, bool deliver)
{
    pw_buffer *b = pw_filter_dequeue_buffer(W->midi_port);
    if (!b)
        return;
    spa_data *d = &b->buffer->datas[0];
    if (d->data && deliver) {
        spa_pod_parser parser;
        spa_pod_frame frame;
        spa_pod_sequence seq;
        const void *seq_body, *c_body;
        spa_pod_control c;
        spa_pod_parser_init_from_data(&parser, d->data, d->maxsize, d->chunk->offset, d->chunk->size);
        if (spa_pod_parser_push_sequence_body(&parser, &frame, &seq, &seq_body) >= 0) {
            while (spa_pod_parser_get_control_body(&parser, &c, &c_body) >= 0) {
                uint32_t t = std::min<uint32_t>(c.offset, n ? n - 1 : 0);
                if (c.type == SPA_CONTROL_Midi) {
                    push_midi_event(t, (const uint8_t *)c_body, c.value.size);
                } else if (c.type == SPA_CONTROL_UMP) {
                    const uint32_t *u = (const uint32_t *)c_body;
                    size_t usize = c.value.size;
                    uint64_t state = 0;
                    while (usize > 0) {
                        uint8_t midi[16];
                        int sz = spa_ump_to_midi(&u, &usize, midi, sizeof(midi), &state);
                        if (sz < 0) break;
                        if (sz > 0) push_midi_event(t, midi, (size_t)sz);
                    }
                }
            }
        }
    }
    pw_filter_queue_buffer(W->midi_port, b);
}

void on_process(void *, spa_io_position *pos)
{
    Worker *w = W;
    t_audio_thread = true;
    uint32_t n = pos->clock.duration;
    uint32_t rate = pos->clock.rate.denom;
    w->cycles++;
    w->last_process_ns.store(now_ns(), std::memory_order_relaxed);
    if (rate != w->graph_rate.load(std::memory_order_relaxed))
        w->graph_rate.store(rate);

    bool ok = w->active.load(std::memory_order_acquire) && rate == w->active_rate && n <= kMaxFrames;
    if (ok && !w->processing) {
        if (w->plugin->start_processing(w->plugin))
            w->processing = true;
        else
            ok = false;
    }
    if (!ok) {
        silence(n);
        drain_midi_port(n, false);
        CtlEvent ce;
        uint64_t dropped_ctl = 0;
        while (w->to_audio.pop(ce)) dropped_ctl++; // stale when inactive
        if (dropped_ctl)
            w->ctl_consumed.fetch_add(dropped_ctl, std::memory_order_release);
        if (w->active && rate != w->active_rate && !w->want_reactivate.exchange(true))
            pw_loop_signal_event(pw_main_loop_get_loop(w->loop), w->main_event);
        return;
    }

    // Events: control changes first (time 0), then MIDI in timeline order.
    w->n_evs = 0;
    CtlEvent ce;
    uint64_t popped = 0;
    while (w->to_audio.pop(ce)) {
        popped++;
        if (ce.kind == CtlEvent::ParamValue) {
            auto *e = ev_alloc<clap_event_param_value>();
            if (!e) break;
            e->header = {sizeof(*e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            e->param_id = ce.id;
            e->cookie = ce.cookie;
            e->note_id = -1;
            e->port_index = -1;
            e->channel = -1;
            e->key = -1;
            e->value = ce.value;
        } else if (ce.kind == CtlEvent::Midi) {
            push_midi_event(0, ce.midi, 3);
        } else { // ReleaseAll: a MIDI source went away with keys down
            for (int ch = 0; ch < 16; ch++)
                for (int k = 0; k < 128; k++)
                    if (w->held[ch][k]) {
                        uint8_t off[3] = {(uint8_t)(0x80 | ch), (uint8_t)k, 0};
                        push_midi_event(0, off, 3);
                        w->released++;
                    }
        }
    }
    drain_midi_port(n, true);

    for (size_t gi = 0; gi < w->ins.size(); gi++) {
        auto &g = w->ins[gi];
        for (size_t c = 0; c < g.pw.size(); c++) {
            auto *b = (float *)pw_filter_get_dsp_buffer(g.pw[c], n);
            g.ptrs[c] = b ? b : w->zeros.data();
        }
        w->in_bufs[gi] = {g.ptrs.data(), nullptr, g.channels, 0, 0};
    }
    for (size_t gi = 0; gi < w->outs.size(); gi++) {
        auto &g = w->outs[gi];
        for (size_t c = 0; c < g.pw.size(); c++) {
            auto *b = (float *)pw_filter_get_dsp_buffer(g.pw[c], n);
            g.ptrs[c] = b ? b : w->trash.data();
        }
        w->out_bufs[gi] = {g.ptrs.data(), nullptr, g.channels, 0, 0};
    }

    clap_process p{};
    p.steady_time = (int64_t)w->steady;
    p.frames_count = n;
    p.transport = nullptr; // TODO: from the Backline clock service / spa_io_position segment
    p.audio_inputs = w->in_bufs.data();
    p.audio_outputs = w->out_bufs.data();
    p.audio_inputs_count = (uint32_t)w->in_bufs.size();
    p.audio_outputs_count = (uint32_t)w->out_bufs.size();
    p.in_events = &in_events;
    p.out_events = &out_events;
    if (w->plugin->process(w->plugin, &p) == CLAP_PROCESS_ERROR)
        silence(n);
    w->steady += n;
    if (popped)
        w->ctl_consumed.fetch_add(popped, std::memory_order_release);
}

int reconcile_port(void *, const bl_port *p);

void on_state_changed(void *, pw_filter_state, pw_filter_state state, const char *error)
{
    if (state == PW_FILTER_STATE_ERROR)
        LOG("filter error: %s\n", error ? error : "?");
    if ((state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING) &&
        W->node_id == SPA_ID_INVALID) {
        W->node_id = pw_filter_get_node_id(W->filter);
        if (W->graph)
            bl_graph_for_each_port(W->graph, reconcile_port, nullptr);
        if (W->graph)
            bl_graph_for_each_port(W->graph, reconcile_port, nullptr); // own ports first, then peers
    }
}

const pw_filter_events filter_events = {
    .version = PW_VERSION_FILTER_EVENTS,
    .state_changed = on_state_changed,
    .process = on_process,
};

// =========================================================== main thread
int do_stop_processing(spa_loop *, bool, uint32_t, const void *, size_t, void *)
{
    t_audio_thread = true;
    if (W->processing) {
        W->plugin->stop_processing(W->plugin);
        W->processing = false;
    }
    return 0;
}

void stop_audio()
{
    W->active.store(false, std::memory_order_release);
    if (pw_loop *dl = W->filter ? pw_filter_get_data_loop(W->filter) : nullptr)
        pw_loop_invoke(dl, do_stop_processing, 0, nullptr, 0, true, nullptr);
}

void activate(uint32_t rate)
{
    if (!W->plugin->activate(W->plugin, rate, 1, kMaxFrames))
        die("plugin failed to activate at %u Hz", rate);
    W->active_rate = rate;
    W->active.store(true, std::memory_order_release);
    LOG("activated at %u Hz, up to %u frames\n", rate, kMaxFrames);
}

void reactivate(uint32_t rate)
{
    stop_audio();
    W->plugin->deactivate(W->plugin);
    activate(rate);
}

void rescan_params()
{
    W->params.clear();
    W->param_index.clear();
    if (!W->ext_params)
        return;
    uint32_t n = W->ext_params->count(W->plugin);
    W->values.reset(new std::atomic<double>[n]);
    for (uint32_t i = 0; i < n; i++) {
        clap_param_info info{};
        if (!W->ext_params->get_info(W->plugin, i, &info))
            continue;
        double v = info.default_value;
        W->ext_params->get_value(W->plugin, info.id, &v);
        W->values[W->params.size()].store(v);
        W->param_index[info.id] = W->params.size();
        W->params.push_back({info.id, info.name, info.module, info.min_value, info.max_value,
                             info.default_value, info.flags, info.cookie});
    }
}

const Param *find_param(const std::string &key)
{
    char *end = nullptr;
    unsigned long id = strtoul(key.c_str(), &end, 10);
    if (!key.empty() && end && *end == 0) {
        auto it = W->param_index.find((clap_id)id);
        return it == W->param_index.end() ? nullptr : &W->params[it->second];
    }
    const Param *sub = nullptr;
    int nsub = 0;
    for (auto &p : W->params) {
        if (!strcasecmp(p.name.c_str(), key.c_str()))
            return &p;
        std::string full = p.module.empty() ? p.name : p.module + "/" + p.name;
        if (!strcasecmp(full.c_str(), key.c_str()))
            return &p;
        if (strcasestr(full.c_str(), key.c_str())) {
            sub = &p;
            nsub++;
        }
    }
    return nsub == 1 ? sub : nullptr;
}

double param_value(const Param &p)
{
    double v = W->values[W->param_index[p.id]].load();
    if (W->ext_params && !W->active.load())
        W->ext_params->get_value(W->plugin, p.id, &v);
    return v;
}

// Main thread. Clamps, then delivers: through the audio thread while the
// graph runs, or by params.flush on the main thread while inactive.
int set_param(const Param &p, double v)
{
    if (p.flags & CLAP_PARAM_IS_READONLY)
        return -EPERM;
    v = std::clamp(v, p.min, p.max);
    if (p.flags & CLAP_PARAM_IS_STEPPED)
        v = std::round(v);
    W->values[W->param_index[p.id]].store(v);
    mark_dirty();
    if (!W->active.load()) {
        if (W->ext_params) {
            W->n_evs = 0;
            auto *e = ev_alloc<clap_event_param_value>();
            e->header = {sizeof(*e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            e->param_id = p.id;
            e->cookie = p.cookie;
            e->note_id = -1;
            e->port_index = e->channel = e->key = -1;
            e->value = v;
            W->ext_params->flush(W->plugin, &in_events, &out_events);
            W->n_evs = 0;
        }
        return 0;
    }
    if (!W->to_audio.push({CtlEvent::ParamValue, {}, p.id, p.cookie, v}))
        return -EAGAIN;
    W->ctl_needed = ++W->ctl_pushed;
    return 0;
}

int send_midi(uint8_t a, uint8_t b, uint8_t c)
{
    if (!W->to_audio.push({CtlEvent::Midi, {a, b, c}, 0, nullptr, 0}))
        return -EAGAIN;
    W->ctl_pushed++;
    return 0;
}

void on_main_event(void *, uint64_t)
{
    if (W->want_reactivate.exchange(false)) {
        uint32_t r = W->graph_rate.load();
        if (!r) r = W->active_rate;
        LOG("re-activating at %u Hz\n", r);
        reactivate(r);
    }
    if (W->want_callback.exchange(false)) {
        W->plugin->on_main_thread(W->plugin);
        // params.request_flush while inactive: flush here (while active, the
        // next process() call delivers and collects parameter events anyway)
        if (W->ext_params && !W->active.load()) {
            W->n_evs = 0;
            W->ext_params->flush(W->plugin, &in_events, &out_events);
        }
    }
}

bool save_state_to(const std::string &path, std::string *err);
void autosave(bool force);

void on_housekeeping(void *, uint64_t)
{
    OutParam op;
    bool changed = false;
    while (W->from_audio.pop(op)) {
        auto it = W->param_index.find(op.id);
        if (it != W->param_index.end() && W->values[it->second].load() != op.value) {
            W->values[it->second].store(op.value);
            changed = true;
        }
    }
    if (changed)
        mark_dirty();
    autosave(false);
    if (W->bus) {
        if (W->announced_serial != W->state_serial + W->state_saved) {
            W->announced_serial = W->state_serial + W->state_saved;
            sd_bus_emit_properties_changed(W->bus, kObjectPath, kInterface, "StateSerial", "StateSaved", nullptr);
        }
        while (sd_bus_process(W->bus, nullptr) > 0) {}
    }
}

// ---------------------------------------------------------------- linking
int reconcile_port(void *, const bl_port *p)
{
    Worker *w = W;
    if (w->node_id == SPA_ID_INVALID)
        return 0;
    if (p->node_id == w->node_id) {
        w->own_ports[p->name] = p->id;
        if (!strcmp(p->name, "midi_in"))
            w->midi_port_id = p->id;
        return 0;
    }
    // MIDI follow: any MIDI source that matches (hotplugged keyboards too).
    if (w->midi_re && p->midi && p->output && w->midi_port_id != SPA_ID_INVALID) {
        std::string hay = std::string(p->node_name) + ":" + p->name + " | " + p->alias;
        if (std::regex_search(hay, *w->midi_re) && !bl_graph_is_linked(w->graph, p->id, w->midi_port_id)) {
            LOG("midi-follow: linking %s\n", hay.c_str());
            bl_graph_link(w->graph, p->id, w->midi_port_id);
            w->midi_sources.push_back(p->id);
        }
    }
    // Audio target: our main outputs to the target's playback inputs, by channel.
    if (!w->audio_target.empty() && !p->output && !p->midi && !p->monitor &&
        !strcmp(p->node_name, w->audio_target.c_str()) && !w->outs.empty()) {
        auto &g = w->outs[0];
        for (size_t c = 0; c < g.names.size(); c++) {
            auto own = w->own_ports.find(g.names[c]);
            if (own == w->own_ports.end())
                continue;
            bool match = !strcmp(p->channel, g.pos[c].c_str()) ||
                         (g.channels == 1 && (!strcmp(p->channel, "FL") || !strcmp(p->channel, "FR")));
            if (match && !bl_graph_is_linked(w->graph, own->second, p->id)) {
                LOG("audio-target: linking %s -> %s:%s\n", g.names[c].c_str(), p->node_name, p->name);
                bl_graph_link(w->graph, own->second, p->id);
            }
        }
    }
    return 0;
}

void on_port_added(void *, const bl_port *p)
{
    if (W->node_id == SPA_ID_INVALID && W->filter)
        W->node_id = pw_filter_get_node_id(W->filter);
    if (p->node_id == W->node_id) {
        reconcile_port(nullptr, p);
        // our port appeared: (re)try everything else against it
        bl_graph_for_each_port(W->graph, reconcile_port, nullptr);
    } else {
        reconcile_port(nullptr, p);
    }
}

// A MIDI source we were following went away (a keyboard unplugged, an app
// quit). Its note-offs may never arrive, so release whatever it held.
void on_port_removed(void *, uint32_t id)
{
    auto &v = W->midi_sources;
    auto it = std::find(v.begin(), v.end(), id);
    if (it == v.end())
        return;
    v.erase(it);
    LOG("midi-follow: source port %u went away; releasing held notes\n", id);
    if (W->to_audio.push({CtlEvent::ReleaseAll, {}, 0, nullptr, 0}))
        W->ctl_pushed++;
}

const bl_graph_events graph_events = {on_port_added, on_port_removed};

void on_core_info(void *, const pw_core_info *info)
{
    if (info->props)
        if (const char *r = spa_dict_lookup(info->props, "default.clock.rate"))
            W->server_rate = (uint32_t)atoi(r);
}
void on_core_error(void *, uint32_t id, int, int res, const char *message)
{
    LOG("core error on %u: %s (%d)\n", id, message, res);
    if (id == PW_ID_CORE && res == -EPIPE)
        pw_main_loop_quit(W->loop);
}
int pending_sync = -1;
void on_core_done(void *, uint32_t id, int seq)
{
    if (id == PW_ID_CORE && seq == pending_sync)
        pw_main_loop_quit(W->loop);
}
const pw_core_events core_events = {.version = PW_VERSION_CORE_EVENTS, .info = on_core_info, .done = on_core_done, .error = on_core_error};

// --------------------------------------------------------------------- OSC
void osc_reply(const sockaddr_in &to, const std::vector<uint8_t> &b)
{
    sendto(W->osc_fd, b.data(), b.size(), 0, (const sockaddr *)&to, sizeof(to));
}

void osc_reply_param(const sockaddr_in &to, const char *addr, const Param &p)
{
    osc_reply(to, osc::Writer(addr).i((int32_t)p.id).s(p.module.empty() ? p.name : p.module + "/" + p.name)
                      .d(param_value(p)).d(p.min).d(p.max).bytes());
}

void on_osc(void *, int fd, uint32_t)
{
    uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    ssize_t len;
    while ((len = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, (sockaddr *)&from, &fl)) > 0) {
        osc::Message m;
        if (!osc::parse(buf, (size_t)len, m))
            continue;
        std::string a = m.address;
        if (a.rfind("/backline", 0) == 0)
            a = a.substr(9);
        auto key_of = [&](const osc::Arg &arg) -> std::string {
            if (auto s = std::get_if<std::string>(&arg)) return *s;
            if (auto i = std::get_if<int32_t>(&arg)) return std::to_string(*i);
            return {};
        };
        double v;
        if (a == "/param/set" && m.args.size() == 2 && osc::as_double(m.args[1], v)) {
            if (const Param *p = find_param(key_of(m.args[0])))
                set_param(*p, v);
        } else if (a.rfind("/param/", 0) == 0 && a != "/param/get" && a != "/param/list" &&
                   m.args.size() == 1 && osc::as_double(m.args[0], v)) {
            if (const Param *p = find_param(a.substr(7)))
                set_param(*p, v);
        } else if (a == "/param/get" && m.args.size() == 1) {
            if (const Param *p = find_param(key_of(m.args[0])))
                osc_reply_param(from, "/backline/param", *p);
        } else if (a == "/param/list") {
            for (auto &p : W->params)
                osc_reply_param(from, "/backline/param", p);
            osc_reply(from, osc::Writer("/backline/param/end").i((int32_t)W->params.size()).bytes());
        } else if (a == "/note/on" && m.args.size() == 3) {
            double ch, k, vel;
            if (osc::as_double(m.args[0], ch) && osc::as_double(m.args[1], k) && osc::as_double(m.args[2], vel))
                send_midi(0x90 | ((int)ch & 15), (int)k & 127, (int)vel & 127);
        } else if (a == "/note/off" && m.args.size() >= 2) {
            double ch, k;
            if (osc::as_double(m.args[0], ch) && osc::as_double(m.args[1], k))
                send_midi(0x80 | ((int)ch & 15), (int)k & 127, 0);
        }
    }
}

void setup_osc()
{
    W->osc_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)W->osc_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only, by design
    if (bind(W->osc_fd, (sockaddr *)&a, sizeof(a)) < 0)
        die("OSC bind 127.0.0.1:%d: %s", W->osc_port, strerror(errno));
    socklen_t al = sizeof(a);
    getsockname(W->osc_fd, (sockaddr *)&a, &al);
    W->osc_port = ntohs(a.sin_port);
    W->osc_source = pw_loop_add_io(pw_main_loop_get_loop(W->loop), W->osc_fd, SPA_IO_IN, false, on_osc, nullptr);
}

// ------------------------------------------------------------------- D-Bus
int m_list_params(sd_bus_message *m, void *, sd_bus_error *)
{
    sd_bus_message *r = nullptr;
    sd_bus_message_new_method_return(m, &r);
    sd_bus_message_open_container(r, 'a', "(ussddd)");
    for (auto &p : W->params)
        sd_bus_message_append(r, "(ussddd)", p.id, p.name.c_str(), p.module.c_str(), param_value(p), p.min, p.max);
    sd_bus_message_close_container(r);
    int ret = sd_bus_send(nullptr, r, nullptr);
    sd_bus_message_unref(r);
    return ret;
}
int m_get_param(sd_bus_message *m, void *, sd_bus_error *e)
{
    const char *key;
    int r = sd_bus_message_read(m, "s", &key);
    if (r < 0) return r;
    const Param *p = find_param(key);
    if (!p) return sd_bus_error_setf(e, "io.github.manuaudio.Backline.Error.NoSuchParam", "no unique parameter matches '%s'", key);
    return sd_bus_reply_method_return(m, "d", param_value(*p));
}
int m_set_param(sd_bus_message *m, void *, sd_bus_error *e)
{
    const char *key;
    double v;
    int r = sd_bus_message_read(m, "sd", &key, &v);
    if (r < 0) return r;
    const Param *p = find_param(key);
    if (!p) return sd_bus_error_setf(e, "io.github.manuaudio.Backline.Error.NoSuchParam", "no unique parameter matches '%s'", key);
    if ((r = set_param(*p, v)) < 0) return sd_bus_error_set_errno(e, r);
    return sd_bus_reply_method_return(m, "d", std::clamp(v, p->min, p->max));
}
int m_note(sd_bus_message *m, void *, sd_bus_error *e, bool on)
{
    uint8_t ch, key, vel = 0;
    int r = on ? sd_bus_message_read(m, "yyy", &ch, &key, &vel) : sd_bus_message_read(m, "yy", &ch, &key);
    if (r < 0) return r;
    if ((r = send_midi((on ? 0x90 : 0x80) | (ch & 15), key & 127, vel & 127)) < 0) return sd_bus_error_set_errno(e, r);
    return sd_bus_reply_method_return(m, "");
}
int m_note_on(sd_bus_message *m, void *u, sd_bus_error *e) { return m_note(m, u, e, true); }
int m_note_off(sd_bus_message *m, void *u, sd_bus_error *e) { return m_note(m, u, e, false); }

int64_t st_write(const clap_ostream *s, const void *buf, uint64_t size) { return (int64_t)fwrite(buf, 1, size, (FILE *)s->ctx); }
int64_t st_read(const clap_istream *s, void *buf, uint64_t size) { return (int64_t)fread(buf, 1, size, (FILE *)s->ctx); }

void mark_dirty()
{
    if (W->state_serial == W->state_saved)
        W->dirty_since_ns = now_ns();
    W->state_serial++;
}

// Writes the plugin's state to PATH atomically (temp file, fsync, rename), so
// a crash during a save leaves the previous state intact. Main thread.
bool save_state_to(const std::string &path, std::string *err)
{
    if (!W->ext_state) {
        if (err) *err = "plugin has no state extension";
        return false;
    }
    std::string tmp = path + ".tmp." + std::to_string(getpid());
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) {
        if (err) *err = std::string("open ") + tmp + ": " + strerror(errno);
        return false;
    }
    clap_ostream os{f, st_write};
    bool ok = W->ext_state->save(W->plugin, &os);
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    fclose(f);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        if (err) *err = ok ? std::string("rename: ") + strerror(errno) : "plugin failed to save state";
        return false;
    }
    return true;
}

bool load_state_from(const std::string &path, std::string *err)
{
    if (!W->ext_state) {
        if (err) *err = "plugin has no state extension";
        return false;
    }
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = std::string("open ") + path + ": " + strerror(errno);
        return false;
    }
    clap_istream is{f, st_read};
    bool ok = W->ext_state->load(W->plugin, &is);
    fclose(f);
    if (!ok) {
        if (err) *err = "plugin failed to load state";
        return false;
    }
    rescan_params();
    return true;
}

// --state-file: save once the state has been quiet for 100 ms (so a burst of
// parameter moves is one write), but at least every second while it keeps
// changing. Main thread (housekeeping timer).
void autosave(bool force)
{
    if (W->state_file.empty() || W->state_serial == W->state_saved)
        return;
    uint64_t now = now_ns();
    bool quiet = now - W->dirty_since_ns > 100000000ull;
    bool overdue = now - W->dirty_since_ns > 1000000000ull && now - W->last_save_ns > 1000000000ull;
    // Every parameter change must have reached the plugin first. If the graph
    // stopped running, events wait in the ring; after 2 s save what we have.
    bool delivered = W->ctl_consumed.load(std::memory_order_acquire) >= W->ctl_needed;
    bool stuck = now - W->dirty_since_ns > 2000000000ull;
    if (!force && ((!quiet && !overdue) || (!delivered && !stuck)))
        return;
    if (!delivered && !force)
        LOG("autosave: %llu parameter change(s) not yet processed by the plugin (graph not running?); saving anyway\n",
            (unsigned long long)(W->ctl_needed - W->ctl_consumed.load()));
    uint64_t serial = W->state_serial;
    std::string err;
    if (save_state_to(W->state_file, &err)) {
        W->state_saved = serial;
        W->state_saves++;
        W->last_save_ns = now;
        W->dirty_since_ns = now;
    } else {
        LOG("autosave to %s failed: %s\n", W->state_file.c_str(), err.c_str());
        W->last_save_ns = now; // retry in a second, not every tick
    }
}

int m_save_state(sd_bus_message *m, void *, sd_bus_error *e)
{
    const char *path;
    int r = sd_bus_message_read(m, "s", &path);
    if (r < 0) return r;
    std::string err;
    uint64_t serial = W->state_serial;
    if (!save_state_to(path, &err)) return sd_bus_error_set(e, SD_BUS_ERROR_FAILED, err.c_str());
    if (W->state_file == path) W->state_saved = serial;
    return sd_bus_reply_method_return(m, "");
}
int m_load_state(sd_bus_message *m, void *, sd_bus_error *e)
{
    const char *path;
    int r = sd_bus_message_read(m, "s", &path);
    if (r < 0) return r;
    std::string err;
    if (!load_state_from(path, &err)) return sd_bus_error_set(e, SD_BUS_ERROR_FAILED, err.c_str());
    mark_dirty();
    return sd_bus_reply_method_return(m, "");
}

int p_str(sd_bus *, const char *, const char *, const char *prop, sd_bus_message *r, void *, sd_bus_error *)
{
    if (!strcmp(prop, "PluginId")) return sd_bus_message_append(r, "s", W->plugin->desc->id);
    if (!strcmp(prop, "PluginName")) return sd_bus_message_append(r, "s", W->plugin_name.c_str());
    if (!strcmp(prop, "PluginPath")) return sd_bus_message_append(r, "s", W->plugin_path.c_str());
    if (!strcmp(prop, "StateFile")) return sd_bus_message_append(r, "s", W->state_file.c_str());
    return sd_bus_message_append(r, "s", W->node_name.c_str());
}
int p_u(sd_bus *, const char *, const char *, const char *prop, sd_bus_message *r, void *, sd_bus_error *)
{
    if (!strcmp(prop, "NodeId")) return sd_bus_message_append(r, "u", W->node_id);
    if (!strcmp(prop, "OscPort")) return sd_bus_message_append(r, "u", (uint32_t)W->osc_port);
    if (!strcmp(prop, "SampleRate")) return sd_bus_message_append(r, "u", W->active_rate);
    return sd_bus_message_append(r, "u", 0u);
}
int p_t(sd_bus *, const char *, const char *, const char *prop, sd_bus_message *r, void *, sd_bus_error *)
{
    if (!strcmp(prop, "Cycles")) return sd_bus_message_append(r, "t", W->cycles.load());
    if (!strcmp(prop, "MidiEvents")) return sd_bus_message_append(r, "t", W->midi_in.load());
    if (!strcmp(prop, "ReleasedNotes")) return sd_bus_message_append(r, "t", W->released.load());
    if (!strcmp(prop, "StateSerial")) return sd_bus_message_append(r, "t", W->state_serial);
    if (!strcmp(prop, "StateSaved")) return sd_bus_message_append(r, "t", W->state_saved);
    if (!strcmp(prop, "StateSaves")) return sd_bus_message_append(r, "t", W->state_saves);
    return sd_bus_message_append(r, "t", W->dropped.load());
}

const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_ARGS("ListParams", SD_BUS_NO_ARGS, SD_BUS_RESULT("a(ussddd)", params), m_list_params, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("GetParam", SD_BUS_ARGS("s", key), SD_BUS_RESULT("d", value), m_get_param, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("SetParam", SD_BUS_ARGS("s", key, "d", value), SD_BUS_RESULT("d", applied), m_set_param, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("NoteOn", SD_BUS_ARGS("y", channel, "y", key, "y", velocity), SD_BUS_NO_RESULT, m_note_on, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("NoteOff", SD_BUS_ARGS("y", channel, "y", key), SD_BUS_NO_RESULT, m_note_off, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("SaveState", SD_BUS_ARGS("s", path), SD_BUS_NO_RESULT, m_save_state, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("LoadState", SD_BUS_ARGS("s", path), SD_BUS_NO_RESULT, m_load_state, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("PluginId", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PluginName", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PluginPath", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("NodeName", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("NodeId", "u", p_u, 0, 0),
    SD_BUS_PROPERTY("OscPort", "u", p_u, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SampleRate", "u", p_u, 0, 0),
    SD_BUS_PROPERTY("Cycles", "t", p_t, 0, 0),
    SD_BUS_PROPERTY("MidiEvents", "t", p_t, 0, 0),
    SD_BUS_PROPERTY("DroppedEvents", "t", p_t, 0, 0),
    SD_BUS_PROPERTY("ReleasedNotes", "t", p_t, 0, 0),
    SD_BUS_PROPERTY("StateFile", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("StateSerial", "t", p_t, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("StateSaved", "t", p_t, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("StateSaves", "t", p_t, 0, 0),
    SD_BUS_VTABLE_END,
};

void on_bus_io(void *, int, uint32_t)
{
    while (sd_bus_process(W->bus, nullptr) > 0) {}
}

void setup_dbus()
{
    if (sd_bus_open_user(&W->bus) < 0) {
        LOG("no session bus; D-Bus control disabled\n");
        W->bus = nullptr;
        return;
    }
    std::string s;
    for (char c : W->node_name)
        s += isalnum((unsigned char)c) ? c : '_';
    if (s.empty() || isdigit((unsigned char)s[0]))
        s = "n_" + s;
    W->bus_name = kBusPrefix + s;
    sd_bus_add_object_vtable(W->bus, &W->bus_slot, kObjectPath, kInterface, vtable, nullptr);
    int r = sd_bus_request_name(W->bus, W->bus_name.c_str(), 0);
    if (r < 0)
        die("D-Bus name %s: %s (is another instance with this node name running?)", W->bus_name.c_str(), strerror(-r));
    W->bus_source = pw_loop_add_io(pw_main_loop_get_loop(W->loop), sd_bus_get_fd(W->bus), SPA_IO_IN, false, on_bus_io, nullptr);
}

// ------------------------------------------------------------------- CLAP
std::string resolve_plugin(const std::string &p)
{
    struct stat st;
    if (stat(p.c_str(), &st) == 0)
        return p;
    const char *home = getenv("HOME");
    for (std::string dir : {std::string(home ? home : "") + "/.clap", std::string("/usr/lib/clap")}) {
        std::string c = dir + "/" + p + (p.size() > 5 && p.substr(p.size() - 5) == ".clap" ? "" : ".clap");
        if (stat(c.c_str(), &st) == 0)
            return c;
    }
    die("plugin not found: %s (looked in ~/.clap and /usr/lib/clap)", p.c_str());
}

bool has_feature(const clap_plugin_descriptor *d, const char *f)
{
    for (auto *x = d->features; x && *x; x++)
        if (!strcmp(*x, f))
            return true;
    return false;
}

void load_plugin()
{
    W->plugin_path = resolve_plugin(W->plugin_path);
    W->dl = dlopen(W->plugin_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!W->dl)
        die("dlopen: %s", dlerror());
    W->entry = (const clap_plugin_entry *)dlsym(W->dl, "clap_entry");
    if (!W->entry || !clap_version_is_compatible(W->entry->clap_version))
        die("%s: no compatible clap_entry", W->plugin_path.c_str());
    if (!W->entry->init(W->plugin_path.c_str()))
        die("clap_entry.init failed");
    auto *f = (const clap_plugin_factory *)W->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!f)
        die("no plugin factory");
    uint32_t n = f->get_plugin_count(f);
    const clap_plugin_descriptor *chosen = nullptr;
    if (W->list_only) {
        printf("[\n");
        for (uint32_t i = 0; i < n; i++) {
            auto *d = f->get_plugin_descriptor(f, i);
            printf("  {\"id\": \"%s\", \"name\": \"%s\", \"vendor\": \"%s\", \"features\": [", d->id, d->name, d->vendor ? d->vendor : "");
            for (auto *x = d->features; x && *x; x++)
                printf("%s\"%s\"", x == d->features ? "" : ", ", *x);
            printf("]}%s\n", i + 1 < n ? "," : "");
        }
        printf("]\n");
        exit(0);
    }
    for (uint32_t i = 0; i < n; i++) {
        auto *d = f->get_plugin_descriptor(f, i);
        if (!W->plugin_id.empty() ? W->plugin_id == d->id : has_feature(d, CLAP_PLUGIN_FEATURE_INSTRUMENT)) {
            chosen = d;
            break;
        }
    }
    if (!chosen && W->plugin_id.empty() && n)
        chosen = f->get_plugin_descriptor(f, 0);
    if (!chosen)
        die("plugin id '%s' not in %s", W->plugin_id.c_str(), W->plugin_path.c_str());
    W->plugin_name = chosen->name;

    W->host = {CLAP_VERSION_INIT, W, "Backline plugin host", "manuaudio", "https://github.com/manuaudio/backline-audio", "0.1.0",
               host_get_extension, host_request_restart, host_request_process, host_request_callback};
    W->plugin = f->create_plugin(f, &W->host, chosen->id);
    if (!W->plugin || !W->plugin->init(W->plugin))
        die("could not create/init %s", chosen->id);
    W->ext_params = (const clap_plugin_params *)W->plugin->get_extension(W->plugin, CLAP_EXT_PARAMS);
    W->ext_audio_ports = (const clap_plugin_audio_ports *)W->plugin->get_extension(W->plugin, CLAP_EXT_AUDIO_PORTS);
    W->ext_note_ports = (const clap_plugin_note_ports *)W->plugin->get_extension(W->plugin, CLAP_EXT_NOTE_PORTS);
    W->ext_state = (const clap_plugin_state *)W->plugin->get_extension(W->plugin, CLAP_EXT_STATE);
    W->ext_timer = (const clap_plugin_timer_support *)W->plugin->get_extension(W->plugin, CLAP_EXT_TIMER_SUPPORT);

    if (W->ext_note_ports && W->ext_note_ports->count(W->plugin, true) > 0) {
        clap_note_port_info ni{};
        W->ext_note_ports->get(W->plugin, 0, true, &ni);
        W->has_note_port = true;
        W->note_port = 0;
        W->dialect_midi = ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI;
        W->dialect_clap = ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP;
    }
    rescan_params();
    LOG("loaded %s (%s) from %s: %zu params, note input: %s\n", chosen->name, chosen->id, W->plugin_path.c_str(),
        W->params.size(), W->has_note_port ? (W->dialect_midi ? "midi" : "clap") : "none");
}

void add_audio_ports(bool input, std::vector<PortGroup> &groups)
{
    uint32_t n = W->ext_audio_ports ? W->ext_audio_ports->count(W->plugin, input) : 0;
    for (uint32_t i = 0; i < n; i++) {
        clap_audio_port_info info{};
        if (!W->ext_audio_ports->get(W->plugin, i, input, &info))
            continue;
        PortGroup g{i, info.channel_count, {}, {}, {}, {}};
        bool stereo = info.channel_count == 2;
        for (uint32_t c = 0; c < info.channel_count; c++) {
            std::string pos = stereo ? (c ? "FR" : "FL") : info.channel_count == 1 ? "MONO" : "AUX" + std::to_string(c);
            std::string base = input ? "in" : "out";
            std::string name = (i == 0 ? base : base + std::to_string(i)) + "_" + pos;
            void *port = pw_filter_add_port(W->filter, input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                                            PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
                                            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                                              PW_KEY_PORT_NAME, name.c_str(),
                                                              PW_KEY_AUDIO_CHANNEL, pos.c_str(), nullptr),
                                            nullptr, 0);
            g.pw.push_back(port);
            g.names.push_back(name);
            g.pos.push_back(pos);
        }
        g.ptrs.resize(g.channels);
        groups.push_back(std::move(g));
    }
}

void on_signal(void *, int) { pw_main_loop_quit(W->loop); }

std::string json_str(const std::string &v)
{
    std::string o = "\"";
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

void usage(FILE *f)
{
    fputs("usage: backline-plugin-worker --plugin PATH|NAME [options]\n"
          "  -p, --plugin PATH|NAME   .clap file, or a name in ~/.clap or /usr/lib/clap\n"
          "  -i, --id ID              plugin id inside the file (default: first instrument)\n"
          "  -n, --name NODE          node.name (default: backline.<plugin id>)\n"
          "  -m, --midi-follow REGEX  link every MIDI source matching REGEX, now and when hotplugged\n"
          "  -t, --audio-target NODE  link the main output to NODE's playback ports\n"
          "  -o, --osc-port PORT      OSC on 127.0.0.1:PORT (default: any free port)\n"
          "  -a, --autoconnect        let the session manager link the output to the default\n"
          "                           output (the default unless --audio-target is given)\n"
          "  -A, --no-autoconnect     never let the session manager link the output\n"
          "  -s, --state-file PATH    load the plugin state from PATH at start (if it exists),\n"
          "                           and save it there, atomically, whenever it changes\n"
          "      --no-dbus            no D-Bus control object\n"
          "  -l, --list               print the plugins in the file as JSON and exit\n",
          f);
}

} // namespace

int main(int argc, char **argv)
{
    Worker w;
    W = &w;
    w.main_tid = std::this_thread::get_id();
    static const option opts[] = {
        {"plugin", required_argument, nullptr, 'p'}, {"id", required_argument, nullptr, 'i'},
        {"name", required_argument, nullptr, 'n'}, {"midi-follow", required_argument, nullptr, 'm'},
        {"audio-target", required_argument, nullptr, 't'}, {"osc-port", required_argument, nullptr, 'o'},
        {"autoconnect", no_argument, nullptr, 'a'}, {"no-autoconnect", no_argument, nullptr, 'A'},
        {"state-file", required_argument, nullptr, 's'}, {"no-dbus", no_argument, nullptr, 'D'},
        {"list", no_argument, nullptr, 'l'}, {"help", no_argument, nullptr, 'h'}, {}};
    int c;
    while ((c = getopt_long(argc, argv, "p:i:n:m:t:o:aAs:lh", opts, nullptr)) != -1) {
        switch (c) {
        case 'p': w.plugin_path = optarg; break;
        case 'i': w.plugin_id = optarg; break;
        case 'n': w.node_name = optarg; break;
        case 'm': w.midi_follow = optarg; break;
        case 't': w.audio_target = optarg; break;
        case 'o': w.osc_port = atoi(optarg); break;
        case 'a': w.autoconnect = 1; break;
        case 'A': w.autoconnect = 0; break;
        case 's': w.state_file = optarg; break;
        case 'D': w.use_dbus = false; break;
        case 'l': w.list_only = true; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (w.plugin_path.empty()) {
        usage(stderr);
        return 2;
    }
    if (w.autoconnect < 0)
        w.autoconnect = w.audio_target.empty(); // instruments play to the default output (DECISIONS D2)
    if (!w.midi_follow.empty())
        w.midi_re.emplace(w.midi_follow, std::regex::ECMAScript | std::regex::icase);

    pw_init(&argc, &argv);
    w.loop = pw_main_loop_new(nullptr);
    pw_loop *l = pw_main_loop_get_loop(w.loop);
    pw_loop_add_signal(l, SIGINT, on_signal, nullptr);
    pw_loop_add_signal(l, SIGTERM, on_signal, nullptr);
    w.main_event = pw_loop_add_event(l, on_main_event, nullptr);

    load_plugin();
    if (!w.state_file.empty()) {
        struct stat st;
        std::string err;
        if (stat(w.state_file.c_str(), &st) == 0) {
            if (load_state_from(w.state_file, &err))
                LOG("restored state from %s (%lld bytes, %zu params)\n", w.state_file.c_str(), (long long)st.st_size, w.params.size());
            else
                LOG("could not restore state from %s: %s; starting from defaults\n", w.state_file.c_str(), err.c_str());
        }
    }
    if (w.node_name.empty())
        w.node_name = std::string("backline.") + w.plugin->desc->id;
    setup_osc();
    if (w.use_dbus)
        setup_dbus();

    w.context = pw_context_new(l, nullptr, 0);
    w.core = pw_context_connect(w.context, nullptr, 0);
    if (!w.core)
        die("cannot connect to PipeWire: %s", strerror(errno));
    pw_core_add_listener(w.core, &w.core_listener, &core_events, nullptr);
    w.graph = bl_graph_new(w.core, &graph_events, nullptr);
    pending_sync = pw_core_sync(w.core, PW_ID_CORE, 0); // learn default.clock.rate first
    pw_main_loop_run(w.loop);

    std::string desc = w.plugin_name + " (Backline)";
    std::string osc = std::to_string(w.osc_port);
    bool instrument = has_feature(w.plugin->desc, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    w.filter = pw_filter_new(w.core, w.node_name.c_str(),
                             pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                               PW_KEY_MEDIA_CATEGORY, instrument ? "Playback" : "Filter",
                                               PW_KEY_MEDIA_ROLE, "Production",
                                               PW_KEY_MEDIA_CLASS, instrument ? "Stream/Output/Audio" : "Audio/Filter",
                                               PW_KEY_NODE_NAME, w.node_name.c_str(),
                                               PW_KEY_NODE_DESCRIPTION, desc.c_str(),
                                               PW_KEY_NODE_AUTOCONNECT, w.autoconnect > 0 ? "true" : "false",
                                               PW_KEY_NODE_ALWAYS_PROCESS, "true",
                                               "backline.plugin.format", "clap",
                                               "backline.plugin.id", w.plugin->desc->id,
                                               "backline.plugin.path", w.plugin_path.c_str(),
                                               "backline.osc.port", osc.c_str(),
                                               "backline.dbus.name", w.bus_name.c_str(),
                                               nullptr));
    pw_filter_add_listener(w.filter, &w.filter_listener, &filter_events, nullptr);
    if (w.has_note_port)
        w.midi_port = pw_filter_add_port(w.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
                                         pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi",
                                                           PW_KEY_PORT_NAME, "midi_in", nullptr),
                                         nullptr, 0);
    add_audio_ports(true, w.ins);
    add_audio_ports(false, w.outs);
    w.in_bufs.resize(w.ins.size());
    w.out_bufs.resize(w.outs.size());

    activate(w.server_rate); // re-activated on the first cycle if the graph runs at another rate
    if (pw_filter_connect(w.filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0)
        die("pw_filter_connect failed");

    w.housekeeping = pw_loop_add_timer(l, on_housekeeping, nullptr);
    timespec hk{0, 20 * 1000000L};
    pw_loop_update_timer(l, w.housekeeping, &hk, &hk, false);

    printf("{\"node\": %s, \"plugin\": %s, \"plugin_path\": %s, \"osc_port\": %d, \"dbus\": %s, \"pid\": %d}\n",
           json_str(w.node_name).c_str(), json_str(w.plugin->desc->id).c_str(), json_str(w.plugin_path).c_str(),
           w.osc_port, json_str(w.bus_name).c_str(), getpid());
    fflush(stdout);

    pw_main_loop_run(w.loop);

    LOG("shutting down\n");
    autosave(true);
    stop_audio();
    pw_filter_disconnect(w.filter);
    w.plugin->deactivate(w.plugin);
    w.plugin->destroy(w.plugin); // may unregister its timers itself
    for (auto &[id, s] : w.timers)
        pw_loop_destroy_source(l, s);
    w.timers.clear();
    w.entry->deinit();
    pw_filter_destroy(w.filter);
    bl_graph_destroy(w.graph);
    if (w.bus) {
        sd_bus_release_name(w.bus, w.bus_name.c_str());
        sd_bus_flush_close_unref(w.bus);
    }
    pw_core_disconnect(w.core);
    pw_context_destroy(w.context);
    pw_main_loop_destroy(w.loop);
    pw_deinit();
    return 0;
}
