## Highlights

- Added an optional **CUDA backend** for both **IISPH** and **DFSPH**.
- Added **Auto / CPU / CUDA** backend selection in the GUI and `--backend` in headless mode.
- Removed the old Low/Medium/High/Ultra presets in favour of a direct **particle-count input**.
- Removed the application-level particle cap. Large CUDA requests are checked against the active GPU budget when entered; the main window and both render dialogs show a red preflight error instead of starting an impossible scene.
- Added CUDA correctness and performance tooling: `test_cuda_parity` and `benchmark_gpu`.
- Fixed CUDA particle rendering so velocity colours match the CPU backend.

## CUDA backend

- GPU implementations for IISPH and DFSPH using the same boundary, XSPH viscosity and surface-tension model as the CPU reference.
- Kernels are compiled from embedded CUDA source at runtime with **NVRTC** and launched through the **CUDA Driver API**. No `nvcc` or CUDA import library is required at build time.
- Runtime discovery of CUDA/NVRTC from CUDA 11 through 13.
- Windows (`nvcuda.dll`, `nvrtc64_*.dll`) and Linux (`libcuda.so.1`, `libnvrtc.so`) loading paths.
- Particle state stays resident on the device. Accessors synchronise only the requested field, so a solver-only loop avoids per-step device-to-host copies.
- Device-side spatial hash construction: count, prefix scan, scatter and sort all run on the GPU.
- Each fluid pair is discovered once and mirrored into both neighbour lists; the neighbour index is packed into the per-pair gradient record.
- Per-pair gradients and distances are cached once per neighbourhood rebuild and reused by density, IISPH pressure iterations and both DFSPH solvers.
- DFSPH reuses its end-of-step neighbourhood, density and stiffness at the start of the next step, eliminating one full rebuild and one density pass per step.
- Paired DFSPH passes are fused, including density + stiffness, integrate + wall clamp, and final velocity + warm start.
- Small reductions and bounds readbacks use pinned host staging.
- `SPH_CUDA_DEVICE=N`, `SPH_CUDA_PROFILE=1`, `SPH_CUDA_KERNEL_PROFILE=1` and `SPH_CUDA_DUMP_PTX=path` are available for device selection and profiling.
- Explicit CUDA requests fail with a diagnostic when CUDA is unavailable; `Auto` falls back to the CPU solver and reports why.

## Performance

Reference machine: Intel Core i7-12700F (20 OpenMP threads), Quadro P600 2 GB, Windows/WDDM, CUDA 12.1 NVRTC, Release build. Times are ms per solver step after warm-up. The CUDA column is solver-only; the interactive path additionally reads positions and colours back to the host.

| Workload | CPU | CUDA | CPU / CUDA |
|---|---:|---:|---:|
| IISPH 8k | 30.3 ms | 12.2 ms | 2.5x |
| IISPH 32.8k | 92.1 ms | 39.1 ms | 2.4x |
| DFSPH 32.8k | 23.5 ms | 10.0 ms | 2.3x |
| DFSPH 125k | 75.6 ms | 33.2 ms | 2.3x |

These are reference measurements for the Quadro P600, not predictions for other GPUs. The P600 has only 3 SMs and roughly 64 GB/s of memory bandwidth, and it also drives the desktop, so absolute timings vary with display load. IISPH can run 100 pressure iterations during the initial impact and settle to 8–10 later, so short and long measurements are not directly comparable. Run `benchmark_gpu` on the target machine rather than scaling from the P600 numbers.

## Particle count and desktop UI

- The resolution dropdown has been replaced by a numeric **Particles** field in both Qt Quick and Qt Widgets frontends.
- There is no application-level upper cap. The field accepts any count the solver can represent; the backend decides whether it can run.
- On CUDA, the requested count is checked against the active GPU budget immediately after entry. If it does not fit, the field and warning turn red, and no rebuild is started. The running scene is left untouched.
- The same preflight is used by the Qt Quick and Qt Widgets render dialogs; **Start** stays disabled while the requested scene exceeds the GPU budget.
- Spacing is derived from the scene's fixed water volume. The panel shows the resulting spacing and estimated device memory versus free VRAM.
- The **Active** row reports the particle count actually built. The requested count is a target: a non-cubic count can round up to the lattice, for example 500,000 → 512,000.
- Changing a valid count asks for confirmation before rebuilding the scene.
- Added the **Auto / CPU / CUDA** switch to the solver panel.
- CUDA particle colours now follow the same velocity-based blue/white ramp as the CPU backend, including the initial paused frame and scenes rebuilt after an inflow or drop event.

