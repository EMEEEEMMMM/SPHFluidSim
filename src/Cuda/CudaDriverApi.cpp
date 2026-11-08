#if defined(SPH_HAS_CUDA)

#include "CudaDriverApi.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace sphcuda {
namespace {

#if defined(_WIN32)
using LibHandle = HMODULE;
LibHandle OpenLib(const std::string& path) { return LoadLibraryA(path.c_str()); }
void* LibSymbol(LibHandle lib, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(lib, name));
}
#else
using LibHandle = void*;
LibHandle OpenLib(const std::string& path) {
    return dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
}
void* LibSymbol(LibHandle lib, const char* name) { return dlsym(lib, name); }
#endif

template <typename Fn>
bool Bind(LibHandle lib, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(LibSymbol(lib, name));
    return out != nullptr;
}

std::vector<std::string> NvrtcSearchDirs();

std::vector<std::string> NvrtcCandidates() {
    // Discover whatever CUDA toolkit is installed instead of hard-coding a
    // version list. CUDA 12.4/12.6/13.x use nvrtc64_124_0.dll,
    // nvrtc64_126_0.dll, nvrtc64_130_0.dll, ...; a fixed list made Create()
    // fail on those machines even when CUDA_PATH was correct.
    std::vector<std::string> paths;
    for (const auto& dir : NvrtcSearchDirs()) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
#if defined(_WIN32)
            const bool matches = name.rfind("nvrtc64_", 0) == 0 &&
                                 name.size() > 4 && name.substr(name.size() - 4) == ".dll";
#else
            const bool matches = name.rfind("libnvrtc.so", 0) == 0;
#endif
            if (matches) paths.push_back(entry.path().string());
        }
    }
    // Prefer the newest toolkit if several are installed.
    std::sort(paths.begin(), paths.end(), std::greater<std::string>());
#if defined(_WIN32)
    paths.push_back("nvrtc64_123_0.dll");
    paths.push_back("nvrtc64_122_0.dll");
    paths.push_back("nvrtc64_121_0.dll");
    paths.push_back("nvrtc64_120_0.dll");
    paths.push_back("nvrtc.dll");
#else
    paths.push_back("libnvrtc.so.13");
    paths.push_back("libnvrtc.so.12");
    paths.push_back("libnvrtc.so");
#endif
    return paths;
}

std::vector<std::string> NvrtcSearchDirs() {
    std::vector<std::string> dirs;
#if defined(_WIN32)
    if (const char* p = std::getenv("CUDA_PATH")) {
        dirs.push_back(std::string(p) + "\\bin");
    }
    const char* roots[] = {
        "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA",
        "D:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA",
    };
    for (const char* root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (!entry.is_directory()) continue;
            dirs.push_back(entry.path().string() + "\\bin");
        }
    }
#else
    if (const char* p = std::getenv("CUDA_HOME")) {
        dirs.push_back(std::string(p) + "/lib64");
        dirs.push_back(std::string(p) + "/targets/x86_64-linux/lib");
    }
    if (const char* p = std::getenv("CUDA_PATH")) {
        dirs.push_back(std::string(p) + "/lib64");
        dirs.push_back(std::string(p) + "/targets/x86_64-linux/lib");
    }
    dirs.push_back("/usr/local/cuda/lib64");
    dirs.push_back("/usr/local/cuda/targets/x86_64-linux/lib");
    std::error_code ec;
    const std::filesystem::path root = "/usr/local";
    if (std::filesystem::exists(root, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (!entry.is_directory()) continue;
            const std::string name = entry.path().filename().string();
            if (name.rfind("cuda-", 0) != 0) continue;
            dirs.push_back(entry.path().string() + "/lib64");
            dirs.push_back(entry.path().string() + "/targets/x86_64-linux/lib");
        }
    }
#endif
    return dirs;
}

}  // namespace

