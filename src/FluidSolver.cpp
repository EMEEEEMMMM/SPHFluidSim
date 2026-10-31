#include "FluidSolver.hpp"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <omp.h>

#ifdef IISPH_DEBUG
#include <cstdio>
#define DBG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#else
#define DBG(...) ((void)0)
#endif

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

// ---------------------------------------------------------------------------
// Cubic spline kernel
// ---------------------------------------------------------------------------
float FluidSolver::W(float r) const {
    float q = r / m_h;
    if (q > 1.0f) return 0.0f;
    const float c = 8.0f / (kPi * m_h * m_h * m_h);
    if (q <= 0.5f)
        return c * (1.0f - 6.0f * q * q + 6.0f * q * q * q);
    const float t = 1.0f - q;
    return c * 2.0f * t * t * t;
}

Math::Vector3 FluidSolver::gradW(const Math::Vector3& r) const {
    float rr = r.length();
    if (rr < 1e-9f || rr >= m_h) return Math::Vector3();
    float q = rr / m_h;
    float dWdq;
    if (q <= 0.5f) dWdq = -12.0f * q + 18.0f * q * q;
    else           dWdq = -6.0f * (1.0f - q) * (1.0f - q);
    float scale = (8.0f / (kPi * m_h * m_h * m_h)) * (dWdq / m_h) / rr;
    return r * scale;
}

// ---------------------------------------------------------------------------
// Akinci et al. 2013 surface-tension kernels (SPlisHSPlasH SPHKernels.h):
//   cohesion: W_c(r) = 32/(pi h^9) (h-r)^3 r^3                for h/2 < r <= h
//             32/(pi h^9) (2 (h-r)^3 r^3 - h^6/64)            for 0 < r <= h/2
//              (the h^6/64 shift keeps W_c continuous at r = h/2)
//   adhesion: W_a(r) = 0.007/h^3.25 (-4 r^2/h + 6 r - 2 h)^0.25   for h/2 < r <= h
float FluidSolver::WCohesion(float r) const {
    if (r <= 0.0f || r >= m_h) return 0.0f;
    const float k = 32.0f / (kPi * powf(m_h, 9.0f));
    const float r3 = r * r * r;
    const float t = m_h - r;
    const float hr3 = t * t * t;
    if (r > 0.5f * m_h) return k * hr3 * r3;
    return k * (2.0f * hr3 * r3 - powf(m_h, 6.0f) / 64.0f);
}

float FluidSolver::WAdhesion(float r) const {
    if (r <= 0.5f * m_h || r >= m_h) return 0.0f;
    const float k = 0.007f / powf(m_h, 3.25f);
    // base = -4r^2/h + 6r - 2h is >= 0 for r in (h/2, h), but for r just
    // above h/2 the three terms cancel to ~1e-9 and float rounding can make
    // base slightly negative -> powf(neg, 0.25) = NaN (seen in Debug builds).
    // Clamp to 0 so the kernel is well-defined at the lower support edge.
    float base = -4.0f * r * r / m_h + 6.0f * r - 2.0f * m_h;
    if (base < 0.0f) base = 0.0f;
    return k * powf(base, 0.25f);
}

// ---------------------------------------------------------------------------
void FluidSolver::Init(float boxX, float boxY, float boxZ, float waterSize) {
    m_BoxX = boxX; m_BoxY = boxY; m_BoxZ = boxZ; m_WaterSize = waterSize;
    m_UseFullInit = false;
    m_Inflow = false;   // CRITICAL: Reset() branches on this
    m_Drop = false;
    m_UseDamOpen = false;   // Reset() branches on this first - must be cleared
    m_DropSpawned = false;
    m_WallX0 = 0.0f; m_WallX1 = boxX; m_WallY1 = boxY; m_WallZ0 = 0.0f; m_WallZ1 = boxZ;
    m_WallTop = true;
    m_h = 2.0f * params.spacing;
    m_mass = params.rho0 * params.spacing * params.spacing * params.spacing;
    m_Vol = params.spacing * params.spacing * params.spacing;

    m_X.clear(); m_V.clear(); m_Vadv.clear();
    m_dii.clear(); m_S.clear();
    m_Rho.clear(); m_RhoAdv.clear(); m_P.clear(); m_Pprev.clear(); m_Pold.clear();
    m_aii.clear(); m_deltaB.clear(); m_gradB.clear(); m_gradB2.clear();
    m_FluidNb.clear(); m_BoundNb.clear(); m_Normals.clear(); m_Colors.clear();
    m_Alpha.clear(); m_KappaV.clear(); m_KappaVPrev.clear(); m_KappaSeed.clear();
    m_N = 0;

    // water cube in the corner (classic dam break start)
    int n = (int)std::ceil(waterSize / params.spacing);
    for (int k = 0; k < n; k++)
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++)
                PushFluid((i + 0.5f) * params.spacing, (j + 0.5f) * params.spacing, (k + 0.5f) * params.spacing);

    BuildBoundary();
    m_Time = 0.0f;  m_AvgErr = 0.0f;  m_LastIter = 0;
    UpdateColors();
}

// ---------------------------------------------------------------------------
void FluidSolver::InitDamOpen(float boxX, float boxY, float boxZ, float waterSize) {
    m_BoxX = boxX; m_BoxY = boxY; m_BoxZ = boxZ; m_WaterSize = waterSize;
    m_UseFullInit = false;
    m_Inflow = false;
    m_Drop = false;
    m_UseDamOpen = true;
    m_DropSpawned = false;
    m_WallX0 = 0.0f; m_WallX1 = boxX; m_WallY1 = boxY; m_WallZ0 = 0.0f; m_WallZ1 = boxZ;
    m_WallTop = false;              // open top: splash may leave the tank
    m_h = 2.0f * params.spacing;
    m_mass = params.rho0 * params.spacing * params.spacing * params.spacing;
    m_Vol = params.spacing * params.spacing * params.spacing;

    m_X.clear(); m_V.clear(); m_Vadv.clear();
    m_dii.clear(); m_S.clear();
    m_Rho.clear(); m_RhoAdv.clear(); m_P.clear(); m_Pprev.clear(); m_Pold.clear();
    m_aii.clear(); m_deltaB.clear(); m_gradB.clear(); m_gradB2.clear();
    m_FluidNb.clear(); m_BoundNb.clear(); m_Normals.clear(); m_Colors.clear();
    m_Alpha.clear(); m_KappaV.clear(); m_KappaVPrev.clear(); m_KappaSeed.clear();
    m_N = 0;

    // water cube in the corner (classic dam break start)
    int n = (int)std::ceil(waterSize / params.spacing);
    for (int k = 0; k < n; k++)
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++)
                PushFluid((i + 0.5f) * params.spacing, (j + 0.5f) * params.spacing, (k + 0.5f) * params.spacing);

    BuildBoundary(true);
    m_Time = 0.0f;  m_AvgErr = 0.0f;  m_LastIter = 0;
    UpdateColors();
}

// ---------------------------------------------------------------------------
void FluidSolver::SpawnDrop(float radius, float cx, float cy, float cz) {
    if (m_DropSpawned) return;   // one-shot
    m_DropSpawned = true;
    const float sp = params.spacing;
    const int n = (int)std::ceil(radius / sp);
    // shrink by a quarter spacing so the lattice stays one spacing apart
    const float r = radius - 0.25f * sp;
    for (int ix = -n; ix <= n; ix++)
        for (int iy = -n; iy <= n; iy++)
            for (int iz = -n; iz <= n; iz++) {
                const float dx = (float)ix * sp, dy = (float)iy * sp, dz = (float)iz * sp;
                if (dx * dx + dy * dy + dz * dz <= r * r)
                    PushFluid(cx + dx, cy + dy, cz + dz);
            }
    // new particles have zero velocity (free fall); neighbourhoods are rebuilt
    // on the next Step() because m_N changed.
    UpdateColors();
}

