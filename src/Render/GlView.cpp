#include "Render/GlView.hpp"

#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Camera.hpp"
#include "FrameBuffer.hpp"
#include "Math/Constants.hpp"
#include "Math/Vector3.hpp"
#include "OpenGLLayer.hpp"

namespace render {
namespace {

Math::Vector3 FrontVector(float yawDeg, float pitchDeg) {
    const float ry = Math::radians(yawDeg);
    const float rp = Math::radians(pitchDeg);
    return Math::Vector3(std::cos(ry) * std::cos(rp), std::sin(rp), std::sin(ry) * std::cos(rp));
}

}  // namespace

struct GlView::Impl {
    OpenGLLayer* layer = nullptr;
    FrameBuffer* fb = nullptr;
    CameraState cam;
    int width = 0;
    int height = 0;
    bool ready = false;

    // Queued between Render() calls so the GL upload happens inside the
    // context that owns the objects, never from a UI callback.
    bool particlesDirty = false;
    std::vector<Math::Vector3> positions;
    std::vector<float> colors;
    bool surfaceDirty = false;
    std::vector<float> verts;
    std::vector<unsigned> indices;
    bool modelDirty = false;
    ModelDataPtr model;

    void PushCamera() {
        if (!layer) return;
        Camera& camera = layer->GetCamera();
        const Math::Vector3 front = FrontVector(cam.yawDeg, cam.pitchDeg);
        const Math::Vector3 target(cam.targetX, cam.targetY, cam.targetZ);
        camera.Position = target - front * cam.distance;
        camera.Yaw = cam.yawDeg;
        camera.Pitch = cam.pitchDeg;
        camera.Zoom = cam.fovDeg;
        camera.ProcessMouseMovement(0.0f, 0.0f);   // refresh Front/Right/Up
    }

    void ClampCamera() { render::ClampCamera(cam); }
};

GlView::GlView() : m_impl(new Impl) {}

GlView::~GlView() { Shutdown(); }

bool GlView::Initialize(ProcLoader loader, const std::string& shaderDir) {
    if (!loader) return false;
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(loader))) return false;
    if (m_impl->ready) return true;

    if (const GLubyte* version = glGetString(GL_VERSION)) {
        const GLubyte* renderer = glGetString(GL_RENDERER);
        std::fprintf(stderr, "[gl] %s | %s\n", reinterpret_cast<const char*>(version),
                     renderer ? reinterpret_cast<const char*>(renderer) : "?");
    }

    const int w = m_impl->width > 0 ? m_impl->width : 64;
    const int h = m_impl->height > 0 ? m_impl->height : 64;
    m_impl->layer = new OpenGLLayer(static_cast<uint32_t>(w), static_cast<uint32_t>(h), shaderDir);
    m_impl->fb = m_impl->layer->GetFrameBuffer();
    m_impl->ready = true;
    m_impl->PushCamera();
    glDisable(GL_FRAMEBUFFER_SRGB);
    return true;
}

void GlView::Shutdown() {
    if (m_impl->layer) {
        delete m_impl->layer;
        m_impl->layer = nullptr;
        m_impl->fb = nullptr;
    }
    m_impl->ready = false;
}

bool GlView::IsReady() const { return m_impl->ready; }

