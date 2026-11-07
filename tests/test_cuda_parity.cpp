// CPU-vs-GPU parity test.
//
// Acceptance follows the staged rule agreed for this backend:
//   * one step   : positions/velocities/densities agree tightly (the device
//                  reproduces the reference arithmetic; neighbor traversal
//                  order differs, so the tolerance is not bit-exactness)
//   * many steps : the trajectories stay close enough that the physics is the
//                  same (chaotic divergence is expected, so this is a
//                  statistical bound, not a per-particle identity)
//
// Both IISPH and DFSPH are exercised. Exits non-zero when a bound is violated.

#include "Cuda/FluidSolverCuda.hpp"
#include "FluidSolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Diff {
    float maxPos = 0.0f;
    float maxVel = 0.0f;
    float maxRho = 0.0f;
    float meanAbsRho = 0.0f;
};

Diff Compare(const FluidSolver& cpu, const FluidSolverCuda& gpu) {
    Diff d;
    const size_t n = std::min(cpu.Positions().size(), gpu.Positions().size());
    for (size_t i = 0; i < n; ++i) {
        const auto& a = cpu.Positions()[i];
        const auto& b = gpu.Positions()[i];
        d.maxPos = std::max({d.maxPos, std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)});
        const auto& va = cpu.Velocities()[i];
        const auto& vb = gpu.Velocities()[i];
        d.maxVel = std::max({d.maxVel, std::fabs(va.x - vb.x), std::fabs(va.y - vb.y), std::fabs(va.z - vb.z)});
        d.maxRho = std::max(d.maxRho, std::fabs(cpu.Densities()[i] - gpu.Densities()[i]));
        d.meanAbsRho += std::fabs(cpu.Densities()[i] - gpu.Densities()[i]);
    }
    if (n) d.meanAbsRho /= (float)n;
    return d;
}

bool CheckCuda(std::string* error) {
    auto probe = FluidSolverCuda::Create(error);
    if (!probe) return false;
    std::printf("[cuda] backend: %s\n", probe->BackendName());
    return true;
}

bool RunCase(const char* label, int solverKind, int steps, float spacing,
             float singlePosTol, float multiPosTol, float rhoTol) {
    FluidSolver cpu;
    cpu.GetParams().spacing = spacing;
    cpu.GetParams().solver = solverKind;
    cpu.GetParams().useCFL = true;
    cpu.InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);

    std::string error;
    auto gpu = FluidSolverCuda::Create(&error);
    if (!gpu) {
        std::printf("[%s] SKIP: %s\n", label, error.c_str());
        return false;
    }
    gpu->GetParams() = cpu.GetParams();
    gpu->InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);

    std::printf("[%s] particles: cpu=%d gpu=%d (spacing %.4f)\n",
                label, cpu.FluidCount(), gpu->FluidCount(), spacing);
    if (cpu.FluidCount() != gpu->FluidCount()) {
        std::printf("[%s] FAIL: particle count mismatch\n", label);
        return false;
    }

    bool ok = true;
    for (int s = 1; s <= steps; ++s) {
        cpu.Step();
        gpu->Step();
        const Diff d = Compare(cpu, *gpu);
        const bool first = (s == 1);
        const float tol = first ? singlePosTol : multiPosTol;
        const bool pass = d.maxPos <= tol &&
                          (first ? d.maxVel <= 10.0f * tol : true) &&
                          (first ? d.maxRho <= rhoTol : true);
        if (s <= 3 || s == steps || !pass) {
            std::printf("[%s] step %3d  dpos=%.3e dvel=%.3e drho=%.3e (mean|drho|=%.3e) iter cpu/gpu=%d/%d  %s\n",
                        label, s, d.maxPos, d.maxVel, d.maxRho, d.meanAbsRho,
                        cpu.LastIterations(), gpu->LastIterations(), pass ? "ok" : "FAIL");
        }
        if (!pass) ok = false;
    }
    const int maxNb = gpu->MaxNeighbors();
    for (const auto& p : gpu->Positions()) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) ok = false;
    }
    for (const auto& v : gpu->Velocities()) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) ok = false;
    }
    if (maxNb > 64) {
        std::printf("[%s] FAIL: %d neighbors exceed the device list capacity (64)\n", label, maxNb);
        ok = false;
    }
    std::printf("[%s] %s (tolerances: step1 %.1e, later %.1e, rho %.1e; max neighbors %d)\n",
                label, ok ? "PASS" : "FAIL", singlePosTol, multiPosTol, rhoTol, maxNb);
    return ok;
}

}  // namespace

int main() {
    std::string error;
    if (!CheckCuda(&error)) {
        std::printf("[cuda] unavailable: %s\n", error.c_str());
        return 77;  // CTest "skipped" convention used by this project
    }
    const float sp = 0.06f;
    bool ok = true;
    ok &= RunCase("IISPH", 0, 20, sp, 1e-3f * sp, 5e-2f * sp, 2e-2f);
    ok &= RunCase("DFSPH", 1, 20, sp, 1e-3f * sp, 5e-2f * sp, 2e-2f);
    return ok ? 0 : 1;
}
