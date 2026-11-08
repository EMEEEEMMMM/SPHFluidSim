#pragma once
// Minimal dynamic loader for the CUDA Driver API and NVRTC.
//
// Neither nvcuda.dll nor nvrtc*.dll is linked at build time: the project keeps
// building with MinGW (no MSVC, no .lib import libraries) and simply falls back
// to the CPU backend when the DLLs are missing. The signatures below mirror
// cuda.h / nvrtc.h, which are only needed for the enum values and handle types.

#if defined(SPH_HAS_CUDA)

#include <cuda.h>
#include <nvrtc.h>
#include <string>

namespace sphcuda {

struct DriverApi {
    // driver
    CUresult (*Init)(unsigned int) = nullptr;
    CUresult (*DeviceGetCount)(int*) = nullptr;
    CUresult (*DeviceGet)(CUdevice*, int) = nullptr;
    CUresult (*DeviceGetName)(char*, int, CUdevice) = nullptr;
    CUresult (*DeviceGetAttribute)(int*, CUdevice_attribute, CUdevice) = nullptr;
    CUresult (*DevicePrimaryCtxRetain)(CUcontext*, CUdevice) = nullptr;
    CUresult (*DevicePrimaryCtxRelease)(CUdevice) = nullptr;
    CUresult (*CtxSetCurrent)(CUcontext) = nullptr;
    CUresult (*CtxSynchronize)() = nullptr;
    CUresult (*ModuleLoadDataEx)(CUmodule*, const void*, unsigned int, CUjit_option*, void**) = nullptr;
    CUresult (*ModuleUnload)(CUmodule) = nullptr;
    CUresult (*ModuleGetFunction)(CUfunction*, CUmodule, const char*) = nullptr;
    CUresult (*MemAlloc)(CUdeviceptr*, size_t) = nullptr;
    CUresult (*MemFree)(CUdeviceptr) = nullptr;
    CUresult (*MemAllocHost)(void**, size_t) = nullptr;
    CUresult (*MemFreeHost)(void*) = nullptr;
    CUresult (*MemGetInfo)(size_t*, size_t*) = nullptr;
    CUresult (*MemcpyHtoD)(CUdeviceptr, const void*, size_t) = nullptr;
    CUresult (*MemcpyDtoH)(void*, CUdeviceptr, size_t) = nullptr;
    CUresult (*MemsetD32)(CUdeviceptr, unsigned int, size_t) = nullptr;
    CUresult (*MemsetD32Async)(CUdeviceptr, unsigned int, size_t, CUstream) = nullptr;
    CUresult (*LaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                             unsigned, unsigned, unsigned,
                             unsigned, CUstream, void**, void**) = nullptr;
    CUresult (*GetErrorName)(CUresult, const char**) = nullptr;
    CUresult (*GetErrorString)(CUresult, const char**) = nullptr;
    CUresult (*StreamCreate)(CUstream*, unsigned int) = nullptr;
    CUresult (*StreamDestroy)(CUstream) = nullptr;
    CUresult (*StreamSynchronize)(CUstream) = nullptr;
    CUresult (*EventCreate)(CUevent*, unsigned int) = nullptr;
    CUresult (*EventRecord)(CUevent, CUstream) = nullptr;
    CUresult (*EventSynchronize)(CUevent) = nullptr;
    CUresult (*EventElapsedTime)(float*, CUevent, CUevent) = nullptr;
    CUresult (*EventDestroy)(CUevent) = nullptr;
    CUresult (*MemcpyHtoDAsync)(CUdeviceptr, const void*, size_t, CUstream) = nullptr;
    CUresult (*MemcpyDtoHAsync)(void*, CUdeviceptr, size_t, CUstream) = nullptr;

    // nvrtc
    nvrtcResult (*NvrtcCreateProgram)(nvrtcProgram*, const char*, const char*,
                                      int, const char* const*, const char* const*) = nullptr;
    nvrtcResult (*NvrtcCompileProgram)(nvrtcProgram, int, const char* const*) = nullptr;
    nvrtcResult (*NvrtcGetPTXSize)(nvrtcProgram, size_t*) = nullptr;
    nvrtcResult (*NvrtcGetPTX)(nvrtcProgram, char*) = nullptr;
    nvrtcResult (*NvrtcGetProgramLogSize)(nvrtcProgram, size_t*) = nullptr;
    nvrtcResult (*NvrtcGetProgramLog)(nvrtcProgram, char*) = nullptr;
    nvrtcResult (*NvrtcDestroyProgram)(nvrtcProgram*) = nullptr;
    nvrtcResult (*NvrtcAddNameExpression)(nvrtcProgram, const char*) = nullptr;
    nvrtcResult (*NvrtcGetLoweredName)(nvrtcProgram, const char*, const char**) = nullptr;

    bool loaded = false;
    std::string loadError;

    // Loads nvcuda.dll + the first nvrtc*.dll it can find. Returns false and
    // fills loadError on failure; safe to call more than once.
    bool Load();
    const char* ErrorName(CUresult r) const;
};

// Singleton; Load() is idempotent.
DriverApi& Api();

}  // namespace sphcuda

#endif  // SPH_HAS_CUDA
