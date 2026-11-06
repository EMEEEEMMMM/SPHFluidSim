#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class QString;

struct ModelData {
    // Interleaved position (3 floats) and normal (3 floats) per vertex.
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    std::string sourcePath;
    uint32_t meshCount = 0;
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
};
using ModelDataPtr = std::shared_ptr<const ModelData>;

class Model {
public:
    static ModelDataPtr Load(const QString& filePath, QString* errorMessage = nullptr);
};
