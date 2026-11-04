#include "OpenGLLayer.hpp"
#include "Math/Vector3.hpp"
#include "Math/Matrix4.hpp"
#include "Camera.hpp"

#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace {

// The four shader files live in src/ShaderProgram. Depending on how the binary
// is launched the working directory may be the repo root, build/ or the install
// prefix, so try the plausible locations instead of trusting one.
std::filesystem::path ResolveShaderDir(const std::string& overrideDir) {
	namespace fs = std::filesystem;
	auto hasShaders = [](const fs::path& p) {
		return !p.empty() && fs::exists(p / "vertex_shader.vert") &&
		       fs::exists(p / "surface_vertex.glsl");
	};
	if (!overrideDir.empty() && hasShaders(fs::path(overrideDir))) return fs::path(overrideDir);
	if (const char* env = std::getenv("IISPH_SHADER_DIR")) {
		if (hasShaders(fs::path(env))) return fs::path(env);
	}
	std::vector<fs::path> candidates = {
		fs::path("shaders"),                    // copied next to the binary by CMake
		fs::path("src/ShaderProgram"),          // launched from the repo root
		fs::path("../src/ShaderProgram"),       // launched from build/
		fs::path("../../src/ShaderProgram"),    // launched from build/<config>/
	};
	const fs::path self(__FILE__);
	if (!self.parent_path().empty()) {
		const fs::path root = self.parent_path().parent_path();
		candidates.push_back(root / "ShaderProgram");
		candidates.push_back(root / "src/ShaderProgram");
	}
	for (const auto& c : candidates) {
		if (hasShaders(c)) return c;
	}
	return fs::path("src/ShaderProgram");
}

}  // namespace