// ---------------------------------------------------------------------------
void FluidSolver::InitFull(float boxX, float boxY, float boxZ, float waterHeight) {
    m_BoxX = boxX; m_BoxY = boxY; m_BoxZ = boxZ; m_WaterSize = waterHeight;
    m_UseFullInit = true;
    m_Inflow = false;   // CRITICAL: Reset() branches on this
    m_Drop = false;
    m_UseDamOpen = false;
    m_DropSpawned = false;
    m_WallX0 = 0.0f; m_WallX1 = boxX; m_WallY1 = boxY; m_WallZ0 = 0.0f; m_WallZ1 = boxZ;
    m_WallTop = true;
    m_h = 2.0f * params.spacing;
    m_mass = params.rho0 * params.spacing * params.spacing * params.spacing;
    m_Vol = params.spacing * params.spacing * params.spacing;

    m_X.clear(); m_V.clear(); m_Vadv.clear();
    m_dii.clear(); m_S.clear();
    m_Rho.clear(); m_RhoAdv.clear(); m_P.clear(); m_Pprev.clear(); m_Pold.clear();
    m_aii.clear(); m_deltaB.clear(); m_gradB.clear(); m_gradB2.clear();
    m_FluidNb.clear(); m_BoundNb.clear(); m_Normals.clear(); m_Colors.clear();
    m_Alpha.clear(); m_KappaV.clear(); m_KappaVPrev.clear(); m_KappaSeed.clear();
    m_N = 0;

    // water fills the whole tank cross-section (only the top surface is free) —
    // this is the proper hydrostatic test: no lateral free surface to collapse.
    int nx = (int)std::ceil(boxX / params.spacing);
    int ny = (int)std::ceil(waterHeight / params.spacing);
    int nz = (int)std::ceil(boxZ / params.spacing);
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++)
                PushFluid((i + 0.5f) * params.spacing, (j + 0.5f) * params.spacing, (k + 0.5f) * params.spacing);

    BuildBoundary();
    m_Time = 0.0f;  m_AvgErr = 0.0f;  m_LastIter = 0;
    UpdateColors();
}

// ---------------------------------------------------------------------------
void FluidSolver::PushFluid(float x, float y, float z) {
    m_X.push_back(Math::Vector3(x, y, z));
    m_V.push_back(Math::Vector3());
    m_Vadv.push_back(Math::Vector3());
    m_dii.push_back(Math::Vector3());
    m_S.push_back(Math::Vector3());
    m_Rho.push_back(0.0f);
    m_RhoAdv.push_back(0.0f);
    m_P.push_back(0.0f);
    m_Pprev.push_back(0.0f);
    m_aii.push_back(0.0f);
    m_deltaB.push_back(0.0f);
    m_gradB.push_back(Math::Vector3());
    m_gradB2.push_back(0.0f);
    m_FluidNb.emplace_back();
    m_BoundNb.emplace_back();
    m_Normals.push_back(Math::Vector3());
    m_Alpha.push_back(0.0f);
    m_KappaV.push_back(0.0f);
    m_KappaVPrev.push_back(0.0f);
    m_Colors.push_back(0.0f); m_Colors.push_back(0.0f); m_Colors.push_back(0.0f);
    m_N++;
}

// ---------------------------------------------------------------------------
// Open-top cubic container + continuous central jet (inflow scene).
void FluidSolver::InitInflow(float tankSize, float gap, int perStep, float radius,
                             float cx, float cz, int maxParticles) {
    m_BoxX = tankSize; m_BoxY = tankSize; m_BoxZ = tankSize; m_WaterSize = 0.0f;
    m_Inflow = true; m_InflowGap = gap; m_InflowPerStep = perStep;
    m_InflowRadius = radius; m_InflowX = cx; m_InflowZ = cz;
    m_MaxParticles = maxParticles > 0 ? maxParticles : 8000;
    m_Drop = false;
    m_UseFullInit = false;
    m_UseDamOpen = false;
    m_DropSpawned = false;
    m_WallX0 = cx - 0.5f * tankSize; m_WallX1 = cx + 0.5f * tankSize;
    m_WallY1 = tankSize; m_WallZ0 = cz - 0.5f * tankSize; m_WallZ1 = cz + 0.5f * tankSize;
    m_WallTop = false;   // open top: the jet falls in from above
    m_h = 2.0f * params.spacing;
    m_mass = params.rho0 * params.spacing * params.spacing * params.spacing;
    m_Vol = params.spacing * params.spacing * params.spacing;

    m_X.clear(); m_V.clear(); m_Vadv.clear();
    m_dii.clear(); m_S.clear();
    m_Rho.clear(); m_RhoAdv.clear(); m_P.clear(); m_Pprev.clear(); m_Pold.clear();
    m_aii.clear(); m_deltaB.clear(); m_gradB.clear(); m_gradB2.clear();
    m_FluidNb.clear(); m_BoundNb.clear(); m_Normals.clear(); m_Colors.clear();
    m_Alpha.clear(); m_KappaV.clear(); m_KappaVPrev.clear(); m_KappaSeed.clear();
    m_N = 0;

    // small puddle DIRECTLY under the jet, at full spacing density (cushions the
    // first impact; the old 2*spacing puddle was 1/8 density and off-centre, so
    // it got blown away instead of absorbing the jet)
    const int n0 = 6;
    for (int k = 0; k < n0; k++)
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < n0; i++)
                PushFluid(cx - 0.15f + (i + 0.5f) * params.spacing,
                          (j + 0.5f) * params.spacing,
                          cz - 0.15f + (k + 0.5f) * params.spacing);

    BuildBoundary(true);   // open-top container
    m_InflowCounter = 0;   // start the jet phase cleanly
    m_Time = 0.0f;  m_AvgErr = 0.0f;  m_LastIter = 0;
    UpdateColors();
}

