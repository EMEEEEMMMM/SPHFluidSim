# IISPH — Reproduction of "Implicit Incompressible SPH"

A from-scratch, real-time reproduction of **Implicit Incompressible SPH** (Ihmsen et al., IEEE TVCG 2013) in C++17 + OpenGL 3.3 + ImGui + OpenMP, running on Windows (MinGW + CMake).

The solver, the surface reconstruction and the rendering follow the paper's reference implementation (SPlisHSPlasH) formula-for-formula. The focus is **physical correctness** (density error < 0.01% like the paper, relaxed-Jacobi pressure iterations) at interactive speed (33k particles ~ 14 ms/step on a desktop CPU, 20 threads).

---

## Features

- **IISPH solver** (paper Alg. 1): implicit density constraint via relaxed-Jacobi pressure solve (`omega = 0.5`, negative pressure clamped to 0, warm start `p^0 = 0.5 p(t-dt)`), CFL step limiter (`dt <= 0.4 d/vmax`, paper Sec.3.1), single-layer boundary particles with Akinci-2012 volumes, XSPH viscosity (Schechter & Bridson 2012, as in SPlisHSPlasH) and surface tension (Akinci et al. 2013: color-field normals + cohesion + curvature + boundary adhesion)
- **Water surface reconstruction** (Akinci et al. 2012): color-field surface particles, distance-to-weighted-centroid scalar field `phi = |x - xbar| - r`, Marching Cubes lookup tables with shared-edge intersection reuse (watertight, crack-free), opaque fresnel water shader (deep-blue gradient + sun glint)
- **Parallelism** (Ihmsen et al. 2011 style): SoA spatial hash built in 3 passes — atomic count -> prefix sum -> atomic scatter; no locks, cache-friendly neighbor slices; reconstruction runs on a background thread (never stalls the render loop)
- **ImGui control panel**: pause / single step / reset, resolution presets, solver parameters, surface parameters, particles/surface/both view modes, FPS + tank label overlay (top-left)
- **Headless mode**: offline simulation -> offscreen render -> ffmpeg streaming encode (frames piped straight to the encoder, no BMPs on disk; ~500 MB mp4 instead of ~90 GB of BMPs for a 14400-frame 1080p run)

---

## Build & run (Windows)

Requirements: CMake, MinGW (g++ with OpenMP), GLFW/GLAD/ImGui in `thirdparty/` (already vendored).

```powershell
mkdir build && cd build
cmake .. && cmake --build .
IISPH.exe
```

The default configuration is **Debug**. For the fast build (recommended for the real-time demo; Debug is ~5x slower), configure a Release build explicitly:

```powershell
cd build
cmake -DCMAKE_BUILD_TYPE=Release .   # switch to Release
cmake --build .                      # recompile (takes a while)
IISPH.exe
```

This uses a single-configuration generator (MinGW Makefiles), so the build type is baked into the configured directory — to go back to Debug, run `cmake -DCMAKE_BUILD_TYPE=Debug .` and rebuild. (The `--config Release` flag only works with multi-config generators such as Visual Studio / Ninja Multi-Config, not here.)

### GUI controls (ImGui panel)

- **Pause / Step once / Reset** — inspect the dam break step by step
- **Scene** — dam break (open-top) / inflow jet
- **Spawn water drop** (dam scene) — one-shot button: spawns a spherical blob above the open tank rim that falls into the water
- **Resolution** — low (8k) .. ultra (64k) particles; changing it resets the scene
- **View** — particles / surface / both
- **Surface** — threshold (0.8), grid scale (1.0 fine .. 2.0 fast), rebuild interval
- Camera: right-drag to look, WASD to move, scroll to zoom

---

## Headless mode (offline video)
Make sure that ffmpeg is installed on your computer.

Frames are streamed directly into ffmpeg over a pipe (rawvideo), so **no BMP
files are written** and the only disk usage is the final mp4. BMP frames are
only written with `--no-video` (debug / manual encode) or `--keep-frames`.

```powershell
IISPH/build/IISPH.exe --headless --seconds 10 --output drop_demo
# -> drop_demo/output.mp4 only (no frames/); default scene = dam break (open-top)
```

### Scenes (`--scene`)

| scene | description |
|---|---|
| `dam` (default) | classic dam break in an **open-top** 2.0x1.2x1.0 m tank (no lid, splash may leave the tank); optional `--drop <N>[f|s]` spawns a water drop above the rim |
| `inflow` | open-top 1.0 m cubic container with a continuous central jet falling 0.5 m from above (injects until `--particles` is reached, then stops - fixed volume) |

All scenes render their container/tank as a wireframe outline.

**Fixed water volume**: each scene defines a physical water volume (dam 0.512 m^3, inflow 0.6 m^3; the optional drop adds a fixed 0.034 m^3 sphere); `--particles N` derives the spacing from
`spacing = (volume / N)^(1/3)`, so 8000 or 16000 particles show the *same*
water volume - only the resolution changes.

### Options

