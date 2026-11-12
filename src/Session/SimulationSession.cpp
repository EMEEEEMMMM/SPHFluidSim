#include "Session/SimulationSession.hpp"

#include <QDateTime>
#include <QVector3D>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QColor>
#include <QFile>
#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "FluidSolver.hpp"
#include "Render/CameraRig.hpp"
#include "SolverFactory.hpp"
#include "SurfaceReconstructor.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int   kTraceLimit = 360;        // ~6 s at 60 steps/s
constexpr int   kStatusIntervalMs = 45;   // ~22 Hz UI refresh
constexpr int   kTraceIntervalMs = 120;
constexpr float kMaxStepsPerSecond = 240.0f;

struct TankInfo {
    float x, y, z, cx, cz;
    bool openTop;
};

TankInfo TankForScene(int scene) {
    if (scene == 1) return {1.0f, 1.0f, 1.0f, 1.0f, 0.5f, true};
    return {2.0f, 1.2f, 1.0f, 0.0f, 0.0f, true};
}

// The scenes are defined by a fixed water volume; the caller picks the particle
// count and the spacing follows from it. 0.512 m^3 = the 0.8 m dam-break cube,
// 0.6 m^3 = the 60 %-filled inflow tank.
float WaterVolumeForScene(int scene) {
    return scene == 1 ? 0.6f : 0.512f;   // matches the solver's own scene setup
}

constexpr int kMinParticles = 512;

// There is deliberately no application-level upper cap. Large scenes are
// rejected by the active backend's budget check (CUDA) or by the machine's
// available memory (CPU), not by an arbitrary UI number.
int ClampParticleCount(int count) {
    return std::max(count, kMinParticles);
}

float SpacingForCount(int scene, int count) {
    const float volume = WaterVolumeForScene(scene);
    return std::cbrt(volume / static_cast<float>(std::max(1, count)));
}

int CountForSpacing(int scene, float spacing) {
    const double s = std::max(1.0e-4, static_cast<double>(spacing));
    const double volume = static_cast<double>(WaterVolumeForScene(scene));
    return ClampParticleCount(static_cast<int>(std::llround(volume / (s * s * s))));
}

void SetupScene(IFluidSolver& solver, int scene, int particleCount) {
    // Empty is a valid target for every backend and has no particle budget:
    // there is nothing to allocate, so never ask for one.
    if (scene == 2) {
        solver.InitEmpty();
        return;
    }
    const int count = ClampParticleCount(particleCount);
    const float spacing = SpacingForCount(scene, count);
    // Refuse an impossible scene before touching the running one. CUDA checks
    // the device budget here; the CPU backend accepts everything.
    solver.CheckParticleBudget(count);
    solver.GetParams().spacing = spacing;
    if (scene == 0) {
        solver.InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);
    } else {
        solver.InitInflow(1.0f, 0.3f, 100, 0.08f, 1.0f, 0.5f, count);
    }
}

// ---- v2 scene / object helpers -------------------------------------------

constexpr int kSceneDam = 0;
constexpr int kSceneInflow = 1;
constexpr int kSceneEmpty = 2;

int ClampScene(int scene) { return std::clamp(scene, kSceneDam, kSceneEmpty); }

int SceneFromName(const QString& name) {
    if (name.compare(QStringLiteral("inflow"), Qt::CaseInsensitive) == 0) return kSceneInflow;
    if (name.compare(QStringLiteral("empty"), Qt::CaseInsensitive) == 0) return kSceneEmpty;
    return kSceneDam;
}

QString SceneName(int scene) {
    if (scene == kSceneInflow) return QStringLiteral("inflow");
    if (scene == kSceneEmpty) return QStringLiteral("empty");
    return QStringLiteral("dam");
}

// Default framing per scene. Empty keeps the dam target so a newly authored
// object sits in front of the camera without any auto-fit.
render::CameraState DefaultCameraForScene(int scene) {
    render::CameraState cam;
    cam.yawDeg = -128.35f;
    cam.pitchDeg = -29.77f;
    cam.distance = 3.526f;
    cam.targetX = 1.0f;
    cam.targetY = scene == kSceneInflow ? 0.5f : 0.55f;
    cam.targetZ = 0.5f;
    cam.fovDeg = 42.0f;
    return cam;
}

bool Finite(double v) { return std::isfinite(v); }

QVariantList ToList(const Math::Vector3& v) {
    return QVariantList{v.x, v.y, v.z};
}

QVariantList ToList(const Math::Quaternions& q) {
    return QVariantList{q.w, q.x, q.y, q.z};
}

// Wrap to [-180, 180] so Euler values round-trip readably and never grow.
float WrapDegrees(double degrees) {
    if (!Finite(degrees)) return 0.0f;
    double wrapped = std::fmod(degrees + 180.0, 360.0);
    if (wrapped < 0.0) wrapped += 360.0;
    return static_cast<float>(wrapped - 180.0);
}

QString CanonicalHex(const QString& hex) {
    const QColor color(hex);
    if (!color.isValid()) return {};
    return color.name(QColor::HexRgb).toUpper();
}

std::string UniqueModelName(const QString& stem,
                            const std::vector<std::string>& existing) {
    const auto taken = [&existing](const QString& candidate) {
        return std::any_of(existing.begin(), existing.end(), [&](const std::string& n) {
            return QString::fromStdString(n) == candidate;
        });
    };
    if (!taken(stem)) return stem.toStdString();
    for (int suffix = 2; suffix < 100000; ++suffix) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(stem).arg(suffix);
        if (!taken(candidate)) return candidate.toStdString();
    }
    return stem.toStdString();
}

void ApplySingleParam(FluidParams& p, const QString& key, const QVariant& v) {
    const float f = v.toFloat();
    if (key == QLatin1String("omega")) p.omega = f;
    else if (key == QLatin1String("eta")) p.eta = f;
    else if (key == QLatin1String("etaV")) p.etaV = f;
    else if (key == QLatin1String("maxIter")) p.maxIter = v.toInt();
    else if (key == QLatin1String("dt")) p.dt = f;
    else if (key == QLatin1String("cfl")) p.useCFL = v.toBool();
    else if (key == QLatin1String("viscosity")) p.viscosity = f;
    else if (key == QLatin1String("surfaceTension")) p.surfaceTension = f;
    else if (key == QLatin1String("wallAdhesion")) p.surfaceTensionBoundary = f;
    else if (key == QLatin1String("solver")) p.solver = v.toInt();
    else if (key == QLatin1String("spacing")) p.spacing = f;
}

void ApplyParamsToSolver(IFluidSolver& solver, const QVariantMap& m) {
    auto& p = solver.GetParams();
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        ApplySingleParam(p, it.key(), it.value());
    }
}

QVariantMap DefaultParams() {
    FluidSolver probe;
    const FluidSolver::Params p = probe.GetParams();
    const int defaultCount = CountForSpacing(0, p.spacing);
    QVariantMap m;
    // v2 default scene is Empty; models can only be imported there.
    m.insert(QStringLiteral("scene"), 2);
    m.insert(QStringLiteral("particleCount"), defaultCount);
    m.insert(QStringLiteral("spacing"), p.spacing);
    m.insert(QStringLiteral("solver"), p.solver);
    m.insert(QStringLiteral("backend"), QStringLiteral("auto"));
    m.insert(QStringLiteral("viewMode"), 0);
    m.insert(QStringLiteral("speed"), 60);
    m.insert(QStringLiteral("omega"), p.omega);
    m.insert(QStringLiteral("eta"), p.eta);
    m.insert(QStringLiteral("etaV"), p.etaV);
    m.insert(QStringLiteral("maxIter"), p.maxIter);
    m.insert(QStringLiteral("dt"), p.dt);
    m.insert(QStringLiteral("cfl"), p.useCFL);
    m.insert(QStringLiteral("viscosity"), p.viscosity);
    m.insert(QStringLiteral("surfaceTension"), p.surfaceTension);
    m.insert(QStringLiteral("wallAdhesion"), p.surfaceTensionBoundary);
    m.insert(QStringLiteral("surfaceThreshold"), 0.8);
    m.insert(QStringLiteral("gridScale"), 0.5);
    m.insert(QStringLiteral("reconInterval"), 3);
    m.insert(QStringLiteral("viewportBackground"), QStringLiteral("#1F2638"));
    m.insert(QStringLiteral("dropRadius"), 0.2);
    m.insert(QStringLiteral("dropX"), 1.0);
    m.insert(QStringLiteral("dropY"), 1.6);
    m.insert(QStringLiteral("dropZ"), 0.5);
    return m;
}

}  // namespace

struct SimulationSession::Impl {
    SimulationSession* q = nullptr;

    // ---- simulation thread ------------------------------------------------
    std::thread worker;
    std::atomic<bool> quit{false};
    std::atomic<bool> running{false};
    std::atomic<int> stepRequests{0};
    std::atomic<int> speed{60};
    std::atomic<int> viewMode{0};
    std::condition_variable cmdCv;
    std::mutex cmdMutex;
    std::deque<std::function<void(IFluidSolver&)>> commands;

    // ---- surface reconstruction thread ------------------------------------
    std::thread recon;
    std::mutex reconMutex;
    std::condition_variable reconCv;
    bool reconQuit = false;
    bool reconPending = false;
    std::vector<Math::Vector3> reconInput;
    std::atomic<float> reconSpacing{0.025f};
    std::atomic<float> reconThreshold{0.8f};
    std::atomic<float> reconGridScale{0.5f};

    // ---- published state (read by the renderer) ---------------------------
    mutable std::mutex pubMutex;
    SimSnapshotPtr snapshot;
    SurfaceSnapshotPtr surface;
    ModelSnapshotPtr modelSnapshot;   // immutable, rebuilt on every object edit
    uint64_t surfaceVersion = 0;

    // ---- UI copies (read by bindings) -------------------------------------
    mutable std::mutex uiMutex;
    QVariantMap params;
    QVariantMap status;
    QVariantList trace;
    struct TraceEntry {
        double simTime = 0.0;
        double density = 1000.0;   // kg/m^3
        double densityError = 0.0; // normalized signed error
        uint64_t step = 0;
    };
    std::deque<TraceEntry> traceRaw;