// Spawn jet particles at the emitter. A full disk layer (regular grid, spacing
// apart, inside the emitter radius) is added every kInflowPeriod steps, so the
// vertical layer spacing along the falling jet stays close to the particle
// spacing. Spawning vFall*dt per step would pack the layers ~3.4x denser than
// spacing and blow the pressure solve (measured: single-particle v ~1700 m/s).
// Pre-filled pool + hovering drop falling into it (fixed pool:drop volume).
void FluidSolver::InitDrop(float tankSize, float poolHeight, float dropRadius, float dropGap,
                           float cx, float cz, int maxParticles) {
    m_BoxX = tankSize; m_BoxY = tankSize; m_BoxZ = tankSize; m_WaterSize = 0.0f;
    m_Inflow = false;                 // no jet; this scene is a one-shot drop
    m_Drop = true;
    m_DropPool = poolHeight; m_DropSize = dropRadius; m_DropGap = dropGap;   // m_DropSize = sphere radius now
    m_UseFullInit = false;
    m_UseDamOpen = false;
    m_DropSpawned = false;
    // Reset() re-runs InitDrop(..., m_InflowX, m_InflowZ, m_MaxParticles), so the
    // centre and particle budget must be stored here - they were not, and Reset()
    // silently reused the inflow defaults (1.0, 0.5, 8000) instead.
    m_InflowX = cx; m_InflowZ = cz;
    m_WallX0 = cx - 0.5f * tankSize; m_WallX1 = cx + 0.5f * tankSize;
    m_WallY1 = tankSize; m_WallZ0 = cz - 0.5f * tankSize; m_WallZ1 = cz + 0.5f * tankSize;
    m_WallTop = false;                // open top: the drop falls in from above
    m_h = 2.0f * params.spacing;
    m_mass = params.rho0 * params.spacing * params.spacing * params.spacing;
    m_Vol = params.spacing * params.spacing * params.spacing;

    m_X.clear(); m_V.clear(); m_Vadv.clear();
    m_dii.clear(); m_S.clear();
    m_Rho.clear(); m_RhoAdv.clear(); m_P.clear(); m_Pprev.clear(); m_Pold.clear();
    m_aii.clear(); m_deltaB.clear(); m_gradB.clear(); m_gradB2.clear();
    m_FluidNb.clear(); m_BoundNb.clear(); m_Normals.clear(); m_Colors.clear();
    m_Alpha.clear(); m_KappaV.clear(); m_KappaVPrev.clear(); m_KappaSeed.clear();
    m_N = 0;

    const float x0 = cx - 0.5f * tankSize, z0 = cz - 0.5f * tankSize;
    // 1) pool: fill the container cross-section up to poolHeight
    const int nMax = maxParticles > 0 ? maxParticles : 40000;
    m_MaxParticles = nMax;
    int nx = (int)std::ceil(tankSize / params.spacing);
    int ny = (int)std::ceil(poolHeight / params.spacing);
    for (int k = 0; k < nx && m_N < nMax; k++)
        for (int j = 0; j < ny && m_N < nMax; j++)
            for (int i = 0; i < nx && m_N < nMax; i++)
                PushFluid(x0 + (i + 0.5f) * params.spacing,
                          (j + 0.5f) * params.spacing,
                          z0 + (k + 0.5f) * params.spacing);

    BuildBoundary(true);              // open-top container walls (needed during the settle)

    // 2) PRE-SETTLE the pool to hydrostatic equilibrium. With maxIter=100 the
    //    pressure solve converges properly and a plain settle brings a still
    //    pool (closed-box test: mean v 0.14 m/s after 400 steps vs 0.76 with
    //    the old maxIter=25, where truncated iterations left a permanent
    //    residual drift). The drop is added AFTER the settle.
    {
        const int kSettle = 400;
        for (int k = 0; k < kSettle; k++) Step();
        m_Time = 0.0f;   // settle time is not simulation time
        m_Pprev.assign(m_N, 0.0f);
    }

    // 3) the hovering SPHERICAL drop (radius dropRadius), centred above the
    //    container, at rest; the pool gets priority, the drop takes the
    //    remaining particle budget
    const float dropBottom = tankSize + dropGap;          // lowest point of the sphere
    const float cyc = dropBottom + dropRadius;            // sphere centre height
    int nd = (int)std::ceil(dropRadius / params.spacing);
    for (int k = -nd; k <= nd && m_N < nMax; k++)
        for (int j = -nd; j <= nd && m_N < nMax; j++)
            for (int i = -nd; i <= nd && m_N < nMax; i++) {
                const float dx = i * params.spacing, dy = j * params.spacing, dz = k * params.spacing;
                if (dx * dx + dy * dy + dz * dz <= dropRadius * dropRadius)
                    PushFluid(cx + dx, cyc + dy, cz + dz);
            }

    m_Time = 0.0f;  m_AvgErr = 0.0f;  m_LastIter = 0;
    UpdateColors();
}

void FluidSolver::InjectFluid() {
    if (!m_Inflow || m_InflowPerStep <= 0) return;
    if (m_N >= m_MaxParticles) return;   // fixed water volume: stop injecting
    const float vFall = std::sqrt(2.0f * std::fabs(params.gravity) * m_InflowGap);
    // Emit one full disk layer every time the jet has fallen one particle
    // spacing. Counting STEPS would couple the layer spacing to the CFL
    // time step: during the hardest impact the CFL dt shrinks and the layers
    // pack 1.7-1.9x denser than spacing (measured analysis). Travel-based
    // emission keeps the jet density at exactly 1x.
    m_InflowCounter += vFall * params.dt;   // [m] travelled since last layer
    if (m_InflowCounter < params.spacing) return;
    m_InflowCounter -= params.spacing;
    const float L = m_BoxX;
    const float yEmit = L + m_InflowGap;
    const int n = (int)std::ceil(m_InflowRadius / params.spacing);
    // Cap the layer at what is still missing to reach maxParticles, so the
    // final layer tops the volume up to EXACTLY the target count.
    const int cap = std::min(m_InflowPerStep, m_MaxParticles - m_N);
    int spawned = 0;
    for (int i = -n; i <= n && spawned < cap; i++)
        for (int k = -n; k <= n && spawned < cap; k++) {
            const float dx = i * params.spacing, dz = k * params.spacing;
            if (dx * dx + dz * dz <= m_InflowRadius * m_InflowRadius) {
                PushFluid(m_InflowX + dx, yEmit, m_InflowZ + dz);
                m_V.back() = Math::Vector3(0.0f, -vFall, 0.0f);
                spawned++;
            }
        }
}

