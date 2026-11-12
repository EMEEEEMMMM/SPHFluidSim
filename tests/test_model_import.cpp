#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>
#include <cstdio>

#include "Model.hpp"

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool Near(float a, float b, float eps = 1.0e-5f) {
    return std::fabs(a - b) < eps;
}

// Signed volume of a closed triangle mesh (divergence theorem). A positive
// value means the triangles are wound outwards.
double MeshVolume(const ModelDataPtr& model) {
    double sum = 0.0;
    for (size_t i = 0; i + 2 < model->indices.size(); i += 3) {
        const float* a = &model->vertices[static_cast<size_t>(model->indices[i]) * 6u];
        const float* b = &model->vertices[static_cast<size_t>(model->indices[i + 1]) * 6u];
        const float* c = &model->vertices[static_cast<size_t>(model->indices[i + 2]) * 6u];
        sum += static_cast<double>(a[0]) * (static_cast<double>(b[1]) * c[2] -
                                            static_cast<double>(b[2]) * c[1]) -
               static_cast<double>(a[1]) * (static_cast<double>(b[0]) * c[2] -
                                            static_cast<double>(b[2]) * c[0]) +
               static_cast<double>(a[2]) * (static_cast<double>(b[0]) * c[1] -
                                            static_cast<double>(b[1]) * c[0]);
    }
    return sum / 6.0;
}

bool CheckStep(const QString& path, double expectedVolume, const char* label) {
    QString error;
    QString warning;
    const ModelDataPtr step = Model::Load(path, &error, &warning);
    if (!Check(step != nullptr, error.toUtf8().constData())) {
        std::fprintf(stderr, "  (while importing %s)\n", label);
        return false;
    }
    if (!Check(step->triangleCount > 0, "STEP import produced no triangles")) return false;
    if (!Check(step->vertices.size() == static_cast<size_t>(step->vertexCount) * 6u,
               "STEP vertex layout is not pos+normal")) return false;

    bool finite = true;
    bool normalized = true;
    for (size_t v = 0; v < step->vertexCount; ++v) {
        const float* data = &step->vertices[v * 6u];
        finite = finite && std::isfinite(data[0]) && std::isfinite(data[1]) &&
                 std::isfinite(data[2]) && std::isfinite(data[3]) &&
                 std::isfinite(data[4]) && std::isfinite(data[5]);
        const float lengthSq = data[3] * data[3] + data[4] * data[4] + data[5] * data[5];
        normalized = normalized && lengthSq > 0.9f && lengthSq < 1.1f;
    }
    if (!Check(finite, "STEP import produced non-finite vertices")) return false;
    if (!Check(normalized, "STEP import produced non-unit normals")) return false;

    const double volume = MeshVolume(step);
    const double relative = std::fabs(volume - expectedVolume) / expectedVolume;
    if (!Check(relative < 0.01, "STEP volume deviates from the analytic value")) {
        std::fprintf(stderr, "  %s: got %.9g, expected %.9g (relative %.4f)\n",
                     label, volume, expectedVolume, relative);
        return false;
    }
    std::printf("  %-9s %2d faces, %4u triangles, volume %.6g m^3%s\n", label,
                static_cast<int>(step->meshCount), step->triangleCount, volume,
                warning.isEmpty() ? "" : " (warnings)");
    return true;
}

}  // namespace

