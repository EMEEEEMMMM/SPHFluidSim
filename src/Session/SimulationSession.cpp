#include "Session/SimulationSession.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "FluidSolver.hpp"
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

float WaterVolumeForScene(int scene) {
    return scene == 1 ? 0.6f : 0.512f;   // matches the solver's own scene setup
}

// Particle cap of the inflow scene at the CURRENT spacing: keeps the injected
// water volume fixed when the resolution changes (same rule as the old GUI).
int InflowCapFor(float spacing) {
    const float cell = spacing * spacing * spacing;
    return static_cast<int>(0.6f / cell) + 8;
}

void SetupScene(IFluidSolver& solver, int scene, float spacing) {
    solver.GetParams().spacing = spacing;
    if (scene == 0) {
        solver.InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);
    } else {
        solver.InitInflow(1.0f, 0.3f, 100, 0.08f, 1.0f, 0.5f, InflowCapFor(spacing));
    }
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
    QVariantMap m;
    m.insert(QStringLiteral("scene"), 0);
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
    ModelDataPtr model;
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

    // ---- simulation-thread local ------------------------------------------
    int scene = 0;
    float spacing = 0.025f;
    std::atomic<int> reconInterval{3};
    int reconCounter = 0;
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
    m_impl->scene = m_impl->params.value(QStringLiteral("scene")).toInt();
    m_impl->spacing = m_impl->params.value(QStringLiteral("spacing")).toFloat();
    m_impl->reconInterval.store(m_impl->params.value(QStringLiteral("reconInterval")).toInt());
    m_impl->reconThreshold.store(m_impl->params.value(QStringLiteral("surfaceThreshold")).toFloat());
    m_impl->reconGridScale.store(m_impl->params.value(QStringLiteral("gridScale")).toFloat());
    m_impl->reconSpacing.store(m_impl->spacing);
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
    std::lock_guard<std::mutex> lk(m_impl->pubMutex);
    return m_impl->model;
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
    postCommand([this](IFluidSolver& solver) {
        SetupScene(solver, m_impl->scene, m_impl->spacing);
    });
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
    const QString filePath = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("Import 3D Model"), QDir::homePath(),
        QStringLiteral("3D Model Files (*.obj *.fbx *.stl *.dae *.3ds);;All Files (*)"));
    if (!filePath.isEmpty()) importModelFile(filePath);
}

bool SimulationSession::importModelFile(const QString& filePath) {
    if (!QFileInfo::exists(filePath)) {
        Q_EMIT message(QStringLiteral("Model import failed: file does not exist"));
        return false;
    }

    QString error;
    ModelDataPtr loaded = Model::Load(filePath, &error);
    if (!loaded) {
        Q_EMIT message(QStringLiteral("Model import failed: %1").arg(error));
        return false;
    }

    const qulonglong triangleCount = static_cast<qulonglong>(loaded->indices.size() / 3);
    {
        std::lock_guard<std::mutex> lk(m_impl->pubMutex);
        m_impl->model = std::move(loaded);
    }
    Q_EMIT modelChanged();
    Q_EMIT message(QStringLiteral("Imported %1 (%2 triangles)")
                       .arg(QFileInfo(filePath).fileName())
                       .arg(triangleCount));
    return true;
}

void SimulationSession::clearModel() {
    {
        std::lock_guard<std::mutex> lk(m_impl->pubMutex);
        if (!m_impl->model) return;
        m_impl->model.reset();
    }
    Q_EMIT modelChanged();
    Q_EMIT message(QStringLiteral("Imported model cleared"));
}

void SimulationSession::spawnDrop() {
    postCommand([](IFluidSolver& solver) {
        if (solver.HasDrop()) return;
        solver.SpawnDrop(0.2f, 1.0f, 1.6f, 0.5f);
    });
}

