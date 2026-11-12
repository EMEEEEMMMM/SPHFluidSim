# SPHFluidSim — real-time implicit incompressible SPH

A from-scratch, real-time reproduction of **Implicit Incompressible SPH** (Ihmsen et al., IEEE TVCG 2013) and a **Divergence-Free SPH** solver (Bender & Koschier 2017), in C++17 + OpenGL 3.3 + Qt 6 + OpenMP, running on Windows and Linux. The desktop UI is available as both **Qt Widgets** and **Qt Quick/QML**, with four switchable skins.

Repository: **https://github.com/EMEEEEMMMM/SPHFluidSim**

---

<div align="center">
  <img src="./assets/output.gif" alt="演示动画" width="700">
  <br>
  <small>SPHFluidSim.exe --headless --seconds 20 --output demo_dam --fps 120 -r 1920x1080 --scene dam --drop 5s --grid 0.5 --particles 64000 --solver DFSPH</small>
</div>

## Features

- **IISPH and DFSPH solvers** — Implicit Incompressible SPH (Ihmsen et al., TVCG 2013) and Divergence-Free SPH (Bender & Koschier 2017), selectable in the UI or with `--solver`. Both support XSPH viscosity, surface tension and wall adhesion, and the CPU solver is deterministic: the same scene reproduces the same result.
- **Water surface** — Akinci-2012 color-field reconstruction producing a watertight Marching Cubes mesh with an opaque Fresnel shader. Reconstruction runs in the background, so the viewport stays responsive while paused.
- **CPU and GPU backends** — OpenMP on the CPU, an optional CUDA backend via NVRTC; measured cases cover 8k / 32.8k / 125k particles, see [PERFORMANCE.md](PERFORMANCE.md).
- **Qt desktop frontends** — native Qt Widgets and Qt Quick/QML share one simulation session and renderer: frameless window, pause / single step / reset, particle and solver parameters, view modes, FPS overlay, four skins.
- **Empty model scene** — starts with no tank and no fluid. Import several OBJ/STL/PLY/FBX/DAE/3DS/OFF/glTF models plus STEP/STP CAD parts, then place them with the panel or the viewport arrows and rings; a click selects a model and outlines it.
- **Per-viewport background** — `Option → Background color...` changes only the 3D clear colour and letterbox bars; it survives theme switches and restarts and is written into headless render configs.
- **Live density diagnostics** — mean density in real kg/m³ against a labelled `rho0` line, banded green / amber / red by solver error; hover a sample for sim time, step, density and Δρ.
- **Headless mode** — offline simulation rendered straight into ffmpeg, with no BMP frames on disk: a 14 400-frame 1080p run writes one ~500 MB mp4 instead of ~90 GB of BMPs.
- **Particle export** — `--export-particles` writes one binary PLY per frame for Houdini / Blender, with a disk-space pre-check before the run starts.

---

## Build & run

### Requirements

| dependency | needed for | notes |
|---|---|---|
| CMake 3.16+ | build | any generator; the commands below use MinGW Makefiles |
| MinGW-w64 g++ (MSYS2 UCRT64) or GCC/Clang on Linux | build | OpenMP is required (`find_package(OpenMP REQUIRED)`) |
| Qt 6.5+ **Core, Gui, Widgets, Quick, QuickControls2, OpenGL, OpenGLWidgets** | GUI | tested with Qt 6.12.0 / UCRT64 |
| Assimp | OBJ/FBX/STL/glTF import | `find_package(assimp CONFIG)` first, `pkg-config assimp` as fallback |
| — (built in) | STEP/STP import | no extra dependency: ISO 10303-21 parser + B-rep tessellator in `src/StepImporter.cpp` |
| OpenGL 3.3 | renderer | GLAD is vendored in `thirdparty/` |
| OpenCL (optional) | GPU surface reconstruction | CPU reconstruction is used when missing |
| CUDA Toolkit headers `cuda.h` + `nvrtc.h` (optional) | CUDA backend | headers only; no `nvcc`, no import library |
| ffmpeg (optional) | headless mp4 output | without it the runner keeps BMP frames |

Build, packaging and deployment guides: **[Windows](docs/Windows.md)** ·
**[Linux](docs/Linux.md)**.

### GUI controls

