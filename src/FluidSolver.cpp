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
void FluidSolver::Init(float boxX, float boxY, float boxZ, float waterSize) {
    m_BoxX = boxX; m_BoxY = boxY; m_BoxZ = boxZ; m_WaterSize = waterSize;
    m_UseFullInit = false;
    m_Inflow = false;   // CRITICAL: Reset() branches on this
    m_Drop = false;
    m_WallX0 = 0.0f; m_WallX1 = boxX; m_WallY1 = boxY; m_WallZ0 = 0.0f; m_WallZ1 = boxZ;
    m_WallTop = true;
    m_h = 2.0f * params.spacing;
    m_mass = params.rho0 * params.spacing * params.spacing * params.spacing;
    m_Vol = params.spacing * params.spacing * params.spacing;

    m_X.clear(); m_V.clear(); m_Vadv.clear();
    m_dii.clear(); m_S.clear();
    m_Rho.clear(); m_RhoAdv.clear(); m_P.clear(); m_Pprev.clear();
    m_aii.clear(); m_deltaB.clear(); m_gradB.clear(); m_gradB2.clear();
    m_FluidNb.clear(); m_BoundNb.clear(); m_Colors.clear();
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
    m_Rho.clear(); m_RhoAdv.clear(); m_P.clear(); m_Pprev.clear();
    m_aii.clear(); m_deltaB.clear(); m_gradB.clear(); m_gradB2.clear();
    m_FluidNb.clear(); m_BoundNb.clear(); m_Colors.clear();
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
    m_WallX0 = 0.0f; m_WallX1 = boxX; m_WallY1 = boxY; m_WallZ0 = 0.0f; m_WallZ1 = boxZ;
    m_WallTop = true;
    m_h = 2.0f * params.spacing;
    m_mass = params.rho0 * params.spacing * params.spacing * params.spacing;
    m_Vol = params.spacing * params.spacing * params.spacing;

    m_X.clear(); m_V.clear(); m_Vadv.clear();
    m_dii.clear(); m_S.clear();
    m_Rho.clear(); m_RhoAdv.clear(); m_P.clear(); m_Pprev.clear();
    m_aii.clear(); m_deltaB.clear(); m_gradB.clear(); m_gradB2.clear();
    m_FluidNb.clear(); m_BoundNb.clear(); m_Colors.clear();
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
    m_WallX0 = cx - 0.5f * tankSize; m_WallX1 = cx + 0.5f * tankSize;
    m_WallY1 = tankSize; m_WallZ0 = cz - 0.5f * tankSize; m_WallZ1 = cz + 0.5f * tankSize;
    m_WallTop = false;   // open top: the jet falls in from above
    m_h = 2.0f * params.spacing;
    m_mass = params.rho0 * params.spacing * params.spacing * params.spacing;
    m_Vol = params.spacing * params.spacing * params.spacing;

    m_X.clear(); m_V.clear(); m_Vadv.clear();
    m_dii.clear(); m_S.clear();
    m_Rho.clear(); m_RhoAdv.clear(); m_P.clear(); m_Pprev.clear();
    m_aii.clear(); m_deltaB.clear(); m_gradB.clear(); m_gradB2.clear();
    m_FluidNb.clear(); m_BoundNb.clear(); m_Colors.clear();
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
    m_WallX0 = cx - 0.5f * tankSize; m_WallX1 = cx + 0.5f * tankSize;
    m_WallY1 = tankSize; m_WallZ0 = cz - 0.5f * tankSize; m_WallZ1 = cz + 0.5f * tankSize;
    m_WallTop = false;                // open top: the drop falls in from above
    m_h = 2.0f * params.spacing;
    m_mass = params.rho0 * params.spacing * params.spacing * params.spacing;
    m_Vol = params.spacing * params.spacing * params.spacing;

    m_X.clear(); m_V.clear(); m_Vadv.clear();
    m_dii.clear(); m_S.clear();
    m_Rho.clear(); m_RhoAdv.clear(); m_P.clear(); m_Pprev.clear();
    m_aii.clear(); m_deltaB.clear(); m_gradB.clear(); m_gradB2.clear();
    m_FluidNb.clear(); m_BoundNb.clear(); m_Colors.clear();
    m_N = 0;

    const float x0 = cx - 0.5f * tankSize, z0 = cz - 0.5f * tankSize;
    // 1) pool: fill the container cross-section up to poolHeight
    const int nMax = maxParticles > 0 ? maxParticles : 40000;
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
    if (openTop) {
        // open-top cubic container: floor + four walls, NO top lid.
        const float L = m_BoxX;
        const float x0 = m_InflowX - 0.5f * L, x1 = m_InflowX + 0.5f * L;
        const float z0 = m_InflowZ - 0.5f * L, z1 = m_InflowZ + 0.5f * L;
        const int n = (int)std::ceil(L / params.spacing);
        for (int i = 0; i <= n; i++)
            for (int k = 0; k <= n; k++)
                addB(x0 + i * params.spacing, 0.0f, z0 + k * params.spacing);         // floor
        for (int j = 0; j <= n; j++)
            for (int k = 0; k <= n; k++) {
                addB(x0, j * params.spacing, z0 + k * params.spacing);                // x0 wall
                addB(x1, j * params.spacing, z0 + k * params.spacing);                // x1 wall
            }
        for (int i = 0; i <= n; i++)
            for (int j = 0; j <= n; j++) {
                addB(x0 + i * params.spacing, j * params.spacing, z0);                // z0 wall
                addB(x0 + i * params.spacing, j * params.spacing, z1);                // z1 wall
            }
    } else {
    int nx = (int)std::ceil(m_BoxX / params.spacing);
    int ny = (int)std::ceil(m_BoxY / params.spacing);
    int nz = (int)std::ceil(m_BoxZ / params.spacing);
    // single layer of wall particles on each face (paper/SPlisHSPlasH dam break)
    for (int i = 0; i <= nx; i++)
        for (int k = 0; k <= nz; k++) {
            addB(i * params.spacing, 0.0f, k * params.spacing);
            addB(i * params.spacing, m_BoxY, k * params.spacing);
        }
    for (int j = 0; j <= ny; j++)
        for (int k = 0; k <= nz; k++) {
            addB(0.0f, j * params.spacing, k * params.spacing);
            addB(m_BoxX, j * params.spacing, k * params.spacing);
        }
    for (int i = 0; i <= nx; i++)
        for (int j = 0; j <= ny; j++) {
            addB(i * params.spacing, j * params.spacing, 0.0f);
            addB(i * params.spacing, j * params.spacing, m_BoxZ);
        }
    }

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
    const float viscC = params.viscosity * params.soundSpeed;
    // Surface tension (Becker & Teschner 2007, paper [5]): cohesion forces.
    const bool hasTens = params.surfaceTension > 0.0f;

    // Pass 1: density + d_ii (pure function of x(t)); no reads of m_Rho, so the
    // viscosity pass below can safely use the previous step's density.
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        float rhoN = m_Vol * W(0.0f);
        Math::Vector3 gsum;
        for (int j : m_FluidNb[i]) {
            Math::Vector3 r = m_X[i] - m_X[j];
            rhoN += m_Vol * W(r.length());
            gsum += m_Vol * gradW(r);
        }
        rhoN += m_deltaB[i];
        gsum += m_gradB[i];
        m_Rho[i] = rhoN;
        m_dii[i] = gsum * (-1.0f / (rhoN * rhoN));
    }

    // Pass 2: non-pressure forces -> v_adv. Density is stable here.
    #pragma omp parallel for
    for (int i = 0; i < m_N; i++) {
        // Non-pressure forces: gravity + artificial viscosity + surface tension
        // (paper Sec.5, [23] and [5]). Viscosity uses the density from pass 1
        // (= rho(t), positions not yet updated — matches the paper's rho(t)).
        Math::Vector3 aVisc;
        if (hasVisc) {
            for (int j : m_FluidNb[i]) {
                Math::Vector3 r = m_X[i] - m_X[j];
                Math::Vector3 vrel = m_V[i] - m_V[j];
                const float dot = vrel.dot(r);
                if (dot < 0.0f) {   // particles approach each other
                    const float r2 = r.dot(r);
                    const float mu = m_h * dot / (r2 + 0.01f * m_h * m_h);   // < 0
                    const float rhoAvg = 0.5f * (m_Rho[i] + m_Rho[j]);
                    if (rhoAvg > 1e-4f) {
                        // Pi_ij = -alpha c mu / rho_avg  (>0 when approaching)
                        const float pi = -viscC * mu / rhoAvg;
                        // a_i += sum_j m_j Pi_ij gradW(x_i-x_j); in normalized
                        // form m_j/rho_avg = V_j/rhoN_avg, hence:
                        aVisc -= m_Vol * pi * gradW(r);
                    }
                }
            }
        }
        Math::Vector3 aTens;
        if (hasTens) {
            // cohesion acceleration (uniform masses): a_i = -k * sum_j W_ij (x_i-x_j)
            // W is clamped to W(diameter) for r < diameter (no overlap blow-up).
            for (int j : m_FluidNb[i]) {
                Math::Vector3 r = m_X[i] - m_X[j];
                float rr = r.length();
                float w = W(rr);   // paper Eq. (16): no clamp
                aTens -= params.surfaceTension * w * r;
            }
        }
        m_Vadv[i] = m_V[i] + Math::Vector3(0.0f, params.dt * params.gravity, 0.0f) + (aVisc + aTens) * params.dt;
    }

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