bool DriverApi::Load() {
    if (loaded) return true;
    if (!loadError.empty()) return false;

#if defined(_WIN32)
    LibHandle driver = OpenLib("nvcuda.dll");
#else
    LibHandle driver = OpenLib("libcuda.so.1");
#endif
    if (!driver) {
#if defined(_WIN32)
        loadError = "nvcuda.dll not found (no NVIDIA driver installed?)";
#else
        loadError = "libcuda.so.1 not found (no NVIDIA driver installed?)";
#endif
        return false;
    }

    struct DriverBinding {
        const char* name;
        void* slot;
    };
#define SPH_BIND_DRIVER(field, symbol) \
    if (!Bind(driver, symbol, field)) { loadError = std::string("missing ") + symbol; return false; }

    SPH_BIND_DRIVER(Init, "cuInit");
    SPH_BIND_DRIVER(DeviceGetCount, "cuDeviceGetCount");
    SPH_BIND_DRIVER(DeviceGet, "cuDeviceGet");
    SPH_BIND_DRIVER(DeviceGetName, "cuDeviceGetName");
    SPH_BIND_DRIVER(DeviceGetAttribute, "cuDeviceGetAttribute");
    SPH_BIND_DRIVER(DevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain");
    SPH_BIND_DRIVER(DevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease");
    SPH_BIND_DRIVER(CtxSetCurrent, "cuCtxSetCurrent");
    SPH_BIND_DRIVER(CtxSynchronize, "cuCtxSynchronize");
    SPH_BIND_DRIVER(ModuleLoadDataEx, "cuModuleLoadDataEx");
    SPH_BIND_DRIVER(ModuleUnload, "cuModuleUnload");
    SPH_BIND_DRIVER(ModuleGetFunction, "cuModuleGetFunction");
    SPH_BIND_DRIVER(MemAlloc, "cuMemAlloc_v2");
    if (!MemAlloc) SPH_BIND_DRIVER(MemAlloc, "cuMemAlloc");
    SPH_BIND_DRIVER(MemFree, "cuMemFree_v2");
    if (!MemFree) SPH_BIND_DRIVER(MemFree, "cuMemFree");
    SPH_BIND_DRIVER(MemAllocHost, "cuMemAllocHost_v2");
    if (!MemAllocHost) SPH_BIND_DRIVER(MemAllocHost, "cuMemAllocHost");
    SPH_BIND_DRIVER(MemFreeHost, "cuMemFreeHost");
    SPH_BIND_DRIVER(MemGetInfo, "cuMemGetInfo_v2");
    if (!MemGetInfo) SPH_BIND_DRIVER(MemGetInfo, "cuMemGetInfo");
    SPH_BIND_DRIVER(MemcpyHtoD, "cuMemcpyHtoD_v2");
    if (!MemcpyHtoD) SPH_BIND_DRIVER(MemcpyHtoD, "cuMemcpyHtoD");
    SPH_BIND_DRIVER(MemcpyDtoH, "cuMemcpyDtoH_v2");
    if (!MemcpyDtoH) SPH_BIND_DRIVER(MemcpyDtoH, "cuMemcpyDtoH");
    SPH_BIND_DRIVER(MemsetD32, "cuMemsetD32_v2");
    if (!MemsetD32) SPH_BIND_DRIVER(MemsetD32, "cuMemsetD32");
    SPH_BIND_DRIVER(MemsetD32Async, "cuMemsetD32Async");
    SPH_BIND_DRIVER(LaunchKernel, "cuLaunchKernel");
    SPH_BIND_DRIVER(GetErrorName, "cuGetErrorName");
    SPH_BIND_DRIVER(GetErrorString, "cuGetErrorString");
    SPH_BIND_DRIVER(StreamCreate, "cuStreamCreate");
    SPH_BIND_DRIVER(StreamDestroy, "cuStreamDestroy_v2");
    if (!StreamDestroy) SPH_BIND_DRIVER(StreamDestroy, "cuStreamDestroy");
    SPH_BIND_DRIVER(StreamSynchronize, "cuStreamSynchronize");
    SPH_BIND_DRIVER(EventCreate, "cuEventCreate");
    SPH_BIND_DRIVER(EventRecord, "cuEventRecord");
    SPH_BIND_DRIVER(EventSynchronize, "cuEventSynchronize");
    SPH_BIND_DRIVER(EventElapsedTime, "cuEventElapsedTime");
    SPH_BIND_DRIVER(EventDestroy, "cuEventDestroy_v2");
    if (!EventDestroy) SPH_BIND_DRIVER(EventDestroy, "cuEventDestroy");
    SPH_BIND_DRIVER(MemcpyHtoDAsync, "cuMemcpyHtoDAsync_v2");
    if (!MemcpyHtoDAsync) SPH_BIND_DRIVER(MemcpyHtoDAsync, "cuMemcpyHtoDAsync");
    SPH_BIND_DRIVER(MemcpyDtoHAsync, "cuMemcpyDtoHAsync_v2");
    if (!MemcpyDtoHAsync) SPH_BIND_DRIVER(MemcpyDtoHAsync, "cuMemcpyDtoHAsync");
#undef SPH_BIND_DRIVER

    LibHandle nvrtc = nullptr;
    for (const auto& path : NvrtcCandidates()) {
        nvrtc = OpenLib(path);
        if (nvrtc) break;
    }
    if (!nvrtc) {
        loadError = "nvrtc64_*.dll not found (install the CUDA toolkit or add its bin to PATH)";
        return false;
    }

#define SPH_BIND_NVRTC(field, symbol) \
    if (!Bind(nvrtc, symbol, field)) { loadError = std::string("missing ") + symbol; return false; }

    SPH_BIND_NVRTC(NvrtcCreateProgram, "nvrtcCreateProgram");
    SPH_BIND_NVRTC(NvrtcCompileProgram, "nvrtcCompileProgram");
    SPH_BIND_NVRTC(NvrtcGetPTXSize, "nvrtcGetPTXSize");
    SPH_BIND_NVRTC(NvrtcGetPTX, "nvrtcGetPTX");
    SPH_BIND_NVRTC(NvrtcGetProgramLogSize, "nvrtcGetProgramLogSize");
    SPH_BIND_NVRTC(NvrtcGetProgramLog, "nvrtcGetProgramLog");
    SPH_BIND_NVRTC(NvrtcDestroyProgram, "nvrtcDestroyProgram");
    SPH_BIND_NVRTC(NvrtcAddNameExpression, "nvrtcAddNameExpression");
    SPH_BIND_NVRTC(NvrtcGetLoweredName, "nvrtcGetLoweredName");
#undef SPH_BIND_NVRTC

    loaded = true;
    return true;
}

const char* DriverApi::ErrorName(CUresult r) const {
    const char* name = nullptr;
    if (GetErrorName && GetErrorName(r, &name) == CUDA_SUCCESS && name) return name;
    return "CUDA_ERROR";
}

DriverApi& Api() {
    static DriverApi api;
    static bool once = false;
    if (!once) {
        once = true;
        api.Load();
    }
    return api;
}

}  // namespace sphcuda

#endif  // SPH_HAS_CUDA
