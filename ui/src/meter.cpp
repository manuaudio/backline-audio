#include "meter.h"

#include "graph.h"

#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <ctime>

static MeterBank *s_bank = nullptr;

static qint64 monotonicNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000000000ll + qint64(ts.tv_nsec);
}

double meterPosition(double db) {
    if (db >= 0) return 1.0;
    if (db >= -20) return 0.5 + (db + 20) / 40.0;
    if (db >= -40) return 0.25 + (db + 40) / 80.0;
    if (db >= -60) return (db + 60) / 80.0;
    return 0.0;
}

// ------------------------------------------------------------------- bank

MeterBank::MeterBank(Graph *graph, QObject *parent) : QObject(parent), m_graph(graph) {
    s_bank = this;
    m_timer.setInterval(1000 / 30);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &MeterBank::tick);
    m_clock.start();
    // BACKLINE_MIXER_TRACE=FILE logs, per signal onset on any tap, when the
    // tap's data thread saw it and when the frame showing it was presented.
    const QString trace = qEnvironmentVariable("BACKLINE_MIXER_TRACE");
    if (!trace.isEmpty()) {
        m_trace.setFileName(trace);
        m_trace.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }
}

MeterBank *MeterBank::instance() { return s_bank; }

void MeterBank::setActive(bool active) {
    if (m_active == active)
        return;
    m_active = active;
    if (active) {
        m_lastTickNs = 0;
        m_timer.start();
    } else {
        m_timer.stop();
    }
    syncTaps();
    emit activeChanged();
}

void MeterBank::add(MeterItem *item) {
    m_items.append(item);
    syncTaps();
}

void MeterBank::remove(MeterItem *item) {
    m_items.removeAll(item);
    syncTaps();
}

// Taps exist only for the meters there are, and only while the bank runs.
void MeterBank::syncTaps() {
    if (!m_graph)
        return;
    QStringList names;
    if (m_active)
        for (const auto &item : std::as_const(m_items))
            if (item && !item->node().isEmpty() && !names.contains(item->node()))
                names.append(item->node());
    m_graph->setMetered(names);
}

void MeterBank::tick() {
    const qint64 start = monotonicNs();
    const double dt = m_lastTickNs ? (start - m_lastTickNs) / 1e9 : 1.0 / 30;
    m_lastTickNs = start;
    QHash<QString, std::pair<bool, std::array<float, 2>>> read;
    for (const auto &item : std::as_const(m_items)) {
        if (!item || item->node().isEmpty())
            continue;
        auto it = read.find(item->node());
        if (it == read.end()) {
            float peaks[2] = {0, 0};
            quint64 onset = 0;
            const bool ok = m_graph && m_graph->takePeaks(item->node(), peaks, &onset);
            it = read.insert(item->node(), {ok, {peaks[0], peaks[1]}});
            if (onset && m_trace.isOpen() && item->window()) {
                const QString node = item->node();
                const qint64 tickNs = start;
                QQuickWindow *w = item->window();
                auto conn = std::make_shared<QMetaObject::Connection>();
                *conn = connect(w, &QQuickWindow::frameSwapped, this, [this, conn, node, onset, tickNs] {
                    disconnect(*conn);
                    m_trace.write(QStringLiteral("{\"node\":\"%1\",\"tap_ns\":%2,\"tick_ns\":%3,\"frame_ns\":%4}\n")
                                      .arg(node).arg(onset).arg(tickNs).arg(monotonicNs()).toUtf8());
                    m_trace.flush();
                }, Qt::DirectConnection);
            }
        }
        if (!it->first)
            continue;
        const float peaks[2] = {it->second[0], it->second[1]};
        if (item->push(peaks, dt))
            item->update();
    }
    const qint64 end = monotonicNs();
    m_busyNs += end - start;
    if (!m_windowStartNs) m_windowStartNs = start;
    if (end - m_windowStartNs >= 1000000000ll) {
        m_busyPercent = 100.0 * double(m_busyNs) / double(end - m_windowStartNs);
        if (m_trace.isOpen()) {
            m_trace.write(QStringLiteral("{\"tick_busy_percent\":%1}\n").arg(m_busyPercent, 0, 'f', 4).toUtf8());
            m_trace.flush();
        }
        m_busyNs = 0;
        m_windowStartNs = end;
        emit statsChanged();
    }
}

// ------------------------------------------------------------------- item

MeterItem::MeterItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    connect(this, &MeterItem::styleChanged, this, &QQuickItem::update);
}

MeterItem::~MeterItem() {
    if (s_bank)
        s_bank->remove(this);
}

