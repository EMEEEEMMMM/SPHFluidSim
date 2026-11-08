#pragma once
// CUDA device source, compiled at runtime with NVRTC (and, on an MSVC+nvcc
// build, usable verbatim as a .cu translation unit).
//
// Design notes
// ------------
// * Neighbor storage is SoA: slot k of particle i lives at k*n + i. The old
//   per-particle layout (i*MAXNB + k) made a warp touch 32 separate 128-byte
//   cache lines per slot (16 bytes used), an 8x transaction amplification.
// * W and gradW are NOT cached. Recomputing sqrt per pair is cheap on the GPU,
//   while a pair[] cache costs 48*16 B per particle (4.2 MB at 8k, 25 MB at
//   32.8k, 7.7 GB at 10^7) and pushes the pressure-solve working set out of the
//   1 MB L2 of small cards; the 100 Jacobi iterations then stream from DRAM.
//   This is what SPlisHSPlasH does as well.
// * Lists are fixed-slot (MAXNB / MAXBNB). Cell arrays are sorted once; each
//   fluid pair is emitted by its lower index and mirrored into both lists, so
//   discovery and gradient work are not duplicated. Mirror insertion uses
//   atomic slot counters, therefore neighbor order and the last few FP bits can
//   vary between runs. Reductions still use deterministic warp shuffles and no
//   floating-point atomics.

namespace sphkernels {

inline constexpr const char* kSource = R"SPHK(
#define MAXNB 64
#define MAXBNB 48
#define BLOCK 256
#define WARPS (BLOCK / 32)

__device__ __forceinline__ float warpSum(float v) {
    #pragma unroll
    for (int off = 16; off > 0; off >>= 1) v += __shfl_down_sync(0xffffffffu, v, off);
    return v;
}

__device__ __forceinline__ float warpMax(float v) {
    #pragma unroll
    for (int off = 16; off > 0; off >>= 1) v = fmaxf(v, __shfl_down_sync(0xffffffffu, v, off));
    return v;
}

__device__ __forceinline__ float kernelW(float r, float h) {
    float q = r / h;
    if (q > 1.0f) return 0.0f;
    const float c = 8.0f / (3.14159265358979323846f * h * h * h);
    if (q <= 0.5f) return c * (1.0f - 6.0f * q * q + 6.0f * q * q * q);
    const float t = 1.0f - q;
    return c * 2.0f * t * t * t;
}

__device__ __forceinline__ float4 kernelGradW(float3 r, float h) {
    float rr = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z);
    if (rr < 1e-9f || rr >= h) return make_float4(0.f, 0.f, 0.f, 0.f);
    float q = rr / h;
    float dWdq;
    if (q <= 0.5f) dWdq = -12.0f * q + 18.0f * q * q;
    else           dWdq = -6.0f * (1.0f - q) * (1.0f - q);
    float scale = (8.0f / (3.14159265358979323846f * h * h * h)) * (dWdq / h) / rr;
    return make_float4(r.x * scale, r.y * scale, r.z * scale, 0.f);
}

__device__ __forceinline__ float wCohesion(float r, float h) {
    if (r <= 0.0f || r >= h) return 0.0f;
    const float k = 32.0f / (3.14159265358979323846f * powf(h, 9.0f));
    const float r3 = r * r * r;
    const float t = h - r;
    const float hr3 = t * t * t;
    if (r > 0.5f * h) return k * hr3 * r3;
    return k * (2.0f * hr3 * r3 - powf(h, 6.0f) / 64.0f);
}

__device__ __forceinline__ float wAdhesion(float r, float h) {
    if (r <= 0.5f * h || r >= h) return 0.0f;
    const float k = 0.007f / powf(h, 3.25f);
    float base = -4.0f * r * r / h + 6.0f * r - 2.0f * h;
    if (base < 0.0f) base = 0.0f;
    return k * powf(base, 0.25f);
}

__device__ __forceinline__ float3 cellCoords(float3 p, float ox, float oy, float oz,
                                             float invh, int gw, int gh, int gd) {
    int cx = (int)floorf((p.x - ox) * invh);
    int cy = (int)floorf((p.y - oy) * invh);
    int cz = (int)floorf((p.z - oz) * invh);
    cx = cx < 0 ? 0 : (cx >= gw ? gw - 1 : cx);
    cy = cy < 0 ? 0 : (cy >= gh ? gh - 1 : cy);
    cz = cz < 0 ? 0 : (cz >= gd ? gd - 1 : cz);
    return make_float3((float)cx, (float)cy, (float)cz);
}

__device__ __forceinline__ int cellIndex(int cx, int cy, int cz, int gw, int gh) {
    return cz * gw * gh + cy * gw + cx;
}

// Neighbor gradients carry the neighbor index in .w. This removes a separate
// 4-byte index stream from every pressure iteration.
__device__ __forceinline__ int neighborIndex(float4 g) { return __float_as_int(g.w); }

// ---------------------------------------------------------------------------
// Spatial hash
// ---------------------------------------------------------------------------
__global__ void kCellOf(const float4* pos, const float4* bpos, int n, int nb,
                        float ox, float oy, float oz, float invh,
                        int gw, int gh, int gd, int* cellOf) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n + nb) return;
    float3 p = (i < n)
        ? make_float3(pos[i].x, pos[i].y, pos[i].z)
        : make_float3(bpos[i - n].x, bpos[i - n].y, bpos[i - n].z);
    float3 c = cellCoords(p, ox, oy, oz, invh, gw, gh, gd);
    cellOf[i] = cellIndex((int)c.x, (int)c.y, (int)c.z, gw, gh);
}

