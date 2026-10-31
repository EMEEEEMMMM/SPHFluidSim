#include "Application.hpp"
#include "HeadlessRunner.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
#include <cctype>

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

// --solver takes IISPH or DFSPH, case-insensitive. "df" / "df sph" stay as
// aliases so older command lines keep working; they are not advertised.
static std::string NormalizeSolver(const std::string& v) {
    std::string s;
    for (char c : v) {
        if (c == ' ' || c == '\t' || c == '-') continue;
        s += (char)std::tolower((unsigned char)c);
    }
    if (s == "iisph") return "iisph";
    if (s == "dfsph" || s == "df") return "dfsph";
    return std::string();
}

// -r WxH / -r=WxH -> width/height. The "-r=" form used to be matched by an
// empty branch and silently ignored.
static bool ParseResolution(const std::string& r, HeadlessOptions& opt) {
    const size_t x = r.find('x');
    if (x == std::string::npos) return false;
    const int w = atoi(r.substr(0, x).c_str());
    const int h = atoi(r.substr(x + 1).c_str());
    if (w <= 0 || h <= 0) return false;
    opt.width = w; opt.height = h;
    return true;
}

// --drop <N>[f|s]: N frames (default), or N seconds (N*fps frames). "20f" -> frame
// 20, "20s" -> frame 20*fps (for --fps 30: frame 600).
static int ParseDropFrame(const std::string& v, int fps) {
    if (v.empty()) return -1;
    const char last = v.back();
    if (last == 's') return (int)(atof(v.c_str()) * fps + 0.5);   // seconds -> frames
    return atoi(v.c_str());                                        // 'f' or bare = frames
}

int main(int argc, char** argv) {
    bool headless = false;
    std::string benchName;   // --bench is run AFTER parsing all options
    std::string dropSpec;    // parsed after the loop: "5s" needs the FINAL --fps
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
        else if (MatchPrefix(a, "-r=")) {
            if (!ParseResolution(ValueAfter(a, "-r="), opt)) {
                printf("[args] ERROR: invalid resolution '%s' (expected WxH, e.g. 1920x1080)\n", a);
                return 1;
            }
        }
        else if (Match(a, "-r") && i + 1 < argc) {
            if (!ParseResolution(argv[++i], opt)) {
                printf("[args] ERROR: invalid resolution (expected WxH, e.g. 1920x1080)\n");
                return 1;
            }
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
        else if (MatchPrefix(a, "--scene=")) { opt.scene = ValueAfter(a, "--scene="); }
        else if (Match(a, "--scene") && i + 1 < argc) { opt.scene = argv[++i]; }
        else if (MatchPrefix(a, "--solver=")) { opt.solver = ValueAfter(a, "--solver="); if (opt.solver == "df") opt.solver = "df sph"; }
        else if (Match(a, "--solver") && i + 1 < argc) {
            opt.solver = argv[++i];
            if (opt.solver == "df" && i + 1 < argc && std::strcmp(argv[i + 1], "sph") == 0) {
                opt.solver = "df sph";
                i++;
            }
        }
        else if (MatchPrefix(a, "--drop=")) { dropSpec = ValueAfter(a, "--drop="); }
        else if (Match(a, "--drop") && i + 1 < argc) { dropSpec = argv[++i]; }
        else if (MatchPrefix(a, "--grid=")) { opt.gridScale = (float)atof(ValueAfter(a, "--grid=").c_str()); }
        else if (Match(a, "--grid") && i + 1 < argc) { opt.gridScale = (float)atof(argv[++i]); }
        else if (MatchPrefix(a, "--threads=")) { opt.threads = atoi(ValueAfter(a, "--threads=").c_str()); }
        else if (Match(a, "--threads") && i + 1 < argc) { opt.threads = atoi(argv[++i]); }
        else if (Match(a, "--no-video"))      { opt.noVideo = true; }
        else if (Match(a, "--keep-frames"))   { opt.keepFrames = true; }
        else if (Match(a, "--export-particles")) { opt.exportParticles = true; }
        else if (MatchPrefix(a, "--bench="))  { benchName = ValueAfter(a, "--bench="); }
        else if (Match(a, "--bench") && i + 1 < argc) { benchName = argv[++i]; }
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
                   "  --scene <s>      dam | inflow (default dam; dam tank is open-top)\n"
                   "  --solver <s>     IISPH | DFSPH (default IISPH; DFSPH = Bender & Koschier 2017)\n"
                   "  --drop <N>[f|s]  spawn the one-shot water drop at frame N, or N seconds in ('20f'/'20s', dam only)\n"
                   "  --dt <s>         fixed timestep\n"
                   "  --threads <n>    OpenMP threads\n"
                   "  --no-video       keep BMP frames only\n"
                   "  --keep-frames    keep frames after assembling video\n"
                   "  --export-particles  dump per-frame particle positions as PLY (headless only)\n"
                   "  --bench <name>   hydrostatic | dambreak | energy\n");
            return 0;
        }
        else {
            printf("Unknown argument: %s (use --help)\n", a);
            return 1;
        }
    }

    // --drop depends on the FINAL fps, whatever order the flags were given in.
    if (!dropSpec.empty()) opt.dropFrame = ParseDropFrame(dropSpec, opt.fps);

    // Validate before doing any work: a typo used to be accepted silently
    // (0 frames, 0x0 render target, negative particle count, ...).
    if (opt.width <= 0 || opt.height <= 0 || opt.width > 16384 || opt.height > 16384) {
        printf("[args] ERROR: invalid resolution %dx%d (use -r WxH, 1..16384)\n", opt.width, opt.height);
        return 1;
    }
    if (opt.fps <= 0 || opt.fps > 1000) {
        printf("[args] ERROR: invalid --fps %d (1..1000)\n", opt.fps);
        return 1;
    }
    if (opt.frames <= 0) {
        printf("[args] ERROR: invalid frame count %d (--frames / --seconds)\n", opt.frames);
        return 1;
    }
    if (opt.particles <= 0) {
        printf("[args] ERROR: invalid --particles %d (>0)\n", opt.particles);
        return 1;
    }
    if (!(opt.gridScale > 0.0f) || opt.gridScale > 4.0f) {
        printf("[args] ERROR: invalid --grid %.3f (0 < grid <= 4)\n", opt.gridScale);
        return 1;
    }
    if (opt.dt < 0.0f) {
        printf("[args] ERROR: invalid --dt %.5f (>=0; 0 = CFL)\n", opt.dt);
        return 1;
    }
    {
        const std::string norm = NormalizeSolver(opt.solver);
        if (norm.empty()) {
            printf("[args] WARNING: unknown --solver '%s' -> using IISPH (valid: IISPH | DFSPH)\n", opt.solver.c_str());
            opt.solver = "iisph";
        } else {
            opt.solver = norm;
        }
    }

    if (!benchName.empty()) return RunBench(benchName, opt);   // --bench with any other options

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