void FluidSolver::BuildBoundary(bool openTop) {
    m_BoundX.clear();
    auto addB = [&](float x, float y, float z) { m_BoundX.push_back(Math::Vector3(x, y, z)); };
    // Walls are generated from the SCENE's own wall planes (m_Wall*), never
    // from a hard-coded cube. The old open-top branch used m_BoxX as the size
    // for x, y AND z and the inflow centre for all three axes, so
    // InitDamOpen(2.0, 1.2, 1.0) produced wall particles at y in [0, 2.0] and
    // z in [-0.5, 1.5]: the z walls sat 0.5 m outside the fluid (h = 2*spacing,
    // so they contributed NO density or pressure - the fluid had no z boundary
    // at all) and the side walls floated above the 1.2 m tank.
    const float x0 = m_WallX0, x1 = m_WallX1;
    const float z0 = m_WallZ0, z1 = m_WallZ1;
    const float yTop = m_WallY1;
    const int nx = std::max(1, (int)std::ceil((x1 - x0) / params.spacing));
    const int ny = std::max(1, (int)std::ceil(yTop / params.spacing));
    const int nz = std::max(1, (int)std::ceil((z1 - z0) / params.spacing));
    // Sample each wall plane uniformly INCLUDING both end points. With the old
    // `x0 + i * spacing` the last sample of a non-integer span overshot the
    // wall (measured at spacing 0.04: y up to 1.24 for a 1.20 m tank, 154 wall
    // particles outside; at 0.03: x=2.01, y=1.23, z=1.02), so the SPH boundary
    // and the hard-collision plane disagreed by up to one particle spacing.
    auto px = [&](int i) { return x0 + (x1 - x0) * (float)i / (float)nx; };
    auto py = [&](int j) { return yTop * (float)j / (float)ny; };
    auto pz = [&](int k) { return z0 + (z1 - z0) * (float)k / (float)nz; };
    // single layer of wall particles on each face (paper/SPlisHSPlasH dam break)
    for (int i = 0; i <= nx; i++)
        for (int k = 0; k <= nz; k++)
            addB(px(i), 0.0f, pz(k));              // floor
    for (int j = 0; j <= ny; j++)
        for (int k = 0; k <= nz; k++) {
            addB(x0, py(j), pz(k));                // x0 wall
            addB(x1, py(j), pz(k));                // x1 wall
        }
    for (int i = 0; i <= nx; i++)
        for (int j = 0; j <= ny; j++) {
            addB(px(i), py(j), z0);                // z0 wall
            addB(px(i), py(j), z1);                // z1 wall
        }
    if (!openTop)   // closed box: lid
        for (int i = 0; i <= nx; i++)
            for (int k = 0; k <= nz; k++)
                addB(px(i), yTop, pz(k));

    // Boundary particle volumes V_b = 1/delta_b (Akinci et al. 2012): normalize by
    // the boundary's own kernel density so walls don't over-contribute density.
    const size_t nb = m_BoundX.size();
    m_BoundV.assign(nb, 0.0f);
    {
        int gwx = (int)std::ceil(m_BoxX / m_h) + 1;
        int ghy = (int)std::ceil(m_BoxY / m_h) + 1;
        int gdz = (int)std::ceil(m_BoxZ / m_h) + 1;
        std::vector<std::vector<int>> grid((size_t)gwx * ghy * gdz);
        auto idx3 = [&](int cx, int cy, int cz) {
            return (size_t)cz * gwx * ghy + (size_t)cy * gwx + (size_t)cx;
        };
        for (size_t b = 0; b < nb; b++) {
            int cx = std::clamp((int)std::floor(m_BoundX[b].x / m_h), 0, gwx - 1);
            int cy = std::clamp((int)std::floor(m_BoundX[b].y / m_h), 0, ghy - 1);
            int cz = std::clamp((int)std::floor(m_BoundX[b].z / m_h), 0, gdz - 1);
            grid[idx3(cx, cy, cz)].push_back((int)b);
        }
        for (size_t b = 0; b < nb; b++) {
            float delta = W(0.0f);
            int cx = std::clamp((int)std::floor(m_BoundX[b].x / m_h), 0, gwx - 1);
            int cy = std::clamp((int)std::floor(m_BoundX[b].y / m_h), 0, ghy - 1);
            int cz = std::clamp((int)std::floor(m_BoundX[b].z / m_h), 0, gdz - 1);
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int ccx = cx + dx, ccy = cy + dy, ccz = cz + dz;
                        if (ccx < 0 || ccx >= gwx || ccy < 0 || ccy >= ghy || ccz < 0 || ccz >= gdz) continue;
                        for (int bb : grid[idx3(ccx, ccy, ccz)]) {
                            if ((int)b == bb) continue;
                            float rr = (m_BoundX[b] - m_BoundX[bb]).length();
                            if (rr < m_h) delta += W(rr);
                        }
                    }
            m_BoundV[b] = 1.0f / delta;
        }
    }
}

void FluidSolver::Reset() {
    if (m_Inflow)      InitInflow(m_BoxX, m_InflowGap, m_InflowPerStep, m_InflowRadius, m_InflowX, m_InflowZ, m_MaxParticles);
    else if (m_Drop)   InitDrop(m_BoxX, m_DropPool, m_DropSize, m_DropGap, m_InflowX, m_InflowZ, m_MaxParticles);
    else if (m_UseDamOpen) InitDamOpen(m_BoxX, m_BoxY, m_BoxZ, m_WaterSize);
    else if (m_UseFullInit) InitFull(m_BoxX, m_BoxY, m_BoxZ, m_WaterSize);
    else               Init(m_BoxX, m_BoxY, m_BoxZ, m_WaterSize);
}

// ---------------------------------------------------------------------------
void FluidSolver::BuildNeighborhood() {
    // Grid origin = particle bbox (fluid + boundary) minus a margin, so the
    // hash covers exactly what exists. A fixed (0,0,0) origin is fine for the
    // dam tank but breaks the translated inflow/drop containers (everything
    // right of x = BoxX/2 clamps into the last column -> wrong neighbours).
    Math::Vector3 mn(1e30f), mx(-1e30f);
    for (int i = 0; i < m_N; i++) {
        mn.x = std::min(mn.x, m_X[i].x); mn.y = std::min(mn.y, m_X[i].y); mn.z = std::min(mn.z, m_X[i].z);
        mx.x = std::max(mx.x, m_X[i].x); mx.y = std::max(mx.y, m_X[i].y); mx.z = std::max(mx.z, m_X[i].z);
    }
    for (size_t b = 0; b < m_BoundX.size(); b++) {
        mn.x = std::min(mn.x, m_BoundX[b].x); mn.y = std::min(mn.y, m_BoundX[b].y); mn.z = std::min(mn.z, m_BoundX[b].z);
        mx.x = std::max(mx.x, m_BoundX[b].x); mx.y = std::max(mx.y, m_BoundX[b].y); mx.z = std::max(mx.z, m_BoundX[b].z);
    }
    const float margin = 2.0f * m_h;   // neighbourhood lookups reach +-h, plus slack
    m_GridOrigin = mn - margin;
    const Math::Vector3 ext = mx - mn + 2.0f * margin;
    m_GW = (int)std::ceil(ext.x / m_h) + 1;
    m_GH = (int)std::ceil(ext.y / m_h) + 1;
    m_GD = (int)std::ceil(ext.z / m_h) + 1;
    const int ncells = m_GW * m_GH * m_GD;
    m_CellCount.assign(ncells, 0);
    m_CellStart.resize(ncells + 1);

    auto cellIdx = [&](int cx, int cy, int cz) {
        return (size_t)cz * (size_t)m_GW * m_GH + (size_t)cy * m_GW + (size_t)cx;
    };
    auto cellOf = [&](const Math::Vector3& p) {
        int cx = std::clamp((int)std::floor((p.x - m_GridOrigin.x) / m_h), 0, m_GW - 1);
        int cy = std::clamp((int)std::floor((p.y - m_GridOrigin.y) / m_h), 0, m_GH - 1);
        int cz = std::clamp((int)std::floor((p.z - m_GridOrigin.z) / m_h), 0, m_GD - 1);
        return cellIdx(cx, cy, cz);
    };

    // Paper [30] (Ihmsen et al., "Parallel data structures for SPH"): build the
    // spatial hash in 3 passes — count (atomic), prefix sum, scatter (atomic
    // cursor) — instead of a locked push into per-cell std::vectors.
    const int total = m_N + (int)m_BoundX.size();
    #pragma omp parallel for
    for (int i = 0; i < total; i++) {
        const Math::Vector3& p = (i < m_N) ? m_X[i] : m_BoundX[i - m_N];
        #pragma omp atomic
        m_CellCount[cellOf(p)]++;
    }
    m_CellStart[0] = 0;
    for (int c = 0; c < ncells; c++) m_CellStart[c + 1] = m_CellStart[c] + m_CellCount[c];
    m_CellParticles.resize(total);
    std::vector<int> cursor = m_CellStart;
    #pragma omp parallel for
    for (int i = 0; i < total; i++) {
        const Math::Vector3& p = (i < m_N) ? m_X[i] : m_BoundX[i - m_N];
        int c = (int)cellOf(p);
        int slot;
        #pragma omp atomic capture
        slot = cursor[c]++;
        m_CellParticles[slot] = i;
    }

    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        auto& nb = m_FluidNb[i];
        auto& bnb = m_BoundNb[i];
        nb.clear();
        bnb.clear();
        float dsum = 0.0f;
        Math::Vector3 gsum;
        float g2sum = 0.0f;
        int cx = (int)std::floor((m_X[i].x - m_GridOrigin.x) / m_h);
        int cy = (int)std::floor((m_X[i].y - m_GridOrigin.y) / m_h);
        int cz = (int)std::floor((m_X[i].z - m_GridOrigin.z) / m_h);
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int ccx = cx + dx, ccy = cy + dy, ccz = cz + dz;
                    if (ccx < 0 || ccx >= m_GW || ccy < 0 || ccy >= m_GH || ccz < 0 || ccz >= m_GD) continue;
                    const int c = (int)cellIdx(ccx, ccy, ccz);
                    for (int s = m_CellStart[c]; s < m_CellStart[c + 1]; s++) {
                        int idx = m_CellParticles[s];
                        if (idx >= m_N) {
                            Math::Vector3 r = m_X[i] - m_BoundX[idx - m_N];
                            float rr = r.length();
                            if (rr < m_h) {
                                float vb = m_BoundV[idx - m_N];
                                dsum += vb * W(rr);
                                Math::Vector3 g = gradW(r) * vb;
                                gsum += g;
                                g2sum += g.dot(g);
                                bnb.push_back(idx - m_N);   // for surface tension
                            }
                        } else if (idx != i) {
                            if ((m_X[i] - m_X[idx]).length() < m_h) nb.push_back(idx);
                        }
                    }
                }
        m_deltaB[i] = dsum;
        m_gradB[i] = gsum;
        m_gradB2[i] = g2sum;
    }
}

