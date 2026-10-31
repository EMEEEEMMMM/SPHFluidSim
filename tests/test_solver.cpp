// Headless numerical sanity test for the IISPH solver.
// Compile: g++ -std=c++17 -I../include test_solver.cpp ../src/FluidSolver.cpp -o test_solver
#include "FluidSolver.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>

int main(int argc, char** argv) {
    FluidSolver solver;
    solver.Init(2.0f, 1.2f, 1.0f, 0.8f);
    if (argc > 2) solver.GetParams().dt = (float)atof(argv[2]);   // optional dt override
    if (argc > 3 && atoi(argv[3]) == 1) solver.GetParams().gravity = 0.0f;  // hydrostatic test
    if (argc > 4) solver.GetParams().viscosity = (float)atof(argv[4]);      // XSPH coefficient
    if (argc > 5) {   // "DFSPH" (also accepts the old "df" / "df sph" spellings)
        std::string sv = argv[5];
        for (auto& c : sv) c = (char)std::tolower((unsigned char)c);
        if (sv == "dfsph" || sv == "df sph" || sv == "df") solver.GetParams().solver = 1;
    }

    const int N = solver.FluidCount();
    const int B = solver.BoundaryCount();
    printf("fluid particles: %d, boundary particles: %d\n", N, B);
    if (N == 0) { printf("FAIL: no fluid particles\n"); return 1; }

    // --- Step 0: initial density sanity (run one predict only via a step) ---
    solver.Step();
    float err = solver.AvgDensityError();
    int   iters = solver.LastIterations();
    printf("after 1 step: avg density err = %.4f%%, pressure iters = %d\n",
           err * 100.0f, iters);

    // --- Run steps, overridable via argv[1]; report every 50 steps ---
    const int steps = (argc > 1) ? atoi(argv[1]) : 300;
    float maxSpeed = 0.0f;
    float maxErr = 0.0f;
    int   maxDivIter = 0;   // DFSPH divergence solver: a runaway here means the convergence test is broken
    float maxMeanAbs = 0.0f;   // mean |density error|: the signed mean can cancel out
    bool nanFound = false;
    int outOfTank = 0;
    for (int s = 0; s < steps; s++) {
        solver.Step();
        // |err| works for both solvers: IISPH reports a positive algebraic
        // residual, DFSPH a signed average density error that can be negative.
        maxErr = std::max(maxErr, std::fabs(solver.AvgDensityError()));
        maxDivIter = std::max(maxDivIter, solver.DivergenceIterations());
        maxMeanAbs = std::max(maxMeanAbs, solver.MeanAbsDensityError());
        const auto& pos = solver.Positions();
        const auto& vel = solver.Velocities();
        for (size_t i = 0; i < pos.size() && i < vel.size(); i++) {
            maxSpeed = std::max(maxSpeed, vel[i].length());
            if (!std::isfinite(pos[i].x) || !std::isfinite(pos[i].y) || !std::isfinite(pos[i].z)) {
                nanFound = true;
                break;
            }
            if (pos[i].x < -0.01f || pos[i].x > 2.01f || pos[i].y < -0.01f || pos[i].y > 1.21f ||
                pos[i].z < -0.01f || pos[i].z > 1.01f)
                outOfTank++;
        }
        if (nanFound) break;
        if ((s + 1) % 50 == 0)
            printf("  step %d: avg err = %.3f%%, iters = %d\n",
                   s + 1, solver.AvgDensityError() * 100.0f, solver.LastIterations());
    }

    printf("after %d steps: sim time = %.3f s, max avg-density-err = %.4f%%, max speed = %.3f m/s, out-of-tank = %d\n",
           steps, solver.SimTime(), maxErr * 100.0f, maxSpeed, outOfTank);
    if (nanFound) { printf("FAIL: NaN in positions\n"); return 1; }
    if (outOfTank > 100) { printf("FAIL: too many particles left the tank\n"); return 1; }
    if (maxErr > 0.05f) { printf("FAIL: density error exceeded 5%%\n"); return 1; }
    if (maxErr <= 0.0f)  { printf("FAIL: density error never computed\n"); return 1; }

    // Regression guard: the DFSPH divergence solver must converge in a few
    // iterations. A broken tolerance test (e.g. comparing a mean |value|
    // against a signed threshold) makes it run to maxIter on EVERY step and
    // visibly shakes the fluid apart.
    if (solver.GetParams().solver == 1 && maxDivIter > 20) {
        printf("FAIL: divergence solver hit %d iterations (expected < 20)\n", maxDivIter);
        return 1;
    }
    // Local-error guard: a signed average can hide a compressed core next to
    // a dilated surface, so the mean ABSOLUTE error is bounded as well.
    // 10% is a loose ceiling for the violent cold-start phase of this scene.
    if (maxMeanAbs > 0.10f) {
        printf("FAIL: mean absolute density error %.2f%% > 10%%\n", 100.0f * maxMeanAbs);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
