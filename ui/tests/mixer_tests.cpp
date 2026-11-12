// The mixer's C++: the Omarchy theme it reads, the meter law and ballistics,
// and the armed set it shares with the panels. Offscreen, temp directories.
#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "meter.h"
#include "services.h"
#include "theme.h"

class MixerTests : public QObject {
    Q_OBJECT

private:
    static void write(const QString &path, const QByteArray &text) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(text);
    }

private slots:
    // Omarchy's colors.toml, as Commons/Color.qml reads it: red is urgent,
    // accent falls back to color4; the meter adds green and yellow.
    void readsOmarchyColors() {
        QTemporaryDir dir;
        write(dir.filePath("colors.toml"),
              "accent = \"#7aa2f7\"\nbackground = \"#1a1b26\"\nforeground = \"#a9b1d6\"\n"
              "red = \"#f7768e\"\ngreen = \"#9ece6a\"\nyellow = \"#e0af68\"\nmuted = \"#414868\"\n");
        const auto c = Theme::readColors(dir.filePath("colors.toml"));
        QCOMPARE(c.value("accent"), QColor("#7aa2f7"));
        QCOMPARE(c.value("urgent"), QColor("#f7768e"));
        QCOMPARE(c.value("ok"), QColor("#9ece6a"));
        QCOMPARE(c.value("warn"), QColor("#e0af68"));

        write(dir.filePath("terminal.toml"), "color0 = \"#000000\"\ncolor1 = \"#aa0000\"\ncolor2 = \"#00aa00\"\n"
                                             "color3 = \"#aaaa00\"\ncolor4 = \"#0000aa\"\ncolor7 = \"#cccccc\"\n");
        const auto t = Theme::readColors(dir.filePath("terminal.toml"));
        QCOMPARE(t.value("accent"), QColor("#0000aa"));
        QCOMPARE(t.value("urgent"), QColor("#aa0000"));
        QCOMPARE(t.value("ok"), QColor("#00aa00"));
        QCOMPARE(t.value("foreground"), QColor("#cccccc"));
        QCOMPARE(t.value("background"), QColor("#000000"));
    }

    // Surfaces are opaque blends at Omarchy's fill alphas, so a theme switch
    // re-derives them and nothing needs alpha blending to draw.
    void derivesSurfaces() {
        QHash<QString, QColor> p{{"foreground", QColor("#ffffff")}, {"background", QColor("#000000")}};
        const auto d = Theme::derive(p);
        QCOMPARE(d.value("surface").alpha(), 255);
        QVERIFY(qAbs(d.value("surface").redF() - 0.04) < 0.01);
        QVERIFY(qAbs(d.value("raised").redF() - 0.08) < 0.01);
        QVERIFY(d.value("line").redF() > d.value("raised").redF());
    }

    // The design slot: a design.json overrides any token, the font and the
    // radius, and is followed live like the theme.
    void designFileOverridesTokens() {
        QTemporaryDir dir;
        QDir().mkpath(dir.filePath("current/theme"));
        write(dir.filePath("current/theme/colors.toml"), "accent = \"#7aa2f7\"\nbackground = \"#1a1b26\"\n");
        write(dir.filePath("design.json"), "{\"colors\": {\"ok\": \"#00ff88\", \"accent\": \"#ff8800\"}, \"radius\": 6, \"fontFamily\": \"Inter\"}");
        Theme theme(dir.filePath("current"), dir.filePath("design.json"));
        QCOMPARE(theme.ok(), QColor("#00ff88"));
        QCOMPARE(theme.accent(), QColor("#ff8800"));
        QCOMPARE(theme.background(), QColor("#1a1b26"));
        QCOMPARE(theme.radius(), 6);
        QCOMPARE(theme.fontFamily(), QString("Inter"));

        QSignalSpy changed(&theme, &Theme::changed);
        write(dir.filePath("current/theme/colors.toml"), "accent = \"#7aa2f7\"\nbackground = \"#ffffff\"\n");
        QVERIFY(changed.wait(2000));
        QCOMPARE(theme.background(), QColor("#ffffff"));
    }

    void meterLaw() {
        QCOMPARE(meterPosition(0), 1.0);
        QCOMPARE(meterPosition(-20), 0.5);
        QCOMPARE(meterPosition(-40), 0.25);
        QCOMPARE(meterPosition(-60), 0.0);
        QCOMPARE(meterPosition(-200), 0.0);
        double last = -1;
        for (double db = -70; db <= 3; db += 0.5) {
            QVERIFY(meterPosition(db) >= last);
            last = meterPosition(db);
        }
    }

    // Instant attack, 24 dB/s release, 1.5 s hold, a latched clip lamp.
    void ballistics() {
        MeterItem m;
        const float half[2] = {0.5f, 0.5f};      // -6.02 dBFS
        const float silent[2] = {0.f, 0.f};
        const float full[2] = {1.0f, 0.01f};
        QVERIFY(m.push(half, 1.0 / 30));
        QVERIFY(qAbs(m.levelDb(0) + 6.02) < 0.01);
        for (int i = 0; i < 15; i++) m.push(silent, 1.0 / 30);     // 0.5 s
        QVERIFY(qAbs(m.levelDb(0) - (-6.02 - 12.0)) < 0.05);
        QVERIFY(qAbs(m.holdDb() + 6.02) < 0.01);                    // still held
        for (int i = 0; i < 45; i++) m.push(silent, 1.0 / 30);     // 2 s total
        QVERIFY(m.holdDb() < -6.5);                                  // falling now
        QVERIFY(!m.clipped());
        m.push(full, 1.0 / 30);
        QVERIFY(m.clipped());
        for (int i = 0; i < 300; i++) m.push(silent, 1.0 / 30);
        QVERIFY(m.clipped());                                         // latched
        m.resetClip();
        QVERIFY(!m.clipped());
        QCOMPARE(m.levelDb(0), MeterItem::kFloorDb);
    }

    // Nothing changes, nothing redraws.
    void silenceDoesNotRedraw() {
        MeterItem m;
        const float silent[2] = {0.f, 0.f};
        m.push(silent, 1.0 / 30);
        QVERIFY(!m.push(silent, 1.0 / 30));
    }

    // The armed set is the panels' file: a JSON array of node names.
    void armedSetFile() {
        QCOMPARE(Services::parseArmed("[\"alsa_input.usb\",\"backline.surge\"]"), QStringList({"alsa_input.usb", "backline.surge"}));
        QCOMPARE(Services::parseArmed("not json"), QStringList());
        QCOMPARE(Services::parseArmed("{\"a\":1}"), QStringList());
        QTemporaryDir dir;
        write(dir.filePath("record-sources.json"), "[\"mic\"]");
        Services services(nullptr, dir.path());
        QCOMPARE(services.armed(), QStringList({"mic"}));
        QCOMPARE(services.armedFile(), dir.filePath("record-sources.json"));
    }
};

QTEST_MAIN(MixerTests)
#include "mixer_tests.moc"
