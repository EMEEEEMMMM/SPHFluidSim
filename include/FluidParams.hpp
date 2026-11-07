#pragma once

// Shared solver parameters. These used to be nested in FluidSolver; they are a
// standalone struct now so that the CPU reference solver and the GPU backend
// (IFluidSolver implementations) can share exactly one definition.
struct FluidParams {
    float spacing = 0.025f;   // particle spacing [m] (0.04 -> 8k particles for the 0.8m dam)
    float rho0 = 1000.0f;    // rest density [kg/m^3]
    float dt = 0.003f;       // time step [s]
    float omega = 0.5f;      // relaxation factor (paper: 0.5 optimal)
    float eta = 0.0001f;     // pressure-solve convergence: avg density error < 0.01%
                             // (paper Sec.4 enforces < 0.01%; SPlisHSPlasH default maxError)
    int   maxIter = 100;     // Jacobi iteration cap (SPlisHSPlasH default: 100)
    float etaV = 0.001f;     // DFSPH divergence-solver tolerance: avg |Drho/Dt| < etaV/h
                             // (paper Sec.4: density change rate error < 0.1%,
                             // applied as 0.1%/dt like SPlisHSPlasH maxErrorV)
    int   solver = 0;        // 0 = IISPH (Ihmsen et al. 2013), 1 = DFSPH (Bender & Koschier 2017)
    bool  warmStart = true;  // DFSPH: seed both solvers with 0.5x last step's stiffness
    bool  useCFL = true;     // adaptive dt = min(dt, 0.4*d/vmax), d = particle diameter (paper Sec.3.1)
    // Viscosity: XSPH variant of Schechter & Bridson 2012 (paper Sec.4),
    // implemented as in SPlisHSPlasH:
    //   dv_i = -xsph * sum_j (V_j/rhoN_j) * (v_i - v_j) * W_ij
    // (velocity-space filter; xsph = 0.1 is the common choice in the
    // SPlisHSPlasH dam-break scenes).
    float viscosity = 0.1f;
    // Surface tension (Akinci et al. 2013, used for the paper's results):
    // color-field normals + cohesion + curvature + boundary adhesion.
    // Defaults match SPlisHSPlasH (k = 0.05, kb = 0.01).
    float surfaceTension = 0.05f;
    float surfaceTensionBoundary = 0.01f;
    float gravity = -9.81f;  // [m/s^2]
};
