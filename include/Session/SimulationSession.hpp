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

class FluidSolver;

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

public:
    explicit SimulationSession(QObject* parent = nullptr);
    ~SimulationSession() override;

    void start();
    void shutdown();

    // ---- thread-safe reads ------------------------------------------------
    SimSnapshotPtr snapshot() const;
    SurfaceSnapshotPtr surface() const;

    bool running() const;
    QVariantMap params() const;
    QVariantMap status() const;
    QVariantList densityTrace() const;

    // Filled by the renderer (GUI thread) after each surface rebuild.
    int triangleCount() const { return m_triangleCount; }
    void setTriangleCount(int count);

    // ---- commands (queued onto the simulation thread) ----------------------
    Q_INVOKABLE void setRunning(bool running);
    Q_INVOKABLE void toggleRunning();
    Q_INVOKABLE void stepOnce();
    Q_INVOKABLE void resetSimulation();
    Q_INVOKABLE void spawnDrop();

    // One generic entry point for every editable setting. Recognised keys:
    //   scene, spacing, solver, viewMode, speed, omega, eta, etaV, maxIter, dt,
    //   cfl, viscosity, surfaceTension, wallAdhesion, surfaceThreshold,
    //   gridScale, reconInterval
    Q_INVOKABLE void setParam(const QString& key, const QVariant& value);

    // ---- experiment files --------------------------------------------------
    Q_INVOKABLE QString exportParamsJson() const;
    Q_INVOKABLE bool importParamsJson(const QString& json);

Q_SIGNALS:
    void runningChanged();
    void paramsChanged();
    void statusChanged();
    void traceChanged();
    void surfaceUpdated();
    void triangleCountChanged();
    void message(const QString& text);

private:
    struct Impl;
    friend struct Impl;

    void postCommand(std::function<void(FluidSolver&)> fn);
    void workerLoop();
    void reconLoop();

    std::unique_ptr<Impl> m_impl;
    int m_triangleCount = 0;
};