    // ---- UI-thread object state (guarded by stateMutex) --------------------
    struct ModelEntry {
        int id = 0;
        QString name;
        QString path;            // always absolute
        ModelDataPtr data;
        Math::Vector3 position{0, 0, 0};
        Math::Quaternions rotation;   // identity
        float scale = 1.0f;
        bool visible = true;
    };
    mutable std::mutex stateMutex;
    std::vector<ModelEntry> modelEntries;
    int nextModelId = 1;
    int selectedId = -1;
    render::CameraState camera = DefaultCameraForScene(kSceneEmpty);
    QString backgroundHex = QStringLiteral("#1F2638");
    // Directory of the parameter file currently being imported; relative model
    // paths resolve against it. Empty => the process working directory.
    QString importBaseDir;

    // ---- simulation-thread local ------------------------------------------
    std::atomic<int> scene{kSceneEmpty};
    int particleCount = 32768;
    float spacing = 0.025f;
    std::atomic<int> reconInterval{3};
    int reconCounter = 0;
    // Set when a solver call threw (e.g. the GPU ran out of memory). The
    // solver must be rebuilt by a scene command before stepping again.
    bool solverBroken = false;
};

SimulationSession::SimulationSession(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>()) {
    m_impl->q = this;
    m_impl->params = DefaultParams();
    m_impl->status = QVariantMap{};
    m_impl->status.insert(QStringLiteral("simTime"), 0.0);
    m_impl->status.insert(QStringLiteral("fluidCount"), 0);
    m_impl->status.insert(QStringLiteral("boundaryCount"), 0);
    m_impl->status.insert(QStringLiteral("densityError"), 0.0);
    m_impl->status.insert(QStringLiteral("maxDensityError"), 0.0);
    m_impl->status.insert(QStringLiteral("meanDensity"), 1000.0);
    m_impl->status.insert(QStringLiteral("rho0"), 1000.0);
    m_impl->status.insert(QStringLiteral("pressureIterations"), 0);
    m_impl->status.insert(QStringLiteral("divergenceIterations"), 0);
    m_impl->status.insert(QStringLiteral("msPerStep"), 0.0);
    m_impl->status.insert(QStringLiteral("hasDrop"), false);
    m_impl->status.insert(QStringLiteral("stepCount"), 0);

    m_impl->speed.store(m_impl->params.value(QStringLiteral("speed")).toInt());
    m_impl->viewMode.store(m_impl->params.value(QStringLiteral("viewMode")).toInt());
    m_impl->scene.store(m_impl->params.value(QStringLiteral("scene")).toInt());
    m_impl->particleCount = m_impl->params.value(QStringLiteral("particleCount")).toInt();
    m_impl->spacing = SpacingForCount(m_impl->scene.load(), m_impl->particleCount);
    m_impl->reconInterval.store(m_impl->params.value(QStringLiteral("reconInterval")).toInt());
    m_impl->reconThreshold.store(m_impl->params.value(QStringLiteral("surfaceThreshold")).toFloat());
    m_impl->reconGridScale.store(m_impl->params.value(QStringLiteral("gridScale")).toFloat());
    m_impl->reconSpacing.store(m_impl->spacing);
    m_impl->camera = DefaultCameraForScene(m_impl->scene.load());
    m_impl->backgroundHex =
        CanonicalHex(m_impl->params.value(QStringLiteral("viewportBackground")).toString());
    if (m_impl->backgroundHex.isEmpty()) m_impl->backgroundHex = QStringLiteral("#1F2638");
    publishModels();
}

SimulationSession::~SimulationSession() { shutdown(); }

void SimulationSession::start() {
    if (m_impl->worker.joinable()) return;
    m_impl->quit.store(false);
    {
        std::lock_guard<std::mutex> lk(m_impl->reconMutex);
        m_impl->reconQuit = false;
    }
    m_impl->recon = std::thread([this] { reconLoop(); });
    m_impl->worker = std::thread([this] { workerLoop(); });
}

void SimulationSession::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_impl->cmdMutex);
        m_impl->quit.store(true);
    }
    m_impl->cmdCv.notify_all();
    if (m_impl->worker.joinable()) m_impl->worker.join();

    {
        std::lock_guard<std::mutex> lk(m_impl->reconMutex);
        m_impl->reconQuit = true;
    }
    m_impl->reconCv.notify_all();
    if (m_impl->recon.joinable()) m_impl->recon.join();
}

void SimulationSession::postCommand(std::function<void(IFluidSolver&)> fn) {
    {
        std::lock_guard<std::mutex> lk(m_impl->cmdMutex);
        m_impl->commands.push_back(std::move(fn));
    }
    m_impl->cmdCv.notify_all();
}

SimSnapshotPtr SimulationSession::snapshot() const {
    std::lock_guard<std::mutex> lk(m_impl->pubMutex);
    return m_impl->snapshot;
}

SurfaceSnapshotPtr SimulationSession::surface() const {
    std::lock_guard<std::mutex> lk(m_impl->pubMutex);
    return m_impl->surface;
}

ModelDataPtr SimulationSession::model() const {
    const ModelSnapshotPtr snap = models();
    if (!snap) return nullptr;
    for (const ModelInstancePtr& instance : *snap) {
        if (instance && instance->visible && instance->data) return instance->data;
    }
    return nullptr;
}

ModelSnapshotPtr SimulationSession::models() const {
    std::lock_guard<std::mutex> lk(m_impl->pubMutex);
    return m_impl->modelSnapshot;
}

ModelSnapshotPtr SimulationSession::modelsForRender() const { return models(); }

bool SimulationSession::isEmpty() const {
    return m_impl->scene.load() == kSceneEmpty;
}

int SimulationSession::sceneIndex() const { return m_impl->scene.load(); }

int SimulationSession::selectedModelId() const {
    std::lock_guard<std::mutex> lk(m_impl->stateMutex);
    return m_impl->selectedId;
}

QVariantMap SimulationSession::cameraState() const {
    render::CameraState cam;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        cam = m_impl->camera;
    }
    QVariantMap map;
    map.insert(QStringLiteral("yaw"), static_cast<double>(cam.yawDeg));
    map.insert(QStringLiteral("pitch"), static_cast<double>(cam.pitchDeg));
    map.insert(QStringLiteral("distance"), static_cast<double>(cam.distance));
    map.insert(QStringLiteral("targetX"), static_cast<double>(cam.targetX));
    map.insert(QStringLiteral("targetY"), static_cast<double>(cam.targetY));
    map.insert(QStringLiteral("targetZ"), static_cast<double>(cam.targetZ));
    map.insert(QStringLiteral("fov"), static_cast<double>(cam.fovDeg));
    return map;
}

QVariantList SimulationSession::objects() const {
    std::lock_guard<std::mutex> lk(m_impl->stateMutex);
    QVariantList list;
    list.reserve(static_cast<int>(m_impl->modelEntries.size()));
    for (const Impl::ModelEntry& entry : m_impl->modelEntries) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), entry.id);
        item.insert(QStringLiteral("name"), entry.name);
        item.insert(QStringLiteral("path"), entry.path);
        item.insert(QStringLiteral("position"), ToList(entry.position));
        item.insert(QStringLiteral("rotation"), ToList(entry.rotation));
        item.insert(QStringLiteral("rotationEuler"),
                    ToList(entry.rotation.ToEulerDegrees()));
        item.insert(QStringLiteral("scale"), static_cast<double>(entry.scale));
        item.insert(QStringLiteral("visible"), entry.visible);
        item.insert(QStringLiteral("triangleCount"),
                    entry.data ? static_cast<int>(entry.data->triangleCount) : 0);
        list.append(item);
    }
    return list;
}

QVariantList SimulationSession::modelConfigs() const {
    const ModelSnapshotPtr snap = models();
    QVariantList list;
    if (!snap) return list;
    list.reserve(static_cast<int>(snap->size()));
    for (const ModelInstancePtr& instance : *snap) {
        if (!instance) continue;
        QVariantMap item;
        item.insert(QStringLiteral("id"), instance->id);
        item.insert(QStringLiteral("name"), QString::fromStdString(instance->name));
        item.insert(QStringLiteral("position"), ToList(instance->position));
        item.insert(QStringLiteral("rotation"), ToList(instance->rotation));
        item.insert(QStringLiteral("rotationEuler"),
                    ToList(instance->rotation.ToEulerDegrees()));
        item.insert(QStringLiteral("scale"), static_cast<double>(instance->scale));
        item.insert(QStringLiteral("visible"), instance->visible);
        if (instance->data) {
            item.insert(QStringLiteral("path"),
                        QString::fromStdString(instance->data->sourcePath));
            item.insert(QStringLiteral("triangleCount"),
                        static_cast<int>(instance->data->triangleCount));
        }
        const Math::Matrix4 matrix = instance->matrix();
        const float* bytes = matrix.data();
        QVariantList transform;
        transform.reserve(16);
        for (int i = 0; i < 16; ++i) transform.append(static_cast<double>(bytes[i]));
        item.insert(QStringLiteral("transform"), transform);
        list.append(item);
    }
    return list;
}

bool SimulationSession::running() const { return m_impl->running.load(); }

QVariantMap SimulationSession::params() const {
    std::lock_guard<std::mutex> lk(m_impl->uiMutex);
    return m_impl->params;
}

QVariantMap SimulationSession::status() const {
    std::lock_guard<std::mutex> lk(m_impl->uiMutex);
    return m_impl->status;
}

QVariantList SimulationSession::densityTrace() const {
    std::lock_guard<std::mutex> lk(m_impl->uiMutex);
    return m_impl->trace;
}