void MeterItem::setNode(const QString &node) {
    if (node == m_node)
        return;
    if (s_bank && !m_node.isEmpty())
        s_bank->remove(this);
    m_node = node;
    for (int c = 0; c < 2; c++) {
        m_level[c] = m_hold[c] = kFloorDb;
        m_clip[c] = false;
    }
    if (s_bank && !m_node.isEmpty())
        s_bank->add(this);
    emit nodeChanged();
    update();
}

void MeterItem::setChannels(int n) {
    n = std::clamp(n, 1, 2);
    if (n == m_channels)
        return;
    m_channels = n;
    emit styleChanged();
}

bool MeterItem::push(const float peaks[2], double dt) {
    bool changed = false;
    bool readout = false;
    for (int c = 0; c < 2; c++) {
        const double db = peaks[c] > 1e-6f ? 20.0 * std::log10(double(peaks[c])) : kFloorDb;
        const double released = m_level[c] - kReleaseDbPerS * dt;
        const double level = std::max({db, released, kFloorDb});
        if (std::fabs(level - m_level[c]) > 0.05) changed = true;
        m_level[c] = level;
        if (db >= m_hold[c]) {
            m_hold[c] = db;
            m_holdAge[c] = 0;
        } else {
            m_holdAge[c] += dt;
            if (m_holdAge[c] > kHoldS) {
                const double fallen = std::max(m_hold[c] - kReleaseDbPerS * dt, m_level[c]);
                if (std::fabs(fallen - m_hold[c]) > 0.05) changed = true;
                m_hold[c] = fallen;
            }
        }
        if (db >= kClipDb && !m_clip[c]) {
            m_clip[c] = true;
            changed = readout = true;
        }
    }
    const bool live = std::max(m_level[0], m_level[1]) > kFloorDb + 0.5;
    if (live != m_live) {
        m_live = live;
        readout = true;
    }
    // The numeric readout moves in 0.5 dB steps, so text relayout stays rare.
    if (std::fabs(holdDb() - m_shownHold) >= 0.5) {
        m_shownHold = holdDb();
        readout = true;
    }
    if (readout)
        emit readoutChanged();
    return changed || readout;
}

void MeterItem::resetClip() {
    m_clip[0] = m_clip[1] = false;
    emit readoutChanged();
    update();
}

namespace {
struct Builder {
    QSGGeometry::ColoredPoint2D *v;
    int n = 0;
    void rect(float x0, float y0, float x1, float y1, const QColor &c) {
        if (x1 <= x0 || y1 <= y0) return;
        const uchar r = c.red(), g = c.green(), b = c.blue(), a = c.alpha();
        const float pts[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
        for (auto &p : pts) v[n++].set(p[0], p[1], r, g, b, a);
    }
};
}

// Per channel: the track, the fill in up to three zones (ok, warn, hot), the
// peak-hold line and the clip lamp at the top. 36 vertices a channel.
QSGNode *MeterItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    auto *node = static_cast<QSGGeometryNode *>(old);
    constexpr int kMaxVertices = 36 * 2;
    if (!node) {
        node = new QSGGeometryNode;
        auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), kMaxVertices);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    QSGGeometry *geometry = node->geometry();
    Builder b{geometry->vertexDataAsColoredPoint2D()};

    const float w = float(width()), h = float(height());
    const float gap = m_channels > 1 ? std::max(1.0f, w * 0.08f) : 0.f;
    const float bw = (w - gap * (m_channels - 1)) / m_channels;
    const float lamp = std::min(5.0f, h * 0.04f);
    const float top = lamp + 2.0f, span = h - top;
    auto y = [&](double db) { return top + span * float(1.0 - meterPosition(db)); };

    for (int c = 0; c < m_channels; c++) {
        const float x0 = c * (bw + gap), x1 = x0 + bw;
        b.rect(x0, top, x1, h, m_track);
        const double level = m_level[c];
        if (level > kFloorDb) {
            const float yl = y(level), yw = y(kWarnDb), yh = y(kHotDb);
            b.rect(x0, std::max(yl, yw), x1, h, m_ok);
            if (level > kWarnDb) b.rect(x0, std::max(yl, yh), x1, yw, m_warn);
            if (level > kHotDb) b.rect(x0, yl, x1, yh, m_hot);
        }
        if (m_hold[c] > kFloorDb + 0.5) {
            const float yh = y(m_hold[c]);
            b.rect(x0, yh, x1, std::min(h, yh + 2.0f), m_hold[c] > kHotDb ? m_hot : m_holdColor);
        }
        b.rect(x0, 0, x1, lamp, m_clip[c] ? m_hot : m_track);
    }
    // Unused vertices collapse to nothing.
    auto *v = geometry->vertexDataAsColoredPoint2D();
    for (int i = b.n; i < kMaxVertices; i++) v[i].set(0, 0, 0, 0, 0, 0);
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
