#include "Ui/Qml/ViewportItem.hpp"

#include <QCursor>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QTimer>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

#include "Render/GlView.hpp"
#include "Ui/Theme.hpp"

namespace ui {
namespace {

void* QtGlLoader(const char* name) {
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (!ctx) return nullptr;
    return reinterpret_cast<void*>(ctx->getProcAddress(name));
}

// The session's model snapshot getter landed under two names during the
// parallel v2 work (`modelsForRender()` in the task brief, `models()` in
// docs/DEV_CONTRACT_V2.md). Accept whichever exists so this file compiles
// against either implementation; nothing else about the contract changes.
template <typename T, typename = void>
struct HasModelsForRender : std::false_type {};
template <typename T>
struct HasModelsForRender<
    T, std::void_t<decltype(std::declval<const T&>().modelsForRender())>>
    : std::true_type {};

template <typename T, typename = void>
struct HasModelsGetter : std::false_type {};
template <typename T>
struct HasModelsGetter<T, std::void_t<decltype(std::declval<const T&>().models())>>
    : std::true_type {};

template <typename T>
ModelSnapshotPtr modelsFromSession(const T* session) {
    if (!session) return nullptr;
    if constexpr (HasModelsForRender<T>::value) {
        return session->modelsForRender();
    } else if constexpr (HasModelsGetter<T>::value) {
        return session->models();
    } else {
        return nullptr;   // no snapshot API yet: see the build report
    }
}

// Q_INVOKABLE dispatch: the exact setter names are owned by the session group,
// so look them up at runtime instead of hard-coding a signature into the build.
void pushModelPosition(SimulationSession* session, int id, const Math::Vector3& p) {
    if (!session || id < 0) return;
    const QMetaObject* mo = session->metaObject();
    if (mo->indexOfMethod("setModelPosition(int,double,double,double)") < 0) return;
    QMetaObject::invokeMethod(session, "setModelPosition", Qt::AutoConnection,
                              Q_ARG(int, id), Q_ARG(double, p.x),
                              Q_ARG(double, p.y), Q_ARG(double, p.z));
}

void pushModelRotation(SimulationSession* session, int id,
                       const Math::Quaternions& q) {
    if (!session || id < 0) return;
    const QMetaObject* mo = session->metaObject();
    const char* name = nullptr;
    if (mo->indexOfMethod("setModelQuaternion(int,double,double,double,double)") >= 0) {
        name = "setModelQuaternion";
    } else if (mo->indexOfMethod(
                   "setModelRotationQuaternion(int,double,double,double,double)") >= 0) {
        name = "setModelRotationQuaternion";
    }
    if (!name) return;
    QMetaObject::invokeMethod(session, name, Qt::AutoConnection,
                              Q_ARG(int, id), Q_ARG(double, q.w), Q_ARG(double, q.x),
                              Q_ARG(double, q.y), Q_ARG(double, q.z));
}

class ViewportRenderer : public QQuickFramebufferObject::Renderer {
public:
    ~ViewportRenderer() override = default;

    void synchronize(QQuickFramebufferObject* item) override {
        auto* viewport = static_cast<ViewportItem*>(item);
        if (viewport->session() != m_session) m_session = viewport->session();
        m_viewMode = viewport->viewMode();
        m_camera = viewport->cameraState();
        m_models = viewport->modelSnapshot();
        m_selectedId = viewport->selectedModelId();
        m_scene = viewport->sceneIndex();

        if (m_session) {
            SimSnapshotPtr snap = m_session->snapshot();
            if (snap != m_snapshot) m_snapshot = snap;
            SurfaceSnapshotPtr surf = m_session->surface();
            if (surf != m_surface) m_surface = surf;
            ModelDataPtr model = m_session->model();
            if (model != m_model) m_model = model;
        }
        const QColor bg = Theme::instance().viewportBackground();
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
        }
        // Tank geometry follows the session scene, not the particle snapshot:
        // the empty scene has no fluid snapshot at all but still shares the
        // renderer.
        if (m_scene != m_tankScene) {
            if (m_scene == 1) {
                m_view->SetTank(1.0f, 1.0f, 1.0f, 1.0f, 0.5f, true);
            } else {
                m_view->SetTank(2.0f, 1.2f, 1.0f, 0.0f, 0.0f, true);
            }
            m_tankScene = m_scene;
        }
        if (m_surface != m_uploadedSurface) {
            if (m_surface) m_view->UpdateSurface(m_surface->vertices, m_surface->indices);
            else m_view->UpdateSurface({}, {});
            m_uploadedSurface = m_surface;
        }

