#pragma once

// Meters. One MeterBank timer (30 Hz, only while the window is visible) reads
// every tap's peaks and hands them to its MeterItem, which keeps the
// ballistics (instant attack, 24 dB/s release, 1.5 s peak hold, a clip latch)
// and draws itself as one scene-graph geometry node with per-vertex colour:
// one draw call per strip, no QML bindings per frame, and nothing at all when
// a level hasn't moved.

#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>

class Graph;
class MeterItem;

// dB to the meter's 0..1 position: 0 dBFS at the top, -20 at half, -40 at a
// quarter, -60 at the bottom, so the working range gets most of the height.
double meterPosition(double db);

class MeterBank : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(int rate READ rate CONSTANT)
    Q_PROPERTY(double busyPercent READ busyPercent NOTIFY statsChanged)

public:
    explicit MeterBank(Graph *graph, QObject *parent = nullptr);
    static MeterBank *instance();

    bool active() const { return m_active; }
    void setActive(bool active);
    int rate() const { return 30; }
    // Share of wall time spent in the tick itself over the last second.
    double busyPercent() const { return m_busyPercent; }

    // The meter scale, for the tick labels QML draws beside a meter.
    Q_INVOKABLE double position(double db) const { return meterPosition(db); }

    void add(MeterItem *item);
    void remove(MeterItem *item);
    // Feed one item directly (tests, and the tick itself).
    void tick();

signals:
    void activeChanged();
    void statsChanged();

private:
    void syncTaps();
    Graph *m_graph;
    QList<QPointer<MeterItem>> m_items;
    QTimer m_timer;
    bool m_active = false;
    QElapsedTimer m_clock;
    qint64 m_lastTickNs = 0;
    qint64 m_busyNs = 0;
    qint64 m_windowStartNs = 0;
    double m_busyPercent = 0;
    QFile m_trace;
};

class MeterItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString node READ node WRITE setNode NOTIFY nodeChanged)
    Q_PROPERTY(int channels READ channels WRITE setChannels NOTIFY styleChanged)
    Q_PROPERTY(QColor okColor MEMBER m_ok NOTIFY styleChanged)
    Q_PROPERTY(QColor warnColor MEMBER m_warn NOTIFY styleChanged)
    Q_PROPERTY(QColor hotColor MEMBER m_hot NOTIFY styleChanged)
    Q_PROPERTY(QColor trackColor MEMBER m_track NOTIFY styleChanged)
    Q_PROPERTY(QColor holdColor MEMBER m_holdColor NOTIFY styleChanged)
    Q_PROPERTY(double holdDb READ holdDb NOTIFY readoutChanged)
    Q_PROPERTY(bool clipped READ clipped NOTIFY readoutChanged)
    Q_PROPERTY(bool live READ live NOTIFY readoutChanged)

public:
    explicit MeterItem(QQuickItem *parent = nullptr);
    ~MeterItem() override;

    QString node() const { return m_node; }
    void setNode(const QString &node);
    int channels() const { return m_channels; }
    void setChannels(int n);
    double holdDb() const { return std::max(m_hold[0], m_hold[1]); }
    bool clipped() const { return m_clip[0] || m_clip[1]; }
    bool live() const { return m_live; }

    // One reading per channel (linear peak), dt seconds after the last.
    // Returns whether anything visible changed.
    bool push(const float peaks[2], double dt);
    Q_INVOKABLE void resetClip();
    double levelDb(int channel) const { return m_level[channel]; }

    static constexpr double kFloorDb = -60.0;
    static constexpr double kReleaseDbPerS = 24.0;
    static constexpr double kHoldS = 1.5;
    static constexpr double kWarnDb = -12.0;
    static constexpr double kHotDb = -3.0;
    static constexpr double kClipDb = -0.1;

signals:
    void nodeChanged();
    void styleChanged();
    void readoutChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

private:
    QString m_node;
    int m_channels = 2;
    QColor m_ok{"#9ece6a"}, m_warn{"#e0af68"}, m_hot{"#f7768e"}, m_track{"#24283b"}, m_holdColor{"#c0caf5"};
    double m_level[2] = {kFloorDb, kFloorDb};
    double m_hold[2] = {kFloorDb, kFloorDb};
    double m_holdAge[2] = {0, 0};
    bool m_clip[2] = {false, false};
    bool m_live = false;
    double m_shownHold = kFloorDb;
};