| option | default | description |
|---|---|---|
| `--headless` | — | run without a window (video output) |
| `--frames N` | 300 | number of simulated frames |
| `--seconds S` | — | video length in seconds (frames = `S * fps`; overrides `--frames`) |
| `--fps N` | 30 | video frame rate |
| `-r WxH` | 1280x720 | render resolution |
| `--output PATH` | `out` | output directory (`output.mp4`, or `frames/` with `--no-video`/`--keep-frames`) |
| `--mode MODE` | `surface` | `particles` (points) or `surface` (reconstructed mesh) |
| `--particles N` | 8000 | target number of fluid particles (spacing = (scene water volume / N)^(1/3)) |
| `--scene S` | `dam` | `dam` \| `inflow` (see Scenes above) |
| `--drop N[f/s]` | off | spawn the one-shot water drop at frame N, or N seconds in (`20f`/`20s`; dam only) |
| `--dt T` | auto (CFL) | fixed time step in seconds |
| `--grid S` | 1.0 | surface grid scale (1.0 = paper r, 0.5 = fine/high quality) |
| `--threads N` | all cores | OpenMP thread count |
| `--no-video` | off | write BMP frames only, skip ffmpeg encode (debug / manual encode) |
| `--keep-frames` | off | write BMP frames too and keep them after encoding (old behaviour) |
| `--bench NAME` | — | run a validation experiment and exit (see below) |

### Examples

```powershell
# default scene (dam break, open-top): 0.8 m water cube collapses, 10 s @ 30 fps, 1080p HQ surface
IISPH.exe --headless --seconds 10 --fps 30 -r 1920x1080 --grid 0.5 --output hi

# dam break with a water drop spawned at 3 s into the video
IISPH.exe --headless --seconds 10 --scene dam --drop 3s --output dam

# inflow jet (strong central jet, falls 0.5 m)
IISPH.exe --headless --seconds 15 --scene inflow --particles 12000 --output jet

# particle-points video, fixed dt
IISPH.exe --headless --seconds 20 --mode particles --particles 33000 --dt 0.002 --output pv

# render frames only (no ffmpeg), then encode manually
IISPH.exe --headless --frames 120 --no-video --output raw
ffmpeg -framerate 30 -i raw/frames/frame_%04d.bmp -pix_fmt yuv420p raw.mp4
```

---

## Validation experiments (`--bench`)

Run headless and read the verdict line:

```powershell
IISPH/build/IISPH.exe --bench hydrostatic   # pressure grows linearly with depth (slope ~ -9.81)
IISPH/build/IISPH.exe --bench dambreak      # wave front advance, density error < 2%, iters vs paper
IISPH/build/IISPH.exe --bench energy        # KE + PE tracked; no blow-up, density < 2%, no particle loss
```

Latest numbers (this machine, 20 threads):

| bench | result |
|---|---|
| hydrostatic (64k particles) | pressure-depth slope -10.6 .. -11.6 (theory -9.81; SPH noise at 0.025 spacing, criterion < -2) |
| dambreak | peak avg density error 0.30% (paper ~0.1% converged), avg 2.8 pressure iterations (paper Table 1: 2.2-33.5) |
| energy | KE monotonically decays via viscosity; no NaN, no escapes, density < 2% |
| performance | 33k particles ~ 14 ms/sim-step; reconstruction ~20 ms @ grid 1.0, ~100 ms @ grid 0.5 (background thread) |

---

## Headless tests

| target | what it verifies |
|---|---|
| `test_solver` | dam break: density error < 5%, no particle escapes |
| `verify_hydrostatic` | hydrostatic: pressure grows linearly with depth, volume conserved |
| `surface_reconstruct` | surface mesh: non-empty, normals mostly outward, timing |
| `benchmark` | one-step convergence trace, iterations vs dt, strong scaling |

```powershell
IISPH/build/test_solver.exe 1000
IISPH/build/verify_hydrostatic.exe 300
IISPH/build/surface_reconstruct.exe
```

---

## References

Five papers this reproduction is built on. `[n]` is the citation index used in the IISPH paper.

1. **M. Ihmsen, J. Cornelis, B. Solenthaler, C. Horvath, and M. Teschner,** "Implicit incompressible SPH," *IEEE Transactions on Visualization and Computer Graphics*, vol. 20, no. 3, pp. 426-435, 2014. (main paper, also listed as TVCG 2013 early access). DOI: 10.1109/TVCG.2013.105
2. **J. J. Monaghan,** "Smoothed particle hydrodynamics," *Reports on Progress in Physics*, vol. 68, no. 8, pp. 1703-1759, 2005. [23] DOI: 10.1088/0034-4885/68/8/R01
3. **M. Becker and M. Teschner,** "Weakly compressible SPH for free surface flows," in *Proc. ACM SIGGRAPH/Eurographics Symposium on Computer Animation (SCA)*, San Diego, CA, 2007, pp. 209-217. [5] DOI: 10.5555/1272690.1272719 (ACM legacy DOI; confirmed via DBLP)
4. **G. Akinci, M. Ihmsen, N. Akinci, and M. Teschner,** "Parallel surface reconstruction for particle-based fluids," *Computer Graphics Forum*, vol. 31, no. 6, pp. 1797-1809, 2012. [32] DOI: 10.1111/j.1467-8659.2012.02096.x (Note: the citation printed in the IISPH paper — "vol. 32, no. 1, pp. 99-112" — is garbled; 99-112 belongs to the Ihmsen 2011 paper below. CrossRef confirms the record above.)
5. **M. Ihmsen, N. Akinci, M. Becker, and M. Teschner,** "A parallel SPH implementation on multi-core CPUs," *Computer Graphics Forum*, vol. 30, no. 1, pp. 99-112, 2011. [30] DOI: 10.1111/j.1467-8659.2010.01832.x

---

## Notes on my blog
The notes I took when I was working on this project are all on [my blog](https://emeeeemmmm.github.io/).

Deepseek v4 Flash was used during this project.
