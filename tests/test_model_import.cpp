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

    for (size_t vertex = 0; vertex < model->vertexCount; ++vertex) {
        const float* data = &model->vertices[vertex * 6u];
        const float normalLengthSq =
            data[3] * data[3] + data[4] * data[4] + data[5] * data[5];
        if (!Check(std::isfinite(data[0]) && std::isfinite(data[1]) && std::isfinite(data[2]),
                   "position is not finite")) return 1;
        if (!Check(std::isfinite(normalLengthSq) && normalLengthSq > 0.9f,
                   "normal is missing or not normalized")) return 1;
        if (!Check(data[0] >= -0.61f && data[0] <= 0.61f &&
                   data[2] >= -0.61f && data[2] <= 0.61f &&
                   data[1] >= 0.049f && data[1] <= 0.851f,
                   "model was not normalized into the tank")) return 1;
    }

    const QString badPath = temp.filePath(QStringLiteral("empty.obj"));
    QFile bad(badPath);
    if (!Check(bad.open(QIODevice::WriteOnly | QIODevice::Text), "could not write empty OBJ")) return 1;
    bad.write("v 0 0 0\n");
    bad.close();
    error.clear();
    if (!Check(Model::Load(badPath, &error) == nullptr, "empty model should fail")) return 1;
    if (!Check(!error.isEmpty(), "empty model should provide an error message")) return 1;

    std::printf("model_import: PASS (%u meshes, %u vertices, %u triangles)\n",
                model->meshCount, model->vertexCount, model->triangleCount);
    return 0;
}
