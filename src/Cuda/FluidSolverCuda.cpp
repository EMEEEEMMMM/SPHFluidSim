#include "Cuda/FluidSolverCuda.hpp"

#if defined(SPH_HAS_CUDA)

#include "CudaDriverApi.hpp"
#include "SphKernels.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kBlock = 256;   // must match BLOCK in SphKernels.hpp
constexpr int kMaxNb = 64;    // must match MAXNB
constexpr int kMaxBnb = 48;   // must match MAXBNB
constexpr float kPi = 3.14159265358979323846f;

struct F4 { float x, y, z, w; };

float hostW(float r, float h) {
    float q = r / h;
    if (q > 1.0f) return 0.0f;
    const float c = 8.0f / (kPi * h * h * h);
    if (q <= 0.5f) return c * (1.0f - 6.0f * q * q + 6.0f * q * q * q);
    const float t = 1.0f - q;
    return c * 2.0f * t * t * t;
}

void ThrowIf(CUresult r, const char* what) {
    if (r == CUDA_SUCCESS) return;
    const auto& api = sphcuda::Api();
    const char* name = nullptr;
    if (api.GetErrorName) {
        const char* tmp = nullptr;
        if (api.GetErrorName(r, &tmp) == CUDA_SUCCESS && tmp) name = tmp;
    }
    if (!name) name = "CUDA_ERROR";
    char msg[512];
    std::snprintf(msg, sizeof(msg), "CUDA error in %s: %s (%d)", what, name, (int)r);
    throw std::runtime_error(msg);
}

}  // namespace

struct FluidSolverCuda::Impl {
    sphcuda::DriverApi* api = nullptr;
    CUdevice device = 0;
    CUcontext ctx = nullptr;
    CUmodule module = nullptr;
    CUstream stream = nullptr;
    void* hostPinned = nullptr;
    std::string deviceName;
    struct KernelTiming { double ms = 0.0; int calls = 0; };
    std::unordered_map<CUfunction, std::string> kernelNames;
    std::unordered_map<std::string, KernelTiming> kernelTimings;
    const bool profileKernels = std::getenv("SPH_CUDA_KERNEL_PROFILE") != nullptr;
    const int profileSkip = profileKernels && std::getenv("SPH_CUDA_KERNEL_PROFILE_SKIP")
        ? std::atoi(std::getenv("SPH_CUDA_KERNEL_PROFILE_SKIP")) : 0;
    int stepCounter = 0;

    struct Kernels {
        CUfunction bounds{}, boundsFinal{};
        CUfunction cellOf{}, countCells{}, scanBlocks{}, scanAddOffsets{}, finalizeScan{}, copyInt{};
        CUfunction scatter{}, sortCells{}, neighbors{}, clampCounts{};
        CUfunction density{}, densityFactor{}, dii{}, normals{}, forces{}, rhoAdvAii{}, aii{};
        CUfunction pressureS{}, pressureP{}, reduceFinal{}, reduce3Final{}, integrate{};
        CUfunction clampV{}, vmag{}, reduceMaxFinal{}, colors{}, meanRho{};
        CUfunction dfsphInit{}, dfsphVel{}, dfsphRhoAdv{};
        CUfunction dfsphIntegrate{}, dfsphKappaInit{};
        CUfunction dfsphKappaScale{}, dfsphVelV{};
        CUfunction dfsphRhoDot{}, dfsphFinalize{};
        CUfunction zeroInt{}, scaleCopyF{};
    } k;

    ~Impl() {
        if (!api || !api->loaded) return;
        if (ctx) api->CtxSetCurrent(ctx);
        if (ctx) api->CtxSynchronize();
        if (!kernelTimings.empty()) {
            std::vector<std::pair<std::string, KernelTiming>> rows(kernelTimings.begin(), kernelTimings.end());
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
            double total = 0.0;
            for (const auto& row : rows) total += row.second.ms;
            std::fprintf(stderr, "[kernel-profile] %s  total=%.3f ms\n", deviceName.c_str(), total);
            for (const auto& row : rows) {
                std::fprintf(stderr, "  %-28s %10.3f ms  %6d calls\n",
                             row.first.c_str(), row.second.ms, row.second.calls);
            }
        }
        if (stream && api->StreamDestroy) api->StreamDestroy(stream);
        if (module) api->ModuleUnload(module);
        for (CUdeviceptr p : {dPos, dVel, dVadv, dDii, dS, dGradB, dNormals,
                              dRho, dRhoAdv, dP, dPPrev, dPTmp, dAii, dAlpha, dInvRho2, dPS,
                              dDeltaB, dKappaV, dKappaVPrev, dKappaSeed, dColors,
                              dBpos, dBvol, dNbCount, dNbGeom, dNbDist, dBnb, dBnbCount,
                              dCellOf, dCellCount, dCellStart, dCursor, dSorted,
                              dBlkErr, dBlkAbs, dBlkMax, dBlkBounds, dRed, dMaxNb}) {
            if (p) api->MemFree(p);
        }
        for (CUdeviceptr p : dScanLevels) if (p) api->MemFree(p);
        if (hostPinned && api->MemFreeHost) api->MemFreeHost(hostPinned);
        if (ctx) api->DevicePrimaryCtxRelease(device);
    }

    FluidParams params;
    float h = 0.1f;
    float vol = 1.0f;
    float mass = 1.0f;

    std::vector<Math::Vector3> X, V, Vadv, BoundX;
    std::vector<float> Rho, P, PPrev, Colors, BoundV;
    std::vector<int> HostNbCount;
    std::vector<F4> hostF4;
    bool dirtyPos = true, dirtyVel = true, dirtyRho = true, dirtyP = true;
    bool dirtyColors = true, dirtyNb = true;
    int lastMaxNb = 0;

    bool useFullInit = false;
    bool useDamOpen = false;
    bool dropSpawned = false;
    bool inflow = false;
    bool drop = false;

    float boxX = 2.0f, boxY = 1.2f, boxZ = 1.0f, waterSize = 0.8f;
    float wallX0 = 0.0f, wallX1 = 2.0f, wallY1 = 1.2f, wallZ0 = 0.0f, wallZ1 = 1.0f;
    bool wallTop = true;

    float inflowGap = 0.3f, inflowRadius = 0.08f, inflowX = 1.0f, inflowZ = 0.5f;
    int inflowPerStep = 100, maxParticles = 8000;
    float inflowCounter = 0.0f;

    float dropPool = 0.4f, dropSize = 0.585f, dropGap = 0.15f;

    float time = 0.0f;
    float avgErr = 0.0f, avgAbsErr = 0.0f, maxErr = 0.0f;
    int lastIter = 0, lastIterV = 0;
    float lastPressureMs = 0.0f;
    std::vector<float> convTrace;
    bool traceNext = false;
    bool overflowWarned = false;

    CUdeviceptr dPos{}, dVel{}, dVadv{}, dDii{}, dS{}, dGradB{}, dNormals{};
    CUdeviceptr dRho{}, dRhoAdv{}, dP{}, dPPrev{}, dPTmp{}, dAii{}, dAlpha{}, dInvRho2{}, dPS{};
    CUdeviceptr dDeltaB{}, dKappaV{}, dKappaVPrev{}, dKappaSeed{};
    CUdeviceptr dColors{}, dBpos{}, dBvol{};
    CUdeviceptr dNbCount{}, dNbGeom{}, dNbDist{}, dBnb{}, dBnbCount{};
    CUdeviceptr dCellOf{}, dCellCount{}, dCellStart{}, dCursor{}, dSorted{};
    CUdeviceptr dBlkErr{}, dBlkAbs{}, dBlkMax{}, dBlkBounds{}, dRed{};
    CUdeviceptr dMaxNb{};
    std::vector<CUdeviceptr> dScanLevels;
    std::vector<size_t> scanCaps;

    size_t capN = 0, capB = 0, capCells = 0, capTotal = 0, capBlk = 0, capRed = 0;
    // Set when an allocation failed halfway. The affected buffers are in an
    // unknown state, so the next growth frees everything and starts over
    // instead of reusing stale pointers.
    bool buffersBroken = false;

    // Device bytes one fluid particle holds in steady state. Must follow the
    // allocation list in EnsureN(); used for the memory pre-flight.
    static constexpr size_t kFluidBytesPerParticle =
        7 * 16      // pos, vel, vadv, dii, S, gradB, normals
        + 12 * 4    // rho, rhoAdv, p, pPrev, pTmp, aii, alpha, invRho2,
                    // deltaB, kappaV, kappaVPrev, kappaSeed
        + 3 * 4     // colors
        + 2 * 4     // packed pressure (p, V/rho^2)
        + kMaxNb * 16 + kMaxNb * 4 + 4   // nbGeom, nbDist, nbCount
        + kMaxBnb * 4 + 4;                            // bnb, bnbCount

    size_t HeldDeviceBytes() const {
        return capN * kFluidBytesPerParticle + capB * 20 + capCells * 16 + capTotal * 4 +
               capBlk * 34 + capRed;
    }

