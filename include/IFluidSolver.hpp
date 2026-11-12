#pragma once
#include "FluidParams.hpp"
#include "Math/Vector3.hpp"
#include <stdexcept>
#include <string>
#include <vector>

// Backend-agnostic SPH solver interface.
//
// The CPU implementation (FluidSolver) is the reference: bit-reproducible,
// single-threaded-equivalent semantics, OpenMP parallel. The CUDA backend
// (FluidSolverCuda) mirrors every pass on the device and is expected to match
// the CPU results first on a single step (tight tolerance) and then
// statistically over many steps.
//
// Contract for implementations:
//  - Step() advances exactly one time step with the current params.
//  - Scene construction (Init*/SpawnDrop/Reset) leaves the solver in the same
//    state as the CPU reference would.
//  - The accessors return host-side vectors that are valid after Step() and
//    after scene construction. A GPU backend keeps a host mirror and refreshes
//    it lazily.
// A scene request that exceeds the backend's device budget. This is a user
// input error, not a broken solver: the current scene remains valid and keeps
// running while the UI reports the refusal.
class ParticleBudgetError : public std::runtime_error {
public:
    explicit ParticleBudgetError(const std::string& message) : std::runtime_error(message) {}
};

// Device-memory budget of a backend. Everything is zero for the CPU solver.
struct GpuMemoryInfo {
    long long freeBytes = 0;         // free device memory when last measured
    long long totalBytes = 0;        // device memory in total
    long long bytesPerParticle = 0;  // steady-state device bytes per fluid particle
};

class IFluidSolver {
public:
    using Params = FluidParams;

    virtual ~IFluidSolver() = default;

    // --- scene setup -------------------------------------------------------
    virtual void Init(float boxX, float boxY, float boxZ, float waterSize) = 0;
    virtual void InitDamOpen(float boxX, float boxY, float boxZ, float waterSize) = 0;
    virtual void InitFull(float boxX, float boxY, float boxZ, float waterHeight) = 0;
    virtual void InitInflow(float tankSize, float gap, int inflowPerStep, float inflowRadius,
                            float cx, float cz, int maxParticles) = 0;
    virtual void InitDrop(float tankSize, float poolHeight, float dropRadius, float dropGap,
                          float cx, float cz, int maxParticles) = 0;
    // Empty scene (v2): no fluid, no boundary, no tank. The solver keeps no
    // particles but Step() must still advance SimTime() by dt so that
    // headless_timing::AdvanceTo (while (SimTime() < target) Step()) terminates.
    virtual void InitEmpty() = 0;
    virtual void SpawnDrop(float radius, float cx, float cy, float cz) = 0;
    virtual bool HasDrop() const = 0;
    virtual void Reset() = 0;

    // --- simulation --------------------------------------------------------
    virtual void Step() = 0;

    // --- state accessors ---------------------------------------------------
    virtual const std::vector<Math::Vector3>& Positions() const = 0;
    virtual const std::vector<Math::Vector3>& Velocities() const = 0;
    virtual const std::vector<float>&         Densities() const = 0;   // rhoN incl. boundary
    virtual const std::vector<float>&         Colors() const = 0;      // 3 floats per particle
    virtual const std::vector<float>&         Pressures() const = 0;   // normalized pressure
    virtual const std::vector<Math::Vector3>& BoundaryPositions() const = 0;

    virtual float SimTime() const = 0;
    virtual float AvgDensityError() const = 0;
    virtual float MeanDensity() const = 0;
    virtual float MeanAbsDensityError() const = 0;
    virtual float MaxDensityError() const = 0;
    virtual int   LastIterations() const = 0;
    virtual int   DivergenceIterations() const = 0;
    virtual float LastPressureTimeMs() const = 0;
    virtual int   FluidCount() const = 0;
    virtual int   BoundaryCount() const = 0;

    virtual Params& GetParams() = 0;
    virtual const Params& GetParams() const = 0;
    virtual float KernelRadius() const = 0;
    virtual float Spacing() const = 0;

    // Device-memory budget for pre-flight checks. Cached at scene build time, so
    // this is safe to call from the UI thread (no CUDA context switch).
    virtual GpuMemoryInfo GpuMemory() const = 0;

    // Throws std::runtime_error when a scene with roughly this many fluid
    // particles cannot be built on this backend (device memory exhausted).
    // No-op for the CPU backend. Callers use it before rebuilding a scene so a
    // refused request leaves the running simulation untouched.
    virtual void CheckParticleBudget(int particleCount) const = 0;

    // --- diagnostics (paper Sec.5 helpers) ---------------------------------
    virtual void EnableConvergenceTrace() = 0;
    virtual const std::vector<float>& ConvergenceTrace() const = 0;
    virtual int NeighborCount(int i) const = 0;
    virtual int MaxNeighbors() const = 0;

    // Human-readable backend tag, e.g. "CPU (OpenMP)" / "CUDA (Quadro P600)".
    virtual const char* BackendName() const = 0;
};
