// SurfaceReconstructor — Akinci et al. 2012, "Parallel surface reconstruction
// for particle-based fluids" (CGF 32(1)), cited as [32] in the IISPH paper.
//
// Pipeline (paper Eqs. 1-5, Algorithm 1):
//   1. color field        c_i   = sum_j V_j W(x_i - x_j)        (interior ~ 1)
//   2. surface particles: c_i < c_threshold  OR  fewer than 25 neighbours
//   3. surface grid vertices: grid corners inside the AABB (+-1.5r) of any
//      surface particle get the scalar value phi(x) = |x - xbar| - r, where
//      xbar = sum_j x_j k(|x-x_j|/R) / sum_j k(...) is the weighted centroid,
//      k(s) = max(0,(1-s^2)^3), R = 4r influence radius, r = particle spacing.
//      Other corners are classified inside/outside via the color field.
//   4. Marching Tetrahedra extracts phi = 0.
//
// The distance-to-weighted-centroid field is smooth and cannot fragment, which
// is exactly why Akinci et al. use it instead of a per-particle plane min().

#pragma once
#include "Math/Vector3.hpp"
#include <vector>
#include <cstdint>
#include <memory>

class SurfaceReconstructorCL;

class SurfaceReconstructor {
public:
    SurfaceReconstructor();   // defined in .cpp so the OpenCL backend (unique_ptr) sees a complete type
    ~SurfaceReconstructor();
    SurfaceReconstructor(const SurfaceReconstructor&) = delete;
    SurfaceReconstructor& operator=(const SurfaceReconstructor&) = delete;

    // Builds the surface mesh from fluid particle positions.
    // spacing: nominal particle spacing [m] (grid resolution and kernel).
    // Returns false if no surface mesh was produced (too few particles etc.).
    bool Reconstruct(const std::vector<Math::Vector3>& particles, float spacing);

    // Vertices() = interleaved pos(3)+normal(3) per vertex, world space.
    const std::vector<float>&    Vertices() const { return m_verts; }
    const std::vector<unsigned>& Indices()  const { return m_indices; }
    int VertexCount() const { return (int)(m_verts.size() / 6); }
    int IndexCount()  const { return (int)m_indices.size(); }
    void SetSurfaceThreshold(float t) { m_surfaceThreshold = t; }
    // field grid resolution as a multiple of spacing (1.0 = full quality, >1 faster)
    void SetGridScale(float s) { m_gridScale = s; }
    void SetBandScale(float b) { m_bandScale = b; }   // surface-vertex band half-width in units of r (default 2.0)
    void SetUseEvmax(bool b) { m_useEvmax = b; }      // paper Eq. 2-3 curvature correction (default off)

    // per-particle scalar fields from the CPU color-field pass (filled by
    // Reconstruct's CPU path; empty on the GPU path). Used by the Gaussian
    // splatting renderer: c = color field (surface measure), n = grad c dir.
    const std::vector<float>&         ParticleColorField() const { return m_color; }
    const std::vector<Math::Vector3>& ParticleNormals()     const { return m_normal; }

private:
    // internal uniform grid helpers
    void BuildGrid(const std::vector<Math::Vector3>& particles, float spacing);
    void ComputeColorAndNormal(const std::vector<Math::Vector3>& particles);
    void BuildImplicitField(const std::vector<Math::Vector3>& particles);
    void MarchingCubes(const std::vector<Math::Vector3>& particles);

    float W(float r) const;                     // cubic spline, support h = 2*spacing
    Math::Vector3 gradW(const Math::Vector3& r) const;
    float FieldAt(const Math::Vector3& p) const;   // trilinear field sample
    Math::Vector3 FieldGrad(const Math::Vector3& p) const;  // numerical gradient

