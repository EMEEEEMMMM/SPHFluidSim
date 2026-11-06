#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

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

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

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

    const QString json = original.exportParamsJson();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (!Check(parseError.error == QJsonParseError::NoError && document.isObject(),
               "export is not valid JSON")) return 1;
    const QJsonObject root = document.object();
    if (!Check(root.value(QStringLiteral("schemaVersion")).toInt() == 1,
               "schemaVersion missing")) return 1;
    const QJsonObject simulation = root.value(QStringLiteral("simulation")).toObject();
    if (!Check(simulation.value(QStringLiteral("scene")).toString() == QStringLiteral("inflow"),
               "scene was not exported as a readable string")) return 1;
    if (!Check(simulation.value(QStringLiteral("solver")).toString() == QStringLiteral("DFSPH"),
               "solver was not exported as a readable string")) return 1;
    if (!Check(simulation.value(QStringLiteral("particleCount")).toInt() > 0,
               "particleCount missing")) return 1;
    if (!Check(simulation.value(QStringLiteral("viewMode")).toInt() == 2,
               "view mode was not preserved")) return 1;
    const QJsonObject render = root.value(QStringLiteral("render")).toObject();
    if (!Check(render.value(QStringLiteral("gridScale")).toDouble() == 0.75,
               "render gridScale missing")) return 1;

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

    const QByteArray legacy = R"({"version":1,"settings":{"omega":0.42,"viewMode":0}})";
    if (!Check(restored.importParamsJson(QString::fromUtf8(legacy)),
               "legacy JSON did not import")) return 1;
    const QVariantMap legacyParams = restored.params();
    if (!Check(Near(legacyParams.value(QStringLiteral("omega")).toDouble(), 0.42),
               "legacy omega was ignored")) return 1;
    if (!Check(legacyParams.value(QStringLiteral("viewMode")).toInt() == 0,
               "legacy viewMode was ignored")) return 1;

    std::printf("params_json: PASS\n");
    return 0;
}
