#ifndef MATH_RAY_HPP
#define MATH_RAY_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "Matrix4.hpp"
#include "Vector3.hpp"

namespace Math {

struct Ray {
    Vector3 origin;
    Vector3 direction;  // normalized
};

struct AABB {
    Vector3 min;
    Vector3 max;
};

// Slab test. On success tMin/tMax bound the intersection interval along
// ray.direction; tMin may be negative when the origin is inside the box.
// Returns false when the box is entirely behind the ray or missed.
inline bool RayAABB(const Ray& ray, const AABB& box, float& tMin, float& tMax) {
    float t0 = -std::numeric_limits<float>::max();
    float t1 = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        const float o = ray.origin[i];
        const float d = ray.direction[i];
        const float lo = box.min[i];
        const float hi = box.max[i];
        if (std::abs(d) < 1.0e-12f) {
            if (o < lo || o > hi) return false;
        } else {
            const float inv = 1.0f / d;
            float ta = (lo - o) * inv;
            float tb = (hi - o) * inv;
            if (ta > tb) std::swap(ta, tb);
            if (ta > t0) t0 = ta;
            if (tb < t1) t1 = tb;
            if (t0 > t1) return false;
        }
    }
    tMin = t0;
    tMax = t1;
    return t1 >= 0.0f;
}

// Moller-Trumbore, double sided. `t` is the distance along ray.direction
// (assumed normalized) to the hit point; false when there is no forward hit.
inline bool RayTriangle(const Ray& ray, const Vector3& a, const Vector3& b,
                        const Vector3& c, float& t) {
    const Vector3 e1 = b - a;
    const Vector3 e2 = c - a;
    const Vector3 p = ray.direction.cross(e2);
    const float det = e1.dot(p);
    if (std::abs(det) < 1.0e-12f) return false;
    const float invDet = 1.0f / det;  // keep sign -> double sided
    const Vector3 tv = ray.origin - a;
    const float u = tv.dot(p) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    const Vector3 q = tv.cross(e1);
    const float v = ray.direction.dot(q) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float tt = e2.dot(q) * invDet;
    if (tt < 0.0f) return false;
    t = tt;
    return true;
}

// Unproject NDC x/y at z=-1 and z=+1 with Inverse(proj*view) and shoot the ray
// through the camera eye. view/proj are the existing column-major matrices.
inline Ray MakeScreenRay(float ndcX, float ndcY,
                         const Matrix4& view, const Matrix4& proj, const Vector3& eye) {
    const Matrix4 invVP = (proj * view).Inverse();
    const auto unproject = [&](float ndcZ, Vector3& out) {
        // Homogeneous multiply for column-major m[col][row]:
        // out[row] = sum_col m[col][row] * v[col].
        const float hx = invVP.m[0][0] * ndcX + invVP.m[1][0] * ndcY +
                         invVP.m[2][0] * ndcZ + invVP.m[3][0];
        const float hy = invVP.m[0][1] * ndcX + invVP.m[1][1] * ndcY +
                         invVP.m[2][1] * ndcZ + invVP.m[3][1];
        const float hz = invVP.m[0][2] * ndcX + invVP.m[1][2] * ndcY +
                         invVP.m[2][2] * ndcZ + invVP.m[3][2];
        const float hw = invVP.m[0][3] * ndcX + invVP.m[1][3] * ndcY +
                         invVP.m[2][3] * ndcZ + invVP.m[3][3];
        if (std::abs(hw) > 1.0e-12f)
            out = Vector3(hx / hw, hy / hw, hz / hw);
        else
            out = Vector3(hx, hy, hz);
    };
    Vector3 nearPoint, farPoint;
    unproject(-1.0f, nearPoint);
    unproject(1.0f, farPoint);
    Ray ray;
    ray.origin = eye;
    ray.direction = (farPoint - nearPoint).normalize();
    return ray;
}

// Bounding-volume hierarchy over indexed triangles. The source geometry is
// interleaved pos3+normal3 (stride 6 floats) with uint32 indices. Small ranges
// become leaves scanned linearly; large ranges use median (centroid) splits.
struct BVH {
    struct Node {
        AABB box;
        int left = -1;       // child node index; -1 => leaf
        int right = -1;
        uint32_t begin = 0;  // range [begin,end) into leafTriangles (leaf only)
        uint32_t end = 0;
        bool isLeaf() const { return left < 0; }
    };

    std::vector<Node> nodes;
    std::vector<float> positions;        // compact pos3 per vertex
    std::vector<uint32_t> triangles;     // 3 vertex indices per triangle
    std::vector<uint32_t> leafTriangles; // triangle ids referenced by leaves

    static constexpr size_t kLeafTriangles = 8;

    void Build(const float* interleavedPos3Normal3, const uint32_t* indices,
               size_t indexCount) {
        nodes.clear();
        positions.clear();
        triangles.clear();
        leafTriangles.clear();
        if (!interleavedPos3Normal3 || !indices || indexCount < 3) return;

        const size_t triCount = indexCount / 3;
        triangles.assign(indices, indices + indexCount);

        uint32_t maxIndex = 0;
        for (size_t i = 0; i < indexCount; ++i)
            if (indices[i] > maxIndex) maxIndex = indices[i];
        positions.assign((static_cast<size_t>(maxIndex) + 1u) * 3u, 0.0f);
        for (uint32_t v = 0; v <= maxIndex; ++v) {
            const float* src = interleavedPos3Normal3 + static_cast<size_t>(v) * 6u;
            const size_t o = static_cast<size_t>(v) * 3u;
            positions[o] = src[0];
            positions[o + 1u] = src[1];
            positions[o + 2u] = src[2];
        }

        std::vector<uint32_t> order(triCount);
        for (uint32_t i = 0; i < triCount; ++i) order[i] = i;
        nodes.reserve(2u * triCount + 1u);
        leafTriangles.reserve(triCount);
        BuildNode(order.begin(), order.end());
    }