__global__ void kCountCells(const int* cellOf, int total, int* cellCount) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= total) return;
    atomicAdd(&cellCount[cellOf[i]], 1);
}

// Device-side bounds make the grid independent of the host mirror, which is
// now refreshed lazily. Six floats per block, then a single-block reduction.
__global__ void kBounds(const float4* pos, const float4* bpos, int n, int nb, float* blockBounds) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    float mnx = 1e30f, mny = 1e30f, mnz = 1e30f;
    float mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
    if (i < n + nb) {
        const float3 p = (i < n) ? make_float3(pos[i].x, pos[i].y, pos[i].z)
                                 : make_float3(bpos[i - n].x, bpos[i - n].y, bpos[i - n].z);
        mnx = mxx = p.x; mny = mxy = p.y; mnz = mxz = p.z;
    }
    __shared__ float w[6][WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    mnx = -warpMax(-mnx); mny = -warpMax(-mny); mnz = -warpMax(-mnz);
    mxx = warpMax(mxx); mxy = warpMax(mxy); mxz = warpMax(mxz);
    if (lane == 0) {
        w[0][wid] = mnx; w[1][wid] = mny; w[2][wid] = mnz;
        w[3][wid] = mxx; w[4][wid] = mxy; w[5][wid] = mxz;
    }
    __syncthreads();
    if (wid == 0) {
        mnx = (lane < WARPS) ? w[0][lane] : 1e30f;
        mny = (lane < WARPS) ? w[1][lane] : 1e30f;
        mnz = (lane < WARPS) ? w[2][lane] : 1e30f;
        mxx = (lane < WARPS) ? w[3][lane] : -1e30f;
        mxy = (lane < WARPS) ? w[4][lane] : -1e30f;
        mxz = (lane < WARPS) ? w[5][lane] : -1e30f;
        mnx = -warpMax(-mnx); mny = -warpMax(-mny); mnz = -warpMax(-mnz);
        mxx = warpMax(mxx); mxy = warpMax(mxy); mxz = warpMax(mxz);
        if (lane == 0) {
            float* o = blockBounds + blockIdx.x * 6;
            o[0] = mnx; o[1] = mny; o[2] = mnz;
            o[3] = mxx; o[4] = mxy; o[5] = mxz;
        }
    }
}

__global__ void kBoundsFinal(const float* blockBounds, int nblocks, float* out) {
    __shared__ float w[6][WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    float mnx = 1e30f, mny = 1e30f, mnz = 1e30f;
    float mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
    for (int b = tid; b < nblocks; b += BLOCK) {
        const float* p = blockBounds + b * 6;
        mnx = fminf(mnx, p[0]); mny = fminf(mny, p[1]); mnz = fminf(mnz, p[2]);
        mxx = fmaxf(mxx, p[3]); mxy = fmaxf(mxy, p[4]); mxz = fmaxf(mxz, p[5]);
    }
    mnx = -warpMax(-mnx); mny = -warpMax(-mny); mnz = -warpMax(-mnz);
    mxx = warpMax(mxx); mxy = warpMax(mxy); mxz = warpMax(mxz);
    if (lane == 0) {
        w[0][wid] = mnx; w[1][wid] = mny; w[2][wid] = mnz;
        w[3][wid] = mxx; w[4][wid] = mxy; w[5][wid] = mxz;
    }
    __syncthreads();
    if (wid == 0) {
        mnx = (lane < WARPS) ? w[0][lane] : 1e30f;
        mny = (lane < WARPS) ? w[1][lane] : 1e30f;
        mnz = (lane < WARPS) ? w[2][lane] : 1e30f;
        mxx = (lane < WARPS) ? w[3][lane] : -1e30f;
        mxy = (lane < WARPS) ? w[4][lane] : -1e30f;
        mxz = (lane < WARPS) ? w[5][lane] : -1e30f;
        mnx = -warpMax(-mnx); mny = -warpMax(-mny); mnz = -warpMax(-mnz);
        mxx = warpMax(mxx); mxy = warpMax(mxy); mxz = warpMax(mxz);
        if (lane == 0) {
            out[0] = mnx; out[1] = mny; out[2] = mnz;
            out[3] = mxx; out[4] = mxy; out[5] = mxz;
        }
    }
}

// Work-efficient-enough block scan (Hillis-Steele in shared memory) plus a
// recursive scan of the block sums. Replaces a host round trip and a serial
// CPU prefix sum on every neighborhood rebuild.
__global__ void kScanBlocks(const int* in, int n, int* out, int* blockSums) {
    __shared__ int s[BLOCK];
    const int tid = threadIdx.x;
    const int i = blockIdx.x * BLOCK + tid;
    const int v = (i < n) ? in[i] : 0;
    s[tid] = v;
    __syncthreads();
    for (int off = 1; off < BLOCK; off <<= 1) {
        const int add = (tid >= off) ? s[tid - off] : 0;
        __syncthreads();
        s[tid] += add;
        __syncthreads();
    }
    if (i < n) out[i] = s[tid] - v;
    if (tid == BLOCK - 1 && blockSums) blockSums[blockIdx.x] = s[BLOCK - 1];
}

__global__ void kScanAddOffsets(int* out, int n, const int* blockSums) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] += blockSums[blockIdx.x];
}

__global__ void kFinalizeScan(const int* counts, int* starts, int n) {
    if (blockIdx.x == 0 && threadIdx.x == 0)
        starts[n] = starts[n - 1] + counts[n - 1];
}

__global__ void kCopyInt(int* dst, const int* src, int n) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) dst[i] = src[i];
}

__global__ void kScatter(const int* cellOf, int total, int* cursor, int* sorted) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= total) return;
    int slot = atomicAdd(&cursor[cellOf[i]], 1);
    sorted[slot] = i;
}

