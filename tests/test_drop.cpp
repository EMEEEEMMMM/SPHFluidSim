#include "FluidSolver.hpp"
#include <cstdio>
#include <cmath>
int main(int argc, char** argv) {
    const float ph = argc > 1 ? (float)atof(argv[1]) : 0.6f;
    const float vis = argc > 2 ? (float)atof(argv[2]) : 0.3f;
    FluidSolver s;
    s.GetParams().spacing = 0.04f;   // lighter: 20k particles
    s.GetParams().viscosity = vis;
    s.InitDrop(1.0f, ph, 0.27f, 0.1f, 1.0f, 0.5f, 60000);
    float peakV = 0; int nEsc = 0;
    for (int k = 0; k < 80; k++) {
        s.Step();
        const auto& V = s.Velocities();
        const auto& X = s.Positions();
        for (int i = 0; i < s.FluidCount(); i++) {
            peakV = std::max(peakV, V[i].length());
            if (X[i].x < -0.1 || X[i].x > 2.1 || X[i].y < -0.05 || X[i].y > 2.5 || X[i].z < -0.6 || X[i].z > 1.6) nEsc++;
        }
    }
    printf("池=%.1fm 粘=%.1f: peak v=%.1f escaped=%d\n", ph, vis, peakV, nEsc);
    // Regression assertions: this test used to only print numbers and always
    // exit 0, so it could never fail. peakV is capped by the 4 m/s splash
    // clamp; nothing may leave the tank.
    const bool pass = (nEsc == 0) && (peakV < 4.5f) && (s.FluidCount() > 0);
    printf(pass ? "PASS\n" : "FAIL\n");
    return pass ? 0 : 1;
}
