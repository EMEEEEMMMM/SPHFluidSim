// ViewportItem — the QML front-end's GL surface.
//
// QQuickFramebufferObject hands the renderer a Qt-owned FBO and a GL context on
// the render thread; the renderer draws into its own target (GlView) and blits
// the result, exactly like the widget front-end does. Camera state lives on the
// GUI thread and travels to the renderer through synchronize().
#pragma once

#include <QPoint>
#include <QQuickFramebufferObject>
#include <QSet>

#include "Render/CameraRig.hpp"
#include "Session/SimulationSession.hpp"

class QTimer;

namespace ui {

class ViewportItem : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(SimulationSession* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(int viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(bool orbitOnly READ orbitOnly WRITE setOrbitOnly NOTIFY orbitOnlyChanged)
    Q_PROPERTY(qreal fieldOfView READ fieldOfView WRITE setFieldOfView NOTIFY fieldOfViewChanged)

public:
    explicit ViewportItem(QQuickItem* parent = nullptr);

    Renderer* createRenderer() const override;

    SimulationSession* session() const { return m_session; }
    void setSession(SimulationSession* session);

    int viewMode() const { return m_viewMode; }
    void setViewMode(int mode);

    bool orbitOnly() const { return m_orbitOnly; }
    void setOrbitOnly(bool orbitOnly);

    qreal fieldOfView() const { return m_camera.fovDeg; }
    void setFieldOfView(qreal degrees);

    const render::CameraState& cameraState() const { return m_camera; }

    Q_INVOKABLE void resetCamera();

Q_SIGNALS:
    void sessionChanged();
    void viewModeChanged();
    void orbitOnlyChanged();
    void fieldOfViewChanged();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void onTick();
    void applyFlyStep();

    SimulationSession* m_session = nullptr;
    QTimer* m_timer = nullptr;
    int m_viewMode = 0;
    bool m_orbitOnly = true;
    render::CameraState m_camera;

    QPoint m_lastPos;
    bool m_orbiting = false;
    bool m_panning = false;
    QSet<int> m_keys;
};

}  // namespace ui