QString SimulationSession::particleBudgetError(int count) const {
    const QVariantMap st = status();
    const qlonglong bytesPerParticle =
        st.value(QStringLiteral("bytesPerParticle")).toLongLong();
    const qlonglong freeBytes = st.value(QStringLiteral("vramFreeBytes")).toLongLong();
    if (count <= 0 || bytesPerParticle <= 0 || freeBytes <= 0) return {};

    const int currentCount = params().value(QStringLiteral("particleCount")).toInt();
    if (count <= currentCount) return {};

    // The current fluid buffers are reusable while rebuilding, so the free
    // figure alone would reject a straightforward resolution increase.
    const long double available = static_cast<long double>(freeBytes) +
                                  static_cast<long double>(currentCount) * bytesPerParticle;
    const long double margin = 1.12L;
    const long double scratch = 80.0L * 1024.0L * 1024.0L;
    const long double reserve = 96.0L * 1024.0L * 1024.0L;
    const long double need = static_cast<long double>(count) * bytesPerParticle * margin + scratch;
    if (need <= available) return {};

    const long double usable = std::max(0.0L, available - reserve);
    const long double fits = std::floor(usable / (bytesPerParticle * margin));
    const auto mib = [](long double bytes) {
        return QString::number(static_cast<qlonglong>(bytes / (1024.0L * 1024.0L)));
    };
    return QStringLiteral(
               "Not enough GPU memory for about %1 particles: needs about %2 MiB, "
               "only %3 MiB usable. Lower the count to about %4.")
        .arg(count)
        .arg(mib(need), mib(available), QString::number(static_cast<qlonglong>(fits)));
}

void SimulationSession::setTriangleCount(int count) {
    if (m_triangleCount == count) return;
    m_triangleCount = count;
    Q_EMIT triangleCountChanged();
}

void SimulationSession::setRunning(bool run) {
    if (m_impl->running.exchange(run) == run) return;
    m_impl->cmdCv.notify_all();
    Q_EMIT runningChanged();
}

void SimulationSession::toggleRunning() { setRunning(!m_impl->running.load()); }

void SimulationSession::stepOnce() {
    // "Step once" must leave the user paused on the next frame even when the
    // transport is currently running; otherwise the extra request is invisible
    // and the button appears inert.
    setRunning(false);
    m_impl->stepRequests.fetch_add(1);
    m_impl->cmdCv.notify_all();
}

void SimulationSession::resetSimulation() {
    // Restarting the fluid does not discard authored models: only an explicit
    // scene switch clears them. Empty reset is therefore a no-op for geometry.
    const int scene = m_impl->scene.load();
    postCommand([this, scene](IFluidSolver& solver) {
        SetupScene(solver, scene, m_impl->particleCount);
    });
    resetSurfaceAndTrace();
}

void SimulationSession::resetSurfaceAndTrace() {
    {
        std::lock_guard<std::mutex> lk(m_impl->pubMutex);
        m_impl->surface.reset();
        ++m_impl->surfaceVersion;
    }
    {
        std::lock_guard<std::mutex> lk(m_impl->uiMutex);
        m_impl->traceRaw.clear();
        m_impl->trace.clear();
    }
    Q_EMIT traceChanged();
    Q_EMIT surfaceUpdated();
}

void SimulationSession::importModel() {
    // Legacy entry point used by the Qt Widgets front-end. The QML File menu
    // hides it in fluid scenes; the widget front-end deliberately stays on the
    // pre-v2 Dam workflow and keeps its single-model import.
    const QString filePath = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("Import 3D Model"), QDir::homePath(),
        QStringLiteral("3D Model Files (*.obj *.fbx *.stl *.dae *.3ds *.ply *.off *.gltf *.glb *.step *.stp);;All Files (*)"));
    if (!filePath.isEmpty()) importModelFile(filePath);
}

bool SimulationSession::importModelFile(const QString& filePath) {
    // Legacy single-file import (also used by the widgets front-end). QML only
    // exposes this behind the Empty-scene gate; the session accepts it in any
    // scene so the older widget UI keeps working.
    if (!QFileInfo::exists(filePath)) {
        Q_EMIT message(QStringLiteral("Model import failed: file does not exist"));
        return false;
    }

    QString error;
    QString warning;
    ModelDataPtr loaded = Model::Load(filePath, &error, &warning);
    if (!loaded) {
        Q_EMIT message(QStringLiteral("Model import failed: %1").arg(error));
        return false;
    }

    const qulonglong triangleCount = static_cast<qulonglong>(loaded->indices.size() / 3);
    // Copy the bounds before `loaded` is moved into the object list below;
    // dereferencing it afterwards is a null-pointer crash.
    const Math::Vector3 extent = loaded->boundsExtent;
    const QFileInfo info(filePath);
    const QString absolutePath = info.absoluteFilePath();
    int newId = 0;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        // Soft-merge into the object list: the legacy single-model import now
        // adds one more entry instead of replacing the whole scene.
        std::vector<std::string> existing;
        existing.reserve(m_impl->modelEntries.size());
        for (const Impl::ModelEntry& entry : m_impl->modelEntries) {
            existing.push_back(entry.name.toStdString());
        }
        Impl::ModelEntry entry;
        entry.id = m_impl->nextModelId++;
        entry.name = QString::fromStdString(UniqueModelName(info.completeBaseName(), existing));
        entry.path = absolutePath;
        entry.data = std::move(loaded);
        m_impl->modelEntries.push_back(entry);
        newId = entry.id;
    }
    publishModels();
    selectModel(newId);
    QString summary = QStringLiteral("Imported %1 (%2 triangles, %3 x %4 x %5 mm)")
                          .arg(info.fileName())
                          .arg(triangleCount)
                          .arg(extent.x * 1000.0f, 0, 'f', 1)
                          .arg(extent.y * 1000.0f, 0, 'f', 1)
                          .arg(extent.z * 1000.0f, 0, 'f', 1);
    if (!warning.isEmpty()) summary += QStringLiteral(" - ") + warning;
    Q_EMIT message(summary);
    return true;
}

void SimulationSession::clearModel() {
    // Legacy "clear the imported model" entry point: the object list is the
    // single source of truth now, so this clears everything.
    clearModelsInternal(true);
}

void SimulationSession::clearModels() { clearModelsInternal(true); }

void SimulationSession::clearModelsInternal(bool announce) {
    bool hadModels = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        hadModels = !m_impl->modelEntries.empty() || m_impl->selectedId != -1;
        m_impl->modelEntries.clear();
        m_impl->selectedId = -1;
    }
    publishModels();
    Q_EMIT selectionChanged();
    if (announce && hadModels) Q_EMIT message(QStringLiteral("All models cleared"));
}

void SimulationSession::selectModel(int id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        int resolved = -1;
        if (id >= 0) {
            for (const Impl::ModelEntry& entry : m_impl->modelEntries) {
                if (entry.id == id) {
                    resolved = id;
                    break;
                }
            }
        }
        if (m_impl->selectedId != resolved) {
            m_impl->selectedId = resolved;
            changed = true;
        }
    }
    if (changed) Q_EMIT selectionChanged();
}

void SimulationSession::removeModel(int id) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        const auto it = std::find_if(m_impl->modelEntries.begin(), m_impl->modelEntries.end(),
                                     [id](const Impl::ModelEntry& e) { return e.id == id; });
        if (it != m_impl->modelEntries.end()) {
            m_impl->modelEntries.erase(it);
            removed = true;
            if (m_impl->selectedId == id) m_impl->selectedId = -1;
        }
    }
    if (!removed) return;
    publishModels();
    Q_EMIT selectionChanged();
}

// Runs `fn` on the object entry `id` on the UI thread, then republishes.
void SimulationSession::setModelPosition(int id, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        Q_EMIT message(QStringLiteral("Position must be a finite number"));
        return;
    }
    const Math::Vector3 position(static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(z));
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        for (Impl::ModelEntry& entry : m_impl->modelEntries) {
            if (entry.id != id) continue;
            entry.position = position;
            changed = true;
            break;
        }
    }
    if (changed) publishModels();
}

void SimulationSession::setModelRotationEuler(int id, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        Q_EMIT message(QStringLiteral("Rotation must be a finite number"));
        return;
    }
    const Math::Vector3 euler(WrapDegrees(x), WrapDegrees(y), WrapDegrees(z));
    const Math::Quaternions rotation = Math::Quaternions::FromEulerDegrees(euler);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        for (Impl::ModelEntry& entry : m_impl->modelEntries) {
            if (entry.id != id) continue;
            entry.rotation = rotation;
            changed = true;
            break;
        }
    }
    if (changed) publishModels();
}

void SimulationSession::setModelRotationQuaternion(int id, double w, double x, double y,
                                                   double z) {
    if (!std::isfinite(w) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        Q_EMIT message(QStringLiteral("Rotation must be a finite number"));
        return;
    }
    Math::Quaternions rotation(static_cast<float>(w), static_cast<float>(x),
                               static_cast<float>(y), static_cast<float>(z));
    const double length = std::sqrt(static_cast<double>(rotation.w) * rotation.w +
                                    static_cast<double>(rotation.x) * rotation.x +
                                    static_cast<double>(rotation.y) * rotation.y +
                                    static_cast<double>(rotation.z) * rotation.z);
    if (!(length > 1.0e-12)) rotation = Math::Quaternions();   // identity
    else rotation.normalize();
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        for (Impl::ModelEntry& entry : m_impl->modelEntries) {
            if (entry.id != id) continue;
            entry.rotation = rotation;
            changed = true;
            break;
        }
    }
    if (changed) publishModels();
}

void SimulationSession::setModelQuaternion(int id, double w, double x, double y, double z) {
    setModelRotationQuaternion(id, w, x, y, z);
}

void SimulationSession::setModelScale(int id, double scale) {
    if (!std::isfinite(scale)) {
        Q_EMIT message(QStringLiteral("Scale must be a finite number"));
        return;
    }
    const float clamped = static_cast<float>(std::clamp(scale, 0.001, 1000.0));
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        for (Impl::ModelEntry& entry : m_impl->modelEntries) {
            if (entry.id != id) continue;
            entry.scale = clamped;
            changed = true;
            break;
        }
    }
    if (changed) publishModels();
}

void SimulationSession::setModelVisible(int id, bool visible) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        for (Impl::ModelEntry& entry : m_impl->modelEntries) {
            if (entry.id != id) continue;
            if (entry.visible != visible) {
                entry.visible = visible;
                changed = true;
            }
            break;
        }
    }
    if (changed) publishModels();
}