// ---------------------------------------------------------------------------
// Predict advection (SPlisHSPlasH normalized formulation, NO dt^2 in d/aii)
// ---------------------------------------------------------------------------
void FluidSolver::PredictAdvection() {
    const bool hasVisc = params.viscosity > 0.0f;
    const bool hasTens = params.surfaceTension > 0.0f;

    // Pass 1: density (pure function of x(t)); no reads of m_Rho, so the
    // XSPH / Akinci-2013 passes below can safely use this fresh rho(t).
    ComputeDensity();
    // Pass 1b: d_ii (IISPH only; DFSPH uses the precomputed factor alpha_i).
    if (params.solver == 0) {
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++) {
            Math::Vector3 gsum = m_gradB[i];
            for (int j : m_FluidNb[i])
                gsum += m_Vol * gradW(m_X[i] - m_X[j]);
            m_dii[i] = gsum * (-1.0f / (m_Rho[i] * m_Rho[i]));
        }
    }

    // Pass 2a: Akinci-2013 color-field normals n_i = h * sum_j (V_j/rhoN_j) gradW_ij
    // (SPlisHSPlasH scales the raw normal by the support radius h).
    if (hasTens) {
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++) {
            Math::Vector3 n;
            for (int j : m_FluidNb[i]) {
                if (m_Rho[j] > 1e-6f)
                    n += (m_Vol / m_Rho[j]) * gradW(m_X[i] - m_X[j]);
            }
            m_Normals[i] = n * m_h;
        }
    }

    // Pass 2b: non-pressure forces -> v_adv (gravity + XSPH + Akinci-2013).
    // XSPH (Schechter & Bridson 2012, as in SPlisHSPlasH): velocity-space filter
    //   dv_i = -xsph * sum_j (V_j/rhoN_j) (v_i - v_j) W_ij
    // Akinci-2013 surface tension: per fluid neighbor j
    //   accel = -k m_j xhat_ij W_c(r_ij)  -  k (n_i - n_j),   weighted by K_ij = 2/(rhoN_i+rhoN_j),
    // plus boundary adhesion  -kb rho0 V_b xhat_ib W_a(r_ib).
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        Math::Vector3 dvXSPH;
        if (hasVisc) {
            for (int j : m_FluidNb[i]) {
                if (m_Rho[j] < 1e-6f) continue;
                Math::Vector3 r = m_X[i] - m_X[j];
                dvXSPH -= params.viscosity * (m_Vol / m_Rho[j]) * (m_V[i] - m_V[j]) * W(r.length());
            }
        }
        Math::Vector3 aTens;
        if (hasTens) {
            const Math::Vector3 ni = m_Normals[i];
            for (int j : m_FluidNb[i]) {
                Math::Vector3 r = m_X[i] - m_X[j];
                const float rr = r.length();
                if (rr < 1e-9f) continue;
                Math::Vector3 xhat = r * (1.0f / rr);
                Math::Vector3 accel;
                // cohesion (attractive toward the neighbor)
                accel -= params.surfaceTension * (params.rho0 * m_Vol) * xhat * WCohesion(rr);
                // curvature: -k (n_i - n_j)
                accel -= params.surfaceTension * (ni - m_Normals[j]);
                const float Kij = 2.0f / (m_Rho[i] + m_Rho[j]);
                aTens += Kij * accel;
            }
            // adhesion to Akinci-2012 boundary particles
            for (int b : m_BoundNb[i]) {
                Math::Vector3 r = m_X[i] - m_BoundX[b];
                const float rr = r.length();
                if (rr < 1e-9f) continue;
                Math::Vector3 xhat = r * (1.0f / rr);
                aTens -= params.surfaceTensionBoundary * params.rho0 * m_BoundV[b] * xhat * WAdhesion(rr);
            }
        }
        m_Vadv[i] = m_V[i] + Math::Vector3(0.0f, params.dt * params.gravity, 0.0f) + aTens * params.dt + dvXSPH;
    }

    // Pass 3 (IISPH only): predicted density for the pressure solve, warm
    // start, and the a_ii diagonal. DFSPH recomputes rho* inside its constant
    // density solver, so this whole pass is skipped for solver == 1.
    if (params.solver == 0) {
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++) {
            float rhoAdv = m_Rho[i];
            Math::Vector3 vi = m_Vadv[i];
            for (int j : m_FluidNb[i]) {
                Math::Vector3 r = m_X[i] - m_X[j];
                rhoAdv += params.dt * m_Vol * (vi - m_Vadv[j]).dot(gradW(r));
            }
            rhoAdv += params.dt * vi.dot(m_gradB[i]);   // v_b = 0 (static walls)
            m_RhoAdv[i] = rhoAdv;

            m_P[i] = 0.5f * m_Pprev[i];

            float a = 0.0f;
            float coefJi = m_Vol / (m_Rho[i] * m_Rho[i]);
            for (int j : m_FluidNb[i]) {
                Math::Vector3 g = gradW(m_X[i] - m_X[j]);
                a += m_Vol * (m_dii[i] - g * coefJi).dot(g);
            }
            // boundary neighbors: sum_b V_b (d_ii - d_ji).gradW_ib = d_ii . gradB
            // (the paper's Eq. 12 boundary part; an extra -coefJi*|gradB|^2 term
            // was present and is removed: it shrinks a_ii for wall-adjacent
            // particles, which the shallow open-tank pool amplifies into a
            // permanent wall-squeeze jitter - measured pool vmax 1.5-4 m/s that
            // no settle/relaxation could remove.)
            a += m_dii[i].dot(m_gradB[i]);
            m_aii[i] = a;
        }
    }
}

// ---------------------------------------------------------------------------
// DFSPH (Bender & Koschier 2017) — paper Algorithm 1-3, normalized formulation
// (rhoN = rho/rho0, V = spacing^3, boundary particles are static).
// ---------------------------------------------------------------------------

// Density rhoN_i = sum_j V_j W_ij + sum_b V_b W_ib.
void FluidSolver::ComputeDensity() {
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        float rhoN = m_Vol * W(0.0f);
        for (int j : m_FluidNb[i]) {
            float rr = (m_X[i] - m_X[j]).length();
            rhoN += m_Vol * W(rr);
        }
        m_Rho[i] = rhoN + m_deltaB[i];
    }
}