// Atomic scatter only randomises the order *within* a cell. Sorting those
// short cell slices (typically a handful of particles) makes every neighbor
// traversal deterministic for a fraction of the cost of sorting each
// particle's 30-50 entry neighbor list afterwards.
__global__ void kSortCells(const int* cellStart, int ncells, int* sorted) {
    const int c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= ncells) return;
    const int begin = cellStart[c], end = cellStart[c + 1];
    for (int i = begin + 1; i < end; ++i) {
        const int v = sorted[i];
        int j = i - 1;
        while (j >= begin && sorted[j] > v) { sorted[j + 1] = sorted[j]; --j; }
        sorted[j + 1] = v;
    }
}

__device__ __forceinline__ void insertionSort(int* a, int n) {
    for (int i = 1; i < n; ++i) {
        int v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; --j; }
        a[j + 1] = v;
    }
}

__global__ void kNeighbors(const float4* pos, const float4* bpos, int n, int nb,
                           float ox, float oy, float oz, float invh,
                           int gw, int gh, int gd, float h,
                           const int* cellStart, const int* sorted,
                           const float* bvol, float4* nbGeom, float* nbDist,
                           float* deltaB, float4* gradB,
                           int* nbCount, int* bnbIdx, int* bnbCount,
                           int* maxNeighbors) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float3 pi = make_float3(pos[i].x, pos[i].y, pos[i].z);
    float3 c = cellCoords(pi, ox, oy, oz, invh, gw, gh, gd);
    int cx = (int)c.x, cy = (int)c.y, cz = (int)c.z;
    int nbound = 0;
    float dsum = 0.f, gx = 0.f, gy = 0.f, gz = 0.f;
    const float h2 = h * h;
    for (int dz = -1; dz <= 1; ++dz) {
        int z = cz + dz;
        if (z < 0 || z >= gd) continue;
        for (int dy = -1; dy <= 1; ++dy) {
            int y = cy + dy;
            if (y < 0 || y >= gh) continue;
            for (int dx = -1; dx <= 1; ++dx) {
                int x = cx + dx;
                if (x < 0 || x >= gw) continue;
                int cell = cellIndex(x, y, z, gw, gh);
                int s = cellStart[cell], e = cellStart[cell + 1];
                for (int k = s; k < e; ++k) {
                    const int j = sorted[k];
                    // Each fluid-fluid pair is emitted once (j > i) and
                    // mirrored into both lists. Boundary pairs are owned by
                    // their fluid particle only.
                    if (j < n && j <= i) continue;
                    float3 pj = (j < n)
                        ? make_float3(pos[j].x, pos[j].y, pos[j].z)
                        : make_float3(bpos[j - n].x, bpos[j - n].y, bpos[j - n].z);
                    float rx = pi.x - pj.x, ry = pi.y - pj.y, rz = pi.z - pj.z;
                    float rr2 = rx * rx + ry * ry + rz * rz;
                    if (rr2 >= h2) continue;
                    if (j < n) {
                        const float rr = sqrtf(rr2);
                        const float4 g = kernelGradW(make_float3(rx, ry, rz), h);
                        const int si = atomicAdd(&nbCount[i], 1);
                        if (si < MAXNB) {
                            nbGeom[si * n + i] = make_float4(g.x, g.y, g.z, __int_as_float(j));
                            nbDist[si * n + i] = rr;
                        }
                        const int sj = atomicAdd(&nbCount[j], 1);
                        if (sj < MAXNB) {
                            nbGeom[sj * n + j] = make_float4(-g.x, -g.y, -g.z, __int_as_float(i));
                            nbDist[sj * n + j] = rr;
                        }
                    } else {
                        const int slot = nbound++;
                        if (slot < MAXBNB) {
                            const int b = j - n;
                            const float rr = sqrtf(rr2);
                            const float4 g = kernelGradW(make_float3(rx, ry, rz), h);
                            const float vb = bvol[b];
                            bnbIdx[slot * n + i] = b;
                            dsum += vb * kernelW(rr, h);
                            gx += vb * g.x; gy += vb * g.y; gz += vb * g.z;
                        }
                    }
                }
            }
        }
    }
    deltaB[i] = dsum;
    gradB[i] = make_float4(gx, gy, gz, 0.f);
    bnbCount[i] = min(nbound, MAXBNB);
}

// Clamp overflowed counts before any solver pass loops over them, and retain
// the true maximum for diagnostics.
__global__ void kClampCounts(int* nbCount, int* bnbCount, int n, int* maxNeighbors) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const int c = nbCount[i];
    atomicMax(maxNeighbors, c);
    nbCount[i] = min(c, MAXNB);
    bnbCount[i] = min(bnbCount[i], MAXBNB);
}

// Boundary density/gradient sums (Akinci 2012), recomputed from bpos.
__global__ void kBoundarySums(const float4* pos, const float4* bpos,
                              const int* bnbIdx, const int* bnbCount, const float* bvol,
                              int n, float h, float* deltaB, float4* gradB) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float3 pi = make_float3(pos[i].x, pos[i].y, pos[i].z);
    float d = 0.f, gx = 0.f, gy = 0.f, gz = 0.f;
    int cb = bnbCount[i];
    for (int k = 0; k < cb; ++k) {
        int b = __ldg(&bnbIdx[k * n + i]);
        float3 pb = make_float3(bpos[b].x, bpos[b].y, bpos[b].z);
        float3 r = make_float3(pi.x - pb.x, pi.y - pb.y, pi.z - pb.z);
        float rr = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z);
        float4 g = kernelGradW(r, h);
        float vb = bvol[b];
        d += vb * kernelW(rr, h);
        gx += vb * g.x; gy += vb * g.y; gz += vb * g.z;
    }
    deltaB[i] = d;
    gradB[i] = make_float4(gx, gy, gz, 0.f);
}