- **Transport** — pause / resume (`Space`), single step (`S`), reset (`R`)
- **Scene** — `Option → Scene`: Empty / Dam break / Inflow, with the active item checked. Switching asks for confirmation, clears all models, resets the simulation and restores that scene's fixed camera. The app starts in Empty.
- **Objects** (Empty only) — multi-select import, model list with visibility/delete, position XYZ, rotation XYZ (degrees), uniform scale 0.001–1000 and Reset. Multiple imports may overlap at the origin; move them apart with the panel or gizmo.
- **Selection / gizmo** (Empty only) — left-click selects a model (yellow stencil outline); clicking empty space or `Esc` deselects. Drag the coloured world X/Y/Z arrows to translate, or the X/Y/Z rings to rotate. Dragging the model body or empty space still orbits; middle/right-drag pans. No snapping and no scale gizmo.
- **Spawn water drop** (dam scene) — one-shot spherical blob above the tank rim, with editable radius and X/Y/Z defaults in the Scene card
- **Particles** — type any fluid-particle count of 512 or more; there is no application-level upper cap. Spacing follows from the scene's fixed water volume, and the box under the field shows the resulting spacing plus the estimated device memory. On CUDA, the count is checked against the active GPU budget as soon as it is entered; an overloaded request is marked in red and is not applied. The same preflight is used by the render dialog. The `Active` row always reports the particle count the solver really built (a non-cubic number rounds up to the lattice, e.g. 500 000 → 512 000)
- **Solver / forces / surface** — IISPH or DFSPH, XSPH viscosity, surface tension, wall adhesion, threshold, grid scale, rebuild interval
- **View** — particles / surface / both; surface mode also reconstructs while paused
- **Camera** — two modes only: **Orbit** (left-drag orbits the target) and **Fly** (left-drag looks around from the current eye, `WASD` moves, `Q/E` moves down/up). The Camera card and Render dialog share Front / Back / Left / Right / Top / Iso / Current presets. Middle/right-drag pans, the wheel changes the **field of view** and stops exactly at 20° / 75° (the same ends as the Field-of-view control), double-click or **Reset view** restores the camera. The orbit distance can go down to 5 cm, so a 30 mm CAD part can be inspected up close. The **Camera speed** slider (0.2x-3x) scales orbit, pan and fly sensitivity; importing a model frames it automatically, so a part authored in millimetres is not a speck in the 2 m tank. `S` is a single step in Orbit mode and "move backwards" in Fly mode.
- **Density chart** — plots the real mean density in `kg/m³` with a `ρ₀` reference line; hover any point to read its simulation time, step number, density and signed density error
- **Navigation bar** — `File` (import model in Empty, restart, save snapshot, exit), `Option` (scene submenu, viewport background colour, multi-model import), `Appearance` (four skins with preview swatches and current selection), `Help` (shortcuts, about). **Render** and `Ctrl+Shift+R` are disabled in Empty; screenshots remain available.
- **Window** — minimize / maximize / close at the top right; screenshot (`Ctrl+S`), fullscreen (`F11`), and `Ctrl+T` still cycles appearances

---

## Scene files (schema v2)

`File → Export Parameter (Json)` now writes schema v2. Paths are resolved
relative to the config file's directory, so a scene folder can be moved as a
unit:

```json
{
  "app": "IISPH",
  "schemaVersion": 2,
  "simulation": {
    "scene": "empty",
    "models": [
      {
        "path": "models/teapot.obj",
        "position": [0, 0, 0],
        "rotation": [1, 0, 0, 0],
        "scale": 1.0,
        "visible": true
      }
    ]
  },
  "render": {
    "camera": { "yaw": -128.35, "pitch": -29.77, "distance": 3.526,
                "target": [1, 0.55, 0.5], "fov": 42 },
    "background": "#1F2638"
  }
}
```


### Scenes (`--scene`)

| scene | description |
|---|---|
| `dam` (default) | classic dam break in an **open-top** 2.0x1.2x1.0 m tank (no lid, splash may leave the tank); optional `--drop <N>[f|s]` spawns a water drop above the rim |
| `inflow` | open-top 1.0 m cubic container with a continuous central jet falling 0.5 m from above (injects until `--particles` is reached, then stops - fixed volume) |
| `empty` | GUI only: no tank, no fluid, models only. Rejected by headless rendering; use a screenshot instead |


### Options

