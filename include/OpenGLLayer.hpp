#pragma once
#include <glad/glad.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "FrameBuffer.hpp"
#include "Shader.h"
#include "Camera.hpp"
#include "Math/Matrix4.hpp"
#include "Math/Vector3.hpp"
#include "Model.hpp"
#include "Render/CameraRig.hpp"

enum class ProjectionMode { Perspective, Orthographic };

class OpenGLLayer {
public:
    // shaderDir: explicit directory holding the four .vert/.frag/.glsl files.
    // Empty -> the built-in search path (see ResolveShaderDir in the .cpp).
    OpenGLLayer(uint32_t width, uint32_t height, const std::string& shaderDir = std::string());
    ~OpenGLLayer();

    void Render();
    void ResizeFrameBuffer(uint32_t width, uint32_t height);
    FrameBuffer* GetFrameBuffer() const { return m_FrameBuffer; };

    void ToggleProjectionMode() {
        if (m_CurrentMode == ProjectionMode::Perspective) {
            m_CurrentMode = ProjectionMode::Orthographic;
            std::cout << "Switch to Orthographic projection" << std::endl;
        } else {
            m_CurrentMode = ProjectionMode::Perspective;
            std::cout << "Switch to Perspective projection" << std::endl;
        }

    }

    Camera& GetCamera() { return m_Camera; }

    // Feed per-particle (position, color) data; drawn inside Render() with the
    // same shader/model/view/projection as the static geometry.
    void UpdateParticles(const std::vector<Math::Vector3>& positions,
                         const std::vector<float>& colors);
    void SetParticleScale(float scale) { m_ParticleScale = scale; }

    // Wireframe outline of the simulation tank (drawn on top, always visible).
    void SetTankSize(float x, float y, float z, float cx = 0.0f, float cz = 0.0f, bool openTop = false);

    // Reconstructed fluid surface mesh (Akinci 2012): verts = 3 floats/vertex,
    // indices = triangles. Drawn with the Phong surface shader when visible.
    void UpdateSurfaceMesh(const std::vector<float>& verts, const std::vector<unsigned>& indices);

    // v2 multi-model path. Every ModelInstance gets its own VAO/VBO/EBO,
    // cached by its (immutable) ModelData so several instances of the same
    // imported file share one GPU upload. `selectedId` drives the yellow
    // stencil outline and the transform gizmo. Passing a null/empty snapshot
    // clears all models. Must be called from the render thread (it may create
    // or delete GL buffers).
    void UpdateModels(const ModelSnapshotPtr& models, int selectedId);

    // Compatibility wrapper: draws one unselected model with an identity
    // transform. Kept so existing callers of the pre-v2 single-model API keep
    // working.
    void UpdateModel(const ModelDataPtr& model);

    // 0..1 linear RGB used for the 3D clear colour (also the headless default).
    void SetViewportBackground(const float rgb[3]) {
        m_Bg[0] = rgb[0];
        m_Bg[1] = rgb[1];
        m_Bg[2] = rgb[2];
    }

    // scene encoding: 0 = dam, 1 = inflow, 2 = empty. Empty skips tank /
    // particles / surface (models + gizmo + outline are still drawn).
    void SetSceneIndex(int scene) { m_SceneIndex = scene; }

    void SetSurfaceVisible(bool v) { m_ShowSurface = v; }
    void SetParticlesVisible(bool v) { m_ShowParticles = v; }
    bool SurfaceVisible() const { return m_ShowSurface; }
    int  SurfaceTriangleCount() const { return m_MeshIndexCount / 3; }

private:
    // One GPU buffer set per distinct ModelData.
    struct GpuModel {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLsizei indexCount = 0;
        // Optional CAD boundary line list (STEP): drawn as dark feature edges.
        GLuint edgeVao = 0;
        GLuint edgeVbo = 0;
        GLsizei edgeVertexCount = 0;
        uint64_t dataVersion = 0;   // fingerprint of vertices/indices sizes
    };

    void InitGeometry();
    void InitInlinePrograms();
    void ReleaseGpuModels();
    void EnsureGpuModel(const ModelData* data);
    const ModelInstance* SelectedInstance() const;
    void DrawSelectedOutline(const ModelInstance& inst,
                             const Math::Matrix4& view,
                             const Math::Matrix4& projection);
    void DrawGizmo(const ModelInstance& inst,
                   const Math::Matrix4& view,
                   const Math::Matrix4& projection);

    GLuint m_VAO = 0;
    GLuint m_VBO = 0;
    GLuint m_EBO = 0;
    Shader* m_Shader = nullptr;
    FrameBuffer* m_FrameBuffer = nullptr;

    // Reconstructed surface mesh (Phong-lit translucent water).
    GLuint m_MeshVAO = 0;
    GLuint m_MeshVBO = 0;
    GLuint m_MeshEBO = 0;
    GLsizei m_MeshIndexCount = 0;
    size_t m_MeshVBOBytes = 0;   // allocated capacity (grow-only, see UpdateSurfaceMesh)
    size_t m_MeshEBOBytes = 0;
    Shader* m_SurfaceShader = nullptr;
    bool m_ShowSurface = false;
    bool m_ShowParticles = true;   // false in pure-surface mode

    // Imported models: neutral Phong material (uImportedModel=1), one GPU
    // buffer set per ModelData, shared across instances.
    Shader* m_ModelShader = nullptr;
    std::unordered_map<const ModelData*, GpuModel> m_GpuModels;
    ModelSnapshotPtr m_Models;
    int m_SelectedId = -1;
    int m_SceneIndex = 0;
    float m_Bg[3] = {0.12f, 0.15f, 0.22f};   // #1F2638

    // Inline GLSL programs (compiled in OpenGLLayer.cpp, no .glsl file edits):
    // a normal-inflated solid program for the stencil outline, and a simple
    // coloured line program for the transform gizmo.
    GLuint m_OutlineProgram = 0;
    GLuint m_EdgeProgram = 0;
    GLuint m_GizmoProgram = 0;
    GLuint m_GizmoVAO = 0;
    GLuint m_GizmoVBO = 0;

    ProjectionMode m_CurrentMode = ProjectionMode::Perspective;
    Camera m_Camera = Camera(2.8f, 1.4f, 2.2f, 0.0f, 1.0f, 0.0f, -136.7f, -18.0f);

    // Tank wireframe (GL_LINES): 8 corners + 12 edges, drawn without depth test.
    GLuint m_TankVAO = 0;
    GLuint m_TankVBO = 0;
    int m_TankLineCount = 0;

    // Particles: instanced GL_POINTS (spheres via fragment discard).
    // aPosInst(2)/aColorInst(3) = per-instance data with divisor 1.
    GLuint m_ParticleVAO = 0;
    GLuint m_ParticleVBO = 0;         // unused (instance VBO is the geometry)
    GLuint m_ParticleInstanceVBO = 0; // per-instance pos(3)+color(3)
    size_t m_ParticleCount = 0;
    size_t m_ParticleInstanceBytes = 0;   // allocated capacity (grow-only)
    float m_ParticleScale = 0.012f;   // particle radius (set from spacing)
};