__global__ void kScaleCopyF(float* dst, const float* src, int n, float s) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    dst[i] = src[i] * s;
}

// ---------------------------------------------------------------------------
// Predict
// ---------------------------------------------------------------------------
__global__ void kDensity(const float4* nbGeom, const float* nbDist, const int* nbCount, int n,
                         float vol, float h, const float* deltaB, float* rho, float* invRho2) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float rhoN = vol * kernelW(0.0f, h);
    int c = nbCount[i];
    for (int k = 0; k < c; ++k) {
        rhoN += vol * kernelW(__ldg(&nbDist[k * n + i]), h);
    }
    rhoN += deltaB[i];
    rho[i] = rhoN;
    invRho2[i] = vol / fmaxf(rhoN * rhoN, 1e-12f);
}

// DFSPH needs rho and its stiffness factor together. Computing both in one
// neighbor traversal also refreshes invRho2 for the IISPH pressure path.
__global__ void kDensityFactor(const float4* nbGeom, const float* nbDist, const int* nbCount,
                               const float4* gradB, const float* deltaB, int n, float vol,
                               float h, float* rho, float* invRho2, float4* gradPI,
                               float* alpha) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float rhoN = vol * kernelW(0.0f, h);
    const float4 gb = gradB[i];
    float gx = gb.x, gy = gb.y, gz = gb.z;
    float sum = 0.f;
    const int c = nbCount[i];
    for (int k = 0; k < c; ++k) {
        const float4 g = __ldg(&nbGeom[k * n + i]);
        rhoN += vol * kernelW(__ldg(&nbDist[k * n + i]), h);
        const float px = vol * g.x, py = vol * g.y, pz = vol * g.z;
        gx += px; gy += py; gz += pz;
        sum += px * px + py * py + pz * pz;
    }
    rhoN += deltaB[i];
    sum += gx * gx + gy * gy + gz * gz;
    rho[i] = rhoN;
    invRho2[i] = vol / fmaxf(rhoN * rhoN, 1e-12f);
    gradPI[i] = make_float4(gx, gy, gz, 0.f);
    alpha[i] = (sum > 1e-6f) ? 1.0f / sum : 0.0f;
}

__global__ void kDii(const float4* nbGeom, const int* nbCount,
                     const float4* gradB, const float* rho, int n, float vol, float h,
                     float4* dii) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float gx = gradB[i].x, gy = gradB[i].y, gz = gradB[i].z;
    int c = nbCount[i];
    for (int k = 0; k < c; ++k) {
        const float4 g = __ldg(&nbGeom[k * n + i]);
        gx += vol * g.x; gy += vol * g.y; gz += vol * g.z;
    }
    float inv = -1.0f / (rho[i] * rho[i]);
    dii[i] = make_float4(gx * inv, gy * inv, gz * inv, 0.f);
}

__global__ void kNormalsIdx(const float4* nbGeom, const int* nbCount,
                            const float* rho, int n, float vol, float h, float4* normals) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    int c = nbCount[i];
    float nx = 0.f, ny = 0.f, nz = 0.f;
    for (int k = 0; k < c; ++k) {
        const float4 g = __ldg(&nbGeom[k * n + i]);
        const int j = neighborIndex(g);
        if (rho[j] < 1e-6f) continue;
        float s = vol / rho[j];
        nx += s * g.x; ny += s * g.y; nz += s * g.z;
    }
    normals[i] = make_float4(nx * h, ny * h, nz * h, 0.f);
}

__global__ void kForcesFull(const float4* pos, const float4* vel, const float4* bpos,
                            const int* nbCount, const float4* nbGeom,
                            const float* nbDist, const int* bnbIdx, const int* bnbCount,
                            const float4* normals, const float* rho, const float* bvol, int n,
                            float vol, float h, float dt, float gravity, float viscosity,
                            float tension, float tensionB, float rho0, float4* vadv) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float3 pi = make_float3(pos[i].x, pos[i].y, pos[i].z);
    float3 vi = make_float3(vel[i].x, vel[i].y, vel[i].z);
    float dvx = 0.f, dvy = 0.f, dvz = 0.f;
    float ax = 0.f, ay = 0.f, az = 0.f;
    int c = nbCount[i];
    for (int k = 0; k < c; ++k) {
        const float4 geom = __ldg(&nbGeom[k * n + i]);
        const int j = neighborIndex(geom);
        float3 pj = make_float3(pos[j].x, pos[j].y, pos[j].z);
        float rx = pi.x - pj.x, ry = pi.y - pj.y, rz = pi.z - pj.z;
        float rr = __ldg(&nbDist[k * n + i]);
        if (viscosity > 0.0f && rho[j] >= 1e-6f) {
            float s = viscosity * (vol / rho[j]) * kernelW(rr, h);
            dvx -= s * (vi.x - vel[j].x);
            dvy -= s * (vi.y - vel[j].y);
            dvz -= s * (vi.z - vel[j].z);
        }
        if (tension > 0.0f && rr >= 1e-9f) {
            float inv = 1.0f / rr;
            float xhx = rx * inv, xhy = ry * inv, xhz = rz * inv;
            float wc = wCohesion(rr, h);
            float axx = -tension * (rho0 * vol) * xhx * wc;
            float ayy = -tension * (rho0 * vol) * xhy * wc;
            float azz = -tension * (rho0 * vol) * xhz * wc;
            axx -= tension * (normals[i].x - normals[j].x);
            ayy -= tension * (normals[i].y - normals[j].y);
            azz -= tension * (normals[i].z - normals[j].z);
            float K = 2.0f / (rho[i] + rho[j]);
            ax += K * axx; ay += K * ayy; az += K * azz;
        }
    }
    int cb = bnbCount[i];
    if (tension > 0.0f) {
        for (int k = 0; k < cb; ++k) {
            int b = __ldg(&bnbIdx[k * n + i]);
            float rx = pi.x - bpos[b].x, ry = pi.y - bpos[b].y, rz = pi.z - bpos[b].z;
            float rr = sqrtf(rx * rx + ry * ry + rz * rz);
            if (rr < 1e-9f) continue;
            float inv = 1.0f / rr;
            float s = tensionB * rho0 * bvol[b] * wAdhesion(rr, h) * inv;
            ax -= s * rx; ay -= s * ry; az -= s * rz;
        }
    }
    vadv[i] = make_float4(vi.x + ax * dt + dvx,
                          vi.y + dt * gravity + ay * dt + dvy,
                          vi.z + az * dt + dvz, 0.f);
}

