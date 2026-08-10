#include "SurfaceReconstructorCL.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// OpenCL kernels (embedded source, no external .cl file to load)
// ---------------------------------------------------------------------------
static const char* kKernelSrc = R"CL(
#define PI 3.14159265358979323846f

// cubic spline kernel W (support h), same as the CPU reference
float wCubic(float rr, float h) {
    float q = rr / h;
    if (q > 1.0f) return 0.0f;
    float c = 8.0f / (PI * h * h * h);
    if (q <= 0.5f) return c * (1.0f - 6.0f*q*q + 6.0f*q*q*q);
    float t = 1.0f - q;
    return c * 2.0f * t*t*t;
}

// Paper Alg. 1 step 1: color field c_i = sum_j V_j W(x_i - x_j)  + neighbour
// count, and the surface-particle test in the same pass (color < threshold OR
// fewer than 25 neighbours) so we never read color/nb back to the host.
__kernel void color_field(
    __global const float4* pos,
    __global const int* cellStart, __global const int* cellParts,
    int gw, int gh, int gd,
    float originX, float originY, float originZ,
    float h, float vol, float threshold,
    __global uchar* isSurface)
{
    int i = get_global_id(0);
    float4 p = pos[i];
    int gx = clamp((int)floor((p.x-originX)/h), 0, gw-1);
    int gy = clamp((int)floor((p.y-originY)/h), 0, gh-1);
    int gz = clamp((int)floor((p.z-originZ)/h), 0, gd-1);
    float c = vol * wCubic(0.0f, h);   // self term
    int cnt = 0;
    for (int dz = -1; dz <= 1; dz++)
    for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
        int ccx = gx+dx, ccy = gy+dy, ccz = gz+dz;
        if (ccx < 0 || ccx >= gw || ccy < 0 || ccy >= gh || ccz < 0 || ccz >= gd) continue;
        int cid = (ccz*gw*gh) + (ccy*gw) + ccx;
        for (int k = cellStart[cid]; k < cellStart[cid+1]; k++) {
            int j = cellParts[k];
            if (j == i) continue;
            float3 r = pos[j].xyz - p.xyz;
            float rr = length(r);
            if (rr < h) { c += vol * wCubic(rr, h); cnt++; }
        }
    }
    isSurface[i] = (c < threshold || cnt < 25) ? (uchar)1 : (uchar)0;
}

// Paper Alg. 1 step 2-3: mark grid vertices inside the AABB (+-bandR per axis)
// of any surface particle. One thread per grid vertex; no races.
__kernel void mark_vertices(
    __global const float4* pos,
    __global const int* surfStart, __global const int* surfParts,
    int sw, int sh, int sd,
    float originX, float originY, float originZ,
    float cellR, float bandR,
    float fstep, int fw, int fh, int fd,
    __global uchar* surfVert)
{
    int idx = get_global_id(0);
    if (idx >= fw*fh*fd) return;
    int cz = idx / (fw*fh);
    int cy = (idx / fw) % fh;
    int cx = idx % fw;
    float4 p = (float4)(originX + cx*fstep, originY + cy*fstep, originZ + cz*fstep, 0.0f);
    int scx = clamp((int)floor((p.x-originX)/cellR), 0, sw-1);
    int scy = clamp((int)floor((p.y-originY)/cellR), 0, sh-1);
    int scz = clamp((int)floor((p.z-originZ)/cellR), 0, sd-1);
    uchar hit = 0;
    for (int dz = -1; dz <= 1 && !hit; dz++)
    for (int dy = -1; dy <= 1 && !hit; dy++)
    for (int dx = -1; dx <= 1 && !hit; dx++) {
        int ccx = scx+dx, ccy = scy+dy, ccz = scz+dz;
        if (ccx < 0 || ccx >= sw || ccy < 0 || ccy >= sh || ccz < 0 || ccz >= sd) continue;
        int cid = (ccz*sw*sh) + (ccy*sw) + ccx;
        for (int k = surfStart[cid]; k < surfStart[cid+1] && !hit; k++) {
            float4 xi = pos[surfParts[k]];
            if (fabs(p.x-xi.x) <= bandR && fabs(p.y-xi.y) <= bandR && fabs(p.z-xi.z) <= bandR)
                hit = 1;
        }
    }
    surfVert[idx] = hit;
}