    // Free every fluid/boundary buffer and forget the capacities. Used when a
    // previous allocation failed halfway, so no dangling size is reused.
    void DropAllBuffers() {
        for (CUdeviceptr* p : {&dPos, &dVel, &dVadv, &dDii, &dS, &dGradB, &dNormals,
                               &dRho, &dRhoAdv, &dP, &dPPrev, &dPTmp, &dAii, &dAlpha,
                               &dInvRho2, &dDeltaB, &dKappaV, &dKappaVPrev, &dKappaSeed,
                               &dColors, &dPS, &dNbGeom, &dNbDist, &dNbCount,
                               &dBnb, &dBnbCount, &dBpos, &dBvol, &dCellOf, &dCellCount,
                               &dCellStart, &dCursor, &dSorted, &dBlkErr, &dBlkAbs,
                               &dBlkMax, &dBlkBounds, &dRed}) {
            if (*p) api->MemFree(*p);
            *p = 0;
        }
        capN = capB = capCells = capTotal = capBlk = capRed = 0;
        buffersBroken = false;
    }

    // ------------------------------------------------------------------
    void Alloc(CUdeviceptr& p, size_t bytes, const char* tag) {
        if (p) api->MemFree(p);
        ThrowIf(api->MemAlloc(&p, bytes ? bytes : 1), tag);
    }

    void EnsureN(size_t n) {
        if (n <= capN && !buffersBroken) return;
        if (buffersBroken) DropAllBuffers();
        const size_t cap = std::max<size_t>(n, capN ? capN * 2 : n);
        try {
            for (CUdeviceptr* p : {&dPos, &dVel, &dVadv, &dDii, &dS, &dGradB, &dNormals})
                Alloc(*p, cap * sizeof(F4), "fluid vec4");
            for (CUdeviceptr* p : {&dRho, &dRhoAdv, &dP, &dPPrev, &dPTmp, &dAii, &dAlpha,
                                   &dInvRho2, &dDeltaB, &dKappaV, &dKappaVPrev, &dKappaSeed})
                Alloc(*p, cap * sizeof(float), "fluid scalar");
            Alloc(dColors, cap * 3 * sizeof(float), "colors");
            Alloc(dPS, cap * 2 * sizeof(float), "pressureScaled");
            Alloc(dNbGeom, cap * kMaxNb * sizeof(F4), "nbGeom");
            Alloc(dNbDist, cap * kMaxNb * sizeof(float), "nbDist");
            Alloc(dNbCount, cap * sizeof(int), "nbCount");
            Alloc(dBnb, cap * kMaxBnb * sizeof(int), "bnb");
            Alloc(dBnbCount, cap * sizeof(int), "bnbCount");
        } catch (...) {
            buffersBroken = true;
            throw;
        }
        capN = cap;
    }

    void EnsureB(size_t nb) {
        if (nb <= capB) return;
        const size_t cap = std::max<size_t>(nb, capB ? capB * 2 : nb);
        Alloc(dBpos, cap * sizeof(F4), "bpos");
        Alloc(dBvol, cap * sizeof(float), "bvol");
        capB = cap;
    }

    void EnsureScanLevels(int ncells) {
        std::vector<size_t> needed;
        int n = ncells;
        while (n > 1) {
            const int blocks = (n + kBlock - 1) / kBlock;
            needed.push_back((size_t)blocks * sizeof(int));
            n = blocks;
        }
        if (needed.size() > dScanLevels.size()) {
            dScanLevels.resize(needed.size(), 0);
            scanCaps.resize(needed.size(), 0);
        }
        for (size_t i = 0; i < needed.size(); ++i) {
            if (scanCaps[i] < needed[i]) {
                Alloc(dScanLevels[i], needed[i], "scan level");
                scanCaps[i] = needed[i];
            }
        }
    }

    void ScanLevel(CUdeviceptr in, int n, CUdeviceptr out, int level) {
        const int blocks = Blocks(n);
        CUdeviceptr sums = (blocks > 1) ? dScanLevels[level] : 0;
        const void* a[] = {&in, &n, &out, &sums};
        Launch(k.scanBlocks, n, a);
        if (blocks > 1) {
            ScanLevel(sums, blocks, sums, level + 1);
            const void* b[] = {&out, &n, &sums};
            Launch(k.scanAddOffsets, n, b);
        }
    }

    void ScanCells(int ncells) {
        if (ncells <= 0) return;
        EnsureScanLevels(ncells);
        ScanLevel(dCellCount, ncells, dCellStart, 0);
        const void* f[] = {&dCellCount, &dCellStart, &ncells};
        Launch(k.finalizeScan, 1, f);
        int total = ncells + 1;
        const void* c[] = {&dCursor, &dCellStart, &total};
        Launch(k.copyInt, total, c);
    }

    void EnsureBlocks(int blocks) {
        const size_t need = (size_t)blocks * sizeof(float);
        if (need <= capBlk) return;
        const size_t cap = std::max<size_t>(need, capBlk ? capBlk * 2 : need);
        Alloc(dBlkErr, cap, "blkErr");
        Alloc(dBlkAbs, cap, "blkAbs");
        Alloc(dBlkMax, cap, "blkMax");
        Alloc(dBlkBounds, cap * 6, "blkBounds");
        capBlk = cap;
    }

    void EnsureRed() {
        if (capRed >= 32) return;
        Alloc(dRed, 32, "red");
        capRed = 32;
    }

    void ToDev(CUdeviceptr dst, const void* src, size_t bytes) {
        if (!bytes) return;
        if (stream && hostPinned && bytes <= 256) {
            std::memcpy(hostPinned, src, bytes);
            ThrowIf(api->MemcpyHtoDAsync(dst, hostPinned, bytes, stream), "HtoD");
            ThrowIf(api->StreamSynchronize(stream), "stream sync");
        } else if (stream) {
            ThrowIf(api->MemcpyHtoDAsync(dst, src, bytes, stream), "HtoD");
            ThrowIf(api->StreamSynchronize(stream), "stream sync");
        } else {
            ThrowIf(api->MemcpyHtoD(dst, src, bytes), "HtoD");
        }
    }
    void FromDev(void* dst, CUdeviceptr src, size_t bytes) {
        if (!bytes) return;
        if (stream && hostPinned && bytes <= 256) {
            ThrowIf(api->MemcpyDtoHAsync(hostPinned, src, bytes, stream), "DtoH");
            ThrowIf(api->StreamSynchronize(stream), "stream sync");
            std::memcpy(dst, hostPinned, bytes);
        } else if (stream) {
            ThrowIf(api->MemcpyDtoHAsync(dst, src, bytes, stream), "DtoH");
            ThrowIf(api->StreamSynchronize(stream), "stream sync");
        } else {
            ThrowIf(api->MemcpyDtoH(dst, src, bytes), "DtoH");
        }
    }
    void SetInt(CUdeviceptr dst, unsigned value, size_t count) {
        if (!count) return;
        if (stream) {
            ThrowIf(api->MemsetD32Async(dst, value, count, stream), "memset");
        } else {
            ThrowIf(api->MemsetD32(dst, value, count), "memset");
        }
    }

    // Accept const addresses: all kernel arguments are inputs except the
    // explicit output pointers, and this avoids the -fpermissive-only
    // conversion that MSVC rejects.
    template <size_t N>
    void Launch(CUfunction f, int n, const void* const (&args)[N], int block = kBlock) {
        if (n <= 0) return;
        void* raw[N];
        for (size_t i = 0; i < N; ++i) raw[i] = const_cast<void*>(args[i]);
        int grid = (n + block - 1) / block;
        CUevent begin = nullptr, end = nullptr;
        const bool measure = profileKernels && stepCounter > profileSkip;
        if (measure) {
            ThrowIf(api->EventCreate(&begin, 0), "event create");
            ThrowIf(api->EventCreate(&end, 0), "event create");
            ThrowIf(api->EventRecord(begin, stream), "event record");
        }
        ThrowIf(api->LaunchKernel(f, grid, 1, 1, block, 1, 1, 0, stream, raw, nullptr), "launch");
        if (measure) {
            ThrowIf(api->EventRecord(end, stream), "event record");
            ThrowIf(api->EventSynchronize(end), "event sync");
            float ms = 0.0f;
            ThrowIf(api->EventElapsedTime(&ms, begin, end), "event elapsed");
            auto it = kernelNames.find(f);
            auto& timing = kernelTimings[it == kernelNames.end() ? "unknown" : it->second];
            timing.ms += ms;
            timing.calls++;
            api->EventDestroy(begin);
            api->EventDestroy(end);
        }
    }

    int Blocks(int n) const { return (n + kBlock - 1) / kBlock; }

    // ------------------------------------------------------------------
    void ClearParticles() {
        X.clear(); V.clear(); Vadv.clear();
        Rho.clear(); P.clear(); PPrev.clear(); Colors.clear();
        HostNbCount.clear();
        BoundX.clear(); BoundV.clear();
    }

