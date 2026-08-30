#include "GaussianRenderer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <omp.h>

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

// ---------------------------------------------------------------------------
// Poly6 kernel (theory.md Appendix B):
//   W(r,h) = 315/(64 pi h^9) * (h^2 - r^2)^3
//   gradW  = -945/(32 pi h^9) * (h^2 - r^2)^2 * r_vec
// ---------------------------------------------------------------------------
float GaussianRenderer::WPoly6(float r, float h) {
    if (r >= h) return 0.0f;
    const float h2 = h * h;
    const float d = h2 - r * r;
    return (315.0f / (64.0f * kPi * h2 * h2 * h2 * h2 * h)) * d * d * d;
}

Math::Vector3 GaussianRenderer::gradWPoly6(const Math::Vector3& rvec, float h) {
    const float r2 = rvec.dot(rvec);
    if (r2 >= h * h || r2 < 1e-12f) return Math::Vector3();
    const float d = h * h - r2;
    const float s = -945.0f / (32.0f * kPi * h * h * h * h * h * h * h * h * h) * d * d;
    return rvec * s;
}

// ---------------------------------------------------------------------------
// Jacobi eigenvalue decomposition of a symmetric 3x3 matrix (row-major a[9]).
// Returns eigenvalues in descending order; evec columns are the eigenvectors
// (evec[3*c + k] = k-th component of eigenvector c).
// ---------------------------------------------------------------------------
void GaussianRenderer::SymEigen3(const float a[9], float eval[3], float evec[9]) {
    float A[9] = {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]};
    float V[9] = {1,0,0, 0,1,0, 0,0,1};
    const int kMaxIt = 32;
    for (int it = 0; it < kMaxIt; it++) {
        // largest off-diagonal |A[p][q]|
        int p = 0, q = 1;
        float mx = std::fabs(A[1]);
        const int offs[3][2] = {{1,2},{0,2},{0,1}};
        for (int k = 0; k < 3; k++) {
            float v = std::fabs(A[3 * offs[k][0] + offs[k][1]]);
            if (v > mx) { mx = v; p = offs[k][0]; q = offs[k][1]; }
        }
        if (mx < 1e-12f) break;
        const float App = A[3 * p + p], Aqq = A[3 * q + q], Apq = A[3 * p + q];
        const float theta = 0.5f * (Aqq - App) / Apq;
        const float t = (theta >= 0.0f ? 1.0f : -1.0f) /
                        (std::fabs(theta) + std::sqrt(theta * theta + 1.0f));
        const float c = 1.0f / std::sqrt(t * t + 1.0f);
        const float s = t * c;
        // rotate A
        for (int k = 0; k < 3; k++) {
            if (k == p || k == q) continue;
            const float Akp = A[3 * k + p], Akq = A[3 * k + q];
            A[3 * k + p] = c * Akp - s * Akq;
            A[3 * k + q] = s * Akp + c * Akq;
            A[3 * p + k] = A[3 * k + p];
            A[3 * q + k] = A[3 * k + q];
        }
        const float App2 = c * c * App - 2.0f * s * c * Apq + s * s * Aqq;
        const float Aqq2 = s * s * App + 2.0f * s * c * Apq + c * c * Aqq;
        A[3 * p + p] = App2;
        A[3 * q + q] = Aqq2;
        A[3 * p + q] = A[3 * q + p] = 0.0f;
        // accumulate V
        for (int k = 0; k < 3; k++) {
            const float Vkp = V[3 * k + p], Vkq = V[3 * k + q];
            V[3 * k + p] = c * Vkp - s * Vkq;
            V[3 * k + q] = s * Vkp + c * Vkq;
        }
    }
    // sort eigenvalues descending, permute columns of V
    int order[3] = {0, 1, 2};
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (A[3 * order[j] + order[j]] > A[3 * order[i] + order[i]])
                std::swap(order[i], order[j]);
    for (int c = 0; c < 3; c++) {
        eval[c] = A[3 * order[c] + order[c]];
        for (int k = 0; k < 3; k++) evec[3 * c + k] = V[3 * k + order[c]];
    }
}