OpenGLLayer::OpenGLLayer(uint32_t width, uint32_t height, const std::string& shaderDir) {
	// InitGeometry();
	const std::filesystem::path baseDir = ResolveShaderDir(shaderDir);

	std::string vertPath = (baseDir / "vertex_shader.vert").string();
    std::string fragPath = (baseDir / "fragment_shader.frag").string();
    
	m_Shader = new Shader(vertPath, fragPath);
	m_FrameBuffer = new FrameBuffer(width, height);

	// Surface mesh shader (Phong-lit translucent water)
	std::string sVert = (baseDir / "surface_vertex.glsl").string();
	std::string sFrag = (baseDir / "surface_fragment.glsl").string();
	m_SurfaceShader = new Shader(sVert, sFrag);
	glGenVertexArrays(1, &m_MeshVAO);
	glBindVertexArray(m_MeshVAO);
	glGenBuffers(1, &m_MeshVBO);
	glBindBuffer(GL_ARRAY_BUFFER, m_MeshVBO);
	glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
	glGenBuffers(1, &m_MeshEBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_MeshEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
	glBindVertexArray(0);

	// Particles: instanced GL_POINTS (rendered as spheres via fragment discard).
	// aPosInst(2)/aColorInst(3) = per-instance data, divisor 1.
	glGenVertexArrays(1, &m_ParticleVAO);
	glBindVertexArray(m_ParticleVAO);
	glGenBuffers(1, &m_ParticleVBO);
	glGenBuffers(1, &m_ParticleInstanceVBO);
	glBindBuffer(GL_ARRAY_BUFFER, m_ParticleInstanceVBO);
	glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glVertexAttribDivisor(2, 1);
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
	glVertexAttribDivisor(3, 1);
	glBindVertexArray(0);
	glEnable(GL_PROGRAM_POINT_SIZE);
}

OpenGLLayer::~OpenGLLayer() {
	glDeleteVertexArrays(1, &m_VAO);
	glDeleteBuffers(1, &m_VBO);
	glDeleteVertexArrays(1, &m_TankVAO);
	glDeleteBuffers(1, &m_TankVBO);
	glDeleteBuffers(1, &m_EBO);
	glDeleteVertexArrays(1, &m_ParticleVAO);
	glDeleteBuffers(1, &m_ParticleVBO);
	glDeleteBuffers(1, &m_ParticleInstanceVBO);
	glDeleteVertexArrays(1, &m_MeshVAO);
	glDeleteBuffers(1, &m_MeshVBO);
	glDeleteBuffers(1, &m_MeshEBO);
	delete m_SurfaceShader;
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

void OpenGLLayer::SetTankSize(float x, float y, float z, float cx, float cz, bool openTop) {
	// box [cx-x/2, cx+x/2] x [0,y] x [cz-z/2, cz+z/2] (openTop: no top face)
	const float x0 = cx - 0.5f * x, x1 = cx + 0.5f * x;
	const float z0 = cz - 0.5f * z, z1 = cz + 0.5f * z;
	float c[8][3] = {
		{x0,0,z0},{x1,0,z0},{x1,0,z1},{x0,0,z1},
		{x0,y,z0},{x1,y,z0},{x1,y,z1},{x0,y,z1},
	};
	// edges; top face omitted when the container is open
	int e[12][2] = {
		{0,1},{1,2},{2,3},{3,0},   // bottom
		{4,5},{5,6},{6,7},{7,4},   // top (skipped if openTop)
		{0,4},{1,5},{2,6},{3,7},   // vertical
	};
	const int nEdges = openTop ? 8 : 12;
	// GL_LINES: two vertices per edge; pos(3) + light-blue color(3)
	float v[12 * 2 * 6];
	int n = 0;
	for (int i = 0; i < nEdges; i++) {
		for (int k = 0; k < 2; k++) {
			int idx = e[i][k];
			v[n++] = c[idx][0];
			v[n++] = c[idx][1];
			v[n++] = c[idx][2];
			v[n++] = 0.85f;   // brighter cyan-blue so the tank is clearly visible
			v[n++] = 0.95f;
			v[n++] = 1.00f;
		}
	}
	m_TankLineCount = nEdges * 2;

	if (m_TankVAO == 0) glGenVertexArrays(1, &m_TankVAO);
	if (m_TankVBO == 0) glGenBuffers(1, &m_TankVBO);
	glBindVertexArray(m_TankVAO);
	glBindBuffer(GL_ARRAY_BUFFER, m_TankVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	// NOTE: drawn with m_Shader (particle shader), whose attributes are
	// location 2 = aPosInst, location 3 = aColorInst — so use those locations.
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
	glEnableVertexAttribArray(3);
	glBindVertexArray(0);
}

void OpenGLLayer::Render() {
	// Qt Quick's render thread can leave blending, depth writes or scissor
	// state enabled from its own compositing pass. The widget/headless hosts
	// start from clean defaults, so pin the states the water pass relies on.
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glDisable(GL_CULL_FACE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	m_FrameBuffer->Bind();

	glViewport(0, 0, m_FrameBuffer->GetWidth(), m_FrameBuffer->GetHeight());
	glClearColor(0.12f, 0.15f, 0.22f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	m_Shader->Bind();

	Math::Matrix4 model = Math::Matrix4();
	Math::Matrix4 view = m_Camera.GetViewMatrix();
	Math::Matrix4 projection = Math::Matrix4();

	model = model.Rotate(0.0f, Math::Vector3(0.0f, 1.0f, 0.0f));

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
	glUniform1f(glGetUniformLocation(m_Shader->m_RendererID, "particleScale"), m_ParticleScale);

	// The static-cube path is only valid if InitGeometry() ran (it is currently
	// disabled), so guard it.
	if (m_VAO != 0) {
		glBindVertexArray(m_VAO);
		glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, (void*)0);
		glBindVertexArray(0);
	}

	// Particles first, then the opaque surface. Particles outside the mesh stay
	// visible (closer depth), while particles inside it are correctly hidden.
	// This order also keeps "both" consistent on Qt Quick, whose compositor can
	// leave depth-related state in a different shape than QOpenGLWidget.
	if (m_ShowParticles && m_ParticleCount > 0) {
		m_Shader->Bind();
		glBindVertexArray(m_ParticleVAO);
		glDrawArraysInstanced(GL_POINTS, 0, 1, (GLsizei)m_ParticleCount);
		glBindVertexArray(0);
	}

	// Reconstructed fluid surface (Phong-lit, opaque water).
	if (m_ShowSurface && m_MeshIndexCount > 0 && m_SurfaceShader) {
		// Set depth state again immediately before the mesh pass. This protects
		// the opaque water from host-side state leakage between two draw calls
		// (most visibly on Qt Quick's render thread).
		glEnable(GL_DEPTH_TEST);
		glDepthMask(GL_TRUE);
		glDepthFunc(GL_LESS);
		m_SurfaceShader->Bind();
		glUniformMatrix4fv(glGetUniformLocation(m_SurfaceShader->m_RendererID, "model"), 1, GL_FALSE, &model.m[0][0]);
		glUniformMatrix4fv(glGetUniformLocation(m_SurfaceShader->m_RendererID, "view"), 1, GL_FALSE, &view.m[0][0]);
		glUniformMatrix4fv(glGetUniformLocation(m_SurfaceShader->m_RendererID, "projection"), 1, GL_FALSE, &projection.m[0][0]);
		glUniform3f(glGetUniformLocation(m_SurfaceShader->m_RendererID, "viewPos"),
		            m_Camera.Position.x, m_Camera.Position.y, m_Camera.Position.z);
		glBindVertexArray(m_MeshVAO);
		glDrawElements(GL_TRIANGLES, m_MeshIndexCount, GL_UNSIGNED_INT, (void*)0);
		glBindVertexArray(0);
		m_SurfaceShader->Unbind();
	}

	// Tank wireframe: drawn on top (depth test off) so the box is always visible.
	if (m_TankVAO != 0) {
		// Re-bind unconditionally: the surface pass ends with
		// m_SurfaceShader->Unbind() (glUseProgram(0)), and in pure-surface mode
		// (m_ShowParticles == false) the particle pass that would re-bind
		// m_Shader is skipped - the wireframe then drew with no program bound.
		m_Shader->Bind();
		glDisable(GL_DEPTH_TEST);
		glLineWidth(1.5f);
		glBindVertexArray(m_TankVAO);
		glDrawArrays(GL_LINES, 0, m_TankLineCount);
		glBindVertexArray(0);
		glEnable(GL_DEPTH_TEST);
	}

	m_Shader->Unbind();

	m_FrameBuffer->UBind();
}

void OpenGLLayer::UpdateParticles(const std::vector<Math::Vector3>& positions,
                                  const std::vector<float>& colors) {
	const size_t n = positions.size();
	m_ParticleCount = n;
	if (n == 0) return;

	// per-instance data: pos(3) + color(3)
	std::vector<float> data(n * 6);
	for (size_t p = 0; p < n; p++) {
		data[p * 6 + 0] = positions[p].x;
		data[p * 6 + 1] = positions[p].y;
		data[p * 6 + 2] = positions[p].z;
		data[p * 6 + 3] = colors[p * 3 + 0];
		data[p * 6 + 4] = colors[p * 3 + 1];
		data[p * 6 + 5] = colors[p * 3 + 2];
	}

	glBindBuffer(GL_ARRAY_BUFFER, m_ParticleInstanceVBO);
	// Grow-only allocation. glBufferData is NOT called every frame: each call
	// makes the driver allocate a fresh buffer object and park the old one in
	// its deferred-delete queue, which a headless run (no present/SwapBuffers
	// to advance the frame) never drains — committed memory then grows by the
	// full per-frame buffer size until a std::bad_alloc ends the run.
	const size_t bytes = data.size() * sizeof(float);
	if (bytes > m_ParticleInstanceBytes) {
		glBufferData(GL_ARRAY_BUFFER, bytes, data.data(), GL_DYNAMIC_DRAW);
		m_ParticleInstanceBytes = bytes;
	} else if (bytes > 0) {
		glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, data.data());
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void OpenGLLayer::ResizeFrameBuffer(uint32_t width, uint32_t height) {
	m_FrameBuffer->Resize(width, height);
	glViewport(0, 0, width, height);
}

void OpenGLLayer::UpdateSurfaceMesh(const std::vector<float>& verts,
                                    const std::vector<unsigned>& indices) {
	m_MeshIndexCount = (GLsizei)indices.size();
	if (verts.empty() || indices.empty()) return;

	// ELEMENT_ARRAY_BUFFER binding is part of the VAO state, so the VAO must be
	// bound before uploading indices — otherwise the data goes to the default
	// VAO and m_MeshVAO keeps an EMPTY index buffer (=> garbage triangles).
	glBindVertexArray(m_MeshVAO);
	// verts are already interleaved pos(3)+normal(3) from the reconstructor.
	// Grow-only allocation: re-specifying the storage every rebuild (that is,
	// every 3rd frame) leaves an orphaned buffer in the driver's deferred-delete
	// queue, which a headless run never drains (see UpdateParticles).
	glBindBuffer(GL_ARRAY_BUFFER, m_MeshVBO);
	const size_t vbytes = verts.size() * sizeof(float);
	if (vbytes > m_MeshVBOBytes) {
		glBufferData(GL_ARRAY_BUFFER, vbytes, verts.data(), GL_DYNAMIC_DRAW);
		m_MeshVBOBytes = vbytes;
	} else {
		glBufferSubData(GL_ARRAY_BUFFER, 0, vbytes, verts.data());
	}
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_MeshEBO);
	const size_t ibytes = indices.size() * sizeof(unsigned);
	if (ibytes > m_MeshEBOBytes) {
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, ibytes, indices.data(), GL_DYNAMIC_DRAW);
		m_MeshEBOBytes = ibytes;
	} else {
		glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, ibytes, indices.data());
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	// NOTE: do NOT unbind GL_ELEMENT_ARRAY_BUFFER here while the VAO is bound —
	// that would clear m_MeshVAO's index-buffer slot to 0 and glDrawElements
	// would run with a NULL index buffer (driver crash at present()).
	glBindVertexArray(0);
}