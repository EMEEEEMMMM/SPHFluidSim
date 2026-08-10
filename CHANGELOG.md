# IISPH — Release Notes v1.3

## v1.3 (2026-08)

### What's new
- **GPU surface reconstruction (OpenCL)** — the scalar-field construction of the Akinci et al. 2012 pipeline (color field `c_i = sum_j V_j W(x_i - x_j)` -> surface-particle test -> surface-grid-vertex marking -> implicit field `phi = |x - xbar| - r * f`) now runs as embedded OpenCL kernels on the GPU, while Marching Cubes stays on the CPU exactly like the paper (Sec. 4.2: *"The triangulation process is not parallelized, since it takes a rather small amount of time"*). No external `.cl` file to load — the kernels are compiled from source at startup. Automatic fallback to the CPU pipeline if OpenCL is unavailable or a kernel fails.
- **Wider surface-vertex band (`bandScale` 1.5r -> 2.0r)** — this is the fix for the long-standing "cracks on calm water" problem. The paper's band (Sec. 4.1, ±1r) and our previous ±1.5r both let the isosurface — which sits ~0.5r *below* the top particles for a settled body of water — fall outside the band for a measurable share of Marching-Cubes cells, so the 8/8 surface-vertex cull dropped those triangles (visible cracks). ±2r keeps the whole isosurface band inside the band while the 8/8 cull still rejects the (false) bottom-layer surface, so there is no double layer either.
- **More resolution presets in the GUI** — 0.040 m (~8k) down to 0.010 m (~500k particles). The GPU field kernels only become GPU-bound at large particle counts, so the high end now has somewhere to go.
- **EVmax gradient fix** — corrected the `dw/dx` sign in the EVmax Jacobian and guarded the `d = 0` case (no more NaN normals from coincident particles).

### Fixed
- **Surface-mode stutter** — the per-frame scalar-field construction (previously ~100+ ms of CPU time per frame) moved to the GPU, so the render loop is no longer blocked by field building.
- **Cracks / missing triangles on a settled water surface** — see the band change above (the visual issue you reported repeatedly since v1.1).
- Removed a leftover debug `fprintf` in the grid count pass.

### Build
- CMake now finds OpenCL (`find_package(OpenCL)` with a fallback to the NVIDIA CUDA toolkit's shipped `CL/cl.h` + `OpenCL.lib`); a machine without OpenCL still builds and runs — surface reconstruction silently falls back to the CPU path with a one-line warning.

### Validation
- All v1.2 validation still passes on this machine (20 threads): hydrostatic pressure-depth slope -10.6 .. -11.6, dam-break peak avg density error 0.30% with 2.8 pressure iterations, energy bench no NaN / no escapes / density < 2%.
- Surface quality: calm-water isosurface no longer tears (boundary-edge share on a settled surface drops to ~0%); the GPU and CPU paths produce the same mesh (same kernels, same Marching Cubes).

---

## v1.2 (2026-08)

### What's new
- **Open-top dam break** — the dam-break tank (2.0 x 1.2 x 1.0 m) has no lid anymore, so splash can fly out of the tank like a real dam break.
- **"Spawn water drop" button** — one-shot button in the GUI (dam scene): spawns a spherical blob (R = 0.2 m) above the open rim that falls into the water. Disabled after use; Reset re-enables it.
- **Stronger inflow jet** — fall gap 0.3 -> 0.5 m (entry speed 2.43 -> 3.13 m/s), emitter radius 0.08 -> 0.12 m (~2.3x flow rate).
- **`--drop <N>[f|s]`** for headless — spawn the drop at frame N or N seconds in: `--drop 20f` = frame 20, `--drop 3s` = 3*fps frames in (dam scene only). Bare numbers stay frame-based.

### Fixed
- **Still water stays still** (root cause, not a band-aid). The pressure solve was capped at 25 iterations, which left the hydrostatic pressure ~20x overshoot (interior p = 93 vs theory 4.9) and a permanent drift in resting water (23.5% of pool particles > 1 m/s). With the paper-default `maxIter = 100` a settled pool reaches mean v = 0.14 m/s, 99.4% of particles < 1 m/s. Dam-break density error improved 1.7% -> 0.30%.
- **Removed the extra `-coefJi*|gradB|^2` term in `a_ii`** — the paper's Eq. 12 boundary part only keeps `d_ii . gradB`; the extra term shrank `a_ii` for wall-adjacent particles and caused a permanent wall-squeeze jitter in shallow tanks.
- **Particle pile-up along one wall fixed** — the neighbourhood grid origin now follows the particle bbox instead of being fixed at (0,0,0), which clamped particles of translated containers (inflow/drop) into the last grid column.
- **Colour tearing in videos fixed** — the pipe writer copied 4 bytes/pixel (BGRA incl. the alpha byte) into a 3-byte BGR stream, producing a dense alternating stripe pattern; now strips to BGR per pixel (mp4 vs reference BMP: 100% -> 0.2% pixel difference).
- **ffmpeg failures are detected** — the encoder's exit code is checked; a failed encode no longer reports success.

### Performance
- **Streaming encode**: frames are piped straight into ffmpeg — no BMPs on disk at all (a 14400-frame 1080p run is ~500 MB mp4 instead of ~90 GB of BMPs).
- Fixed water volume everywhere: dam 0.512 m^3 (0.034 m^3 more if the drop is spawned), inflow 0.6 m^3; `--particles N` derives `spacing = (volume/N)^(1/3)`, so 8k or 16k particles show the same physical water, just at different resolution.
