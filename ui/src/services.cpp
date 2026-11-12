#include "services.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <cmath>

static const QString kBus = QStringLiteral("io.github.manuaudio.Backline.");
static const QString kPath = QStringLiteral("/io/github/manuaudio/Backline/");
static const QStringList kServices{"Host", "Recorder", "Clock"};

static QDBusConnection bus() { return QDBusConnection::sessionBus(); }

// D-Bus values arrive as QDBusArgument or QDBusVariant inside variants; turn
// them into plain QVariants QML can read.
static QVariant plain(const QVariant &v);

static QVariantMap plainMap(const QDBusArgument &arg) {
    QVariantMap out;
    arg.beginMap();
    while (!arg.atEnd()) {
        // The key is basic; the value is whatever the signature says (a
        // variant in a{sv}, a plain number in a{su}), which asVariant reads.
        arg.beginMapEntry();
        const QString key = arg.asVariant().toString();
        const QVariant value = arg.asVariant();
        arg.endMapEntry();
        out.insert(key, plain(value));
    }
    arg.endMap();
    return out;
}

static QVariant plain(const QVariant &v) {
    if (v.canConvert<QDBusVariant>() && v.userType() == qMetaTypeId<QDBusVariant>())
        return plain(v.value<QDBusVariant>().variant());
    if (v.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        if (arg.currentType() == QDBusArgument::MapType)
            return plainMap(arg);
        if (arg.currentType() == QDBusArgument::ArrayType) {
            QVariantList list;
            arg.beginArray();
            while (!arg.atEnd()) {
                list.append(arg.currentType() == QDBusArgument::MapType ? QVariant(plainMap(arg)) : plain(arg.asVariant()));
            }
            arg.endArray();
            return list;
        }
    }
    return v;
}

Services::Services(QObject *parent, const QString &stateDir)
    : QObject(parent),
      m_stateDir(!stateDir.isEmpty() ? stateDir
                 : (qEnvironmentVariableIsEmpty("XDG_STATE_HOME") ? QDir::homePath() + "/.local/state"
                                                                  : qEnvironmentVariable("XDG_STATE_HOME")) +
                       "/backline/omarchy") {
    m_watcher.setConnection(bus());
    m_watcher.setWatchMode(QDBusServiceWatcher::WatchForOwnerChange);
    for (const QString &s : kServices)
        m_watcher.addWatchedService(kBus + s);
    connect(&m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &name, const QString &, const QString &owner) {
                const QString s = name.mid(kBus.size());
                if (owner.isEmpty()) m_running.remove(s);
                else m_running.insert(s);
                emit servicesChanged();
                if (s == "Host") readHost();
                if (s == "Recorder") { readRecorder(); readTakes(); }
                if (s == "Clock") readClock();
            });

    for (const char *sig : {"Loaded", "Unloaded", "Crashed", "Restarted", "Failed"})
        bus().connect(kBus + "Host", kPath + "Host", kBus + "Host1", sig, this, SLOT(onHostSignal(QDBusMessage)));
    for (const char *sig : {"Level", "Overrun", "Marker", "TakeFinished", "SoundcheckInput"})
        bus().connect(kBus + "Recorder", kPath + "Recorder", kBus + "Recorder1", sig, this, SLOT(onRecorderSignal(QDBusMessage)));
    for (const char *sig : {"Transport", "Tempo", "LinkPeers", "Lock"})
        bus().connect(kBus + "Clock", kPath + "Clock", kBus + "Clock1", sig, this, SLOT(onClockSignal(QDBusMessage)));
    for (const QString &s : {QStringLiteral("Recorder"), QStringLiteral("Clock")})
        bus().connect(kBus + s, kPath + s, "org.freedesktop.DBus.Properties", "PropertiesChanged", this,
                      SLOT(onPropertiesChanged(QDBusMessage)));

    m_midiTimer.setInterval(200);
    connect(&m_midiTimer, &QTimer::timeout, this, &Services::pollMidi);

    // The armed set changes from the panels too; follow its file.
    readArmed();
    refreshAll();
}

