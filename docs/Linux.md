# Linux — build and deployment

Tested on Ubuntu x86_64 with GCC, Qt 6 and CMake; CI builds on Ubuntu 24.04.

## 1. Dependencies

```bash
sudo apt install build-essential cmake ninja-build \
    qt6-base-dev qt6-declarative-dev libassimp-dev libgl1-mesa-dev \
    ocl-icd-opencl-dev libxcb-cursor0
```

`libxcb-cursor0` is a runtime dependency of Qt 6.5+; without it the GUI stops
with "Could not load the Qt platform plugin xcb".

Optional, only for the matching feature:

| package | feature |
|---|---|
| CUDA Toolkit (`cuda.h`, `nvrtc.h`) | CUDA backend; headers only, no `nvcc` required |
| ffmpeg | headless mp4 output |
| `linuxdeploy` + `linuxdeploy-plugin-qt` | only for building the portable archive |

## 2. Configure and build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

`SPH_ENABLE_CUDA` defaults to `ON`. The backend opens `libcuda.so.1` and
`libnvrtc.so` at runtime, so only the headers are needed at build time; they are
looked up through `CUDA_PATH`, `CUDA_HOME`, `/usr/local/cuda` and `/opt/cuda`.
On a machine without the headers, configure with `-DSPH_ENABLE_CUDA=OFF`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSPH_ENABLE_CUDA=OFF
```

Outputs: `build/SPHFluidSim`, plus `build/test_cuda_parity` and
`build/benchmark_gpu` when the CUDA backend is enabled.

## 3. Run

```bash
./build/SPHFluidSim
```

The shaders are copied next to the executable at build time, so the binary can
be launched from any directory.

## 4. Tests

```bash
ctest --test-dir build --output-on-failure
```

## 5. Portable package

The archive is built with `linuxdeploy` and its Qt plugin. Both must be
executable and on `PATH`, or passed with `--linuxdeploy`:

```bash
bash deploy/linux.sh --build-dir build --qt-root /path/to/Qt/6.8.3/gcc_64
```

| option | default | meaning |
|---|---|---|
| `--build-dir DIR` | `build` | build tree holding `SPHFluidSim` |
| `--output FILE` | `dist/SPHFluidSim-v<version>-linux-x86_64.tar.gz` | archive to write |
| `--version VERSION` | root `VERSION` | version string |
| `--qt-root DIR` | `QT_ROOT_DIR` or qmake's prefix | Qt installation |
| `--cuda-root DIR` | `/usr/local/cuda` | source of `libnvrtc.so*` |
| `--linuxdeploy PATH` | `linuxdeploy` on `PATH` | linuxdeploy executable |

The script stages an AppDir, runs `linuxdeploy --plugin qt` to collect Qt
libraries, plugins and QML modules, adds the OpenCL ICD loader and the NVRTC
runtime, writes a relocatable `qt.conf`, strips symbols and packs the result.

Contents of the archive (top-level `SPHFluidSim-v<version>-linux-x86_64/`):

```
AppRun
SPHFluidSim.desktop
usr/bin/SPHFluidSim
usr/bin/shaders/
usr/lib/          Qt, assimp, OpenCL, NVRTC
usr/plugins/      Qt platform / image / QML plugins
usr/bin/qt.conf
README.md
```

Run `./AppRun` from the extracted folder. Launching `usr/bin/SPHFluidSim`
directly also works because of `qt.conf`, but AppRun sets the library paths.

The AppDir does not bundle glibc: build the archive on a distribution no newer
than the oldest one you intend to support.

## 6. Target machine

- **CUDA**: needs the NVIDIA display driver (`libcuda.so.1`). The driver is not
  redistributable and is not bundled; without it the app starts on the CPU
  backend. NVRTC (`libnvrtc.so*`, `libnvrtc-builtins.so*`) is bundled, because
  the kernels are compiled at runtime.
- **OpenCL**: the ICD loader is packaged; a vendor ICD is still required for GPU
  surface reconstruction, otherwise the CPU path is used.
- **ffmpeg** is not bundled. Put it on `PATH` for `--headless` video output.

## 7. Running on a GPU server

The Windows `.exe` is a native PE binary and cannot be copied to a Linux
machine. Build on the server with the commands above. The server needs a driver
plus NVRTC (either the packaged `libnvrtc.so`, or a CUDA runtime installed
system-wide); `nvcc` is not needed at build time, and the build itself only
needs the two CUDA headers.

Headless rendering opens no window. From the extracted package:

```bash
./AppRun --headless --seconds 10 --output demo --backend cuda
```

## 8. Troubleshooting

| symptom | cause / fix |
|---|---|
| `Could not load the Qt platform plugin "xcb"` | Missing runtime libraries. Install `libxcb-cursor0`, or run the packaged `AppRun`, which carries the Qt plugins. |
| `module "QtQuick.Controls" is not installed` | Qt Quick modules are not deployed. Run the packaged `AppRun`, or install `qt6-declarative-dev` when running from the build tree. |
| `[headless] backend 'cuda' unavailable: ...` | No NVIDIA driver (`libcuda.so.1`) or no NVRTC on this machine. Use `--backend cpu`/`auto`, or install the driver/runtime. |
| `linuxdeploy-plugin-qt not found` | Put `linuxdeploy-plugin-qt` next to `linuxdeploy` on `PATH`. |
