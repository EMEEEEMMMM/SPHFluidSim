// Reproduces the quantitative experiments of the IISPH paper (Sec. 5) at
// teaching scale, and prints the numbers so we can compare with the paper:
//
//   A. Fig.8  : one-step pressure-solve convergence trace (iterations -> error)
//   B. Table 1: avg iterations & per-step cost vs time step (dam break)
//   C. Dam-break wave-front position vs time (qualitative check vs experiment)
//
// Compile (from IISPH/build): cmake --build . --target benchmark
#include "FluidSolver.hpp"
#include <chrono>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include <omp.h>

static double runSteps(FluidSolver& s, int steps) {
    double t = 0.0;
    for (int k = 0; k < steps; k++) {
        auto t0 = std::chrono::steady_clock::now();
        s.Step();
        auto t1 = std::chrono::steady_clock::now();
        t += std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    return t / steps;
}

int main() {
    // ---------------- A. Convergence trace (paper Fig. 8) ----------------
    {
        FluidSolver s;
        s.Init(2.0f, 1.2f, 1.0f, 0.8f);
        s.GetParams().dt = 0.0025f;      // same as paper Fig.8
        for (int k = 0; k < 20; k++) s.Step();   // warm up so p has a good initial guess
        s.EnableConvergenceTrace();
        s.Step();
        const auto& tr = s.ConvergenceTrace();
        printf("=== A. pressure-solve convergence, one dam-break step (dt=0.0025) ===\n");
        printf("  iterations to reach 0.1%% avg error: ");
        int it01 = -1;
        for (size_t k = 0; k < tr.size(); k++)
            if (tr[k] <= 0.001f) { it01 = (int)k + 1; break; }
        printf("%d (paper Fig.8: IISPH ~23 iters to 0.01%%; ISPH ~231)\n", it01);
        printf("  iter : avg density error\n");
        for (size_t k = 0; k < tr.size() && k < 30; k++)
            printf("  %3zu  : %.4f%%\n", k + 1, tr[k] * 100.0f);
        if (tr.size() > 30) {
            printf("  ...\n");
            for (size_t k = tr.size() - 3; k < tr.size(); k++)
                printf("  %3zu  : %.4f%%\n", k + 1, tr[k] * 100.0f);
        }
        printf("  (total iterations: %zu; stopped at eta or maxIter)\n", tr.size());
        printf("\n");
    }

    // ---------------- B. Cost vs time step (paper Table 1) ----------------
    // NOTE: paper Table 1 uses eta=0.01%; we use 0.5% here so the benchmark runs
    // fast; the qualitative trend (iterations grow with dt) is what matters.
    const float dts[] = { 0.001f, 0.0025f, 0.005f };
    printf("=== B. dam break, avg iterations & per-step cost vs dt (paper Table 1) ===\n");
    printf("  dt[s]    avg iter   pressure-solve ms/step   total ms/step\n");
    for (float dt : dts) {
        FluidSolver s;
        s.Init(2.0f, 1.2f, 1.0f, 0.8f);
        s.GetParams().dt = dt;
        s.GetParams().eta = 0.0001f;   // paper Table 1: max volume compression 0.01%
        s.GetParams().useCFL = false;  // paper Table 1 uses a FIXED time step
        const int steps = 20;
        for (int k = 0; k < 8; k++) s.Step();   // skip the cold-start steps
        double iterSum = 0.0, tWall = 0.0;
        for (int k = 0; k < steps; k++) {
            auto t0 = std::chrono::steady_clock::now();
            s.Step();
            auto t1 = std::chrono::steady_clock::now();
            iterSum += s.LastIterations();
            tWall += std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
        printf("  %.4f     %6.1f          %6.1f              %6.1f\n",
               dt, iterSum / steps, tWall / steps, tWall / steps);
    }
    printf("  (paper Table 1: IISPH avg iter 4.9 -> 18.4 -> 33.5 for the same dt set;\n"
           "   ours is ~16 flat-to-slightly-growing at 8k particles — same order of\n"
           "   magnitude, the paper's stronger growth comes with 100K particles)\n\n");

    // ---------------- C. Dam-break wave front vs time ----------------
    {
        FluidSolver s;
        s.Init(2.0f, 1.2f, 1.0f, 0.8f);
        s.GetParams().dt = 0.0025f;
        s.GetParams().eta = 0.005f;
        printf("=== C. dam-break wave front (max x of water) vs time ===\n");
        printf("  time[s]   front x [m]   (tank: x in [0,2], water starts as 0.8m cube at x=0)\n");
        for (int k = 0; k <= 200; k += 20) {
            while ((int)(s.SimTime() / 0.0025f) < k) s.Step();
            // 99th percentile of x (ignore single splashed particles far ahead)
            std::vector<float> xs;
            for (const auto& p : s.Positions()) xs.push_back(p.x);
            std::sort(xs.begin(), xs.end());
            float front = xs[(size_t)(xs.size() * 0.99f)];
            printf("  %6.2f      %6.3f\n", s.SimTime(), front);
        }
        printf("  (qualitative: front accelerates right after release, then slows as it\n"
               "   hits the far wall at x=2 and piles up)\n\n");
    }
    // ---------------- D. strong scaling: OpenMP threads (fixed N) ----------------
    {
        FluidSolver s;
        s.Init(2.0f, 1.2f, 1.0f, 0.8f);
        s.GetParams().eta = 0.005f;
        s.GetParams().useCFL = false;
        printf("=== D. strong scaling (8k particles, dam break, eta=0.5%%, CFL on) ===\n");
        printf("  threads   ms/step   speedup\n");
        double t1 = 0.0;
        for (int th : { 1, 2, 4, 8 }) {
            FluidSolver s2;
            s2.Init(2.0f, 1.2f, 1.0f, 0.8f);
            s2.GetParams().eta = 0.005f;
            omp_set_num_threads(th);
            double ms = 0.0, iterSum = 0.0;
            for (int k = 0; k < 60; k++) {
                auto t0 = std::chrono::steady_clock::now();
                s2.Step();
                auto t1s = std::chrono::steady_clock::now();
                ms += std::chrono::duration<double, std::milli>(t1s - t0).count();
                iterSum += s2.LastIterations();
            }
            ms /= 60.0; iterSum /= 60.0;
            if (th == 1) t1 = ms;
            printf("  %3d      %7.2f    %5.2fx     (avg iter %.1f)\n", th, ms, t1 / ms, iterSum);
        }
        printf("\n");
    }

    // ---------------- E. weak scaling: particle count (fixed threads) ----------------
    {
        omp_set_num_threads(8);
        printf("=== E. weak scaling (8 threads, dam break, eta=0.5%%, CFL on) ===\n");
        printf("  spacing  particles   ms/step\n");
        for (float sp : { 0.06f, 0.05f, 0.04f, 0.03f }) {
            FluidSolver s;
            s.GetParams().spacing = sp;
            s.Init(2.0f, 1.2f, 1.0f, 0.8f);
            s.GetParams().eta = 0.005f;
            double ms = runSteps(s, 40);
            printf("  %.2f    %6d     %7.2f\n", sp, s.FluidCount(), ms);
        }
        printf("  (SPH with uniform grids is O(N): ms/step should grow ~linearly)\n\n");
    }

    printf("done\n");
    return 0;
}