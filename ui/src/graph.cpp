#include "graph.h"

#include <QMetaObject>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <ctime>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

static Graph *s_instance = nullptr;

static quint64 monotonicNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return quint64(ts.tv_sec) * 1000000000ull + quint64(ts.tv_nsec);
}

// One tap: a capture stream and what its data thread hands the UI.
struct Tap {
    pw_stream *stream = nullptr;
    spa_hook listener{};
    Graph *graph = nullptr;
    QByteArray target;
    bool manual = false;      // links itself (an instrument's output stream)
    QList<pw_proxy *> links;
    std::atomic<float> peak[2];
    std::atomic<quint64> onset{0};
    // Data thread only: whether the last buffer was silent, for onset.
    bool quiet = true;
    Tap() { peak[0] = 0.f; peak[1] = 0.f; }
};

// Signal above -40 dBFS after a buffer below -60 dBFS counts as an onset;
// that is what the meter-latency proof times.
static constexpr float kOnsetOn = 0.01f;
static constexpr float kOnsetOff = 0.001f;

static void onProcess(void *data) {
    Tap *tap = static_cast<Tap *>(data);
    pw_buffer *b = pw_stream_dequeue_buffer(tap->stream);
    if (!b)
        return;
    spa_buffer *buf = b->buffer;
    if (buf->datas[0].data && buf->datas[0].chunk) {
        const float *samples = static_cast<const float *>(buf->datas[0].data) + buf->datas[0].chunk->offset / sizeof(float);
        const uint32_t n = buf->datas[0].chunk->size / sizeof(float);
        float m0 = 0.f, m1 = 0.f;
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            const float a = std::fabs(samples[i]), c = std::fabs(samples[i + 1]);
            m0 = a > m0 ? a : m0;
            m1 = c > m1 ? c : m1;
        }
        // Keep the largest since the UI last read; a lost race only loses a
        // peak to a later, equal-or-larger store.
        if (m0 > tap->peak[0].load(std::memory_order_relaxed)) tap->peak[0].store(m0, std::memory_order_relaxed);
        if (m1 > tap->peak[1].load(std::memory_order_relaxed)) tap->peak[1].store(m1, std::memory_order_relaxed);
        const float m = m0 > m1 ? m0 : m1;
        if (tap->quiet && m > kOnsetOn) {
            tap->quiet = false;
            quint64 expected = 0;
            tap->onset.compare_exchange_strong(expected, monotonicNs());
        } else if (!tap->quiet && m < kOnsetOff) {
            tap->quiet = true;
        }
    }
    pw_stream_queue_buffer(tap->stream, b);
}

struct TapCallbacks {
    static void stateChanged(void *data, pw_stream_state, pw_stream_state state, const char *) {
        Tap *tap = static_cast<Tap *>(data);
        if (tap->manual && state >= PW_STREAM_STATE_PAUSED)
            tap->graph->linkPending();
    }
};

static const pw_stream_events kTapEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = onProcess;
    e.state_changed = TapCallbacks::stateChanged;
    return e;
}();