    void PushFluid(float x, float y, float z) {
        X.push_back(Math::Vector3(x, y, z));
        V.push_back(Math::Vector3());
        Vadv.push_back(Math::Vector3());
        Rho.push_back(0.0f);
        P.push_back(0.0f);
        PPrev.push_back(0.0f);
        Colors.push_back(0.0f); Colors.push_back(0.0f); Colors.push_back(0.0f);
    }

    void SetupDerived() {
        h = 2.0f * params.spacing;
        mass = params.rho0 * params.spacing * params.spacing * params.spacing;
        vol = params.spacing * params.spacing * params.spacing;
    }

    // ---- device memory budget ------------------------------------------
    long long gpuFreeBytes = 0, gpuTotalBytes = 0;

    void RefreshMemoryInfo() {
        if (!api || !api->MemGetInfo) return;
        size_t freeB = 0, totalB = 0;
        if (api->MemGetInfo(&freeB, &totalB) == CUDA_SUCCESS) {
            gpuFreeBytes = static_cast<long long>(freeB);
            gpuTotalBytes = static_cast<long long>(totalB);
        }
    }

    // Device bytes the scene needs for `n` fluid particles: the per-particle
    // state plus a margin for the boundary layer and the grid scratch, which
    // are both far smaller than the fluid arrays at these sizes.
    size_t EstimateSceneBytes(size_t n) const {
        return static_cast<size_t>(n * kFluidBytesPerParticle * 1.12) + (80ull << 20);
    }

    // Throws before anything on the device changes when the requested set does
    // not fit. `available` counts the buffers of the current scene as reusable,
    // because EnsureN frees them before the new allocation.
    void CheckBudget(size_t n) const {
        if (!api || !api->MemGetInfo || n == 0) return;
        size_t freeB = 0, totalB = 0;
        if (api->MemGetInfo(&freeB, &totalB) != CUDA_SUCCESS) return;
        const size_t available = freeB + HeldDeviceBytes();
        const size_t need = EstimateSceneBytes(n);
        if (need <= available) return;
        const size_t reserve = 96ull << 20;
        const size_t budget = available > reserve ? available - reserve : 0;
        const size_t fits = budget / static_cast<size_t>(kFluidBytesPerParticle * 1.12);
        char msg[640];
        std::snprintf(msg, sizeof(msg),
                      "not enough GPU memory for about %zu particles: needs about %.0f MiB, "
                      "only %.0f MiB usable (%.0f MiB free of %.0f MiB). "
                      "Lower the particle count to about %zu.",
                      n, static_cast<double>(need) / 1048576.0,
                      static_cast<double>(available) / 1048576.0,
                      static_cast<double>(freeB) / 1048576.0,
                      static_cast<double>(totalB) / 1048576.0, fits);
        throw ParticleBudgetError(msg);
    }