// ---------------------------------------------------------------------------
void GaussianRenderer::BuildGrid(const std::vector<Math::Vector3>& X, float h) {
    Math::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    for (const auto& p : X) {
        mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
        mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
    }
    m_origin = mn - Math::Vector3(h, h, h);
    const Math::Vector3 ext = (mx - mn) + Math::Vector3(2.0f * h, 2.0f * h, 2.0f * h);
    m_gw = std::max(1, (int)std::ceil(ext.x / h));
    m_gh = std::max(1, (int)std::ceil(ext.y / h));
    m_gd = std::max(1, (int)std::ceil(ext.z / h));
    const size_t nCells = (size_t)m_gw * m_gh * m_gd;
    m_cellStart.assign(nCells + 1, 0);
    for (size_t i = 0; i < X.size(); i++) {
        int cx = std::clamp((int)std::floor((X[i].x - m_origin.x) / h), 0, m_gw - 1);
        int cy = std::clamp((int)std::floor((X[i].y - m_origin.y) / h), 0, m_gh - 1);
        int cz = std::clamp((int)std::floor((X[i].z - m_origin.z) / h), 0, m_gd - 1);
        m_cellStart[(size_t)cz * m_gw * m_gh + (size_t)cy * m_gw + (size_t)cx]++;
    }
    {   // exclusive prefix sum
        int acc = 0;
        for (size_t c = 0; c < nCells; c++) { int cnt = m_cellStart[c]; m_cellStart[c] = acc; acc += cnt; }
        m_cellStart[nCells] = acc;
    }
    m_cellParticles.assign(X.size(), 0);
    std::vector<int> fill = m_cellStart;
    for (size_t i = 0; i < X.size(); i++) {
        int cx = std::clamp((int)std::floor((X[i].x - m_origin.x) / h), 0, m_gw - 1);
        int cy = std::clamp((int)std::floor((X[i].y - m_origin.y) / h), 0, m_gh - 1);
        int cz = std::clamp((int)std::floor((X[i].z - m_origin.z) / h), 0, m_gd - 1);
        const size_t cid = (size_t)cz * m_gw * m_gh + (size_t)cy * m_gw + (size_t)cx;
        if (fill[cid] < (int)X.size()) m_cellParticles[fill[cid]++] = (int)i;
    }
}

