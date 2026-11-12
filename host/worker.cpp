// backline-plugin-worker: one plugin instance, one PipeWire node, one process.
// SPDX-License-Identifier: MIT
//
// This is the unit of isolation of the Backline plugin host. A crash here
// takes down exactly one node: PipeWire drops this client, its node and the
// links it made, and every other node keeps running.
//
// The plugin sits behind one interface, bl::PluginInstance (plugin.h), with
// one adapter per format: CLAP (plugin_clap.cpp), VST3 (plugin_vst3.cpp) and
// LV2 (plugin_lv2.cpp). Everything here is format-free.
//
// Threads:
//   main   = the PipeWire main loop. The plugin's main-thread calls, D-Bus,
//            OSC, graph watching and linking. Never blocks on the audio
//            thread except for the documented stop handshake (pw_loop_invoke).
//   audio  = PipeWire's data loop (PW_FILTER_FLAG_RT_PROCESS). The plugin's
//            start/stop processing and process. No locks, no allocation,
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
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/control/control.h>
#include <spa/control/ump-utils.h>
#include <spa/pod/parser.h>
#include <spa/utils/string.h>
#include <systemd/sd-bus.h>

#include "../common/bl_graph.h"
#include "osc.h"
#include "plugin.h"
#include "spsc.h"

using bl::kMaxEvents;
using bl::kMaxFrames;
using bl::ParamInfo;
using bl::PresetInfo;

namespace bl {

FILE *out = stdout;

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

std::string slug(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c))
            o += (char)tolower(c);
        else if (!o.empty() && o.back() != '-')
            o += '-';
    }
    while (!o.empty() && o.back() == '-')
        o.pop_back();
    return o.empty() ? "plugin" : o;
}

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

} // namespace bl

namespace {

using bl::die;
using bl::json_str;

constexpr const char *kBusPrefix = "io.github.manuaudio.Backline.Instance.";
constexpr const char *kObjectPath = "/io/github/manuaudio/Backline/Instance";
constexpr const char *kInterface = "io.github.manuaudio.Backline.Instance1";

thread_local bool t_audio_thread = false;

struct CtlEvent {
    enum Kind : uint8_t { ParamValue, Midi, ReleaseAll } kind;
    uint8_t midi[3];
    uint32_t index;   // param index
    uint32_t id;
    void *cookie;
    double value;     // adapter units
};

struct OutParam {
    uint32_t id;
    double value;
};

struct PortGroup {   // one plugin audio bus, as N mono PipeWire DSP ports
    uint32_t channels;
    std::vector<void *> pw;          // pw_filter port data
    std::vector<std::string> names;  // port.name
    std::vector<std::string> pos;    // audio.channel
    std::vector<float *> ptrs;       // per-cycle buffer pointers (preallocated)
};

enum class Fmt { Auto, Clap, Vst3, Lv2 };

struct Worker;
Worker *W = nullptr;

class WorkerHost final : public bl::Host {
public:
    void request_restart() override;
    void request_callback() override;
    void params_changed() override;
    void mark_dirty() override;
    void param_from_plugin(uint32_t id, double value) override;
    int set_param(uint32_t id, double value) override;
    void count_dropped() override;
    bool is_main_thread() override;
    bool is_audio_thread() override { return t_audio_thread; }
    pw_loop *main_loop() override;
    void log(const char *fmt, ...) override __attribute__((format(printf, 2, 3)))
    {
        va_list ap;
        va_start(ap, fmt);
        fputs("backline-plugin-worker: ", stderr);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
};

struct Worker {
    // ---- options
    std::string plugin_arg, plugin_id, node_name, midi_follow, audio_target, audio_source, state_file;
    std::vector<std::string> preset_dirs;
    Fmt fmt = Fmt::Auto;
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
    std::vector<float **> in_ptrs, out_ptrs; // per bus: PortGroup::ptrs.data()
    spa_source *main_event = nullptr, *housekeeping = nullptr;
    uint32_t node_id = SPA_ID_INVALID;
    uint32_t midi_port_id = SPA_ID_INVALID;
    std::unordered_map<std::string, uint32_t> own_ports; // port.name -> global id
    std::optional<std::regex> midi_re;
    uint32_t server_rate = 48000;

