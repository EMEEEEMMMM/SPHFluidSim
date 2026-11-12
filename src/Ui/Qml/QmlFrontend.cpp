#include "Ui/Qml/QmlFrontend.hpp"

#include <QApplication>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>

#include <cstdio>

#include "Render/RenderJob.hpp"
#include "Session/SimulationSession.hpp"
#include "Ui/Qml/CaptureHelper.hpp"
#include "Ui/Qml/IconItem.hpp"
#include "Ui/Qml/ViewportItem.hpp"
#include "Ui/Theme.hpp"

namespace ui {

int RunQmlFrontend(QApplication& app, SimulationSession& session, bool autoplay,
                   const std::string& screenshotPath, int screenshotDelayMs) {
    // The renderer is plain OpenGL, so Qt Quick must run on the OpenGL RHI
    // backend; D3D11/Vulkan would give the scene graph a surface our glad-based
    // renderer cannot draw into.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    qmlRegisterSingletonInstance("IISPH.Theme", 1, 0, "Theme", &Theme::instance());
    qmlRegisterType<ViewportItem>("IISPH.View", 1, 0, "ViewportItem");
    qmlRegisterType<IconItem>("IISPH.View", 1, 0, "IconItem");

    CaptureHelper capture;
    RenderJob renderJob;

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                     [](const QList<QQmlError>& list) {
                         for (const QQmlError& error : list) {
                             std::fprintf(stderr, "[qml] %s\n",
                                          error.toString().toUtf8().constData());
                         }
                     });
    engine.rootContext()->setContextProperty(QStringLiteral("session"), &session);
    engine.rootContext()->setContextProperty(QStringLiteral("capture"), &capture);
    engine.rootContext()->setContextProperty(QStringLiteral("renderJob"), &renderJob);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::fprintf(stderr, "[ui] failed to load qrc:/qml/Main.qml\n");
        return 1;
    }

    session.start();
    if (autoplay) session.setRunning(true);

    if (!screenshotPath.empty()) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        if (window) {
            window->raise();
            window->requestActivate();
            const QString path = QString::fromStdString(screenshotPath);
            QTimer::singleShot(screenshotDelayMs, window, [window, path] {
                // Prefer Qt's own window grab: it is deterministic and does not
                // depend on which external window happens to be foreground.
                // Fall back to a screen grab only when the framebuffer item
                // cannot be captured synchronously.
                QImage shot = window->grabWindow();
                if (shot.isNull()) {
                    if (QScreen* screen = window->screen()) {
                        shot = screen->grabWindow(window->winId()).toImage();
                    }
                }
                if (!shot.isNull()) shot.save(path, "PNG");
                QCoreApplication::quit();
            });
        }
    }
    return app.exec();
}

}  // namespace ui
