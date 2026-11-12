#pragma once
#include "IFluidSolver.hpp"
#include "Math/Vector3.hpp"
#include <vector>

// IISPH solver (Ihmsen et al., TVCG 2013).
// Implements Algorithm 1 of the paper:
//   Predict advection -> pressure solve (relaxed Jacobi) -> integrate.
// Boundary handling follows paper Sec.4 (Akinci et al. 2012 rigid-fluid coupling):
// boundary particles contribute to density/forces but carry no pressure.
//
// This is the CPU reference implementation of IFluidSolver. It is
// deterministic: repeated runs of the same scene are bit-identical (the
// pressure solve is strict relaxed Jacobi, neighbor order is stable).
class FluidSolver : public IFluidSolver {
public:
    using Params = FluidParams;

    FluidSolver() = default;

    // boxX/Y/Z: tank dimensions; waterSize: side of the water cube in the corner.
    void Init(float boxX, float boxY, float boxZ, float waterSize) override;
    // Dam break in an OPEN-TOP tank (no lid): the same water cube in the corner,
    // but splash can leave the tank. A user-triggered SpawnDrop() adds one
    // spherical blob that falls in from above the rim.
    void InitDamOpen(float boxX, float boxY, float boxZ, float waterSize) override;
    // Appends one spherical water blob (radius, centre) as a lattice of new
    // particles above the tank; a one-shot button calls this. No-op if already
    // spawned.
    void SpawnDrop(float radius, float cx, float cy, float cz) override;
    bool HasDrop() const override { return m_DropSpawned; }
    // water fills the whole tank cross-section up to waterHeight (hydrostatic test).
    void InitFull(float boxX, float boxY, float boxZ, float waterHeight) override;
    // Open-top cubic container (side = tankSize, centred at (cx, 0, cz)) with a
    // continuous water jet injected from directly above the centre, falling a
    // distance `gap` before entering the container. A disk layer of particles
    // is emitted whenever the jet has fallen one spacing; injection STOPS once
    // the particle count reaches maxParticles (fixed water volume).
    void InitInflow(float tankSize, float gap, int inflowPerStep, float inflowRadius,
                    float cx, float cz, int maxParticles) override;
    // Open-top cubic container with a PRE-FILLED pool plus a separate SPHERICAL
    // drop hovering above the centre; the drop falls naturally into the pool.
    // poolHeight is the resting water level inside the container; dropRadius
    // is the sphere radius, whose bottom starts dropGap above the rim.
    // Pool and drop volumes are fixed -> the pool:drop volume ratio is fixed
    // for any particle count (spacing is derived externally).
    void InitDrop(float tankSize, float poolHeight, float dropRadius, float dropGap,
                  float cx, float cz, int maxParticles) override;
    // Empty scene (v2): clear all particles/boundaries, enter a no-fluid state.
    void InitEmpty() override;
    void Reset() override;
    void Step() override;

    const std::vector<Math::Vector3>& Positions() const override { return m_X; }
    const std::vector<Math::Vector3>& Velocities() const override { return m_V; }
    const std::vector<float>&         Densities() const override { return m_Rho; }   // rhoN: density incl. boundary-particle contribution
    const std::vector<float>&         Colors() const override { return m_Colors; }   // 3 floats per particle
    float SimTime() const override { return m_Time; }
    float AvgDensityError() const override { return m_AvgErr; }
    float MeanDensity() const override;   // arithmetic mean density [kg/m^3] from the current rho field
    int   LastIterations() const override { return m_LastIter; }
    float LastPressureTimeMs() const override { return m_LastPressureMs; }   // wall time of the last pressure solve
    int   DivergenceIterations() const override { return m_LastIterV; }   // DFSPH divergence solver
    int   FluidCount() const override { return m_N; }
    int   BoundaryCount() const override { return static_cast<int>(m_BoundX.size()); }
    // wall-particle positions (for boundary invariants in tests)
    const std::vector<Math::Vector3>& BoundaryPositions() const override { return m_BoundX; }
    Params& GetParams() override { return params; }
    const Params& GetParams() const override { return params; }
    float  KernelRadius() const override { return m_h; }
    float  Spacing() const override { return params.spacing; }
    const std::vector<float>& Pressures() const override { return m_P; }   // normalized pressure
    // Density-error diagnostics. AvgDensityError() (above) is the SIGNED
    // average the solvers use internally (IISPH: algebraic residual, DFSPH:
    // mean rho*-1); a signed mean can cancel a compressed and a dilated region,
    // so these two report the local picture the mean hides. Normalized units
    // (1.0 = rest density).
    float MeanAbsDensityError() const override { return m_AvgAbsErr; }
    float MaxDensityError() const override { return m_MaxErr; }
    // fluid-neighbor count of particle i (free-surface detection)
    int    NeighborCount(int i) const override { return (int)m_FluidNb[i].size(); }
    int    MaxNeighbors() const override { int m = 0; for (const auto& n : m_FluidNb) m = std::max(m, (int)n.size()); return m; }

    // --- paper Sec.5 reproduction helpers ---
    // Record the density-error trace of the NEXT pressure solve (one entry per
    // Jacobi iteration), then read it with ConvergenceTrace(). One-shot.
    void EnableConvergenceTrace() override { m_traceNext = true; }
    const std::vector<float>& ConvergenceTrace() const override { return m_convTrace; }