struct GraphCallbacks {
    static void global(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props) {
        Graph *graph = static_cast<Graph *>(data);
        if (props && strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            const char *node = spa_dict_lookup(props, PW_KEY_NODE_ID);
            const char *dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
            const char *name = spa_dict_lookup(props, PW_KEY_PORT_NAME);
            const char *dsp = spa_dict_lookup(props, PW_KEY_FORMAT_DSP);
            const char *monitor = spa_dict_lookup(props, PW_KEY_PORT_MONITOR);
            if (node && dir && name) {
                graph->m_ports.insert(id, Graph::Port{id, quint32(atoi(node)), strcmp(dir, "out") == 0,
                                                      !dsp || strstr(dsp, "audio") != nullptr,
                                                      monitor && strcmp(monitor, "true") == 0, QByteArray(name)});
                graph->linkPending();
            }
            return;
        }
        if (!props || strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
            return;
        if (const char *n = spa_dict_lookup(props, PW_KEY_NODE_NAME))
            graph->m_loopNodeNames.insert(id, QByteArray(n));
        const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (!cls || !name)
            return;
        const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        if (!desc) desc = spa_dict_lookup(props, PW_KEY_NODE_NICK);
        if (!desc) desc = name;
        Graph *g = static_cast<Graph *>(data);
        const QString qname = QString::fromUtf8(name), qdesc = QString::fromUtf8(desc), qcls = QString::fromUtf8(cls);
        QMetaObject::invokeMethod(g, [g, id, qname, qdesc, qcls] { g->onGlobal(id, qname, qdesc, qcls); }, Qt::QueuedConnection);
    }
    static void globalRemove(void *data, uint32_t id) {
        Graph *g = static_cast<Graph *>(data);
        g->m_ports.remove(id);
        g->m_loopNodeNames.remove(id);
        QMetaObject::invokeMethod(g, [g, id] { g->onGlobalRemoved(id); }, Qt::QueuedConnection);
    }
};

static const pw_registry_events kRegistryEvents = [] {
    pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = GraphCallbacks::global;
    e.global_remove = GraphCallbacks::globalRemove;
    return e;
}();

Graph::Graph(QObject *parent) : QObject(parent) {
    s_instance = this;
    m_filter = qEnvironmentVariable("BACKLINE_NODE_FILTER");
    pw_init(nullptr, nullptr);
    m_loop = pw_thread_loop_new("backline-mixer", nullptr);
    m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
    pw_thread_loop_lock(m_loop);
    pw_thread_loop_start(m_loop);
    m_core = pw_context_connect(m_context,
                                pw_properties_new(PW_KEY_APP_NAME, "backline-mixer", nullptr), 0);
    if (m_core) {
        m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
        auto *hook = new spa_hook{};
        m_registryHook = hook;
        pw_registry_add_listener(m_registry, hook, &kRegistryEvents, this);
        m_connected = true;
    }
    pw_thread_loop_unlock(m_loop);
}

Graph::~Graph() {
    pw_thread_loop_lock(m_loop);
    for (Tap *tap : std::as_const(m_taps)) {
        for (pw_proxy *link : std::as_const(tap->links)) pw_proxy_destroy(link);
        pw_stream_destroy(tap->stream);
        delete tap;
    }
    m_taps.clear();
    if (m_registry) {
        spa_hook_remove(static_cast<spa_hook *>(m_registryHook));
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(m_registry));
    }
    delete static_cast<spa_hook *>(m_registryHook);
    if (m_core) pw_core_disconnect(m_core);
    pw_thread_loop_unlock(m_loop);
    pw_thread_loop_stop(m_loop);
    pw_context_destroy(m_context);
    pw_thread_loop_destroy(m_loop);
    if (s_instance == this) s_instance = nullptr;
}

Graph *Graph::instance() { return s_instance; }

QString Graph::kindOf(const QString &name, const QString &cls) {
    if (name.startsWith("backline.recorder") || name.startsWith("backline.soundcheck") || name == "backline.clock" ||
        name.startsWith("backline-mixer"))
        return {};
    if (name.startsWith("backline.") && cls.startsWith("Stream/Output/Audio"))
        return "instrument";
    if (cls == "Audio/Source" || cls == "Audio/Source/Virtual" || cls == "Audio/Duplex")
        return "input";
    if (cls == "Audio/Sink")
        return "output";
    return {};
}

void Graph::onGlobal(quint32 id, const QString &name, const QString &label, const QString &mediaClass) {
    const QString kind = kindOf(name, mediaClass);
    if (kind.isEmpty())
        return;
    if (!m_filter.isEmpty() && !QRegularExpression(m_filter).match(name).hasMatch())
        return;
    m_all.insert(id, Node{id, name, label, mediaClass, kind});
    publish();
}

void Graph::onGlobalRemoved(quint32 id) {
    if (m_all.remove(id))
        publish();
}

// Strips in order: inputs, instruments, outputs, each by label.
void Graph::publish() {
    QList<Node> list = m_all.values();
    static const QHash<QString, int> order{{"input", 0}, {"instrument", 1}, {"output", 2}};
    std::sort(list.begin(), list.end(), [](const Node &a, const Node &b) {
        if (order.value(a.kind) != order.value(b.kind)) return order.value(a.kind) < order.value(b.kind);
        return a.label.compare(b.label, Qt::CaseInsensitive) < 0;
    });
    QVariantList out;
    for (const Node &n : list)
        out.append(QVariantMap{{"id", n.id}, {"name", n.name}, {"label", n.label}, {"mediaClass", n.mediaClass}, {"kind", n.kind}});
    m_nodes = out;
    emit nodesChanged();
}

