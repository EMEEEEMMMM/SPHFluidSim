// Camera rig — orbit/pan/zoom/fly maths, with no GL and no Qt dependency, so
// both front-ends drive the same camera the same way.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "Math/Vector3.hpp"

namespace render {

struct CameraState {
    float yawDeg = -128.35f;
    float pitchDeg = -29.77f;
    float distance = 3.526f;
    float targetX = 1.0f;
    float targetY = 0.55f;
    float targetZ = 0.5f;
    float fovDeg = 42.0f;
};

void OrbitCamera(CameraState& state, float dxPixels, float dyPixels);
// Free-look: rotates the view around the current eye position instead of
// orbiting the target. Used by Fly mode for left-drag mouse look.
void LookCamera(CameraState& state, float dxPixels, float dyPixels);
void PanCamera(CameraState& state, float dxPixels, float dyPixels, int viewportHeight);
void ZoomCamera(CameraState& state, float wheelDelta);
void FlyCamera(CameraState& state, float forward, float right, float up, float deltaSeconds);
void ClampCamera(CameraState& state);

// ---------------------------------------------------------------------------
// v2 helpers. These are header-only on purpose: this group owns only the
// header, so no new definitions may be added to src/Render/CameraRig.cpp.
// ---------------------------------------------------------------------------

// Convention used everywhere: FrontVector(yaw,pitch) is the view direction
// from the eye towards the target; the eye sits at target - front * distance.
inline Math::Vector3 FrontVector(float yawDeg, float pitchDeg) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float ry = yawDeg * kDegToRad;
    const float rp = pitchDeg * kDegToRad;
    return Math::Vector3(std::cos(ry) * std::cos(rp), std::sin(rp),
                         std::sin(ry) * std::cos(rp));
}

// Standard view directions. A preset only changes the orbit orientation;
// target / distance / fov are carried over from `current` so switching a
// preset never teleports the framing.
enum class ViewPreset { Front, Back, Left, Right, Top, Iso, Current };

inline CameraState ApplyViewPreset(ViewPreset preset, const CameraState& current) {
    CameraState s = current;
    switch (preset) {
        case ViewPreset::Front:  s.yawDeg = -90.0f;   s.pitchDeg =  0.0f;  break;
        case ViewPreset::Back:   s.yawDeg =  90.0f;   s.pitchDeg =  0.0f;  break;
        case ViewPreset::Left:   s.yawDeg =   0.0f;   s.pitchDeg =  0.0f;  break;
        case ViewPreset::Right:  s.yawDeg = 180.0f;   s.pitchDeg =  0.0f;  break;
        case ViewPreset::Top:    s.yawDeg = -90.0f;   s.pitchDeg = -89.0f; break;
        case ViewPreset::Iso:    s.yawDeg = -136.7f;  s.pitchDeg = -18.0f; break;
        case ViewPreset::Current: break;
    }
    return s;
}

inline ViewPreset ViewPresetFromName(std::string name) {
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (name == "front")  return ViewPreset::Front;
    if (name == "back")   return ViewPreset::Back;
    if (name == "left")   return ViewPreset::Left;
    if (name == "right")  return ViewPreset::Right;
    if (name == "top")    return ViewPreset::Top;
    if (name == "iso")    return ViewPreset::Iso;
    return ViewPreset::Current;   // "Current" and unknown names: no change
}

// ---------------------------------------------------------------------------
// Gizmo geometry — pure math, shared by the OpenGL renderer (draw) and the
// QML front-end (screen-space picking). Keeping one generator guarantees the
// picked handles line up exactly with what is drawn.
//
// Handle encoding: 1..3 = translate X/Y/Z, 4..6 = rotate about X/Y/Z, 0 = none.
// ---------------------------------------------------------------------------
struct GizmoLine {
    Math::Vector3 a;
    Math::Vector3 b;
    int handle = 0;
};

// Gizmo size roughly constant on screen (scaled by distance to the eye).
inline float GizmoLength(float distanceToEye) { return 0.15f * distanceToEye; }
inline float GizmoRadius(float distanceToEye) { return 0.10f * distanceToEye; }

inline std::vector<GizmoLine> MakeGizmo(const Math::Vector3& origin,
                                        const Math::Vector3& eye) {
    const float dist = (origin - eye).length();
    const float L = GizmoLength(dist);
    const float R = GizmoRadius(dist);
    std::vector<GizmoLine> segs;
    const Math::Vector3 axes[3] = {{1.0f, 0.0f, 0.0f},
                                   {0.0f, 1.0f, 0.0f},
                                   {0.0f, 0.0f, 1.0f}};

    // Translation arrows: shaft + a small four-line arrow head.
    for (int a = 0; a < 3; ++a) {
        const int h = 1 + a;
        const Math::Vector3 tip = origin + axes[a] * L;
        const float head = L * 0.18f;
        const Math::Vector3 base = tip - axes[a] * head;
        const Math::Vector3 u = axes[(a + 1) % 3];
        const Math::Vector3 v = axes[(a + 2) % 3];
        segs.push_back({origin, tip, h});
        segs.push_back({tip, base + u * (head * 0.5f), h});
        segs.push_back({tip, base - u * (head * 0.5f), h});
        segs.push_back({tip, base + v * (head * 0.5f), h});
        segs.push_back({tip, base - v * (head * 0.5f), h});
    }

    // Rotation rings: handle 4 => YZ plane (about X), 5 => XZ (about Y),
    // 6 => XY (about Z).
    constexpr int kSegments = 32;
    constexpr float kPi = 3.14159265358979323846f;
    for (int a = 0; a < 3; ++a) {
        const int h = 4 + a;
        std::vector<Math::Vector3> ring(kSegments);
        for (int i = 0; i < kSegments; ++i) {
            const float t = 2.0f * kPi * static_cast<float>(i) /
                            static_cast<float>(kSegments);
            const float c = std::cos(t) * R;
            const float s = std::sin(t) * R;
            if (a == 0)      ring[i] = origin + Math::Vector3(0.0f, c, s);
            else if (a == 1) ring[i] = origin + Math::Vector3(c, 0.0f, s);
            else             ring[i] = origin + Math::Vector3(c, s, 0.0f);
        }
        for (int i = 0; i < kSegments; ++i) {
            segs.push_back({ring[i], ring[(i + 1) % kSegments], h});
        }
    }
    return segs;
}

}  // namespace render
