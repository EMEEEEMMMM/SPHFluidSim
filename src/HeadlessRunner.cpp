// windows.h pulls in min/max macros that would break std::min/std::max below,
// and we only need GetDiskFreeSpaceExA.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h also defines near/far as empty macros, which breaks the
// Matrix4::Persp/Ortho parameters used by the GL layer.
#undef near
#undef far

#include "HeadlessRunner.hpp"
#include "FluidSolver.hpp"
#include "SolverFactory.hpp"
#include "SurfaceReconstructor.hpp"
#include "OpenGLLayer.hpp"
#include <glad/glad.h>
#include "Render/QtOffscreenContext.hpp"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <io.h>   // _popen / _pclose (MinGW)
#include <algorithm>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <direct.h>
#include <omp.h>

namespace {

// Parent <-> renderer control channel. Commands arrive on stdin as plain text
// while structured progress leaves on stdout, one JSON object per line. State
// is shared with a detached reader so a blocked getline can never delay the
// render loop or process shutdown.
struct RenderControl {
    struct State {
        std::mutex mutex;
        std::mutex outputMutex;
        std::condition_variable cv;
        bool paused = false;
        bool cancelled = false;
    };

    std::shared_ptr<State> state = std::make_shared<State>();
    bool enabled = false;

    explicit RenderControl(bool controlEnabled) : enabled(controlEnabled) {
        if (!enabled) return;
        const auto shared = state;
        std::thread([shared] {
            std::string line;
            while (std::getline(std::cin, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                if (line == "pause") {
                    {
                        std::lock_guard<std::mutex> lk(shared->mutex);
                        shared->paused = true;
                    }
                    std::lock_guard<std::mutex> out(shared->outputMutex);
                    std::printf("@STATE {\"state\":\"paused\"}\n");
                    std::fflush(stdout);
                } else if (line == "resume") {
                    {
                        std::lock_guard<std::mutex> lk(shared->mutex);
                        shared->paused = false;
                    }
                    shared->cv.notify_all();
                    std::lock_guard<std::mutex> out(shared->outputMutex);
                    std::printf("@STATE {\"state\":\"running\"}\n");
                    std::fflush(stdout);
                } else if (line == "cancel") {
                    {
                        std::lock_guard<std::mutex> lk(shared->mutex);
                        shared->cancelled = true;
                        shared->paused = false;
                    }
                    shared->cv.notify_all();
                    break;
                }
            }
        }).detach();
    }

    // Blocks while paused and returns false as soon as cancellation is asked.
    // Called between solver steps, stages, and rendered frames.
    bool Checkpoint() {
        if (!enabled) return true;
        std::unique_lock<std::mutex> lk(state->mutex);
        if (state->cancelled) return false;
        if (!state->paused) return true;
        state->cv.wait(lk, [this] { return state->cancelled || !state->paused; });
        return !state->cancelled;
    }

    void ReportProgress(int frame, int totalFrames, double overall, double stage,
                        const char* stageName, double elapsed, double eta) {
        if (!enabled) return;
        std::lock_guard<std::mutex> out(state->outputMutex);
        std::printf("@PROGRESS {\"frame\":%d,\"totalFrames\":%d,\"overall\":%.6f,"
                    "\"stageProgress\":%.6f,\"stage\":\"%s\",\"elapsed\":%.3f,\"eta\":%.3f}\n",
                    frame, totalFrames, std::clamp(overall, 0.0, 1.0),
                    std::clamp(stage, 0.0, 1.0), stageName, std::max(0.0, elapsed), eta);
        std::fflush(stdout);
    }

    void ReportState(const char* value) {
        if (!enabled) return;
        std::lock_guard<std::mutex> out(state->outputMutex);
        std::printf("@STATE {\"state\":\"%s\"}\n", value);
        std::fflush(stdout);
    }
};


bool MakeDirs(const std::string& dir) {
    if (_mkdir(dir.c_str()) == 0) return true;
    return errno == EEXIST;   // already exists is fine; anything else is not
}

// 24-bit bottom-up BMP written from BGRA pixel data (4 bytes/pixel from
// glReadPixels GL_BGRA): the blue/green/red bytes are already in BMP order.
bool WriteBMP(const std::string& path, const unsigned char* bgra, int w, int h) {
    const int rowSize = ((w * 3) + 3) & ~3;
    const int dataSize = rowSize * h;
    unsigned char header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    unsigned fileSize = 54 + dataSize;
    memcpy(header + 2, &fileSize, 4);
    unsigned off = 54;
    memcpy(header + 10, &off, 4);
    unsigned hdr = 40;
    memcpy(header + 14, &hdr, 4);
    int iw = w, ih = h;
    memcpy(header + 18, &iw, 4);
    memcpy(header + 22, &ih, 4);
    unsigned short planes = 1, bpp = 24;
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &bpp, 2);

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = (fwrite(header, 1, 54, f) == 54);
    std::vector<unsigned char> row(rowSize);
    // glReadPixels returns rows bottom-up (row 0 = framebuffer bottom), and a
    // BMP also stores rows bottom-up — so write rows in order 0..h-1.
    for (int y = 0; y < h && ok; y++) {
        const unsigned char* src = bgra + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];   // B
            row[x * 3 + 1] = src[x * 4 + 1];   // G
            row[x * 3 + 2] = src[x * 4 + 2];   // R
        }
        ok = (fwrite(row.data(), 1, rowSize, f) == (size_t)rowSize);
    }
    fclose(f);
    return ok;
}

