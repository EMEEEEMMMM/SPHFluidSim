#include "Ui/Widgets/Viewport.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <vector>

#include "Render/GlView.hpp"
#include "Ui/Theme.hpp"
#include "Ui/Widgets/Controls.hpp"

namespace ui {
namespace {

void* QtGlLoader(const char* name) {
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (!ctx) return nullptr;
    return reinterpret_cast<void*>(ctx->getProcAddress(name));
}

// 1 px frame around the GL surface: QOpenGLWidget content cannot be clipped to
// a rounded rect, so the viewport keeps square corners in every skin.
class FrameOverlay : public QWidget {
public:
    explicit FrameOverlay(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const ThemeTokens& t = Theme::instance().current();
        QPainter p(this);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(t.border, 1));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
};

}  // namespace

Viewport::Viewport(SimulationSession* session, QWidget* parent)
    : QOpenGLWidget(parent), m_session(session) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);
    m_view = std::make_unique<render::GlView>();

    m_hud = new HudPill(this);
    m_hud->setDotVisible(true);
    m_border = new FrameOverlay(this);

    m_timer = new QTimer(this);
    m_timer->setInterval(15);
    connect(m_timer, &QTimer::timeout, this, &Viewport::onTick);
    m_timer->start();
    m_frameTimer.start();
}

Viewport::~Viewport() {
    if (m_view && m_view->IsReady()) {
        makeCurrent();
        m_view->Shutdown();
        doneCurrent();
    }
}

void Viewport::initializeGL() {
    m_view->Initialize(&QtGlLoader);
    m_initialised = m_view->IsReady();
    if (m_initialised) {
        m_view->SetParticleRadius(0.012f);
        m_view->SetTank(2.0f, 1.2f, 1.0f, 0.0f, 0.0f, true);
    }
}

void Viewport::resizeGL(int w, int h) {
    if (m_view) {
        const qreal dpr = devicePixelRatioF();
        m_view->Resize(static_cast<int>(w * dpr), static_cast<int>(h * dpr));
    }
}

void Viewport::paintGL() {
    if (!m_initialised) return;

    const qreal dpr = devicePixelRatioF();
    const int w = std::max(1, static_cast<int>(width() * dpr));
    const int h = std::max(1, static_cast<int>(height() * dpr));
    m_view->Resize(w, h);

    if (SimSnapshotPtr snap = m_session->snapshot()) {
        if (snap != m_lastSnapshot) {
            m_view->UpdateParticles(snap->positions, snap->colors);
            if (m_tankScene != snap->scene) {
                if (snap->scene == 1) m_view->SetTank(1.0f, 1.0f, 1.0f, 1.0f, 0.5f, true);
                else m_view->SetTank(2.0f, 1.2f, 1.0f, 0.0f, 0.0f, true);
                m_tankScene = snap->scene;
            }
            m_lastSnapshot = snap;
        }
    }
    SurfaceSnapshotPtr surf = m_session->surface();
    if (surf != m_lastSurface) {
        if (surf) m_view->UpdateSurface(surf->vertices, surf->indices);
        else m_view->UpdateSurface({}, {});
        m_lastSurface = surf;
    }

    m_view->SetSurfaceVisible(m_viewMode > 0);
    m_view->SetParticlesVisible(m_viewMode != 1);

    m_view->Render();

    const QColor bg = Theme::instance().current().bg;
    m_view->BlitTo(static_cast<uint32_t>(defaultFramebufferObject()), w, h,
                   static_cast<float>(bg.redF()), static_cast<float>(bg.greenF()),
                   static_cast<float>(bg.blueF()));

    // FPS is measured on presented frames.
    const qint64 now = m_frameTimer.nsecsElapsed();
    if (m_lastFrameNs != 0) {
        const double dt = (now - m_lastFrameNs) / 1e9;
        if (dt > 1e-5) {
            const double inst = 1.0 / dt;
            m_fps = (m_fps <= 0.0) ? inst : m_fps * 0.9 + inst * 0.1;
            Q_EMIT fpsChanged(m_fps);
        }
    }
    m_lastFrameNs = now;
}