// Paper Alg. 1 step 4: phi(x) = |x - xbar| - r*f over R = 4r (Eq. 1-5).
// One thread per grid vertex; only band members do the gather. Normals are NOT
// written here — the host rebuilds them lazily from the field gradient only for
// the corners that Marching Cubes actually needs (saves ~2.5 MB of read-back
// per frame on PCIe-bound setups).
__kernel void scalar_field(
    __global const float4* pos,
    __global const int* cellStart, __global const int* cellParts,
    int gw, int gh, int gd,
    float originX, float originY, float originZ,
    float h, float R, float R2, float r,
    float fstep, int fw, int fh, int fd,
    int useEvmax,
    __global const uchar* surfVert,
    __global float* field)
{
    int idx = get_global_id(0);
    if (idx >= fw*fh*fd) return;
    int cz = idx / (fw*fh);
    int cy = (idx / fw) % fh;
    int cx = idx % fw;
    float4 p = (float4)(originX + cx*fstep, originY + cy*fstep, originZ + cz*fstep, 0.0f);
    if (surfVert[idx] == 0) { field[idx] = r; return; }

    float4 xbar = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
    float wsum = 0.0f;
    float3 sumdw = (float3)(0.0f, 0.0f, 0.0f);
    float row[9] = {0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f};

    int gx = clamp((int)floor((p.x-originX)/h), 0, gw-1);
    int gy = clamp((int)floor((p.y-originY)/h), 0, gh-1);
    int gz = clamp((int)floor((p.z-originZ)/h), 0, gd-1);
    for (int dz = -2; dz <= 2; dz++)
    for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++) {
        int ccx = gx+dx, ccy = gy+dy, ccz = gz+dz;
        if (ccx < 0 || ccx >= gw || ccy < 0 || ccy >= gh || ccz < 0 || ccz >= gd) continue;
        int cid = (ccz*gw*gh) + (ccy*gw) + ccx;
        for (int k = cellStart[cid]; k < cellStart[cid+1]; k++) {
            float4 xj = pos[cellParts[k]];
            float3 dj = xj.xyz - p.xyz;
            float d2 = dot(dj, dj);
            float s2 = d2 / R2;
            if (s2 >= 1.0f) continue;
            float t = 1.0f - s2;
            float w = t*t*t;
            xbar += xj * w;
            wsum += w;
            if (useEvmax) {
                float d = sqrt(d2);
                if (d > 1e-9f) {
                    float s = d / R;
                    float kp = 6.0f * s * (1.0f - s*s) * (1.0f - s*s);
                    float3 dw = dj * (kp / (R * d));
                    sumdw += dw;
                    row[0] += dw.x * xj.x; row[1] += dw.x * xj.y; row[2] += dw.x * xj.z;
                    row[3] += dw.y * xj.x; row[4] += dw.y * xj.y; row[5] += dw.y * xj.z;
                    row[6] += dw.z * xj.x; row[7] += dw.z * xj.y; row[8] += dw.z * xj.z;
                }
            }
        }
    }
    if (wsum <= 1e-9f) { field[idx] = r; return; }

    xbar = xbar * (1.0f / wsum);
    float f = 1.0f;
    if (useEvmax) {
        // J = (sum dw x^T - (sum w x)(sum dw)^T) / W^2
        float W2 = wsum * wsum;
        float xs[3] = { xbar.x*wsum, xbar.y*wsum, xbar.z*wsum };
        float J[9];
        J[0] = (row[0]*wsum - xs[0]*sumdw.x)/W2; J[1] = (row[1]*wsum - xs[0]*sumdw.y)/W2; J[2] = (row[2]*wsum - xs[0]*sumdw.z)/W2;
        J[3] = (row[3]*wsum - xs[1]*sumdw.x)/W2; J[4] = (row[4]*wsum - xs[1]*sumdw.y)/W2; J[5] = (row[5]*wsum - xs[1]*sumdw.z)/W2;
        J[6] = (row[6]*wsum - xs[2]*sumdw.x)/W2; J[7] = (row[7]*wsum - xs[2]*sumdw.y)/W2; J[8] = (row[8]*wsum - xs[2]*sumdw.z)/W2;
        // symmetric part A = (J + J^T)/2
        float A[9];
        A[0]=J[0]; A[1]=0.5f*(J[1]+J[3]); A[2]=0.5f*(J[2]+J[6]);
        A[3]=A[1]; A[4]=J[4];             A[5]=0.5f*(J[5]+J[7]);
        A[6]=A[2]; A[7]=A[5];             A[8]=J[8];
        // power iteration (2 iters, like the CPU path)
        float3 v = (float3)(1.0f, 0.0f, 0.0f);
        for (int it = 0; it < 2; it++) {
            float3 Av = (float3)(
                A[0]*v.x + A[1]*v.y + A[2]*v.z,
                A[3]*v.x + A[4]*v.y + A[5]*v.z,
                A[6]*v.x + A[7]*v.y + A[8]*v.z);
            float nrm = length(Av);
            if (nrm > 1e-12f) v = Av / nrm;
        }
        float evmax = v.x*(A[0]*v.x + A[1]*v.y + A[2]*v.z)
                    + v.y*(A[3]*v.x + A[4]*v.y + A[5]*v.z)
                    + v.z*(A[6]*v.x + A[7]*v.y + A[8]*v.z);
        const float tLow = 0.4f, tHigh = 3.5f;
        if (evmax >= tLow) {
            float g = (tHigh - evmax) / (tHigh - tLow);
            g = clamp(g, 0.0f, 1.0f);
            f = g*g*g - 3.0f*g*g + 3.0f*g;
        }
    }
    float3 d = p.xyz - xbar.xyz;
    float dl = length(d);
    field[idx] = dl - r * f;
}
)CL";

