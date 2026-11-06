#include "Ui/Qml/CaptureHelper.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QQuickWindow>

namespace ui {

CaptureHelper::CaptureHelper(QObject* parent) : QObject(parent) {}

QString CaptureHelper::save(QObject* windowObject) {
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window) return QString();

    QDir dir(QStringLiteral("captures"));
    if (!dir.exists() && !QDir().mkpath(dir.path())) return QString();

    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString base = dir.filePath(QStringLiteral("iisph-") + stamp);

    const QString imagePath = base + QStringLiteral(".png");
    const QImage image = window->grabWindow();
    if (image.isNull() || !image.save(imagePath, "PNG")) return QString();
    return QDir::toNativeSeparators(QFileInfo(imagePath).absoluteFilePath());
}

}  // namespace ui
