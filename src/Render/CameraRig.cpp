#include "Render/CameraRig.hpp"

#include <algorithm>
#include <cmath>

#include "Math/Constants.hpp"
#include "Math/Vector3.hpp"

namespace render {
namespace {

constexpr float kOrbitDegPerPixel = 0.32f;
constexpr float kMinDistance = 0.05f;
constexpr float kMaxDistance = 26.0f;
constexpr float kMaxPitch = 89.0f;
constexpr float kFlySpeed = 2.2f;
constexpr float kMinFov = 20.0f;      // matches the Field-of-view control
constexpr float kMaxFov = 75.0f;
constexpr float kZoomDegPerNotch = 1.5f;

Math::Vector3 Front(float yawDeg, float pitchDeg) {
    const float ry = Math::radians(yawDeg);
    const float rp = Math::radians(pitchDeg);
    return Math::Vector3(std::cos(ry) * std::cos(rp), std::sin(rp), std::sin(ry) * std::cos(rp));
}

}  // namespace

void ClampCamera(CameraState& s) {
    s.distance = std::clamp(s.distance, kMinDistance, kMaxDistance);
    s.pitchDeg = std::clamp(s.pitchDeg, -kMaxPitch, kMaxPitch);
    s.fovDeg = std::clamp(s.fovDeg, kMinFov, kMaxFov);
}

void OrbitCamera(CameraState& s, float dxPixels, float dyPixels) {
    s.yawDeg += dxPixels * kOrbitDegPerPixel;
    // Desktop orbit controls conventionally tilt opposite to the drag delta;
    // the previous sign made vertical orbiting feel inverted.
    s.pitchDeg -= dyPixels * kOrbitDegPerPixel;
    ClampCamera(s);
}

void LookCamera(CameraState& s, float dxPixels, float dyPixels) {
    const Math::Vector3 target(s.targetX, s.targetY, s.targetZ);
    const Math::Vector3 eye = target - Front(s.yawDeg, s.pitchDeg) * s.distance;

    s.yawDeg += dxPixels * kOrbitDegPerPixel;
    s.pitchDeg -= dyPixels * kOrbitDegPerPixel;
    ClampCamera(s);

    const Math::Vector3 next = eye + Front(s.yawDeg, s.pitchDeg) * s.distance;
    s.targetX = next.x;
    s.targetY = next.y;
    s.targetZ = next.z;
}

void PanCamera(CameraState& s, float dxPixels, float dyPixels, int viewportHeight) {
    const int denom = std::max(1, viewportHeight);
    const float worldPerPixel =
        2.0f * s.distance * std::tan(Math::radians(s.fovDeg * 0.5f)) / static_cast<float>(denom);
    const Math::Vector3 front = Front(s.yawDeg, s.pitchDeg);
    const Math::Vector3 right = front.cross(Math::Vector3(0.0f, 1.0f, 0.0f)).normalize();
    const Math::Vector3 up = right.cross(front).normalize();
    const Math::Vector3 delta =
        right * (-dxPixels * worldPerPixel) + up * (dyPixels * worldPerPixel);
    s.targetX += delta.x;
    s.targetY += delta.y;
    s.targetZ += delta.z;
}

void ZoomCamera(CameraState& s, float wheelDelta) {
    // The wheel drives the field of view, the way the pre-Qt GLFW viewer did
    // (Camera::ProcessMouseScroll changed Zoom). It stops dead at the ends of
    // the range the Field-of-view control shows, so the number on screen and
    // the wheel can never disagree.
    s.fovDeg = std::clamp(s.fovDeg - wheelDelta * kZoomDegPerNotch, kMinFov, kMaxFov);
}

void FlyCamera(CameraState& s, float forward, float right, float up, float deltaSeconds) {
    const float speed = kFlySpeed * std::clamp(deltaSeconds, 0.0f, 0.1f);
    const Math::Vector3 front = Front(s.yawDeg, s.pitchDeg);
    const Math::Vector3 rightV = front.cross(Math::Vector3(0.0f, 1.0f, 0.0f)).normalize();
    const Math::Vector3 upV = rightV.cross(front).normalize();
    const Math::Vector3 delta =
        front * (forward * speed) + rightV * (right * speed) + upV * (up * speed);
    s.targetX += delta.x;
    s.targetY += delta.y;
    s.targetZ += delta.z;
}

}  // namespace render
