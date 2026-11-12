// GlView — the only bridge between the Qt front-ends and the OpenGL renderer.
//
// WHY THIS FILE EXISTS
// --------------------
// The project renders with glad (function-pointer tables) while Qt's OpenGL
// headers (`qopengl.h` -> `GL/gl.h`) define the same entry points as real
// declarations. Including both in one translation unit fails to compile
// (verified: macro redefinition of GL_TIMEOUT_IGNORED and friends). So every
// glad-side detail lives behind this interface, and the Qt side only ever sees
// uint32_t / float / std::vector — never a GL type, never a GL header.
//
// The contract is: the caller makes a GL 3.3 core context current, hands over a
// proc-address loader, and from then on talks to the view in plain data.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Math/Matrix4.hpp"
#include "Math/Vector3.hpp"
#include "Model.hpp"
#include "Render/CameraRig.hpp"

namespace render {

class GlView {
public:
    using ProcLoader = void* (*)(const char* name);

    GlView();
    ~GlView();
    GlView(const GlView&) = delete;
    GlView& operator=(const GlView&) = delete;

    // Makes the renderer usable. A GL context must be current.
    // `shaderDir` may be empty: the built-in search path is tried instead.
    bool Initialize(ProcLoader loader, const std::string& shaderDir = std::string());
    void Shutdown();
    bool IsReady() const;

    // Size of the internal render target, in DEVICE pixels.
    void Resize(int width, int height);
    int  Width() const;
    int  Height() const;

    // Draws the scene into the internal target. Also uploads whatever was
    // queued by UpdateParticles / UpdateSurface since the last call.
    void Render();

    // Scales the internal target into `dstFbo` (0 = window back buffer),
    // letterboxing to preserve aspect. `letterR/G/B` are the fill colour of
    // the bars, in 0..1 sRGB. The signature and the three default parameters
    // are kept for existing callers; a negative component (the default) means
    // "use the colour stored by SetBackground()", which keeps the letterbox
    // bars the same colour as the 3D clear.
    void BlitTo(uint32_t dstFbo, int dstWidth, int dstHeight,
                float letterR = -1.0f, float letterG = -1.0f,
                float letterB = -1.0f);

    // Reads the internal target back as tightly packed RGBA8, top-down.
    bool ReadPixels(std::vector<uint8_t>& rgba, int& outWidth, int& outHeight);

    // ---- per-frame scene data -------------------------------------------
    void UpdateParticles(const std::vector<Math::Vector3>& positions,
                         const std::vector<float>& colors);
    void UpdateSurface(const std::vector<float>& verts,
                       const std::vector<unsigned>& indices);
    // Queues an imported visual model for upload during Render(). Null clears it.
    void UpdateModel(const ModelDataPtr& model);

    // ---- v2 multi-model feed --------------------------------------------
    // Queued and applied inside Render() on the render thread (they may create
    // or delete GL buffers). `selectedId` drives the outline + transform gizmo.
    void UpdateModels(const ModelSnapshotPtr& models, int selectedId);
    // scene encoding: 0 = dam, 1 = inflow, 2 = empty.
    void SetSceneIndex(int scene);

    // Background colour (0..1 sRGB) used for BOTH the 3D clear colour
    // (forwarded to OpenGLLayer::SetViewportBackground) and the letterbox bars.
    void SetBackground(const float rgb[3]);

    void SetSurfaceVisible(bool visible);
    void SetParticlesVisible(bool visible);
    void SetTank(float x, float y, float z, float cx, float cz, bool openTop);
    void SetParticleRadius(float radius);
    int  TriangleCount() const;

    // ---- camera ----------------------------------------------------------
    void Orbit(float dxDeg, float dyDeg);
    void Look(float dxDeg, float dyDeg);
    void Pan(float dxPixels, float dyPixels);
    void Zoom(float wheelDelta);
    void Fly(float forward, float right, float up, float deltaSeconds);
    void ResetCamera();
    CameraState& cameraState();

    // ---- CPU-side picking helpers ---------------------------------------
    // nx, ny are normalised [0,1] coordinates inside the item (top-left
    // origin). They use only the camera state (no GL calls), so they are safe
    // on the GUI thread even on a GlView that was never Initialize()d.
    // Set cameraState() (and Resize() for the aspect) before calling.
    int PickModel(double nx, double ny, const ModelSnapshotPtr& models) const;
    // Handle encoding: 1..3 translate XYZ, 4..6 rotate XYZ, 0 = none.
    int PickGizmo(double nx, double ny, const ModelInstance* selected) const;

    // CPU-side camera matrices / eye, computed from cameraState() alone.
    Math::Matrix4 ViewMatrix() const;
    Math::Matrix4 ProjectionMatrix() const;
    Math::Vector3 CameraEye() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace render