__global__ void kRhoAdvAii(const float4* pos, const int* nbCount,
                           const float4* nbGeom, const float4* dii, const float4* gradB,
                           const float4* vadv, const float* rho, const float* invRho2,
                           const float* pPrev, int n, float vol, float h, float dt,
                           float* rhoAdv, float* p, float2* ps) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float3 vi = make_float3(vadv[i].x, vadv[i].y, vadv[i].z);
    float rhoA = rho[i];
    int c = nbCount[i];
    for (int k = 0; k < c; ++k) {
        float4 g = __ldg(&nbGeom[k * n + i]);
        int j = neighborIndex(g);
        rhoA += dt * vol * ((vi.x - vadv[j].x) * g.x +
                            (vi.y - vadv[j].y) * g.y +
                            (vi.z - vadv[j].z) * g.z);
    }
    rhoA += dt * (vi.x * gradB[i].x + vi.y * gradB[i].y + vi.z * gradB[i].z);
    rhoAdv[i] = rhoA;
    p[i] = 0.5f * pPrev[i];
    ps[i] = make_float2(p[i], invRho2[i]);
}

__global__ void kAii(const float4* nbGeom, const int* nbCount,
                     const float4* dii, const float4* gradB, const float* rho,
                     int n, float vol, float h, float* aii) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float4 d = dii[i];
    float coef = vol / (rho[i] * rho[i]);
    float a = 0.f;
    int c = nbCount[i];
    for (int k = 0; k < c; ++k) {
        float4 g = __ldg(&nbGeom[k * n + i]);
        float ix = d.x - g.x * coef;
        float iy = d.y - g.y * coef;
        float iz = d.z - g.z * coef;
        a += vol * (ix * g.x + iy * g.y + iz * g.z);
    }
    a += d.x * gradB[i].x + d.y * gradB[i].y + d.z * gradB[i].z;
    aii[i] = a;
}

// ---------------------------------------------------------------------------
// IISPH pressure solve
// ---------------------------------------------------------------------------
__global__ void __launch_bounds__(BLOCK, 4) kPressureSIdx(
        const float4* __restrict__ nbGeom, const int* __restrict__ nbCount,
        const float2* __restrict__ ps, int n, float4* __restrict__ S) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float sx = 0.f, sy = 0.f, sz = 0.f;
    int c = nbCount[i];
    for (int k = 0; k < c; ++k) {
        float4 g = __ldg(&nbGeom[k * n + i]);
        int j = neighborIndex(g);
        const float2 q = __ldg(&ps[j]);       // (pressure, V/rho^2)
        float s = -q.y * q.x;
        sx += g.x * s; sy += g.y * s; sz += g.z * s;
    }
    // Pack p_i into S.w so the next kernel needs only S[j] for p_j.
    S[i] = make_float4(sx, sy, sz, __ldg(&ps[i]).x);
}

__global__ void __launch_bounds__(BLOCK, 4) kPressureP(
        const float4* __restrict__ nbGeom, const int* __restrict__ nbCount,
        const float4* __restrict__ dii, const float4* __restrict__ S,
        const float4* __restrict__ gradB, float2* __restrict__ ps,
        const float* __restrict__ rhoAdv, const float* __restrict__ aii,
        int n, float vol, float h, float dt, float omega,
        float* __restrict__ pOut, float* __restrict__ blkErr,
        float* __restrict__ blkAbs, float* __restrict__ blkMax) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    float errSum = 0.f, absSum = 0.f, maxAbs = 0.f;
    if (i < n) {
        const float dt2 = dt * dt;
        const float b = 1.0f - rhoAdv[i];
        float4 si = S[i];
        const float2 psi = ps[i];
        const float pi = psi.x;
        float coefJi = psi.y * pi;
        float sum = 0.f;
        int c = nbCount[i];
        for (int k = 0; k < c; ++k) {
            const float4 g = __ldg(&nbGeom[k * n + i]);
            const int j = neighborIndex(g);
            float4 sj = __ldg(&S[j]);
            float4 dj = __ldg(&dii[j]);
            float pjv = sj.w;
            float ijx = si.x - dj.x * pjv - (sj.x - g.x * coefJi);
            float ijy = si.y - dj.y * pjv - (sj.y - g.y * coefJi);
            float ijz = si.z - dj.z * pjv - (sj.z - g.z * coefJi);
            sum += vol * (ijx * g.x + ijy * g.y + ijz * g.z);
        }
        sum += si.x * gradB[i].x + si.y * gradB[i].y + si.z * gradB[i].z;
        float pnew = 0.f;
        const float denom = aii[i] * dt2;
        if (fabsf(denom) > 3e-5f)
            pnew = (1.0f - omega) * pi + omega / denom * (b - dt2 * sum);
        if (pnew < 0.0f) pnew = 0.0f;
        if (pnew > 100.0f) pnew = 100.0f;
        pOut[i] = pnew;
        ps[i].x = pnew;
        if (pnew != 0.0f) {
            float res = (aii[i] * pnew + sum) * dt2 - b;
            errSum = res;
            absSum = fabsf(res);
            maxAbs = fabsf(res);
        }
    }
    __shared__ float wErr[WARPS], wAbs[WARPS], wMax[WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    errSum = warpSum(errSum); absSum = warpSum(absSum); maxAbs = warpMax(maxAbs);
    if (lane == 0) { wErr[wid] = errSum; wAbs[wid] = absSum; wMax[wid] = maxAbs; }
    __syncthreads();
    if (wid == 0) {
        errSum = (lane < WARPS) ? wErr[lane] : 0.f;
        absSum = (lane < WARPS) ? wAbs[lane] : 0.f;
        maxAbs = (lane < WARPS) ? wMax[lane] : 0.f;
        errSum = warpSum(errSum); absSum = warpSum(absSum); maxAbs = warpMax(maxAbs);
        if (lane == 0) {
            blkErr[blockIdx.x] = errSum;
            blkAbs[blockIdx.x] = absSum;
            blkMax[blockIdx.x] = maxAbs;
        }
    }
}

