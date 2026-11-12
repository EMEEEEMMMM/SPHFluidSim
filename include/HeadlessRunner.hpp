#pragma once
#include <optional>
#include <string>

#include "IFluidSolver.hpp"

// Small config helpers shared by the CLI parser and its unit tests.
namespace headless_config {

// Parse a "#RRGGBB" string into 0..1 RGB floats. Returns false on any
// malformed input so callers can keep their default background. Header-only on
// purpose: the CLI's ApplyRenderConfig and tests must use the exact same
// conversion for render.background.
inline bool ParseHexColor(const std::string& text, float out[3]) {
    std::string s = text;
    const auto notSpace = [](char c) {
        return c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v';
    };
    size_t begin = 0;
    while (begin < s.size() && !notSpace(s[begin])) ++begin;
    size_t end = s.size();
    while (end > begin && !notSpace(s[end - 1])) --end;
    s = s.substr(begin, end - begin);
    if (s.size() != 7 || s[0] != '#') return false;
    auto hexDigit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int channel[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        const int hi = hexDigit(s[1 + 2 * i]);
        const int lo = hexDigit(s[2 + 2 * i]);
        if (hi < 0 || lo < 0) return false;
        channel[i] = hi * 16 + lo;
    }
    out[0] = static_cast<float>(channel[0]) / 255.0f;
    out[1] = static_cast<float>(channel[1]) / 255.0f;
    out[2] = static_cast<float>(channel[2]) / 255.0f;
    return true;
}

}  // namespace headless_config

// Shared by the renderer and its tests so output cadence cannot drift from the
// simulation clock. `frame` is zero-based; frame 0 is rendered after advancing
// to the end of its first video-frame interval.
namespace headless_timing {
inline double FrameTargetSimTime(int frame, int fps, double timeScale) {
    return static_cast<double>(frame + 1) * timeScale / static_cast<double>(fps);
}

template <typename AfterStep>
void AdvanceTo(IFluidSolver& solver, double targetSimTime, AfterStep afterStep) {
    while (static_cast<double>(solver.SimTime()) < targetSimTime) {
        solver.Step();
        afterStep();
    }
}

inline void AdvanceTo(IFluidSolver& solver, double targetSimTime) {
    AdvanceTo(solver, targetSimTime, [] {});
}
} // namespace headless_timing

// Command-line options for the headless (offline) mode.
struct HeadlessOptions {
    int    fps       = 30;      // output video frame rate
    int    frames    = 300;     // total rendered frames
    double seconds   = 0.0;     // video length in seconds (frames = seconds * fps); 0 = use `frames`
    double timeScale = 1.0;     // simulated seconds per video second (1 = real-time playback)
    int    width     = 1280;    // output resolution
    int    height    = 720;
    std::string output = "out"; // output directory (frames/ + video)
    enum class Mode { Particles, Surface } mode = Mode::Surface;
    std::string scene = "dam";    // "dam" | "inflow" (dam = open-top tank + optional --drop)
    std::string solver = "iisph"; // "iisph" | "df sph" (DFSPH, Bender & Koschier 2017)
    std::string backend = "auto"; // "auto" | "cpu" | "cuda"
    double dropVideoSeconds = -1.0; // drop event on the video timeline (-1 = never)
    int    particles = 8000;    // exact fluid particle count (spacing follows)
    float  dt        = 0.0f;    // maximum nominal timestep (0 = solver default; CFL may shorten it)
    // Optional values are used by the JSON/GUI path. Leaving them unset keeps
    // the solver defaults and the original command-line behaviour.
    std::optional<float> spacing;
    std::optional<bool> useCFL;
    std::optional<float> omega;
    std::optional<float> eta;
    std::optional<float> etaV;
    std::optional<int> maxIter;
    std::optional<float> viscosity;
    std::optional<float> surfaceTension;
    std::optional<float> wallAdhesion;
    float  gridScale = 1.0f;    // surface-reconstruction grid: 1.0 = r (fast), 0.5 = r/2 high quality
    float  surfaceThreshold = 0.8f;
    int    reconInterval = 3;
    int    threads   = 0;       // OpenMP threads (0 = all)
    bool   control   = false;   // machine-readable progress + stdin pause/resume/cancel
    bool   noVideo   = false;   // skip ffmpeg assembly, keep the BMP frames
    bool   keepFrames = false;  // keep frames/ after assembling the video
    // Headless only: dump every frame's particle positions as <output>/particles/
    // frame_%04d.ply (binary little-endian, 3 floats per vertex) for Houdini /
    // Blender. The GUI has no equivalent switch.
    bool   exportParticles = false;

    // v2 config (written by the GUI into render.camera, render.background and
    // events[0]). Leaving these at their defaults preserves the original
    // headless behaviour; explicitCamera gates the yaw/pitch-derived eye.
    struct CameraConfig {
        // eye (2.9, 2.3, 2.9) -> target (1, 0.55, 0.5), fov 42: the fixed
        // offline view used before render.camera existed.
        double yaw = -128.35, pitch = -29.77, distance = 3.526;
        double targetX = 1.0, targetY = 0.55, targetZ = 0.5, fov = 42.0;
        bool explicitCamera = false;
    };
    struct DropConfig { double radius = 0.2, x = 1.0, y = 1.6, z = 0.5; };
    CameraConfig camera;
    DropConfig drop;
    float background[3] = {0.12f, 0.15f, 0.22f};   // #1F2638
};

// Runs the offline simulation and renders frames; returns 0 on success.
int RunHeadless(const HeadlessOptions& opt);

// Runs a verification benchmark (hydrostatic / dambreak / energy) and prints
// the metrics; returns 0 on success.
int RunBench(const std::string& name, const HeadlessOptions& opt);
