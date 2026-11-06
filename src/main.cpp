#include <QApplication>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "HeadlessRunner.hpp"
#include "Session/SimulationSession.hpp"
#include "Ui/Qml/QmlFrontend.hpp"
#include "Ui/Theme.hpp"
#include "Ui/Widgets/MainWindow.hpp"
#include "Ui/Widgets/Viewport.hpp"

// ---------------------------------------------------------------------------
// CLI:
//   SPHFluidSim.exe                 -> GUI mode (Qt), QML front-end by default
//   SPHFluidSim.exe --ui widgets    -> GUI mode, Qt Widgets front-end
//   SPHFluidSim.exe --ui qml        -> GUI mode, Qt Quick front-end
//   SPHFluidSim.exe --headless      -> offline mode
//     --fps <n>        video frame rate        (default 30)
//     --frames <n>     total simulated frames  (default 300)
//     --seconds <s>    video length in seconds (frames = seconds * fps; overrides --frames)
//     --time-scale <s> simulated seconds per video second (default 1)
//     -r <WxH>         output resolution       (default 1280x720)
//     --output <dir>   output directory        (default ./out)
//     --mode <m>       particles | surface     (default surface)
//     --particles <n>  approx. fluid particle count (default 8000)
//     --dt <float>     nominal timestep        (default: solver CFL)
//     --threads <n>    OpenMP threads          (default: all)
//     --no-video       keep only BMP frames
//     --keep-frames    keep frames/ after assembling the video
//     --bench <name>   run a verification benchmark and exit:
//                      hydrostatic | dambreak | energy
//   GUI-only extras:
//     --ui <widgets|qml>   which front-end to start
//     --theme <0..3>       start with a specific skin
//     --screenshot <path>  render a few frames, save a PNG and exit
// ---------------------------------------------------------------------------
namespace {

bool Match(const char* arg, const char* name) { return std::strcmp(arg, name) == 0; }

bool MatchPrefix(const char* arg, const char* name) {
    return std::strncmp(arg, name, std::strlen(name)) == 0;
}

std::string ValueAfter(const char* arg, const char* name) {
    return std::string(arg + std::strlen(name));
}

// --solver takes IISPH or DFSPH, case-insensitive. "df" / "df sph" stay as
// aliases so older command lines keep working; they are not advertised.
std::string NormalizeSolver(const std::string& v) {
    std::string s;
    for (char c : v) {
        if (c == ' ' || c == '\t' || c == '-') continue;
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (s == "iisph") return "iisph";
    if (s == "dfsph" || s == "df") return "dfsph";
    return std::string();
}

// -r WxH / -r=WxH -> width/height.
bool ParseResolution(const std::string& r, HeadlessOptions& opt) {
    const size_t x = r.find('x');
    if (x == std::string::npos) return false;
    const int w = atoi(r.substr(0, x).c_str());
    const int h = atoi(r.substr(x + 1).c_str());
    if (w <= 0 || h <= 0) return false;
    opt.width = w;
    opt.height = h;
    return true;
}

// --drop <N>[f|s]: frame N on the video timeline (default), or N video seconds.
// HeadlessRunner converts this video timestamp to the simulation timeline using
// timeScale, so changing fps alone does not change when the physics event occurs.
double ParseDropVideoSeconds(const std::string& v, int fps) {
    if (v.empty()) return -1.0;
    const char last = v.back();
    if (last == 's') return atof(v.c_str());
    return static_cast<double>(atoi(v.c_str())) / static_cast<double>(fps);
}

// Reads the same versioned JSON document the GUI writes for a render job.
// Missing keys keep existing defaults; malformed values are reported before
// any output directory or GL context is created.
bool ApplyRenderConfig(const std::string& path, HeadlessOptions& opt, std::string& error) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        error = "cannot open config file '" + path + "'";
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        error = "invalid JSON config: " + parseError.errorString().toStdString();
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.contains(QStringLiteral("app")) &&
        root.value(QStringLiteral("app")).toString() != QStringLiteral("IISPH")) {
        error = "config belongs to a different application";
        return false;
    }
    if (root.contains(QStringLiteral("schemaVersion")) &&
        root.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        error = "unsupported config schema version";
        return false;
    }

