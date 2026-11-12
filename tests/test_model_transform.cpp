#include "Model.hpp"

#include <cmath>
#include <cstdio>
#include <memory>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

constexpr float kPi = 3.14159265358979323846f;

bool Approx(float a, float b, float eps = 1.0e-3f) {
    return std::fabs(a - b) <= eps;
}
bool ApproxVec(const Math::Vector3& a, const Math::Vector3& b, float eps = 1.0e-3f) {
    return Approx(a.x, b.x, eps) && Approx(a.y, b.y, eps) && Approx(a.z, b.z, eps);
}
bool ApproxMat(const Math::Matrix4& a, const Math::Matrix4& b, float eps = 1.0e-4f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!Approx(a.m[c][r], b.m[c][r], eps)) return false;
    return true;
}

// Axis-aligned cube [1,3]^3 in local space, interleaved pos3+normal3.
std::shared_ptr<ModelData> MakeCube() {
    auto data = std::make_shared<ModelData>();
    const float corners[8][3] = {
        {1, 1, 1}, {3, 1, 1}, {3, 3, 1}, {1, 3, 1},
        {1, 1, 3}, {3, 1, 3}, {3, 3, 3}, {1, 3, 3}
    };
    for (int i = 0; i < 8; ++i) {
        data->vertices.push_back(corners[i][0]);
        data->vertices.push_back(corners[i][1]);
        data->vertices.push_back(corners[i][2]);
        data->vertices.push_back(0.0f);  // normal (unused by the BVH)
        data->vertices.push_back(0.0f);
        data->vertices.push_back(1.0f);
    }
    const uint32_t faces[12][3] = {
        {0, 3, 2}, {0, 2, 1},  // z = 1 (front)
        {4, 5, 6}, {4, 6, 7},  // z = 3 (back)
        {0, 4, 7}, {0, 7, 3},  // x = 1
        {1, 2, 6}, {1, 6, 5},  // x = 3
        {0, 1, 5}, {0, 5, 4},  // y = 1
        {3, 7, 6}, {3, 6, 2}   // y = 3
    };
    for (auto& f : faces)
        for (int k = 0; k < 3; ++k) data->indices.push_back(f[k]);
    data->vertexCount = 8;
    data->triangleCount = 12;
    data->meshCount = 1;
    data->boundsMin = Math::Vector3(1, 1, 1);
    data->boundsMax = Math::Vector3(3, 3, 3);
    data->boundsCenter = Math::Vector3(2, 2, 2);
    data->boundsExtent = Math::Vector3(2, 2, 2);
    return data;
}

}  // namespace

