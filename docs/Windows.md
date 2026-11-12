# Windows — build and deployment

Tested on Windows with **MSYS2 UCRT64**, MinGW-w64 GCC, Qt 6 and CMake.

## 1. Toolchain

Install MSYS2 from <https://www.msys2.org/>, open the **UCRT64** shell and run:

```bash
pacman -S --needed \
    mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-libgomp \
    mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja \
    mingw-w64-ucrt-x86_64-pkgconf \
    mingw-w64-ucrt-x86_64-qt6-base \
    mingw-w64-ucrt-x86_64-qt6-declarative \
    mingw-w64-ucrt-x86_64-assimp
```

`libgomp` is an optional dependency of the GCC package: without it
`-fopenmp` compiles but does not link. `qt6-base` provides the Qt libraries,
`qt6-declarative` Qt Quick and Quick Controls.

Optional packages, only needed for the matching feature:

| package | feature |
|---|---|
| `mingw-w64-ucrt-x86_64-qt6-tools` | `windeployqt`, if it is not already on `PATH` |
| `mingw-w64-ucrt-x86_64-opencl-headers` + `mingw-w64-ucrt-x86_64-opencl-icd` | OpenCL surface reconstruction (CPU path is used otherwise) |
| CUDA Toolkit (`cuda.h`, `nvrtc.h`) | CUDA backend; headers only, no `nvcc` required |
| ffmpeg on `PATH` | headless mp4 output |

The CUDA Toolkit is not an MSYS2 package. Install it normally (or point
`CUDA_PATH` at an unpacked toolkit); CMake only looks for the two headers.

## 2. Configure and build

From the repository root, in a normal PowerShell (the compiler and Qt are found
through `CMAKE_PREFIX_PATH`):

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH="C:/msys64/ucrt64"
cmake --build build --parallel
```

If MSYS2 is installed elsewhere, change the prefix. When Qt 6 is already on a
standard path the project also probes `D:/msys64/ucrt64` and `C:/msys64/ucrt64`,
so the option may be omitted. The build is single-configuration:
`CMAKE_BUILD_TYPE` is fixed at configure time.

| option | default | effect |
|---|---|---|
| `-DSPH_ENABLE_CUDA=ON/OFF` | `ON` | compile the CUDA backend when `cuda.h`/`nvrtc.h` are found |
| `-DCMAKE_BUILD_TYPE=Release/Debug` | — | optimisation level |
| `-DCMAKE_PREFIX_PATH=<qt6-prefix>` | autodetect | Qt 6 installation |

Outputs: `build/SPHFluidSim.exe`, plus `build/test_cuda_parity.exe` and
`build/benchmark_gpu.exe` when the CUDA backend is enabled.

## 3. Run from the build tree

```powershell
.\build\SPHFluidSim.exe
```

Configure also writes `build/qt.conf`, which points Qt at the QML modules of the
installation given by `CMAKE_PREFIX_PATH`. This matters for MSYS2 packages,
which are compiled for `C:/msys64` and cannot find their plugins when MSYS2 lives
on another drive. If the configure log warns that the QML directory was not
found, set `QML2_IMPORT_PATH` to the Qt installation's QML directory
(`<prefix>/qml` or `<prefix>/share/qt6/qml`).

## 4. Tests

```powershell
ctest --test-dir build --output-on-failure
```

## 5. Portable package

`windeployqt` alone is not enough for a MinGW build: it handles Qt but not the
MinGW runtime, Assimp, OpenCL, NVRTC or the `qt.conf` that makes the copied Qt
tree relocatable. Use the packaging script:

```powershell
powershell -ExecutionPolicy Bypass -File .\deploy\windows.ps1
```

It stages the executable, Qt runtime / plugins / QML modules (via
`windeployqt`), the GLSL shaders, `qt.conf`, the MinGW UCRT64 runtime, Assimp,
the OpenCL ICD loader (when present) and the redistributable NVRTC DLLs, then
compresses the folder.

Output: `dist/SPHFluidSim/` and `dist/SPHFluidSim-v<version>-win64.zip`, where
`<version>` comes from the root `VERSION` file.

| parameter | default | meaning |
|---|---|---|
| `-BuildDir DIR` | `build` | build tree holding `SPHFluidSim.exe` |
| `-Output FILE` | `dist/SPHFluidSim-v<version>-win64.zip` | archive to write |
| `-StageDir DIR` | `dist/SPHFluidSim` | staging folder (zip root) |
| `-QtBin DIR` | auto (`Qt6_DIR`, then `PATH`) | folder with `windeployqt.exe` |
| `-Version STR` | root `VERSION` | version string |
| `-Msys2Bin DIR` | same as `-QtBin` | folder with the MinGW/Assimp/OpenCL DLLs |
| `-CudaBin DIR` | `CUDA_PATH\bin` | folder with `nvrtc64_*.dll` |

Ship the **whole folder or zip**. Copying only `SPHFluidSim.exe` to another
machine fails with missing-DLL or missing-QML-plugin errors.

## 6. Target machine

- No MSYS2 installation is required; the MinGW runtime is inside the package.
- **CUDA**: needs the NVIDIA display driver, which provides `nvcuda.dll`. The
  driver is not redistributable and is not bundled. Without it the app starts on
  the CPU backend. NVRTC (`nvrtc64_*.dll`, `nvrtc-builtins64_*.dll`) is bundled,
  because the kernels are compiled at runtime.
- **OpenCL**: `OpenCL.dll` is bundled when the ICD loader was available at
  packaging time. Missing it only disables GPU surface reconstruction.
- **ffmpeg** is not bundled. Put it on `PATH` for `--headless` video output;
  without it the runner writes BMP frames.

## 7. Troubleshooting

| symptom | cause / fix |
|---|---|
| `module "QtQuick.Controls" plugin "qtquickcontrols2plugin" not found` | `build/qt.conf` missing or pointing at the wrong prefix. Re-run CMake with the correct `CMAKE_PREFIX_PATH`, or set `QML2_IMPORT_PATH`. |
| `The code execution cannot proceed because libgcc_s_seh-1.dll was not found` | Only the .exe was copied. Use the package folder; the MinGW runtime is next to the executable there. |
| Configure prints `CUDA backend: disabled` | `cuda.h`/`nvrtc.h` not found. Install the toolkit or set `CUDA_PATH`, then delete `build/CMakeCache.txt` and reconfigure. |
| `OpenCL not found — GPU surface reconstruction disabled` | Optional. Install `mingw-w64-ucrt-x86_64-opencl-icd` + `opencl-headers`, or ignore the warning and use the CPU path. |
| ffmpeg missing during `--headless` | The runner silently writes BMP frames instead of an mp4. Install ffmpeg and put it on `PATH`, or encode the BMP frames with ffmpeg afterwards. |

The packaging script is the same one CI uses, so a local run and a release build
produce the same layout.
