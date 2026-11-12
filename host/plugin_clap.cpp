// CLAP adapter of the plugin worker.
// SPDX-License-Identifier: MIT
//
// Threads, as CLAP defines them: [main-thread] calls (init, activate, params
// get_info/flush while inactive, state, on_main_thread, timers, presets) on
// the worker's main loop; start/stop_processing and process on PipeWire's
// data loop. No locks or allocation in process().

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <functional>
#include <sys/stat.h>
#include <unordered_map>

#include <clap/clap.h>
#include <pipewire/loop.h>

#include "plugin.h"

namespace bl {
namespace {

struct alignas(8) EvSlot { uint8_t raw[64]; };

class ClapInstance final : public PluginInstance {
public:
    Host *H;
    void *dl = nullptr;
    const clap_plugin_entry *entry = nullptr;
    const clap_plugin *plugin = nullptr;
    clap_host host{};
    const clap_plugin_params *ext_params = nullptr;
    const clap_plugin_audio_ports *ext_audio_ports = nullptr;
    const clap_plugin_note_ports *ext_note_ports = nullptr;
    const clap_plugin_state *ext_state = nullptr;
    const clap_plugin_timer_support *ext_timer = nullptr;
    const clap_plugin_preset_load *ext_preset_load = nullptr;
    bool dialect_midi = false, dialect_clap = false;
    uint16_t note_port = 0;
    bool active = false;
    std::unordered_map<clap_id, spa_source *> timers;
    clap_id next_timer = 1;
    std::vector<uint32_t> in_channels, out_channels;

    // realtime
    std::vector<EvSlot> evs = std::vector<EvSlot>(kMaxEvents);
    uint32_t n_evs = 0;
    std::vector<clap_audio_buffer> in_bufs, out_bufs;
    clap_input_events in_events{};
    clap_output_events out_events{};

    // presets: last discovered list; id -> (kind, location, load_key)
    struct PresetRef { uint32_t kind; std::string location, load_key; };
    std::unordered_map<std::string, PresetRef> preset_refs;
    std::string preset_error;

    explicit ClapInstance(Host *h) : H(h)
    {
        in_events = {this, [](const clap_input_events *l) { return static_cast<ClapInstance *>(l->ctx)->n_evs; },
                     [](const clap_input_events *l, uint32_t i) {
                         return reinterpret_cast<const clap_event_header_t *>(static_cast<ClapInstance *>(l->ctx)->evs[i].raw);
                     }};
        out_events = {this, [](const clap_output_events *l, const clap_event_header_t *ev) {
                          auto *self = static_cast<ClapInstance *>(l->ctx);
                          if (ev->space_id == CLAP_CORE_EVENT_SPACE_ID && ev->type == CLAP_EVENT_PARAM_VALUE) {
                              auto *pv = reinterpret_cast<const clap_event_param_value *>(ev);
                              self->H->param_from_plugin(pv->param_id, pv->value);
                          }
                          return true;
                      }};
    }

    ~ClapInstance() override
    {
        if (plugin) {
            if (active)
                plugin->deactivate(plugin);
            plugin->destroy(plugin); // may unregister its timers itself
        }
        for (auto &[id, s] : timers)
            pw_loop_destroy_source(H->main_loop(), s);
        timers.clear();
        if (entry)
            entry->deinit();
        // dl stays loaded: plugins may leave threads or atexit handlers behind
    }

    const char *format() const override { return "clap"; }

    template <typename T> T *ev_alloc()
    {
        static_assert(sizeof(T) <= sizeof(EvSlot));
        if (n_evs >= kMaxEvents) {
            H->count_dropped();
            return nullptr;
        }
        auto *e = reinterpret_cast<T *>(evs[n_evs++].raw);
        memset(e, 0, sizeof(T));
        return e;
    }

