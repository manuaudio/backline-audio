// backline-mixer: Backline's mixer and recorder window. Every channel strip
// (inputs, Backline instruments, outputs) with a live meter, the transport and
// timecode, the takes and the virtual soundcheck, themed live from Omarchy.

#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "graph.h"
#include "meter.h"
#include "services.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("backline-mixer");
    app.setApplicationVersion("0.1.0");
    // Matches the window to backline-mixer.desktop (Wayland app_id), so the
    // launcher and omarchy-launch-or-focus find it.
    app.setDesktopFileName("backline-mixer");
    app.setWindowIcon(QIcon::fromTheme("backline-mixer"));
    QQuickStyle::setStyle("Basic");

    Translator translator;
    app.installTranslator(&translator);

    qmlRegisterType<MeterItem>("Backline", 1, 0, "Meter");

    Theme theme;
    Graph graph;
    MeterBank meters(&graph);
    Services services;

    // Text items take the application font, so it carries Omarchy's font.
    auto applyFont = [&theme] {
        QFont font(theme.fontFamily());
        font.setPixelSize(12);
        QGuiApplication::setFont(font);
    };
    applyFont();
    QObject::connect(&theme, &Theme::changed, &app, applyFont);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("graph", &graph);
    engine.rootContext()->setContextProperty("meters", &meters);
    engine.rootContext()->setContextProperty("services", &services);
    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}
