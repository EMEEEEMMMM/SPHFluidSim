#include "OpenGLLayer.hpp"
#include "Math/Vector3.hpp"
#include "Math/Matrix4.hpp"
#include "Camera.hpp"

#include <iostream>
#include <filesystem>
#include <GLFW/glfw3.h>

OpenGLLayer::OpenGLLayer(uint32_t width, uint32_t height) {
    // InitGeometry();
	std::filesystem::path baseDir = 
		std::filesystem::path(__FILE__)
			.parent_path()
			.parent_path();

	std::string vertPath = (baseDir / "src/ShaderProgram/vertex_shader.vert").string();
    std::string fragPath = (baseDir / "src/ShaderProgram/fragment_shader.frag").string();
    
	m_Shader = new Shader(vertPath, fragPath);
	m_FrameBuffer = new FrameBuffer(width, height);
}

OpenGLLayer::~OpenGLLayer() {
	glDeleteVertexArrays(1, &m_VAO);
	glDeleteBuffers(1, &m_VBO);
	delete m_Shader;
	delete m_FrameBuffer;
}

void OpenGLLayer::InitGeometry() {
	float vertices[] = {
		-0.5f, -0.5f, 0.5f,  1.0f, 1.0f, 0.0f,
         0.5f, -0.5f, 0.5f,  1.0f, 1.0f, 0.0f,
         0.5f,  0.5f, 0.5f,  1.0f, 1.0f, 0.0f,
        -0.5f,  0.5f, 0.5f,  1.0f, 1.0f, 0.0f,

        -0.5f, -0.5f, -0.5f,  1.0f, 1.0f, 0.0f,
         0.5f, -0.5f, -0.5f,  1.0f, 1.0f, 0.0f,
         0.5f,  0.5f, -0.5f,  1.0f, 1.0f, 0.0f,
        -0.5f,  0.5f, -0.5f,  1.0f, 1.0f, 0.0f,
	};

	unsigned int indices[] = {
		0, 1, 2, 2, 3, 0,
        1, 5, 6, 6, 2, 1,
        7, 6, 5, 5, 4, 7,
        4, 0, 3, 3, 7, 4,
        3, 2, 6, 6, 7, 3,
        4, 5, 1, 1, 0, 4
	};

	glGenVertexArrays(1, &m_VAO);
	glBindVertexArray(m_VAO);

	glGenBuffers(1, &m_VBO);
	glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	glGenBuffers(1, &m_EBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);

	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3* sizeof(float)));
	glEnableVertexAttribArray(1);

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
};

void OpenGLLayer::Render() {
	glEnable(GL_DEPTH_TEST);

	m_FrameBuffer->Bind();

	glViewport(0, 0, m_FrameBuffer->GetWidth(), m_FrameBuffer->GetHeight());
	glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	m_Shader->Bind();

	Math::Matrix4 model = Math::Matrix4();
	Math::Matrix4 view = Math::Matrix4();
	Math::Matrix4 projection = Math::Matrix4();

	model = model.Rotate(0.0f, Math::Vector3(0.0f, 1.0f, 0.0f));
	view = m_Camera.GetViewMatrix();

	float aspect = (float)m_FrameBuffer->GetWidth() / m_FrameBuffer->GetHeight();

	if (m_CurrentMode == ProjectionMode::Perspective) {
		projection = Math::Matrix4::Persp(Math::radians(m_Camera.Zoom), aspect, 0.1f, 100.0f);
	} else {
		float size = 5.0f;
		projection = Math::Matrix4::Ortho(-size * aspect, size * aspect, -size, size, 0.1f, 100.0f);
	}

	unsigned int modelLoc = glGetUniformLocation(m_Shader->m_RendererID, "model");
	unsigned int viewLoc = glGetUniformLocation(m_Shader->m_RendererID, "view");
	unsigned int projectionLoc = glGetUniformLocation(m_Shader->m_RendererID, "projection");

	glUniformMatrix4fv(modelLoc, 1, GL_FALSE, &model.m[0][0]);
	glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &view.m[0][0]);
	glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, &projection.m[0][0]);

	glBindVertexArray(m_VAO);
	glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, (void*)0);
	glBindVertexArray(0);
	
	m_Shader->Unbind();

	m_FrameBuffer->UBind();
}

void OpenGLLayer::ResizeFrameBuffer(uint32_t width, uint32_t height) {
	m_FrameBuffer->Resize(width, height);
	glViewport(0, 0, width, height);
}