// ---------------------------------------------------------------------------
namespace {
constexpr float kPi = 3.14159265358979323846f;

inline void SetErrMsg(char* dst, size_t n, const char* fmt, int code) {
    snprintf(dst, n, fmt, code);
}
} // namespace

SurfaceReconstructorCL::SurfaceReconstructorCL() {
    m_err[0] = '\0';
    m_ok = EnsureInit();   // probe the GPU eagerly so Available() is meaningful
}

SurfaceReconstructorCL::~SurfaceReconstructorCL() {
    if (m_kScalar) clReleaseKernel(m_kScalar);
    if (m_kMark)   clReleaseKernel(m_kMark);
    if (m_kColor)  clReleaseKernel(m_kColor);
    if (m_program) clReleaseProgram(m_program);
    if (m_queue)   clReleaseCommandQueue(m_queue);
    if (m_context) clReleaseContext(m_context);
#define REL(buf) if (buf) clReleaseMemObject(buf)
    REL(m_bufPos); REL(m_bufCellStart); REL(m_bufCellParts);
    REL(m_bufSurfStart); REL(m_bufSurfParts); REL(m_bufIsSurface);
    REL(m_bufSurfVert); REL(m_bufField); REL(m_bufNormal);
#undef REL
}

bool SurfaceReconstructorCL::EnsureInit() {
    if (m_init) return m_ok;
    m_init = true;
    cl_int err = 0;

    cl_uint nplat = 0;
    if (clGetPlatformIDs(0, nullptr, &nplat) != CL_SUCCESS || nplat == 0) {
        SetErrMsg(m_err, sizeof(m_err), "no OpenCL platforms (%d)", 0);
        return (m_ok = false);
    }
    std::vector<cl_platform_id> plats(nplat);
    clGetPlatformIDs(nplat, plats.data(), nullptr);
    for (cl_uint i = 0; i < nplat; i++) {
        cl_uint ndev = 0;
        if (clGetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 0, nullptr, &ndev) != CL_SUCCESS || ndev == 0)
            continue;
        m_platform = plats[i];
        clGetDeviceIDs(m_platform, CL_DEVICE_TYPE_GPU, 1, &m_device, nullptr);
        break;
    }
    if (!m_device) {
        SetErrMsg(m_err, sizeof(m_err), "no OpenCL GPU device (%d)", 0);
        return (m_ok = false);
    }

    m_context = clCreateContext(nullptr, 1, &m_device, nullptr, nullptr, &err);
    if (!m_context) { SetErrMsg(m_err, sizeof(m_err), "clCreateContext failed (%d)", err); return (m_ok = false); }

    cl_queue_properties props[] = { CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0 };
    m_queue = clCreateCommandQueueWithProperties(m_context, m_device, props, &err);
    if (!m_queue) { SetErrMsg(m_err, sizeof(m_err), "clCreateCommandQueue failed (%d)", err); return (m_ok = false); }

    m_program = clCreateProgramWithSource(m_context, 1, &kKernelSrc, nullptr, &err);
    if (!m_program) { SetErrMsg(m_err, sizeof(m_err), "clCreateProgramWithSource failed (%d)", err); return (m_ok = false); }
    err = clBuildProgram(m_program, 1, &m_device, "-cl-fast-relaxed-math", nullptr, nullptr);
    if (err != CL_SUCCESS) {
        char log[8192] = {};
        clGetProgramBuildInfo(m_program, m_device, CL_PROGRAM_BUILD_LOG, sizeof(log), log, nullptr);
        snprintf(m_err, sizeof(m_err), "kernel build failed (%d): %s", err, log);
        return (m_ok = false);
    }
    m_kColor  = clCreateKernel(m_program, "color_field", &err);
    m_kMark   = clCreateKernel(m_program, "mark_vertices", &err);
    m_kScalar = clCreateKernel(m_program, "scalar_field", &err);
    if (!m_kColor || !m_kMark || !m_kScalar) {
        SetErrMsg(m_err, sizeof(m_err), "clCreateKernel failed (%d)", err);
        return (m_ok = false);
    }
    return (m_ok = true);
}

