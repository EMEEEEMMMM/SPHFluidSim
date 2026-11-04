// CaptureHelper — saves the QML window as PNG plus the matching parameter JSON.
// QML cannot write files itself, so the front-end exposes one invokable.
#pragma once

#include <QObject>
#include <QString>

class SimulationSession;

namespace ui {

class CaptureHelper : public QObject {
    Q_OBJECT
public:
    explicit CaptureHelper(SimulationSession& session, QObject* parent = nullptr);

    // windowObject must be the QQuickWindow; returns the relative PNG path or an
    // empty string on failure.
    Q_INVOKABLE QString save(QObject* windowObject);

private:
    SimulationSession& m_session;
};

}  // namespace ui