        // v2 multi-model feed (Empty scene). The legacy single-model path stays
        // for dam/inflow so the existing import flow keeps working; an empty
        // snapshot must not shadow it.
        const bool useModels = (m_scene == 2) || (m_models && !m_models->empty());
        if (useModels) {
            // Drop the legacy single-model feed first: UpdateModel(null) would
            // reset OpenGLLayer's model list, so it must precede UpdateModels.
            if (m_uploadedModel) {
                m_view->UpdateModel(ModelDataPtr());
                m_uploadedModel.reset();
            }
            if (m_models != m_uploadedModels || m_selectedId != m_uploadedSelected) {
                m_view->UpdateModels(m_models, m_selectedId);
                m_uploadedModels = m_models;
                m_uploadedSelected = m_selectedId;
            }
        } else {
            // Leaving the model list behind: clear its GPU buffers so dam /
            // inflow do not keep drawing the Empty scene's assets.
            if (m_uploadedModels) {
                m_view->UpdateModels(ModelSnapshotPtr(), -1);
                m_uploadedModels.reset();
                m_uploadedSelected = -1;
            }
            if (m_model != m_uploadedModel) {
                m_view->UpdateModel(m_model);
                m_uploadedModel = m_model;
            }
        }

        m_view->SetSceneIndex(m_scene);
        m_view->SetBackground(m_bg);
        m_view->SetSurfaceVisible(m_scene != 2 && m_viewMode > 0);
        m_view->SetParticlesVisible(m_scene != 2 && m_viewMode != 1);
        m_view->cameraState() = m_camera;

        m_view->Render();
        m_view->BlitTo(static_cast<uint32_t>(fbo->handle()), w, h,
                       m_bg[0], m_bg[1], m_bg[2]);
        m_triangles.store(m_view->TriangleCount());
    }

private:
    std::unique_ptr<render::GlView> m_view;
    SimulationSession* m_session = nullptr;
    SimSnapshotPtr m_snapshot;
    SimSnapshotPtr m_uploadedSnapshot;
    SurfaceSnapshotPtr m_surface;
    SurfaceSnapshotPtr m_uploadedSurface;
    ModelDataPtr m_model;
    ModelDataPtr m_uploadedModel;
    ModelSnapshotPtr m_models;
    ModelSnapshotPtr m_uploadedModels;
    int m_selectedId = -1;
    int m_uploadedSelected = -1;
    render::CameraState m_camera;
    int m_viewMode = 0;
    int m_scene = 0;
    int m_tankScene = -1;
    float m_bg[3] = {0.12f, 0.15f, 0.22f};
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