    void BuildBoundary(bool openTop) {
        BoundX.clear();
        auto addB = [&](float x, float y, float z) { BoundX.push_back(Math::Vector3(x, y, z)); };
        const float x0 = wallX0, x1 = wallX1, z0 = wallZ0, z1 = wallZ1, yTop = wallY1;
        const int nx = std::max(1, (int)std::ceil((x1 - x0) / params.spacing));
        const int ny = std::max(1, (int)std::ceil(yTop / params.spacing));
        const int nz = std::max(1, (int)std::ceil((z1 - z0) / params.spacing));
        auto px = [&](int i) { return x0 + (x1 - x0) * (float)i / (float)nx; };
        auto py = [&](int j) { return yTop * (float)j / (float)ny; };
        auto pz = [&](int k) { return z0 + (z1 - z0) * (float)k / (float)nz; };
        for (int i = 0; i <= nx; i++)
            for (int k = 0; k <= nz; k++) addB(px(i), 0.0f, pz(k));
        for (int j = 0; j <= ny; j++)
            for (int k = 0; k <= nz; k++) { addB(x0, py(j), pz(k)); addB(x1, py(j), pz(k)); }
        for (int i = 0; i <= nx; i++)
            for (int j = 0; j <= ny; j++) { addB(px(i), py(j), z0); addB(px(i), py(j), z1); }
        if (!openTop)
            for (int i = 0; i <= nx; i++)
                for (int k = 0; k <= nz; k++) addB(px(i), yTop, pz(k));

        const size_t nb = BoundX.size();
        BoundV.assign(nb, 0.0f);
        int gwx = (int)std::ceil(boxX / h) + 1;
        int ghy = (int)std::ceil(boxY / h) + 1;
        int gdz = (int)std::ceil(boxZ / h) + 1;
        std::vector<std::vector<int>> grid((size_t)gwx * ghy * gdz);
        auto idx3 = [&](int cx, int cy, int cz) {
            return (size_t)cz * gwx * ghy + (size_t)cy * gwx + (size_t)cx;
        };
        for (size_t b = 0; b < nb; b++) {
            int cx = std::clamp((int)std::floor(BoundX[b].x / h), 0, gwx - 1);
            int cy = std::clamp((int)std::floor(BoundX[b].y / h), 0, ghy - 1);
            int cz = std::clamp((int)std::floor(BoundX[b].z / h), 0, gdz - 1);
            grid[idx3(cx, cy, cz)].push_back((int)b);
        }
        for (size_t b = 0; b < nb; b++) {
            float delta = hostW(0.0f, h);
            int cx = std::clamp((int)std::floor(BoundX[b].x / h), 0, gwx - 1);
            int cy = std::clamp((int)std::floor(BoundX[b].y / h), 0, ghy - 1);
            int cz = std::clamp((int)std::floor(BoundX[b].z / h), 0, gdz - 1);
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int ccx = cx + dx, ccy = cy + dy, ccz = cz + dz;
                        if (ccx < 0 || ccx >= gwx || ccy < 0 || ccy >= ghy || ccz < 0 || ccz >= gdz) continue;
                        for (int bb : grid[idx3(ccx, ccy, ccz)]) {
                            if ((int)b == bb) continue;
                            float rr = (BoundX[b] - BoundX[bb]).length();
                            if (rr < h) delta += hostW(rr, h);
                        }
                    }
            BoundV[b] = 1.0f / delta;
        }
    }

    // ------------------------------------------------------------------
    void UploadAll() {
        const size_t n = X.size(), nb = BoundX.size();
        EnsureN(std::max<size_t>(n, 1));
        EnsureB(std::max<size_t>(nb, 1));
        std::vector<F4> tmp(std::max<size_t>(n, 1));
        for (size_t i = 0; i < n; i++) tmp[i] = {X[i].x, X[i].y, X[i].z, 0.f};
        ToDev(dPos, tmp.data(), n * sizeof(F4));
        for (size_t i = 0; i < n; i++) tmp[i] = {V[i].x, V[i].y, V[i].z, 0.f};
        ToDev(dVel, tmp.data(), n * sizeof(F4));
        for (size_t i = 0; i < n; i++) tmp[i] = {Vadv[i].x, Vadv[i].y, Vadv[i].z, 0.f};
        ToDev(dVadv, tmp.data(), n * sizeof(F4));
        ToDev(dRho, Rho.data(), n * sizeof(float));
        ToDev(dP, P.data(), n * sizeof(float));
        ToDev(dPPrev, PPrev.data(), n * sizeof(float));
        ToDev(dColors, Colors.data(), n * 3 * sizeof(float));
        std::vector<F4> btmp(std::max<size_t>(nb, 1));
        for (size_t b = 0; b < nb; b++) btmp[b] = {BoundX[b].x, BoundX[b].y, BoundX[b].z, 0.f};
        ToDev(dBpos, btmp.data(), nb * sizeof(F4));
        ToDev(dBvol, BoundV.data(), nb * sizeof(float));
        HostNbCount.assign(n, 0);
        if (n) {
            SetInt(dNbCount, 0, n);
            SetInt(dBnbCount, 0, n);
            // Fields that the CPU constructor zero-initializes per particle.
            // Without this the first DFSPH warm start reads uninitialized
            // device memory (measured: 64 instead of 29 iterations on step 1).
            for (CUdeviceptr p : {dRhoAdv, dAii, dAlpha, dDeltaB,
                                  dKappaV, dKappaVPrev, dKappaSeed})
                SetInt(p, 0, n);
        }
        dirtyPos = dirtyVel = dirtyRho = dirtyP = dirtyNb = false;
        // PushFluid seeds the host colors with black. Generate the initial
        // velocity colors on demand just as we do after every simulation step.
        dirtyColors = true;
        lastMaxNb = 0;
        neighborCacheValid = false;
        densityCacheValid = false;
        RefreshMemoryInfo();
    }

    void SyncPositions() {
        if (!dirtyPos) return;
        const size_t n = X.size();
        if (!n) { dirtyPos = false; return; }
        hostF4.resize(n);
        FromDev(hostF4.data(), dPos, n * sizeof(F4));
        for (size_t i = 0; i < n; i++)
            X[i] = Math::Vector3(hostF4[i].x, hostF4[i].y, hostF4[i].z);
        dirtyPos = false;
    }
    void SyncVelocities() {
        if (!dirtyVel) return;
        const size_t n = V.size();
        if (!n) { dirtyVel = false; return; }
        hostF4.resize(n);
        FromDev(hostF4.data(), dVel, n * sizeof(F4));
        for (size_t i = 0; i < n; i++)
            V[i] = Math::Vector3(hostF4[i].x, hostF4[i].y, hostF4[i].z);
        dirtyVel = false;
    }
    void SyncDensities() {
        if (!dirtyRho) return;
        if (!Rho.empty()) FromDev(Rho.data(), dRho, Rho.size() * sizeof(float));
        dirtyRho = false;
    }
    void SyncPressures() {
        if (!dirtyP) return;
        if (!P.empty()) FromDev(P.data(), dP, P.size() * sizeof(float));
        dirtyP = false;
    }
    void SyncColors() {
        if (!dirtyColors) return;
        UpdateColors();
        if (!Colors.empty()) FromDev(Colors.data(), dColors, Colors.size() * sizeof(float));
        dirtyColors = false;
    }
    void SyncNeighbors() {
        if (!dirtyNb) return;
        if (HostNbCount.size() == X.size() && !HostNbCount.empty())
            FromDev(HostNbCount.data(), dNbCount, HostNbCount.size() * sizeof(int));
        lastMaxNb = 0;
        if (dMaxNb) FromDev(&lastMaxNb, dMaxNb, sizeof(int));
        if (lastMaxNb > kMaxNb && !overflowWarned) {
            std::fprintf(stderr, "[cuda] WARNING: %d fluid neighbors exceed MAXNB=%d; list truncated\n",
                         lastMaxNb, kMaxNb);
            overflowWarned = true;
        }
        dirtyNb = false;
    }

    // Full mirror refresh is only needed before host-side scene mutation
    // (inflow injection / SpawnDrop), never on the normal Step path.
    void SyncHost() {
        SyncPositions();
        SyncVelocities();
        SyncDensities();
        SyncPressures();
        SyncColors();
        SyncNeighbors();
        const size_t n = X.size();
        if (!n) return;
        hostF4.resize(n);
        FromDev(hostF4.data(), dVadv, n * sizeof(F4));
        for (size_t i = 0; i < n; i++)
            Vadv[i] = Math::Vector3(hostF4[i].x, hostF4[i].y, hostF4[i].z);
    }

    void MarkStepDirty() {
        dirtyPos = dirtyVel = dirtyRho = dirtyP = dirtyColors = true;
    }

    // ------------------------------------------------------------------
    void FinalSum(int blocks) {
        EnsureRed();
        const void* a[] = {&dBlkErr, &blocks, &dRed};
        Launch(k.reduceFinal, 1, a);
    }
    void FinalMax(int blocks) {
        EnsureRed();
        const void* a[] = {&dBlkErr, &blocks, &dRed};
        Launch(k.reduceMaxFinal, 1, a);
    }

    float MaxVelocity(int n) {
        if (!n) return 0.0f;
        EnsureBlocks(Blocks(n));
        const void* a[] = {&dVel, &n, &dBlkErr};
        Launch(k.vmag, n, a);
        FinalMax(Blocks(n));
        float v = 0.0f;
        FromDev(&v, dRed, sizeof(float));
        return v;
    }

    float ColumnSum(CUfunction f, CUdeviceptr src, int n) {
        if (!n) return 0.0f;
        EnsureBlocks(Blocks(n));
        EnsureRed();
        const void* a[] = {&src, &n, &dBlkErr};
        Launch(f, n, a);
        FinalSum(Blocks(n));
        float v = 0.0f;
        FromDev(&v, dRed, sizeof(float));
        return v;
    }

    struct Err3 { float sum, abs, max; };
    Err3 Reduce3(int n) {
        const int blocks = Blocks(n);
        EnsureBlocks(blocks);
        EnsureRed();
        const void* a0[] = {&dBlkErr, &dBlkAbs, &dBlkMax, &blocks, &dRed};
        Launch(k.reduce3Final, 1, a0);
        float out[3] = {0.f, 0.f, 0.f};
        FromDev(out, dRed, 3 * sizeof(float));
        return {out[0], out[1], out[2]};
    }

    // ------------------------------------------------------------------
    struct Grid {
        float ox = 0, oy = 0, oz = 0, invh = 0;
        int gw = 0, gh = 0, gd = 0, ncells = 0;
    };
    // Neighbor lists + boundary sums are position-dependent only. DFSPH ends
    // its step with a rebuild at the final positions, so the next step can
    // reuse it until another position integration/scene mutation invalidates it.
    bool neighborCacheValid = false;
    // rho/alpha are also position-only. The DFSPH end-of-step rebuild leaves
    // them valid for the next step's force pass; normals are still recomputed
    // because surface tension reads them from the current rho field.
    bool densityCacheValid = false;

    Grid ComputeGrid() {
        const int n = (int)X.size();
        const int nb = (int)BoundX.size();
        const int total = n + nb;
        const int blocks = Blocks(total);
        EnsureBlocks(blocks);
        EnsureRed();
        {
            const void* a[] = {&dPos, &dBpos, &n, &nb, &dBlkBounds};
            Launch(k.bounds, total, a);
        }
        {
            const void* a[] = {&dBlkBounds, &blocks, &dRed};
            Launch(k.boundsFinal, 1, a);
        }
        float b[6] = {0, 0, 0, 0, 0, 0};
        FromDev(b, dRed, sizeof(b));
        const float margin = 2.0f * h;
        Grid g;
        g.ox = b[0] - margin; g.oy = b[1] - margin; g.oz = b[2] - margin;
        const float ex = std::max(b[3] - b[0], 1e-6f) + 2 * margin;
        const float ey = std::max(b[4] - b[1], 1e-6f) + 2 * margin;
        const float ez = std::max(b[5] - b[2], 1e-6f) + 2 * margin;
        g.invh = 1.0f / h;
        g.gw = (int)std::ceil(ex / h) + 1;
        g.gh = (int)std::ceil(ey / h) + 1;
        g.gd = (int)std::ceil(ez / h) + 1;
        g.ncells = g.gw * g.gh * g.gd;
        return g;
    }

    int buildProfileCount = 0;
    void BuildNeighborhood() {
        const int n = (int)X.size();
        const int nb = (int)BoundX.size();
        if (n == 0) return;
        using Clock = std::chrono::steady_clock;
        const bool bp = profile && buildProfileCount < 2;
        Clock::time_point t0, t1, t2, t3, t4;
        if (bp) { api->CtxSynchronize(); t0 = Clock::now(); }
        const Grid g = ComputeGrid();

        if ((size_t)g.ncells > capCells) {
            Alloc(dCellCount, (size_t)g.ncells * sizeof(int), "cellCount");
            Alloc(dCellStart, ((size_t)g.ncells + 1) * sizeof(int), "cellStart");
            Alloc(dCursor, ((size_t)g.ncells + 1) * sizeof(int), "cursor");
            capCells = g.ncells;
        }
        if ((size_t)(n + nb) > capTotal) {
            Alloc(dCellOf, (size_t)(n + nb) * sizeof(int), "cellOf");
            Alloc(dSorted, (size_t)(n + nb) * sizeof(int), "sorted");
            capTotal = (size_t)(n + nb);
        }
        SetInt(dCellCount, 0, (size_t)g.ncells);

        float ox = g.ox, oy = g.oy, oz = g.oz, invh = g.invh, hh = h;
        int total = n + nb;
        {
            const void* a[] = {&dPos, &dBpos, &n, &nb, &ox, &oy, &oz, &invh, &g.gw, &g.gh, &g.gd, &dCellOf};
            Launch(k.cellOf, total, a);
        }
        {
            const void* a[] = {&dCellOf, &total, &dCellCount};
            Launch(k.countCells, total, a);
        }
        if (bp) { api->CtxSynchronize(); t1 = Clock::now(); }
        ScanCells(g.ncells);
        if (bp) { api->CtxSynchronize(); t2 = Clock::now(); }
        {
            const void* a[] = {&dCellOf, &total, &dCursor, &dSorted};
            Launch(k.scatter, total, a);
        }
        {
            const void* a[] = {&dCellStart, &g.ncells, &dSorted};
            Launch(k.sortCells, g.ncells, a);
        }
        if (bp) { api->CtxSynchronize(); t3 = Clock::now(); }
        if (!dMaxNb) Alloc(dMaxNb, sizeof(int), "maxNb");
        SetInt(dMaxNb, 0, 1);
        SetInt(dNbCount, 0, n);
        SetInt(dBnbCount, 0, n);
        {
            const void* a[] = {&dPos, &dBpos, &n, &nb, &ox, &oy, &oz, &invh, &g.gw, &g.gh, &g.gd, &hh,
                         &dCellStart, &dSorted, &dBvol, &dNbGeom, &dNbDist, &dDeltaB, &dGradB,
                         &dNbCount, &dBnb, &dBnbCount, &dMaxNb};
            Launch(k.neighbors, n, a);
        }
        {
            const void* a[] = {&dNbCount, &dBnbCount, &n, &dMaxNb};
            Launch(k.clampCounts, n, a);
        }
        if (bp) { api->CtxSynchronize(); t4 = Clock::now(); }
        dirtyNb = true;
        if (bp) {
            auto ms = [](Clock::time_point a, Clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            std::fprintf(stderr,
                         "[build] n=%d cells=%d cell+count=%.2f scan=%.2f scatter+sort=%.2f neigh=%.2f\n",
                         n, g.ncells, ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4));
            buildProfileCount++;
        }
    }

    // ------------------------------------------------------------------
    void Predict(float dt) {
        const int n = (int)X.size();
        float vv = vol, hh = h;
        if (params.solver == 1) {
            if (!densityCacheValid) {
                const void* a[] = {&dNbGeom, &dNbDist, &dNbCount, &dGradB, &dDeltaB, &n, &vv, &hh,
                             &dRho, &dInvRho2, &dDii, &dAlpha};
                Launch(k.densityFactor, n, a);
                densityCacheValid = true;
            }
        } else {
            const void* a[] = {&dNbGeom, &dNbDist, &dNbCount, &n, &vv, &hh, &dDeltaB, &dRho, &dInvRho2};
            Launch(k.density, n, a);
        }
        if (params.solver == 0) {
            const void* a[] = {&dNbGeom, &dNbCount, &dGradB, &dRho, &n, &vv, &hh, &dDii};
            Launch(k.dii, n, a);
        }
        if (params.surfaceTension > 0.0f) {
            const void* a[] = {&dNbGeom, &dNbCount, &dRho, &n, &vv, &hh, &dNormals};
            Launch(k.normals, n, a);
        }
        float dtv = dt, grav = params.gravity, visc = params.viscosity;
        float tens = params.surfaceTension, tensB = params.surfaceTensionBoundary;
        float rho0 = params.rho0;
        {
            const void* a[] = {&dPos, &dVel, &dBpos, &dNbCount, &dNbGeom, &dNbDist, &dBnb, &dBnbCount,
                         &dNormals, &dRho, &dBvol, &n, &vv, &hh, &dtv, &grav,
                         &visc, &tens, &tensB, &rho0, &dVadv};
            Launch(k.forces, n, a);
        }
        if (params.solver == 0) {
            const void* a[] = {&dPos, &dNbCount, &dNbGeom, &dDii, &dGradB, &dVadv,
                         &dRho, &dInvRho2, &dPPrev, &n, &vv, &hh, &dtv, &dRhoAdv, &dP, &dPS};
            Launch(k.rhoAdvAii, n, a);
            const void* b[] = {&dNbGeom, &dNbCount, &dDii, &dGradB, &dRho, &n, &vv, &hh, &dAii};
            Launch(k.aii, n, b);
        }
    }

    void SolveIISPH(float dt) {
        const int n = (int)X.size();
        const float dtv = dt, om = params.omega, vv = vol, hh = h;
        CUdeviceptr cur = dP, nxt = dPTmp;
        lastIter = 0;
        while (true) {
            {
                const void* a[] = {&dNbGeom, &dNbCount, &dPS, &n, &dS};
                Launch(k.pressureS, n, a);
            }
            {
                const void* a[] = {&dNbGeom, &dNbCount, &dDii, &dS, &dGradB, &dPS,
                             &dRhoAdv, &dAii, &n, &vv, &hh, &dtv, &om, &nxt,
                             &dBlkErr, &dBlkAbs, &dBlkMax};
                EnsureBlocks(Blocks(n));
                Launch(k.pressureP, n, a);
            }
            std::swap(cur, nxt);
            lastIter++;
            // One host sync per 8 Jacobi iterations instead of one per
            // iteration: on Windows/WDDM each cuMemcpyDtoH stalls the pipeline.
            if (lastIter % 8 == 0 || lastIter >= params.maxIter) {
                Err3 e = Reduce3(n);
                avgErr = e.sum / (float)n;
                avgAbsErr = e.abs / (float)n;
                maxErr = e.max;
                if (traceNext) convTrace.push_back(std::fabs(avgErr));
                if (lastIter >= 2 && avgAbsErr <= params.eta) break;
                if (lastIter >= params.maxIter) break;
            }
        }
        dP = cur;
        dPTmp = nxt;
        traceNext = false;

        const float half = 0.5f * params.spacing;
        int wallTopFlag = wallTop ? 1 : 0;
        {
            const void* a[] = {&dDii, &dS, &dP, &dVadv, &dPos, &n,
                         &dtv, &wallX0, &wallX1, &wallY1, &wallZ0, &wallZ1,
                         &wallTopFlag, &half, &dVel, &dPos, &dPPrev};
            Launch(k.integrate, n, a);
        }
        const float vmax = 4.0f;
        {
            const void* a[] = {&dVel, &n, &vmax};
            Launch(k.clampV, n, a);
        }
        neighborCacheValid = false;   // positions moved in Integrate
        densityCacheValid = false;
    }

    void UpdateColors() {
        const int n = (int)X.size();
        if (!n) return;
        EnsureBlocks(Blocks(n));
        {
            const void* a[] = {&dVel, &n, &dBlkErr};
            Launch(k.vmag, n, a);
        }
        FinalMax(Blocks(n));
        {
            const void* a[] = {&dVel, &n, &dRed, &dColors};
            Launch(k.colors, n, a);
        }
        // SyncColors owns the host-side dirty flag. Clearing it here used to
        // suppress the device-to-host copy and left every GUI particle black.
    }

    // ------------------------------------------------------------------
    void SolveDFSPH(float dt) {
        const int n = (int)X.size();
        if (!n) return;
        const float vv = vol, hh = h, dtv = dt, om = params.omega;
        const int warm = params.warmStart ? 1 : 0;
        const float invH = 1.0f / dt;
        const float half = 0.5f * params.spacing;
        int wallTopFlag = wallTop ? 1 : 0;

        auto reduce3 = [&]() { return Reduce3(n); };

        // constant-density solver (rho/alpha were produced by Predict)
        {
            const void* b[] = {&dNbGeom, &dNbCount, &dBnbCount, &dVadv, &dGradB, &dRho,
                         &dAlpha, &dPPrev, &n, &vv, &hh, &dtv, &warm, &dRhoAdv, &dP};
            Launch(k.dfsphInit, n, b);
        }
        {
            const void* a[] = {&dNbGeom, &dNbCount, &dGradB, &dP,
                         &n, &vv, &hh, &dtv, &om, &dVadv};
            Launch(k.dfsphVel, n, a);
            const void* b[] = {&dNbGeom, &dNbCount, &dVadv, &dGradB, &dRho, &dAlpha,
                         &n, &vv, &dtv, &dRhoAdv, &dP, &dBlkErr, &dBlkAbs, &dBlkMax};
            EnsureBlocks(Blocks(n));
            Launch(k.dfsphRhoAdv, n, b);
        }
        // The first reduction cannot stop the solve: minIterations is 2. Skip
        // it and let the second iteration produce the first decision point.
        Err3 e{0.f, 0.f, 0.f};
        lastIter = 1;
        int iter = 1;
        if (params.maxIter <= 1) {
            e = reduce3();
        } else {
            do {
                iter++;
                {
                    const void* a[] = {&dNbGeom, &dNbCount, &dGradB, &dP,
                                 &n, &vv, &hh, &dtv, &om, &dVadv};
                    Launch(k.dfsphVel, n, a);
                    const void* b[] = {&dNbGeom, &dNbCount, &dVadv, &dGradB, &dRho, &dAlpha,
                                 &n, &vv, &dtv, &dRhoAdv, &dP, &dBlkErr, &dBlkAbs, &dBlkMax};
                    Launch(k.dfsphRhoAdv, n, b);
                }
                e = reduce3();
                lastIter = iter;
            } while (e.sum / (float)n > params.eta && iter < params.maxIter);
        }
        avgErr = e.sum / (float)n;
        avgAbsErr = e.abs / (float)n;
        maxErr = e.max;
        if (params.warmStart) {
            float s = dt * dt;
            const void* a[] = {&dPPrev, &dP, &n, &s};
            Launch(k.scaleCopyF, n, a);
        }

        {
            const void* a[] = {&dVadv, &dPos, &n, &dtv, &wallX0, &wallX1, &wallY1, &wallZ0, &wallZ1,
                         &wallTopFlag, &half, &dPos, &dVadv};
            Launch(k.dfsphIntegrate, n, a);
        }

        BuildNeighborhood();
        neighborCacheValid = true;   // matches the positions at the next step start
        // density + stiffness on the new positions
        {
            const void* a[] = {&dNbGeom, &dNbDist, &dNbCount, &dGradB, &dDeltaB, &n, &vv, &hh,
                         &dRho, &dInvRho2, &dDii, &dAlpha};
            Launch(k.densityFactor, n, a);
            densityCacheValid = true;
            EnsureBlocks(Blocks(n));
            const void* c[] = {&dNbGeom, &dNbCount, &dBnbCount, &dVadv, &dGradB,
                         &n, &vv, &invH, &warm, &dKappaVPrev, &dKappaV, &dKappaSeed, &dBlkErr};
            Launch(k.dfsphKappaInit, n, c);
        }
        // One divergence iteration is mandatory, so its initial residual does
        // not affect the loop decision. Avoid a redundant full reduction.
        float err = std::numeric_limits<float>::max();
        lastIterV = 0;
        int iterv = 0;
        while ((err > params.etaV / dtv || iterv < 1) && iterv < params.maxIter) {
            iterv++;
            {
                const void* a[] = {&dAlpha, &n, &invH, &iterv, &warm, &dKappaSeed, &dKappaV};
                Launch(k.dfsphKappaScale, n, a);
                const void* b[] = {&dNbGeom, &dNbCount, &dGradB, &dKappaV,
                             &n, &vv, &dtv, &om, &dVadv};
                Launch(k.dfsphVelV, n, b);
                const void* c[] = {&dNbGeom, &dNbCount, &dBnbCount, &dVadv, &dGradB,
                             &n, &vv, &hh, &dKappaV, &dBlkErr};
                EnsureBlocks(Blocks(n));
                Launch(k.dfsphRhoDot, n, c);
            }
            FinalSum(Blocks(n));
            float errSum = 0.0f;
            FromDev(&errSum, dRed, sizeof(float));
            err = errSum / (float)n;
            lastIterV = iterv;
        }
        lastIterV = iterv;
        const float vmax = 4.0f;
        {
            const void* a[] = {&dVadv, &dVel, &n, &vmax, &dKappaV, &dAlpha, &warm, &dKappaVPrev};
            Launch(k.dfsphFinalize, n, a);
        }
    }

    // ------------------------------------------------------------------
    int profileCount = 0;
    bool profile = (std::getenv("SPH_CUDA_PROFILE") != nullptr);

    float RunStep() {
        ++stepCounter;
        const int n0 = (int)X.size();
        if (n0 == 0) return params.dt;
        using Clock = std::chrono::steady_clock;
        const bool prof = profile && profileCount < 5;
        Clock::time_point tB, tP, tS;

        float dt = params.dt;
        if (params.useCFL) {
            const float vmax = MaxVelocity(n0);
            dt = std::min(params.dt, 0.4f * params.spacing / std::max(vmax, 0.1f));
        }
        const float prevDt = params.dt;
        params.dt = dt;

        if (inflow) InjectFluid(dt);

        if (prof) { api->CtxSynchronize(); tB = Clock::now(); }
        if (!neighborCacheValid) {
            BuildNeighborhood();
            neighborCacheValid = true;
        }
        if (prof) { api->CtxSynchronize(); tP = Clock::now(); }
        Predict(dt);
        if (prof) { api->CtxSynchronize(); tS = Clock::now(); }
        if (params.solver == 1) {
            SolveDFSPH(dt);
        } else {
            SolveIISPH(dt);
        }
        if (prof) {
            api->CtxSynchronize();
            auto tE = Clock::now();
            auto ms = [](Clock::time_point a, Clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            std::fprintf(stderr, "[profile] n=%d build=%.1f predict=%.1f solve=%.1f ms\n",
                         (int)X.size(), ms(tB, tP), ms(tP, tS), ms(tS, tE));
            profileCount++;
        }
        time += dt;
        params.dt = prevDt;
        MarkStepDirty();
        return dt;
    }

    void InjectFluid(float dt) {
        if (!inflow || inflowPerStep <= 0) return;
        const int n = (int)X.size();
        if (n >= maxParticles) return;
        const float vFall = std::sqrt(2.0f * std::fabs(params.gravity) * inflowGap);
        inflowCounter += vFall * dt;
        if (inflowCounter < params.spacing) return;
        inflowCounter -= params.spacing;
        // Scene mutation appends to the host mirror and UploadAll() re-uploads
        // the whole state, so this rare path needs a current mirror.
        SyncHost();
        const float yEmit = boxY + inflowGap;
        const int nn = (int)std::ceil(inflowRadius / params.spacing);
        const int cap = std::min(inflowPerStep, maxParticles - n);
        int spawned = 0;
        for (int i = -nn; i <= nn && spawned < cap; i++)
            for (int k = -nn; k <= nn && spawned < cap; k++) {
                const float dx = i * params.spacing, dz = k * params.spacing;
                if (dx * dx + dz * dz <= inflowRadius * inflowRadius) {
                    PushFluid(inflowX + dx, yEmit, inflowZ + dz);
                    V.back() = Math::Vector3(0.0f, -vFall, 0.0f);
                    spawned++;
                }
            }
        if (spawned > 0) UploadAll();
    }

    // ------------------------------------------------------------------
    void Compile() {
        api = &sphcuda::Api();
        if (!api->loaded) throw std::runtime_error(api->loadError.empty()
                                                       ? "CUDA driver/NVRTC unavailable"
                                                       : api->loadError);
        ThrowIf(api->Init(0), "cuInit");
        int count = 0;
        ThrowIf(api->DeviceGetCount(&count), "cuDeviceGetCount");
        if (count <= 0) throw std::runtime_error("no CUDA device");
        int devIndex = 0;
        if (const char* e = std::getenv("SPH_CUDA_DEVICE")) {
            int v = std::atoi(e);
            if (v >= 0 && v < count) devIndex = v;
        }
        ThrowIf(api->DeviceGet(&device, devIndex), "cuDeviceGet");
        char name[256] = {0};
        api->DeviceGetName(name, sizeof(name), device);
        deviceName = name;
        ThrowIf(api->DevicePrimaryCtxRetain(&ctx, device), "primary ctx");
        ThrowIf(api->CtxSetCurrent(ctx), "ctx set");
        RefreshMemoryInfo();

        int ccMajor = 6, ccMinor = 1;
        api->DeviceGetAttribute(&ccMajor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device);
        api->DeviceGetAttribute(&ccMinor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device);
        const std::string arch = "--gpu-architecture=compute_" +
                                 std::to_string(ccMajor * 10 + ccMinor);

        nvrtcProgram prog = nullptr;
        const char* src = sphkernels::kSource;
        if (api->NvrtcCreateProgram(&prog, src, "sph_kernels.cu", 0, nullptr, nullptr) != NVRTC_SUCCESS)
            throw std::runtime_error("nvrtcCreateProgram failed");
        // FMA contraction and fast div/sqrt are within the parity test's
        // tolerance and cut the per-neighbor instruction count substantially.
        const char* opts[] = {arch.c_str(), "--std=c++14", "--fmad=true",
                              "--prec-div=false", "--prec-sqrt=false",
                              "--use_fast_math",
                              "--device-as-default-execution-space"};
        // NVRTC mangles C++ kernel names, so register every entry point before
        // compilation and resolve the lowered name afterwards.
        static const char* kKernelNames[] = {
            "kBounds", "kBoundsFinal",
            "kCellOf", "kCountCells", "kScanBlocks", "kScanAddOffsets", "kFinalizeScan", "kCopyInt",
            "kScatter", "kSortCells", "kNeighbors", "kClampCounts",
            "kDensity", "kDensityFactor", "kDii", "kNormalsIdx", "kForcesFull", "kRhoAdvAii", "kAii",
            "kPressureSIdx", "kPressureP", "kReduceFinal", "kReduce3Final", "kIntegrate", "kClampV", "kVMag",
            "kReduceMaxFinal", "kColors", "kMeanRho", "kDFSPHInit",
            "kDFSPHVelocityUpdate", "kDFSPHRhoAdvUpdateFull", "kDFSPHIntegratePositions",
            "kDFSPHKappaInit", "kDFSPHKappaScale",
            "kDFSPHVelocityUpdateV", "kDFSPHRhoDotUpdate", "kDFSPHFinalize",
            "kZeroInt", "kScaleCopyF"};
        for (const char* n : kKernelNames) api->NvrtcAddNameExpression(prog, n);
        nvrtcResult cr = api->NvrtcCompileProgram(prog, 7, opts);
        size_t logSize = 0;
        api->NvrtcGetProgramLogSize(prog, &logSize);
        std::string log;
        if (logSize > 1) {
            log.resize(logSize);
            api->NvrtcGetProgramLog(prog, &log[0]);
        }
        if (cr != NVRTC_SUCCESS) {
            throw std::runtime_error("NVRTC compile failed: " + log);
        }
        if (!log.empty() && log.find("warning") != std::string::npos) {
            std::fprintf(stderr, "[cuda] NVRTC warnings:\n%s\n", log.c_str());
        }
        size_t ptxSize = 0;
        if (api->NvrtcGetPTXSize(prog, &ptxSize) != NVRTC_SUCCESS)
            throw std::runtime_error("nvrtcGetPTXSize failed");
        std::vector<char> ptx(ptxSize + 1, 0);
        if (api->NvrtcGetPTX(prog, ptx.data()) != NVRTC_SUCCESS)
            throw std::runtime_error("nvrtcGetPTX failed");
        if (const char* dump = std::getenv("SPH_CUDA_DUMP_PTX")) {
            if (FILE* f = std::fopen(dump, "wb")) {
                std::fwrite(ptx.data(), 1, ptxSize, f);
                std::fclose(f);
            }
        }
        std::unordered_map<std::string, std::string> lowered;
        for (const char* n : kKernelNames) {
            const char* ln = nullptr;
            if (api->NvrtcGetLoweredName(prog, n, &ln) == NVRTC_SUCCESS && ln) lowered[n] = ln;
        }
        api->NvrtcDestroyProgram(&prog);

        ThrowIf(api->ModuleLoadDataEx(&module, ptx.data(), 0, nullptr, nullptr), "module load");

        auto get = [&](const char* name) {
            CUfunction f = nullptr;
            auto it = lowered.find(name);
            const char* real = (it != lowered.end()) ? it->second.c_str() : name;
            CUresult r = api->ModuleGetFunction(&f, module, real);
            if (r != CUDA_SUCCESS)
                throw std::runtime_error(std::string("missing kernel ") + name + " (" + real + ")");
            kernelNames[f] = name;
            return f;
        };
        k.bounds = get("kBounds");
        k.boundsFinal = get("kBoundsFinal");
        k.cellOf = get("kCellOf");
        k.countCells = get("kCountCells");
        k.scanBlocks = get("kScanBlocks");
        k.scanAddOffsets = get("kScanAddOffsets");
        k.finalizeScan = get("kFinalizeScan");
        k.copyInt = get("kCopyInt");
        k.scatter = get("kScatter");
        k.sortCells = get("kSortCells");
        k.neighbors = get("kNeighbors");
        k.clampCounts = get("kClampCounts");
        k.density = get("kDensity");
        k.densityFactor = get("kDensityFactor");
        k.dii = get("kDii");
        k.normals = get("kNormalsIdx");
        k.forces = get("kForcesFull");
        k.rhoAdvAii = get("kRhoAdvAii");
        k.aii = get("kAii");
        k.pressureS = get("kPressureSIdx");
        k.pressureP = get("kPressureP");
        k.reduceFinal = get("kReduceFinal");
        k.reduce3Final = get("kReduce3Final");
        k.integrate = get("kIntegrate");
        k.clampV = get("kClampV");
        k.vmag = get("kVMag");
        k.reduceMaxFinal = get("kReduceMaxFinal");
        k.colors = get("kColors");
        k.meanRho = get("kMeanRho");
        k.dfsphInit = get("kDFSPHInit");
        k.dfsphVel = get("kDFSPHVelocityUpdate");
        k.dfsphRhoAdv = get("kDFSPHRhoAdvUpdateFull");
        k.dfsphIntegrate = get("kDFSPHIntegratePositions");
        k.dfsphKappaInit = get("kDFSPHKappaInit");
        k.dfsphKappaScale = get("kDFSPHKappaScale");
        k.dfsphVelV = get("kDFSPHVelocityUpdateV");
        k.dfsphRhoDot = get("kDFSPHRhoDotUpdate");
        k.dfsphFinalize = get("kDFSPHFinalize");
        k.zeroInt = get("kZeroInt");
        k.scaleCopyF = get("kScaleCopyF");

        // A non-default stream avoids the extra implicit-synchronisation cost
        // the legacy default stream carries under Windows/WDDM.
        if (api->StreamCreate) ThrowIf(api->StreamCreate(&stream, CU_STREAM_NON_BLOCKING), "stream");
        if (api->MemAllocHost) ThrowIf(api->MemAllocHost(&hostPinned, 256), "pinned host");

        // Optional launch-latency probe: SPH_CUDA_LAUNCH_BENCH=1 prints the
        // per-launch cost, which is what dominates the 100-iteration pressure
        // solve under WDDM.
        if (std::getenv("SPH_CUDA_LAUNCH_BENCH")) {
            CUdeviceptr dummy = 0;
            api->MemAlloc(&dummy, 4);
            int one = 1;
            const void* a[] = {&dummy, &one};
            api->CtxSynchronize();
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 1000; ++i) Launch(k.zeroInt, 1, a);
            api->CtxSynchronize();
            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::fprintf(stderr, "[launch-bench] %.3f ms/launch (1000 empty kernels)\n", ms / 1000.0);
            api->MemFree(dummy);
        }
    }

    // ------------------------------------------------------------------
    void InitScene(int mode, float a, float b, float c, float d, float e, float f,
                   int g, float cx, float cz) {
        (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)cx; (void)cz;
        (void)mode;
    }
};

