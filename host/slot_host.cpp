// backline-slot-host: one strip's insert chain, in one process, inside the
// engine's audio cycle (Slot1, docs/api/Slot1.md).
// SPDX-License-Identifier: MIT
//
// The engine owns the cycle and the memory (a sealed memfd). This process
// maps it, loads the chain's plugins (CLAP, VST3, LV2, in any mix, behind the
// same bl::PluginInstance adapters as the worker), and serves one request per
// cycle: the engine copies the strip's audio in and bumps `req`; the audio
// thread here wakes, runs every insert in order in place, and sets
// `done = req`. If that takes longer than the engine's deadline, the engine
// plays the dry signal delayed by the chain's latency and waits for this side
// to catch up (slot1.h has the rule). A plugin that crashes takes this
// process down, and only this chain: the supervisor (backline-hostd) starts a
// new one on the same memory, which takes over by bumping `generation`.
//
// Threads:
//   main   a PipeWire loop with no server connection: the plugins'
//          main-thread calls and timers, latency polling, parameter
//          conversions VST3 needs, state saves. Never touches the audio.
//   audio  waits on the `req` futex; runs the chain; no locks, no
//          allocation (counted into the header: audio_allocs), no logging.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <pipewire/pipewire.h>

#include "backline/slot1.h"
#include "bl_rtalloc.h"
#include "plugin.h"
#include "spsc.h"

using bl::Fmt;
using bl::kMaxEvents;
using bl::ParamInfo;

namespace {

constexpr const char *kProg = "backline-slot-host";
#define LOG(...) fprintf(stderr, "backline-slot-host: " __VA_ARGS__)

thread_local bool t_audio = false;

struct Slot;

class SlotHostCb final : public bl::Host {
public:
    Slot *s = nullptr;
    void request_restart() override;
    void request_callback() override;
    void params_changed() override;
    void mark_dirty() override;
    void param_from_plugin(uint32_t id, double value) override;
    int set_param(uint32_t id, double value) override;
    void count_dropped() override;
    bool is_main_thread() override;
    bool is_audio_thread() override { return t_audio; }
    pw_loop *main_loop() override;
    void log(const char *fmt, ...) override
    {
        va_list ap;
        va_start(ap, fmt);
        fputs("backline-slot-host: plugin: ", stderr);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
};

// What the audio thread needs of a parameter: a copy, so the main thread can
// rescan the plugin's list while the audio thread keeps its own.
struct AParam {
    uint32_t id, index;
    void *cookie;
    double min, max;
    bool stepped;
};

struct Slot {
    int index = 0;
    std::string spec, plugin_id, state_path;
    Fmt fmt = Fmt::Auto;
    std::unique_ptr<bl::PluginInstance> inst;
    SlotHostCb host;

    // audio buffers for the adapter: [bus][channel] pointer tables
    std::vector<std::vector<float *>> in_tbl, out_tbl;
    std::vector<float *const *> in_bus, out_bus;
    uint32_t out0_channels = 0;          // the main output's width

    std::vector<AParam> aparams;          // sorted by id (audio thread's copy)
    std::vector<bl::ParamEv> pevs;
    uint32_t n_pevs = 0;
    std::vector<bl::MidiEv> mevs;
    uint32_t n_mevs = 0;

    // the bypass delay line (channels × blen): a bypassed insert passes its
    // input delayed by its latency, so the chain's alignment holds
    std::vector<float> bdly;
    uint32_t blen = 0, bpos = 0;
    std::atomic<uint32_t> lat_audio{0};

    // main ↔ audio
    std::atomic<bool> suspended{false};
    bool processing = false;              // audio thread
    bool failed = false;                  // audio thread: start_processing refused
    std::atomic<bool> want_restart{false}, want_callback{false}, want_rescan{false};
    std::atomic<uint32_t> dirty{0};       // audio + main: something to save
    std::atomic<bool> ran{false};         // audio: processed once (LV2 reports latency only then)
    uint32_t latency = 0;                 // main's view
};

struct Vst3Plain { uint8_t slot; uint32_t id; double value; };          // audio → main: convert
struct Converted { uint8_t slot; uint32_t index, id; void *cookie; double value; }; // main → audio
struct Applied { uint8_t slot; uint32_t index; double value; };                      // audio → main: note it

struct G {
    std::string shm_path;
    int shm_fd = -1;
    std::string name = "slot";
    int rt_priority = 80;
    std::string state_dir;
    std::vector<Slot *> slots;