    void push_param(uint32_t time, clap_id id, void *cookie, double v)
    {
        auto *e = ev_alloc<clap_event_param_value>();
        if (!e) return;
        e->header = {sizeof(*e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e->param_id = id;
        e->cookie = cookie;
        e->note_id = -1;
        e->port_index = -1;
        e->channel = -1;
        e->key = -1;
        e->value = v;
    }

    void push_midi(uint32_t time, const uint8_t *m, size_t n)
    {
        uint8_t status = m[0] & 0xf0;
        bool is_note = (status == 0x90 || status == 0x80) && n >= 3;
        if (dialect_midi) {
            if (m[0] >= 0xf0)
                return; // system/sysex: not forwarded yet
            auto *e = ev_alloc<clap_event_midi>();
            if (!e) return;
            e->header = {sizeof(*e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e->port_index = note_port;
            for (size_t i = 0; i < 3 && i < n; i++)
                e->data[i] = m[i];
            return;
        }
        if (dialect_clap && is_note) {
            auto *e = ev_alloc<clap_event_note>();
            if (!e) return;
            bool on = status == 0x90 && m[2] > 0;
            e->header = {sizeof(*e), time, CLAP_CORE_EVENT_SPACE_ID,
                         (uint16_t)(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            e->note_id = -1;
            e->port_index = (int16_t)note_port;
            e->channel = m[0] & 0x0f;
            e->key = m[1];
            e->velocity = on ? m[2] / 127.0 : 0.0;
        }
    }

    // ---------------------------------------------------------------- main
    void rescan_params() override
    {
        params.clear();
        if (!ext_params)
            return;
        uint32_t n = ext_params->count(plugin);
        for (uint32_t i = 0; i < n; i++) {
            clap_param_info info{};
            if (!ext_params->get_info(plugin, i, &info))
                continue;
            ParamInfo p;
            p.id = info.id;
            p.name = info.name;
            p.module = info.module;
            p.min = info.min_value;
            p.max = info.max_value;
            p.def = info.default_value;
            p.readonly = info.flags & CLAP_PARAM_IS_READONLY;
            p.stepped = info.flags & CLAP_PARAM_IS_STEPPED;
            p.cookie = info.cookie;
            params.push_back(std::move(p));
        }
    }

    bool read_param(const ParamInfo &p, double &v) override
    {
        if (!ext_params)
            return false;
        v = p.def;
        return ext_params->get_value(plugin, p.id, &v);
    }

    bool set_param_inactive(const ParamInfo &p, double v) override
    {
        if (!ext_params)
            return true;
        n_evs = 0;
        push_param(0, p.id, p.cookie, v);
        ext_params->flush(plugin, &in_events, &out_events);
        n_evs = 0;
        return true;
    }

    bool activate(uint32_t rate, uint32_t max_frames) override
    {
        if (!plugin->activate(plugin, rate, 1, max_frames))
            return false;
        active = true;
        return true;
    }
    void deactivate() override
    {
        if (active)
            plugin->deactivate(plugin);
        active = false;
    }

    void on_main_thread() override
    {
        plugin->on_main_thread(plugin);
        // params.request_flush while inactive: flush here (while active, the
        // next process() call delivers and collects parameter events anyway)
        if (ext_params && !active) {
            n_evs = 0;
            ext_params->flush(plugin, &in_events, &out_events);
        }
    }

    bool save_state(std::vector<uint8_t> &out, std::string *err) override
    {
        if (!ext_state) {
            if (err) *err = "plugin has no state extension";
            return false;
        }
        out.clear();
        clap_ostream os{&out, [](const clap_ostream *s, const void *buf, uint64_t size) -> int64_t {
                            auto *v = static_cast<std::vector<uint8_t> *>(s->ctx);
                            v->insert(v->end(), (const uint8_t *)buf, (const uint8_t *)buf + size);
                            return (int64_t)size;
                        }};
        if (!ext_state->save(plugin, &os)) {
            if (err) *err = "plugin failed to save state";
            return false;
        }
        return true;
    }

    struct ReadCtx { const std::vector<uint8_t> *v; size_t pos; };
    bool load_state(const std::vector<uint8_t> &in, std::string *err) override
    {
        if (!ext_state) {
            if (err) *err = "plugin has no state extension";
            return false;
        }
        ReadCtx rc{&in, 0};
        clap_istream is{&rc, [](const clap_istream *s, void *buf, uint64_t size) -> int64_t {
                            auto *c = static_cast<ReadCtx *>(s->ctx);
                            size_t n = std::min<size_t>(size, c->v->size() - c->pos);
                            memcpy(buf, c->v->data() + c->pos, n);
                            c->pos += n;
                            return (int64_t)n;
                        }};
        if (!ext_state->load(plugin, &is)) {
            if (err) *err = "plugin failed to load state";
            return false;
        }
        rescan_params();
        return true;
    }

    // ------------------------------------------------------------- presets
    // preset-discovery: every provider of the file, crawled for presets that
    // apply to this plugin id.
    struct Crawl {
        ClapInstance *self;
        std::vector<std::string> exts;
        std::vector<clap_preset_discovery_location_t> locs; // strings copied below
        std::vector<std::unique_ptr<std::string>> strs;
        std::vector<PresetInfo> *out;
        // per get_metadata call
        uint32_t kind = 0;
        std::string location;
        std::string cur_name, cur_key;
        bool cur_open = false, cur_ours = false, cur_any_id = false;
        void finish()
        {
            if (cur_open && (cur_ours || !cur_any_id)) {
                std::string id = kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
                                     ? "plugin:" + cur_key
                                     : location + (cur_key.empty() ? "" : "#" + cur_key);
                out->push_back({id, cur_name, kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? "(inside the plugin)" : location});
                self->preset_refs[id] = {kind, location, cur_key};
            }
            cur_open = false;
        }
    };

    std::vector<PresetInfo> list_presets() override
    {
        std::vector<PresetInfo> out;
        preset_refs.clear();
        if (!ext_preset_load)
            return out; // without preset-load there is no way to load one
        auto *f = (const clap_preset_discovery_factory *)entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
        if (!f)
            f = (const clap_preset_discovery_factory *)entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT);
        if (!f)
            return out;
        uint32_t np = f->count(f);
        for (uint32_t pi = 0; pi < np; pi++) {
            auto *pd = f->get_descriptor(f, pi);
            if (!pd)
                continue;
            Crawl c;
            c.self = this;
            c.out = &out;
            clap_preset_discovery_indexer idx{CLAP_VERSION_INIT, "Backline", "manuaudio", "https://github.com/manuaudio/backline-audio",
                                              "0.2.0", &c, nullptr, nullptr, nullptr, nullptr};
            idx.declare_filetype = [](const clap_preset_discovery_indexer *i, const clap_preset_discovery_filetype *ft) {
                auto *c = static_cast<Crawl *>(i->indexer_data);
                c->exts.push_back(ft->file_extension ? ft->file_extension : "");
                return true;
            };
            idx.declare_location = [](const clap_preset_discovery_indexer *i, const clap_preset_discovery_location *l) {
                auto *c = static_cast<Crawl *>(i->indexer_data);
                clap_preset_discovery_location_t copy = *l;
                c->strs.push_back(std::make_unique<std::string>(l->location ? l->location : ""));
                copy.location = l->location ? c->strs.back()->c_str() : nullptr;
                copy.name = nullptr;
                c->locs.push_back(copy);
                return true;
            };
            idx.declare_soundpack = [](const clap_preset_discovery_indexer *, const clap_preset_discovery_soundpack *) { return true; };
            idx.get_extension = [](const clap_preset_discovery_indexer *, const char *) -> const void * { return nullptr; };
            auto *prov = f->create(f, &idx, pd->id);
            if (!prov)
                continue;
            if (!prov->init(prov)) {
                prov->destroy(prov);
                continue;
            }
            clap_preset_discovery_metadata_receiver rcv{};
            rcv.receiver_data = &c;
            rcv.on_error = [](const clap_preset_discovery_metadata_receiver *, int32_t, const char *) {};
            rcv.begin_preset = [](const clap_preset_discovery_metadata_receiver *r, const char *name, const char *key) {
                auto *c = static_cast<Crawl *>(r->receiver_data);
                c->finish();
                c->cur_open = true;
                c->cur_ours = c->cur_any_id = false;
                c->cur_name = name ? name : "";
                c->cur_key = key ? key : "";
                return true;
            };
            rcv.add_plugin_id = [](const clap_preset_discovery_metadata_receiver *r, const clap_universal_plugin_id *pid) {
                auto *c = static_cast<Crawl *>(r->receiver_data);
                c->cur_any_id = true;
                if (pid && pid->abi && pid->id && !strcmp(pid->abi, "clap") && c->self->id == pid->id)
                    c->cur_ours = true;
            };
            rcv.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver *, const char *) {};
            rcv.set_flags = [](const clap_preset_discovery_metadata_receiver *, uint32_t) {};
            rcv.add_creator = [](const clap_preset_discovery_metadata_receiver *, const char *) {};
            rcv.set_description = [](const clap_preset_discovery_metadata_receiver *, const char *) {};
            rcv.set_timestamps = [](const clap_preset_discovery_metadata_receiver *, clap_timestamp, clap_timestamp) {};
            rcv.add_feature = [](const clap_preset_discovery_metadata_receiver *, const char *) {};
            rcv.add_extra_info = [](const clap_preset_discovery_metadata_receiver *, const char *, const char *) {};
            auto meta = [&](uint32_t kind, const std::string &loc) {
                c.kind = kind;
                c.location = loc;
                c.cur_open = false;
                prov->get_metadata(prov, kind, kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? nullptr : loc.c_str(), &rcv);
                c.finish();
            };
            size_t files = 0;
            std::function<void(const std::string &, int)> crawl = [&](const std::string &p, int depth) {
                struct stat st;
                if (files > 50000 || depth > 12 || stat(p.c_str(), &st) != 0)
                    return;
                if (S_ISREG(st.st_mode)) {
                    bool match = c.exts.empty();
                    for (auto &e : c.exts)
                        if (e.empty() || (p.size() > e.size() + 1 && p[p.size() - e.size() - 1] == '.' &&
                                          !strcasecmp(p.c_str() + p.size() - e.size(), e.c_str())))
                            match = true;
                    if (match) {
                        files++;
                        meta(CLAP_PRESET_DISCOVERY_LOCATION_FILE, p);
                    }
                    return;
                }
                if (!S_ISDIR(st.st_mode))
                    return;
                DIR *d = opendir(p.c_str());
                if (!d)
                    return;
                std::vector<std::string> names;
                while (dirent *de = readdir(d))
                    if (de->d_name[0] != '.')
                        names.push_back(de->d_name);
                closedir(d);
                std::sort(names.begin(), names.end());
                for (auto &n : names)
                    crawl(p + "/" + n, depth + 1);
            };
            for (auto &l : c.locs) {
                if (l.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
                    meta(CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, "");
                else if (l.location)
                    crawl(l.location, 0);
            }
            prov->destroy(prov);
        }
        return out;
    }

    bool load_preset(const PresetInfo &p, std::string *err) override
    {
        if (!ext_preset_load) {
            if (err) *err = "plugin has no preset-load extension";
            return false;
        }
        auto it = preset_refs.find(p.id);
        if (it == preset_refs.end()) {
            if (err) *err = "unknown preset id";
            return false;
        }
        preset_error.clear();
        auto &r = it->second;
        bool ok = ext_preset_load->from_location(plugin, r.kind,
                                                 r.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? nullptr : r.location.c_str(),
                                                 r.load_key.empty() ? nullptr : r.load_key.c_str());
        if (!ok || !preset_error.empty()) {
            if (err) *err = preset_error.empty() ? "plugin failed to load the preset" : preset_error;
            return false;
        }
        return true;
    }

    // --------------------------------------------------------------- audio
    bool start_processing() override { return plugin->start_processing(plugin); }
    void stop_processing() override { plugin->stop_processing(plugin); }

    bool process(const ProcessArgs &a) override
    {
        n_evs = 0;
        for (uint32_t i = 0; i < a.n_params; i++)
            push_param(0, a.params[i].id, a.params[i].cookie, a.params[i].value);
        if (midi_input)
            for (uint32_t i = 0; i < a.n_midi; i++)
                push_midi(a.midi[i].time, a.midi[i].data, a.midi[i].size);
        for (size_t b = 0; b < in_bufs.size(); b++)
            in_bufs[b] = {const_cast<float **>(a.ins[b]), nullptr, in_channels[b], 0, 0};
        for (size_t b = 0; b < out_bufs.size(); b++)
            out_bufs[b] = {const_cast<float **>(a.outs[b]), nullptr, out_channels[b], 0, 0};
        clap_process p{};
        p.steady_time = a.steady;
        p.frames_count = a.frames;
        p.transport = nullptr; // TODO: from the Backline clock service / spa_io_position segment
        p.audio_inputs = in_bufs.data();
        p.audio_outputs = out_bufs.data();
        p.audio_inputs_count = (uint32_t)in_bufs.size();
        p.audio_outputs_count = (uint32_t)out_bufs.size();
        p.in_events = &in_events;
        p.out_events = &out_events;
        return plugin->process(plugin, &p) != CLAP_PROCESS_ERROR;
    }

    void add_ports(bool input)
    {
        uint32_t n = ext_audio_ports ? ext_audio_ports->count(plugin, input) : 0;
        for (uint32_t i = 0; i < n; i++) {
            clap_audio_port_info info{};
            if (!ext_audio_ports->get(plugin, i, input, &info))
                continue;
            (input ? inputs : outputs).push_back({info.name, info.channel_count});
            (input ? in_channels : out_channels).push_back(info.channel_count);
        }
        (input ? in_bufs : out_bufs).resize((input ? inputs : outputs).size());
    }
};

// ======================================================= host callbacks
ClapInstance *self_of(const clap_host *h) { return static_cast<ClapInstance *>(h->host_data); }

void host_request_restart(const clap_host *h) { self_of(h)->H->request_restart(); }
void host_request_process(const clap_host *) {}
void host_request_callback(const clap_host *h) { self_of(h)->H->request_callback(); }

void host_log(const clap_host *h, clap_log_severity sev, const char *msg)
{
    static const char *names[] = {"debug", "info", "warning", "error", "fatal", "host-misbehaving", "plugin-misbehaving"};
    if (sev >= CLAP_LOG_INFO)
        self_of(h)->H->log("plugin %s: %s\n", sev <= CLAP_LOG_PLUGIN_MISBEHAVING ? names[sev] : "?", msg);
}
const clap_host_log host_log_ext{host_log};

bool host_is_main_thread(const clap_host *h) { return self_of(h)->H->is_main_thread(); }
bool host_is_audio_thread(const clap_host *h) { return self_of(h)->H->is_audio_thread(); }
const clap_host_thread_check host_thread_check_ext{host_is_main_thread, host_is_audio_thread};

void host_params_rescan(const clap_host *h, clap_param_rescan_flags flags)
{
    if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_INFO))
        self_of(h)->rescan_params();
    if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_VALUES))
        self_of(h)->H->params_changed();
}
void host_params_clear(const clap_host *, clap_id, clap_param_clear_flags) {}
void host_params_request_flush(const clap_host *h) { self_of(h)->H->request_callback(); }
const clap_host_params host_params_ext{host_params_rescan, host_params_clear, host_params_request_flush};

void host_state_mark_dirty(const clap_host *h) { self_of(h)->H->mark_dirty(); }
const clap_host_state host_state_ext{host_state_mark_dirty};

void host_latency_changed(const clap_host *h) { self_of(h)->H->log("plugin latency changed\n"); }
const clap_host_latency host_latency_ext{host_latency_changed};

bool host_ap_is_rescan_flag_supported(const clap_host *, uint32_t) { return false; }
void host_ap_rescan(const clap_host *h, uint32_t) { self_of(h)->H->log("plugin asked for an audio port rescan (not supported yet)\n"); }
const clap_host_audio_ports host_audio_ports_ext{host_ap_is_rescan_flag_supported, host_ap_rescan};

uint32_t host_np_supported_dialects(const clap_host *) { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; }
void host_np_rescan(const clap_host *, uint32_t) {}
const clap_host_note_ports host_note_ports_ext{host_np_supported_dialects, host_np_rescan};

struct TimerCtx { ClapInstance *self; clap_id id; };
void on_plugin_timer(void *data, uint64_t)
{
    auto *t = static_cast<TimerCtx *>(data);
    if (t->self->ext_timer)
        t->self->ext_timer->on_timer(t->self->plugin, t->id);
}
bool host_register_timer(const clap_host *h, uint32_t period_ms, clap_id *timer_id)
{
    auto *self = self_of(h);
    pw_loop *l = self->H->main_loop();
    clap_id id = self->next_timer++;
    auto *ctx = new TimerCtx{self, id}; // freed with the process
    spa_source *s = pw_loop_add_timer(l, on_plugin_timer, ctx);
    timespec v{period_ms / 1000, (long)(period_ms % 1000) * 1000000L};
    if (period_ms == 0)
        v.tv_nsec = 1000000;
    pw_loop_update_timer(l, s, &v, &v, false);
    self->timers[id] = s;
    *timer_id = id;
    return true;
}
bool host_unregister_timer(const clap_host *h, clap_id id)
{
    auto *self = self_of(h);
    auto it = self->timers.find(id);
    if (it == self->timers.end())
        return false;
    pw_loop_destroy_source(self->H->main_loop(), it->second);
    self->timers.erase(it);
    return true;
}
const clap_host_timer_support host_timer_ext{host_register_timer, host_unregister_timer};

void host_preset_on_error(const clap_host *h, uint32_t, const char *, const char *, int32_t os_error, const char *msg)
{
    self_of(h)->preset_error = msg ? msg : ("error " + std::to_string(os_error));
}
void host_preset_loaded(const clap_host *h, uint32_t, const char *location, const char *)
{
    self_of(h)->H->log("preset loaded: %s\n", location ? location : "(plugin)");
    self_of(h)->H->mark_dirty();
}
const clap_host_preset_load host_preset_load_ext{host_preset_on_error, host_preset_loaded};

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
    if (!strcmp(id, CLAP_EXT_PRESET_LOAD) || !strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)) return &host_preset_load_ext;
    return nullptr;
}

bool has_feature(const clap_plugin_descriptor *d, const char *f)
{
    for (auto *x = d->features; x && *x; x++)
        if (!strcmp(*x, f))
            return true;
    return false;
}

} // namespace