// ---------------------------------------------------------------------------
// public interface
// ---------------------------------------------------------------------------
std::unique_ptr<FluidSolverCuda> FluidSolverCuda::Create(std::string* error) {
    try {
        auto solver = std::unique_ptr<FluidSolverCuda>(new FluidSolverCuda(FluidParams{}));
        solver->m_impl->Compile();
        return solver;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return nullptr;
    }
}

FluidSolverCuda::FluidSolverCuda(Params p) : m_impl(new Impl()) { m_impl->params = p; }
FluidSolverCuda::~FluidSolverCuda() = default;

void FluidSolverCuda::Init(float boxX, float boxY, float boxZ, float waterSize) {
    Impl& d = *m_impl;
    d.ClearParticles();
    d.boxX = boxX; d.boxY = boxY; d.boxZ = boxZ; d.waterSize = waterSize;
    d.useFullInit = false; d.useDamOpen = false; d.inflow = false; d.drop = false;
    d.dropSpawned = false;
    d.wallX0 = 0; d.wallX1 = boxX; d.wallY1 = boxY; d.wallZ0 = 0; d.wallZ1 = boxZ;
    d.wallTop = true;
    d.SetupDerived();
    int n = (int)std::ceil(waterSize / d.params.spacing);
    for (int k = 0; k < n; k++)
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++)
                d.PushFluid((i + 0.5f) * d.params.spacing, (j + 0.5f) * d.params.spacing,
                            (k + 0.5f) * d.params.spacing);
    d.BuildBoundary(false);
    d.time = 0; d.avgErr = 0; d.lastIter = 0;
    d.UploadAll();
}

