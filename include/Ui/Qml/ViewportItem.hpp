// ViewportItem — the QML front-end's GL surface.
//
// QQuickFramebufferObject hands the renderer a Qt-owned FBO and a GL context on
// the render thread; the renderer draws into its own target (GlView) and blits
// the result, exactly like the widget front-end does. Camera state lives on the
// GUI thread and travels to the renderer through synchronize().
//
// Empty-scene editing (selection + transform gizmo) is resolved entirely on the
// GUI thread: a second, never-Initialized GlView supplies the CPU-side ray /
// projection maths (no GL calls are made from here). Only the resulting
// transform is pushed into the session; the session's immutable model snapshot
// travels back to the render thread through synchronize().
#pragma once

#include <QPoint>
#include <QQuickFramebufferObject>
#include <QSet>
#include <QVariantMap>

#include "Math/Quaternions.hpp"
#include "Math/Ray.hpp"
#include "Render/CameraRig.hpp"
#include "Render/GlView.hpp"
#include "Session/SimulationSession.hpp"

class QTimer;

namespace ui {

class ViewportItem : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(SimulationSession* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(int viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(bool orbitOnly READ orbitOnly WRITE setOrbitOnly NOTIFY orbitOnlyChanged)
    Q_PROPERTY(qreal fieldOfView READ fieldOfView WRITE setFieldOfView NOTIFY fieldOfViewChanged)
    // Multiplier for orbit / pan / fly sensitivity (0.1x .. 4x).
    Q_PROPERTY(qreal cameraSpeed READ cameraSpeed WRITE setCameraSpeed NOTIFY cameraSpeedChanged)
    Q_PROPERTY(int sceneIndex READ sceneIndex NOTIFY sceneIndexChanged)
    Q_PROPERTY(int selectedModelId READ selectedModelId NOTIFY selectionChanged)

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

    qreal cameraSpeed() const { return m_cameraSpeed; }
    void setCameraSpeed(qreal speed);

    // Mirrors of the session state, refreshed on the GUI thread (tick + signals)
    // and read by the renderer in synchronize().
    int sceneIndex() const { return m_sceneIndex; }
    int selectedModelId() const { return m_selectedId; }
    const ModelSnapshotPtr& modelSnapshot() const { return m_models; }

    const render::CameraState& cameraState() const { return m_camera; }

    // ---- QML camera API ---------------------------------------------------
    Q_INVOKABLE QVariantMap currentCamera() const;
    Q_INVOKABLE void applyCameraPreset(const QString& name);
    Q_INVOKABLE void setCamera(const QVariantMap& config);
    // Alias kept for callers that use the session's setCameraConfig wording.
    Q_INVOKABLE void applyCameraConfig(const QVariantMap& config) { setCamera(config); }
    Q_INVOKABLE void resetCamera();
    // Orbit target/distance to the bounds of all visible models (no-op when the
    // scene has none). Called after an import so a 20 mm part is not a dot.
    Q_INVOKABLE void frameModels();

    // ---- QML picking / gizmo API (Empty scene) ----------------------------
    // nx, ny are normalised [0,1] coordinates inside the item, top-left origin.
    Q_INVOKABLE int pickModel(double nx, double ny);
    Q_INVOKABLE int pickGizmo(double nx, double ny);
    Q_INVOKABLE void beginGizmoDrag(int handle, double nx, double ny);
    Q_INVOKABLE void updateGizmoDrag(double nx, double ny);
    Q_INVOKABLE void endGizmoDrag();

Q_SIGNALS:
    void sessionChanged();
    void viewModeChanged();
    void orbitOnlyChanged();
    void fieldOfViewChanged();
    void cameraSpeedChanged();
    void sceneIndexChanged();
    void selectionChanged();

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

    // GUI-thread session mirror. Safe to read from synchronize() because Qt
    // blocks the GUI thread while the renderer synchronizes.
    void refreshSessionState();
    int querySceneIndex() const;
    int querySelectedModelId() const;
    void selectModelById(int id);

    // Camera helpers.
    static render::CameraState emptySceneCamera();
    void applySceneCamera();

    // CPU picking helpers.
    void updatePickView() const;
    bool screenRay(double nx, double ny, Math::Ray& ray) const;
    const ModelInstance* selectedInstance() const;
    static Math::Vector3 axisForHandle(int handle);
    static bool axisParam(const Math::Ray& ray, const Math::Vector3& origin,
                          const Math::Vector3& axis, float& param);
    static bool planeHit(const Math::Ray& ray, const Math::Vector3& origin,
                         const Math::Vector3& normal, Math::Vector3& point);

    SimulationSession* m_session = nullptr;
    QTimer* m_timer = nullptr;
    int m_viewMode = 0;
    bool m_orbitOnly = true;
    qreal m_cameraSpeed = 1.0;
    render::CameraState m_camera;

    QPoint m_lastPos;
    bool m_orbiting = false;
    bool m_panning = false;
    QSet<int> m_keys;

    // Session mirror (GUI thread).
    ModelSnapshotPtr m_models;
    int m_sceneIndex = -1;
    int m_selectedId = -1;
    QMetaObject::Connection m_paramsConnection;

    // Never Initialize()d: only ViewMatrix/ProjectionMatrix/Pick* are used.
    mutable render::GlView m_pickView;

    // Gizmo drag state. handle 1..3 = translate XYZ, 4..6 = rotate XYZ.
    int m_gizmoHandle = 0;
    int m_gizmoModelId = -1;
    Math::Vector3 m_dragAxis{1, 0, 0};
    Math::Vector3 m_dragOrigin{0, 0, 0};
    Math::Vector3 m_dragPosition0{0, 0, 0};
    Math::Quaternions m_dragRotation0;
    float m_dragAxisParam0 = 0.0f;
    Math::Vector3 m_dragRefDir{1, 0, 0};
};

}  // namespace ui