void SimulationSession::resetModelTransform(int id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        for (Impl::ModelEntry& entry : m_impl->modelEntries) {
            if (entry.id != id) continue;
            entry.position = Math::Vector3(0, 0, 0);
            entry.rotation = Math::Quaternions();
            entry.scale = 1.0f;
            changed = true;
            break;
        }
    }
    if (changed) publishModels();
}

QVariantMap SimulationSession::dropParams() const {
    QVariantMap params;
    {
        std::lock_guard<std::mutex> lk(m_impl->uiMutex);
        params = m_impl->params;
    }
    QVariantMap drop;
    drop.insert(QStringLiteral("radius"), params.value(QStringLiteral("dropRadius"), 0.2));
    drop.insert(QStringLiteral("x"), params.value(QStringLiteral("dropX"), 1.0));
    drop.insert(QStringLiteral("y"), params.value(QStringLiteral("dropY"), 1.6));
    drop.insert(QStringLiteral("z"), params.value(QStringLiteral("dropZ"), 0.5));
    return drop;
}

void SimulationSession::setDropParams(double radius, double x, double y, double z) {
    if (!std::isfinite(radius) || radius <= 0.0 || !std::isfinite(x) ||
        !std::isfinite(y) || !std::isfinite(z)) {
        Q_EMIT message(QStringLiteral("Drop parameters must be finite (radius > 0)"));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_impl->uiMutex);
        m_impl->params.insert(QStringLiteral("dropRadius"), radius);
        m_impl->params.insert(QStringLiteral("dropX"), x);
        m_impl->params.insert(QStringLiteral("dropY"), y);
        m_impl->params.insert(QStringLiteral("dropZ"), z);
    }
    Q_EMIT paramsChanged();
}

void SimulationSession::setViewportBackgroundHex(const QString& hex) {
    const QString canonical = CanonicalHex(hex);
    if (canonical.isEmpty()) {
        Q_EMIT message(QStringLiteral("Invalid background colour: %1").arg(hex));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        if (m_impl->backgroundHex == canonical) return;
        m_impl->backgroundHex = canonical;
    }
    {
        std::lock_guard<std::mutex> lk(m_impl->uiMutex);
        m_impl->params.insert(QStringLiteral("viewportBackground"), canonical);
    }
    Q_EMIT paramsChanged();
}

void SimulationSession::setCameraConfig(const QVariantMap& config) {
    render::CameraState cam;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        cam = m_impl->camera;
    }
    const auto number = [&config](const char* key, float& out) {
        const QVariant value = config.value(QLatin1String(key));
        if (!value.isValid() || value.isNull()) return true;
        bool ok = false;
        const double d = value.toDouble(&ok);
        if (!ok || !std::isfinite(d)) return false;
        out = static_cast<float>(d);
        return true;
    };
    render::CameraState next = cam;
    if (!number("yaw", next.yawDeg) || !number("pitch", next.pitchDeg) ||
        !number("distance", next.distance) || !number("targetX", next.targetX) ||
        !number("targetY", next.targetY) || !number("targetZ", next.targetZ) ||
        !number("fov", next.fovDeg)) {
        Q_EMIT message(QStringLiteral("Camera values must be finite numbers"));
        return;
    }
    // Alternative target array form: {target: [x, y, z]}.
    const QVariant targetValue = config.value(QStringLiteral("target"));
    if (targetValue.isValid() && !targetValue.isNull()) {
        const QVariantList target = targetValue.toList();
        if (target.size() != 3) {
            Q_EMIT message(QStringLiteral("Camera target must be [x, y, z]"));
            return;
        }
        for (int i = 0; i < 3; ++i) {
            bool ok = false;
            const double d = target.at(i).toDouble(&ok);
            if (!ok || !std::isfinite(d)) {
                Q_EMIT message(QStringLiteral("Camera target must be finite"));
                return;
            }
        }
        next.targetX = static_cast<float>(target.at(0).toDouble());
        next.targetY = static_cast<float>(target.at(1).toDouble());
        next.targetZ = static_cast<float>(target.at(2).toDouble());
    }
    next.pitchDeg = std::clamp(next.pitchDeg, -89.0f, 89.0f);
    next.fovDeg = std::clamp(next.fovDeg, 20.0f, 75.0f);
    if (!(next.distance > 0.0f) || !std::isfinite(next.distance)) {
        Q_EMIT message(QStringLiteral("Camera distance must be positive"));
        return;
    }
    next.yawDeg = WrapDegrees(next.yawDeg);

    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        m_impl->camera = next;
    }
    Q_EMIT cameraStateChanged();
}

void SimulationSession::applyCameraPreset(const QString& name) {
    const render::ViewPreset preset = render::ViewPresetFromName(name.toStdString());
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        m_impl->camera = render::ApplyViewPreset(preset, m_impl->camera);
    }
    Q_EMIT cameraStateChanged();
}

void SimulationSession::useCurrentViewCamera() {
    // The session deliberately holds no viewport. Main.qml feeds the live
    // camera through setCameraConfig(); this re-publishes what we already have
    // so a binding refresh never leaves the fields stale.
    Q_EMIT cameraStateChanged();
}

void SimulationSession::spawnDrop() {
    if (m_impl->scene.load() != kSceneDam) {
        // A drop is a dam-break special effect; other scenes ignore it.
        Q_EMIT message(QStringLiteral("Drop is only available in the Dam break scene"));
        return;
    }
    const QVariantMap drop = dropParams();
    const float radius = drop.value(QStringLiteral("radius")).toFloat();
    const float x = drop.value(QStringLiteral("x")).toFloat();
    const float y = drop.value(QStringLiteral("y")).toFloat();
    const float z = drop.value(QStringLiteral("z")).toFloat();
    postCommand([radius, x, y, z](IFluidSolver& solver) {
        if (solver.HasDrop()) return;
        solver.SpawnDrop(radius, x, y, z);
    });
}

void SimulationSession::setScene(int scene) {
    scene = ClampScene(scene);
    if (m_impl->scene.load() == scene) return;

    const int previousScene = m_impl->scene.load();
    int count = 0;
    float spacing = 0.0f;
    QVariantMap previousParams;
    {
        std::lock_guard<std::mutex> lk(m_impl->uiMutex);
        previousParams = m_impl->params;
        count = ClampParticleCount(m_impl->params.value(QStringLiteral("particleCount")).toInt());
        spacing = SpacingForCount(scene, count);
        m_impl->params.insert(QStringLiteral("scene"), scene);
        m_impl->params.insert(QStringLiteral("particleCount"), count);
        m_impl->params.insert(QStringLiteral("spacing"), spacing);
    }

    // Scene switches always discard the authored objects and reframe the view.
    clearModelsInternal(false);
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        m_impl->camera = DefaultCameraForScene(scene);
    }
    m_impl->scene.store(scene);
    Q_EMIT cameraStateChanged();

    const int previousCount = previousParams.value(QStringLiteral("particleCount")).toInt();
    const float previousSpacing = previousParams.value(QStringLiteral("spacing")).toFloat();
    postCommand([this, scene, count, spacing, previousScene, previousCount,
                 previousSpacing, previousParams](IFluidSolver& solver) {
        try {
            SetupScene(solver, scene, count);
        } catch (...) {
            // The scene was refused (GPU budget): restore the previous view of
            // the world so the UI keeps matching the still-running solver.
            {
                std::lock_guard<std::mutex> lk(m_impl->uiMutex);
                m_impl->params = previousParams;
            }
            m_impl->scene.store(previousScene);
            Q_EMIT sceneChanged();
            Q_EMIT paramsChanged();
            throw;
        }
        m_impl->particleCount = count;
        m_impl->spacing = spacing;
        m_impl->reconSpacing.store(spacing);
        m_impl->solverBroken = false;
    });

    Q_EMIT sceneChanged();
    Q_EMIT paramsChanged();
    resetSurfaceAndTrace();
}

void SimulationSession::publishModels() {
    auto snapshot = std::make_shared<ModelSnapshot>();
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        snapshot->reserve(m_impl->modelEntries.size());
        for (const Impl::ModelEntry& entry : m_impl->modelEntries) {
            auto instance = std::make_shared<ModelInstance>();
            instance->id = entry.id;
            instance->name = entry.name.toStdString();
            instance->data = entry.data;
            instance->position = entry.position;
            instance->rotation = entry.rotation;
            instance->scale = entry.scale;
            instance->visible = entry.visible;
            snapshot->push_back(instance);
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_impl->pubMutex);
        m_impl->modelSnapshot = snapshot;
    }
    Q_EMIT modelChanged();
    Q_EMIT objectsChanged();
}

void SimulationSession::importModels(const QStringList& paths) {
    if (!isEmpty()) {
        Q_EMIT message(QStringLiteral("Import only in Empty scene"));
        return;
    }
    int imported = 0;
    QStringList failures;
    QStringList notes;
    bool haveExtent = false;
    QVector3D lastExtent;
    for (const QString& path : paths) {
        if (path.trimmed().isEmpty()) continue;
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            failures.append(QStringLiteral("%1 (not found)").arg(path));
            continue;
        }
        QString error;
        QString warning;
        ModelDataPtr loaded = Model::Load(path, &error, &warning);
        if (!loaded) {
            failures.append(QStringLiteral("%1 (%2)").arg(info.fileName(), error));
            continue;
        }
        if (!warning.isEmpty()) notes.append(warning);
        lastExtent = QVector3D(loaded->boundsExtent.x, loaded->boundsExtent.y,
                               loaded->boundsExtent.z);
        haveExtent = true;
        {
            std::lock_guard<std::mutex> lk(m_impl->stateMutex);
            std::vector<std::string> existing;
            existing.reserve(m_impl->modelEntries.size());
            for (const Impl::ModelEntry& entry : m_impl->modelEntries) {
                existing.push_back(entry.name.toStdString());
            }
            Impl::ModelEntry entry;
            entry.id = m_impl->nextModelId++;
            entry.name = QString::fromStdString(UniqueModelName(info.completeBaseName(), existing));
            entry.path = info.absoluteFilePath();
            entry.data = std::move(loaded);
            m_impl->modelEntries.push_back(entry);
            m_impl->selectedId = entry.id;
        }
        ++imported;
    }
    if (imported > 0) {
        publishModels();
        Q_EMIT selectionChanged();
    }
    QString summary;
    if (imported > 0) {
        summary = QStringLiteral("Imported %1 model(s)").arg(imported);
        if (haveExtent) {
            summary += QStringLiteral(" · %1 x %2 x %3 mm")
                           .arg(lastExtent.x() * 1000.0f, 0, 'f', 1)
                           .arg(lastExtent.y() * 1000.0f, 0, 'f', 1)
                           .arg(lastExtent.z() * 1000.0f, 0, 'f', 1);
        }
    }
    if (!failures.isEmpty()) {
        if (!summary.isEmpty()) summary += QStringLiteral(", ");
        summary += QStringLiteral("%1 failed: %2")
                       .arg(failures.size())
                       .arg(failures.join(QStringLiteral("; ")));
    }
    if (!notes.isEmpty()) {
        if (!summary.isEmpty()) summary += QStringLiteral(" · ");
        summary += notes.join(QStringLiteral("; "));
    }
    if (!summary.isEmpty()) Q_EMIT message(summary);
}