void GlView::Resize(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == m_impl->width && height == m_impl->height) return;
    m_impl->width = width;
    m_impl->height = height;
    if (m_impl->layer) {
        m_impl->layer->ResizeFrameBuffer(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
}

int GlView::Width() const { return m_impl->width; }
int GlView::Height() const { return m_impl->height; }

void GlView::Render() {
    if (!m_impl->ready) return;
    Impl& d = *m_impl;

    if (d.particlesDirty) {
        d.layer->UpdateParticles(d.positions, d.colors);
        d.particlesDirty = false;
    }
    if (d.surfaceDirty) {
        d.layer->UpdateSurfaceMesh(d.verts, d.indices);
        d.surfaceDirty = false;
    }
    if (d.modelDirty) {
        d.layer->UpdateModel(d.model);
        d.modelDirty = false;
    }
    d.PushCamera();
    d.layer->Render();
}

void GlView::BlitTo(uint32_t dstFbo, int dstWidth, int dstHeight,
                    float letterR, float letterG, float letterB) {
    if (!m_impl->ready || dstWidth <= 0 || dstHeight <= 0) return;

    const int sw = m_impl->width;
    const int sh = m_impl->height;
    if (sw <= 0 || sh <= 0) return;

    // Bind the source as READ (and momentarily as DRAW), then repoint DRAW at
    // the destination: GL_FRAMEBUFFER is just the read+draw alias, so this
    // leaves READ on our FBO and DRAW on the caller's target.
    m_impl->fb->Bind();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);

    glViewport(0, 0, dstWidth, dstHeight);
    glClearColor(letterR, letterG, letterB, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const float srcAspect = static_cast<float>(sw) / static_cast<float>(sh);
    const float dstAspect = static_cast<float>(dstWidth) / static_cast<float>(dstHeight);
    int w = dstWidth;
    int h = dstHeight;
    int x = 0;
    int y = 0;
    if (srcAspect > dstAspect) {
        h = static_cast<int>(static_cast<float>(dstWidth) / srcAspect);
        y = (dstHeight - h) / 2;
    } else {
        w = static_cast<int>(static_cast<float>(dstHeight) * srcAspect);
        x = (dstWidth - w) / 2;
    }

    glBlitFramebuffer(0, 0, sw, sh, x, y, x + w, y + h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // Leave the caller's target bound: QOpenGLWidget and QQuickFramebufferObject
    // both expect their own FBO to still be current when we return.
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
}

bool GlView::ReadPixels(std::vector<uint8_t>& rgba, int& outWidth, int& outHeight) {
    if (!m_impl->ready) return false;
    const int w = m_impl->width;
    const int h = m_impl->height;
    if (w <= 0 || h <= 0) return false;

    std::vector<uint8_t> raw(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    m_impl->fb->Bind();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    rgba.resize(raw.size());
    const size_t rowBytes = static_cast<size_t>(w) * 4u;
    for (int y = 0; y < h; ++y) {
        std::memcpy(rgba.data() + static_cast<size_t>(y) * rowBytes,
                    raw.data() + static_cast<size_t>(h - 1 - y) * rowBytes,
                    rowBytes);
    }
    outWidth = w;
    outHeight = h;
    return true;
}

void GlView::UpdateParticles(const std::vector<Math::Vector3>& positions,
                             const std::vector<float>& colors) {
    m_impl->positions = positions;
    m_impl->colors = colors;
    m_impl->particlesDirty = true;
}

void GlView::UpdateSurface(const std::vector<float>& verts,
                           const std::vector<unsigned>& indices) {
    m_impl->verts = verts;
    m_impl->indices = indices;
    m_impl->surfaceDirty = true;
}

void GlView::UpdateModel(const ModelDataPtr& model) {
    m_impl->model = model;
    m_impl->modelDirty = true;
}

void GlView::SetSurfaceVisible(bool visible) {
    if (m_impl->layer) m_impl->layer->SetSurfaceVisible(visible);
}

void GlView::SetParticlesVisible(bool visible) {
    if (m_impl->layer) m_impl->layer->SetParticlesVisible(visible);
}

void GlView::SetTank(float x, float y, float z, float cx, float cz, bool openTop) {
    if (m_impl->layer) m_impl->layer->SetTankSize(x, y, z, cx, cz, openTop);
}

void GlView::SetParticleRadius(float radius) {
    if (m_impl->layer) m_impl->layer->SetParticleScale(radius);
}

int GlView::TriangleCount() const {
    return m_impl->layer ? m_impl->layer->SurfaceTriangleCount() : 0;
}

void GlView::Orbit(float dxPixels, float dyPixels) {
    render::OrbitCamera(m_impl->cam, dxPixels, dyPixels);
}

void GlView::Look(float dxPixels, float dyPixels) {
    render::LookCamera(m_impl->cam, dxPixels, dyPixels);
}

void GlView::Pan(float dxPixels, float dyPixels) {
    render::PanCamera(m_impl->cam, dxPixels, dyPixels, m_impl->height);
}

void GlView::Zoom(float wheelDelta) {
    render::ZoomCamera(m_impl->cam, wheelDelta);
}

void GlView::Fly(float forward, float right, float up, float deltaSeconds) {
    render::FlyCamera(m_impl->cam, forward, right, up, deltaSeconds);
}

void GlView::ResetCamera() {
    m_impl->cam = CameraState{};
    m_impl->PushCamera();
}

CameraState& GlView::cameraState() { return m_impl->cam; }

}  // namespace render