    const char* BackendName() const override { return "CPU (OpenMP)"; }
    GpuMemoryInfo GpuMemory() const override { return {}; }
    void CheckParticleBudget(int particleCount) const override { (void)particleCount; }

private:
    void PushFluid(float x, float y, float z);
    void BuildBoundary(bool openTop = false);   // true = open-top cubic container (inflow scene)
    void InjectFluid();   // spawn inflow particles for the open-tank scene
    void BuildNeighborhood();
    void PredictAdvection();
    void PressureSolve();
    void Integrate();
    void UpdateColors();
    // --- DFSPH (Bender & Koschier 2017, paper Alg. 1-3) ---
    void ComputeDensity();          // rhoN = sum V_j W_ij (+ boundary)
    void ComputeDFSPHFactor();      // alpha_i = 1 / (sum |V gradW|^2 + |sum V gradW|^2)
    void CorrectDensityError();     // Alg. 3: fix rho* - rho0 on position level
    void CorrectDivergenceError();  // Alg. 2: fix Drho/Dt = 0 on velocity level
    void ApplyWallClamp();          // hard-wall safety net (position + velocity)
    float RhoAdvOf(int i) const;    // rhoN + dt * Drho/Dt at current velocity
    float RhoDotOf(int i) const;    // Drho/Dt at current velocity (SPH divergence)
    float W(float r) const;                       // cubic spline kernel value
    Math::Vector3 gradW(const Math::Vector3& r) const;  // kernel gradient
    float WCohesion(float r) const;               // Akinci 2013 cohesion kernel (support h)
    float WAdhesion(float r) const;               // Akinci 2013 adhesion kernel (support h)

    Params params;
    bool m_UseFullInit = false;   // Reset() needs to know which Init variant to rerun
    bool m_UseDamOpen = false;    // dam-break with open top (InitDamOpen)
    bool m_DropSpawned = false;   // SpawnDrop() already fired (one-shot)
    bool m_Empty = false;         // v2 empty scene: no particles, no boundary
    float m_h = 0.1f;      // kernel support radius = 2 * spacing
    float m_mass = 1.0f;   // particle mass = rho0 * spacing^3

    // fluid particle state (SoA)
    std::vector<Math::Vector3> m_X, m_V, m_Vadv;   // position, velocity, advected velocity
    std::vector<Math::Vector3> m_dii;              // Eqn.(9) coefficient, NO dt^2 factor (vector, incl. boundary)
    std::vector<Math::Vector3> m_S;                // Pass-1 result: sum_j d_ij * p_j (vector, NO dt^2)
    std::vector<float> m_Rho, m_RhoAdv;            // NORMALIZED density rho/rho0, predicted density
    std::vector<float> m_P, m_Pprev;               // pressure (normalized), previous pressure (warm start)
    std::vector<float> m_Pold;                     // IISPH Jacobi: previous-iteration pressure (read-only)
    std::vector<float> m_aii;                      // Eqn.(12) diagonal (scalar, NO dt^2, incl. boundary)
    std::vector<float> m_deltaB;                   // boundary density contribution: sum_b V_b W(x_i-x_b)
    std::vector<Math::Vector3> m_gradB;            // sum_b V_b gradW(x_i-x_b)
    std::vector<float> m_gradB2;                   // sum_b V_b |gradW|^2 (for the a_ii boundary term)
    std::vector<std::vector<int>> m_FluidNb;       // fluid neighbors of each particle
    std::vector<std::vector<int>> m_BoundNb;       // boundary neighbors of each particle (for surface tension)
    std::vector<Math::Vector3> m_Normals;          // Akinci-2013 color-field normals (scaled by h)
    std::vector<float> m_Alpha;                    // DFSPH factor alpha_i (paper Eq. 8 denominator inverse)
    std::vector<float> m_KappaV;                   // DFSPH divergence-solver stiffness (p/rho^2 form)
    std::vector<float> m_KappaVPrev;               // DFSPH divergence-solver kappa_v*h (warm start seed)
    std::vector<float> m_KappaSeed;                // divergence warm-start seed, in kappa_v units
    int m_LastIterV = 0;                           // DFSPH divergence-solver iterations
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

    // wall planes for the hard-wall safety net (Integrate). The closed box
    // spans [0, BoxX]x[0, BoxY]x[0, BoxZ]; the open-top inflow container is
    // [x0,x1]x[0,L]x[z0,z1] with NO top wall.
    float m_WallX0 = 0.0f, m_WallX1 = 2.0f, m_WallY1 = 1.2f, m_WallZ0 = 0.0f, m_WallZ1 = 1.0f;
    bool  m_WallTop = true;

    // open-tank scenes: inflow (jet) and drop (pool + falling blob)
    bool  m_Inflow = false;
    bool  m_Drop = false;
    float m_DropPool = 0.4f, m_DropSize = 0.585f, m_DropGap = 0.15f;
    float m_InflowGap = 0.3f;         // fall distance from the emitter to the container rim [m]
    float m_InflowRadius = 0.08f;     // emitter disk radius [m]
    int   m_InflowPerStep = 100;      // max particles per emitted layer (radius decides the actual count)
    float m_InflowCounter = 0.0f;     // distance travelled since the last layer [m]
    int   m_MaxParticles = 8000;      // injection stops at this many particles (fixed volume)
    float m_InflowX = 1.0f, m_InflowZ = 0.5f;   // emitter centre (x, z)

    // neighborhood grid (uniform, cell = h). Origin follows the particle
    // bounding box (fluid + boundary) each rebuild, so translated containers
    // (inflow/drop scenes, box centred at x=1) hash correctly - a fixed
    // (0,0,0) origin clamped everything right of x=1.0 into the last cell
    // column, corrupting neighbour queries along that wall (visible pile-up).
    Math::Vector3 m_GridOrigin;   // grid corner = bbox min - margin
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
    float m_AvgAbsErr = 0.0f;   // mean |density error| of the last solve
    float m_MaxErr = 0.0f;      // max  |density error| of the last solve
    int m_LastIter = 0;
    float m_LastPressureMs = 0.0f;
};
