#pragma once

// Backline's services as the mixer sees them, over the session bus: the
// host's instruments (Host1, Instance1), the recorder (Recorder1) and the
// clock (Clock1), per docs/api. Every call is asynchronous, so the window
// never waits on a service (Arm can take seconds while links come up).
// Signals and PropertiesChanged keep the state current; nothing polls,
// except each instrument's MIDI counter (Instance1.MidiEvents has no change
// signal), read at 5 Hz while the window is visible for the activity lamp.
//
// The armed set is shared with the Omarchy panels and commands
// ($XDG_STATE_HOME/backline/omarchy/record-sources.json), so arming a strip
// here arms it there.

#include <QDBusServiceWatcher>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class QDBusPendingCallWatcher;
class QDBusMessage;

class Services : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hostRunning READ hostRunning NOTIFY servicesChanged)
    Q_PROPERTY(bool recorderRunning READ recorderRunning NOTIFY servicesChanged)
    Q_PROPERTY(bool clockRunning READ clockRunning NOTIFY servicesChanged)
    Q_PROPERTY(QVariantList instances READ instances NOTIFY instancesChanged)
    Q_PROPERTY(QVariantMap recorder READ recorder NOTIFY recorderChanged)
    Q_PROPERTY(QStringList armed READ armed NOTIFY armedChanged)
    Q_PROPERTY(QVariantList takes READ takes NOTIFY takesChanged)
    Q_PROPERTY(QVariantMap clock READ clock NOTIFY clockChanged)
    Q_PROPERTY(QVariantMap transport READ transport NOTIFY transportChanged)
    Q_PROPERTY(QVariantMap midiActivity READ midiActivity NOTIFY midiActivityChanged)
    Q_PROPERTY(QVariantMap volumes READ volumes NOTIFY volumesChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(double preRoll READ preRoll CONSTANT)

public:
    explicit Services(QObject *parent = nullptr, const QString &stateDir = {});

    bool hostRunning() const { return m_running.contains("Host"); }
    bool recorderRunning() const { return m_running.contains("Recorder"); }
    bool clockRunning() const { return m_running.contains("Clock"); }
    QVariantList instances() const { return m_instances; }
    QVariantMap recorder() const { return m_recorder; }
    QStringList armed() const { return m_armed; }
    QVariantList takes() const { return m_takes; }
    QVariantMap clock() const { return m_clock; }
    QVariantMap transport() const { return m_transport; }
    QVariantMap midiActivity() const { return m_midiActivity; }
    QVariantMap volumes() const { return m_volumes; }
    QString lastError() const { return m_lastError; }
    bool active() const { return m_active; }
    void setActive(bool active);
    double preRoll() const { return 5.0; }

    // Recorder.
    Q_INVOKABLE void toggleArm(const QString &node);
    Q_INVOKABLE void toggleRecord();
    Q_INVOKABLE void addMarker();
    Q_INVOKABLE void soundcheck(const QString &take);
    Q_INVOKABLE void stopSoundcheck();
    Q_INVOKABLE void setInput(const QString &node, const QString &mode);
    // Clock.
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void locate(const QString &position);
    Q_INVOKABLE void setTempo(double bpm);
    // Node volume (dB) through wpctl, for nodes that have one.
    Q_INVOKABLE void setVolumeDb(int nodeId, double db);
    Q_INVOKABLE void toggleMute(int nodeId);
    Q_INVOKABLE void readVolume(int nodeId);
    Q_INVOKABLE void refreshAll();

    // For tests: the armed-set file and its parsing.
    QString armedFile() const { return m_stateDir + "/record-sources.json"; }
    static QStringList parseArmed(const QByteArray &json);

signals:
    void servicesChanged();
    void instancesChanged();
    void recorderChanged();
    void armedChanged();
    void takesChanged();
    void clockChanged();
    void transportChanged();
    void midiActivityChanged();
    void volumesChanged();
    void lastErrorChanged();
    void activeChanged();

private slots:
    void onHostSignal(const QDBusMessage &message);
    void onRecorderSignal(const QDBusMessage &message);
    void onClockSignal(const QDBusMessage &message);
    void onPropertiesChanged(const QDBusMessage &message);

private:
    void call(const QString &service, const QString &method, const QVariantList &args,
              std::function<void(const QDBusMessage &)> done = {});
    void readHost();
    void readRecorder();
    void readTakes();
    void readClock();
    void pollMidi();
    void arm(const QStringList &sources);
    void writeArmed(const QStringList &sources);
    void readArmed();
    void fail(const QString &text);

    QString m_stateDir;
    QSet<QString> m_running;
    QDBusServiceWatcher m_watcher;
    QVariantList m_instances;
    QVariantMap m_recorder;
    QStringList m_armed;
    QVariantList m_takes;
    QVariantMap m_clock;
    QVariantMap m_transport;
    QVariantMap m_midiActivity;
    QHash<QString, qulonglong> m_midiCounts;
    QVariantMap m_volumes;
    QString m_lastError;
    bool m_active = false;
    QTimer m_midiTimer;
};
