#pragma once

// The PipeWire side of the mixer: the audio nodes worth a channel strip
// (inputs, Backline instruments, outputs), and one peak tap per metered node.
//
// A tap is a passive capture stream on the node (a sink's monitor for an
// output) that keeps, per channel, the largest absolute sample since the UI
// last looked. The data thread does one pass over each buffer and two atomic
// stores; nothing else runs there. Taps are created only for the strips on
// screen while the window is visible, are `node.passive` (they never keep a
// device awake) and `node.dont-fallback` (a tap whose node is gone records
// nothing rather than the default device).

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <atomic>
#include <memory>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;

struct Tap;

class Graph : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList nodes READ nodes NOTIFY nodesChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QString filter READ filter CONSTANT)

public:
    explicit Graph(QObject *parent = nullptr);
    ~Graph() override;

    static Graph *instance();

    QVariantList nodes() const { return m_nodes; }
    bool connected() const { return m_connected; }
    QString filter() const { return m_filter; }

    // The set of node names to meter; taps for others are removed.
    Q_INVOKABLE void setMetered(const QStringList &names);

    // Peaks since the last call, linear, per channel (max 2), and the
    // CLOCK_MONOTONIC time the tap first saw signal after silence (0 if none
    // since the last call). Called from the UI thread by the meter bank.
    bool takePeaks(const QString &name, float out[2], quint64 *onsetNs);

    // Classify a node by its media class and name, for the strips and tests:
    // "input", "instrument", "output", or "" for no strip.
    static QString kindOf(const QString &name, const QString &mediaClass);

signals:
    void nodesChanged();
    void connectedChanged();

private:
    friend struct GraphCallbacks;
    void onGlobal(quint32 id, const QString &name, const QString &label, const QString &mediaClass);
    void onGlobalRemoved(quint32 id);
    void publish();

    pw_thread_loop *m_loop = nullptr;
    pw_context *m_context = nullptr;
    pw_core *m_core = nullptr;
    pw_registry *m_registry = nullptr;
    void *m_registryHook = nullptr;
    bool m_connected = false;
    QString m_filter;

    // Ports and node names as the PipeWire thread sees them, for taps that
    // link themselves (an instrument is an output stream, which WirePlumber
    // won't take as a capture target). Touched only on the PipeWire thread.
    struct Port { quint32 id, node; bool output, audio, monitor; QByteArray name; };
    QHash<quint32, Port> m_ports;
    QHash<quint32, QByteArray> m_loopNodeNames;
    void linkPending();
    friend struct TapCallbacks;

    struct Node { quint32 id; QString name, label, mediaClass, kind; };
    QHash<quint32, Node> m_all;
    QVariantList m_nodes;
    QHash<QString, Tap *> m_taps;
};
