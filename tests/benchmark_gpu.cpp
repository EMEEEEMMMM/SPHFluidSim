// CPU vs GPU benchmark for the same dam-break workload.
//
// Two GPU numbers are reported:
//   solver : Step() only, with particle state resident on the device
//   render : Step() plus the host mirror needed by the renderer (positions and
//            colors), which is the end-to-end cost in the interactive app
//
// Usage: benchmark_gpu [steps] [--large] [--render=0]
//   --large   also run ~131k-particle cases
//   --render=0  skip the (more expensive) render-synchronised pass

#include "Cuda/FluidSolverCuda.hpp"
#include "FluidSolver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <omp.h>

namespace {

using Clock = std::chrono::steady_clock;

double MsPerStep(Clock::time_point a, Clock::time_point b, int steps) {
    return std::chrono::duration<double, std::milli>(b - a).count() / std::max(steps, 1);
}

template <class Solver>
double TimeCpu(Solver& s, int warmup, int steps) {
    for (int i = 0; i < warmup; ++i) s.Step();
    const auto t0 = Clock::now();
    for (int i = 0; i < steps; ++i) s.Step();
    const auto t1 = Clock::now();
    return MsPerStep(t0, t1, steps);
}

double TimeGpuSolver(FluidSolverCuda& s, int warmup, int steps) {
    for (int i = 0; i < warmup; ++i) s.Step();
    const auto t0 = Clock::now();
    for (int i = 0; i < steps; ++i) s.Step();
    const auto t1 = Clock::now();
    return MsPerStep(t0, t1, steps);
}

double TimeGpuRender(FluidSolverCuda& s, int warmup, int steps) {
    for (int i = 0; i < warmup; ++i) s.Step();
    const auto t0 = Clock::now();
    for (int i = 0; i < steps; ++i) {
        s.Step();
        // Exactly the two host mirrors the particle renderer consumes.
        (void)s.Positions();
        (void)s.Colors();
    }
    const auto t1 = Clock::now();
    return MsPerStep(t0, t1, steps);
}

struct Case {
    const char* label;
    int solver;
    float spacing;
    bool large;
};

bool AllFinite(const FluidSolverCuda& s) {
    for (const auto& p : s.Positions()) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    }
    for (float r : s.Densities()) {
        if (!std::isfinite(r)) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int steps = 30;
    bool large = false;
    bool renderPass = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--large") == 0) large = true;
        else if (std::strncmp(argv[i], "--render=", 9) == 0) renderPass = std::atoi(argv[i] + 9) != 0;
        else steps = std::max(1, std::atoi(argv[i]));
    }

    std::string error;
    auto gpuProbe = FluidSolverCuda::Create(&error);
    if (!gpuProbe) {
        std::printf("[cuda] unavailable: %s\n", error.c_str());
        return 77;
    }
    const std::string device = gpuProbe->BackendName();
    gpuProbe.reset();

    std::printf("device: %s   (CPU: OpenMP, %d threads)\n", device.c_str(), omp_get_max_threads());
    std::printf("%-26s %9s %9s %9s %8s %8s %6s\n",
                "case", "cpu", "gpu", "gpu+rndr", "speedup", "rndr-x", "maxnb");

    std::vector<Case> cases = {
        {"IISPH 8k  (spacing .04)", 0, 0.04f, false},
        {"IISPH 32.8k(spacing .025)", 0, 0.025f, false},
        {"DFSPH 8k  (spacing .04)", 1, 0.04f, false},
        {"DFSPH 32.8k(spacing .025)", 1, 0.025f, false},
    };
    if (large) {
        cases.push_back({"IISPH 125k(spacing .016)", 0, 0.016f, true});
        cases.push_back({"DFSPH 125k(spacing .016)", 1, 0.016f, true});
    }

    bool anyFail = false;
    for (const Case& c : cases) {
        // Less warmup for the large cases so the default invocation stays usable
        // on a small display GPU; the measured loop is still the same.
        const int warmup = c.large ? 3 : 5;

        FluidSolver cpu;
        cpu.GetParams().spacing = c.spacing;
        cpu.GetParams().solver = c.solver;
        cpu.InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);

        auto gpu = FluidSolverCuda::Create(&error);
        if (!gpu) {
            std::printf("%-26s  SKIP: %s\n", c.label, error.c_str());
            anyFail = true;
            continue;
        }
        gpu->GetParams() = cpu.GetParams();
        gpu->InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);

        // Render-sync gets its own solver so both GPU numbers cover the same
        // trajectory length instead of the second pass running on a settled
        // state with fewer pressure iterations.
        std::unique_ptr<FluidSolverCuda> gpuRenderSolver;
        if (renderPass) {
            gpuRenderSolver = FluidSolverCuda::Create(&error);
            if (gpuRenderSolver) {
                gpuRenderSolver->GetParams() = cpu.GetParams();
                gpuRenderSolver->InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);
            }
        }

        const double cpuMs = TimeCpu(cpu, warmup, steps);
        const double gpuMs = TimeGpuSolver(*gpu, warmup, steps);
        const double gpuRender = gpuRenderSolver
            ? TimeGpuRender(*gpuRenderSolver, warmup, steps) : 0.0;
        const bool finite = AllFinite(*gpu) && (!gpuRenderSolver || AllFinite(*gpuRenderSolver));
        if (!finite) anyFail = true;

        std::printf("%-26s %9.2f %9.2f %9.2f %7.2fx %7.2fx %6d  n=%d%s\n",
                    c.label, cpuMs, gpuMs, gpuRender,
                    cpuMs / gpuMs, gpuRender > 0 ? cpuMs / gpuRender : 0.0,
                    gpu->MaxNeighbors(), gpu->FluidCount(), finite ? "" : "  NONFINITE");
        if (cpuMs / gpuMs < 1.0) anyFail = true;
    }
    return anyFail ? 1 : 0;
}