void FluidSolverCuda::InitDamOpen(float boxX, float boxY, float boxZ, float waterSize) {
    Impl& d = *m_impl;
    d.ClearParticles();
    d.boxX = boxX; d.boxY = boxY; d.boxZ = boxZ; d.waterSize = waterSize;
    d.useFullInit = false; d.useDamOpen = true; d.inflow = false; d.drop = false;
    d.dropSpawned = false;
    d.wallX0 = 0; d.wallX1 = boxX; d.wallY1 = boxY; d.wallZ0 = 0; d.wallZ1 = boxZ;
    d.wallTop = false;
    d.SetupDerived();
    int n = (int)std::ceil(waterSize / d.params.spacing);
    for (int k = 0; k < n; k++)
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++)
                d.PushFluid((i + 0.5f) * d.params.spacing, (j + 0.5f) * d.params.spacing,
                            (k + 0.5f) * d.params.spacing);
    d.BuildBoundary(true);
    d.time = 0; d.avgErr = 0; d.lastIter = 0;
    d.UploadAll();
}

void FluidSolverCuda::InitFull(float boxX, float boxY, float boxZ, float waterHeight) {
    Impl& d = *m_impl;
    d.ClearParticles();
    d.boxX = boxX; d.boxY = boxY; d.boxZ = boxZ; d.waterSize = waterHeight;
    d.useFullInit = true; d.useDamOpen = false; d.inflow = false; d.drop = false;
    d.dropSpawned = false;
    d.wallX0 = 0; d.wallX1 = boxX; d.wallY1 = boxY; d.wallZ0 = 0; d.wallZ1 = boxZ;
    d.wallTop = true;
    d.SetupDerived();
    int nx = (int)std::ceil(boxX / d.params.spacing);
    int ny = (int)std::ceil(waterHeight / d.params.spacing);
    int nz = (int)std::ceil(boxZ / d.params.spacing);
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++)
                d.PushFluid((i + 0.5f) * d.params.spacing, (j + 0.5f) * d.params.spacing,
                            (k + 0.5f) * d.params.spacing);
    d.BuildBoundary(false);
    d.time = 0; d.avgErr = 0; d.lastIter = 0;
    d.UploadAll();
}