    const QJsonObject simulation = root.value(QStringLiteral("simulation")).toObject();
    const QJsonObject render = root.value(QStringLiteral("render")).toObject();
    auto number = [](const QJsonObject& object, const char* key, double& out) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (!value.isDouble()) return false;
        out = value.toDouble();
        return std::isfinite(out);
    };
    double value = 0.0;

    if (simulation.value(QStringLiteral("scene")).isString())
        opt.scene = simulation.value(QStringLiteral("scene")).toString().toStdString();
    if (simulation.value(QStringLiteral("solver")).isString()) {
        const std::string normalized =
            NormalizeSolver(simulation.value(QStringLiteral("solver")).toString().toStdString());
        if (!normalized.empty()) opt.solver = normalized;
    }
    if (number(simulation, "particleCount", value) && value > 0.0)
        opt.particles = static_cast<int>(value);
    if (simulation.contains(QStringLiteral("spacing")) &&
        number(simulation, "spacing", value))
        opt.spacing = static_cast<float>(value);
    if (simulation.contains(QStringLiteral("dt")) && number(simulation, "dt", value))
        opt.dt = static_cast<float>(value);
    if (simulation.value(QStringLiteral("useCFL")).isBool())
        opt.useCFL = simulation.value(QStringLiteral("useCFL")).toBool();
    if (simulation.contains(QStringLiteral("omega")) && number(simulation, "omega", value))
        opt.omega = static_cast<float>(value);
    if (simulation.contains(QStringLiteral("eta")) && number(simulation, "eta", value))
        opt.eta = static_cast<float>(value);
    if (simulation.contains(QStringLiteral("etaV")) && number(simulation, "etaV", value))
        opt.etaV = static_cast<float>(value);
    if (simulation.contains(QStringLiteral("maxIter")) && number(simulation, "maxIter", value))
        opt.maxIter = static_cast<int>(value);
    if (simulation.contains(QStringLiteral("viscosity")) &&
        number(simulation, "viscosity", value))
        opt.viscosity = static_cast<float>(value);
    if (simulation.contains(QStringLiteral("surfaceTension")) &&
        number(simulation, "surfaceTension", value))
        opt.surfaceTension = static_cast<float>(value);
    if (simulation.contains(QStringLiteral("wallAdhesion")) &&
        number(simulation, "wallAdhesion", value))
        opt.wallAdhesion = static_cast<float>(value);

    if (render.value(QStringLiteral("mode")).isString())
        opt.mode = render.value(QStringLiteral("mode")).toString() == QStringLiteral("particles")
            ? HeadlessOptions::Mode::Particles : HeadlessOptions::Mode::Surface;
    if (number(render, "fps", value)) opt.fps = static_cast<int>(value);
    if (number(render, "frames", value)) opt.frames = static_cast<int>(value);
    if (number(render, "seconds", value)) opt.seconds = value;
    if (number(render, "timeScale", value)) opt.timeScale = value;
    if (number(render, "width", value)) opt.width = static_cast<int>(value);
    if (number(render, "height", value)) opt.height = static_cast<int>(value);
    if (number(render, "gridScale", value)) opt.gridScale = static_cast<float>(value);
    if (number(render, "surfaceThreshold", value))
        opt.surfaceThreshold = static_cast<float>(value);
    if (number(render, "reconInterval", value))
        opt.reconInterval = static_cast<int>(value);
    if (number(render, "threads", value)) opt.threads = static_cast<int>(value);
    if (render.value(QStringLiteral("output")).isString())
        opt.output = render.value(QStringLiteral("output")).toString().toStdString();
    if (render.value(QStringLiteral("exportParticles")).isBool())
        opt.exportParticles = render.value(QStringLiteral("exportParticles")).toBool();
    if (render.value(QStringLiteral("keepFrames")).isBool())
        opt.keepFrames = render.value(QStringLiteral("keepFrames")).toBool();
    if (render.value(QStringLiteral("noVideo")).isBool())
        opt.noVideo = render.value(QStringLiteral("noVideo")).toBool();

    const QJsonArray events = root.value(QStringLiteral("events")).toArray();
    for (const QJsonValue& eventValue : events) {
        const QJsonObject event = eventValue.toObject();
        if (event.value(QStringLiteral("type")).toString() != QStringLiteral("drop")) continue;
        if (number(event, "time", value) && value >= 0.0) opt.dropVideoSeconds = value;
    }
    return true;
}