    bl_slot1_header *h = nullptr;
    size_t size = 0;
    uint32_t C = 0, max_frames = 0, rate = 0;

    std::vector<float> scratch;           // C × max_frames
    std::vector<float> zeros, trash;      // max_frames
    std::vector<float *> shm_ch, scr_ch;

    pw_main_loop *loop = nullptr;
    spa_source *wake = nullptr, *tick = nullptr;
    std::thread::id main_tid;
    std::thread audio;
    volatile int quit = 0;
    std::atomic<bool> in_cycle{false};
    uint32_t start_seen = 0;

    Spsc<Vst3Plain> to_main{1024};
    Spsc<Converted> from_main{1024};
    Spsc<Applied> applied{4096};
    std::atomic<uint64_t> dropped{0};
    uint64_t last_dirty_ns = 0;
} g;

uint64_t now_ns() { return bl_slot1_now_ns(); }

// -------------------------------------------------------------- Host callbacks
void SlotHostCb::request_restart()
{
    s->want_restart = true;
    pw_loop_signal_event(pw_main_loop_get_loop(g.loop), g.wake);
}
void SlotHostCb::request_callback()
{
    s->want_callback = true;
    pw_loop_signal_event(pw_main_loop_get_loop(g.loop), g.wake);
}
void SlotHostCb::params_changed()
{
    s->want_rescan = true;
    pw_loop_signal_event(pw_main_loop_get_loop(g.loop), g.wake);
}
void SlotHostCb::mark_dirty() { s->dirty.fetch_add(1, std::memory_order_relaxed); }
void SlotHostCb::param_from_plugin(uint32_t id, double value)
{
    // to the engine, on the out ring; VST3 reports normalized values
    bl_slot1_event e{};
    e.slot = (uint8_t)s->index;
    e.type = s->fmt == Fmt::Vst3 ? BL_SLOT1_EV_PARAM_NORM : BL_SLOT1_EV_PARAM;
    e.id = id;
    e.value = value;
    if (bl_slot1_ring_push(&g.h->out_head, &g.h->out_tail, bl_slot1_out_events(g.h), g.h->n_events, &e) < 0)
        g.dropped++;
    s->dirty.fetch_add(1, std::memory_order_relaxed);
}
int SlotHostCb::set_param(uint32_t id, double value)
{
    // main: a plugin's own controller moved a parameter (VST3); deliver it
    for (auto &p : s->inst->params)
        if (p.id == id) {
            double v = s->inst->prepare_set(p, value);
            uint32_t idx = (uint32_t)(&p - s->inst->params.data());
            if (!g.from_main.push({(uint8_t)s->index, idx, id, p.cookie, v}))
                g.dropped++;
            return 0;
        }
    return -ENOENT;
}
void SlotHostCb::count_dropped() { g.dropped++; }
bool SlotHostCb::is_main_thread() { return std::this_thread::get_id() == g.main_tid; }
pw_loop *SlotHostCb::main_loop() { return pw_main_loop_get_loop(g.loop); }

// -------------------------------------------------------------- audio thread
const AParam *find_aparam(const Slot &s, uint32_t id)
{
    auto it = std::lower_bound(s.aparams.begin(), s.aparams.end(), id, [](const AParam &a, uint32_t v) { return a.id < v; });
    return it != s.aparams.end() && it->id == id ? &*it : nullptr;
}

void add_pev(Slot &s, const AParam &p, double v)
{
    if (s.n_pevs >= kMaxEvents) {
        g.dropped++;
        return;
    }
    s.pevs[s.n_pevs++] = {p.index, p.id, p.cookie, v};
}

void take_events()
{
    bl_slot1_header *h = g.h;
    bl_slot1_event e;
    while (bl_slot1_ring_pop(&h->in_head, &h->in_tail, bl_slot1_in_events(h), h->n_events, &e) == 0) {
        if (e.slot >= g.slots.size())
            continue;
        Slot &s = *g.slots[e.slot];
        if (e.type == BL_SLOT1_EV_MIDI) {
            if (s.n_mevs < kMaxEvents)
                s.mevs[s.n_mevs++] = {std::min<uint32_t>(e.offset, h->frames ? h->frames - 1 : 0), 3,
                                      {(uint8_t)(e.id & 0xff), (uint8_t)((e.id >> 8) & 0x7f), (uint8_t)((e.id >> 16) & 0x7f)}};
            else
                g.dropped++;
            continue;
        }
        const AParam *p = find_aparam(s, e.id);
        if (!p)
            continue;
        double v = e.value;
        if (e.type == BL_SLOT1_EV_PARAM_NORM) {
            v = std::clamp(v, 0.0, 1.0);
            if (s.fmt != Fmt::Vst3) {
                v = p->min + v * (p->max - p->min);
                if (p->stepped)
                    v = std::round(v);
            }
        } else if (s.fmt == Fmt::Vst3) {
            // plain → normalized needs the controller, on the main thread: it
            // lands a cycle or two later (Slot1.md: PARAM_NORM is same-cycle)
            if (!g.to_main.push({e.slot, e.id, v}))
                g.dropped++;
            pw_loop_signal_event(pw_main_loop_get_loop(g.loop), g.wake);
            continue;
        } else {
            v = std::clamp(v, std::min(p->min, p->max), std::max(p->min, p->max));
        }
        add_pev(s, *p, v);
        if (!g.applied.push({e.slot, p->index, v}))
            g.dropped++;
        s.dirty.fetch_add(1, std::memory_order_relaxed);
    }
    Converted c;
    while (g.from_main.pop(c)) {
        if (c.slot >= g.slots.size())
            continue;
        Slot &s = *g.slots[c.slot];
        if (s.n_pevs < kMaxEvents)
            s.pevs[s.n_pevs++] = {c.index, c.id, c.cookie, c.value};
        else
            g.dropped++;
    }
}

void delay_through(Slot &s, float *const *src, float *const *dst, uint32_t n, bool output)
{
    // feed this insert's bypass line with its input; when output, write the
    // input delayed by its latency into dst
    uint32_t len = s.blen, L = std::min(s.lat_audio.load(std::memory_order_relaxed), len - g.max_frames);
    for (uint32_t c = 0; c < g.C; c++) {
        float *d = s.bdly.data() + (size_t)c * len;
        uint32_t w = s.bpos;
        for (uint32_t i = 0; i < n; i++) {
            d[w] = src[c][i];
            if (++w == len)
                w = 0;
        }
        if (output) {
            uint32_t r = (s.bpos + len - L) % len;
            for (uint32_t i = 0; i < n; i++) {
                dst[c][i] = d[r];
                if (++r == len)
                    r = 0;
            }
        }
    }
    s.bpos = (s.bpos + n) % len;
}

void run_cycle(uint32_t n)
{
    bl_slot1_header *h = g.h;
    bl::Transport tr;
    tr.valid = h->flags & BL_SLOT1_TR_VALID;
    tr.rolling = h->flags & BL_SLOT1_TR_ROLLING;
    tr.bar_valid = h->flags & BL_SLOT1_TR_BAR;
    tr.changed = h->flags & BL_SLOT1_TR_CHANGED;
    tr.frame = h->tr_frame;
    tr.rate = g.rate;
    tr.bpm = h->tr_bpm;
    tr.beat = h->tr_beat;
    tr.bar_start = h->tr_bar_start;
    tr.sig_num = h->tr_sig_num ? h->tr_sig_num : 4;
    tr.sig_den = h->tr_sig_den ? h->tr_sig_den : 4;
    tr.bar = tr.sig_num > 0 ? (int32_t)std::floor(tr.beat / tr.sig_num) : 0;
    tr.nsec = now_ns();

    take_events();

    float *const *cur = g.shm_ch.data();
    float *const *oth = g.scr_ch.data();
    uint32_t bypass = h->bypass;
    for (Slot *sp : g.slots) {
        Slot &s = *sp;
        bool bypassed = (bypass >> s.index) & 1;
        bool skip = bypassed || s.failed || s.suspended.load(std::memory_order_seq_cst);
        // the bypass line is fed every cycle, so switching it is aligned
        delay_through(s, cur, oth, n, skip);
        if (!skip) {
            if (!s.processing) {
                if (s.inst->start_processing())
                    s.processing = true;
                else
                    s.failed = true;
            }
        }
        if (!skip && s.processing) {
            if (!s.in_tbl.empty())
                for (uint32_t c = 0; c < s.in_tbl[0].size(); c++)
                    s.in_tbl[0][c] = c < g.C ? cur[c] : g.zeros.data();
            if (!s.out_tbl.empty())
                for (uint32_t c = 0; c < s.out_tbl[0].size(); c++)
                    s.out_tbl[0][c] = c < g.C ? oth[c] : g.trash.data();
            bl::ProcessArgs a{};
            a.frames = n;
            a.steady = (int64_t)h->steady;
            a.transport = &tr;
            a.ins = s.in_bus.data();
            a.outs = s.out_bus.data();
            a.params = s.pevs.data();
            a.n_params = s.n_pevs;
            a.midi = s.mevs.data();
            a.n_midi = s.n_mevs;
            bool ok = s.inst->process(a);
            if (!s.ran.load(std::memory_order_relaxed))
                s.ran.store(true, std::memory_order_release);
            if (!ok) {
                h->process_errors++;
                for (uint32_t c = 0; c < g.C; c++)
                    memcpy(oth[c], cur[c], n * sizeof(float));
            } else {
                // channels the plugin's main output doesn't have pass through
                for (uint32_t c = s.out0_channels; c < g.C; c++)
                    memcpy(oth[c], cur[c], n * sizeof(float));
            }
        }
        s.n_pevs = s.n_mevs = 0;
        std::swap(cur, oth);
    }
    if (cur != g.shm_ch.data())
        for (uint32_t c = 0; c < g.C; c++)
            memcpy(g.shm_ch[c], cur[c], n * sizeof(float));
}

void audio_main()
{
    t_audio = true;
    sched_param sp{};
    sp.sched_priority = g.rt_priority;
    if (g.rt_priority > 0 && pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        LOG("audio thread: no realtime priority %d (%s); running at normal priority\n", g.rt_priority, strerror(errno));
    bl_rtalloc_enter();
    bl_slot1_header *h = g.h;
    uint32_t seen = g.start_seen;
    for (;;) {
        uint32_t seq = bl_slot1_sandbox_wait(h, seen, &g.quit);
        if (g.quit)
            break;
        seen = seq;
        g.in_cycle.store(true, std::memory_order_seq_cst);
        uint64_t t0 = now_ns();
        uint32_t n = std::min(h->frames, g.max_frames);
        run_cycle(n);
        uint64_t dt = now_ns() - t0;
        h->process_ns_last = dt;
        if (dt > h->process_ns_max)
            h->process_ns_max = dt;
        h->served++;
        g.in_cycle.store(false, std::memory_order_seq_cst);
        bl_slot1_sandbox_done(h, seq);
    }
    for (Slot *s : g.slots)
        if (s->processing) {
            s->inst->stop_processing();
            s->processing = false;
        }
    bl_rtalloc_leave();
}

// -------------------------------------------------------------- main thread
void build_aparams(Slot &s)
{
    s.aparams.clear();
    for (size_t i = 0; i < s.inst->params.size(); i++) {
        auto &p = s.inst->params[i];
        s.aparams.push_back({p.id, (uint32_t)i, p.cookie, p.min, p.max, p.stepped});
    }
    std::sort(s.aparams.begin(), s.aparams.end(), [](const AParam &a, const AParam &b) { return a.id < b.id; });
}

// Keeps the audio thread out of one insert (Dekker: the audio thread sets
// in_cycle, then reads suspended; we set suspended, then wait for in_cycle
// to drop). The audio thread is parked or past this insert afterwards.
void suspend(Slot &s)
{
    s.suspended.store(true, std::memory_order_seq_cst);
    while (g.in_cycle.load(std::memory_order_seq_cst))
        sched_yield();
    if (s.processing) {
        s.inst->stop_processing(); // the audio thread is out of it
        s.processing = false;
    }
}
void resume(Slot &s) { s.suspended.store(false, std::memory_order_seq_cst); }

void publish_latency()
{
    bl_slot1_header *h = g.h;
    uint32_t total = 0;
    bool changed = false;
    for (Slot *s : g.slots) {
        // LV2 reports latency on an output port, so only after a run: until
        // then keep what the memory says (a restart: the last generation's
        // value, so the engine's dry path stays aligned)
        uint32_t l = s->fmt == Fmt::Lv2 && !s->ran.load(std::memory_order_acquire) ? h->slots[s->index].latency
                                                                                   : s->inst->latency();
        if (l > h->max_latency)
            l = h->max_latency;
        if (l != s->latency || h->slots[s->index].latency != l) {
            s->latency = l;
            h->slots[s->index].latency = l;
            changed = true;
        }
        s->lat_audio.store(l, std::memory_order_relaxed);
        total += l;
    }
    if (total > h->max_latency)
        total = h->max_latency;
    if (changed || bl_slot1_load(&h->latency) != total) {
        bl_slot1_store(&h->latency, total);
        bl_slot1_store(&h->latency_serial, h->latency_serial + 1);
    }
}

void restart_slot(Slot &s)
{
    suspend(s);
    s.inst->deactivate();
    if (!s.inst->activate(g.rate, g.max_frames)) {
        LOG("insert %d (%s): re-activation failed; it passes its input through\n", s.index, s.inst->name.c_str());
        g.h->slots[s.index].flags |= BL_SLOT1_SLOT_FAILED;
        return; // stays suspended: bypassed with its last latency
    }
    publish_latency();
    resume(s);
}

std::string state_ext(Fmt f) { return f == Fmt::Vst3 ? ".vst3state" : f == Fmt::Lv2 ? ".lv2state" : ".clapstate"; }

void save_state(Slot &s)
{
    if (s.state_path.empty())
        return;
    std::vector<uint8_t> b;
    std::string err;
    if (!s.inst->save_state(b, &err))
        return;
    std::string tmp = s.state_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return;
    bool ok = fwrite(b.data(), 1, b.size(), f) == b.size() && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (ok)
        rename(tmp.c_str(), s.state_path.c_str());
    else
        unlink(tmp.c_str());
}

void load_state(Slot &s)
{
    if (s.state_path.empty())
        return;
    FILE *f = fopen(s.state_path.c_str(), "rb");
    if (!f)
        return;
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        b.insert(b.end(), buf, buf + n);
    fclose(f);
    std::string err;
    if (!s.inst->load_state(b, &err))
        LOG("insert %d: state %s not restored: %s\n", s.index, s.state_path.c_str(), err.c_str());
    else
        LOG("insert %d: state restored from %s\n", s.index, s.state_path.c_str());
}

void on_wake(void *, uint64_t)
{
    Applied ap;
    while (g.applied.pop(ap))
        if (ap.slot < g.slots.size()) {
            auto &I = *g.slots[ap.slot]->inst;
            if (ap.index < I.params.size())
                I.applied(I.params[ap.index], ap.value);
        }
    Vst3Plain v;
    while (g.to_main.pop(v)) {
        if (v.slot >= g.slots.size())
            continue;
        g.slots[v.slot]->host.set_param(v.id, v.value);
    }
    for (Slot *s : g.slots) {
        if (s->want_callback.exchange(false))
            s->inst->on_main_thread();
        if (s->want_rescan.exchange(false)) {
            suspend(*s);
            s->inst->rescan_params();
            build_aparams(*s);
            resume(*s);
        }
        if (s->want_restart.exchange(false))
            restart_slot(*s);
    }
}

void on_tick(void *, uint64_t)
{
    on_wake(nullptr, 0);
    publish_latency();
    // state: 100 ms after the last change
    uint32_t any = 0;
    for (Slot *s : g.slots)
        any |= s->dirty.load(std::memory_order_relaxed);
    uint64_t t = now_ns();
    if (any) {
        static uint32_t last_sum = 0;
        uint32_t sum = 0;
        for (Slot *s : g.slots)
            sum += s->dirty.load(std::memory_order_relaxed);
        if (sum != last_sum) {
            last_sum = sum;
            g.last_dirty_ns = t;
        } else if (t - g.last_dirty_ns > 100000000ull) {
            for (Slot *s : g.slots)
                if (s->dirty.exchange(0))
                    save_state(*s);
            last_sum = 0;
        }
    }
}

void on_signal(void *, int) { pw_main_loop_quit(g.loop); }

void setup_buses(Slot &s)
{
    auto *I = s.inst.get();
    s.in_tbl.assign(I->inputs.size(), {});
    s.out_tbl.assign(I->outputs.size(), {});
    for (size_t b = 0; b < I->inputs.size(); b++)
        s.in_tbl[b].assign(I->inputs[b].channels, g.zeros.data());
    for (size_t b = 0; b < I->outputs.size(); b++)
        s.out_tbl[b].assign(I->outputs[b].channels, g.trash.data());
    s.in_bus.clear();
    s.out_bus.clear();
    for (auto &t : s.in_tbl)
        s.in_bus.push_back(t.data());
    for (auto &t : s.out_tbl)
        s.out_bus.push_back(t.data());
    s.out0_channels = I->outputs.empty() ? 0 : std::min(I->outputs[0].channels, g.C);
    s.pevs.assign(kMaxEvents, {});
    s.mevs.assign(kMaxEvents, {});
    s.blen = g.h->max_latency + g.max_frames;
    s.bdly.assign((size_t)g.C * s.blen, 0.f);
}

std::string ready_json()
{
    std::string j = "{\"node\": " + bl::json_str(g.name);
    Slot *f = g.slots.empty() ? nullptr : g.slots[0];
    j += ", \"plugin\": " + bl::json_str(f ? f->inst->id : "");
    j += ", \"plugin_path\": " + bl::json_str(f ? f->inst->path : "");
    j += ", \"dbus\": \"\", \"osc_port\": 0, \"pid\": " + std::to_string(getpid());
    j += ", \"format\": \"slot\", \"slot\": {\"latency\": " + std::to_string(g.h->latency);
    j += ", \"channels\": " + std::to_string(g.C) + ", \"rate\": " + std::to_string(g.rate);
    j += ", \"generation\": " + std::to_string(g.h->generation) + ", \"inserts\": [";
    for (size_t k = 0; k < g.slots.size(); k++) {
        auto *I = g.slots[k]->inst.get();
        j += k ? ", " : "";
        j += "{\"index\": " + std::to_string(k) + ", \"format\": " + bl::json_str(I->format()) + ", \"id\": " + bl::json_str(I->id) +
             ", \"name\": " + bl::json_str(I->name) + ", \"latency\": " + std::to_string(g.slots[k]->latency) + ", \"params\": [";
        for (size_t i = 0; i < I->params.size(); i++) {
            auto &p = I->params[i];
            char b[160];
            snprintf(b, sizeof(b), ", \"min\": %.17g, \"max\": %.17g, \"default\": %.17g}", p.min, p.max, p.def);
            j += (i ? ", " : "") + std::string("{\"id\": ") + std::to_string(p.id) + ", \"name\": " + bl::json_str(p.name) + b;
        }
        j += "]}";
    }
    return j + "]}}";
}

void usage(FILE *f)
{
    fprintf(f,
            "usage: backline-slot-host (--shm PATH | --shm-fd N) [options] --plugin P [--plugin P ...]\n"
            "Runs one strip's insert chain over Slot1 shared memory (docs/api/Slot1.md).\n"
            "  --shm PATH          the engine's memory, /proc/<pid>/fd/<n>\n"
            "  --shm-fd N          ... or an inherited descriptor\n"
            "  --plugin P          an insert, in chain order: a .clap file, .vst3 bundle, LV2 URI or bundle,\n"
            "                      or a bare name; P#ID picks a plugin id inside a file or bundle\n"
            "  --format F          clap, vst3 or lv2, for the --plugin that follows\n"
            "  --name NAME         for the log and the ready line (default \"slot\")\n"
            "  --state-dir DIR     restore each insert from DIR/insert<k><ext> and keep it saved there\n"
            "  --rt-priority N     SCHED_FIFO priority of the audio thread (default 80; 0: none)\n");
}

} // namespace

int main(int argc, char **argv)
{
    bl::prog = kProg;
    // Machine-readable output (the ready line) keeps the real stdout; a
    // plugin that prints goes to stderr.
    int real_out = dup(1);
    dup2(2, 1);
    bl::out = fdopen(real_out, "w");

    static const option opts[] = {
        {"shm", required_argument, nullptr, 's'},      {"shm-fd", required_argument, nullptr, 'F'},
        {"plugin", required_argument, nullptr, 'p'},   {"format", required_argument, nullptr, 'f'},
        {"name", required_argument, nullptr, 'n'},     {"state-dir", required_argument, nullptr, 'S'},
        {"rt-priority", required_argument, nullptr, 'r'}, {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};
    Fmt next_fmt = Fmt::Auto;
    int c;
    while ((c = getopt_long(argc, argv, "h", opts, nullptr)) != -1) {
        switch (c) {
        case 's': g.shm_path = optarg; break;
        case 'F': g.shm_fd = atoi(optarg); break;
        case 'p': {
            auto *s = new Slot;
            s->spec = optarg;
            auto hash = s->spec.find('#');
            if (hash != std::string::npos && s->spec.find("://") == std::string::npos) {
                s->plugin_id = s->spec.substr(hash + 1);
                s->spec.resize(hash);
            } else if (hash != std::string::npos) {
                // an LV2 URI may hold '#'; only a bundle path takes #ID
            }
            s->fmt = next_fmt;
            next_fmt = Fmt::Auto;
            s->index = (int)g.slots.size();
            g.slots.push_back(s);
            break;
        }
        case 'f':
            if (!strcmp(optarg, "clap")) next_fmt = Fmt::Clap;
            else if (!strcmp(optarg, "vst3")) next_fmt = Fmt::Vst3;
            else if (!strcmp(optarg, "lv2")) next_fmt = Fmt::Lv2;
            else bl::die("unknown format '%s'", optarg);
            break;
        case 'n': g.name = optarg; break;
        case 'S': g.state_dir = optarg; break;
        case 'r': g.rt_priority = atoi(optarg); break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if ((g.shm_path.empty() && g.shm_fd < 0) || g.slots.empty()) {
        usage(stderr);
        return 2;
    }
    if (g.slots.size() > BL_SLOT1_MAX_SLOTS)
        bl::die("at most %u inserts per chain", BL_SLOT1_MAX_SLOTS);

    const char *why = nullptr;
    g.h = bl_slot1_map(g.shm_path.empty() ? nullptr : g.shm_path.c_str(), g.shm_fd, &g.size, &why);
    if (!g.h)
        bl::die("%s (%s)", why, g.shm_path.empty() ? "fd" : g.shm_path.c_str());
    bl_slot1_header *h = g.h;
    g.C = h->channels;
    g.max_frames = h->max_frames;
    g.rate = h->rate;
    bl_slot1_store(&h->state, (uint32_t)BL_SLOT1_LOADING);
    h->pid = (uint32_t)getpid();
    bl_rtalloc_counters(&h->audio_allocs, &h->audio_frees);

    pw_init(&argc, &argv);
    g.loop = pw_main_loop_new(nullptr);
    g.main_tid = std::this_thread::get_id();
    pw_loop *L = pw_main_loop_get_loop(g.loop);
    g.wake = pw_loop_add_event(L, on_wake, nullptr);
    pw_loop_add_signal(L, SIGTERM, on_signal, nullptr);
    pw_loop_add_signal(L, SIGINT, on_signal, nullptr);

    g.scratch.assign((size_t)g.C * g.max_frames, 0.f);
    g.zeros.assign(std::max<uint32_t>(g.max_frames, bl::kMaxFrames), 0.f);
    g.trash.assign(std::max<uint32_t>(g.max_frames, bl::kMaxFrames), 0.f);
    for (uint32_t ch = 0; ch < g.C; ch++) {
        g.shm_ch.push_back(bl_slot1_audio(h, ch));
        g.scr_ch.push_back(g.scratch.data() + (size_t)ch * g.max_frames);
    }

    for (Slot *s : g.slots) {
        s->host.s = s;
        auto [f, path] = bl::resolve_plugin(s->spec, s->fmt);
        s->fmt = f;
        s->inst = bl::load_plugin(&s->host, f, path, s->plugin_id, false);
        auto *I = s->inst.get();
        if (!g.state_dir.empty())
            s->state_path = g.state_dir + "/insert" + std::to_string(s->index) + state_ext(f);
        load_state(*s);
        if (!I->activate(g.rate, g.max_frames)) {
            bl_slot1_store(&h->state, (uint32_t)BL_SLOT1_FAILED);
            bl::die("insert %d (%s): activate failed at %u Hz", s->index, I->name.c_str(), g.rate);
        }
        setup_buses(*s);
        build_aparams(*s);
        auto &si = h->slots[s->index];
        snprintf(si.id, sizeof(si.id), "%s", I->id.c_str());
        si.flags = BL_SLOT1_SLOT_LOADED | (I->instrument ? BL_SLOT1_SLOT_INSTRUMENT : 0);
        LOG("insert %d: %s (%s, %s), %zu in / %zu out buses, %zu params\n", s->index, I->name.c_str(), I->format(),
            I->id.c_str(), I->inputs.size(), I->outputs.size(), I->params.size());
    }
    h->n_slots = (uint32_t)g.slots.size();
    publish_latency();
    mlockall(MCL_CURRENT); // best effort

    // take the memory over: whatever request is pending belongs to the last
    // sandbox; serve from the next one
    g.start_seen = bl_slot1_load(&h->req);
    bl_slot1_store(&h->done, g.start_seen);
    g.audio = std::thread(audio_main);
    bl_slot1_store(&h->generation, h->generation + 1);
    bl_slot1_store(&h->state, (uint32_t)BL_SLOT1_READY);
    bl_slot1_futex_wake(&h->done);

    g.tick = pw_loop_add_timer(L, on_tick, nullptr);
    timespec v{0, 20000000}, iv{0, 20000000};
    pw_loop_update_timer(L, g.tick, &v, &iv, false);

    fprintf(bl::out, "%s\n", ready_json().c_str());
    fflush(bl::out);
    LOG("%s: ready, %u channels at %u Hz, up to %u frames, %zu inserts, latency %u, generation %u\n", g.name.c_str(), g.C,
        g.rate, g.max_frames, g.slots.size(), h->latency, h->generation);

    pw_main_loop_run(g.loop);

    g.quit = 1;
    bl_slot1_futex_wake(&h->req);
    g.audio.join();
    for (Slot *s : g.slots)
        if (s->dirty.load())
            save_state(*s);
    bl_slot1_store(&h->state, (uint32_t)BL_SLOT1_EXITED);
    for (Slot *s : g.slots) {
        s->inst->deactivate();
        s->inst.reset();
        delete s;
    }
    pw_main_loop_destroy(g.loop);
    pw_deinit();
    return 0;
}
