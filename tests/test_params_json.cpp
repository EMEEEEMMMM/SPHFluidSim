#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>
#include <cstdio>

#include "Session/SimulationSession.hpp"

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool Near(double a, double b, double epsilon = 1.0e-6) {
    return std::fabs(a - b) <= epsilon;
}

bool WritePyramid(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream stream(&file);
    stream << "v 0 0 0\n"
           << "v 2 0 0\n"
           << "v 0 2 0\n"
           << "v 0 0 2\n"
           << "f 1 2 3\n"
           << "f 1 2 4\n"
           << "f 2 3 4\n"
           << "f 3 1 4\n";
    file.close();
    return true;
}

// Looks up the first models[] entry in an exported document.
QJsonObject FirstModel(const QJsonObject& root) {
    const QJsonObject simulation = root.value(QStringLiteral("simulation")).toObject();
    const QJsonArray models = simulation.value(QStringLiteral("models")).toArray();
    if (models.isEmpty()) return {};
    return models.at(0).toObject();
}

QVariantList Vec3(const QVariantMap& map, const char* key) {
    return map.value(QLatin1String(key)).toList();
}

bool NearList(const QVariantList& list, double a, double b, double c, double eps = 1.0e-5) {
    if (list.size() != 3) return false;
    return Near(list.at(0).toDouble(), a, eps) && Near(list.at(1).toDouble(), b, eps) &&
           Near(list.at(2).toDouble(), c, eps);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    QTemporaryDir temp;
    if (!Check(temp.isValid(), "could not create a temporary directory")) return 1;
    const QString objPath = temp.filePath(QStringLiteral("pyramid.obj"));
    if (!Check(WritePyramid(objPath), "could not write the test OBJ")) return 1;

    // --- v2 defaults: the application starts in the Empty scene -------------
    {
        SimulationSession session;
        if (!Check(session.sceneIndex() == 2, "default scene is not Empty (2)")) return 1;
        if (!Check(session.isEmpty(), "isEmpty is false in the default scene")) return 1;
        if (!Check(session.params().value(QStringLiteral("scene")).toInt() == 2,
                   "params.scene is not 2 by default")) return 1;
        if (!Check(session.objects().isEmpty(), "a fresh session already has objects")) return 1;
        if (!Check(session.selectedModelId() == -1, "a fresh session has a selection")) return 1;
        const QVariantMap camera = session.cameraState();
        if (!Check(Near(camera.value(QStringLiteral("fov")).toDouble(), 42.0),
                   "default camera fov is not 42")) return 1;
        if (!Check(Near(camera.value(QStringLiteral("targetY")).toDouble(), 0.55),
                   "default camera target is wrong")) return 1;
        const QVariantMap drop = session.dropParams();
        if (!Check(Near(drop.value(QStringLiteral("radius")).toDouble(), 0.2) &&
                       Near(drop.value(QStringLiteral("x")).toDouble(), 1.0) &&
                       Near(drop.value(QStringLiteral("y")).toDouble(), 1.6) &&
                       Near(drop.value(QStringLiteral("z")).toDouble(), 0.5),
                   "default drop parameters are wrong")) return 1;
    }

    // --- export is always schema 2 and carries models/camera/background ------
    SimulationSession original;
    original.setParam(QStringLiteral("scene"), 1);
    original.setParam(QStringLiteral("solver"), 1);
    original.setParam(QStringLiteral("spacing"), 0.03);
    original.setParam(QStringLiteral("speed"), 120);
    original.setParam(QStringLiteral("viewMode"), 2);
    original.setParam(QStringLiteral("omega"), 0.72);
    original.setParam(QStringLiteral("gridScale"), 0.75);
    original.setParam(QStringLiteral("surfaceThreshold"), 1.2);
    original.setParam(QStringLiteral("reconInterval"), 5);

    if (!Check(original.sceneIndex() == 1, "setParam(scene, 1) did not switch the scene")) return 1;
    if (!Check(!original.isEmpty(), "inflow scene reports as Empty")) return 1;

    const QString json = original.exportParamsJson();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (!Check(parseError.error == QJsonParseError::NoError && document.isObject(),
               "export is not valid JSON")) return 1;
    const QJsonObject root = document.object();
    if (!Check(root.value(QStringLiteral("schemaVersion")).toInt() == 2,
               "schemaVersion is not 2")) return 1;
    const QJsonObject simulation = root.value(QStringLiteral("simulation")).toObject();
    if (!Check(simulation.value(QStringLiteral("scene")).toString() == QStringLiteral("inflow"),
               "scene was not exported as a readable string")) return 1;
    if (!Check(simulation.value(QStringLiteral("solver")).toString() == QStringLiteral("DFSPH"),
               "solver was not exported as a readable string")) return 1;
    if (!Check(simulation.value(QStringLiteral("particleCount")).toInt() > 0,
               "particleCount missing")) return 1;
    if (!Check(simulation.value(QStringLiteral("viewMode")).toInt() == 2,
               "view mode was not preserved")) return 1;
    if (!Check(simulation.value(QStringLiteral("models")).isArray(),
               "simulation.models is not an array")) return 1;
    if (!Check(simulation.value(QStringLiteral("models")).toArray().isEmpty(),
               "a fluid scene exported models")) return 1;
    const QJsonObject render = root.value(QStringLiteral("render")).toObject();
    if (!Check(render.value(QStringLiteral("gridScale")).toDouble() == 0.75,
               "render gridScale missing")) return 1;
    const QJsonObject camera = render.value(QStringLiteral("camera")).toObject();
    if (!Check(camera.value(QStringLiteral("target")).toArray().size() == 3,
               "render.camera.target is not [x, y, z]")) return 1;
    if (!Check(Near(camera.value(QStringLiteral("fov")).toDouble(), 42.0),
               "render.camera.fov missing")) return 1;
    if (!Check(render.value(QStringLiteral("background")).toString() == QStringLiteral("#1F2638"),
               "render.background is not the default")) return 1;

    SimulationSession restored;
    if (!Check(restored.importParamsJson(json), "canonical JSON did not import")) return 1;
    const QVariantMap p = restored.params();
    if (!Check(p.value(QStringLiteral("scene")).toInt() == 1, "scene did not round-trip")) return 1;
    if (!Check(p.value(QStringLiteral("solver")).toInt() == 1, "solver did not round-trip")) return 1;
    if (!Check(Near(p.value(QStringLiteral("spacing")).toDouble(), 0.03),
               "spacing did not round-trip")) return 1;
    if (!Check(p.value(QStringLiteral("speed")).toInt() == 120, "speed did not round-trip")) return 1;
    if (!Check(p.value(QStringLiteral("viewMode")).toInt() == 2, "viewMode did not round-trip")) return 1;
    if (!Check(Near(p.value(QStringLiteral("omega")).toDouble(), 0.72),
               "omega did not round-trip")) return 1;
    if (!Check(p.value(QStringLiteral("reconInterval")).toInt() == 5,
               "reconInterval did not round-trip")) return 1;

    // A v1 document (legacy settings block) still imports.
    const QByteArray legacy = R"({"version":1,"settings":{"omega":0.42,"viewMode":0}})";
    if (!Check(restored.importParamsJson(QString::fromUtf8(legacy)),
               "legacy JSON did not import")) return 1;
    const QVariantMap legacyParams = restored.params();
    if (!Check(Near(legacyParams.value(QStringLiteral("omega")).toDouble(), 0.42),
               "legacy omega was ignored")) return 1;
    if (!Check(legacyParams.value(QStringLiteral("viewMode")).toInt() == 0,
               "legacy viewMode was ignored")) return 1;

    // schema v1 with a simulation block, but no models array.
    const QByteArray v1Doc = R"({
        "app":"IISPH","schemaVersion":1,
        "simulation":{"scene":"dam","solver":"IISPH","particleCount":4096},
        "render":{"mode":"surface"}
    })";
    if (!Check(restored.importParamsJson(QString::fromUtf8(v1Doc)),
               "schema v1 document did not import")) return 1;
    if (!Check(restored.sceneIndex() == 0, "v1 dam scene did not land on 0")) return 1;
    if (!Check(restored.objects().isEmpty(), "v1 import left objects behind")) return 1;

    // There is no application-level particle cap: large requested counts must
    // survive import and be left to the backend's memory preflight.
    const QByteArray largeRequest = R"({
        "app":"IISPH","schemaVersion":1,
        "simulation":{"scene":"dam","solver":"IISPH","particleCount":10000000}
    })";
    if (!Check(restored.importParamsJson(QString::fromUtf8(largeRequest)),
               "10M-particle JSON did not import")) return 1;
    if (!Check(restored.params().value(QStringLiteral("particleCount")).toInt() == 10000000,
               "particleCount was capped below 10000000")) return 1;

    // --- multi-model authoring in the Empty scene ---------------------------
    SimulationSession scene2;
    scene2.importModels(QStringList{objPath});
    if (!Check(scene2.objects().size() == 1, "model import in Empty failed")) return 1;
    if (!Check(scene2.selectedModelId() >= 0, "import did not select the new model")) return 1;

    const QVariantMap object = scene2.objects().at(0).toMap();
    const int modelId = object.value(QStringLiteral("id")).toInt();
    if (!Check(object.value(QStringLiteral("name")).toString() == QStringLiteral("pyramid"),
               "model name is not the file stem")) return 1;
    if (!Check(QFileInfo(object.value(QStringLiteral("path")).toString()).isAbsolute(),
               "object path is not absolute")) return 1;
    if (!Check(NearList(Vec3(object, "position"), 0, 0, 0), "initial position is not the origin")) return 1;
    if (!Check(object.value(QStringLiteral("rotation")).toList().size() == 4,
               "rotation is not [w, x, y, z]")) return 1;
    if (!Check(object.value(QStringLiteral("rotation")).toList().at(0).toDouble() == 1.0,
               "initial rotation is not identity")) return 1;
    if (!Check(Near(object.value(QStringLiteral("scale")).toDouble(), 1.0),
               "initial scale is not 1")) return 1;
    if (!Check(object.value(QStringLiteral("visible")).toBool(), "imported model is hidden")) return 1;
    if (!Check(object.value(QStringLiteral("triangleCount")).toInt() == 4,
               "triangleCount is wrong")) return 1;
    if (!Check(scene2.model() != nullptr, "legacy model() returned null for a visible model")) return 1;
    if (!Check(scene2.modelsForRender() != nullptr &&
                   scene2.modelsForRender()->size() == 1,
               "modelsForRender() snapshot is wrong")) return 1;

    // Duplicate import dedupes the display name.
    scene2.importModels(QStringList{objPath});
    if (!Check(scene2.objects().size() == 2, "second import did not append")) return 1;
    if (!Check(scene2.objects().at(1).toMap().value(QStringLiteral("name")).toString() ==
                   QStringLiteral("pyramid (2)"),
               "duplicate names were not deduped")) return 1;

    // Transforms: finite checks, Euler wrapping, scale clamp, quaternion norm.
    scene2.setModelPosition(modelId, 1.0, 2.0, 3.0);
    scene2.setModelRotationEuler(modelId, 270.0, -10.0, 540.0);
    scene2.setModelScale(modelId, 0.0001);
    scene2.setModelScale(modelId, 5000.0);
    QVariantMap edited = scene2.objects().at(0).toMap();
    if (!Check(NearList(Vec3(edited, "position"), 1, 2, 3), "position edit was ignored")) return 1;
    if (!Check(Near(edited.value(QStringLiteral("scale")).toDouble(), 1000.0),
               "scale was not clamped to 1000")) return 1;
    const QVariantList euler = Vec3(edited, "rotationEuler");
    if (!Check(euler.size() == 3 && euler.at(0).toDouble() >= -180.5 &&
                   euler.at(0).toDouble() <= 180.5,
               "Euler angles were not wrapped into [-180, 180]")) return 1;

    scene2.setModelRotationQuaternion(modelId, 2.0, 0.0, 0.0, 0.0);
    edited = scene2.objects().at(0).toMap();
    const QVariantList quat = edited.value(QStringLiteral("rotation")).toList();
    if (!Check(Near(quat.at(0).toDouble(), 1.0), "quaternion was not normalised")) return 1;

    scene2.setModelVisible(modelId, false);
    if (!Check(!scene2.objects().at(0).toMap().value(QStringLiteral("visible")).toBool(),
               "visibility edit was ignored")) return 1;
    if (!Check(scene2.model() != nullptr,
               "model() must fall back to the remaining visible model")) return 1;
    scene2.setModelVisible(modelId, true);

    scene2.resetModelTransform(modelId);
    edited = scene2.objects().at(0).toMap();
    if (!Check(NearList(Vec3(edited, "position"), 0, 0, 0), "reset did not clear the position")) return 1;
    if (!Check(Near(edited.value(QStringLiteral("scale")).toDouble(), 1.0),
               "reset did not clear the scale")) return 1;

    // Selection / removal.
    scene2.selectModel(scene2.objects().at(1).toMap().value(QStringLiteral("id")).toInt());
    if (!Check(scene2.selectedModelId() ==
                   scene2.objects().at(1).toMap().value(QStringLiteral("id")).toInt(),
               "selectModel failed")) return 1;
    scene2.removeModel(scene2.selectedModelId());
    if (!Check(scene2.objects().size() == 1, "removeModel failed")) return 1;
    if (!Check(scene2.selectedModelId() == -1, "removing the selection did not clear it")) return 1;

    // Camera config: ranges are clamped, target arrays are accepted.
    QVariantMap cameraConfig;
    cameraConfig.insert(QStringLiteral("yaw"), 200.0);
    cameraConfig.insert(QStringLiteral("pitch"), -120.0);
    cameraConfig.insert(QStringLiteral("distance"), 5.0);
    cameraConfig.insert(QStringLiteral("target"), QVariantList{2.0, 1.0, 0.0});
    cameraConfig.insert(QStringLiteral("fov"), 200.0);
    scene2.setCameraConfig(cameraConfig);
    QVariantMap liveCamera = scene2.cameraState();
    if (!Check(Near(liveCamera.value(QStringLiteral("pitch")).toDouble(), -89.0),
               "pitch was not clamped to -89")) return 1;
    if (!Check(Near(liveCamera.value(QStringLiteral("fov")).toDouble(), 75.0),
               "fov was not clamped to 75")) return 1;
    if (!Check(Near(liveCamera.value(QStringLiteral("targetX")).toDouble(), 2.0),
               "target array was ignored")) return 1;
    scene2.applyCameraPreset(QStringLiteral("Top"));
    liveCamera = scene2.cameraState();
    if (!Check(Near(liveCamera.value(QStringLiteral("pitch")).toDouble(), -89.0),
               "Top preset did not set the pitch")) return 1;

    // Background colour canonicalisation.
    scene2.setViewportBackgroundHex(QStringLiteral("#aabbcc"));
    if (!Check(scene2.params().value(QStringLiteral("viewportBackground")).toString() ==
                   QStringLiteral("#AABBCC"),
               "background colour was not canonicalised")) return 1;
    scene2.setViewportBackgroundHex(QStringLiteral("not-a-colour"));

    // Drop parameters.
    scene2.setDropParams(0.3, 1.1, 1.7, 0.4);
    const QVariantMap drop = scene2.dropParams();
    if (!Check(Near(drop.value(QStringLiteral("radius")).toDouble(), 0.3),
               "drop radius was not stored")) return 1;

    // --- schema v2 export of authored models --------------------------------
    const QString modelJson = scene2.exportParamsJson();
    const QJsonObject modelRoot =
        QJsonDocument::fromJson(modelJson.toUtf8(), &parseError).object();
    if (!Check(parseError.error == QJsonParseError::NoError, "model export is invalid")) return 1;
    const QJsonObject modelSim = modelRoot.value(QStringLiteral("simulation")).toObject();
    if (!Check(modelSim.value(QStringLiteral("scene")).toString() == QStringLiteral("empty"),
               "Empty scene was not exported as \"empty\"")) return 1;
    const QJsonObject modelEntry = FirstModel(modelRoot);
    if (!Check(!modelEntry.isEmpty(), "exported models[] is empty")) return 1;
    if (!Check(modelEntry.value(QStringLiteral("position")).toArray().size() == 3,
               "exported model position is not an array")) return 1;
    if (!Check(modelEntry.value(QStringLiteral("rotation")).toArray().size() == 4,
               "exported model rotation is not [w, x, y, z]")) return 1;
    if (!Check(modelEntry.value(QStringLiteral("visible")).isBool(),
               "exported model visibility is missing")) return 1;
    const QJsonObject modelCamera =
        modelRoot.value(QStringLiteral("render")).toObject()
            .value(QStringLiteral("camera")).toObject();
    if (!Check(Near(modelCamera.value(QStringLiteral("fov")).toDouble(), 75.0),
               "exported camera fov is wrong")) return 1;

    // --- round-trip of authored models (absolute paths) ---------------------
    SimulationSession roundTrip;
    if (!Check(roundTrip.importParamsJson(modelJson), "model JSON did not import")) return 1;
    if (!Check(roundTrip.sceneIndex() == 2, "v2 empty scene did not import as Empty")) return 1;
    if (!Check(roundTrip.objects().size() == 1, "models did not round-trip")) return 1;
    if (!Check(roundTrip.objects().at(0).toMap().value(QStringLiteral("name")).toString() ==
                   QStringLiteral("pyramid"),
               "round-tripped model name changed")) return 1;

    // --- relative model paths resolve against the config directory ----------
    const QString configPath = temp.filePath(QStringLiteral("scene.json"));
    if (!Check(scene2.exportParametersFile(configPath), "exportParametersFile failed")) return 1;
    {
        QFile file(configPath);
        if (!Check(file.open(QIODevice::ReadOnly | QIODevice::Text), "cannot read the config")) return 1;
        const QJsonObject exported =
            QJsonDocument::fromJson(file.readAll()).object();
        const QJsonObject entry = FirstModel(exported);
        if (!Check(entry.value(QStringLiteral("path")).toString() == QStringLiteral("pyramid.obj"),
                   "model path was not made relative to the config file")) return 1;
    }
    SimulationSession fromFile;
    if (!Check(fromFile.importParametersFile(configPath), "importParametersFile failed")) return 1;
    if (!Check(fromFile.objects().size() == 1, "relative model path did not resolve")) return 1;
    const QString resolved =
        fromFile.objects().at(0).toMap().value(QStringLiteral("path")).toString();
    if (!Check(QFileInfo(resolved).isAbsolute() &&
                   QFileInfo(resolved).fileName() == QStringLiteral("pyramid.obj"),
               "resolved model path is wrong")) return 1;

    // A model entry without a path is a fatal import error.
    const QByteArray missingPath = R"({
        "app":"IISPH","schemaVersion":2,
        "simulation":{"scene":"empty","models":[{"position":[0,0,0]}]}
    })";
    SimulationSession broken;
    if (!Check(!broken.importParamsJson(QString::fromUtf8(missingPath)),
               "a model without a path was accepted")) return 1;

    // A missing file is reported but does not abort the import.
    const QByteArray missingFile = R"({
        "app":"IISPH","schemaVersion":2,
        "simulation":{"scene":"empty","models":[{"path":"nope.obj"}]}
    })";
    if (!Check(broken.importParamsJson(QString::fromUtf8(missingFile)),
               "a missing model file aborted the whole import")) return 1;
    if (!Check(broken.objects().isEmpty(), "the missing model was added anyway")) return 1;

    // --- scene rules --------------------------------------------------------
    // Import in a fluid scene is rejected; switching scenes clears objects.
    SimulationSession scenes;
    scenes.importModels(QStringList{objPath});
    if (!Check(scenes.objects().size() == 1, "import in Empty failed")) return 1;
    scenes.setScene(0);
    if (!Check(scenes.sceneIndex() == 0 && !scenes.isEmpty(), "setScene(0) failed")) return 1;
    if (!Check(scenes.objects().isEmpty(), "a scene switch did not clear the models")) return 1;
    scenes.importModels(QStringList{objPath});
    if (!Check(scenes.objects().isEmpty(), "import in a fluid scene added a model")) return 1;

    // Scene defaults frame inflow differently from dam.
    scenes.setScene(1);
    if (!Check(Near(scenes.cameraState().value(QStringLiteral("targetY")).toDouble(), 0.5),
               "inflow scene did not reset the camera target")) return 1;

    // Drop events are exported for the dam scene only.
    scenes.setScene(0);
    scenes.setDropParams(0.25, 1.0, 1.5, 0.5);
    const QString damJson = scenes.exportParamsJson();
    const QJsonObject damRoot = QJsonDocument::fromJson(damJson.toUtf8()).object();
    const QJsonArray events = damRoot.value(QStringLiteral("events")).toArray();
    if (!Check(events.size() == 1, "dam export has no drop event")) return 1;
    if (!Check(Near(events.at(0).toObject().value(QStringLiteral("radius")).toDouble(), 0.25),
               "drop radius was not exported")) return 1;
    scenes.setScene(2);
    const QJsonObject emptyRoot =
        QJsonDocument::fromJson(scenes.exportParamsJson().toUtf8()).object();
    if (!Check(emptyRoot.value(QStringLiteral("events")).toArray().isEmpty(),
               "the Empty scene exported a drop event")) return 1;

    std::printf("params_json: PASS\n");
    return 0;
}
