// SurfaceReconstructorCL — OpenCL 计算核心 for Akinci et al. 2012 surface
// reconstruction (CGF 31(6), "Parallel surface reconstruction for particle-based
// fluids"). This class replaces the CPU-side scalar-field construction
// (BuildGrid / ComputeColorAndNormal / BuildImplicitField) with OpenCL kernels
// on the GPU; the Marching Cubes triangulation stays on the CPU exactly like
// the paper (Sec. 4.2: "The triangulation process is not parallelized, since it
// takes a rather small amount of time").
//
// Pipeline (paper Alg. 1 + Sec. 4.1):
//   1. color field  c_i = sum_j V_j W(x_i - x_j)          [GPU kernel]
//   2. surface particles: c_i < threshold  OR  < 25 neigh [CPU, O(N)]
//   3. surface grid vertices: vertices inside the AABB
//      (+-bandScale*r per axis) of a surface particle     [GPU kernel]
//   4. scalar value phi(x) = |x - xbar| - r * f,  xbar the
//      weighted centroid over R = 4r (Eq. 1-5)            [GPU kernel]
//
// The uniform grids (particle buckets and surface-particle buckets) are built
// on the CPU (flat SoA: count -> prefix sum -> scatter); they are O(N) and far
// cheaper than the per-vertex gather loops, so keeping them on the host avoids
// a GPU parallel prefix-sum dependency with no measurable cost.

#pragma once
#include "Math/Vector3.hpp"
#include <vector>
#include <cstdint>

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

class SurfaceReconstructorCL {
public:
    SurfaceReconstructorCL();
    ~SurfaceReconstructorCL();
    SurfaceReconstructorCL(const SurfaceReconstructorCL&) = delete;
    SurfaceReconstructorCL& operator=(const SurfaceReconstructorCL&) = delete;

    bool Available() const { return m_ok; }
    const char* LastError() const { return m_err; }

    // Computes the scalar field + surface-vertex marks for one frame.
    // Output contract matches SurfaceReconstructor::BuildImplicitField:
    //   field     [fw*fh*fd]  phi at grid corners (sentinel +r outside the band)
    //   surfVert  [fw*fh*fd]  1 = surface vertex (band member)
    //   surfNormal           CPU path fills it; on the GPU path it is left EMPTY
    //                        and MarchingCubes rebuilds corner normals lazily
    //                        from the field gradient (avoids reading ~2.5 MB
    //                        of normals back over PCIe each frame).
    // Returns false on any OpenCL error (caller falls back to the CPU path).
    bool ComputeField(const std::vector<Math::Vector3>& particles, float spacing,
                      float bandScale, float gridScale, bool useEvmax, float surfaceThreshold,
                      Math::Vector3& origin, float& fstep, int& fw, int& fh, int& fd,
                      std::vector<float>& field,
                      std::vector<uint8_t>& surfVert,
                      std::vector<Math::Vector3>& surfNormal);

private:
    bool EnsureInit();
    void SetErr(const char* fmt, int code);

    // CPU helpers (same flat-SoA buckets as SurfaceReconstructor::BuildGrid)
    bool BuildParticleGrid(const std::vector<Math::Vector3>& particles, float h);
    bool BuildSurfaceParticleGrid(const std::vector<Math::Vector3>& particles, float R);

    // GPU kernel launchers
    bool RunColorField(int n);
    bool RunMarkVertices(int n);
    bool RunScalarField(int n);

    // device-buffer (re)allocation helper: grow-only, reused across frames
    bool EnsureBuffer(cl_mem* buf, size_t bytes);

    bool m_ok = false;
    bool m_init = false;
    char m_err[512] = {};

    // OpenCL objects
    cl_platform_id m_platform = 0;
    cl_device_id   m_device = 0;
    cl_context     m_context = 0;
    cl_command_queue m_queue = 0;
    cl_program     m_program = 0;
    cl_kernel      m_kColor = 0;
    cl_kernel      m_kMark = 0;
    cl_kernel      m_kScalar = 0;

    // device buffers (grow-only)
    cl_mem m_bufPos = 0;          // float4 positions
    cl_mem m_bufCellStart = 0;    // int [nCells+1]
    cl_mem m_bufCellParts = 0;    // int [N]
    cl_mem m_bufSurfStart = 0;    // int [nSurfCells+1]
    cl_mem m_bufSurfParts = 0;    // int [nSurfParticles]
    cl_mem m_bufIsSurface = 0;    // uchar [N]
    cl_mem m_bufSurfVert = 0;     // uchar [nField]
    cl_mem m_bufField = 0;        // float [nField]
    cl_mem m_bufNormal = 0;       // float4 [nField]

    // current frame geometry (host copies of grid params)
    Math::Vector3 m_origin;
    float m_fstep = 0.0f;
    int m_fw = 0, m_fh = 0, m_fd = 0;
    int m_gw = 0, m_gh = 0, m_gd = 0;
    float m_h = 0.0f, m_R = 0.0f, m_r = 0.0f, m_vol = 0.0f;
    int m_sw = 0, m_sh = 0, m_sd = 0;
    float m_bandScale = 2.0f, m_gridScale = 1.0f;
    float m_surfaceThreshold = 0.8f;
    bool m_useEvmax = false;

    // host-side grid data (kept for buffer uploads)
    std::vector<int> m_cellStart, m_cellParts;
    std::vector<int> m_surfStart, m_surfParts;
    std::vector<int> m_surface;
    std::vector<unsigned char> m_isSurfaceHost;   // surface-particle marks read back from GPU
};