void SimulationSession::setParam(const QString& key, const QVariant& value) {
    if (key == QLatin1String("scene")) {
        // Scene switches go through the v2 path so the object list, the camera
        // and the sceneChanged/isEmpty notifications stay consistent.
        setScene(value.toInt());
        return;
    }
    if (key == QLatin1String("viewportBackground")) {
        setViewportBackgroundHex(value.toString());
        return;
    }
    if (key == QLatin1String("dropRadius") || key == QLatin1String("dropX") ||
        key == QLatin1String("dropY") || key == QLatin1String("dropZ")) {
        const QVariantMap current = dropParams();
        double radius = current.value(QStringLiteral("radius")).toDouble();
        double x = current.value(QStringLiteral("x")).toDouble();
        double y = current.value(QStringLiteral("y")).toDouble();
        double z = current.value(QStringLiteral("z")).toDouble();
        if (key == QLatin1String("dropRadius")) radius = value.toDouble();
        else if (key == QLatin1String("dropX")) x = value.toDouble();
        else if (key == QLatin1String("dropY")) y = value.toDouble();
        else z = value.toDouble();
        setDropParams(radius, x, y, z);
        return;
    }

    QVariantMap snapshotParams;
    {
        std::lock_guard<std::mutex> lk(m_impl->uiMutex);
        if (m_impl->params.value(key) == value) return;
        m_impl->params.insert(key, value);
        snapshotParams = m_impl->params;
    }

    if (key == QLatin1String("speed")) {
        m_impl->speed.store(std::clamp(value.toInt(), 1, static_cast<int>(kMaxStepsPerSecond)));
    } else if (key == QLatin1String("viewMode")) {
        m_impl->viewMode.store(value.toInt());
    } else if (key == QLatin1String("surfaceThreshold")) {
        m_impl->reconThreshold.store(value.toFloat());
    } else if (key == QLatin1String("gridScale")) {
        m_impl->reconGridScale.store(value.toFloat());
    } else if (key == QLatin1String("reconInterval")) {
        m_impl->reconInterval.store(std::max(1, value.toInt()));
    }

    if (key == QLatin1String("backend")) {
        // A different backend needs a new solver object. Restarting the worker
        // re-creates it from the stored params (and resets the scene, which is
        // the honest thing to do when the solver itself changes).
        shutdown();
        start();
        Q_EMIT paramsChanged();
        return;
    }

    if (key == QLatin1String("spacing") || key == QLatin1String("solver") ||
        key == QLatin1String("particleCount")) {
        // These rebuild the solver state, so they run as one atomic command.
        // particleCount is the source of truth; spacing is derived from it (and
        // written back so the UI, the JSON export and the render jobs agree).
        QVariantMap p = snapshotParams;
        const int scene = p.value(QStringLiteral("scene")).toInt();
        int count = p.value(QStringLiteral("particleCount")).toInt();
        if (key == QLatin1String("spacing")) {
            count = CountForSpacing(scene, p.value(QStringLiteral("spacing")).toFloat());
        }
        count = ClampParticleCount(count);
        p.insert(QStringLiteral("particleCount"), count);
        const float spacing = SpacingForCount(scene, count);
        p.insert(QStringLiteral("spacing"), spacing);
        QVariantMap previousParams;
        {
            std::lock_guard<std::mutex> lk(m_impl->uiMutex);
            previousParams = m_impl->params;
            m_impl->params = p;
        }
        postCommand([this, p, previousParams, scene, count, spacing](IFluidSolver& solver) {
            try {
                // Validate the budget BEFORE ApplyParamsToSolver: a refused
                // scene must leave the running solver's spacing/params untouched,
                // not half-updated while its particle state is still the old one.
                solver.CheckParticleBudget(count);
                ApplyParamsToSolver(solver, p);
                SetupScene(solver, scene, count);
            } catch (...) {
                // The scene was refused (e.g. it does not fit in GPU memory).
                // Put the previous settings back so the UI shows what is still
                // running, then let the worker report the reason.
                {
                    std::lock_guard<std::mutex> lk(m_impl->uiMutex);
                    m_impl->params = previousParams;
                }
                Q_EMIT paramsChanged();
                throw;
            }
            m_impl->scene.store(scene);
            m_impl->particleCount = count;
            m_impl->spacing = spacing;
            m_impl->reconSpacing.store(spacing);
            m_impl->solverBroken = false;
        });
        {
            std::lock_guard<std::mutex> lk(m_impl->pubMutex);
            m_impl->surface.reset();
            ++m_impl->surfaceVersion;
        }
        Q_EMIT surfaceUpdated();
    } else {
        const QVariant v = value;
        postCommand([key, v](IFluidSolver& solver) {
            ApplySingleParam(solver.GetParams(), key, v);
        });
    }

    Q_EMIT paramsChanged();
}

QString SimulationSession::exportParamsJson() const {
    return exportParamsJsonWithBase(QString());
}

QString SimulationSession::exportParamsJsonWithBase(const QString& baseDir) const {
    const QVariantMap p = params();
    const int scene = p.value(QStringLiteral("scene"), kSceneEmpty).toInt();
    const double spacing = p.value(QStringLiteral("spacing"), 0.025).toDouble();
    const int particleCount = p.value(QStringLiteral("particleCount")).toInt();

    QVariantMap simulation;
    simulation.insert(QStringLiteral("scene"), SceneName(scene));
    simulation.insert(QStringLiteral("solver"),
                      p.value(QStringLiteral("solver")).toInt() == 1 ? QStringLiteral("DFSPH")
                                                                     : QStringLiteral("IISPH"));
    simulation.insert(QStringLiteral("particleCount"), particleCount);
    simulation.insert(QStringLiteral("spacing"), spacing);
    simulation.insert(QStringLiteral("dt"), p.value(QStringLiteral("dt")));
    simulation.insert(QStringLiteral("useCFL"), p.value(QStringLiteral("cfl")));
    simulation.insert(QStringLiteral("omega"), p.value(QStringLiteral("omega")));
    simulation.insert(QStringLiteral("eta"), p.value(QStringLiteral("eta")));
    simulation.insert(QStringLiteral("etaV"), p.value(QStringLiteral("etaV")));
    simulation.insert(QStringLiteral("maxIter"), p.value(QStringLiteral("maxIter")));
    simulation.insert(QStringLiteral("viscosity"), p.value(QStringLiteral("viscosity")));
    simulation.insert(QStringLiteral("surfaceTension"),
                      p.value(QStringLiteral("surfaceTension")));
    simulation.insert(QStringLiteral("wallAdhesion"), p.value(QStringLiteral("wallAdhesion")));
    simulation.insert(QStringLiteral("viewMode"), p.value(QStringLiteral("viewMode")));
    simulation.insert(QStringLiteral("speed"), p.value(QStringLiteral("speed")));

    // Model paths go in relative to the exported file when they live under it,
    // so a config plus its meshes can be copied to another machine as a folder.
    const QDir base(baseDir.isEmpty() ? QDir::currentPath() : baseDir);
    QVariantList models;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        models.reserve(static_cast<int>(m_impl->modelEntries.size()));
        for (const Impl::ModelEntry& entry : m_impl->modelEntries) {
            QString path = entry.path;
            if (!baseDir.isEmpty() && !path.isEmpty()) {
                const QString relative = base.relativeFilePath(path);
                if (!relative.startsWith(QLatin1String(".."))) path = relative;
            }
            QVariantMap item;
            item.insert(QStringLiteral("path"), path);
            item.insert(QStringLiteral("position"), ToList(entry.position));
            item.insert(QStringLiteral("rotation"), ToList(entry.rotation));
            item.insert(QStringLiteral("scale"), static_cast<double>(entry.scale));
            item.insert(QStringLiteral("visible"), entry.visible);
            models.append(item);
        }
    }
    simulation.insert(QStringLiteral("models"), models);

    QVariantMap render;
    render.insert(QStringLiteral("mode"),
                  p.value(QStringLiteral("viewMode")).toInt() == 1 ? QStringLiteral("surface")
                                                                  : QStringLiteral("particles"));
    render.insert(QStringLiteral("gridScale"), p.value(QStringLiteral("gridScale")));
    render.insert(QStringLiteral("surfaceThreshold"),
                  p.value(QStringLiteral("surfaceThreshold")));
    render.insert(QStringLiteral("reconInterval"), p.value(QStringLiteral("reconInterval")));

    const QVariantMap cam = cameraState();
    QVariantMap camera;
    camera.insert(QStringLiteral("yaw"), cam.value(QStringLiteral("yaw")));
    camera.insert(QStringLiteral("pitch"), cam.value(QStringLiteral("pitch")));
    camera.insert(QStringLiteral("distance"), cam.value(QStringLiteral("distance")));
    camera.insert(QStringLiteral("target"),
                  QVariantList{cam.value(QStringLiteral("targetX")),
                               cam.value(QStringLiteral("targetY")),
                               cam.value(QStringLiteral("targetZ"))});
    // Older readers look for flat target keys; keep both forms in sync.
    camera.insert(QStringLiteral("targetX"), cam.value(QStringLiteral("targetX")));
    camera.insert(QStringLiteral("targetY"), cam.value(QStringLiteral("targetY")));
    camera.insert(QStringLiteral("targetZ"), cam.value(QStringLiteral("targetZ")));
    camera.insert(QStringLiteral("fov"), cam.value(QStringLiteral("fov")));
    render.insert(QStringLiteral("camera"), camera);

    QString background;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        background = m_impl->backgroundHex;
    }
    render.insert(QStringLiteral("background"), background);

    // A drop is a single dam-break event; other scenes have nothing to emit.
    QVariantList events;
    if (scene == kSceneDam) {
        const QVariantMap drop = dropParams();
        QVariantMap event;
        event.insert(QStringLiteral("type"), QStringLiteral("drop"));
        event.insert(QStringLiteral("time"), 0.0);
        event.insert(QStringLiteral("radius"), drop.value(QStringLiteral("radius")));
        event.insert(QStringLiteral("x"), drop.value(QStringLiteral("x")));
        event.insert(QStringLiteral("y"), drop.value(QStringLiteral("y")));
        event.insert(QStringLiteral("z"), drop.value(QStringLiteral("z")));
        events.append(event);
    }

    QVariantMap root;
    root.insert(QStringLiteral("app"), QStringLiteral("IISPH"));
    root.insert(QStringLiteral("schemaVersion"), 2);
    root.insert(QStringLiteral("savedAt"),
                QDateTime::currentDateTime().toString(Qt::ISODate));
    root.insert(QStringLiteral("simulation"), simulation);
    root.insert(QStringLiteral("events"), events);
    root.insert(QStringLiteral("render"), render);
    const QJsonDocument doc(QJsonObject::fromVariantMap(root));
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

