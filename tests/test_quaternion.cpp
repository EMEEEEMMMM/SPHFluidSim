#include "Math/Matrix4.hpp"
#include "Math/Quaternions.hpp"
#include "Math/Vector3.hpp"

#include <cmath>
#include <cstdio>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

constexpr float kPi = 3.14159265358979323846f;
float Deg2Rad(float d) { return d * kPi / 180.0f; }

bool Approx(float a, float b, float eps = 1.0e-4f) {
    return std::fabs(a - b) <= eps;
}
bool ApproxVec(const Math::Vector3& a, const Math::Vector3& b, float eps = 1.0e-4f) {
    return Approx(a.x, b.x, eps) && Approx(a.y, b.y, eps) && Approx(a.z, b.z, eps);
}
bool ApproxMat(const Math::Matrix4& a, const Math::Matrix4& b, float eps = 1.0e-4f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!Approx(a.m[c][r], b.m[c][r], eps)) return false;
    return true;
}

}  // namespace

int main() {
    using Math::Matrix4;
    using Math::Quaternions;
    using Math::Vector3;

    // 1. Default construction is identity.
    Quaternions id;
    if (!Check(Approx(id.w, 1.0f) && Approx(id.x, 0.0f) &&
               Approx(id.y, 0.0f) && Approx(id.z, 0.0f),
               "default Quaternions is not identity")) return 1;
    if (!Check(ApproxVec(id * Vector3(1, 2, 3), Vector3(1, 2, 3)),
               "identity rotation changes a vector")) return 1;

    // 2. Axis-angle with a non-unit axis; vector rotation.
    Quaternions z90 = Quaternions::FromAxisAngle(Vector3(0, 0, 5), kPi / 2.0f);
    const float len = std::sqrt(z90.w * z90.w + z90.x * z90.x +
                                z90.y * z90.y + z90.z * z90.z);
    if (!Check(Approx(len, 1.0f), "axis-angle quaternion is not normalized")) return 1;
    if (!Check(ApproxVec(z90 * Vector3(1, 0, 0), Vector3(0, 1, 0)),
               "90 deg about +Z did not map (1,0,0) -> (0,1,0)")) return 1;

    Quaternions x90 = Quaternions::formAxisAngle(Vector3(2, 0, 0), kPi / 2.0f);
    if (!Check(ApproxVec(x90 * Vector3(0, 1, 0), Vector3(0, 0, 1)),
               "formAxisAngle 90 deg about +X did not map (0,1,0) -> (0,0,1)")) return 1;

    Quaternions z180 = Quaternions::FromAxisAngle(Vector3(0, 0, 1), kPi);
    if (!Check(ApproxVec(z180 * Vector3(1, 0, 0), Vector3(-1, 0, 0)),
               "180 deg rotation wrong")) return 1;

    // 3. q * conjugate(q) == identity; normalize()/normalized().
    Quaternions q = Quaternions::FromEulerDegrees(Vector3(23, -47, 61)).normalized();
    Quaternions prod = q * q.conjugate();
    if (!Check(Approx(prod.w, 1.0f) && Approx(prod.x, 0.0f) &&
               Approx(prod.y, 0.0f) && Approx(prod.z, 0.0f),
               "q * conjugate(q) is not identity")) return 1;

    Quaternions nonUnit(2, 0, 0, 0);
    Quaternions nu = nonUnit.normalized();
    if (!Check(Approx(nu.w, 1.0f) && Approx(nu.x, 0.0f),
               "normalized() did not normalize")) return 1;
    nonUnit.normalize();
    if (!Check(Approx(nonUnit.w, 1.0f), "normalize() did not normalize")) return 1;

    // 4. ToMatrix4 == Rz * Ry * Rx (column-major composition).
    const Vector3 angles[] = {
        {0, 0, 0}, {30, -20, 45}, {90, 0, 0}, {0, 0, 90},
        {10, 70, 20}, {-120, 30, -60}, {45, -45, 45},
        {0, 90, 0}, {30, 90, -40}, {0, -90, 15}, {80, 89.5f, 20}
    };
    for (const Vector3& e : angles) {
        const Quaternions qe = Quaternions::FromEulerDegrees(e);
        const Matrix4 rx = Matrix4::Rotate(Deg2Rad(e.x), Vector3(1, 0, 0));
        const Matrix4 ry = Matrix4::Rotate(Deg2Rad(e.y), Vector3(0, 1, 0));
        const Matrix4 rz = Matrix4::Rotate(Deg2Rad(e.z), Vector3(0, 0, 1));
        const Matrix4 composed = rz * ry * rx;
        if (!Check(ApproxMat(qe.ToMatrix4(), composed, 2.0e-3f),
                   "ToMatrix4 differs from Rz*Ry*Rx")) return 1;
    }

    // 5. Euler -> q -> Euler round trip (matrix check handles gimbal lock).
    for (const Vector3& e : angles) {
        const Quaternions qe = Quaternions::FromEulerDegrees(e);
        const Vector3 back = qe.ToEulerDegrees();
        if (!Check(ApproxMat(qe.ToMatrix4(),
                             Quaternions::FromEulerDegrees(back).ToMatrix4(), 2.0e-3f),
                   "Euler round trip changed the rotation")) return 1;
        if (std::fabs(e.y) < 88.0f) {
            if (!Check(ApproxVec(e, back, 1.0e-2f),
                       "Euler angles did not round trip")) return 1;
        }
    }

    std::printf("quaternion: PASS\n");
    return 0;
}
