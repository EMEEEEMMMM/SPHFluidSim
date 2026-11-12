#include "HeadlessRunner.hpp"
#include "Render/CameraRig.hpp"
#include "Render/RenderJob.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool Approx(double a, double b, double eps = 1.0e-3) {
    return std::fabs(a - b) <= eps;
}

QVariantMap BaseConfig(const QString& output, const QString& scene = QStringLiteral("dam")) {
    QVariantMap simulation;
    simulation.insert(QStringLiteral("scene"), scene);
    simulation.insert(QStringLiteral("solver"), QStringLiteral("IISPH"));

    QVariantMap render;
    render.insert(QStringLiteral("mode"), QStringLiteral("particles"));
    render.insert(QStringLiteral("fps"), 30);
    render.insert(QStringLiteral("frames"), 2);
    render.insert(QStringLiteral("width"), 64);
    render.insert(QStringLiteral("height"), 64);
    render.insert(QStringLiteral("noVideo"), true);
    render.insert(QStringLiteral("output"), output);

    QVariantMap root;
    root.insert(QStringLiteral("app"), QStringLiteral("IISPH"));
    root.insert(QStringLiteral("schemaVersion"), 2);
    root.insert(QStringLiteral("simulation"), simulation);
    root.insert(QStringLiteral("render"), render);
    root.insert(QStringLiteral("events"), QVariantList{});
    return root;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // 1. CLI defaults equal the former hard-coded headless eye (2.9,2.3,2.9).
    const HeadlessOptions defaults;
    if (!Check(Approx(defaults.camera.yaw, -128.35) &&
               Approx(defaults.camera.pitch, -29.77) &&
               Approx(defaults.camera.distance, 3.526) &&
               Approx(defaults.camera.targetX, 1.0) &&
               Approx(defaults.camera.targetY, 0.55) &&
               Approx(defaults.camera.targetZ, 0.5) &&
               Approx(defaults.camera.fov, 42.0),
               "headless camera defaults drifted")) return 1;
    if (!Check(Approx(defaults.background[0], 0.12f) &&
               Approx(defaults.background[1], 0.15f) &&
               Approx(defaults.background[2], 0.22f),
               "default background is not #1F2638")) return 1;
    if (!Check(!defaults.camera.explicitCamera,
               "camera must stay implicit until render.camera is present")) return 1;

    Math::Vector3 eye = render::FrontVector(static_cast<float>(defaults.camera.yaw),
                                            static_cast<float>(defaults.camera.pitch));
    eye = Math::Vector3(static_cast<float>(defaults.camera.targetX),
                        static_cast<float>(defaults.camera.targetY),
                        static_cast<float>(defaults.camera.targetZ)) -
          eye * static_cast<float>(defaults.camera.distance);
    if (!Check(Approx(eye.x, 2.9, 0.02) && Approx(eye.y, 2.3, 0.02) &&
               Approx(eye.z, 2.9, 0.02),
               "default yaw/pitch/distance do not reconstruct eye (2.9,2.3,2.9)")) return 1;

    // 2. Colour parsing.
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    if (!Check(headless_config::ParseHexColor("#1F2638", rgb), "#1F2638 rejected")) return 1;
    if (!Check(Approx(rgb[0], 0x1F / 255.0f) && Approx(rgb[1], 0x26 / 255.0f) &&
               Approx(rgb[2], 0x38 / 255.0f), "#1F2638 parsed incorrectly")) return 1;
    if (!Check(!headless_config::ParseHexColor("1F2638", rgb), "missing # accepted")) return 1;
    if (!Check(!headless_config::ParseHexColor("#GG2638", rgb), "non-hex accepted")) return 1;

    // 3. Camera presets change orientation only, never framing.
    render::CameraState base;
    base.distance = 7.0f;
    base.targetY = 2.0f;
    const render::CameraState front = render::ApplyViewPreset(render::ViewPreset::Front, base);
    if (!Check(Approx(front.yawDeg, -90.0f) && Approx(front.pitchDeg, 0.0f) &&
               Approx(front.distance, 7.0f) && Approx(front.targetY, 2.0f),
               "Front preset changed distance/target")) return 1;
    if (!Check(render::ViewPresetFromName("iso") == render::ViewPreset::Iso &&
               render::ViewPresetFromName("TOP") == render::ViewPreset::Top &&
               render::ViewPresetFromName("unknown") == render::ViewPreset::Current,
               "preset name mapping is wrong")) return 1;

    QTemporaryDir temp;
    if (!Check(temp.isValid(), "temporary output directory unavailable")) return 1;
    RenderJob job;

    // 4. Schema 1 and 2 remain readable; unknown versions fail.
    QVariantMap v1 = BaseConfig(temp.path());
    v1.insert(QStringLiteral("schemaVersion"), 1);
    if (!Check(job.validateOptions(v1).value(QStringLiteral("valid")).toBool(),
               "schema v1 should remain valid")) return 1;
    if (!Check(job.validateOptions(BaseConfig(temp.path())).value(QStringLiteral("valid")).toBool(),
               "schema v2 should be valid")) return 1;
    QVariantMap v3 = BaseConfig(temp.path());
    v3.insert(QStringLiteral("schemaVersion"), 3);
    if (!Check(!job.validateOptions(v3).value(QStringLiteral("valid")).toBool(),
               "schema v3 must be rejected")) return 1;

    // 5. Empty is screenshot-only.
    if (!Check(!job.validateOptions(BaseConfig(temp.path(), QStringLiteral("empty")))
                    .value(QStringLiteral("valid")).toBool(),
               "Empty must be rejected by the headless renderer")) return 1;

    // 6. Camera numeric validation.
    QVariantMap badCamera = BaseConfig(temp.path());
    QVariantMap render = badCamera.value(QStringLiteral("render")).toMap();
    QVariantMap camera;
    camera.insert(QStringLiteral("yaw"), 0.0);
    camera.insert(QStringLiteral("pitch"), 0.0);
    camera.insert(QStringLiteral("distance"), 0.0);
    camera.insert(QStringLiteral("fov"), 42.0);
    camera.insert(QStringLiteral("target"), QVariantList{1.0, 0.55, 0.5});
    render.insert(QStringLiteral("camera"), camera);
    badCamera.insert(QStringLiteral("render"), render);
    if (!Check(!job.validateOptions(badCamera).value(QStringLiteral("valid")).toBool(),
               "zero camera distance must be rejected")) return 1;

    camera.insert(QStringLiteral("distance"), 3.526);
    camera.insert(QStringLiteral("fov"), 200.0);
    render.insert(QStringLiteral("camera"), camera);
    badCamera.insert(QStringLiteral("render"), render);
    if (!Check(!job.validateOptions(badCamera).value(QStringLiteral("valid")).toBool(),
               "out-of-range FOV must be rejected")) return 1;

    // 7. One drop event, positive radius, finite position.
    QVariantMap dropConfig = BaseConfig(temp.path());
    QVariantMap drop;
    drop.insert(QStringLiteral("type"), QStringLiteral("drop"));
    drop.insert(QStringLiteral("time"), 1.0);
    drop.insert(QStringLiteral("radius"), -0.2);
    dropConfig.insert(QStringLiteral("events"), QVariantList{drop});
    if (!Check(!job.validateOptions(dropConfig).value(QStringLiteral("valid")).toBool(),
               "negative drop radius must be rejected")) return 1;

    std::printf("camera_config: PASS\n");
    return 0;
}
