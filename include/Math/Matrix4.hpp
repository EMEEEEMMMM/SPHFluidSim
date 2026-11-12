#ifndef MATH_MATRIX4_H
#define MATH_MATRIX4_H

#include <algorithm>
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

    // Avoid `near` / `far` as parameter names: <windef.h> (pulled in by
    // windows.h) defines them as empty macros on MinGW, which corrupts the
    // declarations in any translation unit that includes both headers.
    static Matrix4 Ortho(float left, float right, float bottom, float top,
                         float zNear, float zFar) {
        Matrix4 matrix;
        matrix.m[0][0] = 2.0f / (right - left);
        matrix.m[1][1] = 2.0f / (top - bottom);
        matrix.m[2][2] = -2.0f / (zFar - zNear);
        matrix.m[3][0] = -(right + left) / (right - left);
        matrix.m[3][1] = -(top + bottom) / (top - bottom);
        matrix.m[3][2] = -(zFar + zNear) / (zFar - zNear);
        return matrix;
    }

    static Matrix4 Persp(float fovRad, float aspect, float zNear, float zFar) {
        Matrix4 matrix;
        std::memset(matrix.m, 0, sizeof(matrix.m));
        float tanHalfFov = std::tan(fovRad / 2.0f);
        matrix.m[0][0] = 1.0f / (aspect * tanHalfFov);
        matrix.m[1][1] = 1.0f / (tanHalfFov);
        matrix.m[2][2] = -(zFar + zNear) / (zFar - zNear);
        matrix.m[2][3] = -1.0f;
        matrix.m[3][2] = -(2.0f * zFar * zNear) / (zFar - zNear);
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

    // Column-major composition: (this * rhs).m[c][r] = sum_k this->m[k][r] * rhs.m[c][k].
    Matrix4 operator*(const Matrix4& rhs) const {
        Matrix4 result;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += m[k][r] * rhs.m[c][k];
                result.m[c][r] = sum;
            }
        }
        return result;
    }

    // General 4x4 inverse via Gauss-Jordan. Work in logical row-major
    // a[row][col] = m[col][row], then write back as result.m[col][row].
    // Singular matrices yield identity.
    Matrix4 Inverse() const {
        Matrix4 result;
        double a[4][8];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                a[r][c] = static_cast<double>(m[c][r]);
                a[r][4 + c] = (r == c) ? 1.0 : 0.0;
            }
        }
        for (int col = 0; col < 4; ++col) {
            int pivot = col;
            double best = std::abs(a[col][col]);
            for (int r = col + 1; r < 4; ++r) {
                const double v = std::abs(a[r][col]);
                if (v > best) { best = v; pivot = r; }
            }
            if (best < 1.0e-12) return Matrix4();  // singular
            if (pivot != col) {
                for (int k = 0; k < 8; ++k) std::swap(a[pivot][k], a[col][k]);
            }
            const double inv = 1.0 / a[col][col];
            for (int k = 0; k < 8; ++k) a[col][k] *= inv;
            for (int r = 0; r < 4; ++r) {
                if (r == col) continue;
                const double factor = a[r][col];
                if (factor == 0.0) continue;
                for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[col][k];
            }
        }
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                result.m[c][r] = static_cast<float>(a[r][4 + c]);
            }
        }
        return result;
    }

    // Affine point transform: result[row] = sum_col m[col][row] * p[col] + m[3][row].
    // (The bottom row is assumed (0,0,0,1); use MakeScreenRay for projective unprojection.)
    Vector3 TransformPoint(const Vector3& p) const {
        return Vector3(
            m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0],
            m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1],
            m[0][2] * p.x + m[1][2] * p.y + m[2][2] * p.z + m[3][2]);
    }

    // Direction transform: linear 3x3 part only, no translation.
    Vector3 TransformDirection(const Vector3& v) const {
        return Vector3(
            m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z);
    }

    // Column-major data, ready for glUniformMatrix4fv(..., GL_FALSE, data()).
    const float* data() const { return &m[0][0]; }
};

}  // namespace Math

#endif