bool SurfaceReconstructorCL::EnsureBuffer(cl_mem* buf, size_t bytes) {
    if (bytes == 0) return true;
    if (*buf) {
        size_t sz = 0;
        if (clGetMemObjectInfo(*buf, CL_MEM_SIZE, sizeof(sz), &sz, nullptr) == CL_SUCCESS && sz >= bytes)
            return true;
        clReleaseMemObject(*buf);
        *buf = nullptr;
    }
    cl_int err = 0;
    *buf = clCreateBuffer(m_context, CL_MEM_READ_WRITE, bytes, nullptr, &err);
    return *buf != nullptr;
}

// ---------------------------------------------------------------------------
bool SurfaceReconstructorCL::BuildParticleGrid(const std::vector<Math::Vector3>& particles, float h) {
    const size_t N = particles.size();
    Math::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    for (const auto& p : particles) {
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
    for (size_t i = 0; i < N; i++) {
        int cx = std::clamp((int)std::floor((particles[i].x - m_origin.x) / h), 0, m_gw - 1);
        int cy = std::clamp((int)std::floor((particles[i].y - m_origin.y) / h), 0, m_gh - 1);
        int cz = std::clamp((int)std::floor((particles[i].z - m_origin.z) / h), 0, m_gd - 1);
        const size_t cid = (size_t)cz * m_gw * m_gh + (size_t)cy * m_gw + (size_t)cx;
        m_cellStart[cid]++;
    }
    int acc = 0;
    for (size_t c = 0; c < nCells; c++) { int cnt = m_cellStart[c]; m_cellStart[c] = acc; acc += cnt; }
    m_cellStart[nCells] = acc;
    m_cellParts.assign(N, 0);
    std::vector<int> fill = m_cellStart;
    for (size_t i = 0; i < N; i++) {
        int cx = std::clamp((int)std::floor((particles[i].x - m_origin.x) / h), 0, m_gw - 1);
        int cy = std::clamp((int)std::floor((particles[i].y - m_origin.y) / h), 0, m_gh - 1);
        int cz = std::clamp((int)std::floor((particles[i].z - m_origin.z) / h), 0, m_gd - 1);
        const size_t cid = (size_t)cz * m_gw * m_gh + (size_t)cy * m_gw + (size_t)cx;
        if (fill[cid] >= (int)N) fill[cid] = (int)N - 1;
        m_cellParts[fill[cid]++] = (int)i;
    }
    return true;
}

// surface-particle buckets; stores DIRECT particle indices (unlike the CPU
// reference which stores indices into the m_surface list) — the kernels use
// pos[surfParts[k]] directly.
bool SurfaceReconstructorCL::BuildSurfaceParticleGrid(const std::vector<Math::Vector3>& particles, float R) {
    const size_t ns = m_surface.size();
    m_sw = std::max(1, (int)std::ceil((m_gw * m_h) / R));
    m_sh = std::max(1, (int)std::ceil((m_gh * m_h) / R));
    m_sd = std::max(1, (int)std::ceil((m_gd * m_h) / R));
    const size_t nCells = (size_t)m_sw * m_sh * m_sd;
    m_surfStart.assign(nCells + 1, 0);
    for (size_t tt = 0; tt < ns; tt++) {
        int si = m_surface[tt];
        int cx = std::clamp((int)std::floor((particles[si].x - m_origin.x) / R), 0, m_sw - 1);
        int cy = std::clamp((int)std::floor((particles[si].y - m_origin.y) / R), 0, m_sh - 1);
        int cz = std::clamp((int)std::floor((particles[si].z - m_origin.z) / R), 0, m_sd - 1);
        m_surfStart[(size_t)cz * m_sw * m_sh + (size_t)cy * m_sw + (size_t)cx]++;
    }
    int acc = 0;
    for (size_t c = 0; c < nCells; c++) { int cnt = m_surfStart[c]; m_surfStart[c] = acc; acc += cnt; }
    m_surfStart[nCells] = acc;
    m_surfParts.assign(ns, 0);
    std::vector<int> fill = m_surfStart;
    for (size_t tt = 0; tt < ns; tt++) {
        int si = m_surface[tt];
        int cx = std::clamp((int)std::floor((particles[si].x - m_origin.x) / R), 0, m_sw - 1);
        int cy = std::clamp((int)std::floor((particles[si].y - m_origin.y) / R), 0, m_sh - 1);
        int cz = std::clamp((int)std::floor((particles[si].z - m_origin.z) / R), 0, m_sd - 1);
        const size_t cid = (size_t)cz * m_sw * m_sh + (size_t)cy * m_sw + (size_t)cx;
        m_surfParts[fill[cid]++] = si;
    }
    return true;
}

bool SurfaceReconstructorCL::RunColorField(int n) {
    if (!EnsureBuffer(&m_bufIsSurface, sizeof(cl_uchar) * n))
        return false;
    int arg = 0;
    clSetKernelArg(m_kColor, arg++, sizeof(cl_mem), &m_bufPos);
    clSetKernelArg(m_kColor, arg++, sizeof(cl_mem), &m_bufCellStart);
    clSetKernelArg(m_kColor, arg++, sizeof(cl_mem), &m_bufCellParts);
    clSetKernelArg(m_kColor, arg++, sizeof(int), &m_gw);
    clSetKernelArg(m_kColor, arg++, sizeof(int), &m_gh);
    clSetKernelArg(m_kColor, arg++, sizeof(int), &m_gd);
    float ox = m_origin.x, oy = m_origin.y, oz = m_origin.z;
    clSetKernelArg(m_kColor, arg++, sizeof(float), &ox);
    clSetKernelArg(m_kColor, arg++, sizeof(float), &oy);
    clSetKernelArg(m_kColor, arg++, sizeof(float), &oz);
    clSetKernelArg(m_kColor, arg++, sizeof(float), &m_h);
    clSetKernelArg(m_kColor, arg++, sizeof(float), &m_vol);
    clSetKernelArg(m_kColor, arg++, sizeof(float), &m_surfaceThreshold);
    clSetKernelArg(m_kColor, arg++, sizeof(cl_mem), &m_bufIsSurface);
    size_t gsz = (size_t)n;
    cl_int err = clEnqueueNDRangeKernel(m_queue, m_kColor, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
    return err == CL_SUCCESS;
}

bool SurfaceReconstructorCL::RunMarkVertices(int n) {
    if (!EnsureBuffer(&m_bufSurfVert, sizeof(cl_uchar) * n))
        return false;
    int arg = 0;
    clSetKernelArg(m_kMark, arg++, sizeof(cl_mem), &m_bufPos);
    clSetKernelArg(m_kMark, arg++, sizeof(cl_mem), &m_bufSurfStart);
    clSetKernelArg(m_kMark, arg++, sizeof(cl_mem), &m_bufSurfParts);
    clSetKernelArg(m_kMark, arg++, sizeof(int), &m_sw);
    clSetKernelArg(m_kMark, arg++, sizeof(int), &m_sh);
    clSetKernelArg(m_kMark, arg++, sizeof(int), &m_sd);
    float ox = m_origin.x, oy = m_origin.y, oz = m_origin.z;
    clSetKernelArg(m_kMark, arg++, sizeof(float), &ox);
    clSetKernelArg(m_kMark, arg++, sizeof(float), &oy);
    clSetKernelArg(m_kMark, arg++, sizeof(float), &oz);
    float cellR = 2.0f * m_r, bandR = m_bandScale * m_r;
    clSetKernelArg(m_kMark, arg++, sizeof(float), &cellR);
    clSetKernelArg(m_kMark, arg++, sizeof(float), &bandR);
    clSetKernelArg(m_kMark, arg++, sizeof(float), &m_fstep);
    clSetKernelArg(m_kMark, arg++, sizeof(int), &m_fw);
    clSetKernelArg(m_kMark, arg++, sizeof(int), &m_fh);
    clSetKernelArg(m_kMark, arg++, sizeof(int), &m_fd);
    clSetKernelArg(m_kMark, arg++, sizeof(cl_mem), &m_bufSurfVert);
    size_t gsz = (size_t)n;
    cl_int err = clEnqueueNDRangeKernel(m_queue, m_kMark, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
    return err == CL_SUCCESS;
}

bool SurfaceReconstructorCL::RunScalarField(int n) {
    if (!EnsureBuffer(&m_bufField, sizeof(float) * n))
        return false;
    int arg = 0;
    clSetKernelArg(m_kScalar, arg++, sizeof(cl_mem), &m_bufPos);
    clSetKernelArg(m_kScalar, arg++, sizeof(cl_mem), &m_bufCellStart);
    clSetKernelArg(m_kScalar, arg++, sizeof(cl_mem), &m_bufCellParts);
    clSetKernelArg(m_kScalar, arg++, sizeof(int), &m_gw);
    clSetKernelArg(m_kScalar, arg++, sizeof(int), &m_gh);
    clSetKernelArg(m_kScalar, arg++, sizeof(int), &m_gd);
    float ox = m_origin.x, oy = m_origin.y, oz = m_origin.z;
    clSetKernelArg(m_kScalar, arg++, sizeof(float), &ox);
    clSetKernelArg(m_kScalar, arg++, sizeof(float), &oy);
    clSetKernelArg(m_kScalar, arg++, sizeof(float), &oz);
    float h = m_h, R = 4.0f * m_r, R2 = R * R, r = m_r;
    clSetKernelArg(m_kScalar, arg++, sizeof(float), &h);
    clSetKernelArg(m_kScalar, arg++, sizeof(float), &R);
    clSetKernelArg(m_kScalar, arg++, sizeof(float), &R2);
    clSetKernelArg(m_kScalar, arg++, sizeof(float), &r);
    clSetKernelArg(m_kScalar, arg++, sizeof(float), &m_fstep);
    clSetKernelArg(m_kScalar, arg++, sizeof(int), &m_fw);
    clSetKernelArg(m_kScalar, arg++, sizeof(int), &m_fh);
    clSetKernelArg(m_kScalar, arg++, sizeof(int), &m_fd);
    int useEvmax = m_useEvmax ? 1 : 0;
    clSetKernelArg(m_kScalar, arg++, sizeof(int), &useEvmax);
    clSetKernelArg(m_kScalar, arg++, sizeof(cl_mem), &m_bufSurfVert);
    clSetKernelArg(m_kScalar, arg++, sizeof(cl_mem), &m_bufField);
    size_t gsz = (size_t)n;
    cl_int err = clEnqueueNDRangeKernel(m_queue, m_kScalar, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
    return err == CL_SUCCESS;
}

// ---------------------------------------------------------------------------
bool SurfaceReconstructorCL::ComputeField(const std::vector<Math::Vector3>& particles,
                                          float spacing, float bandScale, float gridScale,
                                          bool useEvmax, float surfaceThreshold,
                                          Math::Vector3& origin, float& fstep, int& fw, int& fh, int& fd,
                                          std::vector<float>& field,
                                          std::vector<uint8_t>& surfVert,
                                          std::vector<Math::Vector3>& surfNormal) {
    if (!EnsureInit()) return false;
    const size_t N = particles.size();
    if (N == 0) return false;

    m_r = spacing;
    m_h = 2.0f * spacing;
    m_vol = spacing * spacing * spacing;
    m_bandScale = bandScale;
    m_gridScale = gridScale;
    m_useEvmax = useEvmax;
    m_surfaceThreshold = surfaceThreshold;
    m_fstep = m_gridScale * spacing;

    if (!BuildParticleGrid(particles, m_h)) return false;
    m_fw = (int)std::ceil((m_gw * m_h) / m_fstep) + 1;
    m_fh = (int)std::ceil((m_gh * m_h) / m_fstep) + 1;
    m_fd = (int)std::ceil((m_gd * m_h) / m_fstep) + 1;
    const size_t nField = (size_t)m_fw * m_fh * m_fd;

    // upload positions (float4) + particle grid
    std::vector<cl_float4> pos4(N);
    for (size_t i = 0; i < N; i++) {
        pos4[i].s[0] = particles[i].x; pos4[i].s[1] = particles[i].y;
        pos4[i].s[2] = particles[i].z; pos4[i].s[3] = 0.0f;
    }
    if (!EnsureBuffer(&m_bufPos, sizeof(cl_float4) * N) ||
        !EnsureBuffer(&m_bufCellStart, sizeof(int) * (m_cellStart.size())) ||
        !EnsureBuffer(&m_bufCellParts, sizeof(int) * m_cellParts.size()))
        return false;
    cl_int err = clEnqueueWriteBuffer(m_queue, m_bufPos, CL_FALSE, 0, sizeof(cl_float4) * N, pos4.data(), 0, nullptr, nullptr);
    if (err != CL_SUCCESS) return false;
    clEnqueueWriteBuffer(m_queue, m_bufCellStart, CL_FALSE, 0, sizeof(int) * m_cellStart.size(), m_cellStart.data(), 0, nullptr, nullptr);
    clEnqueueWriteBuffer(m_queue, m_bufCellParts, CL_FALSE, 0, sizeof(int) * m_cellParts.size(), m_cellParts.data(), 0, nullptr, nullptr);

    // 1) color field + surface-particle marks
    if (!RunColorField((int)N)) return false;
    m_isSurfaceHost.resize(N);
    clEnqueueReadBuffer(m_queue, m_bufIsSurface, CL_FALSE, 0, sizeof(cl_uchar) * N, m_isSurfaceHost.data(), 0, nullptr, nullptr);
    clFinish(m_queue);   // surface-particle list is built on the host from the marks

    // 2) surface particles (host scan, O(N))
    m_surface.clear();
    m_surface.reserve(N / 4);
    for (size_t i = 0; i < N; i++)
        if (m_isSurfaceHost[i])
            m_surface.push_back((int)i);

    // 3) surface-particle buckets (host, O(nSurf))
    m_R = 2.0f * spacing;
    if (!BuildSurfaceParticleGrid(particles, m_R)) return false;
    if (!EnsureBuffer(&m_bufSurfStart, sizeof(int) * m_surfStart.size()) ||
        !EnsureBuffer(&m_bufSurfParts, sizeof(int) * m_surfParts.size()))
        return false;
    clEnqueueWriteBuffer(m_queue, m_bufSurfStart, CL_FALSE, 0, sizeof(int) * m_surfStart.size(), m_surfStart.data(), 0, nullptr, nullptr);
    clEnqueueWriteBuffer(m_queue, m_bufSurfParts, CL_FALSE, 0, sizeof(int) * m_surfParts.size(), m_surfParts.data(), 0, nullptr, nullptr);

    // 4) mark surface vertices
    if (!RunMarkVertices((int)nField)) return false;

    // 5) scalar field
    if (!RunScalarField((int)nField)) return false;

    // read back (field + surfVert only; normals are rebuilt lazily on the CPU
    // from the field gradient by MarchingCubes when m_surfNormal is empty)
    field.resize(nField);
    surfVert.resize(nField);
    surfNormal.clear();
    clEnqueueReadBuffer(m_queue, m_bufSurfVert, CL_FALSE, 0, sizeof(cl_uchar) * nField, surfVert.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(m_queue, m_bufField, CL_FALSE, 0, sizeof(float) * nField, field.data(), 0, nullptr, nullptr);
    clFinish(m_queue);

    origin = m_origin;
    fstep = m_fstep;
    fw = m_fw; fh = m_fh; fd = m_fd;
    return true;
}