// Factor alpha_i (paper Eq. 8): inverse of
//   sum_j |V_j gradW_ij|^2  +  |sum_j V_j gradW_ij + sum_b V_b gradW_ib|^2
// (SPlisHSPlasH computeDFSPHFactor; boundary particles are included in the
// gradient sum so wall-adjacent fluid gets the correct stiffness).
void FluidSolver::ComputeDFSPHFactor() {
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        Math::Vector3 gradPI;   // sum V gradW over fluid + boundary
        float sum = 0.0f;
        for (int j : m_FluidNb[i]) {
            Math::Vector3 gp = m_Vol * gradW(m_X[i] - m_X[j]);
            gradPI += gp;
            sum += gp.dot(gp);
        }
        for (int b : m_BoundNb[i]) {
            Math::Vector3 gp = m_BoundV[b] * gradW(m_X[i] - m_BoundX[b]);
            gradPI += gp;
        }
        sum += gradPI.dot(gradPI);
        // Clamp tiny denominators (paper Sec.3.2: threshold 1e-6).
        m_Alpha[i] = (sum > 1e-6f) ? 1.0f / sum : 0.0f;
    }
}

// Drho/Dt at the current velocity (paper Eq. 6, normalized):
//   sum_j V_j (v_i - v_j) . gradW_ij + v_i . gradB_i   (v_b = 0)
float FluidSolver::RhoDotOf(int i) const {
    float drho = 0.0f;
    const Math::Vector3 vi = m_Vadv[i];
    for (int j : m_FluidNb[i]) {
        Math::Vector3 r = m_X[i] - m_X[j];
        drho += m_Vol * (vi - m_Vadv[j]).dot(gradW(r));
    }
    drho += vi.dot(m_gradB[i]);
    return drho;
}

// Predicted density rho*_i = rhoN_i + dt * Drho/Dt (paper Sec. 3.3).
float FluidSolver::RhoAdvOf(int i) const {
    return m_Rho[i] + params.dt * RhoDotOf(i);
}

// Constant density solver (paper Algorithm 3): enforce rho* - 1 = 0 by
// adapting velocities BEFORE the position integration.
//   kappa_i = max(rho*_i - 1, 0) * alpha_i / dt^2
//   v*_i -= dt * sum_j V_j (kappa_i + kappa_j) gradW_ij   (+ boundary term)
// (the rhoN_i factors cancel in the normalized p/rho^2 form, as in
// SPlisHSPlasH). min 2 iterations, stops at avg |rho* - 1| < eta (0.01%).
void FluidSolver::CorrectDensityError() {
    const float h = params.dt;
    const float invH2 = 1.0f / (h * h);
    const float omega = params.omega;

    // Warm start (SPlisHSPlasH): seed the pressure with 0.5x the previous
    // step's kappa (stored as kappa*h^2 so it is step-size independent, capped
    // at 0.00025 so a stale outlier cannot inject momentum) and apply it once
    // before iterating. Density-deficit particles stay at p = 0.
    // Seed the stiffness: warm start reuses the previous step's kappa (stored
    // as kappa*h^2, capped at 0.00025 so a stale outlier cannot inject
    // momentum); otherwise the closed-form rho*-1.
    // The seed is applied ONLY by the iteration below, at its full weight.
    // The previous code applied 0.5x here and 0.5x again in the first
    // iteration, which is the same impulse as 1.0x once - so the factor is
    // 1.0 now that the duplicate application is gone. Halving it as well
    // under-corrects and the body slowly inflates (measured: signed density
    // error -5.4% vs -3.5% after 20 dam-break steps).
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        const float rhoAdv = RhoAdvOf(i);
        m_RhoAdv[i] = rhoAdv;
        if (params.warmStart)
            m_P[i] = (rhoAdv > 1.0f) ? std::min(m_Pprev[i], 0.00025f) * invH2 : 0.0f;
        else
            // Negative pressure clamp (paper Sec.4): rho* < 1 -> p = 0.
            m_P[i] = std::max(rhoAdv - 1.0f, 0.0f) * m_Alpha[i] * invH2;
    }

    int iter = 0;
    float err = 1e9f, errAbs = 1e9f, errMax = 0.0f;
    // Convergence stays on the SIGNED average (SPlisHSPlasH semantics): the
    // closed-form kappa is only ever positive, so a body that is already
    // under-dense on average (err < 0) must not be pushed further - doing so
    // only inflates the free surface. Measured on a 20k dam break, mean |e|
    // by iteration cap: 1 -> 6.30%, 2 -> 2.71% (best), 3 -> 2.83%,
    // 5 -> 3.25%, 10 -> 3.98%, 100 -> 4.57%. The signed mean is therefore the
    // right stopping rule, but it MUST NOT be used as the only diagnostic:
    // mean|e| and max|e| are tracked below and reported separately.
    while ((err > params.eta || iter < 2) && iter < params.maxIter) {
        iter++;
        // Adapt velocities with the current stiffness values. The closed-form
        // kappa = (rho*-1) alpha/dt^2 over-corrects for the wall-corner density
        // surplus of the single-layer boundary (initial rho ~ 1.7 there); the
        // same relaxed-Jacobi omega as IISPH damps the correction.
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++) {
            Math::Vector3 a;
            for (int j : m_FluidNb[i]) {
                Math::Vector3 g = gradW(m_X[i] - m_X[j]);
                a -= m_Vol * (m_P[i] + m_P[j]) * g;
            }
            for (int b : m_BoundNb[i])
                a -= m_BoundV[b] * m_P[i] * gradW(m_X[i] - m_BoundX[b]);
            m_Vadv[i] += omega * h * a;
        }
        // Re-predict the density and refresh kappa. Convergence uses the
        // SIGNED average (SPlisHSPlasH avg_density_err): wall-adjacent
        // particles carry a geometric density surplus (single-layer boundary)
        // that cannot be removed by moving (hard-wall clamp), while surface
        // particles carry a deficit; the signed mean lets these cancel instead
        // of forcing the solver to run into maxIter every step.
        double errSum = 0.0, absSum = 0.0;
        float maxAbs = 0.0f;
        #pragma omp parallel for reduction(+:errSum,absSum) reduction(max:maxAbs)
        for (int i = 0; i < m_N; i++) {
            const float rhoAdv = RhoAdvOf(i);
            m_RhoAdv[i] = rhoAdv;
            m_P[i] = std::max(rhoAdv - 1.0f, 0.0f) * m_Alpha[i] * invH2;
            const float e = rhoAdv - 1.0f;
            errSum += e;
            absSum += std::fabs(e);
            maxAbs = std::max(maxAbs, std::fabs(e));
        }
        err = (float)(errSum / m_N);
        errAbs = (float)(absSum / m_N);
        errMax = maxAbs;
    }
    m_LastIter = iter;
    m_AvgErr = err;
    m_AvgAbsErr = errAbs;
    m_MaxErr = errMax;

    // Keep kappa*h^2 for the next step's warm start (SPlisHSPlasH multiplies
    // the pressure by h^2 at the end of the pressure solve).
    if (params.warmStart) {
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++)
            m_Pprev[i] = m_P[i] * h * h;
    }
}