void SimulationSession::importParameters() {
    const QString filePath = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("Import Parameter (Json)"), QDir::homePath(),
        QStringLiteral("JSON Files (*.json);;All Files (*)"));
    if (!filePath.isEmpty()) importParametersFile(filePath);
}

void SimulationSession::exportParameters() {
    QString filePath = QFileDialog::getSaveFileName(
        nullptr, QStringLiteral("Export Parameter (Json)"),
        QDir::homePath() + QStringLiteral("/parameters.json"),
        QStringLiteral("JSON Files (*.json);;All Files (*)"));
    if (filePath.isEmpty()) return;
    if (QFileInfo(filePath).suffix().isEmpty()) filePath += QStringLiteral(".json");
    exportParametersFile(filePath);
}

bool SimulationSession::importParametersFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Q_EMIT message(QStringLiteral("Parameter import failed: cannot open %1").arg(filePath));
        return false;
    }
    const QString directory = QFileInfo(filePath).absolutePath();
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        m_impl->importBaseDir = directory;
    }
    const bool ok = importParamsJson(QString::fromUtf8(file.readAll()));
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        m_impl->importBaseDir.clear();
    }
    if (!ok) {
        Q_EMIT message(QStringLiteral("Parameter import failed: invalid or incompatible JSON"));
        return false;
    }
    Q_EMIT message(QStringLiteral("Parameters imported: %1").arg(filePath));
    return true;
}

bool SimulationSession::exportParametersFile(const QString& filePath) {
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT message(QStringLiteral("Parameter export failed: cannot write %1").arg(filePath));
        return false;
    }
    const QByteArray json =
        exportParamsJsonWithBase(QFileInfo(filePath).absolutePath()).toUtf8();
    if (file.write(json) != json.size() || !file.commit()) {
        Q_EMIT message(QStringLiteral("Parameter export failed: cannot write %1").arg(filePath));
        return false;
    }
    Q_EMIT message(QStringLiteral("Parameters exported: %1").arg(filePath));
    return true;
}

