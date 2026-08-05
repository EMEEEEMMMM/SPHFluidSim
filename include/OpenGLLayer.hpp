#pragma once
#include <glad/glad.h>

#include <cstdint>
#include <iostream>

#include "FrameBuffer.hpp"
#include "Shader.h"
#include "Camera.hpp"


enum class ProjectionMode { Perspective, Orthographic };

class OpenGLLayer {
public:
    OpenGLLayer(uint32_t width, uint32_t height);
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

private:
    void InitGeometry();
    GLuint m_VAO = 0;
    GLuint m_VBO = 0;
    GLuint m_EBO = 0;
    Shader* m_Shader = nullptr;
    FrameBuffer* m_FrameBuffer = nullptr;

    ProjectionMode m_CurrentMode = ProjectionMode::Perspective;
    Camera m_Camera = Camera(Math::Vector3(0.0f, 0.0f, 3.0f));
};