int RunGui(QApplication& app, const std::string& uiKind, const std::string& screenshotPath,
           int screenshotDelayMs, bool autoplay, int viewMode) {
    // Use a font family that actually ships with Windows 10/11. The previous
    // "Segoe UI Variable Text" is only present on newer Windows builds; when it
    // is missing Qt falls back to the legacy bitmap MS Sans Serif, which
    // DirectWrite cannot load and logs CreateFontFaceFromHDC() failures for.
    QFont base = app.font();
    base.setFamily(QStringLiteral("Segoe UI"));
    base.setPointSizeF(10.0);
    app.setFont(base);

    SimulationSession session;
    if (viewMode >= 0) session.setParam(QStringLiteral("viewMode"), viewMode);
    if (uiKind == "qml") {
        return ui::RunQmlFrontend(app, session, autoplay, screenshotPath, screenshotDelayMs);
    }

    ui::MainWindow window(&session);
    session.start();
    if (autoplay) session.setRunning(true);
    window.show();

    if (!screenshotPath.empty()) {
        const QString path = QString::fromStdString(screenshotPath);
        QTimer::singleShot(screenshotDelayMs, &window, [&window, path] {
            QPixmap shot = window.grab();
            shot.save(path, "PNG");
            QCoreApplication::quit();
        });
    }
    return app.exec();
}

}  // namespace