int main() {
    auto cube = MakeCube();

    // 1. matrix() == T(position) * R * S * T(-boundsCenter).
    ModelInstance inst;
    inst.data = cube;
    inst.position = Math::Vector3(10, 20, 30);
    inst.rotation = Math::Quaternions::FromAxisAngle(Math::Vector3(0, 1, 0), kPi / 2.0f);
    inst.scale = 2.0f;
    const Math::Matrix4 expected =
        Math::Matrix4::Translate(inst.position) * inst.rotation.ToMatrix4() *
        Math::Matrix4::Scale(Math::Vector3(inst.scale, inst.scale, inst.scale)) *
        Math::Matrix4::Translate(-cube->boundsCenter);
    if (!Check(ApproxMat(inst.matrix(), expected),
               "matrix() differs from T*R*S*T(-center)")) return 1;
    if (!Check(ApproxVec(inst.matrix().TransformPoint(cube->boundsCenter), inst.position),
               "matrix() does not map boundsCenter to position")) return 1;

    // 2. intersectWorld on a known cube (identity transform -> world == local).
    ModelInstance base;
    base.data = cube;
    base.position = cube->boundsCenter;  // (2,2,2) => matrix() == identity
    base.rotation = Math::Quaternions();
    base.scale = 1.0f;

    Math::Ray hit;
    hit.origin = Math::Vector3(2.3f, 1.7f, -5.0f);
    hit.direction = Math::Vector3(0, 0, 1);
    float tHit = 0.0f;
    if (!Check(base.intersectWorld(hit, tHit), "center ray should hit the cube")) return 1;
    if (!Check(Approx(tHit, 6.0f), "unscaled hit distance should be 6")) return 1;
    if (!Check(ApproxVec(hit.origin + hit.direction * tHit,
                         Math::Vector3(2.3f, 1.7f, 1.0f)),
               "hit point is not on the front face")) return 1;

    Math::Ray miss;
    miss.origin = Math::Vector3(10, 10, -5);
    miss.direction = Math::Vector3(0, 0, 1);
    float tMiss = 0.0f;
    if (!Check(!base.intersectWorld(miss, tMiss), "off-axis ray should miss the cube")) return 1;

    // 3. Scale: cube becomes [0,4]^3, so the same ray hits at t=5.
    base.scale = 2.0f;
    float tScaled = 0.0f;
    if (!Check(base.intersectWorld(hit, tScaled) && Approx(tScaled, 5.0f),
               "scaled cube should be hit at t=5")) return 1;

    // 4. Rotation: a 45 deg Z rotation turns the cube into a diamond whose
    //    corner reaches x~3.414; the off-center ray at x=3.4 now hits.
    base.scale = 1.0f;
    Math::Ray offCenter;
    offCenter.origin = Math::Vector3(3.4f, 2.0f, -5.0f);
    offCenter.direction = Math::Vector3(0, 0, 1);
    float tOff = 0.0f;
    if (!Check(!base.intersectWorld(offCenter, tOff),
               "unrotated cube should be missed by off-center ray")) return 1;
    base.rotation = Math::Quaternions::FromAxisAngle(Math::Vector3(0, 0, 1), kPi / 4.0f);
    if (!Check(base.intersectWorld(offCenter, tOff) && Approx(tOff, 6.0f, 1.0e-2f),
               "45 deg Z rotation should make the off-center ray hit at t=6")) return 1;

    // 5. Ray primitives / MakeScreenRay / BVH smoke checks.
    Math::Ray rt;
    rt.origin = Math::Vector3(0, 0, -1);
    rt.direction = Math::Vector3(0, 0, 1);
    float t = 0.0f;
    if (!Check(Math::RayTriangle(rt, Math::Vector3(0, 0, 0), Math::Vector3(1, 0, 0),
                                 Math::Vector3(0, 1, 0), t) && Approx(t, 1.0f),
               "RayTriangle front hit")) return 1;
    Math::Ray rtBack;
    rtBack.origin = Math::Vector3(0, 0, 1);
    rtBack.direction = Math::Vector3(0, 0, -1);
    if (!Check(Math::RayTriangle(rtBack, Math::Vector3(0, 0, 0), Math::Vector3(1, 0, 0),
                                  Math::Vector3(0, 1, 0), t) && Approx(t, 1.0f),
               "RayTriangle back (double sided) hit")) return 1;

    Math::AABB box{Math::Vector3(-1, -1, -1), Math::Vector3(1, 1, 1)};
    Math::Ray ra;
    ra.origin = Math::Vector3(0, 0, -5);
    ra.direction = Math::Vector3(0, 0, 1);
    float tMin = 0.0f, tMax = 0.0f;
    if (!Check(Math::RayAABB(ra, box, tMin, tMax) &&
               Approx(tMin, 4.0f) && Approx(tMax, 6.0f),
               "RayAABB slab interval")) return 1;

    const Math::Matrix4 view = Math::Matrix4::LookAt(
        Math::Vector3(0, 0, 5), Math::Vector3(0, 0, 0), Math::Vector3(0, 1, 0));
    const Math::Matrix4 proj = Math::Matrix4::Persp(1.5f, 1.0f, 0.1f, 100.0f);
    const Math::Ray screen = Math::MakeScreenRay(0.0f, 0.0f, view, proj,
                                                 Math::Vector3(0, 0, 5));
    if (!Check(ApproxVec(screen.direction, Math::Vector3(0, 0, -1), 1.0e-3f),
               "MakeScreenRay center direction should be (0,0,-1)")) return 1;
    if (!Check(ApproxVec(screen.origin, Math::Vector3(0, 0, 5)),
               "MakeScreenRay origin should be the eye")) return 1;

    // Direct BVH build from interleaved data (stride 6) + uint32 indices.
    Math::BVH bvh;
    bvh.Build(cube->vertices.data(), cube->indices.data(), cube->indices.size());
    Math::Ray bvhRay;
    bvhRay.origin = Math::Vector3(2.3f, 1.7f, -5.0f);
    bvhRay.direction = Math::Vector3(0, 0, 1);
    float tBvh = 0.0f;
    if (!Check(bvh.Intersect(bvhRay, tBvh) && Approx(tBvh, 6.0f),
               "BVH.Intersect on the cube")) return 1;

    std::printf("model_transform: PASS\n");
    return 0;
}