void Services::refreshAll() {
    m_running.clear();
    auto *iface = bus().interface();
    for (const QString &s : kServices)
        if (iface && iface->isServiceRegistered(kBus + s))
            m_running.insert(s);
    emit servicesChanged();
    readHost();
    readRecorder();
    readTakes();
    readClock();
}

void Services::setActive(bool active) {
    if (m_active == active)
        return;
    m_active = active;
    if (active) {
        m_midiTimer.start();
        readArmed();
    } else {
        m_midiTimer.stop();
    }
    emit activeChanged();
}

void Services::fail(const QString &text) {
    m_lastError = text;
    emit lastErrorChanged();
}

void Services::call(const QString &service, const QString &method, const QVariantList &args,
                    std::function<void(const QDBusMessage &)> done) {
    QDBusMessage msg = QDBusMessage::createMethodCall(kBus + service, kPath + service, kBus + service + "1", method);
    msg.setArguments(args);
    auto *watcher = new QDBusPendingCallWatcher(bus().asyncCall(msg, 30000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, done, method] {
        const QDBusMessage reply = watcher->reply();
        watcher->deleteLater();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            fail(method + ": " + reply.errorMessage());
            return;
        }
        if (done)
            done(reply);
    });
}

// ------------------------------------------------------------------- host

void Services::readHost() {
    if (!hostRunning()) {
        m_instances.clear();
        emit instancesChanged();
        return;
    }
    call("Host", "List", {}, [this](const QDBusMessage &reply) {
        m_instances = plain(reply.arguments().value(0)).toList();
        emit instancesChanged();
    });
}

void Services::onHostSignal(const QDBusMessage &) { readHost(); }

// The MIDI lamp: an instrument whose MidiEvents counter moved since the last
// look lit up at that time; QML fades it.
void Services::pollMidi() {
    for (const QVariant &v : std::as_const(m_instances)) {
        const QVariantMap inst = v.toMap();
        const QString name = inst.value("dbus").toString(), node = inst.value("node").toString();
        if (name.isEmpty())
            continue;
        QDBusMessage msg = QDBusMessage::createMethodCall(name, kPath + "Instance", "org.freedesktop.DBus.Properties", "Get");
        msg.setArguments({kBus + "Instance1", QStringLiteral("MidiEvents")});
        auto *watcher = new QDBusPendingCallWatcher(bus().asyncCall(msg, 1000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, node] {
            const QDBusMessage reply = watcher->reply();
            watcher->deleteLater();
            if (reply.type() == QDBusMessage::ErrorMessage)
                return;
            const qulonglong count = plain(reply.arguments().value(0)).toULongLong();
            const auto it = m_midiCounts.find(node);
            if (it != m_midiCounts.end() && count > it.value()) {
                m_midiActivity.insert(node, QDateTime::currentMSecsSinceEpoch());
                emit midiActivityChanged();
            }
            m_midiCounts.insert(node, count);
        });
    }
}

// --------------------------------------------------------------- recorder

QStringList Services::parseArmed(const QByteArray &json) {
    QStringList out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array())
        if (v.isString()) out.append(v.toString());
    return out;
}

void Services::readArmed() {
    QFile file(armedFile());
    const QStringList armed = file.open(QIODevice::ReadOnly) ? parseArmed(file.readAll()) : QStringList();
    if (armed != m_armed) {
        m_armed = armed;
        emit armedChanged();
    }
}

void Services::writeArmed(const QStringList &sources) {
    QDir().mkpath(m_stateDir);
    QSaveFile file(armedFile());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonArray::fromStringList(sources)).toJson(QJsonDocument::Compact) + "\n");
        file.commit();
    }
    m_armed = sources;
    emit armedChanged();
}

