#ifndef MATH_QUATERNIONS_HPP
#define MATH_QUATERNIONS_HPP

#include <algorithm>
#include <cmath>

#include "Matrix4.hpp"
#include "Vector3.hpp"

namespace Math
{
    struct Quaternions
    {
        float w, x, y, z;
        Quaternions(float w = 1.0f, float x = 0.0f, float y = 0.0f, float z = 0.0f) : w(w), x(x), y(y), z(z) {}

        static Quaternions formAxisAngle(const Vector3& v, float theta) {
            const double length = std::sqrt(static_cast<double>(v.x) * v.x +
                                            static_cast<double>(v.y) * v.y +
                                            static_cast<double>(v.z) * v.z);
            if (!(length > 0.0) || !std::isfinite(length) || !std::isfinite(theta)) {
                return Quaternions();
            }

            const float halfTheta = theta * 0.5f;
            const double scale = std::sin(halfTheta) / length;
            return Quaternions(std::cos(halfTheta),
                               static_cast<float>(v.x * scale),
                               static_cast<float>(v.y * scale),
                               static_cast<float>(v.z * scale));
        }

        static Quaternions FromAxisAngle(const Vector3& axis, float angleRad) {
            return formAxisAngle(axis, angleRad);
        }

        // XYZ Euler angles in degrees, composed as R = Rz * Ry * Rx.
        static Quaternions FromEulerDegrees(const Vector3& euler) {
            const float radiansPerDegree = 3.14159265358979323846f / 180.0f;
            const float halfX = euler.x * radiansPerDegree * 0.5f;
            const float halfY = euler.y * radiansPerDegree * 0.5f;
            const float halfZ = euler.z * radiansPerDegree * 0.5f;
            const Quaternions qx(std::cos(halfX), std::sin(halfX), 0.0f, 0.0f);
            const Quaternions qy(std::cos(halfY), 0.0f, std::sin(halfY), 0.0f);
            const Quaternions qz(std::cos(halfZ), 0.0f, 0.0f, std::sin(halfZ));
            Quaternions result = qz * qy * qx;
            result.normalize();
            return result;
        }

        Vector3 ToEulerDegrees() const {
            Quaternions q = normalized();
            const float sinY = std::max(-1.0f, std::min(1.0f, 2.0f * (q.w * q.y - q.z * q.x)));
            const float y = std::asin(sinY);
            float x;
            float z;

            if (std::abs(sinY) < 1.0f - 1.0e-6f) {
                x = std::atan2(2.0f * (q.w * q.x + q.y * q.z),
                               1.0f - 2.0f * (q.x * q.x + q.y * q.y));
                z = std::atan2(2.0f * (q.w * q.z + q.x * q.y),
                               1.0f - 2.0f * (q.y * q.y + q.z * q.z));
            } else {
                x = std::atan2(2.0f * (q.w * q.x - q.y * q.z),
                               1.0f - 2.0f * (q.x * q.x + q.z * q.z));
                z = 0.0f;
            }

            const float degreesPerRadian = 180.0f / 3.14159265358979323846f;
            return Vector3(x * degreesPerRadian, y * degreesPerRadian, z * degreesPerRadian);
        }

        Matrix4 ToMatrix4() const {
            const Quaternions q = normalized();
            const float xx = q.x * q.x;
            const float yy = q.y * q.y;
            const float zz = q.z * q.z;
            const float xy = q.x * q.y;
            const float xz = q.x * q.z;
            const float yz = q.y * q.z;
            const float wx = q.w * q.x;
            const float wy = q.w * q.y;
            const float wz = q.w * q.z;

            Matrix4 matrix;
            matrix.m[0][0] = 1.0f - 2.0f * (yy + zz);
            matrix.m[0][1] = 2.0f * (xy + wz);
            matrix.m[0][2] = 2.0f * (xz - wy);
            matrix.m[1][0] = 2.0f * (xy - wz);
            matrix.m[1][1] = 1.0f - 2.0f * (xx + zz);
            matrix.m[1][2] = 2.0f * (yz + wx);
            matrix.m[2][0] = 2.0f * (xz + wy);
            matrix.m[2][1] = 2.0f * (yz - wx);
            matrix.m[2][2] = 1.0f - 2.0f * (xx + yy);
            return matrix;
        }

        Quaternions operator*(const Quaternions& p) const {
            return Quaternions(
                w * p.w - x * p.x - y * p.y - z * p.z,
                w * p.x + x * p.w + y * p.z - z * p.y,
                w * p.y - x * p.z + y * p.w + z * p.x,
                w * p.z + x * p.y - y * p.x + z * p.w
            );
        }

        Quaternions conjugate() const {
            return Quaternions(w, -x, -y, -z);
        }

        void normalize() {
            const double lengthSquared = static_cast<double>(w) * w +
                                         static_cast<double>(x) * x +
                                         static_cast<double>(y) * y +
                                         static_cast<double>(z) * z;
            if (!(lengthSquared > 0.0) || !std::isfinite(lengthSquared)) {
                *this = Quaternions();
                return;
            }
            const double inverseLength = 1.0 / std::sqrt(lengthSquared);
            w = static_cast<float>(w * inverseLength);
            x = static_cast<float>(x * inverseLength);
            y = static_cast<float>(y * inverseLength);
            z = static_cast<float>(z * inverseLength);
        }

        Quaternions normalized() const {
            Quaternions result = *this;
            result.normalize();
            return result;
        }

        inline Vector3 operator*(const Vector3& v) const {
            Vector3 qv(x, y, z);
            Vector3 t = qv.cross(v) * 2.0f;
            return v + t * w + qv.cross(t);
        }
    };
}


#endif