void FluidSolverCuda::InitInflow(float tankSize, float gap, int perStep, float radius,
                                 float cx, float cz, int maxParticles) {
    Impl& d = *m_impl;
    d.ClearParticles();
    d.boxX = tankSize; d.boxY = tankSize; d.boxZ = tankSize; d.waterSize = 0;
    d.inflow = true; d.inflowGap = gap; d.inflowPerStep = perStep; d.inflowRadius = radius;
    d.inflowX = cx; d.inflowZ = cz;
    d.maxParticles = maxParticles > 0 ? maxParticles : 8000;
    d.drop = false; d.useFullInit = false; d.useDamOpen = false; d.dropSpawned = false;
    d.wallX0 = cx - 0.5f * tankSize; d.wallX1 = cx + 0.5f * tankSize;
    d.wallY1 = tankSize;
    d.wallZ0 = cz - 0.5f * tankSize; d.wallZ1 = cz + 0.5f * tankSize;
    d.wallTop = false;
    d.SetupDerived();
    const int n0 = 6;
    for (int k = 0; k < n0; k++)
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < n0; i++)
                d.PushFluid(cx - 0.15f + (i + 0.5f) * d.params.spacing,
                            (j + 0.5f) * d.params.spacing,
                            cz - 0.15f + (k + 0.5f) * d.params.spacing);
    d.BuildBoundary(true);
    d.inflowCounter = 0;
    d.time = 0; d.avgErr = 0; d.lastIter = 0;
    d.UploadAll();
}

