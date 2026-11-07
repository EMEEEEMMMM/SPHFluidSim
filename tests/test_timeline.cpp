#include "HeadlessRunner.hpp"
#include "FluidSolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

void ConfigureSolver(FluidSolver& solver) {
    // A small dam keeps the test quick while still exercising CFL, pressure
    // solves, free-surface motion and (in the second case) particle insertion.
    solver.GetParams().spacing = 0.06f;
    solver.GetParams().dt = 0.003f;
    solver.GetParams().useCFL = true;
    solver.InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);
}

void RunTimeline(FluidSolver& solver, int fps, double videoSeconds,
                 double timeScale, double dropVideoSeconds) {
    const int frames = static_cast<int>(videoSeconds * fps + 0.5);
    const double dropSimTime = dropVideoSeconds >= 0.0
        ? dropVideoSeconds * timeScale
        : -1.0;
    auto spawnDropIfDue = [&] {
        if (dropSimTime < 0.0 || solver.HasDrop() ||
            static_cast<double>(solver.SimTime()) < dropSimTime) return;
        solver.SpawnDrop(0.2f, 1.0f, 1.6f, 0.5f);
    };

    spawnDropIfDue();
    for (int f = 0; f < frames; ++f) {
        const double target = headless_timing::FrameTargetSimTime(f, fps, timeScale);
        headless_timing::AdvanceTo(solver, target, spawnDropIfDue);
    }
}

bool SameVector(const Math::Vector3& a, const Math::Vector3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool CompareFinalState(const FluidSolver& a, const FluidSolver& b,
                       const char* label) {
    bool same = a.SimTime() == b.SimTime() &&
                a.FluidCount() == b.FluidCount() &&
                a.HasDrop() == b.HasDrop() &&
                a.Positions().size() == b.Positions().size() &&
                a.Velocities().size() == b.Velocities().size() &&
                a.Densities().size() == b.Densities().size() &&
                a.Pressures().size() == b.Pressures().size();

    float maxPositionDiff = 0.0f;
    float maxVelocityDiff = 0.0f;
    float maxDensityDiff = 0.0f;
    float maxPressureDiff = 0.0f;
    if (same) {
        for (size_t i = 0; i < a.Positions().size(); ++i) {
            const auto& pa = a.Positions()[i];
            const auto& pb = b.Positions()[i];
            const auto& va = a.Velocities()[i];
            const auto& vb = b.Velocities()[i];
            same = SameVector(pa, pb) && SameVector(va, vb) && same;
            maxPositionDiff = std::max({maxPositionDiff,
                std::fabs(pa.x - pb.x), std::fabs(pa.y - pb.y), std::fabs(pa.z - pb.z)});
            maxVelocityDiff = std::max({maxVelocityDiff,
                std::fabs(va.x - vb.x), std::fabs(va.y - vb.y), std::fabs(va.z - vb.z)});
        }
        for (size_t i = 0; i < a.Densities().size(); ++i) {
            same = a.Densities()[i] == b.Densities()[i] && same;
            maxDensityDiff = std::max(maxDensityDiff,
                                      std::fabs(a.Densities()[i] - b.Densities()[i]));
        }
        for (size_t i = 0; i < a.Pressures().size(); ++i) {
            same = a.Pressures()[i] == b.Pressures()[i] && same;
            maxPressureDiff = std::max(maxPressureDiff,
                                       std::fabs(a.Pressures()[i] - b.Pressures()[i]));
        }
    }

    printf("[timeline] %s: N=%d, sim time %.9g s, drop=%d\n",
           label, a.FluidCount(), a.SimTime(), a.HasDrop() ? 1 : 0);
    printf("[timeline] max diffs: position=%g velocity=%g density=%g pressure=%g\n",
           maxPositionDiff, maxVelocityDiff, maxDensityDiff, maxPressureDiff);
    printf("[timeline] %s\n", same ? "PASS" : "FAIL");
    return same;
}

bool RunCase(const char* label, double videoSeconds, double timeScale,
             double dropVideoSeconds) {
    FluidSolver at30;
    FluidSolver at120;
    ConfigureSolver(at30);
    ConfigureSolver(at120);
    RunTimeline(at30, 30, videoSeconds, timeScale, dropVideoSeconds);
    RunTimeline(at120, 120, videoSeconds, timeScale, dropVideoSeconds);
    return CompareFinalState(at30, at120, label);
}

} // namespace

int main() {
    // Both video durations contain an integer number of frames at 30 and 120
    // fps. Their final target simulation times must therefore be identical.
    const bool realTime = RunCase("real-time, no drop", 0.1, 1.0, -1.0);
    const bool slowWithDrop = RunCase("0.25x with drop", 0.2, 0.25, 0.04);
    return realTime && slowWithDrop ? 0 : 1;
}
