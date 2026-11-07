#pragma once
#include "IFluidSolver.hpp"

#include <memory>
#include <string>

// Backend selection shared by the CLI, the headless runner and the GUI.
enum class SolverBackend { Auto = 0, CPU = 1, CUDA = 2 };

SolverBackend ParseBackend(const std::string& s, bool* ok = nullptr);
const char* BackendToken(SolverBackend b);        // "auto" / "cpu" / "cuda"
const char* BackendDisplayName(SolverBackend b);  // "Auto" / "CPU" / "CUDA"

// True when the CUDA backend can be created right now (DLLs present, NVRTC can
// compile the kernels, a device is available). Fills *error with the reason
// otherwise.
bool CudaBackendAvailable(std::string* error);

// Creates the requested backend. Auto falls back to the CPU solver (and fills
// *error with the reason), CUDA returns nullptr when unavailable, CPU always
// succeeds.
std::unique_ptr<IFluidSolver> CreateSolver(SolverBackend b, std::string* error);
