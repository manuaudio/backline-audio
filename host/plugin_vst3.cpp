// VST3 adapter of the plugin worker, on the VST3 SDK's hosting classes (MIT).
// SPDX-License-Identifier: MIT
//
// Threads: the module, component and controller are created and driven on
// the worker's main loop (the "UI thread" in VST3 terms); setProcessing and
// process run on PipeWire's data loop. Parameter changes reach the processor
// through IParameterChanges in process() (sample offset 0), notes through
// IEventList at their sample offset. Controller callbacks that arrive on
// another thread (a plugin's own message thread) are queued under a lock that
// only non-audio threads take, then handled on main.

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/vstpresetfile.h"

#include "plugin.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace bl {
namespace {

std::string utf8(const char16 *s, size_t max = 128)
{
    std::string o;
    for (size_t i = 0; i < max && s[i]; i++) {
        uint32_t c = (uint16_t)s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < max && (uint16_t)s[i + 1] >= 0xdc00 && (uint16_t)s[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + ((uint16_t)s[i + 1] - 0xdc00);
            i++;
        }
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xc0 | c >> 6); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xe0 | c >> 12); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xf0 | c >> 18); o += (char)(0x80 | ((c >> 12) & 63)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
    }
    return o;
}

std::string hex_id(const VST3::UID &u)
{
    static const char *d = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 16; i++) {
        uint8_t b = (uint8_t)u.data()[i];
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

bool mkdir_p(const std::string &p)
{
    for (size_t i = 1; i <= p.size(); i++)
        if (i == p.size() || p[i] == '/') {
            std::string d = p.substr(0, i);
            if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    return true;
}

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

class Vst3Instance;

// IComponentHandler: the controller's edits and restart requests.
class Handler : public IComponentHandler {
public:
    Vst3Instance *self = nullptr;
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<IComponentHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override;
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override;
};

class Vst3Instance final : public PluginInstance {
public:
    Host *H;
    VST3::Hosting::Module::Ptr module;
    VST3::Hosting::ClassInfo info;
    FUID cid;
    IPtr<HostApplication> host_app;
    IPtr<PlugProvider> provider;
    IPtr<IComponent> component;
    IPtr<IEditController> controller;
    IPtr<IAudioProcessor> processor;
    IPtr<IUnitInfo> units;
    Handler handler;
    bool active = false;
    uint32_t rate = 48000;

    // buses: every audio bus the plugin has; exposed[] says which become ports
    struct Bus { int32 index; uint32_t channels; bool exposed; };
    std::vector<Bus> in_buses, out_buses;
    std::vector<AudioBusBuffers> in_bb, out_bb;
    std::vector<float *> zero_ptrs, trash_ptrs;
    std::vector<float> zeros = std::vector<float>(kMaxFrames, 0.f), trash = std::vector<float>(kMaxFrames, 0.f);
    bool event_bus = false;

    // realtime
    EventList events{(int32)kMaxEvents};
    EventList out_events{(int32)kMaxEvents};
    ParameterChanges in_params{64}, out_params{64};
    ProcessContext ctx{};
    ParamID cc_map[16][130];                 // MIDI CC / aftertouch / pitch bend -> parameter
    struct Pending { ParamID id; double v; };
    std::vector<Pending> pending;            // set while inactive; sent on the first process
    size_t n_pending = 0;

    // controller callbacks from other threads
    std::mutex edits_lock;
    struct Edit { bool restart; ParamID id; double v; int32 flags; };
    std::vector<Edit> edits;

    // presets: program lists
    std::unordered_map<UnitID, ParamID> program_param; // unit -> its program-change parameter
    std::unordered_map<std::string, std::pair<ParamID, double>> program_presets; // id -> (param, normalized)

    explicit Vst3Instance(Host *h) : H(h) { handler.self = this; }

    ~Vst3Instance() override
    {
        if (active)
            component->setActive(false);
        if (controller)
            controller->setComponentHandler(nullptr);
        processor = nullptr;
        units = nullptr;
        component = nullptr;
        controller = nullptr;
        provider = nullptr; // terminates and disconnects the plugin
        // the module stays loaded: plugins may leave threads behind
        if (module)
            new VST3::Hosting::Module::Ptr(module);
    }

    const char *format() const override { return "vst3"; }

    double to_plain(ParamID id, double n) { return controller->normalizedParamToPlain(id, n); }
    double to_norm(ParamID id, double v) { return std::clamp(controller->plainParamToNormalized(id, v), 0.0, 1.0); }

    // ---------------------------------------------------------------- main
    void rescan_params() override
    {
        params.clear();
        program_param.clear();
        int32 n = controller->getParameterCount();
        std::unordered_map<UnitID, std::string> unit_names;
        if (units)
            for (int32 i = 0; i < units->getUnitCount(); i++) {
                UnitInfo ui{};
                if (units->getUnitInfo(i, ui) == kResultOk && ui.id != kRootUnitId)
                    unit_names[ui.id] = utf8(ui.name);
            }
        for (int32 i = 0; i < n; i++) {
            ParameterInfo pi{};
            if (controller->getParameterInfo(i, pi) != kResultOk)
                continue;
            ParamInfo p;
            p.id = pi.id;
            p.name = utf8(pi.title);
            auto u = unit_names.find(pi.unitId);
            if (u != unit_names.end())
                p.module = u->second;
            p.min = to_plain(pi.id, 0.0);
            p.max = to_plain(pi.id, 1.0);
            if (p.min > p.max)
                std::swap(p.min, p.max);
            p.def = to_plain(pi.id, pi.defaultNormalizedValue);
            p.readonly = pi.flags & ParameterInfo::kIsReadOnly;
            p.stepped = pi.stepCount > 0;
            if (pi.flags & ParameterInfo::kIsProgramChange)
                program_param[pi.unitId] = pi.id;
            params.push_back(std::move(p));
        }
        in_params.setMaxParameters((int32)params.size() + 16);
        out_params.setMaxParameters((int32)params.size() + 16);
        pending.reserve(params.size() + 16);
    }

    bool live_values() const override { return true; }

    bool read_param(const ParamInfo &p, double &v) override
    {
        v = to_plain(p.id, controller->getParamNormalized(p.id));
        return true;
    }

    double prepare_set(const ParamInfo &p, double v) override
    {
        double n = to_norm(p.id, v);
        controller->setParamNormalized(p.id, n);
        return n;
    }

    bool set_param_inactive(const ParamInfo &p, double v) override
    {
        // no process() to deliver it now: the controller has it already
        // (prepare_set); the processor gets it with the first cycle
        double n = to_norm(p.id, v);
        for (size_t i = 0; i < n_pending; i++)
            if (pending[i].id == p.id) {
                pending[i].v = n;
                return true;
            }
        if (n_pending < pending.capacity()) {
            if (n_pending == pending.size())
                pending.push_back({p.id, n});
            else
                pending[n_pending] = {p.id, n};
            n_pending++;
        }
        return true;
    }

    double plugin_value_to_public(uint32_t id, double n) override
    {
        controller->setParamNormalized(id, n);
        return to_plain(id, n);
    }

    bool activate(uint32_t r, uint32_t max_frames) override
    {
        rate = r;
        ProcessSetup setup{kRealtime, kSample32, (int32)max_frames, (SampleRate)r};
        if (processor->setupProcessing(setup) != kResultOk)
            return false;
        if (component->setActive(true) != kResultOk)
            return false;
        active = true;
        return true;
    }

    void deactivate() override
    {
        if (active)
            component->setActive(false);
        active = false;
    }

    void queue_edit(const Edit &e)
    {
        {
            std::lock_guard<std::mutex> g(edits_lock);
            edits.push_back(e);
        }
        H->request_callback();
    }

    void apply_edit(const Edit &e)
    {
        if (!e.restart) {
            H->set_param(e.id, to_plain(e.id, e.v));
            return;
        }
        if (e.flags & (kParamTitlesChanged | kReloadComponent)) {
            rescan_params();
            H->params_changed();
        } else if (e.flags & kParamValuesChanged) {
            H->params_changed();
        }
        if (e.flags & kLatencyChanged)
            H->log("plugin latency changed\n");
        if (e.flags & (kReloadComponent | kIoChanged))
            H->log("plugin asked for a component reload / bus change (not supported yet)\n");
        H->mark_dirty();
    }

    void on_main_thread() override
    {
        std::vector<Edit> todo;
        {
            std::lock_guard<std::mutex> g(edits_lock);
            todo.swap(edits);
        }
        for (auto &e : todo)
            apply_edit(e);
    }

    // State: one .vstpreset image (component and controller chunks), the
    // Steinberg preset format, so a state file is also a preset file.
    bool save_state(std::vector<uint8_t> &out, std::string *err) override
    {
        MemoryStream ms;
        if (!PresetFile::savePreset(&ms, cid, component, controller)) {
            if (err) *err = "plugin failed to save state";
            return false;
        }
        out.assign((uint8_t *)ms.getData(), (uint8_t *)ms.getData() + ms.getSize());
        return true;
    }

    bool load_state(const std::vector<uint8_t> &in, std::string *err) override
    {
        MemoryStream ms((void *)in.data(), (TSize)in.size());
        if (!PresetFile::loadPreset(&ms, cid, component, controller)) {
            if (err) *err = "plugin failed to load state (or the file is for another plugin)";
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------- presets
    std::vector<std::string> preset_dirs()
    {
        std::vector<std::string> v = extra_preset_dirs;
        const char *h = getenv("HOME");
        std::string vendor = info.vendor().empty() ? this->vendor : info.vendor();
        std::string sub = "/" + vendor + "/" + info.name();
        if (h)
            v.push_back(std::string(h) + "/.vst3/presets" + sub);
        v.push_back("/usr/share/vst3/presets" + sub);
        v.push_back("/usr/local/share/vst3/presets" + sub);
        return v;
    }

    bool preset_is_ours(const std::string &path)
    {
        char head[48];
        std::ifstream f(path, std::ios::binary);
        if (!f.read(head, sizeof(head)) || memcmp(head, "VST3", 4))
            return false;
        char8 want[33];
        cid.toString(want);
        return !strncasecmp(head + 8, want, 32);
    }

    std::vector<PresetInfo> list_presets() override
    {
        std::vector<PresetInfo> out;
        // .vstpreset files in the standard directories
        std::function<void(const std::string &, int)> crawl = [&](const std::string &dir, int depth) {
            DIR *d = opendir(dir.c_str());
            if (!d || depth > 8) {
                if (d) closedir(d);
                return;
            }
            std::vector<std::string> names;
            while (dirent *de = readdir(d))
                if (de->d_name[0] != '.')
                    names.push_back(de->d_name);
            closedir(d);
            std::sort(names.begin(), names.end());
            for (auto &n : names) {
                std::string p = dir + "/" + n;
                struct stat st;
                if (stat(p.c_str(), &st) != 0)
                    continue;
                if (S_ISDIR(st.st_mode))
                    crawl(p, depth + 1);
                else if (n.size() > 10 && !strcasecmp(n.c_str() + n.size() - 10, ".vstpreset") && preset_is_ours(p))
                    out.push_back({p, n.substr(0, n.size() - 10), p});
            }
        };
        for (auto &d : preset_dirs())
            crawl(d, 0);
        // programs the plugin carries in its program lists
        program_presets.clear();
        if (units) {
            for (int32 i = 0; i < units->getUnitCount(); i++) {
                UnitInfo ui{};
                if (units->getUnitInfo(i, ui) != kResultOk || ui.programListId == kNoProgramListId)
                    continue;
                auto pp = program_param.find(ui.id);
                if (pp == program_param.end())
                    continue;
                for (int32 l = 0; l < units->getProgramListCount(); l++) {
                    ProgramListInfo li{};
                    if (units->getProgramListInfo(l, li) != kResultOk || li.id != ui.programListId)
                        continue;
                    for (int32 k = 0; k < li.programCount; k++) {
                        String128 name{};
                        units->getProgramName(li.id, k, name);
                        std::string id = "program:" + std::to_string(li.id) + ":" + std::to_string(k);
                        out.push_back({id, utf8(name), "program list '" + utf8(li.name) + "'"});
                        program_presets[id] = {pp->second, li.programCount > 1 ? (double)k / (li.programCount - 1) : 0.0};
                    }
                }
            }
        }
        return out;
    }

    bool load_preset(const PresetInfo &p, std::string *err) override
    {
        auto pr = program_presets.find(p.id);
        if (pr != program_presets.end()) {
            int r = H->set_param(pr->second.first, to_plain(pr->second.first, pr->second.second));
            if (r < 0 && err) *err = "could not set the program parameter";
            return r >= 0;
        }
        std::vector<uint8_t> data;
        if (!read_file(p.location, data)) {
            if (err) *err = "cannot read " + p.location;
            return false;
        }
        if (!load_state(data, err))
            return false;
        H->mark_dirty();
        return true;
    }

    bool save_preset(const std::string &name_or_path, std::string &path, std::string *err) override
    {
        if (name_or_path.find('/') != std::string::npos) {
            path = name_or_path;
        } else {
            auto dirs = preset_dirs();
            path = dirs[0] + "/" + name_or_path;
        }
        if (path.size() < 10 || strcasecmp(path.c_str() + path.size() - 10, ".vstpreset"))
            path += ".vstpreset";
        auto slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0 && !mkdir_p(path.substr(0, slash))) {
            if (err) *err = "cannot create " + path.substr(0, slash) + ": " + strerror(errno);
            return false;
        }
        std::vector<uint8_t> data;
        if (!save_state(data, err))
            return false;
        std::string tmp = path + ".tmp";
        std::ofstream f(tmp, std::ios::binary);
        f.write((const char *)data.data(), (std::streamsize)data.size());
        f.close();
        if (!f || rename(tmp.c_str(), path.c_str()) != 0) {
            unlink(tmp.c_str());
            if (err) *err = "cannot write " + path;
            return false;
        }
        return true;
    }

    // --------------------------------------------------------------- audio
    bool start_processing() override
    {
        tresult r = processor->setProcessing(true);
        return r == kResultOk || r == kNotImplemented;
    }
    void stop_processing() override { processor->setProcessing(false); }

    void add_param_change(ParamID id, int32 offset, double v)
    {
        int32 qi = 0, pi = 0;
        if (IParamValueQueue *q = in_params.addParameterData(id, qi))
            q->addPoint(offset, v, pi);
    }

    void add_midi(const MidiEv &m)
    {
        uint8_t st = m.data[0] & 0xf0, ch = m.data[0] & 0x0f;
        Event e{};
        e.busIndex = 0;
        e.sampleOffset = (int32)m.time;
        e.flags = Event::kIsLive;
        switch (st) {
        case 0x90:
        case 0x80:
            if (m.size < 3) return;
            if (st == 0x90 && m.data[2] > 0) {
                e.type = Event::kNoteOnEvent;
                e.noteOn = {(int16)ch, (int16)m.data[1], 0.f, m.data[2] / 127.f, 0, -1};
            } else {
                e.type = Event::kNoteOffEvent;
                e.noteOff = {(int16)ch, (int16)m.data[1], m.data[2] / 127.f, -1, 0.f};
            }
            break;
        case 0xa0:
            if (m.size < 3) return;
            e.type = Event::kPolyPressureEvent;
            e.polyPressure = {(int16)ch, (int16)m.data[1], m.data[2] / 127.f, -1};
            break;
        case 0xb0:
            if (m.size >= 3 && cc_map[ch][m.data[1]] != kNoParamId)
                add_param_change(cc_map[ch][m.data[1]], e.sampleOffset, m.data[2] / 127.0);
            return;
        case 0xd0:
            if (cc_map[ch][kAfterTouch] != kNoParamId)
                add_param_change(cc_map[ch][kAfterTouch], e.sampleOffset, m.data[1] / 127.0);
            return;
        case 0xe0:
            if (m.size >= 3 && cc_map[ch][kPitchBend] != kNoParamId)
                add_param_change(cc_map[ch][kPitchBend], e.sampleOffset, ((m.data[2] << 7) | m.data[1]) / 16383.0);
            return;
        default:
            return;
        }
        if (events.addEvent(e) != kResultOk)
            H->count_dropped();
    }

    bool process(const ProcessArgs &a) override
    {
        in_params.clearQueue();
        out_params.clearQueue();
        events.clear();
        out_events.clear();
        for (size_t i = 0; i < n_pending; i++)
            add_param_change(pending[i].id, 0, pending[i].v);
        n_pending = 0;
        for (uint32_t i = 0; i < a.n_params; i++)
            add_param_change(a.params[i].id, 0, a.params[i].value);
        if (event_bus)
            for (uint32_t i = 0; i < a.n_midi; i++)
                add_midi(a.midi[i]);
        size_t k = 0;
        for (size_t b = 0; b < in_buses.size(); b++) {
            in_bb[b].numChannels = (int32)in_buses[b].channels;
            in_bb[b].silenceFlags = 0;
            in_bb[b].channelBuffers32 = in_buses[b].exposed ? const_cast<float **>(a.ins[k++]) : zero_ptrs.data();
        }
        k = 0;
        for (size_t b = 0; b < out_buses.size(); b++) {
            out_bb[b].numChannels = (int32)out_buses[b].channels;
            out_bb[b].silenceFlags = 0;
            out_bb[b].channelBuffers32 = out_buses[b].exposed ? const_cast<float **>(a.outs[k++]) : trash_ptrs.data();
        }
        // The shared transport (backline-clockd's segment). Musical positions
        // and tempo are in quarter notes, as VST3 defines them.
        const Transport &t = *a.transport;
        ctx = {};
        ctx.state = ProcessContext::kContTimeValid | ProcessContext::kSystemTimeValid;
        ctx.sampleRate = rate;
        ctx.continousTimeSamples = a.steady;
        ctx.systemTime = (int64)t.nsec;
        ctx.projectTimeSamples = t.valid ? t.frame : a.steady;
        if (t.rolling)
            ctx.state |= ProcessContext::kPlaying;
        if (t.bar_valid) {
            ctx.state |= ProcessContext::kTempoValid | ProcessContext::kTimeSigValid |
                         ProcessContext::kProjectTimeMusicValid | ProcessContext::kBarPositionValid;
            ctx.tempo = t.qn_tempo();
            ctx.projectTimeMusic = t.qn();
            ctx.barPositionMusic = t.qn_bar_start();
            ctx.timeSigNumerator = (int32)std::lround(t.sig_num);
            ctx.timeSigDenominator = (int32)std::lround(t.sig_den);
        } else {
            ctx.tempo = 120;
            ctx.timeSigNumerator = 4;
            ctx.timeSigDenominator = 4;
        }

        ProcessData d;
        d.processMode = kRealtime;
        d.symbolicSampleSize = kSample32;
        d.numSamples = (int32)a.frames;
        d.numInputs = (int32)in_bb.size();
        d.numOutputs = (int32)out_bb.size();
        d.inputs = in_bb.empty() ? nullptr : in_bb.data();
        d.outputs = out_bb.empty() ? nullptr : out_bb.data();
        d.inputParameterChanges = &in_params;
        d.outputParameterChanges = &out_params;
        d.inputEvents = &events;
        d.outputEvents = &out_events;
        d.processContext = &ctx;
        tresult r = processor->process(d);
        for (int32 i = 0; i < out_params.getParameterCount(); i++) {
            IParamValueQueue *q = out_params.getParameterData(i);
            int32 n = q ? q->getPointCount() : 0, off;
            ParamValue v;
            if (n > 0 && q->getPoint(n - 1, off, v) == kResultOk)
                H->param_from_plugin(q->getParameterId(), v);
        }
        return r == kResultOk;
    }

    // ---------------------------------------------------------------- setup
    void setup_buses()
    {
        uint32_t maxch = 2;
        for (int dir = 0; dir < 2; dir++) {
            BusDirection bd = dir == 0 ? kInput : kOutput;
            auto &buses = dir == 0 ? in_buses : out_buses;
            int32 n = component->getBusCount(kAudio, bd);
            for (int32 i = 0; i < n; i++) {
                Steinberg::Vst::BusInfo bi{};
                component->getBusInfo(kAudio, bd, i, bi);
                // the main bus, and any the plugin wants on by default
                bool on = i == 0 || (bi.flags & Steinberg::Vst::BusInfo::kDefaultActive);
                component->activateBus(kAudio, bd, i, on);
                uint32_t ch = (uint32_t)std::max<int32>(bi.channelCount, 0);
                maxch = std::max(maxch, ch);
                buses.push_back({i, ch, on && ch > 0});
                if (on && ch > 0)
                    (dir == 0 ? inputs : outputs).push_back({utf8(bi.name), ch});
            }
        }
        if (component->getBusCount(kEvent, kInput) > 0) {
            component->activateBus(kEvent, kInput, 0, true);
            event_bus = true;
        }
        in_bb.resize(in_buses.size());
        out_bb.resize(out_buses.size());
        zero_ptrs.assign(maxch, zeros.data());
        trash_ptrs.assign(maxch, trash.data());
    }

    void setup_midi_map()
    {
        for (auto &row : cc_map)
            for (auto &x : row)
                x = kNoParamId;
        FUnknownPtr<IMidiMapping> mm(controller);
        if (!mm)
            return;
        for (int16 ch = 0; ch < 16; ch++)
            for (CtrlNumber cc = 0; cc < 130; cc++) {
                ParamID id;
                if (mm->getMidiControllerAssignment(0, ch, cc, id) == kResultOk)
                    cc_map[ch][cc] = id;
            }
    }
};

tresult PLUGIN_API Handler::performEdit(ParamID id, ParamValue v)
{
    Vst3Instance::Edit e{false, id, v, 0};
    if (self->H->is_main_thread())
        self->apply_edit(e);
    else
        self->queue_edit(e);
    return kResultOk;
}

tresult PLUGIN_API Handler::restartComponent(int32 flags)
{
    Vst3Instance::Edit e{true, 0, 0, flags};
    if (self->H->is_main_thread())
        self->apply_edit(e);
    else
        self->queue_edit(e);
    return kResultOk;
}

bool has_sub(const VST3::Hosting::ClassInfo &c, const char *s)
{
    for (auto &x : c.subCategories())
        if (!strcasecmp(x.c_str(), s))
            return true;
    return false;
}

} // namespace

std::unique_ptr<PluginInstance> load_vst3(Host *h, const std::string &path, const std::string &plugin_id, bool list_only)
{
    auto I = std::make_unique<Vst3Instance>(h);
    I->path = path;
    std::string err;
    I->module = VST3::Hosting::Module::create(path, err);
    if (!I->module)
        die("%s: %s", path.c_str(), err.c_str());
    auto factory = I->module->getFactory();
    std::vector<VST3::Hosting::ClassInfo> audio;
    for (auto &c : factory.classInfos())
        if (c.category() == kVstAudioEffectClass)
            audio.push_back(c);
    if (list_only) {
        fprintf(out, "[\n");
        for (size_t i = 0; i < audio.size(); i++) {
            auto &c = audio[i];
            fprintf(out, "  {\"format\": \"vst3\", \"id\": \"%s\", \"name\": %s, \"vendor\": %s, \"features\": [", hex_id(c.ID()).c_str(),
                   json_str(c.name()).c_str(), json_str(c.vendor()).c_str());
            size_t k = 0;
            for (auto &s : c.subCategories())
                fprintf(out, "%s%s", k++ ? ", " : "", json_str(s).c_str());
            fprintf(out, "]}%s\n", i + 1 < audio.size() ? "," : "");
        }
        fprintf(out, "]\n");
        fflush(out);
        exit(0);
    }
    const VST3::Hosting::ClassInfo *chosen = nullptr;
    for (auto &c : audio) {
        bool match = plugin_id.empty() ? has_sub(c, "Instrument")
                                       : !strcasecmp(hex_id(c.ID()).c_str(), plugin_id.c_str()) || c.name() == plugin_id;
        if (match) {
            chosen = &c;
            break;
        }
    }
    if (!chosen && plugin_id.empty() && !audio.empty())
        chosen = &audio[0];
    if (!chosen)
        die("no VST3 audio class '%s' in %s", plugin_id.c_str(), path.c_str());
    I->info = *chosen;
    I->cid = FUID::fromTUID(chosen->ID().data());
    I->id = hex_id(chosen->ID());
    I->name = chosen->name();
    I->vendor = chosen->vendor().empty() ? factory.info().vendor() : chosen->vendor();
    I->node_suffix = slug(chosen->name());
    I->instrument = has_sub(*chosen, "Instrument");

    I->host_app = owned(new HostApplication());
    PluginContextFactory::instance().setPluginContext(I->host_app);
    I->provider = owned(new PlugProvider(factory, *chosen, true));
    if (!I->provider->initialize())
        die("%s: could not create %s (component/controller init failed)", path.c_str(), chosen->name().c_str());
    I->component = I->provider->getComponentPtr();
    I->controller = I->provider->getControllerPtr();
    if (!I->component || !I->controller)
        die("%s: %s has no %s", path.c_str(), chosen->name().c_str(), I->component ? "edit controller" : "component");
    I->processor = FUnknownPtr<IAudioProcessor>(I->component);
    if (!I->processor)
        die("%s: %s is not an audio processor", path.c_str(), chosen->name().c_str());
    I->units = FUnknownPtr<IUnitInfo>(I->controller);
    I->controller->setComponentHandler(&I->handler);
    // Bring the controller in line with the processor's state, as hosts do
    // after creating a plugin (otherwise a controller may report values the
    // processor doesn't have).
    {
        MemoryStream ms;
        if (I->component->getState(&ms) == kResultOk) {
            int64 pos = 0;
            ms.seek(0, IBStream::kIBSeekSet, &pos);
            I->controller->setComponentState(&ms);
        }
    }
    I->setup_buses();
    I->setup_midi_map();
    I->midi_input = I->event_bus;
    I->midi_dialect = "vst3 events";
    I->rescan_params();
    return I;
}

} // namespace bl