// Divergence-free solver (paper Algorithm 2): enforce Drho/Dt = 0 by
// adapting the final velocities AFTER the position integration.
//   kappa_v_i = (Drho/Dt)_i * alpha_i / dt
//   v_i -= dt * sum_j V_j (kappa_v_i + kappa_v_j) gradW_ij  (+ boundary term)
// min 1 iteration, stops at avg |Drho/Dt| < etaV (0.1%).
void FluidSolver::CorrectDivergenceError() {
    const float h = params.dt;
    const float invH = 1.0f / h;
    const float omega = params.omega;
    const int kMinNeighbors = 20;

    // m_KappaV temporarily stores Drho/Dt (stiffness is Drho/Dt * alpha / h).
    // Particles with a deficient neighborhood (free surface / splashes) are
    // excluded from the divergence solve (SPlisHSPlasH: numNeighbors < 20 ->
    // densityAdv = 0), which also stops them from over-correcting.
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        if ((int)(m_FluidNb[i].size() + m_BoundNb[i].size()) < kMinNeighbors)
            m_KappaV[i] = 0.0f;
        else
            m_KappaV[i] = RhoDotOf(i);
    }

    // Warm-start seed in kappa_v units (Drho/Dt * alpha / h), capped. It is
    // applied by the iteration below, exactly once: the previous code applied
    // it here AND again in the first iteration with the same value (2x), and
    // the iteration then multiplied it by alpha/h a second time on top.
    if (params.warmStart) {
        m_KappaSeed.resize(m_N);
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++) {
            const bool poor = (int)(m_FluidNb[i].size() + m_BoundNb[i].size()) < kMinNeighbors;
            m_KappaSeed[i] = poor ? 0.0f : std::min(m_KappaVPrev[i], 0.00025f) * invH;
        }
    }

    // Convergence uses the SIGNED average: positive and negative divergence
    // cancel, exactly like SPlisHSPlasH's avg divergence error. Aggregating
    // |Drho/Dt| instead (as one revision of this file did) makes the test
    // `err > etaV/h` true almost always, so the solver runs to maxIter and
    // re-applies the closed-form correction every iteration - measured: 100 vs
    // 1 iterations on the first step of a dam break, with the whole body
    // jittering.
    double errSum0 = 0.0;
    #pragma omp parallel for reduction(+:errSum0)
    for (int i = 0; i < m_N; i++)
        errSum0 += m_KappaV[i];
    float err = (float)(errSum0 / m_N);

    int iter = 0;
    // SPlisHSPlasH scales the tolerance by 1/h: the divergence error is a
    // density change RATE (1/s), so eta_v = 0.1% / h.
    while ((err > params.etaV / h || iter < 1) && iter < params.maxIter) {
        iter++;
        // kappa_v = Drho/Dt * alpha / h. The warm-start seed is already in
        // kappa_v units, so the first iteration must not scale it again.
        if (iter == 1 && params.warmStart) {
            #pragma omp parallel for
            for (int i = 0; i < m_N; i++) m_KappaV[i] = m_KappaSeed[i];
        } else {
            #pragma omp parallel for
            for (int i = 0; i < m_N; i++) m_KappaV[i] *= m_Alpha[i] * invH;
        }
        // Adapt velocities (relaxed with omega). The divergence stiffness
        // kappa_v = Drho/Dt*alpha/h needs the same damping as the constant
        // density solver: without it the free-surface particles over-correct
        // and the body slowly inflates (measured avg rho down to 0.7).
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++) {
            Math::Vector3 a;
            for (int j : m_FluidNb[i]) {
                Math::Vector3 g = gradW(m_X[i] - m_X[j]);
                a -= m_Vol * (m_KappaV[i] + m_KappaV[j]) * g;
            }
            for (int b : m_BoundNb[i])
                a -= m_BoundV[b] * m_KappaV[i] * gradW(m_X[i] - m_BoundX[b]);
            m_Vadv[i] += omega * h * a;
        }
        // Convergence: signed avg Drho/Dt after the update (SPlisHSPlasH
        // convention; the geometric boundary surplus cancels against the
        // free-surface deficit).
        double errSum = 0.0;
        #pragma omp parallel for reduction(+:errSum)
        for (int i = 0; i < m_N; i++) {
            if ((int)(m_FluidNb[i].size() + m_BoundNb[i].size()) < kMinNeighbors) {
                m_KappaV[i] = 0.0f;
                continue;
            }
            const float rhoDot = RhoDotOf(i);
            m_KappaV[i] = rhoDot;   // reused as the next iteration's input
            errSum += rhoDot;
        }
        err = (float)(errSum / m_N);
    }
    m_LastIterV = iter;

    // Keep kappa_v*h (= Drho/Dt * alpha) for the next step's warm start
    // (SPlisHSPlasH multiplies the divergence stiffness by h at the end).
    if (params.warmStart) {
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++)
            m_KappaVPrev[i] = m_KappaV[i] * m_Alpha[i];
    }
}

// Hard-wall safety net shared by IISPH Integrate and the DFSPH step:
// keep the particle CENTER at least half a spacing from the wall.
void FluidSolver::ApplyWallClamp() {
    const float half = 0.5f * params.spacing;
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        Math::Vector3& x = m_X[i];
        Math::Vector3& v = m_Vadv[i];
        if (x.x < m_WallX0 + half)          { x.x = m_WallX0 + half;          if (v.x < 0.0f) v.x = 0.0f; }
        if (x.x > m_WallX1 - half)          { x.x = m_WallX1 - half;          if (v.x > 0.0f) v.x = 0.0f; }
        if (x.y < half)                     { x.y = half;                     if (v.y < 0.0f) v.y = 0.0f; }
        if (m_WallTop && x.y > m_WallY1 - half) { x.y = m_WallY1 - half;      if (v.y > 0.0f) v.y = 0.0f; }
        if (x.z < m_WallZ0 + half)          { x.z = m_WallZ0 + half;          if (v.z < 0.0f) v.z = 0.0f; }
        if (x.z > m_WallZ1 - half)          { x.z = m_WallZ1 - half;          if (v.z > 0.0f) v.z = 0.0f; }
    }
}

