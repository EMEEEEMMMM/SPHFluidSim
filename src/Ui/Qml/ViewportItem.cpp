#include "Ui/Qml/ViewportItem.hpp"

#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QTimer>
#include <QWheelEvent>

#include <atomic>
#include <cstdio>
#include <memory>

#include "Render/GlView.hpp"
#include "Ui/Theme.hpp"

namespace ui {
namespace {

void* QtGlLoader(const char* name) {
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (!ctx) return nullptr;
    return reinterpret_cast<void*>(ctx->getProcAddress(name));
}

class ViewportRenderer : public QQuickFramebufferObject::Renderer {
public:
    ~ViewportRenderer() override = default;

    void synchronize(QQuickFramebufferObject* item) override {
        auto* viewport = static_cast<ViewportItem*>(item);
        if (viewport->session() != m_session) m_session = viewport->session();
        m_viewMode = viewport->viewMode();
        m_camera = viewport->cameraState();

        if (m_session) {
            SimSnapshotPtr snap = m_session->snapshot();
            if (snap != m_snapshot) m_snapshot = snap;
            SurfaceSnapshotPtr surf = m_session->surface();
            if (surf != m_surface) m_surface = surf;
        }
        const QColor bg = Theme::instance().current().bg;
        m_bg[0] = static_cast<float>(bg.redF());
        m_bg[1] = static_cast<float>(bg.greenF());
        m_bg[2] = static_cast<float>(bg.blueF());

        // GUI thread: safe to forward the last triangle count to the session.
        if (m_session) m_session->setTriangleCount(m_triangles.load());
    }