// ---------------------------------------------------------------------------
void FluidSolver::PressureSolve() {
    const float h = params.dt, h2 = h * h, omega = params.omega;
    int l = 0;
    while (true) {
        #pragma omp parallel for
        for (int i = 0; i < m_N; i++) {
            Math::Vector3 s;
            for (int j : m_FluidNb[i])
                s += gradW(m_X[i] - m_X[j]) * (-m_Vol / (m_Rho[j] * m_Rho[j]) * m_P[j]);
            m_S[i] = s;
        }

        double errSum = 0.0;
        #pragma omp parallel for reduction(+:errSum)
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
            m_P[i] = pnew;
            // SPlisHSPlasH convergence: signed algebraic residual, only for p>0 particles
            if (pnew != 0.0f) errSum += (m_aii[i] * pnew + sum) * h2 - b;
        }
        l++;
        m_AvgErr = (float)(errSum / m_N);
        m_LastIter = l;
        if (m_traceNext) m_convTrace.push_back(std::fabs(m_AvgErr));
        if (l >= 2 && std::fabs(m_AvgErr) <= params.eta) break;
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
    // CFL condition: keep the particle displacement per step well below h
    // (helps a lot when the water block slams into the tank bottom).
    float vmax = 0.0f;
    for (int i = 0; i < m_N; i++) vmax = std::max(vmax, m_V[i].length());
    float dt = params.dt;
    if (params.useCFL)
        dt = std::min(params.dt, 0.25f * m_h / std::max(vmax, 0.1f));   // 0.4h -> 0.25h: calms impact/splash steps
    float prevDt = params.dt;
    params.dt = dt;

    InjectFluid();          // open-tank scene: spawn this step's jet particles
    BuildNeighborhood();
    PredictAdvection();
    PressureSolve();
    Integrate();

    params.dt = prevDt;
}