__global__ void kReduceFinal(const float* blockVals, int nblocks, float* out) {
    __shared__ float w[WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    float v = 0.f;
    for (int i = tid; i < nblocks; i += BLOCK) v += blockVals[i];
    v = warpSum(v);
    if (lane == 0) w[wid] = v;
    __syncthreads();
    if (wid == 0) {
        v = (lane < WARPS) ? w[lane] : 0.f;
        v = warpSum(v);
        if (lane == 0) out[0] = v;
    }
}

__global__ void kReduce3Final(const float* e, const float* a, const float* m,
                              int nblocks, float* out) {
    __shared__ float wE[WARPS], wA[WARPS], wM[WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    float ve = 0.f, va = 0.f, vm = 0.f;
    for (int i = tid; i < nblocks; i += BLOCK) {
        ve += e[i];
        va += a[i];
        vm = fmaxf(vm, m[i]);
    }
    ve = warpSum(ve); va = warpSum(va); vm = warpMax(vm);
    if (lane == 0) { wE[wid] = ve; wA[wid] = va; wM[wid] = vm; }
    __syncthreads();
    if (wid == 0) {
        ve = (lane < WARPS) ? wE[lane] : 0.f;
        va = (lane < WARPS) ? wA[lane] : 0.f;
        vm = (lane < WARPS) ? wM[lane] : 0.f;
        ve = warpSum(ve); va = warpSum(va); vm = warpMax(vm);
        if (lane == 0) {
            out[0] = ve;
            out[1] = va;
            out[2] = vm;
        }
    }
}

__global__ void kIntegrate(const float4* dii, const float4* S, const float* p,
                           const float4* vadv, const float4* pos, int n,
                           float dt, float wx0, float wx1, float wy1, float wz0, float wz1,
                           int wallTop, float half, float4* vel, float4* posOut, float* pPrev) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float4 d = dii[i];
    float4 s = S[i];
    float pi = p[i];
    float4 va = vadv[i];
    float4 po = pos[i];
    float3 v = make_float3(va.x + (d.x * pi + s.x) * dt,
                           va.y + (d.y * pi + s.y) * dt,
                           va.z + (d.z * pi + s.z) * dt);
    float3 x = make_float3(po.x + v.x * dt, po.y + v.y * dt, po.z + v.z * dt);
    if (x.x < wx0 + half) { x.x = wx0 + half; if (v.x < 0.f) v.x = 0.f; }
    if (x.x > wx1 - half) { x.x = wx1 - half; if (v.x > 0.f) v.x = 0.f; }
    if (x.y < half)       { x.y = half;       if (v.y < 0.f) v.y = 0.f; }
    if (wallTop && x.y > wy1 - half) { x.y = wy1 - half; if (v.y > 0.f) v.y = 0.f; }
    if (x.z < wz0 + half) { x.z = wz0 + half; if (v.z < 0.f) v.z = 0.f; }
    if (x.z > wz1 - half) { x.z = wz1 - half; if (v.z > 0.f) v.z = 0.f; }
    vel[i] = make_float4(v.x, v.y, v.z, 0.f);
    posOut[i] = make_float4(x.x, x.y, x.z, 0.f);
    pPrev[i] = pi;
}

__global__ void kClampV(float4* vel, int n, float vmax) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float4 v = vel[i];
    float speed = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (speed > vmax) {
        float s = vmax / speed;
        vel[i] = make_float4(v.x * s, v.y * s, v.z * s, 0.f);
    }
}

__global__ void kVMag(const float4* vel, int n, float* blockVals) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    float v = 0.f;
    if (i < n) v = sqrtf(vel[i].x * vel[i].x + vel[i].y * vel[i].y + vel[i].z * vel[i].z);
    __shared__ float w[WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    v = warpMax(v);
    if (lane == 0) w[wid] = v;
    __syncthreads();
    if (wid == 0) {
        v = (lane < WARPS) ? w[lane] : 0.f;
        v = warpMax(v);
        if (lane == 0) blockVals[blockIdx.x] = v;
    }
}