void FluidSolverCuda::InitDrop(float tankSize, float poolHeight, float dropRadius, float dropGap,
                               float cx, float cz, int maxParticles) {
    Impl& d = *m_impl;
    d.ClearParticles();
    d.boxX = tankSize; d.boxY = tankSize; d.boxZ = tankSize; d.waterSize = 0;
    d.inflow = false; d.drop = true;
    d.dropPool = poolHeight; d.dropSize = dropRadius; d.dropGap = dropGap;
    d.useFullInit = false; d.useDamOpen = false; d.dropSpawned = false;
    d.inflowX = cx; d.inflowZ = cz;
    d.wallX0 = cx - 0.5f * tankSize; d.wallX1 = cx + 0.5f * tankSize;
    d.wallY1 = tankSize;
    d.wallZ0 = cz - 0.5f * tankSize; d.wallZ1 = cz + 0.5f * tankSize;
    d.wallTop = false;
    d.SetupDerived();
    const int nMax = maxParticles > 0 ? maxParticles : 40000;
    d.maxParticles = nMax;
    const float x0 = cx - 0.5f * tankSize, z0 = cz - 0.5f * tankSize;
    int nx = (int)std::ceil(tankSize / d.params.spacing);
    int ny = (int)std::ceil(poolHeight / d.params.spacing);
    for (int k = 0; k < nx && (int)d.X.size() < nMax; k++)
        for (int j = 0; j < ny && (int)d.X.size() < nMax; j++)
            for (int i = 0; i < nx && (int)d.X.size() < nMax; i++)
                d.PushFluid(x0 + (i + 0.5f) * d.params.spacing,
                            (j + 0.5f) * d.params.spacing,
                            z0 + (k + 0.5f) * d.params.spacing);
    d.BuildBoundary(true);
    d.UploadAll();
    for (int k = 0; k < 400; k++) d.RunStep();
    d.time = 0.0f;
    d.SetInt(d.dPPrev, 0, d.X.size());
    for (int k = -((int)std::ceil(dropRadius / d.params.spacing));
         k <= (int)std::ceil(dropRadius / d.params.spacing) && (int)d.X.size() < nMax; k++)
        for (int j = -((int)std::ceil(dropRadius / d.params.spacing));
             j <= (int)std::ceil(dropRadius / d.params.spacing) && (int)d.X.size() < nMax; j++)
            for (int i = -((int)std::ceil(dropRadius / d.params.spacing));
                 i <= (int)std::ceil(dropRadius / d.params.spacing) && (int)d.X.size() < nMax; i++) {
                const float dx = i * d.params.spacing, dy = j * d.params.spacing, dz = k * d.params.spacing;
                if (dx * dx + dy * dy + dz * dz <= dropRadius * dropRadius) {
                    const float dropBottom = tankSize + dropGap;
                    const float cyc = dropBottom + dropRadius;
                    d.PushFluid(cx + dx, cyc + dy, cz + dz);
                }
            }
    d.UploadAll();
}

void FluidSolverCuda::SpawnDrop(float radius, float cx, float cy, float cz) {
    Impl& d = *m_impl;
    if (d.dropSpawned) return;
    d.dropSpawned = true;
    d.SyncHost();
    const float sp = d.params.spacing;
    const int n = (int)std::ceil(radius / sp);
    const float r = radius - 0.25f * sp;
    for (int ix = -n; ix <= n; ix++)
        for (int iy = -n; iy <= n; iy++)
            for (int iz = -n; iz <= n; iz++) {
                const float dx = (float)ix * sp, dy = (float)iy * sp, dz = (float)iz * sp;
                if (dx * dx + dy * dy + dz * dz <= r * r) d.PushFluid(cx + dx, cy + dy, cz + dz);
            }
    d.UploadAll();
}

bool FluidSolverCuda::HasDrop() const { return m_impl->dropSpawned; }

void FluidSolverCuda::Reset() {
    Impl& d = *m_impl;
    if (d.inflow) InitInflow(d.boxX, d.inflowGap, d.inflowPerStep, d.inflowRadius, d.inflowX, d.inflowZ, d.maxParticles);
    else if (d.drop) InitDrop(d.boxX, d.dropPool, d.dropSize, d.dropGap, d.inflowX, d.inflowZ, d.maxParticles);
    else if (d.useDamOpen) InitDamOpen(d.boxX, d.boxY, d.boxZ, d.waterSize);
    else if (d.useFullInit) InitFull(d.boxX, d.boxY, d.boxZ, d.waterSize);
    else Init(d.boxX, d.boxY, d.boxZ, d.waterSize);
}

void FluidSolverCuda::Step() {
    const auto t0 = std::chrono::steady_clock::now();
    m_impl->RunStep();
    m_impl->lastPressureMs = std::chrono::duration<float, std::milli>(
                                 std::chrono::steady_clock::now() - t0).count();
}

const std::vector<Math::Vector3>& FluidSolverCuda::Positions() const {
    m_impl->SyncPositions();
    return m_impl->X;
}
const std::vector<Math::Vector3>& FluidSolverCuda::Velocities() const {
    m_impl->SyncVelocities();
    return m_impl->V;
}
const std::vector<float>& FluidSolverCuda::Densities() const {
    m_impl->SyncDensities();
    return m_impl->Rho;
}
const std::vector<float>& FluidSolverCuda::Colors() const {
    m_impl->SyncColors();
    return m_impl->Colors;
}
const std::vector<float>& FluidSolverCuda::Pressures() const {
    m_impl->SyncPressures();
    return m_impl->P;
}
const std::vector<Math::Vector3>& FluidSolverCuda::BoundaryPositions() const { return m_impl->BoundX; }

float FluidSolverCuda::SimTime() const { return m_impl->time; }
float FluidSolverCuda::AvgDensityError() const { return m_impl->avgErr; }
float FluidSolverCuda::MeanDensity() const {
    Impl& d = *m_impl;
    const int n = (int)d.X.size();
    if (n == 0) return d.params.rho0;
    float sum = d.ColumnSum(d.k.meanRho, d.dRho, n);
    if (sum <= 1e-9f) return d.params.rho0;
    return d.params.rho0 * (sum / (float)n);
}
float FluidSolverCuda::MeanAbsDensityError() const { return m_impl->avgAbsErr; }
float FluidSolverCuda::MaxDensityError() const { return m_impl->maxErr; }
int FluidSolverCuda::LastIterations() const { return m_impl->lastIter; }
int FluidSolverCuda::DivergenceIterations() const { return m_impl->lastIterV; }
float FluidSolverCuda::LastPressureTimeMs() const { return m_impl->lastPressureMs; }
int FluidSolverCuda::FluidCount() const { return (int)m_impl->X.size(); }
int FluidSolverCuda::BoundaryCount() const { return (int)m_impl->BoundX.size(); }

FluidSolverCuda::Params& FluidSolverCuda::GetParams() { return m_impl->params; }
const FluidSolverCuda::Params& FluidSolverCuda::GetParams() const { return m_impl->params; }
float FluidSolverCuda::KernelRadius() const { return m_impl->h; }
float FluidSolverCuda::Spacing() const { return m_impl->params.spacing; }

void FluidSolverCuda::EnableConvergenceTrace() { m_impl->traceNext = true; }
const std::vector<float>& FluidSolverCuda::ConvergenceTrace() const { return m_impl->convTrace; }
int FluidSolverCuda::NeighborCount(int i) const {
    m_impl->SyncNeighbors();
    if (i < 0 || i >= (int)m_impl->HostNbCount.size()) return 0;
    return m_impl->HostNbCount[i];
}
int FluidSolverCuda::MaxNeighbors() const {
    m_impl->SyncNeighbors();
    return m_impl->lastMaxNb;
}
const char* FluidSolverCuda::BackendName() const { return m_impl->deviceName.c_str(); }

GpuMemoryInfo FluidSolverCuda::GpuMemory() const {
    GpuMemoryInfo info;
    info.freeBytes = m_impl->gpuFreeBytes;
    info.totalBytes = m_impl->gpuTotalBytes;
    info.bytesPerParticle = static_cast<long long>(Impl::kFluidBytesPerParticle);
    return info;
}

void FluidSolverCuda::CheckParticleBudget(int particleCount) const {
    if (particleCount > 0)
        m_impl->CheckBudget(static_cast<size_t>(particleCount));
}

#endif  // SPH_HAS_CUDA
