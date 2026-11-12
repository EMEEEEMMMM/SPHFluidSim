#include "Model.hpp"

#include <QFile>
#include <QFileInfo>
#include <QString>

#include "StepImporter.hpp"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

ModelDataPtr Fail(const QString& message, QString* errorMessage) {
    if (errorMessage) *errorMessage = message;
    return nullptr;
}

bool Finite(const aiVector3D& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Derives the four bounds fields from the already-built vertex buffer. The
// geometry itself is never scaled, fitted or re-centered.
void ComputeBounds(ModelData& data) {
    float lower[3] = {std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
    float upper[3] = {-std::numeric_limits<float>::max(),
                      -std::numeric_limits<float>::max(),
                      -std::numeric_limits<float>::max()};
    for (size_t vertex = 0; vertex < data.vertexCount; ++vertex) {
        const float* position = &data.vertices[vertex * 6u];
        for (int axis = 0; axis < 3; ++axis) {
            lower[axis] = std::min(lower[axis], position[axis]);
            upper[axis] = std::max(upper[axis], position[axis]);
        }
    }
    data.boundsMin = Math::Vector3(lower[0], lower[1], lower[2]);
    data.boundsMax = Math::Vector3(upper[0], upper[1], upper[2]);
    data.boundsCenter = (data.boundsMin + data.boundsMax) * 0.5f;
    data.boundsExtent = data.boundsMax - data.boundsMin;
}

}  // namespace

ModelDataPtr Model::Load(const QString& filePath, QString* errorMessage, QString* warningMessage) {
    if (errorMessage) errorMessage->clear();
    if (warningMessage) warningMessage->clear();
    if (filePath.trimmed().isEmpty()) {
        return Fail(QStringLiteral("No model file was specified."), errorMessage);
    }

    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return Fail(QStringLiteral("Model file does not exist: %1").arg(filePath), errorMessage);
    }

    const QString extension = fileInfo.suffix().toLower();
    static const QStringList supported = {
        QStringLiteral("obj"), QStringLiteral("fbx"), QStringLiteral("stl"),
        QStringLiteral("dae"), QStringLiteral("3ds"), QStringLiteral("ply"),
        QStringLiteral("off"), QStringLiteral("gltf"), QStringLiteral("glb"),
        QStringLiteral("step"), QStringLiteral("stp")
    };
    if (!supported.contains(extension)) {
        return Fail(QStringLiteral("Unsupported model format '.%1'. Supported formats: obj, fbx, stl, dae, 3ds, ply, off, gltf, glb, step, stp.")
                        .arg(extension), errorMessage);
    }

    // STEP is a CAD exchange format, not a triangle mesh: Assimp cannot read
    // it, so it goes through the built-in B-rep tessellator instead.
    if (extension == QLatin1String("step") || extension == QLatin1String("stp")) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            return Fail(QStringLiteral("Could not open '%1': %2")
                            .arg(filePath, file.errorString()), errorMessage);
        }
        const QByteArray bytes = file.readAll();
        file.close();

        Step::Mesh mesh;
        std::string stepError;
        if (!Step::Tessellate(std::string(bytes.constData(), static_cast<size_t>(bytes.size())),
                              mesh, stepError)) {
            return Fail(QStringLiteral("Could not import '%1': %2")
                            .arg(filePath, QString::fromStdString(stepError)), errorMessage);
        }
        if (warningMessage) *warningMessage = QString::fromStdString(mesh.warning);

        auto data = std::make_shared<ModelData>();
        data->sourcePath = filePath.toUtf8().toStdString();
        const size_t vertexCount = mesh.positions.size() / 3u;
        if (mesh.normals.size() != mesh.positions.size() || vertexCount == 0 ||
            mesh.indices.empty()) {
            return Fail(QStringLiteral("The STEP importer produced an empty mesh."), errorMessage);
        }
        data->vertices.resize(vertexCount * 6u);
        for (size_t v = 0; v < vertexCount; ++v) {
            data->vertices[v * 6u + 0] = mesh.positions[v * 3u + 0];
            data->vertices[v * 6u + 1] = mesh.positions[v * 3u + 1];
            data->vertices[v * 6u + 2] = mesh.positions[v * 3u + 2];
            data->vertices[v * 6u + 3] = mesh.normals[v * 3u + 0];
            data->vertices[v * 6u + 4] = mesh.normals[v * 3u + 1];
            data->vertices[v * 6u + 5] = mesh.normals[v * 3u + 2];
        }
        data->indices = std::move(mesh.indices);
        data->edges = std::move(mesh.edges);
        data->vertexCount = static_cast<uint32_t>(vertexCount);
        data->triangleCount = static_cast<uint32_t>(data->indices.size() / 3u);
        data->meshCount = static_cast<uint32_t>(mesh.faces);
        ComputeBounds(*data);
        return data;
    }

    Assimp::Importer importer;
    const unsigned int flags = aiProcess_Triangulate |
                               aiProcess_PreTransformVertices |
                               aiProcess_GenSmoothNormals |
                               aiProcess_JoinIdenticalVertices;
    const aiScene* scene = importer.ReadFile(filePath.toUtf8().toStdString(), flags);
    if (!scene) {
        return Fail(QStringLiteral("Could not import '%1': %2")
                        .arg(filePath, QString::fromUtf8(importer.GetErrorString())), errorMessage);
    }
    if ((scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 || !scene->mRootNode) {
        return Fail(QStringLiteral("The imported model has an incomplete scene hierarchy."), errorMessage);
    }
    if (scene->mNumMeshes == 0 || !scene->mMeshes) {
        return Fail(QStringLiteral("The imported model contains no meshes."), errorMessage);
    }

    auto data = std::make_shared<ModelData>();
    data->sourcePath = filePath.toUtf8().toStdString();
    bool normalsValid = true;

    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (!mesh || !mesh->mVertices || !mesh->mFaces || mesh->mNumVertices == 0 || mesh->mNumFaces == 0) {
            return Fail(QStringLiteral("Mesh %1 is empty or has no vertex/face data.")
                            .arg(meshIndex), errorMessage);
        }
        if ((mesh->mPrimitiveTypes & ~(aiPrimitiveType_TRIANGLE)) != 0 ||
            mesh->mPrimitiveTypes == 0) {
            return Fail(QStringLiteral("Mesh %1 contains non-triangular geometry.")
                            .arg(meshIndex), errorMessage);
        }
        if (data->vertexCount > std::numeric_limits<uint32_t>::max() - mesh->mNumVertices) {
            return Fail(QStringLiteral("The model has too many vertices for 32-bit indices."), errorMessage);
        }

        const uint32_t baseVertex = data->vertexCount;
        data->vertices.reserve(data->vertices.size() + static_cast<size_t>(mesh->mNumVertices) * 6u);
        for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex) {
            const aiVector3D& position = mesh->mVertices[vertexIndex];
            if (!Finite(position)) {
                return Fail(QStringLiteral("Mesh %1 contains a non-finite vertex position.")
                                .arg(meshIndex), errorMessage);
            }
            data->vertices.push_back(position.x);
            data->vertices.push_back(position.y);
            data->vertices.push_back(position.z);

            if (mesh->HasNormals()) {
                const aiVector3D& normal = mesh->mNormals[vertexIndex];
                const float lengthSquared = normal.SquareLength();
                if (!Finite(normal) || !std::isfinite(lengthSquared) || lengthSquared < 1.0e-20f) {
                    normalsValid = false;
                    data->vertices.insert(data->vertices.end(), {0.0f, 0.0f, 0.0f});
                } else {
                    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
                    data->vertices.push_back(normal.x * inverseLength);
                    data->vertices.push_back(normal.y * inverseLength);
                    data->vertices.push_back(normal.z * inverseLength);
                }
            } else {
                normalsValid = false;
                data->vertices.insert(data->vertices.end(), {0.0f, 0.0f, 0.0f});
            }
        }
        data->vertexCount += mesh->mNumVertices;

        data->indices.reserve(data->indices.size() + static_cast<size_t>(mesh->mNumFaces) * 3u);
        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
            const aiFace& face = mesh->mFaces[faceIndex];
            if (face.mNumIndices != 3 || !face.mIndices) {
                return Fail(QStringLiteral("Mesh %1 contains a face that is not a triangle.")
                                .arg(meshIndex), errorMessage);
            }
            for (unsigned int corner = 0; corner < 3; ++corner) {
                const unsigned int index = face.mIndices[corner];
                if (index >= mesh->mNumVertices) {
                    return Fail(QStringLiteral("Mesh %1 contains an out-of-range vertex index.")
                                    .arg(meshIndex), errorMessage);
                }
                data->indices.push_back(baseVertex + index);
            }
        }
        ++data->meshCount;
    }

    data->triangleCount = static_cast<uint32_t>(data->indices.size() / 3u);
    if (data->vertexCount == 0 || data->triangleCount == 0) {
        return Fail(QStringLiteral("The imported model contains no triangle geometry."), errorMessage);
    }

    // Use valid imported/generated normals when possible. Otherwise derive
    // area-weighted vertex normals from the combined, index-adjusted mesh.
    if (!normalsValid) {
        for (size_t vertex = 0; vertex < data->vertexCount; ++vertex) {
            const size_t offset = vertex * 6u + 3u;
            std::fill_n(data->vertices.begin() + static_cast<std::ptrdiff_t>(offset), 3, 0.0f);
        }
        for (size_t triangle = 0; triangle < data->indices.size(); triangle += 3u) {
            const uint32_t ia = data->indices[triangle];
            const uint32_t ib = data->indices[triangle + 1u];
            const uint32_t ic = data->indices[triangle + 2u];
            const float* a = &data->vertices[static_cast<size_t>(ia) * 6u];
            const float* b = &data->vertices[static_cast<size_t>(ib) * 6u];
            const float* c = &data->vertices[static_cast<size_t>(ic) * 6u];
            const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            const float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float normal[3] = {
                ab[1] * ac[2] - ab[2] * ac[1],
                ab[2] * ac[0] - ab[0] * ac[2],
                ab[0] * ac[1] - ab[1] * ac[0]
            };
            const uint32_t corners[3] = {ia, ib, ic};
            for (uint32_t index : corners) {
                float* destination = &data->vertices[static_cast<size_t>(index) * 6u + 3u];
                destination[0] += normal[0];
                destination[1] += normal[1];
                destination[2] += normal[2];
            }
        }
        for (size_t vertex = 0; vertex < data->vertexCount; ++vertex) {
            float* normal = &data->vertices[vertex * 6u + 3u];
            const float lengthSquared = normal[0] * normal[0] + normal[1] * normal[1] +
                                        normal[2] * normal[2];
            if (std::isfinite(lengthSquared) && lengthSquared > 1.0e-20f) {
                const float inverseLength = 1.0f / std::sqrt(lengthSquared);
                normal[0] *= inverseLength;
                normal[1] *= inverseLength;
                normal[2] *= inverseLength;
            } else {
                normal[0] = 0.0f;
                normal[1] = 1.0f;
                normal[2] = 0.0f;
            }
        }
    }

    ComputeBounds(*data);

    return data;
}
