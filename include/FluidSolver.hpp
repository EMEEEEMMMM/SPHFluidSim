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

    FluidSolver() = default;

    // boxX/Y/Z: tank dimensions; waterSize: side of the water cube in the corner.
    void Init(float boxX, float boxY, float boxZ, float waterSize);
    // Dam break in an OPEN-TOP tank (no lid): the same water cube in the corner,
    // but splash can leave the tank. A user-triggered SpawnDrop() adds one
    // spherical blob that falls in from above the rim.
    void InitDamOpen(float boxX, float boxY, float boxZ, float waterSize);
    // Appends one spherical water blob (radius, centre) as a lattice of new
    // particles above the tank; a one-shot button calls this. No-op if already
    // spawned.
    void SpawnDrop(float radius, float cx, float cy, float cz);
    bool HasDrop() const { return m_DropSpawned; }
    // water fills the whole tank cross-section up to waterHeight (hydrostatic test).
    void InitFull(float boxX, float boxY, float boxZ, float waterHeight);
    // Open-top cubic container (side = tankSize, centred at (cx, 0, cz)) with a
    // continuous water jet injected from directly above the centre, falling a
    // distance `gap` before entering the container. A disk layer of particles
    // is emitted whenever the jet has fallen one spacing; injection STOPS once
    // the particle count reaches maxParticles (fixed water volume).
    void InitInflow(float tankSize, float gap, int inflowPerStep, float inflowRadius,
                    float cx, float cz, int maxParticles);
    // Open-top cubic container with a PRE-FILLED pool plus a separate SPHERICAL
    // drop hovering above the centre; the drop falls naturally into the pool.
    // poolHeight is the resting water level inside the container; dropRadius
    // is the sphere radius, whose bottom starts dropGap above the rim.
    // Pool and drop volumes are fixed -> the pool:drop volume ratio is fixed
    // for any particle count (spacing is derived externally).
    void InitDrop(float tankSize, float poolHeight, float dropRadius, float dropGap,
                  float cx, float cz, int maxParticles);
    void Reset();
    void Step();

    const std::vector<Math::Vector3>& Positions() const { return m_X; }
    const std::vector<Math::Vector3>& Velocities() const { return m_V; }
    const std::vector<float>&         Densities() const { return m_Rho; }   // rhoN: density incl. boundary-particle contribution
    const std::vector<float>&         Colors() const { return m_Colors; }   // 3 floats per particle
    float SimTime() const { return m_Time; }
    float AvgDensityError() const { return m_AvgErr; }
    int   LastIterations() const { return m_LastIter; }
    int   DivergenceIterations() const { return m_LastIterV; }   // DFSPH divergence solver
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
    std::vector<Math::Vector3> m_Normals;          // Akinci-2013 color-field normals (scaled by h)
    std::vector<float> m_Alpha;                    // DFSPH factor alpha_i (paper Eq. 8 denominator inverse)
    std::vector<float> m_KappaV;                   // DFSPH divergence-solver stiffness (p/rho^2 form)
    std::vector<float> m_KappaVPrev;               // DFSPH divergence-solver kappa_v*h (warm start seed)
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
    int m_LastIter = 0;
};