| option | default | description |
|---|---|---|
| `--headless` | — | run without a window (video output) |
| `--frames N` | 300 | number of simulated frames |
| `--seconds S` | — | video length in seconds (frames = `S * fps`; overrides `--frames`) |
| `--time-scale S` | 1.0 | simulated seconds per video second: `1` = real time, `0.1` = 10x slow motion, `2` = 2x fast motion |
| `--fps N` | 30 | video frame rate / output sampling rate |
| `-r WxH` | 1280x720 | render resolution |
| `--output PATH` | `out` | output directory (`output.mp4`, or `frames/` with `--no-video`/`--keep-frames`) |
| `--mode MODE` | `surface` | `particles` (points) or `surface` (reconstructed mesh) |
| `--solver S` | `IISPH` | `IISPH` \| `DFSPH` (case-insensitive; DFSPH = Bender & Koschier 2017) |
| `--backend B` | `auto` | `auto` \| `cpu` \| `cuda`; `auto` picks CUDA when it can be created |
| `--particles N` | 8000 | target number of fluid particles (spacing = (scene water volume / N)^(1/3)) |
| `--scene S` | `dam` | `dam` \| `inflow` (see Scenes above) |
| `--drop N[f/s]` | off | spawn the one-shot water drop at video frame N, or N video seconds (`20f`/`20s`; dam only); its simulation time is scaled by `--time-scale` |
| `--dt T` | auto (CFL) | nominal time step in seconds; CFL may shorten it |
| `--grid S` | 1.0 | surface grid scale (1.0 = paper r, 0.5 = fine/high quality) |
| `--threads N` | all cores | OpenMP thread count |
| `--no-video` | off | write BMP frames only, skip ffmpeg encode (debug / manual encode) |
| `--keep-frames` | off | write BMP frames too and keep them after encoding (old behaviour) |
| `--export-particles` | off | dump per-frame particle positions to `<output>/particles/frame_%04d.ply` (binary PLY, headless only) |
| `--bench NAME` | — | run a validation experiment and exit: `hydrostatic` \| `dambreak` \| `energy` |

### Examples

```powershell
# default scene (dam break, open-top): 0.8 m water cube collapses, 10 s @ 30 fps, 1080p HQ surface
SPHFluidSim.exe --headless --seconds 10 --fps 30 -r 1920x1080 --grid 0.5 --output hi

# dam break with a water drop spawned at 3 s into the video
SPHFluidSim.exe --headless --seconds 10 --scene dam --drop 3s --output dam

# inflow jet (strong central jet, falls 0.5 m)
SPHFluidSim.exe --headless --seconds 15 --scene inflow --particles 12000 --output jet

# particle-points video, fixed dt
SPHFluidSim.exe --headless --seconds 20 --mode particles --particles 33000 --dt 0.002 --output pv

# render frames only (no ffmpeg), then encode manually
SPHFluidSim.exe --headless --frames 120 --no-video --output raw
ffmpeg -framerate 30 -i raw/frames/frame_%04d.bmp -pix_fmt yuv420p raw.mp4

# DFSPH + per-frame particle positions for Houdini / Blender
SPHFluidSim.exe --headless --seconds 20 --fps 120 -r 1920x1080 --particles 100000 --solver DFSPH --export-particles --output dam_ply
```

---

## References

1. **M. Ihmsen, J. Cornelis, B. Solenthaler, C. Horvath, and M. Teschner,** "Implicit incompressible SPH," *IEEE Transactions on Visualization and Computer Graphics*, vol. 20, no. 3, pp. 426-435, 2014. DOI: 10.1109/TVCG.2013.105
2. **J. J. Monaghan,** "Smoothed particle hydrodynamics," *Reports on Progress in Physics*, vol. 68, no. 8, pp. 1703-1759, 2005. DOI: 10.1088/0034-4885/68/8/R01
3. **M. Becker and M. Teschner,** "Weakly compressible SPH for free surface flows," in *Proc. ACM SIGGRAPH/Eurographics Symposium on Computer Animation (SCA)*, San Diego, CA, 2007, pp. 209-217. DOI: 10.5555/1272690.1272719
4. **G. Akinci, M. Ihmsen, N. Akinci, and M. Teschner,** "Parallel surface reconstruction for particle-based fluids," *Computer Graphics Forum*, vol. 31, no. 6, pp. 1797-1809, 2012. DOI: 10.1111/j.1467-8659.2012.02096.x
5. **M. Ihmsen, N. Akinci, M. Becker, and M. Teschner,** "A parallel SPH implementation on multi-core CPUs," *Computer Graphics Forum*, vol. 30, no. 1, pp. 99-112, 2011. DOI: 10.1111/j.1467-8659.2010.01832.x

---

## Notes on my blog
The notes I took when I was working on this project are all on [my blog](https://emeeeemmmm.github.io/).

Deepseek v4 Flash was used during this project.
