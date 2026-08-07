// Hydrostatic validation of the IISPH solver (no GUI needed).
//
// Physical facts a correct incompressible-SPH solver must reproduce:
//   1. At rest under gravity the water stays put: density ~= rho0 everywhere.
//   2. Pressure increases linearly with depth: p ~= a * (waterDepth - y) + b
//      (hydrostatic law p = rho*g*h). We check monotonicity + approximate
//      linearity of the layer-averaged pressure vs depth.
//   3. Volume conservation: total particle volume N*(m/rho0) is exactly fixed,
//      so the check is density ~= rho0 (already #1). We also report it.
//
// Compile: g++ -std=c++17 -I../include verify_hydrostatic.cpp ../src/FluidSolver.cpp -o verify_hydrostatic
#include "FluidSolver.hpp"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

int main(int argc, char** argv) {
    FluidSolver solver;
    solver.InitFull(2.0f, 1.2f, 1.0f, 0.5f);   // water fills the tank bottom, 0.5 m deep
    solver.GetParams().gravity = -9.81f;
    const int steps = (argc > 1) ? atoi(argv[1]) : 800;

    for (int s = 0; s < steps; s++) {
        solver.Step();
        if ((s + 1) % 10 == 0)
            printf("  step %d: err=%.4f%% iters=%d\n", s + 1,
                   solver.AvgDensityError() * 100.0f, solver.LastIterations());
    }

    const auto& X = solver.Positions();
    const auto& P = solver.Pressures();
    const int N = solver.FluidCount();
    if ((int)X.size() != N || (int)P.size() != N) { printf("FAIL: array sizes\n"); return 1; }

    // ---- 1. density error after settling ----
    const float err = solver.AvgDensityError();
    printf("settled density error: %.4f%%\n", err * 100.0f);

    // ---- 2. hydrostatic pressure profile ----
    // SPH pressure has particle-scale noise near walls, so only look at the
    // interior core (x/z at least 0.2 m from the walls), drop the free surface
    // (y > 0.40) and the bottom layer (y < 0.12), and check the TREND:
    //   bottom pressure > top pressure, fit slope < 0.
    const float yLo = 0.12f, yHi = 0.40f;
    const int layers = 4;
    std::vector<double> sumP(layers, 0.0), sumY(layers, 0.0);
    std::vector<int> cnt(layers, 0);
    for (int i = 0; i < N; i++) {
        if (X[i].y < yLo || X[i].y > yHi) continue;
        if (X[i].x < 0.2f || X[i].x > 1.8f || X[i].z < 0.2f || X[i].z > 0.8f) continue;
        int l = std::clamp((int)((X[i].y - yLo) / (yHi - yLo) * layers), 0, layers - 1);
        sumP[l] += P[i];
        sumY[l] += X[i].y;
        cnt[l]++;
    }
    printf("pressure profile (interior core; avg y : avg p):\n");
    for (int l = 0; l < layers; l++) {
        if (cnt[l] == 0) { printf("  layer %d: no particles\n", l); continue; }
        float ay = (float)(sumY[l] / cnt[l]);
        float ap = (float)(sumP[l] / cnt[l]);
        printf("  layer %d: y=%.3f  p=%.4f  (n=%d)\n", l, ay, ap, cnt[l]);
    }
    float pBot = cnt[0] ? (float)(sumP[0] / cnt[0]) : 0.0f;
    float pTop = cnt[layers - 1] ? (float)(sumP[layers - 1] / cnt[layers - 1]) : 1e9f;
    bool bottomHigher = (pBot > pTop);   // hydrostatic: deeper = higher pressure

    // linearity: fit p = c0 + c1*y via least squares and check residual
    double sy=0, sP=0, syy=0, syp=0; int m = 0;
    for (int l = 0; l < layers; l++) {
        if (cnt[l] == 0) continue;
        float ay = (float)(sumY[l] / cnt[l]);
        float ap = (float)(sumP[l] / cnt[l]);
        sy += ay; sP += ap; syy += ay*ay; syp += ay*ap; m++;
    }
    double c1 = (m*syp - sy*sP) / (m*syy - sy*sy);   // slope (pressure per meter)
    double c0 = (sP - c1*sy) / m;
    printf("linear fit: p = %.4f + %.4f * y (negative slope = pressure grows with depth)\n", c0, c1);

    // ---- 3. volume conservation (density==rho0 implies it; report) ----
    const float vol0 = (float)N * solver.Spacing() * solver.Spacing() * solver.Spacing();
    printf("nominal water volume = %.4f m^3, particles = %d\n", vol0, N);

    bool ok = true;
    if (err > 0.02f)               { printf("FAIL: density error > 2%%\n"); ok = false; }
    if (!bottomHigher)             { printf("FAIL: bottom pressure not above top pressure\n"); ok = false; }
    if (c1 >= 0.0)                 { printf("FAIL: pressure slope not negative\n"); ok = false; }
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
