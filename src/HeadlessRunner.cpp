#include "HeadlessRunner.hpp"
#include "FluidSolver.hpp"
#include "SurfaceReconstructor.hpp"
#include "OpenGLLayer.hpp"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <vector>
#include <io.h>   // _popen / _pclose (MinGW)
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <direct.h>
#include <omp.h>

namespace {

bool MakeDirs(const std::string& dir) {
    return _mkdir(dir.c_str()) == 0;   // ignore "already exists"
}

// 24-bit bottom-up BMP written from BGRA pixel data (4 bytes/pixel from
// glReadPixels GL_BGRA): the blue/green/red bytes are already in BMP order.
void WriteBMP(const std::string& path, const unsigned char* bgra, int w, int h) {
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
    if (!f) return;
    fwrite(header, 1, 54, f);
    std::vector<unsigned char> row(rowSize);
    // glReadPixels returns rows bottom-up (row 0 = framebuffer bottom), and a
    // BMP also stores rows bottom-up — so write rows in order 0..h-1.
    for (int y = 0; y < h; y++) {
        const unsigned char* src = bgra + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];   // B
            row[x * 3 + 1] = src[x * 4 + 1];   // G
            row[x * 3 + 2] = src[x * 4 + 2];   // R
        }
        fwrite(row.data(), 1, rowSize, f);
    }
    fclose(f);
}

