#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Math/Matrix4.hpp"
#include "Math/Quaternions.hpp"
#include "Math/Ray.hpp"
#include "Math/Vector3.hpp"

class QString;

struct ModelData {
    // Interleaved position (3 floats) and normal (3 floats) per vertex.
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    // Optional CAD boundary line list (xyz pairs, 3 floats per endpoint).
    std::vector<float> edges;
    std::string sourcePath;
    uint32_t meshCount = 0;
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;

    // Original, un-normalized bounds. Vertices/size are kept exactly as
    // imported; no fit/center/scale is applied by Model::Load.
    Math::Vector3 boundsMin{0, 0, 0};
    Math::Vector3 boundsMax{0, 0, 0};
    Math::Vector3 boundsCenter{0, 0, 0};
    Math::Vector3 boundsExtent{0, 0, 0};

    // Lazy, thread-safe BVH cache. std::once_flag makes ModelData
    // non-copyable, which is intentional: it is always owned via ModelDataPtr
    // (shared_ptr) and never copied by value.
    mutable std::once_flag bvhOnce;
    mutable Math::BVH bvh;

    void EnsureBVH() const {
        std::call_once(bvhOnce, [this]() {
            if (!vertices.empty() && !indices.empty()) {
                bvh.Build(vertices.data(), indices.data(), indices.size());
            }
        });
    }

    bool RaycastLocal(const Math::Ray& localRay, float& tHit) const {
        const Math::AABB box{boundsMin, boundsMax};
        float tMin = 0.0f, tMax = 0.0f;
        if (!Math::RayAABB(localRay, box, tMin, tMax)) return false;
        EnsureBVH();
        return bvh.Intersect(localRay, tHit);
    }
};
using ModelDataPtr = std::shared_ptr<const ModelData>;

struct ModelInstance {
    int id = 0;
    std::string name;
    ModelDataPtr data;
    Math::Vector3 position{0, 0, 0};
    Math::Quaternions rotation;  // identity
    float scale = 1.0f;          // uniform, suggested 0.001..1000
    bool visible = true;

    // T(position) * R(rotation.ToMatrix4()) * S(scale) * T(-boundsCenter).
    Math::Matrix4 matrix() const {
        const Math::Vector3 center =
            data ? data->boundsCenter : Math::Vector3(0, 0, 0);
        return Math::Matrix4::Translate(position) * rotation.ToMatrix4() *
               Math::Matrix4::Scale(Math::Vector3(scale, scale, scale)) *
               Math::Matrix4::Translate(-center);
    }

    // Transform the world ray into local space with matrix().Inverse(), then
    // raycast locally. The local direction is renormalized, so tHit is the
    // parameter along the *world* ray: worldRay.origin + tHit*worldRay.direction
    // is the hit point (world distance when worldRay.direction is normalized).
    bool intersectWorld(const Math::Ray& worldRay, float& tHit) const {
        if (!data) return false;
        const Math::Matrix4 inv = matrix().Inverse();
        Math::Ray localRay;
        localRay.origin = inv.TransformPoint(worldRay.origin);
        const Math::Vector3 localDir = inv.TransformDirection(worldRay.direction);
        const float dirLen = localDir.length();
        if (dirLen <= 0.0f) return false;
        localRay.direction = localDir * (1.0f / dirLen);
        float localT = 0.0f;
        if (!data->RaycastLocal(localRay, localT)) return false;
        tHit = localT / dirLen;  // local distance -> world-ray parameter
        return true;
    }
};
using ModelInstancePtr = std::shared_ptr<const ModelInstance>;
using ModelSnapshot = std::vector<ModelInstancePtr>;
using ModelSnapshotPtr = std::shared_ptr<const ModelSnapshot>;

class Model {
public:
    // STEP/STP go through the built-in B-rep tessellator; every other format
    // is handed to Assimp. `warningMessage` receives non-fatal notes such as
    // STEP faces that had to be skipped.
    static ModelDataPtr Load(const QString& filePath, QString* errorMessage = nullptr,
                             QString* warningMessage = nullptr);
};