    // ---- the plugin
    WorkerHost host;
    std::unique_ptr<bl::PluginInstance> inst;
    std::unordered_map<uint32_t, size_t> param_index;  // id -> index in inst->params
    std::unique_ptr<std::atomic<double>[]> values;     // public units, by index
    std::vector<PresetInfo> presets;
    bool presets_listed = false;

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
    std::vector<bl::ParamEv> pevs = std::vector<bl::ParamEv>(kMaxEvents);
    std::vector<bl::MidiEv> mevs = std::vector<bl::MidiEv>(kMaxEvents);
    uint32_t n_pevs = 0, n_mevs = 0;
    std::vector<float> zeros = std::vector<float>(kMaxFrames, 0.f);
    std::vector<float> trash = std::vector<float>(kMaxFrames, 0.f);
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
    // mark_dirty, parameter values the plugin reports, LoadState, presets.
    uint64_t state_serial = 0, state_saved = 0, state_saves = 0;
    uint64_t dirty_since_ns = 0, last_save_ns = 0;
    uint64_t announced_serial = UINT64_MAX;
    // Control events pushed to the audio thread (main) and consumed by it
    // (audio, after process() returned). A save waits until the plugin has
    // seen every change, or it would capture the state before the last move.
    uint64_t ctl_pushed = 0, ctl_needed = 0;
    std::atomic<uint64_t> ctl_consumed{0};
    std::vector<uint64_t> refresh_at;      // re-read parameter values then (after a preset)
    spa_source *bus_source = nullptr;
    std::string bus_name;
};

#define LOG(...) fprintf(stderr, "backline-plugin-worker: " __VA_ARGS__)

uint64_t now_ns()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

void mark_dirty();
void rebuild_param_index();
void refresh_values();
int set_param(const ParamInfo &p, double v);

// ============================================================= Host for adapters
void WorkerHost::request_restart()
{
    W->want_reactivate = true;
    pw_loop_signal_event(pw_main_loop_get_loop(W->loop), W->main_event);
}
void WorkerHost::request_callback()
{
    W->want_callback = true;
    pw_loop_signal_event(pw_main_loop_get_loop(W->loop), W->main_event);
}
void WorkerHost::params_changed()
{
    if (W->inst && W->param_index.size() != W->inst->params.size())
        rebuild_param_index();
    else
        refresh_values();
}
void WorkerHost::mark_dirty() { ::mark_dirty(); }
void WorkerHost::param_from_plugin(uint32_t id, double value)
{
    if (!W->from_audio.push({id, value}))
        W->dropped++;
}
int WorkerHost::set_param(uint32_t id, double value)
{
    auto it = W->param_index.find(id);
    if (it == W->param_index.end())
        return -ENOENT;
    return ::set_param(W->inst->params[it->second], value);
}
void WorkerHost::count_dropped() { W->dropped++; }
bool WorkerHost::is_main_thread() { return std::this_thread::get_id() == W->main_tid; }
pw_loop *WorkerHost::main_loop() { return pw_main_loop_get_loop(W->loop); }

// ============================================================ input events
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
    if (!W->inst->midi_input)
        return;
    if (W->n_mevs >= kMaxEvents) {
        W->dropped++;
        return;
    }
    auto &e = W->mevs[W->n_mevs++];
    e.time = time;
    e.size = (uint8_t)std::min<size_t>(n, 3);
    e.data[0] = m[0];
    e.data[1] = n > 1 ? m[1] : 0;
    e.data[2] = n > 2 ? m[2] : 0;
}

// ================================================================ process
void silence(uint32_t n)
{
    for (auto &g : W->outs)
        for (void *p : g.pw)
            if (auto *b = (float *)pw_filter_get_dsp_buffer(p, n))
                memset(b, 0, n * sizeof(float));
}

#ifdef BL_HAVE_SPA_PARSER_BODY
// UMP packets in one control (PipeWire >= 1.6: the stateful converter).
void push_ump(uint32_t t, const void *body, size_t size)
{
    const uint32_t *u = (const uint32_t *)body;
    uint64_t state = 0;
    while (size > 0) {
        uint8_t midi[16];
        int sz = spa_ump_to_midi(&u, &size, midi, sizeof(midi), &state);
        if (sz < 0) break;
        if (sz > 0) push_midi_event(t, midi, (size_t)sz);
    }
}
#else
// PipeWire < 1.6: one UMP message per control.
void push_ump(uint32_t t, const void *body, size_t size)
{
    uint8_t midi[16];
    int sz = spa_ump_to_midi((uint32_t *)body, size, midi, sizeof(midi));
    if (sz > 0) push_midi_event(t, midi, (size_t)sz);
}
#endif

void drain_midi_port(uint32_t n, bool deliver)
{
    if (!W->midi_port)
        return;
    pw_buffer *b = pw_filter_dequeue_buffer(W->midi_port);
    if (!b)
        return;
    spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk && deliver) {
#ifdef BL_HAVE_SPA_PARSER_BODY
        spa_pod_parser parser;
        spa_pod_frame frame;
        spa_pod_sequence seq;
        const void *seq_body, *c_body;
        spa_pod_control c;
        spa_pod_parser_init_from_data(&parser, d->data, d->maxsize, d->chunk->offset, d->chunk->size);
        if (spa_pod_parser_push_sequence_body(&parser, &frame, &seq, &seq_body) >= 0) {
            while (spa_pod_parser_get_control_body(&parser, &c, &c_body) >= 0) {
                uint32_t t = std::min<uint32_t>(c.offset, n ? n - 1 : 0);
                if (c.type == SPA_CONTROL_Midi)
                    push_midi_event(t, (const uint8_t *)c_body, c.value.size);
                else if (c.type == SPA_CONTROL_UMP)
                    push_ump(t, c_body, c.value.size);
            }
        }
#else
        auto *pod = (spa_pod *)spa_pod_from_data(d->data, d->maxsize, d->chunk->offset, d->chunk->size);
        if (pod && spa_pod_is_sequence(pod)) {
            spa_pod_control *c;
            SPA_POD_SEQUENCE_FOREACH((spa_pod_sequence *)pod, c) {
                uint32_t t = std::min<uint32_t>(c->offset, n ? n - 1 : 0);
                if (c->type == SPA_CONTROL_Midi)
                    push_midi_event(t, (const uint8_t *)SPA_POD_BODY(&c->value), SPA_POD_BODY_SIZE(&c->value));
                else if (c->type == SPA_CONTROL_UMP)
                    push_ump(t, SPA_POD_BODY(&c->value), SPA_POD_BODY_SIZE(&c->value));
            }
        }
#endif
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
        if (w->inst->start_processing())
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
    w->n_pevs = w->n_mevs = 0;
    CtlEvent ce;
    uint64_t popped = 0;
    while (w->to_audio.pop(ce)) {
        popped++;
        if (ce.kind == CtlEvent::ParamValue) {
            if (w->n_pevs >= kMaxEvents) { w->dropped++; continue; }
            w->pevs[w->n_pevs++] = {ce.index, ce.id, ce.cookie, ce.value};
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

    for (auto &g : w->ins)
        for (size_t c = 0; c < g.pw.size(); c++) {
            auto *b = (float *)pw_filter_get_dsp_buffer(g.pw[c], n);
            g.ptrs[c] = b ? b : w->zeros.data();
        }
    for (auto &g : w->outs)
        for (size_t c = 0; c < g.pw.size(); c++) {
            auto *b = (float *)pw_filter_get_dsp_buffer(g.pw[c], n);
            g.ptrs[c] = b ? b : w->trash.data();
        }

    bl::ProcessArgs a{};
    a.frames = n;
    a.steady = (int64_t)w->steady;
    a.ins = w->in_ptrs.data();
    a.outs = w->out_ptrs.data();
    a.params = w->pevs.data();
    a.n_params = w->n_pevs;
    a.midi = w->mevs.data();
    a.n_midi = w->n_mevs;
    if (!w->inst->process(a))
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
        W->inst->stop_processing();
        W->processing = false;
    }
    return 0;
}

// Afterwards the audio thread is not inside process() and won't call it
// until `active` is set again.
void stop_audio()
{
    W->active.store(false, std::memory_order_release);
    if (pw_loop *dl = W->filter ? pw_filter_get_data_loop(W->filter) : nullptr)
        pw_loop_invoke(dl, do_stop_processing, 0, nullptr, 0, true, nullptr);
}

void activate(uint32_t rate)
{
    if (!W->inst->activate(rate, kMaxFrames))
        die("plugin failed to activate at %u Hz", rate);
    W->active_rate = rate;
    W->active.store(true, std::memory_order_release);
    LOG("activated at %u Hz, up to %u frames\n", rate, kMaxFrames);
}

void reactivate(uint32_t rate)
{
    stop_audio();
    W->inst->deactivate();
    activate(rate);
}

// Runs fn with the audio thread kept out of the plugin, if the format needs
// that for a restore (LV2); otherwise just runs it.
template <typename F> bool with_restore_guard(F fn)
{
    bool pause = W->inst->restore_needs_pause() && W->active.load();
    if (pause)
        stop_audio();
    bool ok = fn();
    if (pause)
        W->active.store(true, std::memory_order_release);
    return ok;
}

void refresh_values()
{
    auto &ps = W->inst->params;
    for (size_t i = 0; i < ps.size(); i++) {
        double v;
        if (W->inst->read_param(ps[i], v))
            W->values[i].store(v);
    }
}

void rebuild_param_index()
{
    auto &ps = W->inst->params;
    W->param_index.clear();
    W->values.reset(new std::atomic<double>[ps.size() ? ps.size() : 1]);
    for (size_t i = 0; i < ps.size(); i++) {
        W->param_index[ps[i].id] = i;
        W->values[i].store(ps[i].def);
    }
    refresh_values();
}

const ParamInfo *find_param(const std::string &key)
{
    auto &ps = W->inst->params;
    char *end = nullptr;
    unsigned long id = strtoul(key.c_str(), &end, 10);
    if (!key.empty() && end && *end == 0) {
        auto it = W->param_index.find((uint32_t)id);
        return it == W->param_index.end() ? nullptr : &ps[it->second];
    }
    const ParamInfo *sub = nullptr;
    int nsub = 0;
    for (auto &p : ps) {
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

double param_value(const ParamInfo &p)
{
    size_t i = W->param_index[p.id];
    double v = W->values[i].load();
    if (!W->active.load() || W->inst->live_values())
        W->inst->read_param(p, v);
    return v;
}

// Main thread. Clamps, then delivers: through the audio thread while the
// graph runs, or straight to the plugin on the main thread while inactive.
int set_param(const ParamInfo &p, double v)
{
    if (p.readonly)
        return -EPERM;
    v = std::clamp(v, p.min, p.max);
    if (p.stepped)
        v = std::round(v);
    size_t idx = W->param_index[p.id];
    W->values[idx].store(v);
    mark_dirty();
    double rt = W->inst->prepare_set(p, v);
    if (!W->active.load()) {
        W->inst->set_param_inactive(p, v);
        return 0;
    }
    if (!W->to_audio.push({CtlEvent::ParamValue, {}, (uint32_t)idx, p.id, p.cookie, rt}))
        return -EAGAIN;
    W->ctl_needed = ++W->ctl_pushed;
    return 0;
}

int send_midi(uint8_t a, uint8_t b, uint8_t c)
{
    if (!W->to_audio.push({CtlEvent::Midi, {a, b, c}, 0, 0, nullptr, 0}))
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
    if (W->want_callback.exchange(false))
        W->inst->on_main_thread();
}

void autosave(bool force);

void on_housekeeping(void *, uint64_t)
{
    OutParam op;
    bool changed = false;
    while (W->from_audio.pop(op)) {
        auto it = W->param_index.find(op.id);
        if (it == W->param_index.end())
            continue;
        double v = W->inst->plugin_value_to_public(op.id, op.value);
        if (W->values[it->second].load() != v) {
            W->values[it->second].store(v);
            changed = true;
        }
    }
    if (changed)
        mark_dirty();
    if (!W->refresh_at.empty()) {
        uint64_t now = now_ns();
        size_t before = W->refresh_at.size();
        W->refresh_at.erase(std::remove_if(W->refresh_at.begin(), W->refresh_at.end(), [&](uint64_t t) { return t <= now; }),
                            W->refresh_at.end());
        if (W->refresh_at.size() != before)
            refresh_values();
    }
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
    // Audio source: its outputs (a sink's monitor, a source's capture) into
    // our main inputs, by channel.
    if (!w->audio_source.empty() && p->output && !p->midi && !strcmp(p->node_name, w->audio_source.c_str()) &&
        !w->ins.empty()) {
        auto &g = w->ins[0];
        for (size_t c = 0; c < g.names.size(); c++) {
            auto own = w->own_ports.find(g.names[c]);
            if (own == w->own_ports.end())
                continue;
            bool match = !strcmp(p->channel, g.pos[c].c_str()) ||
                         (g.channels == 1 && !strcmp(p->channel, "FL")) ||
                         (!strcmp(p->channel, "MONO") && g.channels <= 2);
            if (match && !bl_graph_is_linked(w->graph, p->id, own->second)) {
                LOG("audio-source: linking %s:%s -> %s\n", p->node_name, p->name, g.names[c].c_str());
                bl_graph_link(w->graph, p->id, own->second);
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
    if (W->to_audio.push({CtlEvent::ReleaseAll, {}, 0, 0, nullptr, 0}))
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

// ----------------------------------------------------------------- presets
const std::vector<PresetInfo> &presets()
{
    if (!W->presets_listed) {
        W->presets = W->inst->list_presets();
        W->presets_listed = true;
    }
    return W->presets;
}

// A preset key: an id from ListPresets, an exact name, or a unique substring
// of a name. A path to a preset file works too where the format has files.
const PresetInfo *find_preset(const std::string &key)
{
    auto &ps = presets();
    for (auto &p : ps)
        if (p.id == key)
            return &p;
    for (auto &p : ps)
        if (!strcasecmp(p.name.c_str(), key.c_str()))
            return &p;
    const PresetInfo *sub = nullptr;
    int nsub = 0;
    for (auto &p : ps)
        if (strcasestr(p.name.c_str(), key.c_str())) {
            sub = &p;
            nsub++;
        }
    return nsub == 1 ? sub : nullptr;
}

bool load_preset(const std::string &key, std::string *err, std::string *loaded_name = nullptr)
{
    const PresetInfo *p = find_preset(key);
    PresetInfo file;
    struct stat st;
    if (!p && key.find('/') != std::string::npos && stat(key.c_str(), &st) == 0) {
        file = {key, key, key};
        p = &file;
    }
    if (!p) {
        if (err) *err = "no unique preset matches '" + key + "'";
        return false;
    }
    PresetInfo copy = *p;
    if (!with_restore_guard([&] { return W->inst->load_preset(copy, err); }))
        return false;
    if (loaded_name) *loaded_name = copy.name;
    if (W->param_index.size() != W->inst->params.size())
        rebuild_param_index();
    refresh_values();
    uint64_t now = now_ns();
    W->refresh_at.push_back(now + 100000000ull); // plugins may apply a preset asynchronously
    W->refresh_at.push_back(now + 500000000ull);
    mark_dirty();
    LOG("preset loaded: %s (%s)\n", copy.name.c_str(), copy.id.c_str());
    return true;
}

// --------------------------------------------------------------------- OSC
void osc_reply(const sockaddr_in &to, const std::vector<uint8_t> &b)
{
    sendto(W->osc_fd, b.data(), b.size(), 0, (const sockaddr *)&to, sizeof(to));
}

void osc_reply_param(const sockaddr_in &to, const char *addr, const ParamInfo &p)
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
            if (const ParamInfo *p = find_param(key_of(m.args[0])))
                set_param(*p, v);
        } else if (a.rfind("/param/", 0) == 0 && a != "/param/get" && a != "/param/list" &&
                   m.args.size() == 1 && osc::as_double(m.args[0], v)) {
            if (const ParamInfo *p = find_param(a.substr(7)))
                set_param(*p, v);
        } else if (a == "/param/get" && m.args.size() == 1) {
            if (const ParamInfo *p = find_param(key_of(m.args[0])))
                osc_reply_param(from, "/backline/param", *p);
        } else if (a == "/param/list") {
            for (auto &p : W->inst->params)
                osc_reply_param(from, "/backline/param", p);
            osc_reply(from, osc::Writer("/backline/param/end").i((int32_t)W->inst->params.size()).bytes());
        } else if (a == "/note/on" && m.args.size() == 3) {
            double ch, k, vel;
            if (osc::as_double(m.args[0], ch) && osc::as_double(m.args[1], k) && osc::as_double(m.args[2], vel))
                send_midi(0x90 | ((int)ch & 15), (int)k & 127, (int)vel & 127);
        } else if (a == "/note/off" && m.args.size() >= 2) {
            double ch, k;
            if (osc::as_double(m.args[0], ch) && osc::as_double(m.args[1], k))
                send_midi(0x80 | ((int)ch & 15), (int)k & 127, 0);
        } else if (a == "/preset/list") {
            W->presets_listed = false;
            auto &ps = presets();
            for (auto &p : ps)
                osc_reply(from, osc::Writer("/backline/preset").s(p.id).s(p.name).s(p.location).bytes());
            osc_reply(from, osc::Writer("/backline/preset/end").i((int32_t)ps.size()).bytes());
        } else if (a == "/preset/load" && m.args.size() == 1) {
            std::string err, name;
            bool ok = load_preset(key_of(m.args[0]), &err, &name);
            osc_reply(from, osc::Writer("/backline/preset/loaded").i(ok ? 1 : 0).s(ok ? name : err).bytes());
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
constexpr const char *kErrNoSuchParam = "io.github.manuaudio.Backline.Error.NoSuchParam";
constexpr const char *kErrNotFound = "io.github.manuaudio.Backline.Error.NotFound";
constexpr const char *kErrUnsupported = "io.github.manuaudio.Backline.Error.Unsupported";

int m_list_params(sd_bus_message *m, void *, sd_bus_error *)
{
    sd_bus_message *r = nullptr;
    sd_bus_message_new_method_return(m, &r);
    sd_bus_message_open_container(r, 'a', "(ussddd)");
    for (auto &p : W->inst->params)
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
    const ParamInfo *p = find_param(key);
    if (!p) return sd_bus_error_setf(e, kErrNoSuchParam, "no unique parameter matches '%s'", key);
    return sd_bus_reply_method_return(m, "d", param_value(*p));
}
int m_set_param(sd_bus_message *m, void *, sd_bus_error *e)
{
    const char *key;
    double v;
    int r = sd_bus_message_read(m, "sd", &key, &v);
    if (r < 0) return r;
    const ParamInfo *p = find_param(key);
    if (!p) return sd_bus_error_setf(e, kErrNoSuchParam, "no unique parameter matches '%s'", key);
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
    std::vector<uint8_t> data;
    if (!W->inst->save_state(data, err))
        return false;
    std::string tmp = path + ".tmp." + std::to_string(getpid());
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) {
        if (err) *err = std::string("open ") + tmp + ": " + strerror(errno);
        return false;
    }
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    fclose(f);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        if (err) *err = ok ? std::string("rename: ") + strerror(errno) : "could not write the state file";
        return false;
    }
    return true;
}

bool load_state_from(const std::string &path, std::string *err)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = std::string("open ") + path + ": " + strerror(errno);
        return false;
    }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        data.insert(data.end(), buf, buf + n);
    fclose(f);
    if (!with_restore_guard([&] { return W->inst->load_state(data, err); }))
        return false;
    if (W->param_index.size() != W->inst->params.size())
        rebuild_param_index();
    refresh_values();
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

// D-Bus strings must be valid UTF-8; file names and plugin strings may not be.
std::string utf8(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n && i + n <= s.size() && c != 0;
        for (size_t k = 1; ok && k < n; k++)
            ok = ((unsigned char)s[i + k] >> 6) == 2;
        if (ok) {
            o.append(s, i, n);
            i += n;
        } else {
            o += '?';
            i++;
        }
    }
    return o;
}

int m_list_presets(sd_bus_message *m, void *, sd_bus_error *)
{
    W->presets_listed = false; // rescan: files may have come or gone
    sd_bus_message *r = nullptr;
    sd_bus_message_new_method_return(m, &r);
    sd_bus_message_open_container(r, 'a', "(sss)");
    for (auto &p : presets())
        if (int e = sd_bus_message_append(r, "(sss)", utf8(p.id).c_str(), utf8(p.name).c_str(), utf8(p.location).c_str()); e < 0) {
            sd_bus_message_unref(r);
            return e;
        }
    sd_bus_message_close_container(r);
    int ret = sd_bus_send(nullptr, r, nullptr);
    sd_bus_message_unref(r);
    return ret;
}
int m_load_preset(sd_bus_message *m, void *, sd_bus_error *e)
{
    const char *key;
    int r = sd_bus_message_read(m, "s", &key);
    if (r < 0) return r;
    std::string err;
    if (!find_preset(key) && !(strchr(key, '/') && access(key, R_OK) == 0))
        return sd_bus_error_setf(e, kErrNotFound, "no unique preset matches '%s'", key);
    if (!load_preset(key, &err)) return sd_bus_error_set(e, SD_BUS_ERROR_FAILED, err.c_str());
    return sd_bus_reply_method_return(m, "");
}
int m_save_preset(sd_bus_message *m, void *, sd_bus_error *e)
{
    const char *name;
    int r = sd_bus_message_read(m, "s", &name);
    if (r < 0) return r;
    std::string err, path;
    if (!W->inst->save_preset(name, path, &err))
        return sd_bus_error_set(e, err.find("not supported") != std::string::npos ? kErrUnsupported : SD_BUS_ERROR_FAILED, err.c_str());
    W->presets_listed = false;
    return sd_bus_reply_method_return(m, "s", path.c_str());
}

int p_str(sd_bus *, const char *, const char *, const char *prop, sd_bus_message *r, void *, sd_bus_error *)
{
    if (!strcmp(prop, "PluginId")) return sd_bus_message_append(r, "s", W->inst->id.c_str());
    if (!strcmp(prop, "PluginName")) return sd_bus_message_append(r, "s", W->inst->name.c_str());
    if (!strcmp(prop, "PluginPath")) return sd_bus_message_append(r, "s", W->inst->path.c_str());
    if (!strcmp(prop, "PluginFormat")) return sd_bus_message_append(r, "s", W->inst->format());
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
    SD_BUS_METHOD_WITH_ARGS("ListPresets", SD_BUS_NO_ARGS, SD_BUS_RESULT("a(sss)", presets), m_list_presets, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("LoadPreset", SD_BUS_ARGS("s", id), SD_BUS_NO_RESULT, m_load_preset, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("SavePreset", SD_BUS_ARGS("s", name), SD_BUS_RESULT("s", path), m_save_preset, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("PluginId", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PluginName", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PluginPath", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PluginFormat", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
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

// ------------------------------------------------------------- the plugin
bool ends_with(const std::string &s, const char *suf)
{
    size_t n = strlen(suf);
    return s.size() >= n && !strcasecmp(s.c_str() + s.size() - n, suf);
}

bool is_uri(const std::string &s)
{
    return s.find("://") != std::string::npos || s.rfind("urn:", 0) == 0;
}

// The plugin argument -> (format, path or URI). A path decides by its
// suffix; a bare name is looked up in the standard directories, CLAP first,
// then VST3, then LV2 (or only in --format's).
std::pair<Fmt, std::string> resolve_plugin(std::string p, Fmt want)
{
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    struct stat st;
    if (stat(p.c_str(), &st) == 0) {
        Fmt f = want;
        if (f == Fmt::Auto)
            f = ends_with(p, ".vst3") ? Fmt::Vst3 : ends_with(p, ".lv2") && S_ISDIR(st.st_mode) ? Fmt::Lv2 : Fmt::Clap;
        return {f, p};
    }
    if (is_uri(p) && (want == Fmt::Auto || want == Fmt::Lv2))
        return {Fmt::Lv2, p};
    const char *h = getenv("HOME");
    std::string home = h ? h : "";
    struct Dir { Fmt f; std::string dir; const char *suffix; };
    const Dir dirs[] = {
        {Fmt::Clap, home + "/.clap", ".clap"}, {Fmt::Clap, "/usr/lib/clap", ".clap"},
        {Fmt::Clap, "/usr/local/lib/clap", ".clap"},
        {Fmt::Vst3, home + "/.vst3", ".vst3"}, {Fmt::Vst3, "/usr/lib/vst3", ".vst3"},
        {Fmt::Vst3, "/usr/local/lib/vst3", ".vst3"},
        {Fmt::Lv2, home + "/.lv2", ".lv2"}, {Fmt::Lv2, "/usr/lib/lv2", ".lv2"}, {Fmt::Lv2, "/usr/local/lib/lv2", ".lv2"},
    };
    std::string looked;
    for (auto &d : dirs) {
        if (want != Fmt::Auto && want != d.f)
            continue;
        std::string c = d.dir + "/" + p + (ends_with(p, d.suffix) ? "" : d.suffix);
        if (stat(c.c_str(), &st) == 0)
            return {d.f, c};
        looked += (looked.empty() ? "" : ", ") + d.dir;
    }
    die("plugin not found: %s (looked in %s)", p.c_str(), looked.c_str());
}

void load_plugin()
{
    auto [f, path] = resolve_plugin(W->plugin_arg, W->fmt);
    W->fmt = f;
    switch (f) {
    case Fmt::Vst3:
#ifdef BL_HAVE_VST3
        W->inst = bl::load_vst3(&W->host, path, W->plugin_id, W->list_only);
#else
        die("built without VST3: cannot host %s (configure with the VST3 SDK, BACKLINE_VST3=ON)", path.c_str());
#endif
        break;
    case Fmt::Lv2:
#ifdef BL_HAVE_LV2
        W->inst = bl::load_lv2(&W->host, path, W->plugin_id, W->list_only);
#else
        die("built without LV2: cannot host %s (configure with lilv, BACKLINE_LV2=ON)", path.c_str());
#endif
        break;
    default:
        W->inst = bl::load_clap(&W->host, path, W->plugin_id, W->list_only);
    }
    for (auto &d : W->preset_dirs)
        W->inst->extra_preset_dirs.push_back(d);
    rebuild_param_index();
    auto *I = W->inst.get();
    LOG("loaded %s (%s, %s) from %s: %zu params, note input: %s\n", I->name.c_str(), I->format(), I->id.c_str(),
        I->path.c_str(), I->params.size(), I->midi_input ? I->midi_dialect.c_str() : "none");
}

// Every output channel's position, in port order: the first bus keeps its own
// (MONO, or FL/FR), every further channel is AUX<n> (n = its index on the
// node), so a node-level channel map has no duplicates.
std::vector<std::string> out_positions(const std::vector<bl::BusInfo> &buses)
{
    std::vector<std::string> v;
    for (size_t b = 0; b < buses.size(); b++)
        for (uint32_t c = 0; c < buses[b].channels; c++) {
            uint32_t n = buses[b].channels;
            if (b == 0 && n == 1)
                v.push_back("MONO");
            else if (b == 0 && n == 2)
                v.push_back(c ? "FR" : "FL");
            else
                v.push_back("AUX" + std::to_string(v.size()));
        }
    return v;
}

void add_audio_ports(bool input, std::vector<PortGroup> &groups)
{
    auto &buses = input ? W->inst->inputs : W->inst->outputs;
    for (uint32_t i = 0; i < buses.size(); i++) {
        uint32_t n = buses[i].channels;
        PortGroup g{n, {}, {}, {}, {}};
        bool stereo = n == 2;
        for (uint32_t c = 0; c < n; c++) {
            // port names as before: in_FL, out_FR, out1_FL, out_AUX2, ...
            std::string pos = stereo ? (c ? "FR" : "FL") : n == 1 ? "MONO" : "AUX" + std::to_string(c);
            std::string base = input ? "in" : "out";
            std::string name = (i == 0 ? base : base + std::to_string(i)) + "_" + pos;
            g.names.push_back(name);
            g.pos.push_back(pos);
        }
        groups.push_back(std::move(g));
    }
    // audio.channel: outputs follow the node's audio.position
    std::vector<std::string> node_pos = input ? std::vector<std::string>{} : out_positions(buses);
    size_t k = 0;
    for (auto &g : groups)
        for (size_t c = 0; c < g.channels; c++, k++) {
            const std::string &ch = input ? g.pos[c] : node_pos[k];
            void *port = pw_filter_add_port(W->filter, input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                                            PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
                                            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                                              PW_KEY_PORT_NAME, g.names[c].c_str(),
                                                              PW_KEY_AUDIO_CHANNEL, ch.c_str(), nullptr),
                                            nullptr, 0);
            g.pw.push_back(port);
        }
    for (auto &g : groups)
        g.ptrs.resize(g.channels);
    for (auto &g : groups)
        (input ? W->in_ptrs : W->out_ptrs).push_back(g.ptrs.data());
}

void on_signal(void *, int) { pw_main_loop_quit(W->loop); }

void usage(FILE *f)
{
    fputs("usage: backline-plugin-worker --plugin PATH|NAME|URI [options]\n"
          "  -p, --plugin PATH|NAME|URI  a .clap file, a .vst3 bundle, an LV2 plugin URI or .lv2 bundle,\n"
          "                           or a name in the standard directories (~/.clap, /usr/lib/clap,\n"
          "                           ~/.vst3, /usr/lib/vst3, ~/.lv2, /usr/lib/lv2; CLAP first)\n"
          "  -f, --format FMT         clap, vst3 or lv2: only look for that format\n"
          "  -i, --id ID              plugin inside the file: CLAP id, VST3 class id (hex) or name,\n"
          "                           LV2 URI (default: first instrument, else first plugin)\n"
          "  -n, --name NODE          node.name (default: backline.<plugin id>)\n"
          "  -m, --midi-follow REGEX  link every MIDI source matching REGEX, now and when hotplugged\n"
          "  -t, --audio-target NODE  link the main output to NODE's playback ports\n"
          "  -S, --audio-source NODE  link NODE's outputs (a sink's monitor, a source) to the main input\n"
          "  -o, --osc-port PORT      OSC on 127.0.0.1:PORT (default: any free port)\n"
          "  -a, --autoconnect        let the session manager link the output to the default\n"
          "                           output (the default unless --audio-target is given)\n"
          "  -A, --no-autoconnect     never let the session manager link the output\n"
          "  -s, --state-file PATH    load the plugin state from PATH at start (if it exists),\n"
          "                           and save it there, atomically, whenever it changes\n"
          "      --preset-dir DIR     also look for preset files (.vstpreset) in DIR (repeatable)\n"
          "      --no-dbus            no D-Bus control object\n"
          "  -l, --list               print the plugins in the file/bundle as JSON and exit\n",
          f);
}

} // namespace

int main(int argc, char **argv)
{
    // Plugins print banners to stdout; keep it for our own JSON only.
    if (int fd = dup(STDOUT_FILENO); fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        bl::out = fdopen(fd, "w");
    }
    Worker w;
    W = &w;
    w.main_tid = std::this_thread::get_id();
    static const option opts[] = {
        {"plugin", required_argument, nullptr, 'p'}, {"id", required_argument, nullptr, 'i'},
        {"format", required_argument, nullptr, 'f'},
        {"name", required_argument, nullptr, 'n'}, {"midi-follow", required_argument, nullptr, 'm'},
        {"audio-target", required_argument, nullptr, 't'}, {"audio-source", required_argument, nullptr, 'S'},
        {"osc-port", required_argument, nullptr, 'o'},
        {"autoconnect", no_argument, nullptr, 'a'}, {"no-autoconnect", no_argument, nullptr, 'A'},
        {"state-file", required_argument, nullptr, 's'}, {"no-dbus", no_argument, nullptr, 'D'},
        {"preset-dir", required_argument, nullptr, 'P'},
        {"list", no_argument, nullptr, 'l'}, {"help", no_argument, nullptr, 'h'}, {}};
    int c;
    while ((c = getopt_long(argc, argv, "p:i:f:n:m:t:S:o:aAs:lh", opts, nullptr)) != -1) {
        switch (c) {
        case 'p': w.plugin_arg = optarg; break;
        case 'i': w.plugin_id = optarg; break;
        case 'f':
            if (!strcmp(optarg, "clap")) w.fmt = Fmt::Clap;
            else if (!strcmp(optarg, "vst3")) w.fmt = Fmt::Vst3;
            else if (!strcmp(optarg, "lv2")) w.fmt = Fmt::Lv2;
            else if (strcmp(optarg, "auto")) { fprintf(stderr, "unknown format '%s' (clap, vst3, lv2)\n", optarg); return 2; }
            break;
        case 'n': w.node_name = optarg; break;
        case 'm': w.midi_follow = optarg; break;
        case 't': w.audio_target = optarg; break;
        case 'S': w.audio_source = optarg; break;
        case 'o': w.osc_port = atoi(optarg); break;
        case 'a': w.autoconnect = 1; break;
        case 'A': w.autoconnect = 0; break;
        case 's': w.state_file = optarg; break;
        case 'D': w.use_dbus = false; break;
        case 'P': w.preset_dirs.push_back(optarg); break;
        case 'l': w.list_only = true; break;
        case 'h': usage(bl::out); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (w.plugin_arg.empty()) {
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
                LOG("restored state from %s (%lld bytes, %zu params)\n", w.state_file.c_str(), (long long)st.st_size, w.inst->params.size());
            else
                LOG("could not restore state from %s: %s; starting from defaults\n", w.state_file.c_str(), err.c_str());
        }
    }
    if (w.node_name.empty())
        w.node_name = "backline." + w.inst->node_suffix;
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

    auto *I = w.inst.get();
    std::string desc = I->name + " (Backline)";
    std::string osc = std::to_string(w.osc_port);
    bool instrument = I->instrument;
    // The node's channel layout (out_* ports), so level meters and mixers can
    // read it: audio.channels and audio.position, like any audio stream.
    std::vector<std::string> pos = out_positions(I->outputs);
    std::string positions, nchan = std::to_string(pos.size());
    for (auto &p : pos)
        positions += (positions.empty() ? "" : ",") + p;
    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                             PW_KEY_MEDIA_CATEGORY, instrument ? "Playback" : "Filter",
                                             PW_KEY_MEDIA_ROLE, "Production",
                                             PW_KEY_MEDIA_CLASS, instrument ? "Stream/Output/Audio" : "Audio/Filter",
                                             PW_KEY_NODE_NAME, w.node_name.c_str(),
                                             PW_KEY_NODE_DESCRIPTION, desc.c_str(),
                                             PW_KEY_NODE_AUTOCONNECT, w.autoconnect > 0 ? "true" : "false",
                                             PW_KEY_NODE_ALWAYS_PROCESS, "true",
                                             "backline.plugin.format", I->format(),
                                             "backline.plugin.id", I->id.c_str(),
                                             "backline.plugin.path", I->path.c_str(),
                                             "backline.osc.port", osc.c_str(),
                                             "backline.dbus.name", w.bus_name.c_str(),
                                             nullptr);
    if (!pos.empty()) {
        pw_properties_set(props, PW_KEY_AUDIO_CHANNELS, nchan.c_str());
        pw_properties_set(props, "audio.position", positions.c_str());
    }
    w.filter = pw_filter_new(w.core, w.node_name.c_str(), props);
    pw_filter_add_listener(w.filter, &w.filter_listener, &filter_events, nullptr);
    if (I->midi_input)
        w.midi_port = pw_filter_add_port(w.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
                                         pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi",
                                                           PW_KEY_PORT_NAME, "midi_in", nullptr),
                                         nullptr, 0);
    add_audio_ports(true, w.ins);
    add_audio_ports(false, w.outs);

    activate(w.server_rate); // re-activated on the first cycle if the graph runs at another rate
    if (pw_filter_connect(w.filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0)
        die("pw_filter_connect failed");

    w.housekeeping = pw_loop_add_timer(l, on_housekeeping, nullptr);
    timespec hk{0, 20 * 1000000L};
    pw_loop_update_timer(l, w.housekeeping, &hk, &hk, false);

    fprintf(bl::out, "{\"node\": %s, \"plugin\": %s, \"format\": \"%s\", \"plugin_path\": %s, \"osc_port\": %d, \"dbus\": %s, \"pid\": %d}\n",
           json_str(w.node_name).c_str(), json_str(I->id).c_str(), I->format(), json_str(I->path).c_str(),
           w.osc_port, json_str(w.bus_name).c_str(), getpid());
    fflush(bl::out);

    pw_main_loop_run(w.loop);

    LOG("shutting down\n");
    autosave(true);
    stop_audio();
    pw_filter_disconnect(w.filter);
    w.inst->deactivate();
    w.inst.reset();
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