// ---------------------------------------------------------------------------
void FluidSolver::PressureSolve() {
    const float h = params.dt, h2 = h * h, omega = params.omega;
    // Strict (relaxed) Jacobi. Every iteration reads the pressure of the
    // PREVIOUS iteration and writes the new one into m_Pold, then the two are
    // swapped. Updating m_P[i] in place while other threads still read m_P[j]
    // is a data race: the result then depends on thread scheduling and the
    // iteration is no longer Jacobi (measured: three 4-thread runs of the same
    // scene produced three different state hashes, while single-thread runs
    // were bit-identical).
    m_Pold.resize(m_N);
    int l = 0;
    while (true) {
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++) {
            Math::Vector3 s;
            for (int j : m_FluidNb[i])
                s += gradW(m_X[i] - m_X[j]) * (-m_Vol / (m_Rho[j] * m_Rho[j]) * m_P[j]);
            m_S[i] = s;
        }

        double errSum = 0.0, absSum = 0.0;
        float errMax = 0.0f;
        #pragma omp parallel for reduction(+:errSum,absSum) reduction(max:errMax)
        for (int i = 0; i < m_N; i++) {
            const float b = 1.0f - m_RhoAdv[i];
            Math::Vector3 si = m_S[i];
            float coefJi = m_Vol / (m_Rho[i] * m_Rho[i]) * m_P[i];
            float sum = 0.0f;
            for (int j : m_FluidNb[i]) {
                Math::Vector3 g = gradW(m_X[i] - m_X[j]);
                Math::Vector3 inner = si - m_dii[j] * m_P[j] - (m_S[j] - g * coefJi);
                sum += m_Vol * inner.dot(g);
            }
            // boundary term: sum_b V_b S_i . gradW_ib
            sum += si.dot(m_gradB[i]);

            float pnew = 0.0f;
            // Plain IISPH update (paper Eq. 16) with the denom floor + p>=0
            // clamp. A forced free-surface p=0 was tried for the open-top
            // scenes but it keeps the surface layer permanently loose (the
            // layer is excluded from pressure transmission, so it jitters at
            // the v-clamp instead of settling) - measured pool vmax 4 m/s
            // after 300 settle steps, vs 0.9 m/s with the plain update. The
            // open-tank pool is instead PRE-SETTLED in InitDrop.
            // denom = a_ii * dt^2. Isolated/edge particles have a_ii orders of
            // magnitude below normal; without a floor, omega/denom explodes and
            // injects huge pressure.
            const float denom = m_aii[i] * h2;
            // Isolated splash particles (few fluid neighbours) have tiny
            // |a_ii|; skipping them keeps p=0 so the pressure solve never
            // launches them at hundreds of m/s.
            if (std::fabs(denom) > 3e-5f)
                pnew = (1.0f - omega) * m_P[i] + omega / denom * (b - h2 * sum);
            if (pnew < 0.0f) pnew = 0.0f;
            // Numerical anti-divergence cap: a free-flying particle with a low
            // neighbor count gets a tiny |denom|, so omega/denom can inflate
            // p far beyond any physical pressure (hydrostatic here ~ 5). Cap it
            // at 100 (20x headroom) so a single outlier cannot snowball through
            // the Jacobi passes. Physical peaks stay well below this.
            if (pnew > 100.0f) pnew = 100.0f;
            m_Pold[i] = pnew;
            // SPlisHSPlasH convergence: algebraic residual, only for p>0 particles.
            // The signed mean is what the solver tests against eta; the mean
            // absolute and max values are reported so a compressed core and a
            // dilated surface cannot cancel each other out of the diagnostics.
            if (pnew != 0.0f) {
                const float res = (m_aii[i] * pnew + sum) * h2 - b;
                errSum += res;
                absSum += std::fabs(res);
                errMax = std::max(errMax, std::fabs(res));
            }
        }
        m_Pold.swap(m_P);   // m_P is now the newest iterate, m_Pold the previous
        l++;
        m_AvgErr = (float)(errSum / m_N);
        m_AvgAbsErr = (float)(absSum / m_N);
        m_MaxErr = errMax;
        m_LastIter = l;
        if (m_traceNext) m_convTrace.push_back(std::fabs(m_AvgErr));
        if (l >= 2 && m_AvgAbsErr <= params.eta) break;
        if (l >= params.maxIter) break;
    }
    m_traceNext = false;
}

// ---------------------------------------------------------------------------
void FluidSolver::Integrate() {
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        Math::Vector3 ap = m_dii[i] * m_P[i] + m_S[i];
        m_V[i] = m_Vadv[i] + ap * params.dt;
        Math::Vector3 xnew = m_X[i] + m_V[i] * params.dt;
        // Hard-wall safety net: keep the particle CENTER at least half a spacing
        // from the wall (particle radius). Clamping to the wall plane itself would
        // overlap particle and boundary particle -> rhoN ~ 1.7 -> pressure blow-up.
        const float half = 0.5f * params.spacing;
        if (xnew.x < m_WallX0 + half)          { xnew.x = m_WallX0 + half;          if (m_V[i].x < 0.0f) m_V[i].x = 0.0f; }
        if (xnew.x > m_WallX1 - half)          { xnew.x = m_WallX1 - half;          if (m_V[i].x > 0.0f) m_V[i].x = 0.0f; }
        if (xnew.y < half)                     { xnew.y = half;                     if (m_V[i].y < 0.0f) m_V[i].y = 0.0f; }
        if (m_WallTop && xnew.y > m_WallY1 - half) { xnew.y = m_WallY1 - half;      if (m_V[i].y > 0.0f) m_V[i].y = 0.0f; }
        if (xnew.z < m_WallZ0 + half)          { xnew.z = m_WallZ0 + half;          if (m_V[i].z < 0.0f) m_V[i].z = 0.0f; }
        if (xnew.z > m_WallZ1 - half)          { xnew.z = m_WallZ1 - half;          if (m_V[i].z > 0.0f) m_V[i].z = 0.0f; }
        m_X[i] = xnew;
    }
    // Numerical-splash damping: a handful of particles get launched to 60-300
    // m/s by pressure outliers (cold start, drop impact). Physical water here
    // never exceeds ~10 m/s; clamping removes the non-physical tail without
    // touching the bulk flow. (This is why the open-top scenes no longer lose
    // particles through the opening.)
    const float kVMax = 4.0f;   // physical splash here stays under ~4 m/s
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        const float v = m_V[i].length();
        if (v > kVMax) m_V[i] *= kVMax / v;
    }
    m_Pprev = m_P;
    m_Time += params.dt;
    UpdateColors();
}

void FluidSolver::UpdateColors() {
    float vmax = 1e-6f;
    #pragma omp parallel for reduction(max:vmax)
    for (int i = 0; i < m_N; i++) vmax = std::max(vmax, m_V[i].length());
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        float s = std::min(1.0f, m_V[i].length() / vmax);
        m_Colors[3 * i + 0] = 0.15f + 0.85f * s;
        m_Colors[3 * i + 1] = 0.35f + 0.65f * s;
        m_Colors[3 * i + 2] = 1.0f;
    }
}

void FluidSolver::Step() {
    // CFL condition (paper Sec.3.1): dt <= 0.4 * d / vmax with d = particle
    // diameter (= spacing). Keeps the particle displacement per step well
    // below the kernel support (helps when the water block slams into the
    // tank bottom).
    float vmax = 0.0f;
    for (int i = 0; i < m_N; i++) vmax = std::max(vmax, m_V[i].length());
    float dt = params.dt;
    if (params.useCFL)
        dt = std::min(params.dt, 0.4f * params.spacing / std::max(vmax, 0.1f));
    float prevDt = params.dt;
    params.dt = dt;

    InjectFluid();          // open-tank scene: spawn this step's jet particles
    BuildNeighborhood();
    PredictAdvection();     // rho, Akinci-2013 normals, XSPH + surface tension -> v_adv

    if (params.solver == 1) {
        // ---- DFSPH (paper Algorithm 1) ----
        // 1) constant density solver on the predicted velocities
        double tPress = 0.0;
        double tp = omp_get_wtime();
        ComputeDFSPHFactor();
        CorrectDensityError();
        tPress += omp_get_wtime() - tp;
        // 2) integrate positions, rebuild neighborhoods, recompute rho + alpha
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++) m_X[i] += m_Vadv[i] * params.dt;
        ApplyWallClamp();            // hard-wall safety net (existing convention)
        BuildNeighborhood();
        ComputeDensity();
        ComputeDFSPHFactor();
        // 3) divergence-free solver on the final velocities
        tp = omp_get_wtime();
        CorrectDivergenceError();
        m_LastPressureMs = (float)((tPress + omp_get_wtime() - tp) * 1000.0);
        m_V = m_Vadv;
        // Numerical-splash damping (same guard as the IISPH path).
        const float kVMax = 4.0f;
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++) {
            const float v = m_V[i].length();
            if (v > kVMax) m_V[i] *= kVMax / v;
        }
        m_Time += params.dt;
        UpdateColors();
    } else {
        const double tp = omp_get_wtime();
        PressureSolve();
        m_LastPressureMs = (float)((omp_get_wtime() - tp) * 1000.0);
        Integrate();
    }

    params.dt = prevDt;
}
