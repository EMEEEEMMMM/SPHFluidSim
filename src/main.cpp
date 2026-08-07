#include "Application.hpp"
#include "HeadlessRunner.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

// ---------------------------------------------------------------------------
// CLI:
//   IISPH.exe                       -> GUI mode (default)
//   IISPH.exe --gui                 -> GUI mode
//   IISPH.exe --headless            -> offline mode
//     --fps <n>        video frame rate        (default 30)
//     --frames <n>     total simulated frames  (default 300)
//     --seconds <s>    video length in seconds (frames = seconds * fps; overrides --frames)
//     -r <WxH>         output resolution       (default 1280x720)
//     --output <dir>   output directory        (default ./out)
//     --mode <m>       particles | surface     (default surface)
//     --particles <n>  approx. fluid particle count (default 8000)
//     --dt <float>     fixed timestep          (default: solver CFL)
//     --threads <n>    OpenMP threads          (default: all)
//     --no-video       keep only BMP frames
//     --keep-frames    keep frames/ after assembling the video
//     --bench <name>   run a verification benchmark and exit:
//                      hydrostatic | dambreak | energy
// ---------------------------------------------------------------------------
static bool Match(const char* arg, const char* name) {
    return std::strcmp(arg, name) == 0;
}
static bool MatchPrefix(const char* arg, const char* name) {
    return std::strncmp(arg, name, std::strlen(name)) == 0;
}
static std::string ValueAfter(const char* arg, const char* name) {
    return std::string(arg + std::strlen(name));
}

int main(int argc, char** argv) {
    bool headless = false;
    HeadlessOptions opt;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (Match(a, "--headless"))        { headless = true; }
        else if (Match(a, "--gui"))        { headless = false; }
        else if (MatchPrefix(a, "--fps=")) { opt.fps = atoi(ValueAfter(a, "--fps=").c_str()); }
        else if (Match(a, "--fps") && i + 1 < argc) { opt.fps = atoi(argv[++i]); }
        else if (MatchPrefix(a, "--frames=")) { opt.frames = atoi(ValueAfter(a, "--frames=").c_str()); }
        else if (Match(a, "--frames") && i + 1 < argc) { opt.frames = atoi(argv[++i]); }
        else if (MatchPrefix(a, "--seconds=")) { opt.seconds = atof(ValueAfter(a, "--seconds=").c_str()); }
        else if (Match(a, "--seconds") && i + 1 < argc) { opt.seconds = atof(argv[++i]); }
        else if (MatchPrefix(a, "-r=")) { /* -r=WxH */ }
        else if (Match(a, "-r") && i + 1 < argc) {
            const std::string r = argv[++i];
            const size_t x = r.find('x');
            if (x != std::string::npos) { opt.width = atoi(r.substr(0, x).c_str()); opt.height = atoi(r.substr(x + 1).c_str()); }
        }
        else if (MatchPrefix(a, "--output=")) { opt.output = ValueAfter(a, "--output="); }
        else if (Match(a, "--output") && i + 1 < argc) { opt.output = argv[++i]; }
        else if (MatchPrefix(a, "--mode=")) {
            const std::string m = ValueAfter(a, "--mode=");
            opt.mode = (m == "particles") ? HeadlessOptions::Mode::Particles : HeadlessOptions::Mode::Surface;
        }
        else if (Match(a, "--mode") && i + 1 < argc) {
            const std::string m = argv[++i];
            opt.mode = (m == "particles") ? HeadlessOptions::Mode::Particles : HeadlessOptions::Mode::Surface;
        }
        else if (MatchPrefix(a, "--particles=")) { opt.particles = atoi(ValueAfter(a, "--particles=").c_str()); }
        else if (Match(a, "--particles") && i + 1 < argc) { opt.particles = atoi(argv[++i]); }
        else if (MatchPrefix(a, "--dt=")) { opt.dt = (float)atof(ValueAfter(a, "--dt=").c_str()); }
        else if (Match(a, "--dt") && i + 1 < argc) { opt.dt = (float)atof(argv[++i]); }
        else if (MatchPrefix(a, "--grid=")) { opt.gridScale = (float)atof(ValueAfter(a, "--grid=").c_str()); }
        else if (Match(a, "--grid") && i + 1 < argc) { opt.gridScale = (float)atof(argv[++i]); }
        else if (MatchPrefix(a, "--threads=")) { opt.threads = atoi(ValueAfter(a, "--threads=").c_str()); }
        else if (Match(a, "--threads") && i + 1 < argc) { opt.threads = atoi(argv[++i]); }
        else if (Match(a, "--no-video"))      { opt.noVideo = true; }
        else if (Match(a, "--keep-frames"))   { opt.keepFrames = true; }
        else if (MatchPrefix(a, "--bench="))  { return RunBench(ValueAfter(a, "--bench="), opt); }
        else if (Match(a, "--bench") && i + 1 < argc) { return RunBench(argv[++i], opt); }
        else if (Match(a, "--help") || Match(a, "-h")) {
            printf("Usage: IISPH.exe [--gui|--headless] [options]\n"
                   "  --gui            interactive window (default)\n"
                   "  --headless       offline render to frames/video\n"
                   "  --fps <n>        video fps (default 30)\n"
                   "  --frames <n>     total frames (default 300)\n"
                   "  --seconds <s>    video length in seconds (frames = seconds * fps)\n"
                   "  -r <WxH>         resolution (default 1280x720)\n"
                   "  --output <dir>   output dir (default ./out)\n"
                   "  --mode <m>       particles | surface (default surface)\n"
                   "  --particles <n>  approx. fluid particles (default 8000)\n"
                   "  --dt <s>         fixed timestep\n"
                   "  --threads <n>    OpenMP threads\n"
                   "  --no-video       keep BMP frames only\n"
                   "  --keep-frames    keep frames after assembling video\n"
                   "  --bench <name>   hydrostatic | dambreak | energy\n");
            return 0;
        }
        else {
            printf("Unknown argument: %s (use --help)\n", a);
            return 1;
        }
    }

    if (headless) {
        // --seconds is a convenience: N seconds of video at `fps` = N * fps frames.
        if (opt.seconds > 0.0) {
            const int f = (int)(opt.seconds * opt.fps + 0.5);
            if (f > 0) opt.frames = f;
        }
        return RunHeadless(opt);
    }

    Application app(opt.width, opt.height, "IISPH - Implicit Incompressible SPH");
    return app.Run();
}