// Iterate neighbours of particle i within the 27-cell neighbourhood.
#define FOR_NB(i, X, body)                                                       \
    do {                                                                         \
        const int cx0 = std::clamp((int)std::floor((X[i].x - m_origin.x) / m_h), 0, m_gw - 1); \
        const int cy0 = std::clamp((int)std::floor((X[i].y - m_origin.y) / m_h), 0, m_gh - 1); \
        const int cz0 = std::clamp((int)std::floor((X[i].z - m_origin.z) / m_h), 0, m_gd - 1); \
        for (int dz = -1; dz <= 1; dz++)                                         \
        for (int dy = -1; dy <= 1; dy++)                                         \
        for (int dx = -1; dx <= 1; dx++) {                                       \
            const int ccx = cx0 + dx, ccy = cy0 + dy, ccz = cz0 + dz;            \
            if (ccx < 0 || ccx >= m_gw || ccy < 0 || ccy >= m_gh || ccz < 0 || ccz >= m_gd) continue; \
            for (int _k = m_cellStart[(size_t)ccz * m_gw * m_gh + (size_t)ccy * m_gw + (size_t)ccx]; \
                 _k < m_cellStart[(size_t)ccz * m_gw * m_gh + (size_t)ccy * m_gw + (size_t)ccx + 1]; _k++) { \
                const int j = m_cellParticles[_k];                               \
                body;                                                            \
            }                                                                    \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
void GaussianRenderer::ComputeColorField(const std::vector<Math::Vector3>& X,
                                         const std::vector<float>& rhoN,
                                         float spacing) {
    const size_t N = X.size();
    m_color.assign(N, 0.0f);
    m_nbCount.assign(N, 0);
    m_maxNb = 0;
    #pragma omp parallel for schedule(dynamic, 64)
    for (size_t i = 0; i < N; i++) {
        float c = WPoly6(0.0f, m_h) * spacing * spacing * spacing / rhoN[i];  // self term V_i W(0)
        int nb = 0;
        FOR_NB(i, X, {
            if (j == (int)i) continue;
            Math::Vector3 r = X[i] - X[j];
            const float rr = r.length();
            if (rr < m_h) {
                c += WPoly6(rr, m_h) * spacing * spacing * spacing / rhoN[j];
                nb++;
            }
        });
        m_color[i] = c;
        m_nbCount[i] = nb;
        if (nb > m_maxNb) m_maxNb = nb;
    }
    m_avgC = 0.0f;
    for (float c : m_color) m_avgC += c;
    if (N) m_avgC /= (float)N;
}

// ---------------------------------------------------------------------------
// Compute the per-surface-particle quantities that only need the neighbors'
// positions/velocities: normal (Sec. 4), strain-rate eigendecomposition
// (Sec. 5) and curvature (Sec. 7.3).
// ---------------------------------------------------------------------------
void GaussianRenderer::ComputeSurfaceAniso(const std::vector<Math::Vector3>& X,
                                           const std::vector<Math::Vector3>& V,
                                           const std::vector<float>& rhoN,
                                           float spacing, float dt) {
    const size_t N = X.size();
    const int ns = (int)m_surface.size();
    const float h = m_h;

    // pass A: normal + velocity gradient / strain rate -> t_max, lambda1
    m_normal.assign(N, Math::Vector3());
    m_tmax.assign(N, Math::Vector3());
    m_lambda1.assign(N, 0.0f);
    #pragma omp parallel for schedule(dynamic, 32)
    for (int si = 0; si < ns; si++) {
        const size_t i = (size_t)m_surface[si];
        Math::Vector3 gradC;
        float grad[9] = {0};   // velocity gradient (row-major)
        FOR_NB(i, X, {
            if (j == (int)i) continue;
            Math::Vector3 r = X[i] - X[j];
            const float rr = r.length();
            if (rr >= h) continue;
            const float volj = spacing * spacing * spacing / rhoN[j];
            const Math::Vector3 gw = gradWPoly6(r, h);
            gradC += gw * volj;
            // gradV += V_j (v_j - v_i) gradW^T
            const Math::Vector3 dv = V[j] - V[i];
            grad[0] += volj * dv.x * gw.x;   grad[1] += volj * dv.x * gw.y;   grad[2] += volj * dv.x * gw.z;
            grad[3] += volj * dv.y * gw.x;   grad[4] += volj * dv.y * gw.y;   grad[5] += volj * dv.y * gw.z;
            grad[6] += volj * dv.z * gw.x;   grad[7] += volj * dv.z * gw.y;   grad[8] += volj * dv.z * gw.z;
        });
        const float gn = gradC.length();
        if (gn > 1e-6f) m_normal[i] = -(gradC * (1.0f / gn));   // Sec. 4.3 outward
        // symmetrize -> strain rate D (Sec. 5.2)
        float D[9];
        for (int r2 = 0; r2 < 3; r2++)
            for (int c2 = 0; c2 < 3; c2++)
                D[3 * r2 + c2] = 0.5f * (grad[3 * r2 + c2] + grad[3 * c2 + r2]);
        float eval[3], evec[9];
        SymEigen3(D, eval, evec);
        m_lambda1[i] = eval[0];   // largest
        m_tmax[i] = Math::Vector3(evec[0], evec[3], evec[6]);
    }

    // pass B: curvature kappa_i = sum_j V_j (n_j - n_i) . gradW_ij (Sec. 7.3)
    m_kappa.assign(N, 0.0f);
    #pragma omp parallel for schedule(dynamic, 32)
    for (int si = 0; si < ns; si++) {
        const size_t i = (size_t)m_surface[si];
        const Math::Vector3 ni = m_normal[i];
        float kappa = 0.0f;
        FOR_NB(i, X, {
            if (j == (int)i) continue;
            Math::Vector3 r = X[i] - X[j];
            const float rr = r.length();
            if (rr >= h) continue;
            const float volj = spacing * spacing * spacing / rhoN[j];
            const Math::Vector3 gw = gradWPoly6(r, h);
            // neighbour normal: 0 for interior particles (no surface info)
            kappa += volj * (m_normal[j] - ni).dot(gw);
        });
        m_kappa[i] = kappa;
    }

    // pass C: smooth the surface normals (equal-weight average of the visible
    // neighbours) — the raw color-field normals carry SPH discretisation noise
    // that shows up as bright/dark stripes on flat surfaces (a cube face reads
    // "ridged" instead of clean). Two iterations for a steadier surface.
    std::vector<Math::Vector3> smooth = m_normal;
    for (int iter = 0; iter < 3; iter++) {
        #pragma omp parallel for schedule(dynamic, 32)
        for (int si = 0; si < ns; si++) {
            const size_t i = (size_t)m_surface[si];
            if (m_normal[i].length() < 0.5f) continue;
            Math::Vector3 acc = smooth[i];
            int cnt = 1;
            FOR_NB(i, X, {
                if (j == (int)i) continue;
                Math::Vector3 r = X[i] - X[j];
                if (r.length() >= h) continue;
                if (smooth[j].length() > 0.5f) { acc += smooth[j]; cnt++; }
            });
            smooth[i] = acc.normalize();
        }
    }
    m_normal = std::move(smooth);
}

// ---------------------------------------------------------------------------
// Build the final splat list: rotation R (Sec. 6), scales S (Sec. 7), covariance
// (Sec. 8), render color (Sec. 10-11) and opacity (Sec. 12).
// Surface particles get the full anisotropic ellipsoid; interior particles
// (Sec. 7.5) fall back to isotropic spheres with a low opacity so the water
// body reads as a volume instead of an empty shell.
// Colours are computed in a first pass and averaged over surface neighbours
// in a second pass — raw per-particle shading (specular/fresnel/foam driven by
// noisy per-particle normals/speeds) would otherwise speckle the surface.
// ---------------------------------------------------------------------------
void GaussianRenderer::BuildSplats(const std::vector<Math::Vector3>& X,
                                   const std::vector<Math::Vector3>& V,
                                   float spacing, float dt,
                                   const Math::Vector3& cameraPos) {
    const Params& p = m_params;
    const float r = spacing;
    const float kappaMax = p.kappaMaxFactor / r;
    const Math::Vector3 L = p.lightDir.normalize();
    const Math::Vector3 dDeep = p.deepColor;
    const Math::Vector3 dFoam(1.00f, 1.00f, 1.00f);
    const Math::Vector3 dShallow = p.shallowColor;
    const Math::Vector3 ks(1.00f, 0.98f, 0.92f);
    const Math::Vector3 rimCol(1.00f, 0.97f, 0.92f);
    const size_t N = X.size();

    // ---- Pass A: surface particle colours (interior gets the dark fill) ----
    std::vector<Math::Vector3> surfCol(N, Math::Vector3());
    // Interior fill uses the same bright water blue, not a dark tint — a dark
    // interior would show through the translucent surface and darken the whole
    // body, whereas the reference MC mesh is opaque and uniformly bright.
    const Math::Vector3 interiorCol = dDeep;
    #pragma omp parallel for schedule(dynamic, 32)
    for (int ii = 0; ii < (int)N; ii++) {
        const size_t i = (size_t)ii;
        const float C = m_color[i];
        const Math::Vector3& ni = m_normal[i];
        const float nlen = ni.length();
        if (!(C < p.surfaceThreshold)) { surfCol[i] = interiorCol; continue; }
        // Simple shading as requested: a flat water-blue base (with a light
        // diffuse ramp so the surface has some form) + Blinn-Phong specular.
        // No fresnel / sky reflection / SH — just clean colour + highlight.
        Math::Vector3 col;
        if (nlen > 0.5f) {
            const Math::Vector3 vv = (cameraPos - X[i]).normalize();
            // Flat colour: same water-blue everywhere (all faces), only the
            // Blinn-Phong highlight varies. This makes the whole body read as
            // clean blue water, no dark back-facing shading.
            Math::Vector3 base = dDeep;
            const Math::Vector3 hvec = (L + vv).normalize();
            const float spec = std::pow(std::max(ni.dot(hvec), 0.0f), p.specGamma) * p.specStrength;
            col = base + ks * spec;
            // no foam overlay: user asked for flat colour + highlight only
        } else {
            col = dDeep;
        }
        surfCol[i] = col;
    }

    // ---- Pass C: build the splats with the (flat) colour ----
    // With a single flat water-blue base there is no per-particle colour noise
    // to smooth away, so skip the neighbour colour-average pass.
    const std::vector<Math::Vector3>& smoothCol = surfCol;
    m_splats.clear();
    m_splats.reserve(N);
    #pragma omp parallel
    {
        std::vector<Splat> local;
        local.reserve(N / omp_get_num_threads() + 16);
        #pragma omp for schedule(dynamic, 32)
        for (int ii = 0; ii < (int)N; ii++) {
            const size_t i = (size_t)ii;
            const float C = m_color[i];
            const bool isSurface = (C < p.surfaceThreshold);
            // isolated particles (splashes, sharp cube corners/edges) have too
            // few neighbours for a meaningful strain-rate direction; force the
            // isotropic fallback so they render as round droplets, not shards
            const bool isIsolated = (m_nbCount[i] < p.isoNbThreshold);
            const Math::Vector3& ni = m_normal[i];
            const float nlen = ni.length();

            float s1, s2, s3f;
            Math::Vector3 r1(1,0,0), r2(0,1,0), r3(0,0,1);

            if (isSurface && !isIsolated) {
                // Backface cull: only discard clearly-backfacing surface (the
                // far side of the water is hidden behind the near side — the
                // transparent splat pass has no depth buffer). The threshold is
                // soft (-0.2, not 0) so edge/corner particles whose normal
                // grazes the view direction are kept — culling them left the
                // cube's edges as dark empty lines against the bright faces.
                if (nlen > 0.5f && ni.dot(cameraPos - X[i]) < -0.2f) continue;

                // --- rotation R (Sec. 6) ---
                r3 = nlen > 0.5f ? ni : Math::Vector3(0.0f, 1.0f, 0.0f);
                // r1 = projection of t_max onto the tangent plane (Gram-Schmidt)
                Math::Vector3 tmax = m_tmax[i];
                r1 = tmax - r3 * tmax.dot(r3);
                if (r1.length() < 1e-4f) {
                    // degenerate: t_max parallel to the normal -> any tangent vector
                    Math::Vector3 any = (std::fabs(r3.y) < 0.9f) ? Math::Vector3(0.0f, 1.0f, 0.0f)
                                                                 : Math::Vector3(1.0f, 0.0f, 0.0f);
                    r1 = any - r3 * any.dot(r3);
                }
                r1 = r1.normalize();
                r2 = r3.cross(r1);

                // --- scales S (Sec. 7) ---
                s1 = r * (1.0f + p.stretchAlpha * std::max(0.0f, m_lambda1[i]) * dt);
                s1 = std::min(s1, p.rMaxFactor * r);
                float eps = p.epsMin + (m_kappa[i] / kappaMax) * (p.epsMax - p.epsMin);
                eps = std::clamp(eps, p.epsMin, p.epsMax);
                const float s3 = std::max(r * eps, p.s3MinFactor * r);
                // volume conservation s1*s2*s3 = r^3, with a cap so a very flat
                // pancake (small eps) cannot blow s2 up to a tank-sized disc
                s2 = r * r * r / (s1 * s3);
                s2 = std::clamp(s2, p.s2MinFactor * r, p.s2MaxFactor * r);

                // Sec. 7.5 isotropic fallback: blend scales to r as C -> 1
                const float tIso = std::clamp((C - p.isoBlendLo) / (p.isoBlendHi - p.isoBlendLo), 0.0f, 1.0f);
                if (tIso > 0.0f) {
                    s1 = s1 + (r - s1) * tIso;
                    s2 = s2 + (r - s2) * tIso;
                    // s3 handled through eps below
                }
                s3f = (r * eps) + (r - r * eps) * tIso;
            } else {
                // --- interior particle: isotropic sphere (Sec. 7.5) ---
                s1 = s2 = s3f = r;
            }

            // overall scale-up for continuous coverage; alpha uses the original
            // volume so the opacity stays calibrated
            const float volOrig = s1 * s2 * s3f;
            const float sc = p.splatScale;
            s1 *= sc; s2 *= sc; s3f *= sc;

            // --- covariance Sigma = sum_k s_k^2 r_k r_k^T (Sec. 8) ---
            const Math::Vector3 axes[3] = {r1, r2, r3};
            const float ss[3] = {s1, s2, s3f};
            float cov[9] = {0};
            for (int k = 0; k < 3; k++) {
                const Math::Vector3& a = axes[k];
                const float w = ss[k] * ss[k];
                cov[0] += w * a.x * a.x;  cov[1] += w * a.x * a.y;  cov[2] += w * a.x * a.z;
                cov[3] += w * a.y * a.x;  cov[4] += w * a.y * a.y;  cov[5] += w * a.y * a.z;
                cov[6] += w * a.z * a.x;  cov[7] += w * a.z * a.y;  cov[8] += w * a.z * a.z;
            }

            // --- colour (smoothed) and opacity ---
            const Math::Vector3& col = smoothCol[i];
            float alpha;
            if (isSurface) {
                // --- opacity (Sec. 12) ---
                alpha = std::min(1.0f, (r * r * r / volOrig) * p.alphaBase);
            } else {
                alpha = p.interiorAlpha;
            }

            Splat sp;
            sp.center = X[i];
            sp.cov[0] = cov[0]; sp.cov[1] = cov[4]; sp.cov[2] = cov[8];
            sp.cov[3] = cov[1]; sp.cov[4] = cov[2]; sp.cov[5] = cov[5];
            sp.color[0] = col.x; sp.color[1] = col.y; sp.color[2] = col.z;
            sp.alpha = alpha;
            local.push_back(sp);
        }
        #pragma omp critical
        {
            m_splats.insert(m_splats.end(), local.begin(), local.end());
        }
    }
}

// ---------------------------------------------------------------------------
void GaussianRenderer::Compute(const std::vector<Math::Vector3>& X,
                               const std::vector<Math::Vector3>& V,
                               const std::vector<float>& rhoN,
                               float spacing, float dt,
                               const Math::Vector3& cameraPos) {
    m_splats.clear();
    if (X.size() < 8) return;
    m_spacing = spacing;
    m_h = 2.0f * spacing;

    // Defensive: rhoN may be uninitialized (all 0) before the first solver
    // Step, which would make V_j = spacing^3 / rhoN_j infinite and break the
    // color field (no surface particles -> nothing rendered, e.g. switching to
    // surface view in the GUI while paused). Fall back to unit density.
    bool rhoBad = rhoN.size() != X.size();
    if (!rhoBad)
        for (float r : rhoN)
            if (!(r > 0.0f) || !(r < 1e6f)) { rhoBad = true; break; }
    std::vector<float> rhoSafe;
    if (rhoBad) {
        rhoSafe.assign(X.size(), 1.0f);
    }
    const std::vector<float>& rho = rhoBad ? rhoSafe : rhoN;

    BuildGrid(X, m_h);
    ComputeColorField(X, rho, spacing);

    // surface particles (Sec. 4.3): color field below the threshold
    m_surface.clear();
    for (size_t i = 0; i < X.size(); i++)
        if (m_color[i] < m_params.surfaceThreshold)
            m_surface.push_back((int)i);

    ComputeSurfaceAniso(X, V, rho, spacing, dt);
    BuildSplats(X, V, spacing, dt, cameraPos);
}