bool FfmpegAvailable() {
    return system("ffmpeg -version >nul 2>nul") == 0;
}

// "m:ss" (or "h:mm:ss" past an hour) for the progress line; "--:--" when the
// estimate is not available yet.
void FormatDuration(double seconds, char* out, size_t n) {
    if (!(seconds >= 0.0) || !std::isfinite(seconds)) { snprintf(out, n, "--:--"); return; }
    const long total = (long)(seconds + 0.5);
    const long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0) snprintf(out, n, "%ld:%02ld:%02ld", h, m, s);
    else       snprintf(out, n, "%ld:%02ld", m, s);
}

// ---------------------------------------------------------------------------
// Particle dump (headless only): one binary PLY per frame so Houdini / Blender
// can load the sequence. 3 float32 per particle + a ~200 byte header.
// ---------------------------------------------------------------------------
unsigned long long EstimateParticleDumpBytes(int particles, int frames) {
    const unsigned long long perParticle = 12ull;   // 3 * float32
    // 30% headroom: the dam scene spawns a one-shot drop after the run starts,
    // so the initial particle count is not the maximum.
    const unsigned long long n = (unsigned long long)particles * 13ull / 10ull + 1024ull;
    return (200ull + n * perParticle) * (unsigned long long)frames * 102ull / 100ull;
}

bool FreeDiskBytes(const std::string& path, unsigned long long& freeBytes) {
    char full[_MAX_PATH];
    if (!_fullpath(full, path.c_str(), _MAX_PATH)) return false;
    if (full[1] != ':') return false;               // not a drive-absolute path
    const char root[4] = { full[0], ':', '\\', 0 };
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExA(root, &avail, nullptr, nullptr)) return false;
    freeBytes = avail.QuadPart;
    return true;
}