    // The viewport clear colour is user-editable and lives on Theme.
    connect(&Theme::instance(), &Theme::changed, this, [this]() { update(); });

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
    if (m_paramsConnection) QObject::disconnect(m_paramsConnection);
    endGizmoDrag();
    m_session = session;
    m_models.reset();
    m_selectedId = -1;
    m_sceneIndex = -1;
    if (m_session) {
        m_paramsConnection = connect(m_session, &SimulationSession::paramsChanged, this,
                                     [this]() { refreshSessionState(); });
    }
    Q_EMIT sessionChanged();
    refreshSessionState();
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

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------
render::CameraState ViewportItem::emptySceneCamera() {
    // Fixed Empty-scene view: eye (2.9, 2.3, 2.9), target (1, 0.55, 0.5),
    // fov 42 — no fit-to-model. Stored as yaw/pitch/distance because that is
    // what the camera rig (and the render config) speak.
    const Math::Vector3 eye(2.9f, 2.3f, 2.9f);
    const Math::Vector3 target(1.0f, 0.55f, 0.5f);
    const Math::Vector3 offset = target - eye;
    const float distance = offset.length();
    const Math::Vector3 front =
        distance > 1.0e-6f ? offset * (1.0f / distance) : Math::Vector3(0.0f, 0.0f, -1.0f);
    constexpr float kRadToDeg = 57.29577951308232f;

    render::CameraState state;
    state.distance = distance;
    state.yawDeg = std::atan2(front.z, front.x) * kRadToDeg;
    state.pitchDeg = std::asin(std::clamp(front.y, -1.0f, 1.0f)) * kRadToDeg;
    state.targetX = target.x;
    state.targetY = target.y;
    state.targetZ = target.z;
    state.fovDeg = 42.0f;
    return state;
}

void ViewportItem::applySceneCamera() {
    m_camera = m_sceneIndex == 2 ? emptySceneCamera() : render::CameraState{};
    Q_EMIT fieldOfViewChanged();
    update();
}

void ViewportItem::resetCamera() {
    applySceneCamera();
}

QVariantMap ViewportItem::currentCamera() const {
    QVariantMap cfg;
    cfg.insert(QStringLiteral("yaw"), m_camera.yawDeg);
    cfg.insert(QStringLiteral("pitch"), m_camera.pitchDeg);
    cfg.insert(QStringLiteral("distance"), m_camera.distance);
    cfg.insert(QStringLiteral("targetX"), m_camera.targetX);
    cfg.insert(QStringLiteral("targetY"), m_camera.targetY);
    cfg.insert(QStringLiteral("targetZ"), m_camera.targetZ);
    cfg.insert(QStringLiteral("fov"), m_camera.fovDeg);
    return cfg;
}

void ViewportItem::applyCameraPreset(const QString& name) {
    m_camera = render::ApplyViewPreset(render::ViewPresetFromName(name.toStdString()),
                                       m_camera);
    render::ClampCamera(m_camera);
    Q_EMIT fieldOfViewChanged();
    update();
}

void ViewportItem::setCamera(const QVariantMap& config) {
    render::CameraState next = m_camera;
    auto readFinite = [&config](const char* key, float& out) {
        const auto it = config.constFind(QString::fromLatin1(key));
        if (it == config.constEnd()) return;
        bool ok = false;
        const double value = it->toDouble(&ok);
        if (ok && std::isfinite(value)) out = static_cast<float>(value);
    };
    readFinite("yaw", next.yawDeg);
    readFinite("pitch", next.pitchDeg);
    readFinite("distance", next.distance);
    readFinite("targetX", next.targetX);
    readFinite("targetY", next.targetY);
    readFinite("targetZ", next.targetZ);
    readFinite("fov", next.fovDeg);
    render::ClampCamera(next);
    m_camera = next;
    Q_EMIT fieldOfViewChanged();
    update();
}

// ---------------------------------------------------------------------------
// Session mirror
// ---------------------------------------------------------------------------
int ViewportItem::querySceneIndex() const {
    if (!m_session) return -1;
    const QVariant value = m_session->property("sceneIndex");
    if (value.isValid()) return value.toInt();
    // Until the v2 Q_PROPERTY exists, the params map is authoritative.
    return m_session->params().value(QStringLiteral("scene")).toInt();
}

int ViewportItem::querySelectedModelId() const {
    if (!m_session) return -1;
    const QVariant value = m_session->property("selectedModelId");
    return value.isValid() ? value.toInt() : -1;
}

void ViewportItem::refreshSessionState() {
    const int scene = querySceneIndex();
    const int selected = querySelectedModelId();
    const ModelSnapshotPtr models = modelsFromSession(m_session);

    bool dirty = false;
    if (scene != m_sceneIndex) {
        m_sceneIndex = scene;
        endGizmoDrag();
        m_orbiting = false;
        m_panning = false;
        // Each scene has its own fixed default view; switching never keeps the
        // previous scene's framing.
        applySceneCamera();
        Q_EMIT sceneIndexChanged();
        dirty = true;
    }
    if (selected != m_selectedId) {
        m_selectedId = selected;
        Q_EMIT selectionChanged();
        dirty = true;
    }
    if (models != m_models) {
        m_models = models;
        dirty = true;
    }
    if (dirty) update();
}

void ViewportItem::selectModelById(int id) {
    if (!m_session) return;
    if (m_session->metaObject()->indexOfMethod("selectModel(int)") >= 0) {
        QMetaObject::invokeMethod(m_session, "selectModel", Qt::AutoConnection,
                                  Q_ARG(int, id));
    }
    update();
}

// ---------------------------------------------------------------------------
// Picking
// ---------------------------------------------------------------------------
void ViewportItem::updatePickView() const {
    m_pickView.cameraState() = m_camera;
    const int w = std::max(1, static_cast<int>(std::lround(width())));
    const int h = std::max(1, static_cast<int>(std::lround(height())));
    m_pickView.Resize(w, h);
}

bool ViewportItem::screenRay(double nx, double ny, Math::Ray& ray) const {
    if (!std::isfinite(nx) || !std::isfinite(ny)) return false;
    updatePickView();
    const float ndcX = static_cast<float>(2.0 * nx - 1.0);
    const float ndcY = static_cast<float>(1.0 - 2.0 * ny);   // item y is top-down
    ray = Math::MakeScreenRay(ndcX, ndcY, m_pickView.ViewMatrix(),
                              m_pickView.ProjectionMatrix(), m_pickView.CameraEye());
    return true;
}

const ModelInstance* ViewportItem::selectedInstance() const {
    if (!m_models || m_selectedId < 0) return nullptr;
    for (const auto& inst : *m_models) {
        if (inst && inst->id == m_selectedId && inst->visible) return inst.get();
    }
    return nullptr;
}

int ViewportItem::pickModel(double nx, double ny) {
    if (!m_models || m_models->empty()) return -1;
    updatePickView();
    return m_pickView.PickModel(nx, ny, m_models);
}

int ViewportItem::pickGizmo(double nx, double ny) {
    const ModelInstance* inst = selectedInstance();
    if (!inst) return 0;
    updatePickView();
    return m_pickView.PickGizmo(nx, ny, inst);
}

Math::Vector3 ViewportItem::axisForHandle(int handle) {
    switch ((handle - 1) % 3) {
        case 0: return Math::Vector3(1.0f, 0.0f, 0.0f);
        case 1: return Math::Vector3(0.0f, 1.0f, 0.0f);
        default: return Math::Vector3(0.0f, 0.0f, 1.0f);
    }
}

// Closest point between the screen ray and the infinite world axis line
// origin + s*axis. `param` is the axis parameter s (axis is unit length).
bool ViewportItem::axisParam(const Math::Ray& ray, const Math::Vector3& origin,
                             const Math::Vector3& axis, float& param) {
    const Math::Vector3 w0 = origin - ray.origin;
    const float b = axis.dot(ray.direction);
    const float denom = 1.0f - b * b;   // both directions are unit length
    if (std::abs(denom) < 1.0e-5f) return false;   // ray parallel to the axis
    const float d = axis.dot(w0);
    const float e = ray.direction.dot(w0);
    const float s = (b * e - d) / denom;
    if (!std::isfinite(s)) return false;
    param = s;
    return true;
}

// Ray/plane intersection; the plane passes through `origin` with `normal`.
bool ViewportItem::planeHit(const Math::Ray& ray, const Math::Vector3& origin,
                            const Math::Vector3& normal, Math::Vector3& point) {
    const float denom = ray.direction.dot(normal);
    if (std::abs(denom) < 1.0e-6f) return false;   // ray parallel to the plane
    const float t = (origin - ray.origin).dot(normal) / denom;
    if (!std::isfinite(t) || t <= 0.0f) return false;
    point = ray.origin + ray.direction * t;
    return true;
}

void ViewportItem::beginGizmoDrag(int handle, double nx, double ny) {
    endGizmoDrag();
    if (m_sceneIndex != 2 || handle < 1 || handle > 6) return;
    const ModelInstance* inst = selectedInstance();
    if (!inst) return;

    Math::Ray ray;
    if (!screenRay(nx, ny, ray)) return;

    m_gizmoHandle = handle;
    m_gizmoModelId = inst->id;
    m_dragAxis = axisForHandle(handle);
    m_dragOrigin = inst->position;
    m_dragPosition0 = inst->position;
    m_dragRotation0 = inst->rotation;

    if (m_gizmoHandle <= 3) {
        float param = 0.0f;
        if (!axisParam(ray, m_dragOrigin, m_dragAxis, param)) {
            m_gizmoHandle = 0;
            m_gizmoModelId = -1;
            return;
        }
        m_dragAxisParam0 = param;
    } else {
        Math::Vector3 hit;
        if (!planeHit(ray, m_dragOrigin, m_dragAxis, hit)) {
            m_gizmoHandle = 0;
            m_gizmoModelId = -1;
            return;
        }
        const Math::Vector3 v = hit - m_dragOrigin;
        if (v.length() < 1.0e-5f) {
            m_gizmoHandle = 0;
            m_gizmoModelId = -1;
            return;
        }
        m_dragRefDir = v.normalize();
    }
    setCursor(QCursor(Qt::ClosedHandCursor));
    update();
}

void ViewportItem::updateGizmoDrag(double nx, double ny) {
    if (!m_gizmoHandle || m_gizmoModelId < 0 || !m_session) return;
    Math::Ray ray;
    if (!screenRay(nx, ny, ray)) return;

    if (m_gizmoHandle <= 3) {
        // Translation: slide along the world axis, no snapping.
        float param = 0.0f;
        if (!axisParam(ray, m_dragOrigin, m_dragAxis, param)) return;
        const float delta = param - m_dragAxisParam0;
        if (!std::isfinite(delta)) return;
        pushModelPosition(m_session, m_gizmoModelId, m_dragPosition0 + m_dragAxis * delta);
    } else {
        // Rotation: angle swept on the plane perpendicular to the world axis,
        // applied as a delta quaternion premultiplied on the drag-start pose.
        Math::Vector3 hit;
        if (!planeHit(ray, m_dragOrigin, m_dragAxis, hit)) return;
        Math::Vector3 v = hit - m_dragOrigin;
        if (v.length() < 1.0e-5f) return;
        v = v.normalize();
        const float c = std::clamp(m_dragRefDir.dot(v), -1.0f, 1.0f);
        const float s = m_dragAxis.dot(m_dragRefDir.cross(v));
        const float angle = std::atan2(s, c);
        if (!std::isfinite(angle)) return;
        const Math::Quaternions delta = Math::Quaternions::FromAxisAngle(m_dragAxis, angle);
        const Math::Quaternions pose = (delta * m_dragRotation0).normalized();
        pushModelRotation(m_session, m_gizmoModelId, pose);
    }
    // Pull the republished snapshot right away so the next frame draws the new
    // transform instead of lagging a tick behind.
    refreshSessionState();
    update();
}

void ViewportItem::endGizmoDrag() {
    m_gizmoHandle = 0;
    m_gizmoModelId = -1;
    m_dragAxisParam0 = 0.0f;
}

// ---------------------------------------------------------------------------
// Frame tick / fly controls
// ---------------------------------------------------------------------------
void ViewportItem::onTick() {
    refreshSessionState();
    applyFlyStep();
    update();
}

void ViewportItem::setCameraSpeed(qreal speed) {
    if (!std::isfinite(static_cast<double>(speed))) return;
    const qreal clamped = std::clamp<qreal>(speed, 0.1, 4.0);
    if (qFuzzyCompare(clamped, m_cameraSpeed)) return;
    m_cameraSpeed = clamped;
    Q_EMIT cameraSpeedChanged();
}

// Frames every visible model so an import at true scale (a 20 mm bracket in a
// 2 m tank) fills the viewport instead of landing as a speck at the origin.
void ViewportItem::frameModels() {
    if (!m_session) return;
    const ModelSnapshotPtr snapshot = m_session->modelsForRender();
    if (!snapshot || snapshot->empty()) return;

    Math::Vector3 lower(1.0e30f, 1.0e30f, 1.0e30f);
    Math::Vector3 upper(-1.0e30f, -1.0e30f, -1.0e30f);
    bool any = false;
    for (const ModelInstancePtr& instance : *snapshot) {
        if (!instance || !instance->visible || !instance->data) continue;
        const Math::Matrix4 transform = instance->matrix();
        const Math::Vector3& a = instance->data->boundsMin;
        const Math::Vector3& b = instance->data->boundsMax;
        for (int corner = 0; corner < 8; ++corner) {
            const Math::Vector3 local((corner & 1) ? b.x : a.x,
                                      (corner & 2) ? b.y : a.y,
                                      (corner & 4) ? b.z : a.z);
            const Math::Vector3 world = transform.TransformPoint(local);
            lower.x = std::min(lower.x, world.x);
            lower.y = std::min(lower.y, world.y);
            lower.z = std::min(lower.z, world.z);
            upper.x = std::max(upper.x, world.x);
            upper.y = std::max(upper.y, world.y);
            upper.z = std::max(upper.z, world.z);
        }
        any = true;
    }
    if (!any) return;

    const Math::Vector3 centre = (lower + upper) * 0.5f;
    float radius = (upper - lower).length() * 0.5f;
    if (!(radius > 1.0e-5f)) radius = 1.0e-3f;

    const float halfFov = m_camera.fovDeg * 0.5f * 3.14159265358979f / 180.0f;
    const float sine = std::max(0.05f, std::sin(halfFov));
    m_camera.targetX = centre.x;
    m_camera.targetY = centre.y;
    m_camera.targetZ = centre.z;
    m_camera.distance = std::clamp(radius / sine * 1.15f, 0.06f, 500.0f);
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
    render::FlyCamera(m_camera, forward, strafe, up,
                      static_cast<float>(0.015 * m_cameraSpeed));
}

// ---------------------------------------------------------------------------
// Mouse / keyboard
// ---------------------------------------------------------------------------
void ViewportItem::mousePressEvent(QMouseEvent* event) {
    forceActiveFocus();
    m_lastPos = event->position().toPoint();

    if (event->button() == Qt::LeftButton) {
        const double nx = width() > 0 ? event->position().x() / width() : 0.0;
        const double ny = height() > 0 ? event->position().y() / height() : 0.0;

        // Selection / gizmo editing exists only in the Empty scene; dam and
        // inflow keep the plain orbit behaviour.
        if (m_sceneIndex == 2) {
            const int handle = pickGizmo(nx, ny);
            if (handle != 0) {
                beginGizmoDrag(handle, nx, ny);
                if (m_gizmoHandle != 0) {
                    event->accept();
                    return;
                }
            }
            const int id = pickModel(nx, ny);
            selectModelById(id);   // -1 on blank: deselect
        }

        // A press on a model body selects it; only an actual drag orbits.
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

    if (m_gizmoHandle != 0) {
        const double nx = width() > 0 ? event->position().x() / width() : 0.0;
        const double ny = height() > 0 ? event->position().y() / height() : 0.0;
        updateGizmoDrag(nx, ny);
        event->accept();
        return;
    }

    if (m_orbiting) {
        const float dx = static_cast<float>(delta.x() * m_cameraSpeed);
        const float dy = static_cast<float>(delta.y() * m_cameraSpeed);
        if (m_orbitOnly) {
            render::OrbitCamera(m_camera, dx, dy);
        } else {
            render::LookCamera(m_camera, dx, dy);
        }
        update();
    } else if (m_panning) {
        render::PanCamera(m_camera, static_cast<float>(delta.x() * m_cameraSpeed),
                          static_cast<float>(delta.y() * m_cameraSpeed),
                          static_cast<int>(height()));
        update();
    }
    event->accept();
}

void ViewportItem::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_orbiting = false;
        if (m_gizmoHandle != 0) endGizmoDrag();
    }
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
    if (event->key() == Qt::Key_Escape) {
        if (m_gizmoHandle != 0) endGizmoDrag();
        else if (m_sceneIndex == 2) selectModelById(-1);
        setCursor(QCursor(Qt::OpenHandCursor));
        event->accept();
        return;
    }

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
