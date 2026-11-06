// CaptureHelper — saves the QML window as a PNG.
// QML cannot write files itself, so the front-end exposes one invokable.
#pragma once

#include <QObject>
#include <QString>

namespace ui {

class CaptureHelper : public QObject {
    Q_OBJECT
public:
    explicit CaptureHelper(QObject* parent = nullptr);

    // windowObject must be the QQuickWindow; returns the absolute PNG path or
    // an empty string on failure.
    Q_INVOKABLE QString save(QObject* windowObject);
};

}  // namespace ui
