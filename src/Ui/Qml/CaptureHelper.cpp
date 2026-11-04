#include "Ui/Qml/CaptureHelper.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QQuickWindow>

#include "Session/SimulationSession.hpp"

namespace ui {

CaptureHelper::CaptureHelper(SimulationSession& session, QObject* parent)
    : QObject(parent), m_session(session) {}

QString CaptureHelper::save(QObject* windowObject) {
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window) return QString();

    QDir dir(QStringLiteral("captures"));
    if (!dir.exists() && !QDir().mkpath(dir.path())) return QString();

    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString base = dir.filePath(QStringLiteral("iisph-") + stamp);

    const QImage image = window->grabWindow();
    if (image.isNull() || !image.save(base + QStringLiteral(".png"), "PNG")) return QString();

    QFile json(base + QStringLiteral(".json"));
    if (json.open(QIODevice::WriteOnly | QIODevice::Text)) {
        json.write(m_session.exportParamsJson().toUtf8());
        json.close();
    }
    return base + QStringLiteral(".png");
}

}  // namespace ui
