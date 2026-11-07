#pragma once
#include <optional>
#include <string>

#include "IFluidSolver.hpp"

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
    int    particles = 8000;    // approx. fluid particle count (adjusts spacing)
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
};

// Runs the offline simulation and renders frames; returns 0 on success.
int RunHeadless(const HeadlessOptions& opt);

// Runs a verification benchmark (hydrostatic / dambreak / energy) and prints
// the metrics; returns 0 on success.
int RunBench(const std::string& name, const HeadlessOptions& opt);
