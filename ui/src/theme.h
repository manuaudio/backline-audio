#pragma once

// The Omarchy theme as the mixer uses it: the shell's foundational palette
// (foreground, background, accent, urgent, muted) plus the two status hues a
// meter needs (the theme's green and yellow), the font, and Hyprland's
// rounding. It follows theme switches live, the way Omarchy's own apps do
// (theme watching after Omacut, MIT, David Heinemeier Hansson), and an
// optional design file can override any token (see DESIGN-LANGUAGE.md).

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor urgent READ urgent NOTIFY changed)
    Q_PROPERTY(QColor muted READ muted NOTIFY changed)
    Q_PROPERTY(QColor ok READ ok NOTIFY changed)
    Q_PROPERTY(QColor warn READ warn NOTIFY changed)
    Q_PROPERTY(QColor surface READ surface NOTIFY changed)
    Q_PROPERTY(QColor raised READ raised NOTIFY changed)
    Q_PROPERTY(QColor line READ line NOTIFY changed)
    Q_PROPERTY(QColor dim READ dim NOTIFY changed)
    Q_PROPERTY(QColor accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QString fontFamily READ fontFamily NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QString designFile READ designFile NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, const QString &designFile = {}, QObject *parent = nullptr,
                   const QString &hyprctl = {});
    ~Theme() override;

    QColor foreground() const { return token("foreground"); }
    QColor background() const { return token("background"); }
    QColor accent() const { return token("accent"); }
    QColor urgent() const { return token("urgent"); }
    QColor muted() const { return token("muted"); }
    QColor ok() const { return token("ok"); }
    QColor warn() const { return token("warn"); }
    // Derived surfaces, as Omarchy's Style derives fills from the foreground.
    QColor surface() const { return token("surface"); }
    QColor raised() const { return token("raised"); }
    QColor line() const { return token("line"); }
    QColor dim() const { return token("dim"); }
    QColor accentForeground() const;
    QString fontFamily() const { return m_fontFamily; }
    int radius() const { return m_radius; }
    QString name() const { return m_name; }
    QString designFile() const { return m_designFile; }

    Q_INVOKABLE QColor mix(const QColor &a, const QColor &b, double t) const;
    Q_INVOKABLE QColor alpha(const QColor &c, double a) const;

    // Parsing, public for tests.
    static QHash<QString, QColor> readColors(const QString &colorsToml);
    static QHash<QString, QString> readShell(const QString &shellToml);
    static QHash<QString, QColor> derive(const QHash<QString, QColor> &palette);

signals:
    void changed();

private:
    QColor token(const QString &key) const { return m_tokens.value(key); }
    void reload();
    void refreshRounding();
    void applyDesign(QHash<QString, QColor> &tokens);

    QString m_directory;
    QString m_designFile;
    QString m_name;
    QString m_fontFamily = "monospace";
    QHash<QString, QColor> m_tokens;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    int m_designRadius = -1;
    QProcess m_roundingProcess;
    QTimer m_roundingTimeout;
};
