// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

Theme::Theme(const QString &directory, const QString &designFile, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory),
      m_designFile(!designFile.isEmpty() ? designFile
                   : (qEnvironmentVariableIsEmpty("XDG_CONFIG_HOME") ? QDir::homePath() + "/.config"
                                                                     : qEnvironmentVariable("XDG_CONFIG_HOME")) +
                         "/backline/design.json") {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding; without Hyprland they stay
    // sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (m_designRadius < 0 && value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    reload();
}

Theme::~Theme() {
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

// colors.toml: `key = "#rrggbb"`. Omarchy's shell reads foreground,
// background, accent (else color4), muted (else color8) and red/color1 as
// urgent; the meter's green and yellow are color2/green and color3/yellow.
QHash<QString, QColor> Theme::readColors(const QString &path) {
    QHash<QString, QString> raw;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QRegularExpression line(R"re(^\s*([A-Za-z0-9_-]+)\s*=\s*["']?(#[0-9A-Fa-f]{6}))re",
                                      QRegularExpression::MultilineOption);
        auto it = line.globalMatch(QString::fromUtf8(file.readAll()));
        while (it.hasNext()) {
            const auto m = it.next();
            raw.insert(m.captured(1), m.captured(2));
        }
    }
    auto pick = [&](std::initializer_list<const char *> keys, const char *fallback) {
        for (const char *k : keys)
            if (raw.contains(k))
                return QColor(raw.value(k));
        return QColor(fallback);
    };
    QHash<QString, QColor> p;
    // The fallbacks are Omarchy's shell defaults (Commons/Color.qml).
    p["foreground"] = pick({"foreground", "color7"}, "#cacccc");
    p["background"] = pick({"background", "color0"}, "#101315");
    p["accent"] = pick({"accent", "color4"}, "#cacccc");
    p["urgent"] = pick({"red", "color1"}, "#a55555");
    p["muted"] = pick({"muted", "color8"}, "#707880");
    p["ok"] = pick({"green", "color2"}, "#7a9a6a");
    p["warn"] = pick({"yellow", "color3"}, "#c8a85a");
    return p;
}

QHash<QString, QString> Theme::readShell(const QString &path) {
    QHash<QString, QString> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return values;
    QString section;
    const QRegularExpression sectionLine(R"(^\[([A-Za-z0-9_-]+)\])");
    const QRegularExpression kv(R"re(^([A-Za-z0-9_-]+)\s*=\s*["']?([^"'#]+?)["']?\s*(#.*)?$)re");
    for (const QString &rawLine : QString::fromUtf8(file.readAll()).split('\n')) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const auto s = sectionLine.match(line);
        if (s.hasMatch()) {
            section = s.captured(1);
            continue;
        }
        const auto m = kv.match(line);
        if (m.hasMatch() && !section.isEmpty())
            values.insert(section + "." + m.captured(1), m.captured(2).trimmed());
    }
    return values;
}

static QColor blend(const QColor &a, const QColor &b, double t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, 1.0);
}

// Surfaces from the palette, by the alphas Omarchy's Style uses for fills
// (normal 0.04, hover 0.08, selected 0.18) composited over the background,
// so they are opaque and cheap to draw.
QHash<QString, QColor> Theme::derive(const QHash<QString, QColor> &p) {
    QHash<QString, QColor> t = p;
    const QColor fg = p.value("foreground"), bg = p.value("background");
    t["surface"] = blend(bg, fg, 0.04);
    t["raised"] = blend(bg, fg, 0.08);
    t["line"] = blend(bg, fg, 0.16);
    t["dim"] = blend(bg, fg, 0.55);
    return t;
}

void Theme::applyDesign(QHash<QString, QColor> &tokens) {
    m_designRadius = -1;
    QFile file(m_designFile);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject design = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject colors = design.value("colors").toObject();
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        const QColor c(it.value().toString());
        if (c.isValid())
            tokens.insert(it.key(), c);
    }
    if (design.value("fontFamily").isString())
        m_fontFamily = design.value("fontFamily").toString();
    if (design.value("radius").isDouble()) {
        m_designRadius = design.value("radius").toInt();
        m_radius = m_designRadius;
    }
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory, m_directory + "/theme",
                           m_directory + "/theme/colors.toml", QFileInfo(m_designFile).absolutePath(), m_designFile};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    auto palette = readColors(m_directory + "/theme/colors.toml");
    const auto shell = readShell(m_directory + "/theme/shell.toml");
    const auto user = readShell(QDir::homePath() + "/.config/omarchy/shell.toml");
    QString family = user.value("font.family", shell.value("font.family"));
    // Omarchy's shell resolves its font with fc-match, as here: the generic
    // "monospace" is the font Omarchy's font picker set.
    if (family.isEmpty() || family == "monospace") {
        static QString resolved;
        if (resolved.isEmpty()) {
            QProcess fc;
            fc.start("fc-match", {"-f", "%{family[0]}", "monospace"});
            resolved = fc.waitForFinished(1000) ? QString::fromUtf8(fc.readAllStandardOutput()).trimmed() : QString();
            if (resolved.isEmpty()) resolved = "monospace";
        }
        family = resolved;
    }
    m_fontFamily = family;
    QFile nameFile(m_directory + "/theme.name");
    m_name = nameFile.open(QIODevice::ReadOnly) ? QString::fromUtf8(nameFile.readLine()).trimmed()
                                                : QFileInfo(QFileInfo(m_directory + "/theme").canonicalFilePath()).fileName();
    auto tokens = derive(palette);
    applyDesign(tokens);
    m_tokens = tokens;
    emit changed();
}

QColor Theme::accentForeground() const {
    const QColor c = accent();
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? QColor("black") : QColor("white");
}

QColor Theme::mix(const QColor &a, const QColor &b, double t) const { return blend(a, b, t); }

QColor Theme::alpha(const QColor &c, double a) const {
    QColor out = c;
    out.setAlphaF(a);
    return out;
}
