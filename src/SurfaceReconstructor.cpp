#include "SurfaceReconstructor.hpp"
#include "SurfaceReconstructorCL.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <omp.h>
#include "mc_tables.inc"

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

// ---------------------------------------------------------------------------
float SurfaceReconstructor::W(float r) const {
    float q = r / m_h;
    if (q > 1.0f) return 0.0f;
    const float c = 8.0f / (kPi * m_h * m_h * m_h);
    if (q <= 0.5f)
        return c * (1.0f - 6.0f * q * q + 6.0f * q * q * q);
    const float t = 1.0f - q;
    return c * 2.0f * t * t * t;
}

Math::Vector3 SurfaceReconstructor::gradW(const Math::Vector3& r) const {
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
SurfaceReconstructor::SurfaceReconstructor() = default;
SurfaceReconstructor::~SurfaceReconstructor() = default;

// ---------------------------------------------------------------------------
bool SurfaceReconstructor::Reconstruct(const std::vector<Math::Vector3>& particles, float spacing) {
    m_verts.clear();
    m_indices.clear();
    if (particles.size() < 8) return false;

    m_spacing = spacing;
    m_h = 2.0f * spacing;
    m_vol = spacing * spacing * spacing;
    m_k = 0.5f * spacing;

    // GPU path (OpenCL): scalar-field construction on the GPU, Marching Cubes on
    // the CPU — exactly the split the paper uses (Sec. 4.2 triangulates serially).
    // Falls back to the CPU pipeline on any OpenCL failure.
    if (!m_gpu) {
        m_gpu = std::make_unique<SurfaceReconstructorCL>();
        if (!m_gpu->Available()) {
            fprintf(stderr, "[surface] OpenCL unavailable (%s) — using CPU path\n", m_gpu->LastError());
            m_gpu.reset();
        }
    }
    if (m_gpu) {
        if (m_gpu->ComputeField(particles, spacing, m_bandScale, m_gridScale, m_useEvmax,
                                m_surfaceThreshold, m_origin, m_fstep, m_fw, m_fh, m_fd,
                                m_field, m_surfVert, m_surfNormal)) {
            MarchingCubes(particles);
            return !m_indices.empty();
        }
        fprintf(stderr, "[surface] OpenCL compute failed (%s) — using CPU path\n", m_gpu->LastError());
        m_gpu.reset();
    }

    BuildGrid(particles, spacing);
    ComputeColorAndNormal(particles);
    BuildImplicitField(particles);
    MarchingCubes(particles);
    return !m_indices.empty();
}

// ---------------------------------------------------------------------------
void SurfaceReconstructor::BuildGrid(const std::vector<Math::Vector3>& particles, float spacing) {
    Math::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    for (const auto& p : particles) {
        mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
        mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
    }
    const float m = m_h;
    m_origin = mn - Math::Vector3(m, m, m);
    const Math::Vector3 ext = (mx - mn) + Math::Vector3(2.0f * m, 2.0f * m, 2.0f * m);
    m_gw = std::max(1, (int)std::ceil(ext.x / m_h));
    m_gh = std::max(1, (int)std::ceil(ext.y / m_h));
    m_gd = std::max(1, (int)std::ceil(ext.z / m_h));
    // flat SoA buckets (paper [30]): count per cell, prefix sum, scatter
    const size_t nCells = (size_t)m_gw * m_gh * m_gd;
    m_cellStart.assign(nCells + 1, 0);
    for (size_t i = 0; i < particles.size(); i++) {
        int cx = std::clamp((int)std::floor((particles[i].x - m_origin.x) / m_h), 0, m_gw - 1);
        int cy = std::clamp((int)std::floor((particles[i].y - m_origin.y) / m_h), 0, m_gh - 1);
        int cz = std::clamp((int)std::floor((particles[i].z - m_origin.z) / m_h), 0, m_gd - 1);
        const size_t cid = (size_t)cz * m_gw * m_gh + (size_t)cy * m_gw + (size_t)cx;
        m_cellStart[cid]++;
    }
    {   // exclusive prefix sum: start[c] = sum of counts [0..c-1]
        int acc = 0;
        for (size_t c = 0; c < nCells; c++) { int cnt = m_cellStart[c]; m_cellStart[c] = acc; acc += cnt; }
        m_cellStart[nCells] = acc;
    }
    m_cellParticles.assign(particles.size(), 0);
    std::vector<int> fill = m_cellStart;
    for (size_t i = 0; i < particles.size(); i++) {
        int cx = std::clamp((int)std::floor((particles[i].x - m_origin.x) / m_h), 0, m_gw - 1);
        int cy = std::clamp((int)std::floor((particles[i].y - m_origin.y) / m_h), 0, m_gh - 1);
        int cz = std::clamp((int)std::floor((particles[i].z - m_origin.z) / m_h), 0, m_gd - 1);
        const size_t cid = (size_t)cz * m_gw * m_gh + (size_t)cy * m_gw + (size_t)cx;
        if (fill[cid] >= (int)particles.size()) {
            fprintf(stderr, "[bg] overflow i=%zu pos=(%.4f,%.4f,%.4f) cid=%zu fill=%d start=%d next=%d gw=%d gh=%d gd=%d\n",
                    i, particles[i].x, particles[i].y, particles[i].z, cid, fill[cid],
                    m_cellStart[cid], m_cellStart[cid + 1], m_gw, m_gh, m_gd);
            fill[cid] = (int)particles.size() - 1;   // clamp: avoid crash, keep going
        }
        m_cellParticles[fill[cid]++] = (int)i;
    }
}

// iterate the particles of grid cell `idx` (flat SoA buckets); expands to a
// for-header, the caller writes the loop body after it.
#define FOR_CELL(idx)                                                                 \
    for (int _k = m_cellStart[(idx)]; _k < m_cellStart[(idx) + 1]; _k++) {            \
        int j = m_cellParticles[_k];                                                  \

// iterate the surface particles of grid cell `idx`
#define FOR_SURFCELL(idx)                                                              \
    for (int _k = m_surfStart[(idx)]; _k < m_surfStart[(idx) + 1]; _k++) {            \
        int t = m_surfParticles[_k];                                                  \


// ---------------------------------------------------------------------------
void SurfaceReconstructor::ComputeColorAndNormal(const std::vector<Math::Vector3>& particles) {
    const size_t N = particles.size();
    m_color.assign(N, 0.0f);
    m_normal.assign(N, Math::Vector3());
    m_nbCount.assign(N, 0);
    #pragma omp parallel for schedule(dynamic, 64)
    for (size_t i = 0; i < N; i++) {
        float c = m_vol * W(0.0f);   // self term
        Math::Vector3 n;
        int nb = 0;
        int cx = std::clamp((int)std::floor((particles[i].x - m_origin.x) / m_h), 0, m_gw - 1);
        int cy = std::clamp((int)std::floor((particles[i].y - m_origin.y) / m_h), 0, m_gh - 1);
        int cz = std::clamp((int)std::floor((particles[i].z - m_origin.z) / m_h), 0, m_gd - 1);
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int ccx = cx + dx, ccy = cy + dy, ccz = cz + dz;
                    if (ccx < 0 || ccx >= m_gw || ccy < 0 || ccy >= m_gh || ccz < 0 || ccz >= m_gd) continue;
                    FOR_CELL((size_t)ccz * m_gw * m_gh + (size_t)ccy * m_gw + (size_t)ccx)
                        if ((int)i == j) continue;
                        Math::Vector3 r = particles[i] - particles[j];
                        float rr = r.length();
                        if (rr < m_h) {
                            c += m_vol * W(rr);
                            n += m_vol * gradW(r);
                            nb++;
                        }
                    }
                }
        m_color[i] = c;
        m_normal[i] = n;
        m_nbCount[i] = nb;
    }

    // surface particles: color-field criterion (main body) OR fewer than 25
    // neighbours (isolated splashes, which the color field misses) — Akinci
    // et al. Sec. 4, Algorithm 1.
    m_surface.clear();
    for (size_t i = 0; i < N; i++) {
        if (m_color[i] < m_surfaceThreshold || m_nbCount[i] < 25)
            m_surface.push_back((int)i);
    }
}

// ---------------------------------------------------------------------------
// BuildImplicitField — Akinci et al. 2012 Eqs. (1)-(5):
//   phi(x) = |x - xbar| - r,   xbar = sum_j x_j k(|x-x_j|/R) / sum_j k(...)
//   k(s) = max(0, (1-s^2)^3),  R = 4r,  r = particle spacing.
// Only grid corners inside the AABB (2r total per axis) of a surface particle
// get the true distance value; the rest are inside/outside sentinels.
// ---------------------------------------------------------------------------
void SurfaceReconstructor::BuildImplicitField(const std::vector<Math::Vector3>& particles) {
    const float r = m_spacing;          // equilibrium distance (r-bar = r, paper)
    const float R = 4.0f * r;           // influence radius (Eq. 4); ZB05: >= 4r for smoothness
    const float R2 = R * R;
    m_fstep = m_gridScale * m_spacing;  // MC grid cell size (paper Fig.1: r)
    const Math::Vector3 ext = Math::Vector3(m_gw * m_h, m_gh * m_h, m_gd * m_h);
    m_fw = (int)std::ceil(ext.x / m_fstep) + 1;
    m_fh = (int)std::ceil(ext.y / m_fstep) + 1;
    m_fd = (int)std::ceil(ext.z / m_fstep) + 1;
    m_field.assign((size_t)m_fw * m_fh * m_fd, 0.0f);
    m_surfVert.assign((size_t)m_fw * m_fh * m_fd, 0);
    m_surfNormal.assign((size_t)m_fw * m_fh * m_fd, Math::Vector3(0.0f, 0.0f, 0.0f));

    auto kappa = [&](float d2) {
        float s2 = d2 / R2;
        if (s2 >= 1.0f) return 0.0f;
        float t = 1.0f - s2;
        return t * t * t;
    };

    // grid of surface particles (cell size = 2r, the AABB half-extent)
    m_R = 2.0f * r;
    m_sw = std::max(1, (int)std::ceil((m_gw * m_h) / m_R));
    m_sh = std::max(1, (int)std::ceil((m_gh * m_h) / m_R));
    m_sd = std::max(1, (int)std::ceil((m_gd * m_h) / m_R));
    // grid of surface particles (cell size = 2r, the AABB half-extent) — flat SoA
    const size_t nSurfCells = (size_t)m_sw * m_sh * m_sd;
    m_surfStart.assign(nSurfCells + 1, 0);
    for (size_t tt = 0; tt < m_surface.size(); tt++) {
        int si = m_surface[tt];
        int cx = std::clamp((int)std::floor((particles[si].x - m_origin.x) / m_R), 0, m_sw - 1);
        int cy = std::clamp((int)std::floor((particles[si].y - m_origin.y) / m_R), 0, m_sh - 1);
        int cz = std::clamp((int)std::floor((particles[si].z - m_origin.z) / m_R), 0, m_sd - 1);
        m_surfStart[(size_t)cz * m_sw * m_sh + (size_t)cy * m_sw + (size_t)cx]++;
    }
    {   // exclusive prefix sum
        int acc = 0;
        for (size_t c = 0; c < nSurfCells; c++) { int cnt = m_surfStart[c]; m_surfStart[c] = acc; acc += cnt; }
        m_surfStart[nSurfCells] = acc;
    }
    m_surfParticles.assign(m_surface.size(), 0);
    std::vector<int> sfill = m_surfStart;
    for (size_t tt = 0; tt < m_surface.size(); tt++) {
        int si = m_surface[tt];
        int cx = std::clamp((int)std::floor((particles[si].x - m_origin.x) / m_R), 0, m_sw - 1);
        int cy = std::clamp((int)std::floor((particles[si].y - m_origin.y) / m_R), 0, m_sh - 1);
        int cz = std::clamp((int)std::floor((particles[si].z - m_origin.z) / m_R), 0, m_sd - 1);
        const size_t cid = (size_t)cz * m_sw * m_sh + (size_t)cy * m_sw + (size_t)cx;
        m_surfParticles[sfill[cid]++] = (int)tt;
    }

    #pragma omp parallel for schedule(static)
    for (int cz = 0; cz < m_fd; cz++)
        for (int cy = 0; cy < m_fh; cy++)
            for (int cx = 0; cx < m_fw; cx++) {
                Math::Vector3 p = m_origin + Math::Vector3(cx * m_fstep, cy * m_fstep, cz * m_fstep);

                // surface-vertex test: inside the AABB (half-extent r per axis)
                // of any surface particle?
                bool nearSurf = false;
                int scx = std::clamp((int)std::floor((p.x - m_origin.x) / m_R), 0, m_sw - 1);
                int scy = std::clamp((int)std::floor((p.y - m_origin.y) / m_R), 0, m_sh - 1);
                int scz = std::clamp((int)std::floor((p.z - m_origin.z) / m_R), 0, m_sd - 1);
                for (int dz = -1; dz <= 1 && !nearSurf; dz++)
                    for (int dy = -1; dy <= 1 && !nearSurf; dy++)
                        for (int dx = -1; dx <= 1 && !nearSurf; dx++) {
                            int ccx = scx + dx, ccy = scy + dy, ccz = scz + dz;
                            if (ccx < 0 || ccx >= m_sw || ccy < 0 || ccy >= m_sh || ccz < 0 || ccz >= m_sd) continue;
                            FOR_SURFCELL((size_t)ccz * m_sw * m_sh + (size_t)ccy * m_sw + (size_t)ccx)
                                const Math::Vector3& xi = particles[m_surface[t]];
                                if (std::fabs(p.x - xi.x) <= m_bandScale * r && std::fabs(p.y - xi.y) <= m_bandScale * r && std::fabs(p.z - xi.z) <= m_bandScale * r) {
                                    nearSurf = true;
                                    break;
                                }
                            }
                        }

                const size_t fidx = (size_t)cz * m_fw * m_fh + (size_t)cy * m_fw + (size_t)cx;
                m_surfVert[fidx] = nearSurf ? 1 : 0;

                if (nearSurf) {
                    // scalar value (paper Eq. 1): phi = |x - xbar| - r * f
                    // f from the largest eigenvalue EVmax of d(xbar)/dx (Eq. 2-3).
                    Math::Vector3 xbar;
                    Math::Vector3 row[3];       // J = sum (dw/dx) (x_j)^T
                    Math::Vector3 sumdw;
                    float wsum = 0.0f;
                    int gx = std::clamp((int)std::floor((p.x - m_origin.x) / m_h), 0, m_gw - 1);
                    int gy = std::clamp((int)std::floor((p.y - m_origin.y) / m_h), 0, m_gh - 1);
                    int gz = std::clamp((int)std::floor((p.z - m_origin.z) / m_h), 0, m_gd - 1);
                    for (int dz = -2; dz <= 2; dz++)
                        for (int dy = -2; dy <= 2; dy++)
                            for (int dx = -2; dx <= 2; dx++) {
                                int ccx = gx + dx, ccy = gy + dy, ccz = gz + dz;
                                if (ccx < 0 || ccx >= m_gw || ccy < 0 || ccy >= m_gh || ccz < 0 || ccz >= m_gd) continue;
                                FOR_CELL((size_t)ccz * m_gw * m_gh + (size_t)ccy * m_gw + (size_t)ccx)
                                    Math::Vector3 dj = particles[j] - p;
                                    float d2 = dj.dot(dj);
                                    float w = kappa(d2);
                                    if (w > 0.0f) {
                                        xbar = xbar + particles[j] * w;
                                        wsum += w;
                                        if (m_useEvmax) {
                                            // dw = dk/dx  (gradient of the kernel w.r.t. query point x)
                                            // k(s) = (1-s^2)^3, s = |x-x_j|/R
                                            // dk/dx = dk/ds * ds/dx
                                            //       = (-6 s (1-s^2)^2) * (x-x_j)/(R|x-x_j|)
                                            //       = +6 s (1-s^2)^2 * (x_j-x)/(R|x-x_j|)
                                            float d = std::sqrt(d2);
                                            if (d > 1e-9f) {
                                                float s = d / R;
                                                float kp = 6.0f * s * (1.0f - s * s) * (1.0f - s * s);
                                                Math::Vector3 dw = dj * (kp / (R * d));
                                                sumdw = sumdw + dw;
                                                for (int a = 0; a < 3; a++) {
                                                    row[a][0] += dw[a] * particles[j].x;
                                                    row[a][1] += dw[a] * particles[j].y;
                                                    row[a][2] += dw[a] * particles[j].z;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                    if (wsum > 1e-9f) {
                        xbar = xbar * (1.0f / wsum);
                        float f = 1.0f;
                        if (m_useEvmax) {
                            // J = (sum dw x^T - (sum w x)(sum dw)^T) / W^2
                            const float W2 = wsum * wsum;
                            for (int a = 0; a < 3; a++) {
                                const float xs = xbar[a] * wsum;
                                row[a][0] = (row[a][0] * wsum - xs * sumdw.x) / W2;
                                row[a][1] = (row[a][1] * wsum - xs * sumdw.y) / W2;
                                row[a][2] = (row[a][2] * wsum - xs * sumdw.z) / W2;
                            }
                            // symmetric part A = (J + J^T)/2, power iteration for EVmax
                            Math::Vector3 A[3];
                            for (int a = 0; a < 3; a++)
                                for (int b = 0; b < 3; b++)
                                    A[a][b] = 0.5f * (row[a][b] + row[b][a]);
                            Math::Vector3 v(1.0f, 0.0f, 0.0f);
                            for (int it = 0; it < 2; it++) {
                                Math::Vector3 Av(A[0].dot(v), A[1].dot(v), A[2].dot(v));
                                float nrm = Av.length();
                                if (nrm > 1e-12f) v = Av * (1.0f / nrm);
                            }
                            const float evmax = v.dot(Math::Vector3(A[0].dot(v), A[1].dot(v), A[2].dot(v)));
                            // Eq. 2-3: f = 1 if EVmax < tlow, else gamma polynomial
                            const float tLow = 0.4f, tHigh = 3.5f;
                            if (evmax >= tLow) {
                                float g = (tHigh - evmax) / (tHigh - tLow);
                                g = std::clamp(g, 0.0f, 1.0f);
                                f = g * g * g - 3.0f * g * g + 3.0f * g;
                            }
                        }
                        Math::Vector3 d = p - xbar;
                        m_field[fidx] = d.length() - r * f;
                        // outward normal at this corner: grad(phi) ~ (p-xbar)/|p-xbar|
                        float dl = d.length();
                        m_surfNormal[fidx] = (dl > 1e-6f) ? d * (1.0f / dl) : Math::Vector3(0.0f, 1.0f, 0.0f);
                    } else {
                        m_field[fidx] = r; // isolated
                    }
                } else {
                    // NOT near the surface: value is never used — Marching only
                    // triangulates cells whose 8 corners are ALL surface
                    // vertices (8/8 cull), so a cell containing a non-surface
                    // corner is skipped and its sentinel value is irrelevant.
                    // (The previous version searched 27 cells for the nearest
                    // particle — pure waste for the bulk of the grid corners
                    // at r/2 resolution.)
                    m_field[fidx] = r;
                }
            }
}

// ---------------------------------------------------------------------------
float SurfaceReconstructor::FieldAt(const Math::Vector3& p) const {
    float gx = (p.x - m_origin.x) / m_fstep;
    float gy = (p.y - m_origin.y) / m_fstep;
    float gz = (p.z - m_origin.z) / m_fstep;
    int x0 = std::clamp((int)std::floor(gx), 0, m_fw - 2);
    int y0 = std::clamp((int)std::floor(gy), 0, m_fh - 2);
    int z0 = std::clamp((int)std::floor(gz), 0, m_fd - 2);
    float tx = std::min(1.0f, std::max(0.0f, gx - x0));
    float ty = std::min(1.0f, std::max(0.0f, gy - y0));
    float tz = std::min(1.0f, std::max(0.0f, gz - z0));
    auto V = [&](int ix, int iy, int iz) {
        return m_field[(size_t)iz * m_fw * m_fh + (size_t)iy * m_fw + (size_t)ix];
    };
    float c000 = V(x0, y0, z0), c100 = V(x0 + 1, y0, z0);
    float c010 = V(x0, y0 + 1, z0), c110 = V(x0 + 1, y0 + 1, z0);
    float c001 = V(x0, y0, z0 + 1), c101 = V(x0 + 1, y0, z0 + 1);
    float c011 = V(x0, y0 + 1, z0 + 1), c111 = V(x0 + 1, y0 + 1, z0 + 1);
    float x00 = c000 + tx * (c100 - c000);
    float x10 = c010 + tx * (c110 - c010);
    float x01 = c001 + tx * (c101 - c001);
    float x11 = c011 + tx * (c111 - c011);
    float y0v = x00 + ty * (x10 - x00);
    float y1v = x01 + ty * (x11 - x01);
    return y0v + tz * (y1v - y0v);
}

Math::Vector3 SurfaceReconstructor::FieldGrad(const Math::Vector3& p) const {
    const float e = 0.5f * m_fstep;
    Math::Vector3 g;
    g.x = (FieldAt(p + Math::Vector3(e, 0, 0)) - FieldAt(p - Math::Vector3(e, 0, 0))) / (2.0f * e);
    g.y = (FieldAt(p + Math::Vector3(0, e, 0)) - FieldAt(p - Math::Vector3(0, e, 0))) / (2.0f * e);
    g.z = (FieldAt(p + Math::Vector3(0, 0, e)) - FieldAt(p - Math::Vector3(0, 0, e))) / (2.0f * e);
    return g;
}

// ---------------------------------------------------------------------------
// Marching Tetrahedra: 6 tetrahedra per cube (body diagonal 0-6), 0/1/2
// triangles per tetra, linear edge interpolation. Vertex normals = weighted
// average of the nearby surface-particle OUTWARD normals (smooth, matches
// Akinci et al.). Parallelized per cell; only cubes whose 8 corner signs
// differ are visited.
// ---------------------------------------------------------------------------
void SurfaceReconstructor::MarchingCubes(const std::vector<Math::Vector3>& particles) {
    if (m_field.empty()) return;
    m_verts.clear();
    m_indices.clear();

    // ---- Marching Cubes (paper Sec. 4.2 + Alg. 1; lookup tables: Cory Bloyd /
    // Paul Bourke, public domain). Corner order 0(0,0,0) 1(1,0,0) 2(1,1,0)
    // 3(0,1,0) 4(0,0,1) 5(1,0,1) 6(1,1,1) 7(0,1,1); edges per the tables.
    static const int cid[8][3] = {
        {0,0,0},{1,0,0},{1,1,0},{0,1,0},
        {0,0,1},{1,0,1},{1,1,1},{0,1,1}
    };
    static const int mcEdges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
    };

    const int ncells = (m_fd - 1) * (m_fh - 1) * (m_fw - 1);
    const size_t nVerts = (size_t)m_fw * m_fh * m_fd;

    auto cornerIdx = [&](int fw, int fh, int cx, int cy, int cz, int ci) {
        return (size_t)(cz + cid[ci][2]) * fw * fh +
               (size_t)(cy + cid[ci][1]) * fw +
               (cx + cid[ci][0]);
    };
    auto cornerVal = [&](const float* fld, int fw, int fh, int cx, int cy, int cz, int ci) {
        return fld[cornerIdx(fw, fh, cx, cy, cz, ci)];
    };
    auto cornerPos = [&](int cx, int cy, int cz, int ci) {
        return m_origin + Math::Vector3((cx + cid[ci][0]) * m_fstep,
                                        (cy + cid[ci][1]) * m_fstep,
                                        (cz + cid[ci][2]) * m_fstep);
    };
    auto cornerNrm = [&](int fw, int fh, int cx, int cy, int cz, int ci) {
        return m_surfNormal[cornerIdx(fw, fh, cx, cy, cz, ci)];
    };
    // GPU path: m_surfNormal is empty (normals were not read back). Rebuild the
    // corner normals numerically from the field gradient, lazily cached, only
    // for the corners that survive the 8-of-8 cull.
    std::vector<Math::Vector3> lazyNrm;
    std::vector<uint8_t> lazyDone;
    auto cornerNrmLazy = [&](int fw, int fh, int cx, int cy, int cz, int ci) -> Math::Vector3 {
        size_t idx = cornerIdx(fw, fh, cx, cy, cz, ci);
        if (!lazyDone[idx]) {
            Math::Vector3 n = FieldGrad(cornerPos(cx, cy, cz, ci));
            float nl = n.length();
            lazyNrm[idx] = (nl > 1e-9f) ? n * (1.0f / nl) : Math::Vector3(0.0f, 1.0f, 0.0f);
            lazyDone[idx] = 1;
        }
        return lazyNrm[idx];
    };
    auto getCornerNrm = [&](int fw, int fh, int cx, int cy, int cz, int ci) -> Math::Vector3 {
        if (!m_surfNormal.empty()) return cornerNrm(fw, fh, cx, cy, cz, ci);
        return cornerNrmLazy(fw, fh, cx, cy, cz, ci);
    };

    // Shared edge-intersection points (paper Sec. 4.2): every grid vertex keeps
    // three edge ids for the edges leaving it (+x, +y, +z). Triangulation is
    // serial, exactly as in the paper ("The triangulation process is not
    // parallelized, since it takes a rather small amount of time").
    std::vector<int> edgePoint(nVerts * 3, -1);
    std::vector<float> pointData; // pos(3)+normal(3) per shared intersection point
    lazyNrm.assign(nVerts, Math::Vector3(0.0f, 0.0f, 0.0f));
    lazyDone.assign(nVerts, 0);
    m_indices.clear();
    m_indices.reserve((size_t)ncells * 3);

    const float* fld = m_field.data();
    const int fw = m_fw, fh = m_fh;

    // Narrow band (paper Alg. 1): pre-mark cells that contain at least one
    // surface vertex, so the march skips the (vast) empty volume cells. Each
    // surface vertex is a corner of up to 8 cells.
    std::vector<uint8_t> cellMark((size_t)ncells, 0);
    for (size_t idx = 0; idx < m_surfVert.size(); idx++) {
        if (!m_surfVert[idx]) continue;
        const int vcz = (int)(idx / ((size_t)fw * fh));
        const int vcy = (int)((idx / (size_t)fw) % fh);
        const int vcx = (int)(idx % fw);
        for (int dz = -1; dz <= 0; dz++)
            for (int dy = -1; dy <= 0; dy++)
                for (int dx = -1; dx <= 0; dx++) {
                    const int ccx = vcx + dx, ccy = vcy + dy, ccz = vcz + dz;
                    if (ccx < 0 || ccy < 0 || ccz < 0 || ccx >= fw - 1 || ccy >= fh - 1 || ccz >= m_fd - 1) continue;
                    cellMark[(size_t)ccz * (fh - 1) * (fw - 1) + (size_t)ccy * (fw - 1) + (size_t)ccx] = 1;
                }
    }

    for (int cz = 0; cz < m_fd - 1; cz++)
        for (int cy = 0; cy < m_fh - 1; cy++)
            for (int cx = 0; cx < m_fw - 1; cx++) {
                if (!cellMark[(size_t)cz * (fh - 1) * (fw - 1) + (size_t)cy * (fw - 1) + (size_t)cx]) continue;
                // paper Alg. 1 (last step): triangulate only cells whose 8
                // corners are ALL surface vertices.
                {
                    const size_t base = cornerIdx(fw, fh, cx, cy, cz, 0);
                    int nSurf = m_surfVert[base] + m_surfVert[base + 1] +
                                m_surfVert[base + fw] + m_surfVert[base + fw + 1] +
                                m_surfVert[base + (size_t)fw * fh] + m_surfVert[base + (size_t)fw * fh + 1] +
                                m_surfVert[base + (size_t)fw * fh + fw] + m_surfVert[base + (size_t)fw * fh + fw + 1];
                    if (nSurf < 8) continue;
                }

                float vc[8];
                for (int c = 0; c < 8; c++) vc[c] = cornerVal(fld, fw, fh, cx, cy, cz, c);

                int cubeIndex = 0;
                for (int c = 0; c < 8; c++) if (vc[c] > 0.0f) cubeIndex |= (1 << c);
                int flags = mcEdgeFlags[cubeIndex];
                if (flags == 0) continue;

                int edgeIdx[12];
                for (int e = 0; e < 12; e++) {
                    if (!(flags & (1 << e))) { edgeIdx[e] = -1; continue; }
                    int a = mcEdges[e][0], b = mcEdges[e][1];
                    size_t ga = cornerIdx(fw, fh, cx, cy, cz, a);
                    size_t gb = cornerIdx(fw, fh, cx, cy, cz, b);
                    // orient the edge to its +x/+y/+z form; start = lower index
                    size_t vStart; int dir;
                    if (gb == ga + 1)                 { vStart = ga; dir = 0; }
                    else if (gb == ga + fw)           { vStart = ga; dir = 1; }
                    else if (gb == ga + (size_t)fw * fh) { vStart = ga; dir = 2; }
                    else if (ga == gb + 1)            { vStart = gb; dir = 0; }
                    else if (ga == gb + fw)           { vStart = gb; dir = 1; }
                    else                              { vStart = gb; dir = 2; }
                    size_t key = vStart * 3 + dir;
                    int& pid = edgePoint[key];
                    if (pid < 0) {
                        // interpolate position and normal along the edge (paper
                        // Sec. 4.2: normals interpolated from grid-vertex normals)
                        float t = vc[a] / (vc[a] - vc[b]);
                        Math::Vector3 pa = cornerPos(cx, cy, cz, a);
                        Math::Vector3 pb = cornerPos(cx, cy, cz, b);
                        Math::Vector3 pos = pa + (pb - pa) * t;
                        Math::Vector3 na = getCornerNrm(fw, fh, cx, cy, cz, a);
                        Math::Vector3 nb = getCornerNrm(fw, fh, cx, cy, cz, b);
                        if (na.length() < 1e-6f) na = nb;
                        if (nb.length() < 1e-6f) nb = na;
                        Math::Vector3 nn = na + (nb - na) * t;
                        float nl = nn.length();
                        if (nl < 1e-6f) nn = Math::Vector3(0.0f, 1.0f, 0.0f);
                        else nn = nn * (1.0f / nl);
                        pid = (int)(pointData.size() / 6);
                        pointData.push_back(pos.x); pointData.push_back(pos.y); pointData.push_back(pos.z);
                        pointData.push_back(nn.x); pointData.push_back(nn.y); pointData.push_back(nn.z);
                    }
                    edgeIdx[e] = pid;
                }

                const int* tri = mcTriTable[cubeIndex];
                for (int k = 0; tri[k] != -1; k += 3) {
                    m_indices.push_back((unsigned)edgeIdx[tri[k]]);
                    m_indices.push_back((unsigned)edgeIdx[tri[k + 1]]);
                    m_indices.push_back((unsigned)edgeIdx[tri[k + 2]]);
                }
            }

    // indexed mesh: vertices = shared intersection points, indices reference them
    m_verts.swap(pointData);
}

