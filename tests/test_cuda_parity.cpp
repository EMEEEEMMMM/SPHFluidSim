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
#include <stdexcept>
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

// Renderer-facing color mirror. The GPU colors are derived from the current
// velocities, so checking only positions/densities misses a stale host copy
// that makes every particle black.
bool CheckColors(const char* label, const FluidSolverCuda& gpu, const char* stage) {
    const int n = gpu.FluidCount();
    const auto& colors = gpu.Colors();
    const auto& velocities = gpu.Velocities();
    bool ok = colors.size() == static_cast<size_t>(n) * 3 &&
              velocities.size() == static_cast<size_t>(n);
    float vmax = 1e-6f;
    for (const auto& v : velocities) {
        vmax = std::max(vmax, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    }
    for (int i = 0; ok && i < n; ++i) {
        const auto& v = velocities[i];
        const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        const float s = std::min(1.0f, speed / vmax);
        const float expected[3] = {0.15f + 0.85f * s, 0.35f + 0.65f * s, 1.0f};
        for (int c = 0; c < 3; ++c) {
            const float value = colors[3 * i + c];
            if (!std::isfinite(value) || std::fabs(value - expected[c]) > 2e-5f) {
                ok = false;
                break;
            }
        }
    }
    if (n > 0) {
        std::printf("[colors] %s %-11s n=%d first=(%.3f,%.3f,%.3f) -> %s\n",
                    label, stage, n, colors[0], colors[1], colors[2],
                    ok ? "PASS" : "FAIL");
    } else {
        std::printf("[colors] %s %-11s n=0 -> FAIL\n", label, stage);
        ok = false;
    }
    return ok;
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

    bool ok = CheckColors(label, *gpu, "initial");
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
    ok &= CheckColors(label, *gpu, "after steps");
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

bool RunCachedSceneMutations() {
    std::string error;
    auto gpu = FluidSolverCuda::Create(&error);
    if (!gpu) return false;
    gpu->GetParams().spacing = 0.05f;
    gpu->GetParams().solver = 1;   // DFSPH exercises the across-step caches
    gpu->InitInflow(1.0f, 0.3f, 100, 0.08f, 0.5f, 0.5f, 4000);
    const int n0 = gpu->FluidCount();
    for (int i = 0; i < 20; ++i) gpu->Step();
    const int afterInflow = gpu->FluidCount();
    const bool inflowOk = afterInflow > n0;

    gpu->InitDamOpen(1.0f, 0.8f, 1.0f, 0.4f);
    for (int i = 0; i < 5; ++i) gpu->Step();
    const int beforeDrop = gpu->FluidCount();
    gpu->SpawnDrop(0.12f, 0.5f, 1.0f, 0.5f);
    for (int i = 0; i < 5; ++i) gpu->Step();
    const bool dropOk = gpu->FluidCount() > beforeDrop;
    bool finite = true;
    for (const auto& p : gpu->Positions()) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) finite = false;
    }
    finite = finite && gpu->MaxNeighbors() <= 64;
    const bool colorsOk = CheckColors("scene-cache", *gpu, "after mutate");
    const bool ok = inflowOk && dropOk && finite && colorsOk;
    std::printf("[scene-cache] inflow %d->%d, drop %d->%d, finite/neighbors %s, colors %s -> %s\n",
                n0, afterInflow, beforeDrop, gpu->FluidCount(), finite ? "ok" : "bad",
                colorsOk ? "ok" : "bad", ok ? "PASS" : "FAIL");
    return ok;
}

// A request that cannot fit must be refused BEFORE the device is touched, so a
// running scene survives a too-large particle count.
bool RunMemoryGuard() {
    std::string error;
    auto gpu = FluidSolverCuda::Create(&error);
    if (!gpu) return false;
    gpu->GetParams().spacing = 0.05f;
    gpu->GetParams().solver = 1;
    gpu->InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);
    const int before = gpu->FluidCount();
    const GpuMemoryInfo mem = gpu->GpuMemory();
    if (mem.totalBytes <= 0 || mem.bytesPerParticle <= 0) {
        std::printf("[budget] no device memory info -> SKIP\n");
        return true;
    }
    // Twice what the whole card could ever hold, whatever its size.
    const int tooMany = static_cast<int>(
        std::min<long long>(2000000000LL, 2 * mem.totalBytes / mem.bytesPerParticle));
    bool refused = false;
    std::string why;
    try {
        gpu->CheckParticleBudget(tooMany);
    } catch (const std::exception& ex) {
        refused = true;
        why = ex.what();
    }
    const bool intact = gpu->FluidCount() == before;
    bool steps = true;
    try {
        gpu->Step();
        steps = gpu->FluidCount() == before;
    } catch (const std::exception&) {
        steps = false;
    }
    const bool ok = refused && intact && steps;
    std::printf("[budget] %lld MiB total, %d particles refused=%d, scene intact=%d, "
                "still stepping=%d -> %s\n%s%s\n",
                mem.totalBytes / (1024 * 1024), tooMany, refused ? 1 : 0, intact ? 1 : 0,
                steps ? 1 : 0, ok ? "PASS" : "FAIL", why.c_str(), why.empty() ? "" : "\n");
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
    ok &= RunCachedSceneMutations();
    ok &= RunMemoryGuard();
    return ok ? 0 : 1;
}
