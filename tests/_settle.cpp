#include "FluidSolver.hpp"
#include <cstdio>
#include <cmath>
int main(int argc, char** argv) {
    FluidSolver s;
    s.GetParams().spacing = 0.025f;
    const int settle = argc > 1 ? atoi(argv[1]) : 400;
    // temporarily raise the settle inside InitDrop? No - InitDrop has its own.
    // Instead: InitFull closed box, settle, measure drift distribution.
    s.InitFull(2.0f, 1.2f, 1.0f, 1.0f);
    for (int k = 0; k < settle; k++) s.Step();
    const auto& V = s.Velocities();
    const auto& X = s.Positions();
    int n = s.FluidCount(), n1 = 0; float vSum = 0, vMax = 0;
    for (int i = 0; i < n; i++) {
        const float v = V[i].length();
        vSum += v; vMax = std::max(vMax, v);
        if (v > 1.0f) n1++;
    }
    printf("闭合箱 settle %d 步: 平均v=%.4f vmax=%.2f v>1占%.1f%%\n",
           settle, vSum / n, vMax, 100.0 * n1 / n);
    return 0;
}