void Viewport::onTick() {
    applyFlyStep();
    update();
}

void Viewport::applyFlyStep() {
    if (!m_view || m_orbitOnly || m_keys.isEmpty()) return;
    const float forward = (m_keys.contains(Qt::Key_W) ? 1.0f : 0.0f) -
                          (m_keys.contains(Qt::Key_S) ? 1.0f : 0.0f);
    const float strafe = (m_keys.contains(Qt::Key_D) ? 1.0f : 0.0f) -
                         (m_keys.contains(Qt::Key_A) ? 1.0f : 0.0f);
    const float up = (m_keys.contains(Qt::Key_E) ? 1.0f : 0.0f) -
                     (m_keys.contains(Qt::Key_Q) ? 1.0f : 0.0f);
    if (forward == 0.0f && strafe == 0.0f && up == 0.0f) return;
    m_view->Fly(forward, strafe, up, 0.015f);
}

void Viewport::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    m_lastPos = event->pos();
    if (event->button() == Qt::LeftButton) {
        m_orbiting = true;
        setCursor(Qt::ClosedHandCursor);
    } else if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        m_panning = true;
        setCursor(Qt::SizeAllCursor);
    }
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
    const QPoint delta = event->pos() - m_lastPos;
    m_lastPos = event->pos();
    if (!m_view) return;
    if (m_orbiting) {
        if (m_orbitOnly) {
            m_view->Orbit(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
        } else {
            m_view->Look(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
        }
    } else if (m_panning) {
        m_view->Pan(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
    }
}

void Viewport::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) m_orbiting = false;
    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        m_panning = false;
    }
    if (!m_orbiting && !m_panning) unsetCursor();
}

void Viewport::wheelEvent(QWheelEvent* event) {
    if (m_view) {
        m_view->Zoom(static_cast<float>(event->angleDelta().y()) / 120.0f);
        Q_EMIT fieldOfViewChanged(static_cast<double>(m_view->cameraState().fovDeg));
    }
    event->accept();
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) resetCamera();
}

void Viewport::keyPressEvent(QKeyEvent* event) {
    // WASD/QE only fly the camera in Fly mode; in Orbit mode they must bubble up
    // so the window shortcuts (S = single step, R = restart, ...) still work.
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
    QOpenGLWidget::keyPressEvent(event);
}

void Viewport::keyReleaseEvent(QKeyEvent* event) {
    m_keys.remove(event->key());
    QOpenGLWidget::keyReleaseEvent(event);
}

void Viewport::resizeEvent(QResizeEvent* event) {
    QOpenGLWidget::resizeEvent(event);
    if (m_border) m_border->setGeometry(rect());
    layoutHud();
}

void Viewport::layoutHud() {
    if (!m_hud) return;
    m_hud->adjustSize();
    m_hud->move(12, 12);
}

void Viewport::resetCamera() {
    if (m_view) m_view->ResetCamera();
    Q_EMIT cameraReset();
}

void Viewport::setViewMode(int mode) {
    m_viewMode = std::clamp(mode, 0, 2);
}

void Viewport::setOrbitOnly(bool orbitOnly) {
    if (m_orbitOnly == orbitOnly) return;
    m_orbitOnly = orbitOnly;
    if (m_orbitOnly) m_keys.clear();
}

void Viewport::setHudText(const QString& text) {
    if (!m_hud) return;
    m_hud->setText(text);
    layoutHud();
}

void Viewport::setHudVisible(bool visible) {
    if (m_hud) m_hud->setVisible(visible);
}

bool Viewport::saveFrame(const QString& path) {
    if (!m_initialised) return false;
    makeCurrent();
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    const bool ok = m_view->ReadPixels(rgba, w, h);
    doneCurrent();
    if (!ok || w <= 0 || h <= 0) return false;
    QImage image(rgba.data(), w, h, w * 4, QImage::Format_RGBA8888);
    return image.copy().save(path, "PNG");
}

}  // namespace ui
