#ifndef MATH_CONSTANTS_HPP
#define MATH_CONSTANTS_HPP

#include <cmath>

namespace Math {
    constexpr float GRAVITY = 9.80f;
    inline float radians(float degrees) { return degrees * (M_PI / 180.0f); }
    inline float degrees(float radians) { return radians * (180.0f / M_PI); }
}

#endif