__global__ void kReduceMaxFinal(const float* blockVals, int nblocks, float* out) {
    __shared__ float w[WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    float v = 0.f;
    for (int i = tid; i < nblocks; i += BLOCK) v = fmaxf(v, blockVals[i]);
    v = warpMax(v);
    if (lane == 0) w[wid] = v;
    __syncthreads();
    if (wid == 0) {
        v = (lane < WARPS) ? w[lane] : 0.f;
        v = warpMax(v);
        if (lane == 0) out[0] = v;
    }
}

__global__ void kColors(const float4* vel, int n, const float* vmaxPtr, float* colors) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float4 v = vel[i];
    float speed = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    const float vmax = fmaxf(*vmaxPtr, 1e-6f);
    float s = speed / vmax;
    if (s > 1.0f) s = 1.0f;
    colors[3 * i + 0] = 0.15f + 0.85f * s;
    colors[3 * i + 1] = 0.35f + 0.65f * s;
    colors[3 * i + 2] = 1.0f;
}

__global__ void kMeanRho(const float* rho, int n, float* blockVals) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    float v = (i < n) ? rho[i] : 0.f;
    __shared__ float w[WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    v = warpSum(v);
    if (lane == 0) w[wid] = v;
    __syncthreads();
    if (wid == 0) {
        v = (lane < WARPS) ? w[lane] : 0.f;
        v = warpSum(v);
        if (lane == 0) blockVals[blockIdx.x] = v;
    }
}

// ---------------------------------------------------------------------------
// DFSPH
// ---------------------------------------------------------------------------
__device__ __forceinline__ float rhoDotOf(int i, const float4* __restrict__ nbGeom,
                                          const int* __restrict__ nbCount,
                                          const float4* __restrict__ vadv,
                                          const float4* __restrict__ gradB, float vol, int n) {
    const float4 vi4 = vadv[i];
    const float4 gb = gradB[i];
    float dx = vi4.x * gb.x, dy = vi4.y * gb.y, dz = vi4.z * gb.z;
    const int c = nbCount[i];
    for (int k = 0; k < c; ++k) {
        const float4 g = __ldg(&nbGeom[k * n + i]);
        const int j = neighborIndex(g);
        const float4 vj = __ldg(&vadv[j]);
        dx += vol * (vi4.x - vj.x) * g.x;
        dy += vol * (vi4.y - vj.y) * g.y;
        dz += vol * (vi4.z - vj.z) * g.z;
    }
    return dx + dy + dz;
}

__global__ void kDFSPHInit(const float4* nbGeom, const int* nbCount,
                           const int* bnbCount, const float4* vadv, const float4* gradB,
                           const float* rho, const float* alpha, const float* pPrev,
                           int n, float vol, float h, float dt, int warmStart,
                           float* rhoAdv, float* p) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float rhoA = rho[i] + dt * rhoDotOf(i, nbGeom, nbCount, vadv, gradB, vol, n);
    rhoAdv[i] = rhoA;
    float invH2 = 1.0f / (dt * dt);
    if (warmStart) p[i] = (rhoA > 1.0f) ? fminf(pPrev[i], 0.00025f) * invH2 : 0.0f;
    else p[i] = fmaxf(rhoA - 1.0f, 0.0f) * alpha[i] * invH2;
}

__global__ void kDFSPHVelocityUpdate(const float4* __restrict__ nbGeom,
                                     const int* __restrict__ nbCount,
                                     const float4* __restrict__ gradB,
                                     const float* __restrict__ p,
                                     int n, float vol, float h, float dt, float omega,
                                     float4* __restrict__ vadv) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float pi = p[i];
    const float4 gb = gradB[i];
    float ax = -pi * gb.x, ay = -pi * gb.y, az = -pi * gb.z;
    const int c = nbCount[i];
    for (int k = 0; k < c; ++k) {
        const float4 g = __ldg(&nbGeom[k * n + i]);
        const int j = neighborIndex(g);
        const float s = vol * (pi + __ldg(&p[j]));
        ax -= s * g.x; ay -= s * g.y; az -= s * g.z;
    }
    const float4 v = vadv[i];
    vadv[i] = make_float4(v.x + omega * dt * ax, v.y + omega * dt * ay, v.z + omega * dt * az, 0.f);
}

__global__ void kDFSPHRhoAdvUpdateFull(const float4* __restrict__ nbGeom,
                                       const int* __restrict__ nbCount,
                                       const float4* __restrict__ vadv,
                                       const float4* __restrict__ gradB,
                                       const float* __restrict__ rho,
                                       const float* __restrict__ alpha,
                                       int n, float vol, float dt,
                                       float* __restrict__ rhoAdv, float* __restrict__ p,
                                       float* __restrict__ blkErr, float* __restrict__ blkAbs,
                                       float* __restrict__ blkMax) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    float e = 0.f, ae = 0.f, me = 0.f;
    if (i < n) {
        float rhoA = rho[i] + dt * rhoDotOf(i, nbGeom, nbCount, vadv, gradB, vol, n);
        rhoAdv[i] = rhoA;
        float invH2 = 1.0f / (dt * dt);
        p[i] = fmaxf(rhoA - 1.0f, 0.0f) * alpha[i] * invH2;
        e = rhoA - 1.0f;
        ae = fabsf(e);
        me = ae;
    }
    __shared__ float wE[WARPS], wA[WARPS], wM[WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    e = warpSum(e); ae = warpSum(ae); me = warpMax(me);
    if (lane == 0) { wE[wid] = e; wA[wid] = ae; wM[wid] = me; }
    __syncthreads();
    if (wid == 0) {
        e = (lane < WARPS) ? wE[lane] : 0.f;
        ae = (lane < WARPS) ? wA[lane] : 0.f;
        me = (lane < WARPS) ? wM[lane] : 0.f;
        e = warpSum(e); ae = warpSum(ae); me = warpMax(me);
        if (lane == 0) {
            blkErr[blockIdx.x] = e;
            blkAbs[blockIdx.x] = ae;
            blkMax[blockIdx.x] = me;
        }
    }
}