void Graph::setMetered(const QStringList &names) {
    if (!m_core)
        return;
    pw_thread_loop_lock(m_loop);
    for (auto it = m_taps.begin(); it != m_taps.end();) {
        if (!names.contains(it.key())) {
            for (pw_proxy *link : std::as_const(it.value()->links)) pw_proxy_destroy(link);
            pw_stream_destroy(it.value()->stream);
            delete it.value();
            it = m_taps.erase(it);
        } else {
            ++it;
        }
    }
    for (const QString &name : names) {
        if (m_taps.contains(name))
            continue;
        bool sink = false, stream = false;
        for (const Node &n : std::as_const(m_all))
            if (n.name == name) {
                sink = n.mediaClass == "Audio/Sink";
                stream = n.mediaClass.startsWith("Stream/");
            }
        Tap *tap = new Tap;
        tap->graph = this;
        tap->target = name.toUtf8();
        tap->manual = stream;
        const QByteArray target = name.toUtf8();
        pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Monitor", PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_NODE_NAME, "backline-mixer.meter", PW_KEY_NODE_DESCRIPTION, "Backline mixer meter",
            PW_KEY_TARGET_OBJECT, target.constData(), PW_KEY_STREAM_MONITOR, "true",
            PW_KEY_NODE_PASSIVE, "true", PW_KEY_NODE_DONT_RECONNECT, "true", "node.dont-fallback", "true",
            "node.dont-move", "true", nullptr);
        if (sink)
            pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        if (stream) {
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, nullptr);
            pw_properties_set(props, PW_KEY_NODE_AUTOCONNECT, "false");
        }
        tap->stream = pw_stream_new(m_core, "backline-mixer meter", props);
        pw_stream_add_listener(tap->stream, &tap->listener, &kTapEvents, tap);
        uint8_t buffer[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        pw_stream_connect(tap->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          pw_stream_flags((stream ? 0 : PW_STREAM_FLAG_AUTOCONNECT) | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                          params, 1);
        m_taps.insert(name, tap);
    }
    pw_thread_loop_unlock(m_loop);
}

bool Graph::takePeaks(const QString &name, float out[2], quint64 *onsetNs) {
    Tap *tap = m_taps.value(name, nullptr);
    if (!tap)
        return false;
    out[0] = tap->peak[0].exchange(0.f, std::memory_order_relaxed);
    out[1] = tap->peak[1].exchange(0.f, std::memory_order_relaxed);
    if (onsetNs)
        *onsetNs = tap->onset.exchange(0, std::memory_order_relaxed);
    return true;
}

// On the PipeWire thread: link each self-linking tap that isn't yet, once its
// own input ports and its target's audio outputs exist, channel by channel in
// port order. The links don't linger, so they go with the tap or the mixer.
void Graph::linkPending() {
    for (Tap *tap : std::as_const(m_taps)) {
        if (!tap->manual || !tap->links.isEmpty())
            continue;
        const quint32 tapNode = pw_stream_get_node_id(tap->stream);
        if (tapNode == SPA_ID_INVALID)
            continue;
        quint32 targetNode = SPA_ID_INVALID;
        for (auto it = m_loopNodeNames.begin(); it != m_loopNodeNames.end(); ++it)
            if (it.value() == tap->target) targetNode = it.key();
        if (targetNode == SPA_ID_INVALID)
            continue;
        QList<Port> outs, ins;
        for (const Port &p : std::as_const(m_ports)) {
            if (p.node == targetNode && p.output && p.audio && !p.monitor) outs.append(p);
            if (p.node == tapNode && !p.output) ins.append(p);
        }
        if (qEnvironmentVariableIsSet("BACKLINE_MIXER_DEBUG"))
            fprintf(stderr, "tap %s: node %u target %u, %lld outputs, %lld inputs\n", tap->target.constData(), tapNode,
                    targetNode, (long long)outs.size(), (long long)ins.size());
        if (outs.isEmpty() || ins.size() < 2)
            continue;
        auto byName = [](const Port &a, const Port &b) { return a.name < b.name; };
        // The worker's main output is out_FL/out_FR; aux groups are outN_*.
        std::sort(outs.begin(), outs.end(), [](const Port &a, const Port &b) {
            const bool ma = a.name.startsWith("out_"), mb = b.name.startsWith("out_");
            return ma != mb ? ma : a.name < b.name;
        });
        std::sort(ins.begin(), ins.end(), byName);
        // input_FL, input_FR sort in channel order; a mono source feeds both.
        for (int c = 0; c < 2; c++) {
            const Port &out = outs[std::min<int>(c, outs.size() - 1)];
            pw_properties *props = pw_properties_new(nullptr, nullptr);
            pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", targetNode);
            pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", out.id);
            pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", tapNode);
            pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", ins[c].id);
            pw_properties_set(props, PW_KEY_OBJECT_LINGER, "false");
            auto *link = static_cast<pw_proxy *>(pw_core_create_object(
                m_core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
            pw_properties_free(props);
            if (link) tap->links.append(link);
        }
    }
}