void SimulationSession::setParam(const QString& key, const QVariant& value) {
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

    if (key == QLatin1String("scene") || key == QLatin1String("spacing") ||
        key == QLatin1String("solver")) {
        // These rebuild the solver state, so they run as one atomic command.
        postCommand([this, snapshotParams](IFluidSolver& solver) {
            ApplyParamsToSolver(solver, snapshotParams);
            m_impl->scene = snapshotParams.value(QStringLiteral("scene")).toInt();
            m_impl->spacing = snapshotParams.value(QStringLiteral("spacing")).toFloat();
            m_impl->reconSpacing.store(m_impl->spacing);
            SetupScene(solver, m_impl->scene, m_impl->spacing);
        });
        std::lock_guard<std::mutex> lk(m_impl->pubMutex);
        m_impl->surface.reset();
        ++m_impl->surfaceVersion;
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
    const QVariantMap p = params();
    const bool inflow = p.value(QStringLiteral("scene")).toInt() == 1;
    const double spacing = p.value(QStringLiteral("spacing"), 0.025).toDouble();
    const double volume = inflow ? 0.6 : 0.512;
    const int particleCount = static_cast<int>(
        std::lround(volume / std::pow(std::max(1.0e-6, spacing), 3.0))) + (inflow ? 8 : 0);

    QVariantMap simulation;
    simulation.insert(QStringLiteral("scene"), inflow ? QStringLiteral("inflow")
                                                      : QStringLiteral("dam"));
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

    QVariantMap render;
    render.insert(QStringLiteral("mode"),
                  p.value(QStringLiteral("viewMode")).toInt() == 1 ? QStringLiteral("surface")
                                                                  : QStringLiteral("particles"));
    render.insert(QStringLiteral("gridScale"), p.value(QStringLiteral("gridScale")));
    render.insert(QStringLiteral("surfaceThreshold"),
                  p.value(QStringLiteral("surfaceThreshold")));
    render.insert(QStringLiteral("reconInterval"), p.value(QStringLiteral("reconInterval")));

    QVariantMap root;
    root.insert(QStringLiteral("app"), QStringLiteral("IISPH"));
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("savedAt"),
                QDateTime::currentDateTime().toString(Qt::ISODate));
    root.insert(QStringLiteral("simulation"), simulation);
    root.insert(QStringLiteral("events"), QVariantList{});
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
    if (!importParamsJson(QString::fromUtf8(file.readAll()))) {
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
    const QByteArray json = exportParamsJson().toUtf8();
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
    if (root.value(QStringLiteral("schemaVersion")).isDouble() &&
        root.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        return false;
    }

    QVariantMap incoming;
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
            incoming.insert(QStringLiteral("scene"),
                            sceneValue.toString().compare(QStringLiteral("inflow"),
                                                         Qt::CaseInsensitive) == 0
                                ? 1 : 0);
        } else if (sceneValue.isDouble()) {
            incoming.insert(QStringLiteral("scene"), sceneValue.toInt() == 1 ? 1 : 0);
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

        addNumber(simulation, "spacing");
        const bool hasSpacing = simulation.value(QStringLiteral("spacing")).isDouble();
        if (!hasSpacing) {
            const QJsonValue countValue = simulation.value(QStringLiteral("particleCount"));
            if (countValue.isDouble() && countValue.toDouble() > 0.0) {
                const double count = countValue.toDouble();
                const int scene = incoming.contains(QStringLiteral("scene"))
                    ? incoming.value(QStringLiteral("scene")).toInt()
                    : params().value(QStringLiteral("scene")).toInt();
                const double volume = scene == 1 ? 0.6 : 0.512;
                incoming.insert(QStringLiteral("spacing"), std::cbrt(volume / count));
            }
        }
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
    }
    if (incoming.isEmpty()) return false;

    QVariantMap merged;
    {
        std::lock_guard<std::mutex> lk(m_impl->uiMutex);
        merged = m_impl->params;
        bool recognized = false;
        for (auto it = incoming.constBegin(); it != incoming.constEnd(); ++it) {
            if (merged.contains(it.key())) {
                merged.insert(it.key(), it.value());
                recognized = true;
            }
        }
        if (!recognized) return false;
        m_impl->params = merged;
    }

    m_impl->speed.store(std::clamp(merged.value(QStringLiteral("speed")).toInt(), 1,
                                   static_cast<int>(kMaxStepsPerSecond)));
    m_impl->viewMode.store(merged.value(QStringLiteral("viewMode")).toInt());
    m_impl->reconThreshold.store(merged.value(QStringLiteral("surfaceThreshold")).toFloat());
    m_impl->reconGridScale.store(merged.value(QStringLiteral("gridScale")).toFloat());
    m_impl->reconInterval.store(
        std::max(1, merged.value(QStringLiteral("reconInterval")).toInt()));

    postCommand([this, merged](IFluidSolver& solver) {
        ApplyParamsToSolver(solver, merged);
        m_impl->scene = merged.value(QStringLiteral("scene")).toInt();
        m_impl->spacing = merged.value(QStringLiteral("spacing")).toFloat();
        m_impl->reconSpacing.store(m_impl->spacing);
        SetupScene(solver, m_impl->scene, m_impl->spacing);
    });
    {
        std::lock_guard<std::mutex> lk(m_impl->pubMutex);
        m_impl->surface.reset();
        ++m_impl->surfaceVersion;
    }
    Q_EMIT surfaceUpdated();
    Q_EMIT paramsChanged();
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
    if (!solverHolder) solverHolder = std::make_unique<FluidSolver>();
    IFluidSolver& solver = *solverHolder;
    {
        std::lock_guard<std::mutex> lk(d.uiMutex);
        ApplyParamsToSolver(solver, d.params);
        d.scene = d.params.value(QStringLiteral("scene")).toInt();
        d.spacing = d.params.value(QStringLiteral("spacing")).toFloat();
        d.reconInterval.store(
            std::max(1, d.params.value(QStringLiteral("reconInterval")).toInt()));
    }
    SetupScene(solver, d.scene, d.spacing);

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
        snap->scene = d.scene;
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
        st.insert(QStringLiteral("scene"), d.scene);
        st.insert(QStringLiteral("spacing"), static_cast<double>(d.spacing));
        st.insert(QStringLiteral("waterVolume"),
                  static_cast<double>(WaterVolumeForScene(d.scene)));
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

    while (!d.quit.load()) {
        bool commandsApplied = false;
        {
            std::deque<std::function<void(IFluidSolver&)>> pending;
            {
                std::lock_guard<std::mutex> lk(d.cmdMutex);
                pending.swap(d.commands);
            }
            for (auto& fn : pending) {
                fn(solver);
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
            maybeRequestReconstruction(true);
        }

        const bool wantRunning = d.running.load();
        int requests = d.stepRequests.load();
        bool doStep = false;
        if (wantRunning) {
            doStep = true;
        } else if (requests > 0) {
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
        solver.Step();
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