bool SimulationSession::importParamsJson(const QString& json) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("app")).isString() &&
        root.value(QStringLiteral("app")).toString() != QStringLiteral("IISPH")) {
        return false;
    }
    // Accept both schema generations; exporting always writes version 2.
    if (root.contains(QStringLiteral("schemaVersion"))) {
        const QJsonValue versionValue = root.value(QStringLiteral("schemaVersion"));
        if (!versionValue.isDouble()) return false;
        const int version = versionValue.toInt();
        if (version != 1 && version != 2) return false;
    }

    QVariantMap incoming;
    QJsonArray modelsArray;
    bool hasCamera = false;
    QVariantMap cameraConfig;
    QString background;
    const QJsonObject settings = root.value(QStringLiteral("settings")).toObject();
    if (!settings.isEmpty()) {
        // Legacy files written by the first parameter-JSON exporter.
        incoming = settings.toVariantMap();
    } else {
        const QJsonObject simulation = root.value(QStringLiteral("simulation")).toObject();
        const QJsonObject render = root.value(QStringLiteral("render")).toObject();
        const auto addNumber = [&incoming](const QJsonObject& source, const char* sourceKey,
                                           const char* targetKey = nullptr) {
            const QJsonValue value = source.value(QLatin1String(sourceKey));
            if (!value.isDouble()) return;
            incoming.insert(QString::fromLatin1(targetKey ? targetKey : sourceKey),
                            value.toDouble());
        };
        const auto addBool = [&incoming](const QJsonObject& source, const char* sourceKey,
                                         const char* targetKey = nullptr) {
            const QJsonValue value = source.value(QLatin1String(sourceKey));
            if (value.isBool()) {
                incoming.insert(QString::fromLatin1(targetKey ? targetKey : sourceKey),
                                value.toBool());
            }
        };

        const QJsonValue sceneValue = simulation.value(QStringLiteral("scene"));
        if (sceneValue.isString()) {
            incoming.insert(QStringLiteral("scene"), SceneFromName(sceneValue.toString()));
        } else if (sceneValue.isDouble()) {
            incoming.insert(QStringLiteral("scene"), ClampScene(sceneValue.toInt()));
        }

        const QJsonValue solverValue = simulation.value(QStringLiteral("solver"));
        if (solverValue.isString()) {
            incoming.insert(QStringLiteral("solver"),
                            solverValue.toString().compare(QStringLiteral("DFSPH"),
                                                           Qt::CaseInsensitive) == 0
                                ? 1 : 0);
        } else if (solverValue.isDouble()) {
            incoming.insert(QStringLiteral("solver"), solverValue.toInt() == 1 ? 1 : 0);
        }

        // particleCount is authoritative; spacing is derived from it below.
        const QJsonValue countValue = simulation.value(QStringLiteral("particleCount"));
        if (countValue.isDouble() && countValue.toDouble() > 0.0)
            incoming.insert(QStringLiteral("particleCount"), countValue.toInt());
        addNumber(simulation, "spacing");
        addNumber(simulation, "dt");
        addBool(simulation, "useCFL", "cfl");
        addNumber(simulation, "omega");
        addNumber(simulation, "eta");
        addNumber(simulation, "etaV");
        addNumber(simulation, "maxIter");
        addNumber(simulation, "viscosity");
        addNumber(simulation, "surfaceTension");
        addNumber(simulation, "wallAdhesion");
        addNumber(simulation, "viewMode");
        addNumber(simulation, "speed");

        addNumber(render, "gridScale");
        addNumber(render, "surfaceThreshold");
        addNumber(render, "reconInterval");
        const QString mode = render.value(QStringLiteral("mode")).toString();
        if (!mode.isEmpty() && !simulation.contains(QStringLiteral("viewMode"))) {
            incoming.insert(QStringLiteral("viewMode"),
                            mode.compare(QStringLiteral("surface"), Qt::CaseInsensitive) == 0
                                ? 1 : 0);
        }

        if (simulation.value(QStringLiteral("models")).isArray())
            modelsArray = simulation.value(QStringLiteral("models")).toArray();
        const QJsonObject camera = render.value(QStringLiteral("camera")).toObject();
        if (!camera.isEmpty()) {
            hasCamera = true;
            cameraConfig = camera.toVariantMap();
        }
        if (render.value(QStringLiteral("background")).isString())
            background = render.value(QStringLiteral("background")).toString();

        // schema v2 events: one optional drop.
        const QJsonArray events = root.value(QStringLiteral("events")).toArray();
        for (const QJsonValue& value : events) {
            const QJsonObject event = value.toObject();
            if (event.value(QStringLiteral("type")).toString() != QLatin1String("drop")) continue;
            const auto number = [&event](const char* key, double fallback) {
                const QJsonValue v = event.value(QLatin1String(key));
                if (!v.isDouble() || !std::isfinite(v.toDouble())) return fallback;
                return v.toDouble();
            };
            incoming.insert(QStringLiteral("dropRadius"), number("radius", 0.2));
            incoming.insert(QStringLiteral("dropX"), number("x", 1.0));
            incoming.insert(QStringLiteral("dropY"), number("y", 1.6));
            incoming.insert(QStringLiteral("dropZ"), number("z", 0.5));
            break;
        }
    }

    // Resolve model paths against the config's directory (or the working dir
    // when importParamsJson() was called directly).
    QString baseDir;
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        baseDir = m_impl->importBaseDir;
    }
    if (baseDir.isEmpty()) baseDir = QDir::currentPath();

    struct IncomingModel {
        QString path;
        Math::Vector3 position{0, 0, 0};
        Math::Quaternions rotation;
        float scale = 1.0f;
        bool visible = true;
    };
    std::vector<IncomingModel> importedModels;
    importedModels.reserve(static_cast<size_t>(modelsArray.size()));
    for (const QJsonValue& value : modelsArray) {
        const QJsonObject object = value.toObject();
        const QString rawPath = object.value(QStringLiteral("path")).toString();
        if (rawPath.trimmed().isEmpty()) {
            // A config that lost its model paths cannot be honoured.
            Q_EMIT message(QStringLiteral("Parameter import failed: a model has no path"));
            return false;
        }
        QFileInfo info(rawPath);
        if (info.isRelative()) info = QFileInfo(QDir(baseDir).absoluteFilePath(rawPath));
        IncomingModel model;
        model.path = info.absoluteFilePath();
        const QJsonArray position = object.value(QStringLiteral("position")).toArray();
        if (position.size() == 3) {
            model.position = Math::Vector3(static_cast<float>(position.at(0).toDouble()),
                                           static_cast<float>(position.at(1).toDouble()),
                                           static_cast<float>(position.at(2).toDouble()));
        }
        const QJsonArray rotation = object.value(QStringLiteral("rotation")).toArray();
        if (rotation.size() == 4) {
            Math::Quaternions q(static_cast<float>(rotation.at(0).toDouble()),
                                static_cast<float>(rotation.at(1).toDouble()),
                                static_cast<float>(rotation.at(2).toDouble()),
                                static_cast<float>(rotation.at(3).toDouble()));
            q.normalize();
            model.rotation = q;
        }
        const QJsonValue scale = object.value(QStringLiteral("scale"));
        if (scale.isDouble())
            model.scale = static_cast<float>(std::clamp(scale.toDouble(), 0.001, 1000.0));
        const QJsonValue visible = object.value(QStringLiteral("visible"));
        if (visible.isBool()) model.visible = visible.toBool();
        importedModels.push_back(model);
    }

    const bool cameraOnly = incoming.isEmpty() && importedModels.empty() && !hasCamera &&
                            background.isEmpty();
    if (cameraOnly) return false;

    QVariantMap merged;
    {
        std::lock_guard<std::mutex> lk(m_impl->uiMutex);
        merged = m_impl->params;
    }
    for (auto it = incoming.constBegin(); it != incoming.constEnd(); ++it) {
        if (merged.contains(it.key())) merged.insert(it.key(), it.value());
    }

    // Normalise the particle budget. A file that only carried `spacing` (all
    // files written before particleCount existed) is converted to a count, so
    // the scene afterwards holds exactly the requested number of particles.
    const int scene = ClampScene(merged.value(QStringLiteral("scene")).toInt());
    int count;
    if (incoming.contains(QStringLiteral("particleCount")))
        count = incoming.value(QStringLiteral("particleCount")).toInt();
    else if (incoming.contains(QStringLiteral("spacing")))
        count = CountForSpacing(scene, incoming.value(QStringLiteral("spacing")).toFloat());
    else
        count = merged.value(QStringLiteral("particleCount")).toInt();
    count = ClampParticleCount(count);
    const float spacing = SpacingForCount(scene, count);
    merged.insert(QStringLiteral("scene"), scene);
    merged.insert(QStringLiteral("particleCount"), count);
    merged.insert(QStringLiteral("spacing"), spacing);
    {
        std::lock_guard<std::mutex> lk(m_impl->uiMutex);
        m_impl->params = merged;
    }

    m_impl->speed.store(std::clamp(merged.value(QStringLiteral("speed")).toInt(), 1,
                                   static_cast<int>(kMaxStepsPerSecond)));
    m_impl->viewMode.store(merged.value(QStringLiteral("viewMode")).toInt());
    m_impl->reconThreshold.store(merged.value(QStringLiteral("surfaceThreshold")).toFloat());
    m_impl->reconGridScale.store(merged.value(QStringLiteral("gridScale")).toFloat());
    m_impl->reconInterval.store(
        std::max(1, merged.value(QStringLiteral("reconInterval")).toInt()));
    m_impl->scene.store(scene);

    // Camera + background: v2 files carry them, v1 files fall back to the
    // scene defaults.
    if (hasCamera) {
        setCameraConfig(cameraConfig);
    } else {
        {
            std::lock_guard<std::mutex> lk(m_impl->stateMutex);
            m_impl->camera = DefaultCameraForScene(scene);
        }
        Q_EMIT cameraStateChanged();
    }
    setViewportBackgroundHex(background.isEmpty() ? QStringLiteral("#1F2638") : background);

    // Replace the object list. A file without `models` (schema v1) means no
    // models, so anything authored in the session is dropped.
    int failed = 0;
    QStringList failures;
    struct LoadedModel {
        QString path;
        ModelDataPtr data;
        Math::Vector3 position;
        Math::Quaternions rotation;
        float scale = 1.0f;
        bool visible = true;
    };
    std::vector<LoadedModel> loadedModels;
    loadedModels.reserve(importedModels.size());
    // Decode outside the state lock: assimp I/O must not stall the UI thread.
    for (const IncomingModel& model : importedModels) {
        QString loadError;
        ModelDataPtr data = Model::Load(model.path, &loadError);
        if (!data) {
            ++failed;
            failures.append(QStringLiteral("%1 (%2)")
                                .arg(QFileInfo(model.path).fileName(), loadError));
            continue;
        }
        loadedModels.push_back(LoadedModel{model.path, std::move(data), model.position,
                                           model.rotation, model.scale, model.visible});
    }
    {
        std::lock_guard<std::mutex> lk(m_impl->stateMutex);
        m_impl->modelEntries.clear();
        m_impl->selectedId = -1;
        for (LoadedModel& model : loadedModels) {
            std::vector<std::string> existing;
            existing.reserve(m_impl->modelEntries.size());
            for (const Impl::ModelEntry& entry : m_impl->modelEntries) {
                existing.push_back(entry.name.toStdString());
            }
            Impl::ModelEntry entry;
            entry.id = m_impl->nextModelId++;
            entry.name = QString::fromStdString(
                UniqueModelName(QFileInfo(model.path).completeBaseName(), existing));
            entry.path = model.path;
            entry.data = std::move(model.data);
            entry.position = model.position;
            entry.rotation = model.rotation;
            entry.scale = model.scale;
            entry.visible = model.visible;
            m_impl->modelEntries.push_back(entry);
            if (m_impl->selectedId < 0) m_impl->selectedId = entry.id;
        }
    }
    publishModels();
    Q_EMIT selectionChanged();

    postCommand([this, merged, scene, count, spacing](IFluidSolver& solver) {
        ApplyParamsToSolver(solver, merged);
        m_impl->particleCount = count;
        m_impl->spacing = spacing;
        m_impl->reconSpacing.store(spacing);
        SetupScene(solver, scene, count);
        m_impl->solverBroken = false;
    });
    {
        std::lock_guard<std::mutex> lk(m_impl->pubMutex);
        m_impl->surface.reset();
        ++m_impl->surfaceVersion;
    }
    Q_EMIT surfaceUpdated();
    Q_EMIT sceneChanged();
    Q_EMIT paramsChanged();
    if (failed > 0) {
        Q_EMIT message(QStringLiteral("Imported parameters, %1 model(s) failed: %2")
                           .arg(failed)
                           .arg(failures.join(QStringLiteral("; "))));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Simulation thread
// ---------------------------------------------------------------------------
void SimulationSession::workerLoop() {
    Impl& d = *m_impl;
    std::string backendError;
    SolverBackend backendKind = SolverBackend::Auto;
    {
        std::lock_guard<std::mutex> lk(d.uiMutex);
        bool ok = false;
        backendKind = ParseBackend(
            d.params.value(QStringLiteral("backend"), QStringLiteral("auto"))
                .toString().toStdString(), &ok);
    }
    auto solverHolder = CreateSolver(backendKind, &backendError);
    if (!solverHolder) {
        solverHolder = std::make_unique<FluidSolver>();
        const QString reason = QString::fromStdString(backendError);
        Q_EMIT message(reason.isEmpty()
                           ? QStringLiteral("Requested backend unavailable — using the CPU solver")
                           : QStringLiteral("Backend unavailable (%1) — using the CPU solver")
                                 .arg(reason));
    } else if (!backendError.empty()) {
        // Auto selected the CPU because CUDA was not usable; say why instead of
        // silently presenting a CPU run under an "Auto" label.
        Q_EMIT message(QStringLiteral("CUDA unavailable (%1) — using the CPU solver")
                           .arg(QString::fromStdString(backendError)));
    }
    IFluidSolver& solver = *solverHolder;
    {
        std::lock_guard<std::mutex> lk(d.uiMutex);
        ApplyParamsToSolver(solver, d.params);
        d.scene.store(ClampScene(d.params.value(QStringLiteral("scene")).toInt()));
        d.particleCount = d.params.value(QStringLiteral("particleCount")).toInt();
        d.spacing = SpacingForCount(d.scene.load(), d.particleCount);
        d.reconInterval.store(
            std::max(1, d.params.value(QStringLiteral("reconInterval")).toInt()));
    }
    try {
        SetupScene(solver, d.scene.load(), d.particleCount);
    } catch (const std::exception& ex) {
        // A scene that does not fit never leaves a half-built solver behind;
        // report it and let the user pick a smaller particle count.
        d.solverBroken = true;
        Q_EMIT message(QStringLiteral("Scene setup failed: %1")
                           .arg(QString::fromUtf8(ex.what())));
    }

    auto lastStatus = Clock::now() - std::chrono::milliseconds(kStatusIntervalMs);
    auto lastTrace = Clock::now();
    auto nextDeadline = Clock::now();
    double lastStepMs = 0.0;
    uint64_t steps = 0;

    auto publishSnapshot = [&](double msPerStep) {
        auto snap = std::make_shared<SimSnapshot>();
        snap->positions = solver.Positions();
        snap->colors = solver.Colors();
        snap->simTime = solver.SimTime();
        snap->scene = d.scene.load();
        snap->fluidCount = solver.FluidCount();
        snap->boundaryCount = solver.BoundaryCount();
        snap->avgDensityError = solver.AvgDensityError();
        snap->maxDensityError = solver.MaxDensityError();
        snap->pressureIterations = solver.LastIterations();
        snap->divergenceIterations = solver.DivergenceIterations();
        snap->msPerStep = msPerStep;
        snap->hasDrop = solver.HasDrop();
        snap->stepCount = steps;
        std::lock_guard<std::mutex> lk(d.pubMutex);
        d.snapshot = snap;
    };

    auto publishStatus = [&](double msPerStep) {
        SimSnapshotPtr snap;
        {
            std::lock_guard<std::mutex> lk(d.pubMutex);
            snap = d.snapshot;
        }
        QVariantMap st;
        st.insert(QStringLiteral("simTime"), solver.SimTime());
        st.insert(QStringLiteral("fluidCount"), solver.FluidCount());
        st.insert(QStringLiteral("boundaryCount"), solver.BoundaryCount());
        st.insert(QStringLiteral("densityError"), static_cast<double>(solver.AvgDensityError()));
        st.insert(QStringLiteral("maxDensityError"), static_cast<double>(solver.MaxDensityError()));
        st.insert(QStringLiteral("meanDensity"), static_cast<double>(solver.MeanDensity()));
        st.insert(QStringLiteral("rho0"), static_cast<double>(solver.GetParams().rho0));
        st.insert(QStringLiteral("pressureIterations"), solver.LastIterations());
        st.insert(QStringLiteral("divergenceIterations"), solver.DivergenceIterations());
        st.insert(QStringLiteral("msPerStep"), msPerStep);
        st.insert(QStringLiteral("hasDrop"), solver.HasDrop());
        st.insert(QStringLiteral("stepCount"), static_cast<qulonglong>(steps));
        st.insert(QStringLiteral("backend"), QString::fromLatin1(solver.BackendName()));
        st.insert(QStringLiteral("scene"), d.scene.load());
        st.insert(QStringLiteral("spacing"), static_cast<double>(d.spacing));
        st.insert(QStringLiteral("particleTarget"), d.particleCount);
        st.insert(QStringLiteral("waterVolume"),
                  static_cast<double>(WaterVolumeForScene(d.scene.load())));
        const GpuMemoryInfo mem = solver.GpuMemory();
        st.insert(QStringLiteral("vramFreeBytes"), static_cast<qlonglong>(mem.freeBytes));
        st.insert(QStringLiteral("vramTotalBytes"), static_cast<qlonglong>(mem.totalBytes));
        st.insert(QStringLiteral("bytesPerParticle"),
                  static_cast<qlonglong>(mem.bytesPerParticle));
        st.insert(QStringLiteral("solverError"),
                  d.solverBroken ? QStringLiteral("solver unavailable") : QString());
        {
            std::lock_guard<std::mutex> lk(d.uiMutex);
            d.status = st;
            d.traceRaw.push_back(Impl::TraceEntry{
                static_cast<double>(solver.SimTime()),
                static_cast<double>(solver.MeanDensity()),
                static_cast<double>(solver.AvgDensityError()),
                steps});
            while (static_cast<int>(d.traceRaw.size()) > kTraceLimit) d.traceRaw.pop_front();
        }
        Q_EMIT statusChanged();
    };

    auto refreshTrace = [&]() {
        QVariantList list;
        {
            std::lock_guard<std::mutex> lk(d.uiMutex);
            list.reserve(static_cast<int>(d.traceRaw.size()));
            for (const auto& p : d.traceRaw) {
                QVariantMap point;
                point.insert(QStringLiteral("t"), p.simTime);
                point.insert(QStringLiteral("density"), p.density);
                point.insert(QStringLiteral("e"), p.densityError);
                point.insert(QStringLiteral("step"), QVariant::fromValue<qulonglong>(p.step));
                list.append(point);
            }
            d.trace = list;
        }
        Q_EMIT traceChanged();
    };

    auto maybeRequestReconstruction = [&](bool force = false) {
        // The Empty scene has no fluid: never spend a reconstruction pass on it
        // and make sure a surface left over from the previous scene is gone.
        if (d.scene.load() == kSceneEmpty) {
            bool hadSurface = false;
            {
                std::lock_guard<std::mutex> lk(d.pubMutex);
                hadSurface = static_cast<bool>(d.surface);
                d.surface.reset();
            }
            if (hadSurface) Q_EMIT surfaceUpdated();
            return;
        }
        const int mode = d.viewMode.load();
        const int interval = std::max(1, d.reconInterval.load());
        if (mode <= 0) return;
        if (!force && ++d.reconCounter < interval) return;
        d.reconCounter = 0;
        std::lock_guard<std::mutex> lk(d.reconMutex);
        if (d.reconQuit) return;
        // Keep the newest request even if one is already queued. This matters
        // when a paused scene is reset or rebuilt more than once in quick
        // succession: the reconstruction thread must never publish old input
        // as the final surface.
        d.reconInput = solver.Positions();
        d.reconSpacing.store(solver.Spacing());
        d.reconPending = true;
        d.reconCv.notify_one();
    };

    // First frame so the window has something to draw immediately. Also build
    // the initial surface when --view surface starts in the paused state.
    publishSnapshot(0.0);
    publishStatus(0.0);
    refreshTrace();
    maybeRequestReconstruction(true);

    // A solver call that throws (out of GPU memory, device lost, ...) must not
    // take the window down with it: stop stepping, surface the reason, and wait
    // for a scene command that rebuilds the state.
    auto reportFailure = [&](const std::exception& ex) {
        d.solverBroken = true;
        d.running.store(false);
        Q_EMIT runningChanged();
        const QString text = QStringLiteral("Solver error: %1").arg(QString::fromUtf8(ex.what()));
        Q_EMIT message(text);
        {
            std::lock_guard<std::mutex> lk(d.uiMutex);
            d.status.insert(QStringLiteral("solverError"), text);
        }
        Q_EMIT statusChanged();
    };

    while (!d.quit.load()) {
        bool commandsApplied = false;
        {
            std::deque<std::function<void(IFluidSolver&)>> pending;
            {
                std::lock_guard<std::mutex> lk(d.cmdMutex);
                pending.swap(d.commands);
            }
            for (auto& fn : pending) {
                try {
                    fn(solver);
                } catch (const ParticleBudgetError& ex) {
                    // Refusing an oversized rebuild does not invalidate the
                    // running scene: keep the solver usable and only surface
                    // the reason to the user.
                    const QString text =
                        QStringLiteral("Scene not rebuilt: %1").arg(QString::fromUtf8(ex.what()));
                    Q_EMIT message(text);
                    {
                        std::lock_guard<std::mutex> lk(d.uiMutex);
                        d.status.insert(QStringLiteral("solverError"), text);
                    }
                    Q_EMIT statusChanged();
                } catch (const std::exception& ex) {
                    reportFailure(ex);
                }
                commandsApplied = true;
            }
        }
        if (d.quit.load()) break;

        // Commands can reset/rebuild the solver without advancing time. Publish
        // their result immediately so paused scenes, particles and surfaces all
        // reflect the new state instead of waiting for the next simulation step.
        if (commandsApplied) {
            publishSnapshot(lastStepMs);
            if (!d.running.load()) {
                publishStatus(lastStepMs);
                refreshTrace();
            }
            if (!d.solverBroken) maybeRequestReconstruction(true);
        }

        const bool wantRunning = d.running.load() && !d.solverBroken;
        int requests = d.stepRequests.load();
        bool doStep = false;
        if (wantRunning) {
            doStep = true;
        } else if (requests > 0 && !d.solverBroken) {
            d.stepRequests.fetch_sub(1);
            doStep = true;
        }

        if (!doStep) {
            std::unique_lock<std::mutex> lk(d.cmdMutex);
            d.cmdCv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return d.quit.load() || d.running.load() || d.stepRequests.load() > 0 ||
                       !d.commands.empty();
            });
            continue;
        }

        const auto t0 = Clock::now();
        try {
            solver.Step();
        } catch (const std::exception& ex) {
            reportFailure(ex);
            continue;
        }
        const auto t1 = Clock::now();
        lastStepMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        ++steps;

        publishSnapshot(lastStepMs);
        maybeRequestReconstruction();

        const auto now = Clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatus).count() >=
            kStatusIntervalMs) {
            lastStatus = now;
            publishStatus(lastStepMs);
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTrace).count() >=
            kTraceIntervalMs) {
            lastTrace = now;
            refreshTrace();
        }

        if (wantRunning) {
            const int sps = std::clamp(d.speed.load(), 1, static_cast<int>(kMaxStepsPerSecond));
            const auto interval = std::chrono::microseconds(1000000 / sps);
            nextDeadline += interval;
            const auto now2 = Clock::now();
            if (nextDeadline < now2) nextDeadline = now2;
            std::this_thread::sleep_until(nextDeadline);
        }
    }
}