void Services::readRecorder() {
    if (!recorderRunning()) {
        m_recorder.clear();
        emit recorderChanged();
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(kBus + "Recorder", kPath + "Recorder", "org.freedesktop.DBus.Properties", "GetAll");
    msg.setArguments({kBus + "Recorder1"});
    auto *watcher = new QDBusPendingCallWatcher(bus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        const QDBusMessage reply = watcher->reply();
        watcher->deleteLater();
        if (reply.type() == QDBusMessage::ErrorMessage)
            return;
        m_recorder = plain(reply.arguments().value(0)).toMap();
        emit recorderChanged();
    });
}

void Services::readTakes() {
    if (!recorderRunning())
        return;
    call("Recorder", "Takes", {}, [this](const QDBusMessage &reply) {
        const QJsonObject o = QJsonDocument::fromJson(reply.arguments().value(0).toString().toUtf8()).object();
        m_takes = o.value("takes").toArray().toVariantList();
        emit takesChanged();
    });
}

void Services::onRecorderSignal(const QDBusMessage &message) {
    if (message.member() == "Level") {
        // Frames recorded so far, for the elapsed time; the meters are ours.
        m_recorder.insert("Frames", message.arguments().value(0));
        emit recorderChanged();
        return;
    }
    if (message.member() == "TakeFinished" || message.member() == "Marker")
        readTakes();
    readRecorder();
}

void Services::onPropertiesChanged(const QDBusMessage &message) {
    const QVariantMap changed = plain(message.arguments().value(1)).toMap();
    const QString iface = message.arguments().value(0).toString();
    if (iface == kBus + "Recorder1") {
        for (auto it = changed.begin(); it != changed.end(); ++it) m_recorder.insert(it.key(), it.value());
        emit recorderChanged();
    } else if (iface == kBus + "Clock1") {
        for (auto it = changed.begin(); it != changed.end(); ++it) m_clock.insert(it.key(), it.value());
        emit clockChanged();
    }
}

void Services::arm(const QStringList &sources) {
    const QString state = m_recorder.value("State").toString();
    auto doArm = [this, sources] {
        if (sources.isEmpty()) {
            readRecorder();
            return;
        }
        call("Recorder", "Arm", {sources, QVariantMap{{"pre-roll", QVariant::fromValue(preRoll())}}},
             [this](const QDBusMessage &) { readRecorder(); });
    };
    if (state == "armed")
        call("Recorder", "Stop", {}, [doArm](const QDBusMessage &) { doArm(); });
    else
        doArm();
}

void Services::toggleArm(const QString &node) {
    if (m_recorder.value("State").toString() == "recording") {
        fail(tr("Stop the take first"));
        return;
    }
    readArmed();
    QStringList next = m_armed;
    if (next.contains(node)) next.removeAll(node);
    else next.append(node);
    writeArmed(next);
    arm(next);
}

void Services::toggleRecord() {
    const QString state = m_recorder.value("State").toString();
    if (state == "recording") {
        call("Recorder", "Stop", {}, [this](const QDBusMessage &) {
            readTakes();
            // Stand by again with the same set, so the next take has pre-roll.
            arm(m_armed);
        });
        return;
    }
    readArmed();
    if (m_armed.isEmpty()) {
        fail(tr("Arm a strip first (a)"));
        return;
    }
    auto record = [this] { call("Recorder", "Record", {}, [this](const QDBusMessage &) { readRecorder(); }); };
    if (state == "armed") {
        record();
    } else {
        call("Recorder", "Arm", {m_armed, QVariantMap{{"pre-roll", QVariant::fromValue(preRoll())}}},
             [record](const QDBusMessage &) { record(); });
    }
}

void Services::addMarker() {
    if (m_recorder.value("State").toString() == "recording")
        call("Recorder", "AddMarker", {QString()});
}

