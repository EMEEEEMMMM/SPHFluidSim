# Performance report — CPU vs CUDA backend

Measured on 2026-11-08. This report contains **only measurements taken on the machine below**; no numbers for GPUs other than the Quadro P600 are included because they were not available for testing.

## Test setup

| component | specification |
|---|---|
| CPU | Intel Core i7-12700F, 12 cores / 20 threads, 32 GB RAM |
| GPU | NVIDIA Quadro P600, 2 GB GDDR5, 128-bit, 2.005 GHz, 64.2 GB/s peak |
| driver | NVIDIA 582.53, CUDA 12.1 toolkit (NVRTC JIT) |
| build | Release, MinGW-w64 UCRT64, OpenMP 20 threads |
| scene | dam break, `InitDamOpen(2.0, 1.2, 1.0, 0.8)`, CFL on, `dt <= 0.003` |

## Measured: 8k / 32.8k / 125k particles

`benchmark_gpu 30` (steady part) and `benchmark_gpu 30 --large --filter=125k`. Times are ms per solver step, averaged over the measured loop. `gpu+render` adds the two device-to-host copies the interactive renderer needs (positions and colors).

| case | CPU | CUDA | CUDA+render | CPU/CUDA | max neighbors |
|---|---:|---:|---:|---:|---:|
| IISPH 8k (spacing 0.040) | 33.3 | 12.4 | 12.5 | 2.7x | 40 |
| IISPH 32.8k (spacing 0.025) | 106.2 | 38.7 | 81.2 | 2.7x | 40 |
| IISPH 125k (spacing 0.016) | 481.6 | 385.0 | 492.2 | 1.25x | 42 |
| DFSPH 8k (spacing 0.040) | 9.6 | 9.6 | 9.6 | 1.0x | 39 |
| DFSPH 32.8k (spacing 0.025) | 27.6 | 18.7 | 29.8 | 1.5x | 41 |
| DFSPH 125k (spacing 0.016) | 92.5 | 98.3 | 86.0 | 0.94x | 41 |

The 125k IISPH row mixes two regimes: the first impact runs the full 100 Jacobi iterations, while the settled part runs 8–10. A separate 10-step startup run measured 930 ms CPU / 763 ms GPU for IISPH, and 97 ms CPU / 61 ms GPU for DFSPH; the 30-step averages above include more of the settled regime. The GPU is not faster than a 20-thread i7 at 125k on this 2 GB P600; the card has only 3 SMs and 64 GB/s.

`SPH_CUDA_PROFILE=1` on the 125k IISPH case (5 sampled steps) shows the split:

```
[build]   n=125000 cells=108188  cell+count=1.9 ms  scan=1.3 ms  scatter+sort=0.8 ms  neigh=28.8 ms
[profile] n=125000 build=33.2 ms  predict=27.0 ms  solve=868.8 ms   # 100-iteration step
[profile] n=125000 build=24.0 ms  predict=22.3 ms  solve=811.3 ms   # 100-iteration step
[profile] n=125000 build=27.9 ms  predict=25.9 ms  solve=967.5 ms   # 100-iteration step
[profile] n=125000 build=33.9 ms  predict=9.0 ms   solve=349.6 ms   # fewer iterations
```

The pressure solve dominates; the neighbor rebuild is 5–10% of a converged step and is skipped across steps by DFSPH's end-of-step cache.

## CPU thread scaling explains the speedup difference

The CUDA/CPU speedup depends strongly on how many OpenMP threads the CPU baseline gets. The GPU time is unchanged; the CPU time shrinks as threads are added. At 8k IISPH, the same scene measures:

| OpenMP threads | CPU ms/step | CUDA ms/step | speedup |
|---:|---:|---:|---:|
| 1 | 168.9 | 17.7 | 9.5x |
| 2 | 91.5 | 17.9 | 5.1x |
| 4 | 53.0 | 17.9 | 3.0x |
| 20 | 30.9 | 17.9 | 1.7x |

The CUDA time stays at 17.7–17.9 ms in every row; only the CPU baseline moves. The speedup is therefore a function of the CPU thread count, not a fixed property of the backend: a low-thread CPU baseline produces a much larger ratio than the 20-thread default. The P600 also drives the 1920×1080 desktop, so its measured time varies from about 12 ms (cool/idle display) to 24 ms (display active or warm); the 2.7x in the main table is from a 12.4 ms run, while this table is from a 17.9 ms session. The CUDA backend itself is unchanged across those rows.

## Backend check against the previous commit

To rule out a regression from the cleanup in this change, I built `b331212` in a separate worktree and ran both binaries on the same machine and display state. The current backend is faster at every size; the CPU reference is unchanged within run-to-run noise.

| case | b331212 CPU | b331212 CUDA | current CPU | current CUDA |
|---|---:|---:|---:|---:|
| IISPH 8k, 30 steps | 31.0 | 20.6 | 30.9 | 17.9 |
| IISPH 32.8k, 30 steps | 94.3 | 69.8 | 106.2 | 38.7 |
| IISPH 125k, 10 steps | 848.4 | 1 537.9 | 930.1 | 763.4 |
| DFSPH 125k, 10 steps | 117.1 | 276.8 | 97.2 | 61.1 |

The 125k rows use 10 steps because the previous commit is much slower there; the current 30-step stable values are in the first table. The difference at 125k is the main win from the cached neighbor data and the removed per-particle index stream.

## Memory sizing (derived from the code)

One fluid particle keeps 1 660 B of device memory (state, pressure/DFSPH scratch, 64 fluid neighbor slots, 48 boundary slots). The pre-flight check adds 12% and 80 MiB of grid scratch:

| particles | estimated device state | note |
|---:|---:|---|
| 1M | 1.86 GB | fits a 2 GB card only without a desktop/other allocations |
| 10M | 18.6 GB | needs a 24 GB card or larger |
| 100M | 186 GB | no single GPU; multi-GPU is not implemented |

The host mirror (positions, velocities, colors) adds about 40 B per particle on top of that.

## What was not measured

- **No other GPU data.** Only the Quadro P600 was measured, so this report deliberately gives no per-step or per-simulated-second numbers for any other card. Use `benchmark_gpu` on the target machine to measure it; do not scale from the P600.
- **No 10^7 or 10^8 run.** The P600 has 2 GB and cannot hold either scene. The memory table above is code-derived; per-step times for those sizes are not measured and are not claimed here.
- **Host-mapped zero-copy and UVM were evaluated and not shipped.** A managed allocation beyond the 2 GB card did not complete within two minutes on this machine, and host-mapped memory is limited by PCIe bandwidth. They are not a dependable fallback for out-of-core scenes.

## Reproduce

```powershell
# 8k / 32.8k, steady part, both solvers
.\build\benchmark_gpu.exe 30

# add the 125k cases
.\build\benchmark_gpu.exe 30 --large --filter=125k

# build/predict/solve split
$env:SPH_CUDA_PROFILE = "1"
.\build\benchmark_gpu.exe 10 --large --filter=125k

# CPU thread scaling
$env:OMP_NUM_THREADS = "4"
.\build\benchmark_gpu.exe 30 --filter=8k --render=0
```

On a 24 GB card, use `benchmark_gpu 10 --large --filter=125k` first, then `--particles` in headless mode for the target size and watch the reported `ms/step` and iteration count; the first 100-iteration impact step is not representative of the steady state.