int main(int argc, char** argv) {
    bool headless = false;
    std::string benchName;
    std::string dropSpec;
    std::string uiKind = "qml";
    std::string screenshotPath;
    std::string configPath;
    int screenshotDelayMs = 1500;
    int themeOverride = -1;
    bool autoplay = false;
    int viewMode = -1;
    HeadlessOptions opt;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (Match(a, "--headless")) {
            headless = true;
        } else if (Match(a, "--control")) {
            opt.control = true;
        } else if (MatchPrefix(a, "--config=")) {
            configPath = ValueAfter(a, "--config=");
        } else if (Match(a, "--config") && i + 1 < argc) {
            configPath = argv[++i];
        } else if (Match(a, "--gui")) {
            headless = false;
        } else if (MatchPrefix(a, "--ui=")) {
            uiKind = ValueAfter(a, "--ui=");
        } else if (Match(a, "--ui") && i + 1 < argc) {
            uiKind = argv[++i];
        } else if (MatchPrefix(a, "--theme=")) {
            themeOverride = atoi(ValueAfter(a, "--theme=").c_str());
        } else if (Match(a, "--theme") && i + 1 < argc) {
            themeOverride = atoi(argv[++i]);
        } else if (MatchPrefix(a, "--screenshot=")) {
            screenshotPath = ValueAfter(a, "--screenshot=");
        } else if (Match(a, "--screenshot") && i + 1 < argc) {
            screenshotPath = argv[++i];
        } else if (MatchPrefix(a, "--delay=")) {
            screenshotDelayMs = atoi(ValueAfter(a, "--delay=").c_str());
        } else if (Match(a, "--delay") && i + 1 < argc) {
            screenshotDelayMs = atoi(argv[++i]);
        } else if (Match(a, "--play")) {
            autoplay = true;
        } else if (MatchPrefix(a, "--view=")) {
            viewMode = atoi(ValueAfter(a, "--view=").c_str());
        } else if (Match(a, "--view") && i + 1 < argc) {
            viewMode = atoi(argv[++i]);
        } else if (MatchPrefix(a, "--fps=")) {
            opt.fps = atoi(ValueAfter(a, "--fps=").c_str());
        } else if (Match(a, "--fps") && i + 1 < argc) {
            opt.fps = atoi(argv[++i]);
        } else if (MatchPrefix(a, "--frames=")) {
            opt.frames = atoi(ValueAfter(a, "--frames=").c_str());
        } else if (Match(a, "--frames") && i + 1 < argc) {
            opt.frames = atoi(argv[++i]);
        } else if (MatchPrefix(a, "--seconds=")) {
            opt.seconds = atof(ValueAfter(a, "--seconds=").c_str());
        } else if (Match(a, "--seconds") && i + 1 < argc) {
            opt.seconds = atof(argv[++i]);
        } else if (MatchPrefix(a, "--time-scale=")) {
            opt.timeScale = atof(ValueAfter(a, "--time-scale=").c_str());
        } else if (Match(a, "--time-scale") && i + 1 < argc) {
            opt.timeScale = atof(argv[++i]);
        } else if (MatchPrefix(a, "-r=")) {
            if (!ParseResolution(ValueAfter(a, "-r="), opt)) {
                printf("[args] ERROR: invalid resolution '%s' (expected WxH, e.g. 1920x1080)\n", a);
                return 1;
            }
        } else if (Match(a, "-r") && i + 1 < argc) {
            if (!ParseResolution(argv[++i], opt)) {
                printf("[args] ERROR: invalid resolution (expected WxH, e.g. 1920x1080)\n");
                return 1;
            }
        } else if (MatchPrefix(a, "--output=")) {
            opt.output = ValueAfter(a, "--output=");
        } else if (Match(a, "--output") && i + 1 < argc) {
            opt.output = argv[++i];
        } else if (MatchPrefix(a, "--mode=")) {
            const std::string m = ValueAfter(a, "--mode=");
            opt.mode = (m == "particles") ? HeadlessOptions::Mode::Particles
                                          : HeadlessOptions::Mode::Surface;
        } else if (Match(a, "--mode") && i + 1 < argc) {
            const std::string m = argv[++i];
            opt.mode = (m == "particles") ? HeadlessOptions::Mode::Particles
                                          : HeadlessOptions::Mode::Surface;
        } else if (MatchPrefix(a, "--particles=")) {
            opt.particles = atoi(ValueAfter(a, "--particles=").c_str());
        } else if (Match(a, "--particles") && i + 1 < argc) {
            opt.particles = atoi(argv[++i]);
        } else if (MatchPrefix(a, "--dt=")) {
            opt.dt = static_cast<float>(atof(ValueAfter(a, "--dt=").c_str()));
        } else if (Match(a, "--dt") && i + 1 < argc) {
            opt.dt = static_cast<float>(atof(argv[++i]));
        } else if (MatchPrefix(a, "--scene=")) {
            opt.scene = ValueAfter(a, "--scene=");
        } else if (Match(a, "--scene") && i + 1 < argc) {
            opt.scene = argv[++i];
        } else if (MatchPrefix(a, "--solver=")) {
            opt.solver = ValueAfter(a, "--solver=");
            if (opt.solver == "df") opt.solver = "df sph";
        } else if (Match(a, "--solver") && i + 1 < argc) {
            opt.solver = argv[++i];
            if (opt.solver == "df" && i + 1 < argc && std::strcmp(argv[i + 1], "sph") == 0) {
                opt.solver = "df sph";
                i++;
            }
        } else if (MatchPrefix(a, "--drop=")) {
            dropSpec = ValueAfter(a, "--drop=");
        } else if (Match(a, "--drop") && i + 1 < argc) {
            dropSpec = argv[++i];
        } else if (MatchPrefix(a, "--grid=")) {
            opt.gridScale = static_cast<float>(atof(ValueAfter(a, "--grid=").c_str()));
        } else if (Match(a, "--grid") && i + 1 < argc) {
            opt.gridScale = static_cast<float>(atof(argv[++i]));
        } else if (MatchPrefix(a, "--threads=")) {
            opt.threads = atoi(ValueAfter(a, "--threads=").c_str());
        } else if (Match(a, "--threads") && i + 1 < argc) {
            opt.threads = atoi(argv[++i]);
        } else if (Match(a, "--no-video")) {
            opt.noVideo = true;
        } else if (Match(a, "--keep-frames")) {
            opt.keepFrames = true;
        } else if (Match(a, "--export-particles")) {
            opt.exportParticles = true;
        } else if (MatchPrefix(a, "--bench=")) {
            benchName = ValueAfter(a, "--bench=");
        } else if (Match(a, "--bench") && i + 1 < argc) {
            benchName = argv[++i];
        } else if (Match(a, "--help") || Match(a, "-h")) {
            printf("Usage: SPHFluidSim.exe [--gui|--headless] [options]\n"
                   "  --gui            interactive window (default)\n"
                   "  --ui <k>         widgets | qml (default qml)\n"
                   "  --theme <n>      start with skin 0..3\n"
                   "  --screenshot <p> render a few frames, save PNG, exit (GUI only)\n"
                   "  --headless       offline render to frames/video\n"
                   "  --fps <n>        video fps (default 30)\n"
                   "  --frames <n>     total frames (default 300)\n"
                   "  --seconds <s>    video length in seconds (frames = seconds * fps)\n"
                   "  --time-scale <s> simulated seconds per video second (default 1)\n"
                   "                   0.1 = 10x slow motion, 2 = 2x fast motion\n"
                   "  -r <WxH>         resolution (default 1280x720)\n"
                   "  --output <dir>   output dir (default ./out)\n"
                   "  --mode <m>       particles | surface (default surface)\n"
                   "  --particles <n>  approx. fluid particles (default 8000)\n"
                   "  --scene <s>      dam | inflow (default dam; dam tank is open-top)\n"
                   "  --solver <s>     IISPH | DFSPH (default IISPH)\n"
                   "  --drop <N>[f|s]  spawn at video frame N or video time N seconds (dam only)\n"
                   "  --dt <s>         nominal timestep (CFL may shorten it)\n"
                   "  --threads <n>    OpenMP threads\n"
                   "  --no-video       keep BMP frames only\n"
                   "  --keep-frames    keep frames after assembling video\n"
                   "  --export-particles  dump per-frame particle positions as PLY\n"
                   "  --bench <name>   hydrostatic | dambreak | energy\n");
            return 0;
        } else {
            printf("Unknown argument: %s (use --help)\n", a);
            return 1;
        }
    }

    if (!configPath.empty()) {
        std::string configError;
        if (!ApplyRenderConfig(configPath, opt, configError)) {
            printf("[args] ERROR: %s\n", configError.c_str());
            return 1;
        }
    }
    if (!dropSpec.empty()) opt.dropVideoSeconds = ParseDropVideoSeconds(dropSpec, opt.fps);

    if (opt.width <= 0 || opt.height <= 0 || opt.width > 16384 || opt.height > 16384) {
        printf("[args] ERROR: invalid resolution %dx%d (use -r WxH, 1..16384)\n", opt.width,
               opt.height);
        return 1;
    }
    if (opt.fps <= 0 || opt.fps > 1000) {
        printf("[args] ERROR: invalid --fps %d (1..1000)\n", opt.fps);
        return 1;
    }
    if (!(opt.timeScale > 0.0) || !std::isfinite(opt.timeScale)) {
        printf("[args] ERROR: invalid --time-scale %.5g (must be finite and >0)\n", opt.timeScale);
        return 1;
    }
    if (!dropSpec.empty() &&
        (!(opt.dropVideoSeconds >= 0.0) || !std::isfinite(opt.dropVideoSeconds))) {
        printf("[args] ERROR: invalid --drop '%s' (expected a non-negative frame or seconds)\n",
               dropSpec.c_str());
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
            printf("[args] WARNING: unknown --solver '%s' -> using IISPH (valid: IISPH | DFSPH)\n",
                   opt.solver.c_str());
            opt.solver = "iisph";
        } else {
            opt.solver = norm;
        }
    }

    if (!benchName.empty()) return RunBench(benchName, opt);

    // QOffscreenSurface and QOpenGLContext require a QGuiApplication even when
    // no window or Qt event loop is used. Benchmarks stay solver-only and exit
    // before this point, exactly as before.
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SPHFluidSim"));
    QCoreApplication::setApplicationName(QStringLiteral("SPHFluidSim"));

    // The renderer is GL 3.3 core. Ask for that profile before creating any
    // widget, QML item, or offscreen surface.
    {
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setDepthBufferSize(24);
        format.setStencilBufferSize(8);
        format.setSwapInterval(1);
        QSurfaceFormat::setDefaultFormat(format);
    }

    if (headless) {
        if (opt.seconds > 0.0) {
            const int f = static_cast<int>(opt.seconds * opt.fps + 0.5);
            if (f > 0) opt.frames = f;
        }
        return RunHeadless(opt);
    }

    if (themeOverride >= 0) ui::Theme::instance().setIndex(themeOverride);
    return RunGui(app, uiKind, screenshotPath, screenshotDelayMs, autoplay, viewMode);
}
