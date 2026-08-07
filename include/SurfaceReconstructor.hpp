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

class SurfaceReconstructor {
public:
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
    void SetBandScale(float b) { m_bandScale = b; }   // surface-vertex band half-width in units of r (default 1.5)

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
    float m_bandScale = 1.5f;   // surface-vertex band half-width in units of r (paper uses 1.0; 1.5 keeps moving water crack-free)                    // field grid spacing = gridScale * particle spacing
    bool m_useEvmax = false;                     // paper Eq. 2-3 EVmax factor (measured 0% trigger on dam break)
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

    // grid of surface particles (cell size = 2r; band half-extent 1.5r = superset of the paper 2r-total-span)
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
};
