#ifndef MATH_VECTOR3_H
#define MATH_VECTOR3_H

#include <cmath>

namespace Math {
    struct Vector3
    {
        float x, y, z;

        Vector3(float x = 0.0f, float y = 0.0f, float z = 0.0f) : x(x), y(y), z(z) {}

        inline Vector3 operator+(const Vector3& v) const { return {x + v.x, y + v.y, z + v.z}; }
        inline Vector3 operator+=(const Vector3& v) {
            this->x += v.x;
            this->y += v.y;
            this->z += v.z;
            return *this;
        }
        inline Vector3 operator-(const Vector3& v) const { return {x - v.x, y - v.y, z - v.z}; }
        inline Vector3 operator-=(const Vector3& v) {
            this->x -= v.x;
            this->y -= v.y;
            this->z -= v.z;
            return *this;
        }
        inline Vector3 operator*(float s) const { return {x * s, y * s, z * s}; }

        inline float dot(const Vector3& v) const { return x * v.x + y * v.y + z * v.z; }
        inline Vector3 cross(const Vector3& v) const {
            return { y * v.z - z * v.y, z * v.x - x * v.z, x * v.y - y * v.x };
        }

        inline float length() const { return std::sqrt(x * x + y * y + z * z); }

        inline Vector3 normalize() const {
            float len = length();
            return (len > 0.0f) ? *this * (1.0f / len) : Vector3(0, 0, 0);
        }
    };
}

#endif