int main() {
    QTemporaryDir temp;
    if (!Check(temp.isValid(), "could not create temporary directory")) return 1;

    const QString objPath = temp.filePath(QStringLiteral("pyramid.obj"));
    QFile obj(objPath);
    if (!Check(obj.open(QIODevice::WriteOnly | QIODevice::Text), "could not write test OBJ")) return 1;
    QTextStream stream(&obj);
    stream << "v 0 0 0\n"
           << "v 2 0 0\n"
           << "v 0 2 0\n"
           << "v 0 0 2\n"
           << "f 1 2 3\n"
           << "f 1 2 4\n"
           << "f 2 3 4\n"
           << "f 3 1 4\n";
    obj.close();

    QString error;
    const ModelDataPtr model = Model::Load(objPath, &error);
    if (!Check(model != nullptr, error.toUtf8().constData())) return 1;
    if (!Check(model->indices.size() == 12, "triangulated index count is wrong")) return 1;
    if (!Check(model->vertices.size() == model->vertexCount * 6u, "vertex layout is not pos+normal")) return 1;
    if (!Check(model->triangleCount == 4, "triangle count is wrong")) return 1;

    // Geometry must be preserved raw: the pyramid spans [0,2] on every axis,
    // centred at (1,1,1), with an extent of (2,2,2). No tank normalization.
    if (!Check(Near(model->boundsMin.x, 0.0f) && Near(model->boundsMin.y, 0.0f) &&
               Near(model->boundsMin.z, 0.0f), "boundsMin is not the raw OBJ minimum")) return 1;
    if (!Check(Near(model->boundsMax.x, 2.0f) && Near(model->boundsMax.y, 2.0f) &&
               Near(model->boundsMax.z, 2.0f), "boundsMax is not the raw OBJ maximum")) return 1;
    if (!Check(Near(model->boundsCenter.x, 1.0f) && Near(model->boundsCenter.y, 1.0f) &&
               Near(model->boundsCenter.z, 1.0f), "boundsCenter is wrong")) return 1;
    if (!Check(Near(model->boundsExtent.x, 2.0f) && Near(model->boundsExtent.y, 2.0f) &&
               Near(model->boundsExtent.z, 2.0f), "boundsExtent is not the raw size")) return 1;

    for (size_t vertex = 0; vertex < model->vertexCount; ++vertex) {
        const float* data = &model->vertices[vertex * 6u];
        const float normalLengthSq =
            data[3] * data[3] + data[4] * data[4] + data[5] * data[5];
        if (!Check(std::isfinite(data[0]) && std::isfinite(data[1]) && std::isfinite(data[2]),
                   "position is not finite")) return 1;
        if (!Check(std::isfinite(normalLengthSq) && normalLengthSq > 0.9f,
                   "normal is missing or not normalized")) return 1;
        // Raw, unscaled/untranslated positions: each component is 0 or 2.
        if (!Check((Near(data[0], 0.0f) || Near(data[0], 2.0f)) &&
                   (Near(data[1], 0.0f) || Near(data[1], 2.0f)) &&
                   (Near(data[2], 0.0f) || Near(data[2], 2.0f)),
                   "vertex position was scaled or translated (expected raw 0/2 coordinates)")) return 1;
    }

    const QString badPath = temp.filePath(QStringLiteral("empty.obj"));
    QFile bad(badPath);
    if (!Check(bad.open(QIODevice::WriteOnly | QIODevice::Text), "could not write empty OBJ")) return 1;
    bad.write("v 0 0 0\n");
    bad.close();
    error.clear();
    if (!Check(Model::Load(badPath, &error) == nullptr, "empty model should fail")) return 1;
    if (!Check(!error.isEmpty(), "empty model should provide an error message")) return 1;

    // STEP: analytic B-rep fixtures (10 mm box, r=5 h=10 cylinder, 20 mm plate
    // with a 4 mm hole). Volumes are the analytic values in cubic metres after
    // the millimetre-to-metre unit conversion.
    if (!CheckStep(QStringLiteral(TEST_DATA_DIR "/box.step"), 1.0e-6, "box")) return 1;
    if (!CheckStep(QStringLiteral(TEST_DATA_DIR "/cylinder.step"),
                  3.14159265358979323846 * 0.005 * 0.005 * 0.01, "cylinder")) return 1;
    if (!CheckStep(QStringLiteral(TEST_DATA_DIR "/plate.step"),
                  (0.02 * 0.02 - 3.14159265358979323846 * 0.004 * 0.004) * 0.002,
                  "plate")) return 1;
    // 5 x 4 through-holes: 2 planar faces + 4 sides + 20 hole walls.
    if (!CheckStep(QStringLiteral(TEST_DATA_DIR "/multi_hole_plate.step"),
                  (0.1 * 0.06 - 20.0 * 3.14159265358979323846 * 0.003 * 0.003) * 0.006,
                  "holes20")) return 1;

    std::printf("model_import: PASS (%u meshes, %u vertices, %u triangles)\n",
                model->meshCount, model->vertexCount, model->triangleCount);
    return 0;
}
