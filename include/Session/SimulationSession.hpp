// SimulationSession — the UI-agnostic simulation host.
//
// The solver keeps running on its own thread so the window never blocks on a
// step (a 64k-particle release step is ~150 ms). Everything the UI needs is
// published as an immutable snapshot the renderer can read without locking the
// solver, and every user action travels the other way as a command executed on
// the simulation thread.
//
//     UI thread                          simulation thread
//     ---------                          -----------------
//     setParam / stepOnce  --command-->  apply -> FluidSolver::Step()
//     snapshot()           <--snapshot--  publish immutable SimSnapshot
//
// FluidSolver itself is untouched: same params, same defaults, same stepping.
// Only its owning thread changed.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "Math/Vector3.hpp"
#include "Model.hpp"

class IFluidSolver;

struct SimSnapshot {
    std::vector<Math::Vector3> positions;
    std::vector<float> colors;
    double simTime = 0.0;
    int scene = 0;
    int fluidCount = 0;
    int boundaryCount = 0;
    double avgDensityError = 0.0;
    double maxDensityError = 0.0;
    int pressureIterations = 0;
    int divergenceIterations = 0;
    double msPerStep = 0.0;
    bool hasDrop = false;
    uint64_t stepCount = 0;
};
using SimSnapshotPtr = std::shared_ptr<const SimSnapshot>;

struct SurfaceSnapshot {
    std::vector<float> vertices;    // interleaved pos(3) + normal(3)
    std::vector<unsigned> indices;
    uint64_t version = 0;
};
using SurfaceSnapshotPtr = std::shared_ptr<const SurfaceSnapshot>;

class SimulationSession : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QVariantMap params READ params NOTIFY paramsChanged)
    Q_PROPERTY(QVariantMap status READ status NOTIFY statusChanged)
    Q_PROPERTY(QVariantList densityTrace READ densityTrace NOTIFY traceChanged)
    Q_PROPERTY(int triangleCount READ triangleCount NOTIFY triangleCountChanged)

    // ---- v2 scene / object / camera state ---------------------------------
    // sceneIndex: 0 = dam break, 1 = inflow, 2 = empty (the default scene).
    Q_PROPERTY(bool isEmpty READ isEmpty NOTIFY sceneChanged)
    Q_PROPERTY(int sceneIndex READ sceneIndex NOTIFY sceneChanged)
    Q_PROPERTY(QVariantList objects READ objects NOTIFY objectsChanged)
    Q_PROPERTY(int selectedModelId READ selectedModelId NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap cameraState READ cameraState NOTIFY cameraStateChanged)

