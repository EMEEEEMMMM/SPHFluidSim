// Boundary invariant test: every wall particle must lie exactly on the tank
// planes the solver also uses for its hard-wall clamp. Regression for the old
// `x0 + i * spacing` sampling, which overshot the wall whenever the tank size
// was not an integer multiple of the spacing (measured at spacing 0.04: wall
// particles at y = 1.24 for a 1.20 m tank).
#include "FluidSolver.hpp"
#include <cstdio>
#include <cmath>

namespace {
int CheckBox(const char* name, FluidSolver& s,
             float x0, float x1, float y0, float y1, float z0, float z1) {
    const float eps = 1e-4f;
    int outside = 0;
    float mx = -1e30f, my = -1e30f, mz = -1e30f;
    for (const auto& b : s.BoundaryPositions()) {
        mx = std::max(mx, b.x); my = std::max(my, b.y); mz = std::max(mz, b.z);
        if (b.x < x0 - eps || b.x > x1 + eps ||
            b.y < y0 - eps || b.y > y1 + eps ||
            b.z < z0 - eps || b.z > z1 + eps) {
            if (outside == 0)
                printf("  first outside: (%.4f, %.4f, %.4f)\n", b.x, b.y, b.z);
            outside++;
        }
    }
    printf("%s: n=%d  max=(%.4f, %.4f, %.4f)  outside=%d\n",
           name, s.BoundaryCount(), mx, my, mz, outside);
    return outside;
}
}   // namespace

int main() {
    int bad = 0;

    // open dam tank, 2.0 x 1.2 x 1.0 — several spacings that are NOT integer
    // multiples of the tank size (the case that used to overshoot).
    for (float spacing : { 0.04f, 0.03f, 0.025f, 0.0172f }) {
        FluidSolver s;
        s.GetParams().spacing = spacing;
        s.InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);
        printf("spacing=%.4f\n", spacing);
        bad += CheckBox("  dam-open", s, 0.0f, 2.0f, 0.0f, 1.2f, 0.0f, 1.0f);
    }

    // closed box: same planes plus a lid at y = 1.2
    {
        FluidSolver s;
        s.GetParams().spacing = 0.03f;
        s.Init(2.0f, 1.2f, 1.0f, 0.8f);
        printf("spacing=0.0300\n");
        bad += CheckBox("  closed", s, 0.0f, 2.0f, 0.0f, 1.2f, 0.0f, 1.0f);
    }

    printf(bad == 0 ? "PASS\n" : "FAIL\n");
    return bad == 0 ? 0 : 1;
}