    bool Intersect(const Ray& ray, float& tHit) const {
        if (nodes.empty()) return false;
        tHit = std::numeric_limits<float>::max();
        bool hit = false;
        std::vector<int> stack;
        stack.reserve(nodes.size());
        stack.push_back(0);
        while (!stack.empty()) {
            const int id = stack.back();
            stack.pop_back();
            const Node& node = nodes[id];
            float tMin = 0.0f, tMax = 0.0f;
            if (!RayAABB(ray, node.box, tMin, tMax)) continue;
            if (tMin > tHit) continue;  // node is farther than the nearest hit
            if (node.isLeaf()) {
                for (uint32_t i = node.begin; i < node.end; ++i) {
                    const TriangleVerts tri = GetTriangle(leafTriangles[i]);
                    float t = 0.0f;
                    if (RayTriangle(ray, tri.a, tri.b, tri.c, t) &&
                        t > 1.0e-6f && t < tHit) {
                        tHit = t;
                        hit = true;
                    }
                }
            } else {
                stack.push_back(node.left);
                stack.push_back(node.right);
            }
        }
        return hit;
    }

private:
    struct TriangleVerts {
        Vector3 a, b, c;
    };

    Vector3 Vertex(uint32_t index) const {
        const size_t o = static_cast<size_t>(index) * 3u;
        return Vector3(positions[o], positions[o + 1u], positions[o + 2u]);
    }

    TriangleVerts GetTriangle(uint32_t triId) const {
        const size_t o = static_cast<size_t>(triId) * 3u;
        return {Vertex(triangles[o]), Vertex(triangles[o + 1u]),
                Vertex(triangles[o + 2u])};
    }

    Vector3 Centroid(uint32_t triId) const {
        const TriangleVerts t = GetTriangle(triId);
        return (t.a + t.b + t.c) * (1.0f / 3.0f);
    }

    int BuildNode(std::vector<uint32_t>::iterator begin,
                  std::vector<uint32_t>::iterator end) {
        const size_t count = static_cast<size_t>(end - begin);
        const int nodeId = static_cast<int>(nodes.size());
        nodes.push_back(Node{});

        AABB box, cbox;
        bool first = true;
        for (auto it = begin; it != end; ++it) {
            const TriangleVerts t = GetTriangle(*it);
            const Vector3 centroid = (t.a + t.b + t.c) * (1.0f / 3.0f);
            const Vector3 triMin(std::min(t.a.x, std::min(t.b.x, t.c.x)),
                                 std::min(t.a.y, std::min(t.b.y, t.c.y)),
                                 std::min(t.a.z, std::min(t.b.z, t.c.z)));
            const Vector3 triMax(std::max(t.a.x, std::max(t.b.x, t.c.x)),
                                 std::max(t.a.y, std::max(t.b.y, t.c.y)),
                                 std::max(t.a.z, std::max(t.b.z, t.c.z)));
            if (first) {
                box.min = triMin;
                box.max = triMax;
                cbox.min = centroid;
                cbox.max = centroid;
                first = false;
            } else {
                box.min.x = std::min(box.min.x, triMin.x);
                box.min.y = std::min(box.min.y, triMin.y);
                box.min.z = std::min(box.min.z, triMin.z);
                box.max.x = std::max(box.max.x, triMax.x);
                box.max.y = std::max(box.max.y, triMax.y);
                box.max.z = std::max(box.max.z, triMax.z);
                cbox.min.x = std::min(cbox.min.x, centroid.x);
                cbox.min.y = std::min(cbox.min.y, centroid.y);
                cbox.min.z = std::min(cbox.min.z, centroid.z);
                cbox.max.x = std::max(cbox.max.x, centroid.x);
                cbox.max.y = std::max(cbox.max.y, centroid.y);
                cbox.max.z = std::max(cbox.max.z, centroid.z);
            }
        }
        nodes[nodeId].box = box;

        if (count <= kLeafTriangles) {
            nodes[nodeId].begin = static_cast<uint32_t>(leafTriangles.size());
            for (auto it = begin; it != end; ++it) leafTriangles.push_back(*it);
            nodes[nodeId].end = static_cast<uint32_t>(leafTriangles.size());
            return nodeId;
        }

        const float ex = cbox.max.x - cbox.min.x;
        const float ey = cbox.max.y - cbox.min.y;
        const float ez = cbox.max.z - cbox.min.z;
        const int axis = (ex >= ey && ex >= ez) ? 0 : ((ey >= ez) ? 1 : 2);
        std::sort(begin, end, [this, axis](uint32_t a, uint32_t b) {
            return Centroid(a)[axis] < Centroid(b)[axis];
        });
        auto mid = begin + static_cast<std::ptrdiff_t>(count / 2);
        const int left = BuildNode(begin, mid);
        const int right = BuildNode(mid, end);
        nodes[nodeId].left = left;
        nodes[nodeId].right = right;
        return nodeId;
    }
};

}  // namespace Math

#endif  // MATH_RAY_HPP