// ---------------------------------------------------------------------------
// Surface reconstruction thread
// ---------------------------------------------------------------------------
void SimulationSession::reconLoop() {
    Impl& d = *m_impl;
    SurfaceReconstructor rec;
    float lastThreshold = -1.0f;
    float lastGrid = -1.0f;

    while (true) {
        std::vector<Math::Vector3> input;
        float spacing = 0.025f;
        float threshold = 0.8f;
        float grid = 0.5f;
        {
            std::unique_lock<std::mutex> lk(d.reconMutex);
            d.reconCv.wait(lk, [&] { return d.reconQuit || d.reconPending; });
            if (d.reconQuit) return;
            d.reconPending = false;
            input.swap(d.reconInput);
            spacing = d.reconSpacing.load();
            threshold = d.reconThreshold.load();
            grid = d.reconGridScale.load();
        }
        if (input.empty()) continue;

        if (threshold != lastThreshold) {
            rec.SetSurfaceThreshold(threshold);
            lastThreshold = threshold;
        }
        if (grid != lastGrid) {
            rec.SetGridScale(grid);
            lastGrid = grid;
        }
        if (!rec.Reconstruct(input, spacing)) continue;
        // The scene may have switched to Empty while this rebuild was running;
        // publishing then would resurrect a stale surface.
        if (d.scene.load() == kSceneEmpty) continue;

        auto surf = std::make_shared<SurfaceSnapshot>();
        surf->vertices = rec.Vertices();
        surf->indices = rec.Indices();
        {
            std::lock_guard<std::mutex> lk(d.pubMutex);
            surf->version = ++d.surfaceVersion;
            d.surface = surf;
        }
        Q_EMIT surfaceUpdated();
    }
}