## Headless and CLI

- Added `--backend auto|cpu|cuda` to the headless runner and help text.
- Headless mode now uses the same backend factory, particle budget check and spacing derivation as the GUI.
- The `particles=` line reports both the requested target and the actual count built.
- A scene that does not fit in VRAM fails with a readable message and exit code 1 before any simulation work starts.
- Existing `--particles`, `--solver`, `--scene` and `--config` flows remain supported.

## Build and platform

- `SPH_ENABLE_CUDA` is enabled by default when `cuda.h` and `nvrtc.h` are found.
- CUDA is linked only through the Driver API at runtime, so a normal MinGW build still works without CUDA import libraries.
- Added Linux CUDA include/library discovery and `dlopen` support; the Windows path is the one tested on the reference machine.
- Single-config generators now default to **Release** when no build type is passed, avoiding accidentally unoptimised CPU baselines and misleading CPU/CUDA ratios.
- CUDA device buffers, modules, streams and the primary context are released when the solver is destroyed or the scene/backend is rebuilt.

## Correctness and tests

- Added `test_cuda_parity`, covering CPU/GPU agreement for IISPH and DFSPH over 20 steps, including position, velocity and density tolerances.
- Added colour-mirror checks for the initial frame, after simulation steps, and after inflow/drop scene mutations.
- Added a GPU memory-budget test that verifies an oversized request is refused without touching the running scene.
- Added a parameter-JSON regression proving a 10,000,000-particle request is not capped during import.
- Added `benchmark_gpu` for solver-only and render-synchronised timings at 8k, 32.8k and 125k particles.
- CTest now registers 11 cases. On the P600 reference machine, 10/10 pass with the pre-existing `timeline` bitwise-equality test excluded.

## Fixed

- Fixed CUDA GUI/headless particles rendering as black. The colour kernel result was marked clean before the host mirror was copied back, so the renderer always received `(0, 0, 0)`.
- Fixed the initial paused CUDA frame not generating velocity colours.
- Fixed colour generation after CUDA scene rebuilds, inflow injection and `SpawnDrop`.
- Fixed CUDA resource retention when switching scenes or recreating the solver.
- Fixed neighbour-list overflow being able to truncate silently; the backend reports the overflow and uses 64 fluid / 48 boundary slots.
- A rejected CUDA scene no longer marks the solver as broken or stops the current simulation.

## Upgrade notes

- Rebuild is required.
- CUDA users need an NVIDIA driver and an NVRTC runtime; `nvcc` is not required at build time.
- If CUDA headers are absent, configure with `-DSPH_ENABLE_CUDA=OFF` or let automatic detection disable the backend.
- Existing parameter JSON files and command-line options remain compatible.
- On CUDA, results can differ from the CPU reference in the final floating-point bits because mirrored neighbour insertion uses atomic slot allocation. The CPU backend remains deterministic.

## Known limitations

- Particle counts remain bounded by the 32-bit solver API (`INT_MAX`), but there is no smaller application-defined limit.
- A 10,000,000-particle CUDA scene needs about **18.6 GB** of device memory. It cannot run on the 2 GB Quadro P600; use a large-VRAM GPU.
- On the Quadro P600, the measured preflight budget allows roughly **860,000** particles after desktop/display allocations.
- No A100 or RTX 3090 measurements are included in this release; run `benchmark_gpu` on the target machine.
- The Linux CUDA loading path is implemented but was not validated on a Linux GPU machine in this release.
- Multi-GPU execution is not implemented.
- The existing `timeline` test still uses strict bitwise equality across frame rates and can report very small floating-point differences unrelated to CUDA.

**Full Changelog**:  
https://github.com/EMEEEEMMMM/SPHFluidSim/compare/version_1_8...version_1_9
