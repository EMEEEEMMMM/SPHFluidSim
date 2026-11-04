#pragma once
#include <glad/glad.h>

#include <cstdint>
#include <iostream>
#include <string>

#include "FrameBuffer.hpp"
#include "Shader.h"
#include "Camera.hpp"
#include "Math/Vector3.hpp"
#include <vector>

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
    void SetSurfaceVisible(bool v) { m_ShowSurface = v; }
    void SetParticlesVisible(bool v) { m_ShowParticles = v; }
    bool SurfaceVisible() const { return m_ShowSurface; }
    int  SurfaceTriangleCount() const { return m_MeshIndexCount / 3; }

private:
    void InitGeometry();
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