    void render() override {
        if (!m_view) m_view = std::make_unique<render::GlView>();
        if (!m_view->IsReady()) {
            m_view->Initialize(&QtGlLoader);
            if (!m_view->IsReady()) return;
        }

        auto* fbo = framebufferObject();
        if (!fbo) return;
        const int w = fbo->width();
        const int h = fbo->height();
        if (w <= 0 || h <= 0) return;
        m_view->Resize(w, h);

        if (m_snapshot) {
            if (m_snapshot != m_uploadedSnapshot) {
                m_view->UpdateParticles(m_snapshot->positions, m_snapshot->colors);
                m_uploadedSnapshot = m_snapshot;
            }
            if (m_tankScene != m_snapshot->scene) {
                if (m_snapshot->scene == 1) {
                    m_view->SetTank(1.0f, 1.0f, 1.0f, 1.0f, 0.5f, true);
                } else {
                    m_view->SetTank(2.0f, 1.2f, 1.0f, 0.0f, 0.0f, true);
                }
                m_tankScene = m_snapshot->scene;
            }
        }
        if (m_surface != m_uploadedSurface) {
            if (m_surface) m_view->UpdateSurface(m_surface->vertices, m_surface->indices);
            else m_view->UpdateSurface({}, {});
            m_uploadedSurface = m_surface;
        }
        m_view->SetSurfaceVisible(m_viewMode > 0);
        m_view->SetParticlesVisible(m_viewMode != 1);
        m_view->cameraState() = m_camera;

        m_view->Render();
        m_view->BlitTo(static_cast<uint32_t>(fbo->handle()), w, h, m_bg[0], m_bg[1], m_bg[2]);
        m_triangles.store(m_view->TriangleCount());
    }

private:
    std::unique_ptr<render::GlView> m_view;
    SimulationSession* m_session = nullptr;
    SimSnapshotPtr m_snapshot;
    SimSnapshotPtr m_uploadedSnapshot;
    SurfaceSnapshotPtr m_surface;
    SurfaceSnapshotPtr m_uploadedSurface;
    render::CameraState m_camera;
    int m_viewMode = 0;
    int m_tankScene = -1;
    float m_bg[3] = {0.043f, 0.047f, 0.055f};
    std::atomic<int> m_triangles{0};
};

}  // namespace

ViewportItem::ViewportItem(QQuickItem* parent) : QQuickFramebufferObject(parent) {
    // OpenGL's texture origin is bottom-left, while the Qt Quick scene graph
    // samples this FBO top-down. Without mirroring, the whole 3D view is
    // vertically flipped (the water looks like its inside-out back walls).
    setMirrorVertically(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setFlag(ItemIsFocusScope, true);
    setFocus(true);
    setCursor(QCursor(Qt::OpenHandCursor));

    m_timer = new QTimer(this);
    m_timer->setInterval(15);
    connect(m_timer, &QTimer::timeout, this, &ViewportItem::onTick);
    m_timer->start();
}

QQuickFramebufferObject::Renderer* ViewportItem::createRenderer() const {
    return new ViewportRenderer();
}

void ViewportItem::setSession(SimulationSession* session) {
    if (m_session == session) return;
    m_session = session;
    Q_EMIT sessionChanged();
    update();
}

void ViewportItem::setViewMode(int mode) {
    if (m_viewMode == mode) return;
    m_viewMode = mode;
    Q_EMIT viewModeChanged();
    update();
}

void ViewportItem::setOrbitOnly(bool orbitOnly) {
    if (m_orbitOnly == orbitOnly) return;
    m_orbitOnly = orbitOnly;
    if (m_orbitOnly) m_keys.clear();
    Q_EMIT orbitOnlyChanged();
}

void ViewportItem::setFieldOfView(qreal degrees) {
    if (qFuzzyCompare(static_cast<double>(m_camera.fovDeg), static_cast<double>(degrees))) return;
    m_camera.fovDeg = static_cast<float>(degrees);
    render::ClampCamera(m_camera);
    Q_EMIT fieldOfViewChanged();
    update();
}

void ViewportItem::resetCamera() {
    m_camera = render::CameraState{};
    Q_EMIT fieldOfViewChanged();
    update();
}

void ViewportItem::onTick() {
    applyFlyStep();
    update();
}

void ViewportItem::applyFlyStep() {
    if (m_orbitOnly || m_keys.isEmpty()) return;
    const float forward = (m_keys.contains(Qt::Key_W) ? 1.0f : 0.0f) -
                          (m_keys.contains(Qt::Key_S) ? 1.0f : 0.0f);
    const float strafe = (m_keys.contains(Qt::Key_D) ? 1.0f : 0.0f) -
                         (m_keys.contains(Qt::Key_A) ? 1.0f : 0.0f);
    const float up = (m_keys.contains(Qt::Key_E) ? 1.0f : 0.0f) -
                     (m_keys.contains(Qt::Key_Q) ? 1.0f : 0.0f);
    if (forward == 0.0f && strafe == 0.0f && up == 0.0f) return;
    render::FlyCamera(m_camera, forward, strafe, up, 0.015f);
}

void ViewportItem::mousePressEvent(QMouseEvent* event) {
    forceActiveFocus();
    m_lastPos = event->position().toPoint();
    if (event->button() == Qt::LeftButton) {
        m_orbiting = true;
        setCursor(QCursor(Qt::ClosedHandCursor));
    } else if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        m_panning = true;
        setCursor(QCursor(Qt::SizeAllCursor));
    }
    event->accept();
}

void ViewportItem::mouseMoveEvent(QMouseEvent* event) {
    const QPoint pos = event->position().toPoint();
    const QPoint delta = pos - m_lastPos;
    m_lastPos = pos;
    if (m_orbiting) {
        if (m_orbitOnly) {
            render::OrbitCamera(m_camera, static_cast<float>(delta.x()),
                                static_cast<float>(delta.y()));
        } else {
            render::LookCamera(m_camera, static_cast<float>(delta.x()),
                               static_cast<float>(delta.y()));
        }
        update();
    } else if (m_panning) {
        render::PanCamera(m_camera, static_cast<float>(delta.x()),
                          static_cast<float>(delta.y()), static_cast<int>(height()));
        update();
    }
    event->accept();
}

void ViewportItem::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) m_orbiting = false;
    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        m_panning = false;
    }
    if (!m_orbiting && !m_panning) setCursor(QCursor(Qt::OpenHandCursor));
    event->accept();
}

void ViewportItem::wheelEvent(QWheelEvent* event) {
    render::ZoomCamera(m_camera, static_cast<float>(event->angleDelta().y()) / 120.0f);
    // The wheel changes the field of view, so tell the Camera card to update.
    Q_EMIT fieldOfViewChanged();
    update();
    event->accept();
}

void ViewportItem::mouseDoubleClickEvent(QMouseEvent* event) {
    resetCamera();
    event->accept();
}

void ViewportItem::keyPressEvent(QKeyEvent* event) {
    // WASD/QE are flight controls: in Orbit mode they must fall through to the
    // window shortcuts (S = single step, R = restart, ...) instead of being
    // swallowed here.
    if (!m_orbitOnly) {
        switch (event->key()) {
            case Qt::Key_W:
            case Qt::Key_A:
            case Qt::Key_S:
            case Qt::Key_D:
            case Qt::Key_E:
            case Qt::Key_Q:
                m_keys.insert(event->key());
                return;
            default:
                break;
        }
    }
    QQuickFramebufferObject::keyPressEvent(event);
}

void ViewportItem::keyReleaseEvent(QKeyEvent* event) {
    m_keys.remove(event->key());
    QQuickFramebufferObject::keyReleaseEvent(event);
}

void ViewportItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickFramebufferObject::geometryChange(newGeometry, oldGeometry);
    update();
}

}  // namespace ui