std::unique_ptr<PluginInstance> load_clap(Host *h, const std::string &path, const std::string &plugin_id, bool list_only)
{
    auto I = std::make_unique<ClapInstance>(h);
    I->path = path;
    I->dl = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!I->dl)
        die("dlopen: %s", dlerror());
    I->entry = (const clap_plugin_entry *)dlsym(I->dl, "clap_entry");
    if (!I->entry || !clap_version_is_compatible(I->entry->clap_version))
        die("%s: no compatible clap_entry", path.c_str());
    if (!I->entry->init(path.c_str()))
        die("clap_entry.init failed");
    auto *f = (const clap_plugin_factory *)I->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!f)
        die("no plugin factory");
    uint32_t n = f->get_plugin_count(f);
    if (list_only) {
        printf("[\n");
        for (uint32_t i = 0; i < n; i++) {
            auto *d = f->get_plugin_descriptor(f, i);
            printf("  {\"format\": \"clap\", \"id\": %s, \"name\": %s, \"vendor\": %s, \"features\": [", json_str(d->id).c_str(),
                   json_str(d->name).c_str(), json_str(d->vendor ? d->vendor : "").c_str());
            for (auto *x = d->features; x && *x; x++)
                printf("%s%s", x == d->features ? "" : ", ", json_str(*x).c_str());
            printf("]}%s\n", i + 1 < n ? "," : "");
        }
        printf("]\n");
        exit(0);
    }
    const clap_plugin_descriptor *chosen = nullptr;
    for (uint32_t i = 0; i < n; i++) {
        auto *d = f->get_plugin_descriptor(f, i);
        if (!plugin_id.empty() ? plugin_id == d->id : has_feature(d, CLAP_PLUGIN_FEATURE_INSTRUMENT)) {
            chosen = d;
            break;
        }
    }
    if (!chosen && plugin_id.empty() && n)
        chosen = f->get_plugin_descriptor(f, 0);
    if (!chosen)
        die("plugin id '%s' not in %s", plugin_id.c_str(), path.c_str());
    I->name = chosen->name;
    I->vendor = chosen->vendor ? chosen->vendor : "";
    I->id = chosen->id;
    I->node_suffix = chosen->id;
    I->instrument = has_feature(chosen, CLAP_PLUGIN_FEATURE_INSTRUMENT);

    I->host = {CLAP_VERSION_INIT, I.get(), "Backline plugin host", "manuaudio", "https://github.com/manuaudio/backline-audio", "0.2.0",
               host_get_extension, host_request_restart, host_request_process, host_request_callback};
    I->plugin = f->create_plugin(f, &I->host, chosen->id);
    if (!I->plugin || !I->plugin->init(I->plugin))
        die("could not create/init %s", chosen->id);
    auto *p = I->plugin;
    I->ext_params = (const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
    I->ext_audio_ports = (const clap_plugin_audio_ports *)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
    I->ext_note_ports = (const clap_plugin_note_ports *)p->get_extension(p, CLAP_EXT_NOTE_PORTS);
    I->ext_state = (const clap_plugin_state *)p->get_extension(p, CLAP_EXT_STATE);
    I->ext_timer = (const clap_plugin_timer_support *)p->get_extension(p, CLAP_EXT_TIMER_SUPPORT);
    I->ext_preset_load = (const clap_plugin_preset_load *)p->get_extension(p, CLAP_EXT_PRESET_LOAD);
    if (!I->ext_preset_load)
        I->ext_preset_load = (const clap_plugin_preset_load *)p->get_extension(p, CLAP_EXT_PRESET_LOAD_COMPAT);

    if (I->ext_note_ports && I->ext_note_ports->count(p, true) > 0) {
        clap_note_port_info ni{};
        I->ext_note_ports->get(p, 0, true, &ni);
        I->midi_input = true;
        I->note_port = 0;
        I->dialect_midi = ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI;
        I->dialect_clap = ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP;
        I->midi_dialect = I->dialect_midi ? "midi" : "clap";
    }
    I->add_ports(true);
    I->add_ports(false);
    I->rescan_params();
    return I;
}

} // namespace bl
