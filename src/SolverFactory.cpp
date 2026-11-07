#include "SolverFactory.hpp"

#include "FluidSolver.hpp"

#if defined(SPH_HAS_CUDA)
#include "Cuda/FluidSolverCuda.hpp"
#endif

#include <algorithm>
#include <cctype>

namespace {
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
}  // namespace

SolverBackend ParseBackend(const std::string& s, bool* ok) {
    const std::string v = Lower(s);
    if (ok) *ok = true;
    if (v == "cuda" || v == "gpu") return SolverBackend::CUDA;
    if (v == "cpu") return SolverBackend::CPU;
    if (v == "auto" || v.empty()) return SolverBackend::Auto;
    if (ok) *ok = false;
    return SolverBackend::Auto;
}

const char* BackendToken(SolverBackend b) {
    switch (b) {
        case SolverBackend::CUDA: return "cuda";
        case SolverBackend::CPU: return "cpu";
        default: return "auto";
    }
}

const char* BackendDisplayName(SolverBackend b) {
    switch (b) {
        case SolverBackend::CUDA: return "CUDA";
        case SolverBackend::CPU: return "CPU";
        default: return "Auto";
    }
}

bool CudaBackendAvailable(std::string* error) {
#if defined(SPH_HAS_CUDA)
    auto probe = FluidSolverCuda::Create(error);
    return probe != nullptr;
#else
    if (error) *error = "CUDA backend was not compiled (SPH_HAS_CUDA off)";
    return false;
#endif
}

std::unique_ptr<IFluidSolver> CreateSolver(SolverBackend b, std::string* error) {
#if defined(SPH_HAS_CUDA)
    if (b != SolverBackend::CPU) {
        std::string err;
        if (auto gpu = FluidSolverCuda::Create(&err)) return gpu;
        if (b == SolverBackend::CUDA) {
            if (error) *error = err;
            return nullptr;
        }
        if (error) *error = err;   // auto: keep the reason, then fall back
    }
#else
    if (b == SolverBackend::CUDA) {
        if (error) *error = "CUDA backend was not compiled (SPH_HAS_CUDA off)";
        return nullptr;
    }
#endif
    return std::make_unique<FluidSolver>();
}
