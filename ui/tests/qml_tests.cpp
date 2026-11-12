// Qt Quick Test runner for the mixer's QML, with Backline's C++ types and the
// context objects the window gets (theme, meters, services), the services on
// a temporary state directory.
#include <QtQuickTest>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QVariantList>
#include <QTemporaryDir>

#include "meter.h"
#include "services.h"
#include "theme.h"

class Setup : public QObject {
    Q_OBJECT
public slots:
    void qmlEngineAvailable(QQmlEngine *engine) {
        qmlRegisterType<MeterItem>("Backline", 1, 0, "Meter");
        auto *theme = new Theme({}, m_dir.filePath("design.json"), engine);
        auto *meters = new MeterBank(nullptr, engine);
        auto *services = new Services(engine, m_dir.path());
        engine->rootContext()->setContextProperty("theme", theme);
        engine->rootContext()->setContextProperty("meters", meters);
        engine->rootContext()->setContextProperty("services", services);
        // the graph as the window sees it with PipeWire away (no strips)
        auto *graph = new QQmlPropertyMap(engine);
        graph->insert("connected", false);
        graph->insert("nodes", QVariantList{});
        engine->rootContext()->setContextProperty("graph", graph);
        // BL_UI_SHOTS=DIR: the layout test saves what it laid out there
        engine->rootContext()->setContextProperty("shotsDir", qEnvironmentVariable("BL_UI_SHOTS"));
    }

private:
    QTemporaryDir m_dir;
};

QUICK_TEST_MAIN_WITH_SETUP(mixer_qml, Setup)
#include "qml_tests.moc"
