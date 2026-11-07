#pragma once
#include "IFluidSolver.hpp"

#include <memory>
#include <string>

// CUDA backend. Same public contract as FluidSolver (the CPU reference), but
// every pass runs on the device and the particle state stays resident in GPU
// memory. The host mirror is refreshed lazily when an accessor asks for a
// field, so a pure simulation loop never pays a per-step device-to-host copy.
//
// The backend is compiled only when SPH_ENABLE_CUDA is on and the CUDA include
// directory is available. It loads nvcuda.dll and nvrtc64_*.dll at runtime
// (NVRTC JIT), so the build itself needs no CUDA import libraries; Create()
// returns nullptr with a reason when the runtime is missing, letting the caller
// fall back to the CPU solver.
class FluidSolverCuda final : public IFluidSolver {
public:
    // Returns nullptr and fills *error when CUDA/NVRTC is unavailable or the
    // kernels fail to compile. Never throws.
    static std::unique_ptr<FluidSolverCuda> Create(std::string* error);

    ~FluidSolverCuda() override;

    FluidSolverCuda(const FluidSolverCuda&) = delete;
    FluidSolverCuda& operator=(const FluidSolverCuda&) = delete;

    // --- scene setup -------------------------------------------------------
    void Init(float boxX, float boxY, float boxZ, float waterSize) override;
    void InitDamOpen(float boxX, float boxY, float boxZ, float waterSize) override;
    void InitFull(float boxX, float boxY, float boxZ, float waterHeight) override;
    void InitInflow(float tankSize, float gap, int inflowPerStep, float inflowRadius,
                    float cx, float cz, int maxParticles) override;
    void InitDrop(float tankSize, float poolHeight, float dropRadius, float dropGap,
                  float cx, float cz, int maxParticles) override;
    void SpawnDrop(float radius, float cx, float cy, float cz) override;
    bool HasDrop() const override;
    void Reset() override;

    // --- simulation --------------------------------------------------------
    void Step() override;

    // --- state accessors ---------------------------------------------------
    const std::vector<Math::Vector3>& Positions() const override;
    const std::vector<Math::Vector3>& Velocities() const override;
    const std::vector<float>&         Densities() const override;
    const std::vector<float>&         Colors() const override;
    const std::vector<float>&         Pressures() const override;
    const std::vector<Math::Vector3>& BoundaryPositions() const override;

    float SimTime() const override;
    float AvgDensityError() const override;
    float MeanDensity() const override;
    float MeanAbsDensityError() const override;
    float MaxDensityError() const override;
    int   LastIterations() const override;
    int   DivergenceIterations() const override;
    float LastPressureTimeMs() const override;
    int   FluidCount() const override;
    int   BoundaryCount() const override;

    Params& GetParams() override;
    const Params& GetParams() const override;
    float KernelRadius() const override;
    float Spacing() const override;

    void EnableConvergenceTrace() override;
    const std::vector<float>& ConvergenceTrace() const override;
    int NeighborCount(int i) const override;
    int MaxNeighbors() const override;

    const char* BackendName() const override;

private:
    explicit FluidSolverCuda(Params p);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