__global__ void kDFSPHIntegratePositions(const float4* vadv, const float4* pos, int n,
                                         float dt, float wx0, float wx1, float wy1,
                                         float wz0, float wz1, int wallTop, float half,
                                         float4* posOut, float4* velOut) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float4 v = vadv[i];
    float4 po = pos[i];
    float3 x = make_float3(po.x + v.x * dt, po.y + v.y * dt, po.z + v.z * dt);
    if (x.x < wx0 + half) { x.x = wx0 + half; if (v.x < 0.f) v.x = 0.f; }
    if (x.x > wx1 - half) { x.x = wx1 - half; if (v.x > 0.f) v.x = 0.f; }
    if (x.y < half)       { x.y = half;       if (v.y < 0.f) v.y = 0.f; }
    if (wallTop && x.y > wy1 - half) { x.y = wy1 - half; if (v.y > 0.f) v.y = 0.f; }
    if (x.z < wz0 + half) { x.z = wz0 + half; if (v.z < 0.f) v.z = 0.f; }
    if (x.z > wz1 - half) { x.z = wz1 - half; if (v.z > 0.f) v.z = 0.f; }
    posOut[i] = make_float4(x.x, x.y, x.z, 0.f);
    velOut[i] = v;
}

__global__ void kDFSPHKappaInit(const float4* nbGeom, const int* nbCount,
                                const int* bnbCount, const float4* vadv, const float4* gradB,
                                int n, float vol, float invH, int warmStart,
                                const float* kappaPrev, float* kappaV, float* seed,
                                float* blockVals) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    float v = 0.f;
    if (i < n) {
        if (nbCount[i] + bnbCount[i] < 20) {
            kappaV[i] = 0.f;
            seed[i] = 0.f;
        } else {
            v = rhoDotOf(i, nbGeom, nbCount, vadv, gradB, vol, n);
            kappaV[i] = v;
            seed[i] = warmStart ? fminf(kappaPrev[i], 0.00025f) * invH : 0.f;
        }
    }
    __shared__ float w[WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    v = warpSum(v);
    if (lane == 0) w[wid] = v;
    __syncthreads();
    if (wid == 0) {
        v = (lane < WARPS) ? w[lane] : 0.f;
        v = warpSum(v);
        if (lane == 0) blockVals[blockIdx.x] = v;
    }
}

__global__ void kDFSPHKappaSeed(const int* nbCount, const int* bnbCount, const float* kappaPrev,
                                int n, float invH, int warmStart, float* seed) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    if (!warmStart) { seed[i] = 0.f; return; }
    int poor = (nbCount[i] + bnbCount[i] < 20) ? 1 : 0;
    seed[i] = poor ? 0.f : fminf(kappaPrev[i], 0.00025f) * invH;
}

__global__ void kDFSPHKappaScale(const float* alpha, int n, float invH, int iter, int warmStart,
                                 const float* seed, float* kappaV) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    if (iter == 1 && warmStart) kappaV[i] = seed[i];
    else kappaV[i] *= alpha[i] * invH;
}

__global__ void kDFSPHVelocityUpdateV(const float4* __restrict__ nbGeom,
                                      const int* __restrict__ nbCount,
                                      const float4* __restrict__ gradB,
                                      const float* __restrict__ kappaV,
                                      int n, float vol, float dt, float omega,
                                      float4* __restrict__ vadv) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float ki = kappaV[i];
    const float4 gb = gradB[i];
    float ax = -ki * gb.x, ay = -ki * gb.y, az = -ki * gb.z;
    const int c = nbCount[i];
    for (int k = 0; k < c; ++k) {
        const float4 g = __ldg(&nbGeom[k * n + i]);
        const int j = neighborIndex(g);
        const float s = vol * (ki + __ldg(&kappaV[j]));
        ax -= s * g.x; ay -= s * g.y; az -= s * g.z;
    }
    const float4 v = vadv[i];
    vadv[i] = make_float4(v.x + omega * dt * ax, v.y + omega * dt * ay, v.z + omega * dt * az, 0.f);
}

__global__ void kDFSPHRhoDotUpdate(const float4* nbGeom, const int* nbCount,
                                   const int* bnbCount, const float4* vadv, const float4* gradB,
                                   int n, float vol, float h, float* kappaV, float* blockVals) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    float v = 0.f;
    if (i < n) {
        if (nbCount[i] + bnbCount[i] < 20) {
            kappaV[i] = 0.f;
        } else {
            float rhoDot = rhoDotOf(i, nbGeom, nbCount, vadv, gradB, vol, n);
            kappaV[i] = rhoDot;
            v = rhoDot;
        }
    }
    __shared__ float w[WARPS];
    const int tid = threadIdx.x, lane = tid & 31, wid = tid >> 5;
    v = warpSum(v);
    if (lane == 0) w[wid] = v;
    __syncthreads();
    if (wid == 0) {
        v = (lane < WARPS) ? w[lane] : 0.f;
        v = warpSum(v);
        if (lane == 0) blockVals[blockIdx.x] = v;
    }
}

__global__ void kDFSPHFinalize(const float4* vadv, float4* vel, int n, float vmax,
                               const float* kappaV, const float* alpha, int warmStart,
                               float* prev) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float4 v = vadv[i];
    const float speed = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (speed > vmax) {
        const float s = vmax / speed;
        v.x *= s; v.y *= s; v.z *= s;
    }
    vel[i] = v;
    if (warmStart) prev[i] = kappaV[i] * alpha[i];
}

__global__ void kZeroInt(int* dst, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    dst[i] = 0;
}
)SPHK";

}  // namespace sphkernels