void Services::soundcheck(const QString &take) {
    call("Recorder", "Soundcheck", {take, QVariantMap{}}, [this](const QDBusMessage &) { readRecorder(); });
}

void Services::stopSoundcheck() {
    call("Recorder", "StopSoundcheck", {}, [this](const QDBusMessage &) { readRecorder(); });
}

void Services::setInput(const QString &node, const QString &mode) {
    call("Recorder", "SetInput", {node, mode}, [this](const QDBusMessage &) { readRecorder(); });
}

// ------------------------------------------------------------------ clock

void Services::readClock() {
    if (!clockRunning()) {
        m_clock.clear();
        emit clockChanged();
        return;
    }
    call("Clock", "Get", {}, [this](const QDBusMessage &reply) {
        m_clock = plain(reply.arguments().value(0)).toMap();
        m_transport = QVariantMap{{"rolling", m_clock.value("Rolling")}, {"frame", m_clock.value("Frame")},
                                  {"bbt", m_clock.value("Bbt")}, {"atMs", QDateTime::currentMSecsSinceEpoch()}};
        emit clockChanged();
        emit transportChanged();
    });
}

void Services::onClockSignal(const QDBusMessage &message) {
    const QVariantList a = message.arguments();
    if (message.member() == "Transport" && a.size() >= 4) {
        const bool wasRolling = m_transport.value("rolling").toBool();
        m_transport = QVariantMap{{"rolling", a[0]}, {"frame", a[1]}, {"timecode", a[2]}, {"bbt", a[3]},
                                  {"atMs", QDateTime::currentMSecsSinceEpoch()}};
        emit transportChanged();
        if (wasRolling != a[0].toBool()) {
            m_clock.insert("Rolling", a[0]);
            emit clockChanged();
        }
        return;
    }
    readClock();
}

void Services::togglePlay() {
    call("Clock", m_clock.value("Rolling").toBool() ? "Stop" : "Play", {}, [this](const QDBusMessage &) { readClock(); });
}

void Services::locate(const QString &position) {
    call("Clock", "Locate", {position}, [this](const QDBusMessage &) { readClock(); });
}

void Services::setTempo(double bpm) {
    call("Clock", "SetTempo", {std::clamp(bpm, 20.0, 999.0)});
}

// ----------------------------------------------------------------- volume

// wpctl's volume is cubic (0.5 is a gain of 0.125), so dB = 60·log10(v).
void Services::readVolume(int nodeId) {
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p, nodeId](int code) {
        const QString out = QString::fromUtf8(p->readAllStandardOutput());
        p->deleteLater();
        const auto m = QRegularExpression(R"(Volume:\s*([0-9.]+)(\s*\[MUTED\])?)").match(out);
        if (code != 0 || !m.hasMatch()) {
            m_volumes.remove(QString::number(nodeId));
        } else {
            const double v = m.captured(1).toDouble();
            m_volumes.insert(QString::number(nodeId), QVariantMap{{"db", v > 0 ? 60.0 * std::log10(v) : -120.0},
                                                                  {"muted", !m.captured(2).isEmpty()}});
        }
        emit volumesChanged();
    });
    p->start("wpctl", {"get-volume", QString::number(nodeId)});
}

void Services::setVolumeDb(int nodeId, double db) {
    db = std::clamp(db, -60.0, 6.0);
    const double v = db <= -60.0 ? 0.0 : std::pow(10.0, db / 60.0);
    auto entry = m_volumes.value(QString::number(nodeId)).toMap();
    entry.insert("db", db);
    m_volumes.insert(QString::number(nodeId), entry);
    emit volumesChanged();
    QProcess::startDetached("wpctl", {"set-volume", QString::number(nodeId), QString::number(v, 'f', 4)});
}

void Services::toggleMute(int nodeId) {
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p, nodeId] { p->deleteLater(); readVolume(nodeId); });
    p->start("wpctl", {"set-mute", QString::number(nodeId), "toggle"});
}
