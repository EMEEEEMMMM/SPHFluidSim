// Camera rig — orbit/pan/zoom/fly maths, with no GL and no Qt dependency, so
// both front-ends drive the same camera the same way.
#pragma once

namespace render {

struct CameraState {
    float yawDeg = -136.7f;
    float pitchDeg = -18.0f;
    float distance = 3.35f;
    float targetX = 1.0f;
    float targetY = 0.55f;
    float targetZ = 0.5f;
    float fovDeg = 45.0f;
};

void OrbitCamera(CameraState& state, float dxPixels, float dyPixels);
// Free-look: rotates the view around the current eye position instead of
// orbiting the target. Used by Fly mode for left-drag mouse look.
void LookCamera(CameraState& state, float dxPixels, float dyPixels);
void PanCamera(CameraState& state, float dxPixels, float dyPixels, int viewportHeight);
void ZoomCamera(CameraState& state, float wheelDelta);
void FlyCamera(CameraState& state, float forward, float right, float up, float deltaSeconds);
void ClampCamera(CameraState& state);

}  // namespace render