bool FfmpegAvailable() {
    return system("ffmpeg -version >nul 2>nul") == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Offline rendering: hidden GLFW window + OpenGL FBO, reused OpenGLLayer.
// ---------------------------------------------------------------------------
int RunHeadless(const HeadlessOptions& opt) {
    if (opt.threads > 0) omp_set_num_threads(opt.threads);

    // Hidden GLFW window providing a GL context (off-screen rendering).
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(opt.width, opt.height, "IISPH headless", nullptr, nullptr);
    if (!win) { printf("[headless] failed to create GL context\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        printf("[headless] failed to load GL\n"); return 1;
    }

    OpenGLLayer layer(opt.width, opt.height);
    FluidSolver solver;

    // --particles -> spacing: each scene defines a FIXED water volume, and the
    // spacing is derived so that N particles always fill that volume:
    //   volume = N * spacing^3  ->  spacing = (volume / N)^(1/3).
    // Choosing 8000 or 16000 particles therefore gives the SAME water volume
    // (the water looks identical, just more/less resolved).
    const bool isInflow = (opt.scene == "inflow");
    float waterVol = 0.512f;                       // dam: 0.8^3 water cube (open-top tank)
    if (isInflow) waterVol = 0.6f;                 // inflow: open tank filled to 60%
    const float waterSize = powf(waterVol, 1.0f / 3.0f);
    float spacing = powf(waterVol / (float)opt.particles, 1.0f / 3.0f);
    solver.GetParams().spacing = spacing;
    if (opt.dt > 0.0f) solver.GetParams().dt = opt.dt;
    if (isInflow) {
        // open-top cubic container (side 1 m at x=1, z=0.5) + central jet
        // falling 0.5 m from above (stronger jet)
        solver.InitInflow(1.0f, 0.5f, 100, 0.12f, 1.0f, 0.5f, opt.particles);
        layer.SetTankSize(1.0f, 1.0f, 1.0f, 1.0f, 0.5f, true);
    } else {
        // dam break in an OPEN-TOP tank: splash may leave the tank. With
        // --drop <frame> a spherical blob is spawned above the rim at that
        // frame and falls in (one-shot, like the GUI button).
        solver.InitDamOpen(2.0f, 1.2f, 1.0f, waterSize);
        layer.SetTankSize(2.0f, 1.2f, 1.0f, 0.0f, 0.0f, true);   // open top
    }

    // Camera: a closer three-quarter view centred on the tank.
    {
        auto& cam = layer.GetCamera();
        cam.Position = { 2.9f, 2.3f, 2.9f };
        const float tx = 1.0f, tz = 0.5f;
        const float ty = isInflow ? 0.5f : 0.55f;
        Math::Vector3 f = { tx - 2.9f, ty - 2.3f, tz - 2.9f };
        cam.Front = f * (1.0f / f.length());
        cam.Up = { 0.0f, 1.0f, 0.0f };
        cam.Zoom = 42.0f;
    }

    // Surface or particle view.
    const bool showSurface = (opt.mode == HeadlessOptions::Mode::Surface);
    layer.SetSurfaceVisible(showSurface);
    layer.SetParticlesVisible(!showSurface);
    SurfaceReconstructor recon;
    recon.SetGridScale(opt.gridScale);   // 1.0 = paper r (fast), 0.5 = r/2 high-quality

    std::string dir = opt.output;
    if (dir.empty()) dir = "out";
    MakeDirs(dir);
    // Default path: stream frames straight into ffmpeg over a pipe (no BMPs on
    // disk at all - a 14400-frame 1080p run is ~90 GB as BMPs, ~500 MB as mp4).
    // BMP frames are only written for --no-video (debug, manual ffmpeg) or
    // --keep-frames (old behaviour).
    const bool usePipe = !opt.noVideo && !opt.keepFrames && FfmpegAvailable();
    std::string framesDir = dir + "/frames";
    if (!usePipe) MakeDirs(framesDir);
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
            MakeDirs(framesDir);
        }
    }
    const bool writeBMP = !usePipe || !pipe;

    std::vector<unsigned char> pixels((size_t)opt.width * opt.height * 4);   // BGRA
    const int reconInterval = 3;   // rebuild mesh every N frames

    printf("[headless] particles=%d (spacing=%.4f)  frames=%d  fps=%d  mode=%s  res=%dx%d\n",
           solver.FluidCount(), spacing, opt.frames, opt.fps,
           showSurface ? "surface" : "particles", opt.width, opt.height);

    double tWall0 = omp_get_wtime();
    const int dropFrame = opt.dropFrame;   // -1 = never (spawn at this frame)
    for (int f = 0; f < opt.frames; f++) {
        if (!isInflow && dropFrame >= 0 && f == dropFrame && !solver.HasDrop()) {
            // one-shot: spherical blob above the tank rim, falls in (R=0.2)
            solver.SpawnDrop(0.2f, 1.0f, 1.6f, 0.5f);
            printf("[headless] drop spawned at frame %d (N=%d)\n", f, solver.FluidCount());
        }
        solver.Step();

        layer.UpdateParticles(solver.Positions(), solver.Colors());
        if (showSurface && (f % reconInterval == 0)) {
            if (recon.Reconstruct(solver.Positions(), solver.Spacing()))
                layer.UpdateSurfaceMesh(recon.Vertices(), recon.Indices());
        }

        layer.Render();

        // Read the FBO back to CPU: GL_BGRA is 4-byte aligned and needs no
        // channel swap for the BMP writer (rows stay bottom-up as in the BMP).
        auto fb = layer.GetFrameBuffer();
        fb->Bind();
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, opt.width, opt.height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
        fb->UBind();

        if (pipe) {
            // rawvideo bgr24 expects top-down rows; glReadPixels gives
            // bottom-up, so write rows in reverse order (row h-1 first).
            // MUST strip the alpha byte per pixel (BGRA -> BGR); a plain
            // fwrite of w*3 bytes would take 960 BGRA pixels, not 1280 BGR.
            std::vector<unsigned char> row((size_t)opt.width * 3);
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
            char name[64];
            snprintf(name, sizeof(name), "%s/frames/frame_%04d.bmp", dir.c_str(), f);
            WriteBMP(name, pixels.data(), opt.width, opt.height);
        }

        if (f % 30 == 0)
            printf("  frame %4d/%d  t=%.2fs  avg-rho-err=%.3f%%  iters=%d  tris=%d\n",
                   f + 1, opt.frames, solver.SimTime(), 100.0 * solver.AvgDensityError(),
                   solver.LastIterations(), layer.SurfaceTriangleCount());
    }
    double tWall1 = omp_get_wtime();
    printf("[headless] done: %d frames in %.1f s (%.1f fps wall), sim time %.2f s\n",
           opt.frames, tWall1 - tWall0, opt.frames / (tWall1 - tWall0), solver.SimTime());

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
                std::string rm = "rmdir /s /q \"" + dir + "\\frames\"";
                system(rm.c_str());
            }
        } else {
            printf("[headless] ffmpeg not found - frames saved in %s/frames/, install ffmpeg to assemble a video\n", dir.c_str());
        }
    }

    glfwTerminate();
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
    s.GetParams().spacing = 0.04f;
    if (opt.dt > 0) s.GetParams().dt = opt.dt;
    s.Init(2.0f, 1.2f, 1.0f, 0.8f);
    const float mass = 1000.0f * s.Spacing() * s.Spacing() * s.Spacing();
    const int steps = 400;
    const int warmup = 30;
    double ke0 = 0, pe0 = 0;
    double vmaxOverall = 0;
    double maxErr = 0;
    double keFirst = 0, keLast = 0;
    for (int k = 0; k < steps; k++) {
        s.Step();
        double ke = 0, pe = 0;
        const auto& X = s.Positions();
        const auto& V = s.Velocities();
        for (size_t i = 0; i < X.size(); i++) {
            ke += 0.5 * mass * (V[i].x * V[i].x + V[i].y * V[i].y + V[i].z * V[i].z);
            pe += mass * (-9.81) * X[i].y;   // PE = m g y (g negative)
            if (k >= warmup) {
                double v2 = V[i].x * V[i].x + V[i].y * V[i].y + V[i].z * V[i].z;
                if (v2 > vmaxOverall) vmaxOverall = v2;
            }
        }
        if (k == 0) { ke0 = ke; pe0 = pe; }
        if (k == warmup) keFirst = ke;
        if (k == steps - 1) keLast = ke;
        if (k >= warmup) maxErr = std::max(maxErr, (double)s.AvgDensityError());
        if (k % 50 == 0 && k > 0)
            printf("[bench]   t=%5.2fs  KE=%9.1f  PE=%9.1f  KE+PE=%9.1f\n",
                   s.SimTime(), ke, pe, ke + pe);
    }
    printf("[bench] energy: particles=%d steps=%d (dam break, g=-9.81, viscosity on; stats after %d-step cold start)\n",
           s.FluidCount(), steps, warmup);
    printf("[bench]   initial KE=%.1f PE=%.1f | final KE+PE=%.1f (dissipation from viscosity is expected)\n",
           ke0, pe0, ke0 + pe0 + 0.0);
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
