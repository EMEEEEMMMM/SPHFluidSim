#pragma once
#include <string>

// Command-line options for the headless (offline) mode.
struct HeadlessOptions {
    int    fps       = 30;      // output video frame rate
    int    frames    = 300;     // total simulated frames
    double seconds   = 0.0;     // convenience: video length in seconds (frames = seconds * fps); 0 = use `frames`
    int    width     = 1280;    // output resolution
    int    height    = 720;
    std::string output = "out"; // output directory (frames/ + video)
    enum class Mode { Particles, Surface } mode = Mode::Surface;
    int    particles = 8000;    // approx. fluid particle count (adjusts spacing)
    float  dt        = 0.0f;    // fixed timestep (0 = solver default / CFL)
    float  gridScale = 1.0f;    // surface-reconstruction grid: 1.0 = r (fast), 0.5 = r/2 high quality
    int    threads   = 0;       // OpenMP threads (0 = all)
    bool   noVideo   = false;   // skip ffmpeg assembly, keep the BMP frames
    bool   keepFrames = false;  // keep frames/ after assembling the video
};

// Runs the offline simulation and renders frames; returns 0 on success.
int RunHeadless(const HeadlessOptions& opt);

// Runs a verification benchmark (hydrostatic / dambreak / energy) and prints
// the metrics; returns 0 on success.
int RunBench(const std::string& name, const HeadlessOptions& opt);
