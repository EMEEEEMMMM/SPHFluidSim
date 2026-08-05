#ifndef MATH_MATRIX4_H
#define MATH_MATRIX4_H

#include <cmath>
#include <cstring>

#include "Vector3.hpp"

namespace Math {
struct Matrix4 {
    float m[4][4];

    Matrix4() { setIdentity(1.0f); }
    Matrix4(float v) { setIdentity(v); }
    void setIdentity(float v) {
        std::memset(m, 0, sizeof(m));
        m[0][0] = m[1][1] = m[2][2] = m[3][3] = v;
    }

    static Matrix4 Ortho(float left, float right, float bottom, float top,
                         float near, float far) {
        Matrix4 matrix;
        matrix.m[0][0] = 2.0f / (right - left);
        matrix.m[1][1] = 2.0f / (top - bottom);
        matrix.m[2][2] = -2.0f / (far - near);
        matrix.m[3][0] = -(right + left) / (right - left);
        matrix.m[3][1] = -(top + bottom) / (top - bottom);
        matrix.m[3][2] = -(far + near) / (far - near);
        return matrix;
    }

    static Matrix4 Persp(float fovRad, float aspect, float near, float far) {
        Matrix4 matrix;
        std::memset(matrix.m, 0, sizeof(matrix.m));
        float tanHalfFov = std::tan(fovRad / 2.0f);
        matrix.m[0][0] = 1.0f / (aspect * tanHalfFov);
        matrix.m[1][1] = 1.0f / (tanHalfFov);
        matrix.m[2][2] = -(far + near) / (far - near);
        matrix.m[2][3] = -1.0f;
        matrix.m[3][2] = -(2.0f * far * near) / (far - near);
        return matrix;
    }

    static Matrix4 Translate(const Vector3& v) {
        Matrix4 matrix;
        matrix.m[3][0] = v.x;
        matrix.m[3][1] = v.y;
        matrix.m[3][2] = v.z;
        return matrix;
    }

    static Matrix4 Scale(const Vector3& v) {
        Matrix4 matrix;
        matrix.m[0][0] = v.x;
        matrix.m[1][1] = v.y;
        matrix.m[2][2] = v.z;
        return matrix;
    }

    static Matrix4 Rotate(float angleRad, Vector3 axis) {
        Matrix4 matrix;
        float c = cos(angleRad);
        float s = sin(angleRad);
        axis = axis.normalize();
        Vector3 t = axis * (1.0f - c);

        matrix.m[0][0] = c + t.x * axis.x;
        matrix.m[0][1] = t.x * axis.y + s * axis.z;
        matrix.m[0][2] = t.x * axis.z - s * axis.y;

        matrix.m[1][0] = t.y * axis.x - s * axis.z;
        matrix.m[1][1] = c + t.y * axis.y;
        matrix.m[1][2] = t.y * axis.z + s * axis.x;

        matrix.m[2][0] = t.z * axis.x + s * axis.y;
        matrix.m[2][1] = t.z * axis.y - s * axis.x;
        matrix.m[2][2] = c + t.z * axis.z;

        return matrix;
    }

    static Matrix4 LookAt(const Vector3& eye, const Vector3& target, const Vector3& EyeUp) {
        Vector3 CameraDirection = (target - eye).normalize();
        Vector3 CameraRight = CameraDirection.cross(EyeUp).normalize();
        Vector3 CameraUp = CameraRight.cross(CameraDirection);

        Matrix4 ViewMatrix(1.0f);
        ViewMatrix.m[0][0] = CameraRight.x;
        ViewMatrix.m[1][0] = CameraRight.y;
        ViewMatrix.m[2][0] = CameraRight.z;

        ViewMatrix.m[0][1] = CameraUp.x;
        ViewMatrix.m[1][1] = CameraUp.y;
        ViewMatrix.m[2][1] = CameraUp.z;

        ViewMatrix.m[0][2] = -CameraDirection.x;
        ViewMatrix.m[1][2] = -CameraDirection.y;
        ViewMatrix.m[2][2] = -CameraDirection.z;

        ViewMatrix.m[3][0] = -CameraRight.dot(eye);
        ViewMatrix.m[3][1] = -CameraUp.dot(eye);
        ViewMatrix.m[3][2] = CameraDirection.dot(eye);
        ViewMatrix.m[3][3] = 1.0f;

        return ViewMatrix;
    }
};

}  // namespace Math

#endif