    float m_spacing = 0.04f;
    float m_h = 0.08f;
    float m_vol = 0.0f;                          // V = spacing^3
    float m_k = 0.0f;                            // implicit-plane offset = spacing/2
    float m_surfaceThreshold = 0.8f;             // color-field threshold for surface particles
    float m_gridScale = 1.0f;
    float m_bandScale = 2.0f;   // surface-vertex band half-width in units of r.
    // Paper Sec. 4.1 uses ±1r (2r total span). At ±1.5r the isosurface — which
    // [SSP07] places ~0.5r BELOW the top particles for a settled body of water —
    // still falls outside the band for a measurable share of MC cells, so their
    // 8/8 surface-vertex cull drops the triangles (visible cracks on calm
    // water). ±2r keeps the whole isosurface band inside while the 8/8 cull
    // still rejects the (false) bottom-layer surface (no double layer).
    bool m_useEvmax = true;                      // paper Eq. 2-3 EVmax factor: shrinks the implicit
                                                 // radius where the neighborhood is one-sided (water
                                                 // edges, waves) — the paper's anti-bulge mechanism.
                                                 // On a settled flat top EVmax ~0.39 < t_low=0.4, so
                                                 // it never fires there (measured: 87-92% fewer
                                                 // isosurface bulges on a dam break with it on).
    float m_fstep = 0.0f;                        // field grid spacing (derived)

    // grid of particles for neighbor search — flat SoA buckets (paper [30]:
    // Ihmsen et al. 2011, "Parallel data structures...") so cell traversal is
    // cache-friendly contiguous memory instead of per-cell vectors.
    int m_gw = 0, m_gh = 0, m_gd = 0;
    std::vector<int> m_cellStart;      // cell id -> [start, start+1) into m_cellParticles
    std::vector<int> m_cellParticles;  // particle ids, grouped by cell
    Math::Vector3 m_origin;            // grid origin (bbox min - margin)
    std::vector<float> m_color;        // per particle
    std::vector<Math::Vector3> m_normal;   // per particle (grad color)
    std::vector<int> m_nbCount;        // neighbours within h, per particle
    std::vector<int> m_surface;        // surface particle indices

    // grid of surface particles (cell size = 2r; band half-extent 2.0r = superset of the paper 2r-total-span)
    float m_R = 0.0f;
    int m_sw = 0, m_sh = 0, m_sd = 0;
    std::vector<int> m_surfStart;      // flat SoA buckets, same scheme as m_cellStart
    std::vector<int> m_surfParticles;
    std::vector<Math::Vector3> m_surfOutNormal;  // unit OUTWARD normal per surface particle (unused in Marching; kept for compat)

    // field grid
    int m_fw = 0, m_fh = 0, m_fd = 0;            // field grid dims (corner count)
    std::vector<float> m_field;                  // implicit values at grid corners
    std::vector<uint8_t> m_surfVert;             // 1 = surface vertex (paper Sec. 4.1); triangulate only cells whose 8 corners are all surface vertices
    std::vector<Math::Vector3> m_surfNormal;     // outward normal at surface corners (paper Sec. 4.2: normals interpolated from grid-vertex normals)

    // output
    std::vector<float> m_verts;
    std::vector<unsigned> m_indices;

    // Marching Cubes scratch, kept as members and reused frame to frame: at
    // r/2 field resolution these run to hundreds of MB per rebuild (a 100k-
    // particle dam break: ~78 MB edge table + ~78 MB lazy normals + flags).
    // Re-allocating them every rebuild (every 3rd frame) churns the heap and
    // leaves the committed size growing for the whole run, which on Windows
    // eventually makes a later allocation throw std::bad_alloc.
    std::vector<int>           m_mcEdgePoint;   // nVerts*3, shared edge ids
    std::vector<float>         m_mcPointData;   // pos(3)+normal(3) per point
    std::vector<Math::Vector3> m_mcLazyNrm;
    std::vector<uint8_t>       m_mcLazyDone;
    std::vector<uint8_t>       m_mcCellMark;    // narrow-band cell mask

    // optional OpenCL backend (GPU scalar-field construction; CPU Marching Cubes)
    std::unique_ptr<SurfaceReconstructorCL> m_gpu;
};
