// Viewport — QOpenGLWidget host for the renderer.
//
// The widget owns the GL context, so it also owns the renderer's lifetime: the
// GlView is initialised in initializeGL() and destroyed in the destructor while
// the context is still made current.
#pragma once

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QSet>
#include <QString>

#include <memory>

#include "Session/SimulationSession.hpp"

namespace render {
class GlView;
}
class QTimer;

namespace ui {

class HudPill;

class Viewport : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit Viewport(SimulationSession* session, QWidget* parent = nullptr);
    ~Viewport() override;

    render::GlView* view() const { return m_view.get(); }

    double fps() const { return m_fps; }
    void resetCamera();
    void setViewMode(int mode);
    int  viewMode() const { return m_viewMode; }

    // Saves the last rendered frame as PNG; returns false when no GL frame is
    // available yet.
    bool saveFrame(const QString& path);

    // Overlay HUD text (FPS badge / tank label), refreshed by MainWindow.
    void setHudText(const QString& text);
    void setHudVisible(bool visible);

    void setOrbitOnly(bool orbitOnly);
    bool orbitOnly() const { return m_orbitOnly; }

Q_SIGNALS:
    void fpsChanged(double fps);
    void cameraReset();
    // Emitted when the wheel changes the field of view, so the Camera card can
    // follow along.
    void fieldOfViewChanged(double degrees);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void onTick();
    void applyFlyStep();
    void layoutHud();

    SimulationSession* m_session = nullptr;
    std::unique_ptr<render::GlView> m_view;
    QTimer* m_timer = nullptr;
    HudPill* m_hud = nullptr;
    QWidget* m_border = nullptr;

    QPoint m_lastPos;
    bool m_orbiting = false;
    bool m_panning = false;
    bool m_initialised = false;
    bool m_orbitOnly = true;

    double m_fps = 0.0;
    QElapsedTimer m_frameTimer;
    qint64 m_lastFrameNs = 0;

    int m_viewMode = 0;
    int m_tankScene = 0;
    SimSnapshotPtr m_lastSnapshot;
    SurfaceSnapshotPtr m_lastSurface;
    ModelDataPtr m_lastModel;

    QSet<int> m_keys;
};

}  // namespace ui