public:
    explicit SimulationSession(QObject* parent = nullptr);
    ~SimulationSession() override;

    void start();
    void shutdown();

    // ---- thread-safe reads ------------------------------------------------
    SimSnapshotPtr snapshot() const;
    SurfaceSnapshotPtr surface() const;
    // Legacy single-model view: the first *visible* imported model, or null.
    ModelDataPtr model() const;
    // Immutable multi-model snapshot for the renderer. Rebuilt whenever an
    // object changes; safe to read from the render thread (short lock).
    ModelSnapshotPtr models() const;
    ModelSnapshotPtr modelsForRender() const;

    bool running() const;
    QVariantMap params() const;
    QVariantMap status() const;
    QVariantList densityTrace() const;

    // ---- v2 UI state ------------------------------------------------------
    bool isEmpty() const;
    int sceneIndex() const;
    // Each item: id, name, path, position [x,y,z], rotation [w,x,y,z],
    // rotationEuler [degx,degy,degz], scale, visible, triangleCount.
    QVariantList objects() const;
    int selectedModelId() const;   // -1 when nothing is selected
    // {yaw, pitch, distance, targetX, targetY, targetZ, fov}
    QVariantMap cameraState() const;
    // Render-side config of every model: objects() keys plus a column-major
    // `transform` (16 doubles) that already includes the pivot re-centring.
    QVariantList modelConfigs() const;

    // Filled by the renderer (GUI thread) after each surface rebuild.
    int triangleCount() const { return m_triangleCount; }
    void setTriangleCount(int count);

    // ---- commands (queued onto the simulation thread) ----------------------
    Q_INVOKABLE void setRunning(bool running);
    Q_INVOKABLE void toggleRunning();
    Q_INVOKABLE void stepOnce();
    Q_INVOKABLE void resetSimulation();
    Q_INVOKABLE void importModel();
    Q_INVOKABLE bool importModelFile(const QString& filePath);
    Q_INVOKABLE void clearModel();
    Q_INVOKABLE void spawnDrop();

    // ---- v2 scene + object commands (all execute on the UI thread) ---------
    // Switching the scene clears every imported model (models only exist in
    // the Empty scene) and resets the camera to that scene's default.
    Q_INVOKABLE void setScene(int scene);
    // Multi-select import; only allowed in the Empty scene. A failing file is
    // reported and skipped, the rest still load.
    Q_INVOKABLE void importModels(const QStringList& paths);
    Q_INVOKABLE void clearModels();
    Q_INVOKABLE void selectModel(int id);            // -1 clears the selection
    Q_INVOKABLE void removeModel(int id);
    Q_INVOKABLE void setModelPosition(int id, double x, double y, double z);
    Q_INVOKABLE void setModelRotationEuler(int id, double x, double y, double z);
    Q_INVOKABLE void setModelRotationQuaternion(int id, double w, double x, double y,
                                                double z);
    // Alias used by the v2 contract documents.
    Q_INVOKABLE void setModelQuaternion(int id, double w, double x, double y, double z);
    Q_INVOKABLE void setModelScale(int id, double scale);
    Q_INVOKABLE void setModelVisible(int id, bool visible);
    Q_INVOKABLE void resetModelTransform(int id);
    // {radius, x, y, z} — used by spawnDrop(), the export and the render job.
    Q_INVOKABLE QVariantMap dropParams() const;
    Q_INVOKABLE void setDropParams(double radius, double x, double y, double z);
    Q_INVOKABLE void setViewportBackgroundHex(const QString& hex);
    Q_INVOKABLE void setCameraConfig(const QVariantMap& config);
    Q_INVOKABLE void applyCameraPreset(const QString& name);
    // The session has no viewport; Main.qml normally forwards the live camera
    // through setCameraConfig(). Kept for API completeness: re-publishes the
    // stored config so bindings refresh.
    Q_INVOKABLE void useCurrentViewCamera();

    // One generic entry point for every editable setting. Recognised keys:
    //   scene, spacing, solver, viewMode, speed, omega, eta, etaV, maxIter, dt,
    //   cfl, viscosity, surfaceTension, wallAdhesion, surfaceThreshold,
    //   gridScale, reconInterval
    Q_INVOKABLE void setParam(const QString& key, const QVariant& value);

    // Empty when the current backend can build `count` particles. On CUDA this
    // uses the cached device budget (no context switch); the CPU backend has no
    // upper limit. The main window and render dialog use this for live preflight
    // warnings before a scene rebuild or offline render is requested.
    Q_INVOKABLE QString particleBudgetError(int count) const;

    // ---- experiment files --------------------------------------------------
    Q_INVOKABLE QString exportParamsJson() const;
    Q_INVOKABLE bool importParamsJson(const QString& json);
    Q_INVOKABLE void importParameters();
    Q_INVOKABLE void exportParameters();
    Q_INVOKABLE bool importParametersFile(const QString& filePath);
    Q_INVOKABLE bool exportParametersFile(const QString& filePath);

Q_SIGNALS:
    void runningChanged();
    void paramsChanged();
    void statusChanged();
    void traceChanged();
    void surfaceUpdated();
    void modelChanged();
    void triangleCountChanged();
    void message(const QString& text);

    // ---- v2 ---------------------------------------------------------------
    void sceneChanged();
    void objectsChanged();
    void selectionChanged();
    void cameraStateChanged();

private:
    struct Impl;
    friend struct Impl;

    void postCommand(std::function<void(IFluidSolver&)> fn);
    void workerLoop();
    void reconLoop();
    // Serialises the current object list into the immutable render snapshot
    // and emits the change signals. Call on the UI thread.
    void publishModels();
    // Removes every model without asking; announce controls the message.
    void clearModelsInternal(bool announce);
    // Drops the published surface and the density trace (reset / scene switch).
    void resetSurfaceAndTrace();
    // Export with model paths made relative to `baseDir` when possible.
    QString exportParamsJsonWithBase(const QString& baseDir) const;

    std::unique_ptr<Impl> m_impl;
    int m_triangleCount = 0;
};