bool WriteParticlePLY(const std::string& path, const std::vector<Math::Vector3>& pos,
                      std::vector<float>& scratch) {
    FILE* f = fopen(path.c_str(), "wb");   // binary mode: no CRLF translation
    if (!f) return false;
    const size_t n = pos.size();
    const int head = fprintf(f,
        "ply\n"
        "format binary_little_endian 1.0\n"
        "comment IISPH particle positions\n"
        "element vertex %zu\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "end_header\n", n);
    scratch.resize(n * 3);
    for (size_t i = 0; i < n; i++) {
        scratch[3 * i + 0] = pos[i].x;
        scratch[3 * i + 1] = pos[i].y;
        scratch[3 * i + 2] = pos[i].z;
    }
    const bool ok = head > 0 &&
                    fwrite(scratch.data(), sizeof(float), scratch.size(), f) == scratch.size();
    fclose(f);
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Offline rendering: Qt offscreen GL context + OpenGL FBO, reused OpenGLLayer.
// ---------------------------------------------------------------------------
int RunHeadless(const HeadlessOptions& opt) {
    if (opt.threads > 0) omp_set_num_threads(opt.threads);
    RenderControl control(opt.control);
    if (!control.Checkpoint()) {
        control.ReportState("cancelled");
        return 0;
    }

    // Resolve the output directory and validate the particle dump BEFORE any GL
    // or solver work: a full disk must abort before the simulation starts.
    const std::string dir = opt.output.empty() ? std::string("out") : opt.output;
    const std::string particlesDir = dir + "/particles";
    if (opt.exportParticles) {
        const unsigned long long need = EstimateParticleDumpBytes(opt.particles, opt.frames);
        unsigned long long freeB = 0;
        const bool haveFree = FreeDiskBytes(dir, freeB);
        const double needGiB = (double)need / (1024.0 * 1024.0 * 1024.0);
        const double freeGiB = (double)freeB / (1024.0 * 1024.0 * 1024.0);
        printf("[export] particle positions -> %s/  (%d frames, ~%.2f GiB)\n",
               particlesDir.c_str(), opt.frames, needGiB);
        if (haveFree) {
            printf("[export] free space on target drive: %.2f GiB\n", freeGiB);
        } else {
            printf("[export] WARNING: could not query free space; continuing anyway\n");
        }
        if (haveFree && need > freeB) {
            printf("[export] ERROR: not enough disk space - need %.2f GiB, only %.2f GiB free.\n"
                   "[export]        Simulation aborted. Lower --particles / --frames or free some space.\n",
                   needGiB, freeGiB);
            return 1;
        }
    }

    // Qt supplies the same hidden GL 3.3 core context previously provided by
    // GLFW. This owner is declared before the renderer so the renderer and its
    // GL resources are destroyed while the context is still current.
    render::QtOffscreenContext glContext;
    if (!glContext.Create()) {
        printf("[headless] failed to create Qt offscreen GL 3.3 context\n");
        return 1;
    }
    if (!gladLoadGLLoader(render::QtOffscreenContext::ProcAddress)) {
        printf("[headless] failed to load GL\n");
        return 1;
    }

    OpenGLLayer layer(opt.width, opt.height);
    layer.SetViewportBackground(opt.background);
    std::string backendError;
    auto solverPtr = CreateSolver(ParseBackend(opt.backend), &backendError);
    if (!solverPtr) {
        printf("[headless] backend '%s' unavailable: %s\n",
               opt.backend.c_str(), backendError.c_str());
        return 1;
    }
    if (!backendError.empty())
        printf("[headless] backend fallback to CPU: %s\n", backendError.c_str());
    printf("[headless] backend: %s\n", solverPtr->BackendName());
    IFluidSolver& solver = *solverPtr;

    auto& params = solver.GetParams();
    params.solver = (opt.solver == "dfsph") ? 1 : 0;
    if (opt.useCFL) params.useCFL = *opt.useCFL;
    if (opt.omega) params.omega = *opt.omega;
    if (opt.eta) params.eta = *opt.eta;
    if (opt.etaV) params.etaV = *opt.etaV;
    if (opt.maxIter) params.maxIter = *opt.maxIter;
    if (opt.viscosity) params.viscosity = *opt.viscosity;
    if (opt.surfaceTension) params.surfaceTension = *opt.surfaceTension;
    if (opt.wallAdhesion) params.surfaceTensionBoundary = *opt.wallAdhesion;

    // --particles -> spacing: each scene defines a FIXED water volume, and the
    // spacing is derived so that N particles always fill that volume:
    //   volume = N * spacing^3  ->  spacing = (volume / N)^(1/3).
    // The scene is then built with exactly N particles (the top lattice layer
    // may be partial). A JSON job that only carries a spacing is converted to
    // the corresponding count, so both paths agree.
    const bool isInflow = (opt.scene == "inflow");
    float waterVol = 0.512f;                       // dam: 0.8^3 water cube (open-top tank)
    if (isInflow) waterVol = 0.6f;                 // inflow: open tank filled to 60%
    const float waterSize = powf(waterVol, 1.0f / 3.0f);
    int particles = opt.particles;
    if (opt.spacing && *opt.spacing > 0.0f) {
        const double s = static_cast<double>(*opt.spacing);
        particles = static_cast<int>(std::llround(waterVol / (s * s * s)));
    }
    particles = std::max(particles, 512);
    float spacing = powf(waterVol / (float)particles, 1.0f / 3.0f);
    params.spacing = spacing;
    if (opt.dt > 0.0f) params.dt = opt.dt;
    try {
        solver.CheckParticleBudget(particles);
        if (isInflow) {
            // open-top cubic container (side 1 m at x=1, z=0.5) + central jet
            // falling 0.5 m from above (stronger jet)
            solver.InitInflow(1.0f, 0.5f, 100, 0.12f, 1.0f, 0.5f, particles);
            layer.SetTankSize(1.0f, 1.0f, 1.0f, 1.0f, 0.5f, true);
        } else {
            // dam break in an OPEN-TOP tank: splash may leave the tank. With
            // --drop <frame> a spherical blob is spawned above the rim at that
            // frame and falls in (one-shot, like the GUI button).
            solver.InitDamOpen(2.0f, 1.2f, 1.0f, waterSize);
            layer.SetTankSize(2.0f, 1.2f, 1.0f, 0.0f, 0.0f, true);   // open top
        }
    } catch (const std::exception& ex) {
        printf("[headless] cannot build the scene: %s\n", ex.what());
        printf("[headless] lower --particles (currently %d) and try again.\n", particles);
        return 1;
    }
    spacing = solver.Spacing();

    // Camera: a closer three-quarter view centred on the tank (original
    // behaviour). When a v2 config supplies render.camera, derive the eye from
    // the orbit yaw/pitch/distance around the target instead — the same
    // convention as render::GlView::PushCamera / Render/CameraRig.hpp.
    {
        auto& cam = layer.GetCamera();
        if (opt.camera.explicitCamera) {
            constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
            const double ry = opt.camera.yaw * kDegToRad;
            const double rp = opt.camera.pitch * kDegToRad;
            // Front = eye -> target direction; eye = target - Front * distance.
            const Math::Vector3 front(
                static_cast<float>(std::cos(ry) * std::cos(rp)),
                static_cast<float>(std::sin(rp)),
                static_cast<float>(std::sin(ry) * std::cos(rp)));
            const Math::Vector3 target(
                static_cast<float>(opt.camera.targetX),
                static_cast<float>(opt.camera.targetY),
                static_cast<float>(opt.camera.targetZ));
            cam.Position = target - front * static_cast<float>(opt.camera.distance);
            cam.Front = front.normalize();
            cam.Up = { 0.0f, 1.0f, 0.0f };
            cam.Zoom = static_cast<float>(opt.camera.fov);
        } else {
            cam.Position = { 2.9f, 2.3f, 2.9f };
            const float tx = 1.0f, tz = 0.5f;
            const float ty = isInflow ? 0.5f : 0.55f;
            Math::Vector3 f = { tx - 2.9f, ty - 2.3f, tz - 2.9f };
            cam.Front = f * (1.0f / f.length());
            cam.Up = { 0.0f, 1.0f, 0.0f };
            cam.Zoom = 42.0f;
        }
    }

    // Surface or particle view.
    const bool showSurface = (opt.mode == HeadlessOptions::Mode::Surface);
    layer.SetSurfaceVisible(showSurface);
    layer.SetParticlesVisible(!showSurface);
    SurfaceReconstructor recon;
    recon.SetGridScale(opt.gridScale);   // 1.0 = paper r (fast), 0.5 = r/2 high-quality
    recon.SetSurfaceThreshold(opt.surfaceThreshold);

    if (!MakeDirs(dir)) {
        printf("[headless] ERROR: cannot create output directory '%s' (does its parent exist?)\n", dir.c_str());
        return 1;
    }
    // Default path: stream frames straight into ffmpeg over a pipe (no BMPs on
    // disk at all - a 14400-frame 1080p run is ~90 GB as BMPs, ~500 MB as mp4).
    // BMP frames are only written for --no-video (debug, manual ffmpeg) or
    // --keep-frames (old behaviour).
    const bool usePipe = !opt.noVideo && !opt.keepFrames && FfmpegAvailable();
    std::string framesDir = dir + "/frames";
    if (!usePipe && !MakeDirs(framesDir)) {
        printf("[headless] ERROR: cannot create frames directory '%s'\n", framesDir.c_str());
        return 1;
    }
    if (opt.exportParticles && !MakeDirs(particlesDir)) {
        printf("[headless] ERROR: cannot create particle dump directory '%s'\n", particlesDir.c_str());
        return 1;
    }
    FILE* pipe = nullptr;
    if (usePipe) {
        std::string cmd = "ffmpeg -y -f rawvideo -pixel_format bgr24 -video_size " +
                          std::to_string(opt.width) + "x" + std::to_string(opt.height) +
                          " -framerate " + std::to_string(opt.fps) +
                          " -i pipe:0 -c:v libx264 -preset veryfast -pix_fmt yuv420p -crf 18 \"" +
                          dir + "/output.mp4\"";
        pipe = _popen(cmd.c_str(), "wb");
        if (!pipe) {
            printf("[headless] WARNING: could not start ffmpeg pipe, falling back to BMP frames\n");
            if (!MakeDirs(framesDir)) {
                printf("[headless] ERROR: cannot create frames directory '%s'\n", framesDir.c_str());
                return 1;
            }
        }
    }
    const bool writeBMP = !usePipe || !pipe;

    std::vector<unsigned char> pixels((size_t)opt.width * opt.height * 4);   // BGRA
    std::vector<unsigned char> row((size_t)opt.width * 3);   // one BGR row for the pipe
    int bmpFailures = 0;
    int plyFailures = 0;
    std::vector<float> plyScratch;
    const int reconInterval = std::max(1, opt.reconInterval);

    printf("[headless] particles=%d (target %d, spacing=%.4f)  frames=%d  fps=%d  "
           "time-scale=%.4g  mode=%s  res=%dx%d\n",
           solver.FluidCount(), particles, spacing, opt.frames, opt.fps, opt.timeScale,
           showSurface ? "surface" : "particles", opt.width, opt.height);

    double tWall0 = omp_get_wtime();
    const double dropSimTime = opt.dropVideoSeconds >= 0.0
        ? opt.dropVideoSeconds * opt.timeScale
        : -1.0;
    auto spawnDropIfDue = [&] {
        if (isInflow || dropSimTime < 0.0 || solver.HasDrop() ||
            static_cast<double>(solver.SimTime()) < dropSimTime) return;
        // Check after every solver step, not just at rendered-frame boundaries.
        // The event therefore occurs after the same physical step at every fps.
        solver.SpawnDrop(static_cast<float>(opt.drop.radius),
                         static_cast<float>(opt.drop.x),
                         static_cast<float>(opt.drop.y),
                         static_cast<float>(opt.drop.z));
        printf("[headless] drop spawned at video t=%.3fs, sim t=%.3fs (N=%d)\n",
               opt.dropVideoSeconds, solver.SimTime(), solver.FluidCount());
    };
    spawnDropIfDue();   // --drop 0 / 0f happens before the first simulation step
    bool cancelled = false;
    int lastFrame = 0;
    double lastTargetTime = 0.0;
    for (int f = 0; f < opt.frames; f++) {
        lastFrame = f;
        if (!control.Checkpoint()) {
            cancelled = true;
            break;
        }
        const double targetSimTime =
            headless_timing::FrameTargetSimTime(f, opt.fps, opt.timeScale);
        double elapsed = omp_get_wtime() - tWall0;
        double eta = f > 0 ? elapsed / (double)(f + 1) * (double)(opt.frames - f - 1) : -1.0;
        control.ReportProgress(f, opt.frames, (double)f / (double)opt.frames, 0.0,
                               "Simulating", elapsed, eta);

        // Advance one video frame, checking pause/cancel between every solver
        // step. During slow-motion renders a single frame can be hundreds of
        // steps, so frame-boundary checks alone would feel unresponsive.
        while (static_cast<double>(solver.SimTime()) < targetSimTime) {
            solver.Step();
            spawnDropIfDue();
            if (!control.Checkpoint()) {
                cancelled = true;
                break;
            }
        }
        if (cancelled) break;
        const double simFraction = targetSimTime > lastTargetTime
            ? std::clamp((static_cast<double>(solver.SimTime()) - lastTargetTime) /
                         (targetSimTime - lastTargetTime), 0.0, 1.0)
            : 1.0;
        lastTargetTime = targetSimTime;

        elapsed = omp_get_wtime() - tWall0;
        eta = f > 0 ? elapsed / (double)(f + 1) * (double)(opt.frames - f - 1) : -1.0;
        control.ReportProgress(f, opt.frames, (double)f / (double)opt.frames,
                               simFraction * 0.55, "Simulating", elapsed, eta);

        if (opt.exportParticles) {
            char name[512];
            snprintf(name, sizeof(name), "%s/frame_%04d.ply", particlesDir.c_str(), f);
            if (!WriteParticlePLY(name, solver.Positions(), plyScratch)) {
                if (plyFailures == 0) printf("[export] ERROR: cannot write %s\n", name);
                plyFailures++;
            }
        }

        layer.UpdateParticles(solver.Positions(), solver.Colors());
        if (showSurface && (f % reconInterval == 0)) {
            control.ReportProgress(f, opt.frames, (double)f / (double)opt.frames,
                                   0.6, "Reconstructing surface", elapsed, eta);
            if (!control.Checkpoint()) {
                cancelled = true;
                break;
            }
            if (recon.Reconstruct(solver.Positions(), solver.Spacing()))
                layer.UpdateSurfaceMesh(recon.Vertices(), recon.Indices());
        }

        if (!control.Checkpoint()) {
            cancelled = true;
            break;
        }
        control.ReportProgress(f, opt.frames, (double)f / (double)opt.frames,
                               0.78, "Rendering image", elapsed, eta);
        layer.Render();

        // Read the FBO back to CPU: GL_BGRA is 4-byte aligned and needs no
        // channel swap for the BMP writer (rows stay bottom-up as in the BMP).
        auto fb = layer.GetFrameBuffer();
        fb->Bind();
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, opt.width, opt.height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
        fb->UBind();

        if (!control.Checkpoint()) {
            cancelled = true;
            break;
        }
        control.ReportProgress(f, opt.frames, (double)f / (double)opt.frames,
                               0.9, "Encoding video", elapsed, eta);
        if (pipe) {
            // rawvideo bgr24 expects top-down rows; glReadPixels gives
            // bottom-up, so write rows in reverse order (row h-1 first).
            // MUST strip the alpha byte per pixel (BGRA -> BGR); a plain
            // fwrite of w*3 bytes would take 960 BGRA pixels, not 1280 BGR.
            for (int y = opt.height - 1; y >= 0; y--) {
                const unsigned char* src = pixels.data() + (size_t)y * opt.width * 4;
                for (int x = 0; x < opt.width; x++) {
                    row[x * 3 + 0] = src[x * 4 + 0];   // B
                    row[x * 3 + 1] = src[x * 4 + 1];   // G
                    row[x * 3 + 2] = src[x * 4 + 2];   // R
                }
                fwrite(row.data(), 1, row.size(), pipe);
            }
        } else {
            char name[512];
            snprintf(name, sizeof(name), "%s/frames/frame_%04d.bmp", dir.c_str(), f);
            if (!WriteBMP(name, pixels.data(), opt.width, opt.height)) {
                if (bmpFailures == 0) printf("[headless] ERROR: cannot write %s\n", name);
                bmpFailures++;
            }
        }

        elapsed = omp_get_wtime() - tWall0;
        eta = elapsed / (double)(f + 1) * (double)(opt.frames - f - 1);
        control.ReportProgress(f, opt.frames, (double)(f + 1) / (double)opt.frames,
                               1.0, "Encoding video", elapsed, eta);

        if (f % 30 == 0 || f == opt.frames - 1) {
            // Elapsed / remaining wall time. The estimate is the average cost
            // of the frames done so far, so it self-corrects when a drop is
            // spawned or the mesh gets denser; on frame 0 there is nothing to
            // average yet and it prints "--:--".
            char elStr[32], etaStr[32];
            FormatDuration(elapsed, elStr, sizeof(elStr));
            if (f > 0) {
                const double perFrame = elapsed / (double)(f + 1);
                // Round the remainder up: "0:00" should only appear once the
                // run is actually finished.
                FormatDuration(std::ceil(perFrame * (double)(opt.frames - f - 1)), etaStr, sizeof(etaStr));
            } else {
                snprintf(etaStr, sizeof(etaStr), "--:--");
            }
            const double pct = 100.0 * (double)(f + 1) / (double)opt.frames;
            printf("  frame %4d/%d (%5.1f%%)  t=%.2fs  avg-rho-err=%.3f%%  iters=%d  tris=%d  elapsed=%s  eta=%s\n",
                   f + 1, opt.frames, pct, solver.SimTime(), 100.0 * solver.AvgDensityError(),
                   solver.LastIterations(), layer.SurfaceTriangleCount(), elStr, etaStr);
            fflush(stdout);
        }
    }

    if (cancelled) {
        if (pipe) {
            _pclose(pipe);
            pipe = nullptr;
        }
        control.ReportProgress(lastFrame, opt.frames,
                               (double)lastFrame / (double)opt.frames,
                               0.0, "Cancelled", omp_get_wtime() - tWall0, -1.0);
        control.ReportState("cancelled");
        return 0;
    }

    // ffmpeg drains the remaining pipe data here; this is the final encoding
    // stage and can take a noticeable moment for long videos.
    if (pipe) {
        control.ReportProgress(opt.frames, opt.frames, 1.0, 0.95,
                               "Encoding video", omp_get_wtime() - tWall0, 0.0);
    }
    double tWall1 = omp_get_wtime();
    printf("[headless] done: %d frames in %.1f s (%.1f fps wall), video time %.2f s, sim time %.3f s (time scale %.4g)\n",
           opt.frames, tWall1 - tWall0, opt.frames / (tWall1 - tWall0),
           static_cast<double>(opt.frames) / opt.fps, solver.SimTime(), opt.timeScale);

    // Streaming path: closing the pipe makes ffmpeg finish the mp4; its exit
    // code is the real success signal (previously ignored -> "No space left on
    // device" was reported as success).
    if (pipe) {
        const int rc = _pclose(pipe);
        if (rc == 0) {
            printf("[headless] video written to %s/output.mp4\n", dir.c_str());
        } else {
            printf("[headless] ERROR: ffmpeg exited with code %d - check free disk space and that %s is writable\n",
                   rc, dir.c_str());
            return 1;
        }
    }

    // BMP fallback path (--no-video / --keep-frames / no ffmpeg): assemble from frames.
    if (!opt.noVideo && !pipe) {
        if (FfmpegAvailable()) {
            std::string cmd = "ffmpeg -y -framerate " + std::to_string(opt.fps) +
                              " -i \"" + dir + "/frames/frame_%04d.bmp\" -c:v libx264 -pix_fmt yuv420p -crf 18 \"" +
                              dir + "/output.mp4\"";
            printf("[headless] running ffmpeg...\n");
            const int rc = system(cmd.c_str());
            if (rc != 0) {
                printf("[headless] ERROR: ffmpeg exited with code %d\n", rc);
                return 1;
            }
            printf("[headless] video written to %s/output.mp4\n", dir.c_str());
            if (!opt.keepFrames) {
                // Delete only the files THIS run wrote, then drop the
                // folder if it is empty. The old code ran `rmdir /s /q`
                // on the whole folder, which also deleted unrelated
                // files the user kept there.
                for (int k = 0; k < opt.frames; k++) {
                    char name[512];
                    snprintf(name, sizeof(name), "%s/frame_%04d.bmp", framesDir.c_str(), k);
                    ::remove(name);
                }
                _rmdir(framesDir.c_str());   // succeeds only when empty
            }
        } else {
            printf("[headless] ffmpeg not found - frames saved in %s/frames/, install ffmpeg to assemble a video\n", dir.c_str());
        }
    }

    if (opt.exportParticles) {
        if (plyFailures > 0) {
            printf("[export] ERROR: %d of %d particle frames could not be written to %s/\n",
                   plyFailures, opt.frames, particlesDir.c_str());
            return 1;
        }
        printf("[export] wrote %d PLY frames to %s/ (~%.2f GiB)\n", opt.frames, particlesDir.c_str(),
               (double)EstimateParticleDumpBytes(opt.particles, opt.frames) / (1024.0 * 1024.0 * 1024.0));
    }

    if (bmpFailures > 0) {
        printf("[headless] ERROR: %d of %d frames could not be written to %s/frames\n",
               bmpFailures, opt.frames, dir.c_str());
        return 1;
    }

    control.ReportProgress(opt.frames, opt.frames, 1.0, 1.0, "Completed",
                           omp_get_wtime() - tWall0, 0.0);
    control.ReportState("completed");
    return 0;
}

// ---------------------------------------------------------------------------
// Verification benchmarks (paper Sec. 5 style).
// ---------------------------------------------------------------------------
namespace {

// Hydrostatic: water fills the tank to a height; the pressure field must grow
// linearly with depth (p = rho g h). We fit p vs y and check the slope.
int BenchHydrostatic(const HeadlessOptions& opt) {
    FluidSolver s;
    s.GetParams().solver = (opt.solver == "dfsph") ? 1 : 0;
    s.GetParams().spacing = 0.025f;   // 64k particles: stable pressure field
    if (opt.dt > 0) s.GetParams().dt = opt.dt;
    s.InitFull(2.0f, 1.2f, 1.0f, 0.5f);
    for (int k = 0; k < 300; k++) s.Step();   // settle

    // Layer-averaged hydrostatic profile (same as verify_hydrostatic): average
    // pressure per depth band in the interior core, then fit p = a + b*y.
    const float yLo = 0.12f, yHi = 0.40f;
    const int layers = 4;
    std::vector<double> sumP(layers, 0.0), sumY(layers, 0.0);
    std::vector<int> cnt(layers, 0);
    const auto& X = s.Positions();
    const auto& P = s.Pressures();
    for (size_t i = 0; i < X.size(); i++) {
        if (X[i].y < yLo || X[i].y > yHi) continue;
        if (X[i].x < 0.2f || X[i].x > 1.8f || X[i].z < 0.2f || X[i].z > 0.8f) continue;
        int l = std::clamp((int)((X[i].y - yLo) / (yHi - yLo) * layers), 0, layers - 1);
        sumP[l] += P[i];
        sumY[l] += X[i].y;
        cnt[l]++;
    }
    double sy = 0, sP = 0, syy = 0, syp = 0; int m = 0;
    for (int l = 0; l < layers; l++) {
        if (cnt[l] == 0) continue;
        double ay = sumY[l] / cnt[l];
        double ap = sumP[l] / cnt[l];
        sy += ay; sP += ap; syy += ay * ay; syp += ay * ap; m++;
    }
    const double b = (m * syp - sy * sP) / (m * syy - sy * sy);
    const double a = (sP - b * sy) / m;
    printf("[bench] hydrostatic: particles=%d core-samples=%d\n", s.FluidCount(), (int)X.size());
    printf("[bench]   pressure vs depth slope = %.3f (theory -9.81), intercept = %.3f\n", b, a);
    const bool pass = (b < -2.0);   // clearly negative slope = pressure grows with depth
    printf("[bench]   %s (slope < -2; theory -9.81, SPH noise at 0.025 spacing)\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// Dam break: track the 99th-percentile wave front x-position and the density
// error / pressure iterations. The front should accelerate and stay inside the
// tank; avg density error should stay small.
int BenchDamBreak(const HeadlessOptions& opt) {
    FluidSolver s;
    s.GetParams().solver = (opt.solver == "dfsph") ? 1 : 0;
    s.GetParams().spacing = 0.04f;
    if (opt.dt > 0) s.GetParams().dt = opt.dt;
    s.Init(2.0f, 1.2f, 1.0f, 0.8f);
    const int steps = 400;
    double maxErr = 0;
    int totalIters = 0;
    std::vector<double> frontX;
    for (int k = 0; k < steps; k++) {
        s.Step();
        maxErr = std::max(maxErr, (double)s.AvgDensityError());
        totalIters += s.LastIterations();
        if (k % 20 == 0) {
            // 99th percentile of x
            std::vector<float> xs;
            xs.reserve(s.Positions().size());
            for (const auto& p : s.Positions()) xs.push_back(p.x);
            std::sort(xs.begin(), xs.end());
            frontX.push_back(xs[(size_t)(0.99 * xs.size())]);
        }
    }
    printf("[bench] dambreak: particles=%d steps=%d\n", s.FluidCount(), steps);
    printf("[bench]   wave front (99th pct x): %.2f -> %.2f m over %d samples\n",
           frontX.front(), frontX.back(), (int)frontX.size());
    printf("[bench]   peak avg density error = %.3f%% (paper: < 0.1%% at convergence, ~1%% transient)\n", 100.0 * maxErr);
    printf("[bench]   avg pressure iterations = %.1f (paper Fig. 8/Table 1: ~5-30)\n", (double)totalIters / steps);
    const bool pass = (maxErr < 0.02) && (frontX.back() < 1.99f);
    printf("[bench]   %s (density err < 2%%, front inside tank)\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// Global energy monitoring on the dam break: report the KE/PE decomposition
// over time and check stability (bounded speed, no particle loss, density error
// under control). The first ~30 steps are the cold-start transient (the paper
// reports the same: Fig. 8 shows ~23 iterations on the first frame), so the
// speed/stability metrics are measured after it.
int BenchEnergy(const HeadlessOptions& opt) {
    FluidSolver s;
    s.GetParams().solver = (opt.solver == "dfsph") ? 1 : 0;
    s.GetParams().spacing = 0.04f;
    if (opt.dt > 0) s.GetParams().dt = opt.dt;
    s.Init(2.0f, 1.2f, 1.0f, 0.8f);
    const float mass = 1000.0f * s.Spacing() * s.Spacing() * s.Spacing();
    const int steps = 400;
    const int warmup = 30;
    double ke0 = 0, pe0 = 0;
    double vmaxOverall = 0;
    double maxErr = 0;
    double keFirst = 0, keLast = 0, peLast = 0;
    for (int k = 0; k < steps; k++) {
        s.Step();
        double ke = 0, pe = 0;
        const auto& X = s.Positions();
        const auto& V = s.Velocities();
        for (size_t i = 0; i < X.size(); i++) {
            ke += 0.5 * mass * (V[i].x * V[i].x + V[i].y * V[i].y + V[i].z * V[i].z);
            pe += mass * 9.81 * X[i].y;   // PE = m g y (y up, g = 9.81 m/s^2)
            if (k >= warmup) {
                double v2 = V[i].x * V[i].x + V[i].y * V[i].y + V[i].z * V[i].z;
                if (v2 > vmaxOverall) vmaxOverall = v2;
            }
        }
        if (k == 0) { ke0 = ke; pe0 = pe; }
        if (k == warmup) keFirst = ke;
        if (k == steps - 1) { keLast = ke; peLast = pe; }
        if (k >= warmup) maxErr = std::max(maxErr, (double)s.AvgDensityError());
        if (k % 50 == 0 && k > 0)
            printf("[bench]   t=%5.2fs  KE=%9.1f  PE=%9.1f  KE+PE=%9.1f\n",
                   s.SimTime(), ke, pe, ke + pe);
    }
    printf("[bench] energy: particles=%d steps=%d (dam break, g=-9.81, viscosity on; stats after %d-step cold start)\n",
           s.FluidCount(), steps, warmup);
    printf("[bench]   initial KE=%.1f PE=%.1f | final KE=%.1f PE=%.1f | final KE+PE=%.1f (viscosity dissipates, so KE+PE must not grow)\n",
           ke0, pe0, keLast, peLast, keLast + peLast);
    printf("[bench]   KE @cold-end=%.1f -> KE @final=%.1f (%.1f%%; viscosity should dissipate)\n",
           keFirst, keLast, 100.0 * (keLast - keFirst) / keFirst);
    printf("[bench]   peak particle speed (post cold-start) = %.2f m/s, peak avg density err = %.3f%%\n",
           std::sqrt(vmaxOverall), 100.0 * maxErr);
    // Stability: no particle loss, density under control, KE not monotonically
    // growing (viscosity + wall impacts dissipate). Peak speed is reported but
    // not judged — SPH has particle-scale pressure noise (paper does not report
    // speeds either).
    const bool pass = (s.FluidCount() == (int)s.Positions().size()) &&
                      (maxErr < 0.02) && (keLast < keFirst * 1.5);
    printf("[bench]   %s (no particle loss, density err < 2%%, KE not exploding)\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
} // namespace

int RunBench(const std::string& name, const HeadlessOptions& opt) {
    if (opt.threads > 0) omp_set_num_threads(opt.threads);
    if (name == "hydrostatic")  return BenchHydrostatic(opt);
    if (name == "dambreak")     return BenchDamBreak(opt);
    if (name == "energy")       return BenchEnergy(opt);
    printf("[bench] unknown benchmark '%s'; available: hydrostatic, dambreak, energy\n", name.c_str());
    return 1;
}
