#pragma once
#include "Math/Vector3.hpp"
#include <vector>

// IISPH solver (Ihmsen et al., TVCG 2013).
// Implements Algorithm 1 of the paper:
//   Predict advection -> pressure solve (relaxed Jacobi) -> integrate.
// Boundary handling follows paper Sec.4 (Akinci et al. 2012 rigid-fluid coupling):
// boundary particles contribute to density/forces but carry no pressure.
class FluidSolver {
public:
    struct Params {
        float spacing = 0.025f;   // particle spacing [m] (0.04 -> 8k particles for the 0.8m dam)
        float rho0 = 1000.0f;    // rest density [kg/m^3]
        float dt = 0.003f;       // time step [s]
        float omega = 0.5f;      // relaxation factor (paper: 0.5 optimal)
        float eta = 0.001f;      // pressure-solve convergence (paper Sec.5.1: avg density error < 0.1%)
        int   maxIter = 100;     // Jacobi iteration cap (SPlisHSPlasH default: 100)
        bool  useCFL = true;     // adaptive dt = min(dt, 0.4h/vmax); paper Table 1 uses fixed dt
        // Artificial viscosity (Monaghan 1992/2005, cited as [23] in the paper).
        // alpha: dimensionless viscosity strength; soundSpeed: artificial speed of sound.
        float viscosity = 0.1f;
        float soundSpeed = 25.0f;
        // Surface tension (Becker & Teschner 2007, cited as [5] in the paper):
        // cohesion forces between particles. surfaceTensionBoundary is the
        // fluid-wall cohesion (wetting).
        float surfaceTension = 0.05f;
        float surfaceTensionBoundary = 0.01f;
        float gravity = -9.81f;  // [m/s^2]
    };

    FluidSolver() = default;

    // boxX/Y/Z: tank dimensions; waterSize: side of the water cube in the corner.
    void Init(float boxX, float boxY, float boxZ, float waterSize);
    // water fills the whole tank cross-section up to waterHeight (hydrostatic test).
    void InitFull(float boxX, float boxY, float boxZ, float waterHeight);
    void Reset();
    void Step();

    const std::vector<Math::Vector3>& Positions() const { return m_X; }
    const std::vector<Math::Vector3>& Velocities() const { return m_V; }
    const std::vector<float>&         Colors() const { return m_Colors; }   // 3 floats per particle
    float SimTime() const { return m_Time; }
    float AvgDensityError() const { return m_AvgErr; }
    int   LastIterations() const { return m_LastIter; }
    int   FluidCount() const { return m_N; }
    int   BoundaryCount() const { return static_cast<int>(m_BoundX.size()); }
    Params& GetParams() { return params; }
    float  KernelRadius() const { return m_h; }
    float  Spacing() const { return params.spacing; }
    const std::vector<float>& Pressures() const { return m_P; }   // normalized pressure
    // fluid-neighbor count of particle i (free-surface detection)
    int    NeighborCount(int i) const { return (int)m_FluidNb[i].size(); }
    int    MaxNeighbors() const { int m = 0; for (const auto& n : m_FluidNb) m = std::max(m, (int)n.size()); return m; }

    // --- paper Sec.5 reproduction helpers ---
    // Record the density-error trace of the NEXT pressure solve (one entry per
    // Jacobi iteration), then read it with ConvergenceTrace(). One-shot.
    void EnableConvergenceTrace() { m_traceNext = true; }
    const std::vector<float>& ConvergenceTrace() const { return m_convTrace; }

private:
    void PushFluid(float x, float y, float z);
    void BuildBoundary();
    void BuildNeighborhood();
    void PredictAdvection();
    void PressureSolve();
    void Integrate();
    void UpdateColors();
    float W(float r) const;                       // cubic spline kernel value
    Math::Vector3 gradW(const Math::Vector3& r) const;  // kernel gradient

    Params params;
    bool m_UseFullInit = false;   // Reset() needs to know which Init variant to rerun
    float m_h = 0.1f;      // kernel support radius = 2 * spacing
    float m_mass = 1.0f;   // particle mass = rho0 * spacing^3

    // fluid particle state (SoA)
    std::vector<Math::Vector3> m_X, m_V, m_Vadv;   // position, velocity, advected velocity
    std::vector<Math::Vector3> m_dii;              // Eqn.(9) coefficient, NO dt^2 factor (vector, incl. boundary)
    std::vector<Math::Vector3> m_S;                // Pass-1 result: sum_j d_ij * p_j (vector, NO dt^2)
    std::vector<float> m_Rho, m_RhoAdv;            // NORMALIZED density rho/rho0, predicted density
    std::vector<float> m_P, m_Pprev;               // pressure (normalized), previous pressure (warm start)
    std::vector<float> m_aii;                      // Eqn.(12) diagonal (scalar, NO dt^2, incl. boundary)
    std::vector<float> m_deltaB;                   // boundary density contribution: sum_b V_b W(x_i-x_b)
    std::vector<Math::Vector3> m_gradB;            // sum_b V_b gradW(x_i-x_b)
    std::vector<float> m_gradB2;                   // sum_b V_b |gradW|^2 (for the a_ii boundary term)
    std::vector<std::vector<int>> m_FluidNb;       // fluid neighbors of each particle
    std::vector<std::vector<int>> m_BoundNb;       // boundary neighbors of each particle (for surface tension)
    std::vector<float> m_Colors;                   // per-particle RGB (velocity colored)
    bool m_traceNext = false;                      // record the next pressure solve's error trace
    std::vector<float> m_convTrace;                // |avg density error| per Jacobi iteration

    // boundary particles (static walls); each has its own normalized volume V_b = 1/delta_b
    // (Akinci et al. 2012), so the wall particles do NOT over-contribute density.
    std::vector<Math::Vector3> m_BoundX;
    std::vector<float> m_BoundV;

    float m_Vol = 1.0f;    // particle volume V = spacing^3 = m/rho0 (fluid and boundary)

    // tank
    float m_BoxX = 2.0f, m_BoxY = 1.2f, m_BoxZ = 1.0f;
    float m_WaterSize = 0.8f;

    // neighborhood grid (uniform, cell = h)
    int m_GW = 0, m_GH = 0, m_GD = 0;
    // Spatial hash cells as SoA (paper [30] "Parallel data structures for SPH"):
    // particles are grouped per cell in m_CellParticles, with m_CellStart giving
    // each cell's [start, end) slice; m_CellCount is atomic-build scratch.
    std::vector<int> m_CellStart;
    std::vector<int> m_CellParticles;
    std::vector<int> m_CellCount;

    int m_N = 0;
    float m_Time = 0.0f;
    float m_AvgErr = 0.0f;
    int m_LastIter = 0;
};
