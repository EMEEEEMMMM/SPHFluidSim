#include "OpenGLLayer.hpp"
#include "Math/Vector3.hpp"
#include "Math/Matrix4.hpp"
#include "Camera.hpp"

#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <limits>
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

// ---------------------------------------------------------------------------
// Inline GLSL. The project's .glsl/.vert/.frag files are owned by another
// group in the parallel build, so the v2 outline + gizmo passes compile their
// own tiny programs from strings embedded here. Nothing on disk is touched.
// ---------------------------------------------------------------------------
const char* kOutlineVS = R"GLSL(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec3 uCenter;             // model pivot (= bounds centre in world)
uniform float uInflate;           // fractional uniform expansion
void main() {
    vec4 world = model * vec4(aPos, 1.0);
    // Scale around the pivot, not along vertex normals: smooth normals on a
    // pyramid can collapse the offset hull inside the original silhouette and
    // produce an empty stencil ring. A pivot-centred scale always encloses it.
    world.xyz = uCenter + (world.xyz - uCenter) * (1.0 + uInflate);
    gl_Position = projection * view * world;
}
)GLSL";

const char* kOutlineFS = R"GLSL(
#version 330 core
out vec4 FragColor;
uniform vec3 uColor;
void main() { FragColor = vec4(uColor, 1.0); }
)GLSL";

const char* kEdgeVS = R"GLSL(
#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
void main() {
    vec4 clip = projection * view * model * vec4(aPos, 1.0);
    // Constant depth bias towards the camera: an edge and the face it bounds
    // rasterise at nearly the same depth, and a coincident patch (bore lid)
    // would otherwise punch holes in the line.
    clip.z -= 0.00012 * clip.w;
    gl_Position = clip;
}
)GLSL";

const char* kEdgeFS = R"GLSL(
#version 330 core
out vec4 FragColor;
uniform vec4 uColor;
void main() { FragColor = uColor; }
)GLSL";

const char* kGizmoVS = R"GLSL(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aColor;
uniform mat4 view;
uniform mat4 projection;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = projection * view * vec4(aPos, 1.0);
}
)GLSL";

const char* kGizmoFS = R"GLSL(
#version 330 core
in vec3 vColor;
out vec4 FragColor;
void main() { FragColor = vec4(vColor, 1.0); }
)GLSL";

GLuint CompileInlineProgram(const char* vs, const char* fs) {
    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = {0};
            glGetShaderInfoLog(shader, (GLsizei)sizeof(log), nullptr, log);
            std::cerr << "[gl] inline shader compile: " << log << std::endl;
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (v == 0 || f == 0) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(program, (GLsizei)sizeof(log), nullptr, log);
        std::cerr << "[gl] inline program link: " << log << std::endl;
        glDeleteProgram(program);
        program = 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return program;
}

// Axis colours for the gizmo (1..6 = translate XYZ, rotate XYZ).
const float kGizmoColors[6][3] = {
    {0.95f, 0.25f, 0.25f},   // X
    {0.30f, 0.90f, 0.30f},   // Y
    {0.30f, 0.55f, 1.00f},   // Z
    {0.95f, 0.25f, 0.25f},   // about X
    {0.30f, 0.90f, 0.30f},   // about Y
    {0.30f, 0.55f, 1.00f},   // about Z
};

const float kOutlineColor[3] = {1.0f, 0.85f, 0.1f};   // yellow

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

	// Imported models reuse the surface shader (neutral studio material is
	// selected with uImportedModel=1). GPU buffers are now per ModelInstance's
	// ModelData, created lazily by EnsureGpuModel().
	m_ModelShader = new Shader(sVert, sFrag);

	// Particles: instanced GL_POINTS (rendered as spheres via fragment discard).
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

	InitInlinePrograms();
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
	ReleaseGpuModels();
	if (m_OutlineProgram) glDeleteProgram(m_OutlineProgram);
	if (m_EdgeProgram) glDeleteProgram(m_EdgeProgram);
	if (m_GizmoProgram) glDeleteProgram(m_GizmoProgram);
	glDeleteVertexArrays(1, &m_GizmoVAO);
	glDeleteBuffers(1, &m_GizmoVBO);
	delete m_ModelShader;
	delete m_SurfaceShader;
	delete m_Shader;
	delete m_FrameBuffer;
}

void OpenGLLayer::InitInlinePrograms() {
	m_OutlineProgram = CompileInlineProgram(kOutlineVS, kOutlineFS);
	m_EdgeProgram = CompileInlineProgram(kEdgeVS, kEdgeFS);
	m_GizmoProgram = CompileInlineProgram(kGizmoVS, kGizmoFS);

	// Gizmo lines: interleaved pos(3) + color(3), no index buffer.
	glGenVertexArrays(1, &m_GizmoVAO);
	glGenBuffers(1, &m_GizmoVBO);
	glBindVertexArray(m_GizmoVAO);
	glBindBuffer(GL_ARRAY_BUFFER, m_GizmoVBO);
	glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
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
	// Qt Quick's render thread can leave blending, depth writes, scissor or
	// stencil state enabled from its own compositing pass. The widget/headless
	// hosts start from clean defaults, so pin every state the passes rely on.
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glDisable(GL_CULL_FACE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0xFF);
	glStencilFunc(GL_ALWAYS, 0, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glClearStencil(0);

	m_FrameBuffer->Bind();

	glViewport(0, 0, m_FrameBuffer->GetWidth(), m_FrameBuffer->GetHeight());
	glClearColor(m_Bg[0], m_Bg[1], m_Bg[2], 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	Math::Matrix4 model = Math::Matrix4();          // identity: cube + particles
	Math::Matrix4 view = m_Camera.GetViewMatrix();
	Math::Matrix4 projection = Math::Matrix4();

	float aspect = (float)m_FrameBuffer->GetWidth() / m_FrameBuffer->GetHeight();
	if (m_CurrentMode == ProjectionMode::Perspective) {
		// Near plane at 1 cm: imported CAD parts can be a few centimetres across
		// and are framed closely, so 10 cm would clip them away entirely.
		projection = Math::Matrix4::Persp(Math::radians(m_Camera.Zoom), aspect, 0.01f, 100.0f);
	} else {
		float size = 5.0f;
		projection = Math::Matrix4::Ortho(-size * aspect, size * aspect, -size, size, 0.1f, 100.0f);
	}

	const bool empty = (m_SceneIndex == 2);   // Empty: no tank/particles/surface

	m_Shader->Bind();
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

	// Imported visual assets: one draw per visible ModelInstance, each with its
	// own model matrix (transform comes from A group's ModelInstance::matrix).
	if (m_ModelShader && m_Models && !m_Models->empty()) {
		m_ModelShader->Bind();
		const GLuint pid = m_ModelShader->m_RendererID;
		glUniformMatrix4fv(glGetUniformLocation(pid, "view"), 1, GL_FALSE, &view.m[0][0]);
		glUniformMatrix4fv(glGetUniformLocation(pid, "projection"), 1, GL_FALSE, &projection.m[0][0]);
		glUniform3f(glGetUniformLocation(pid, "viewPos"),
		            m_Camera.Position.x, m_Camera.Position.y, m_Camera.Position.z);
		glUniform1i(glGetUniformLocation(pid, "uImportedModel"), 1);
		const GLint instanceModelLoc = glGetUniformLocation(pid, "model");
		// Push the shaded surface back a hair so the CAD edge pass below wins
		// the depth test on exactly-coincident faces.
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(2.0f, 2.0f);
		for (const auto& inst : *m_Models) {
			if (!inst || !inst->visible || !inst->data) continue;
			auto it = m_GpuModels.find(inst->data.get());
			if (it == m_GpuModels.end() || it->second.indexCount == 0) continue;
			Math::Matrix4 instModel = inst->matrix();
			glUniformMatrix4fv(instanceModelLoc, 1, GL_FALSE, &instModel.m[0][0]);
			glBindVertexArray(it->second.vao);
			glDrawElements(GL_TRIANGLES, it->second.indexCount, GL_UNSIGNED_INT, (void*)0);
		}
		glDisable(GL_POLYGON_OFFSET_FILL);
		glBindVertexArray(0);
		m_ModelShader->Unbind();

		// CAD boundary edges (STEP only): the circles and corners SolidWorks
		// draws, including edges where two faces are geometrically coincident.
		if (m_EdgeProgram != 0) {
			glUseProgram(m_EdgeProgram);
			glUniformMatrix4fv(glGetUniformLocation(m_EdgeProgram, "view"), 1, GL_FALSE,
			                   &view.m[0][0]);
			glUniformMatrix4fv(glGetUniformLocation(m_EdgeProgram, "projection"), 1, GL_FALSE,
			                   &projection.m[0][0]);
			glUniform4f(glGetUniformLocation(m_EdgeProgram, "uColor"),
			            0.10f, 0.12f, 0.16f, 1.0f);
			// Coincident faces (a bore lid on a perforated plate) put the edge
			// and the surface at the same depth: LEQUAL keeps the edge on top.
			glDepthFunc(GL_LEQUAL);
			const GLint edgeModelLoc = glGetUniformLocation(m_EdgeProgram, "model");
			for (const auto& inst : *m_Models) {
				if (!inst || !inst->visible || !inst->data) continue;
				auto it = m_GpuModels.find(inst->data.get());
				if (it == m_GpuModels.end() || it->second.edgeVertexCount == 0) continue;
				Math::Matrix4 instModel = inst->matrix();
				glUniformMatrix4fv(edgeModelLoc, 1, GL_FALSE, &instModel.m[0][0]);
				glBindVertexArray(it->second.edgeVao);
				glDrawArrays(GL_LINES, 0, it->second.edgeVertexCount);
			}
			glBindVertexArray(0);
			glUseProgram(0);
			glDepthFunc(GL_LESS);
		}
	}

	// Selection chrome (yellow stencil outline + transform gizmo) is drawn in
	// every scene, including Empty, whenever a model is selected.
	if (const ModelInstance* sel = SelectedInstance()) {
		DrawSelectedOutline(*sel, view, projection);
		DrawGizmo(*sel, view, projection);
	}

	// Particles first, then the opaque surface. Particles outside the mesh stay
	// visible (closer depth), while particles inside it are correctly hidden.
	if (!empty && m_ShowParticles && m_ParticleCount > 0) {
		m_Shader->Bind();
		glBindVertexArray(m_ParticleVAO);
		glDrawArraysInstanced(GL_POINTS, 0, 1, (GLsizei)m_ParticleCount);
		glBindVertexArray(0);
	}

	// Reconstructed fluid surface (Phong-lit, opaque water).
	if (!empty && m_ShowSurface && m_MeshIndexCount > 0 && m_SurfaceShader) {
		glEnable(GL_DEPTH_TEST);
		glDepthMask(GL_TRUE);
		glDepthFunc(GL_LESS);
		m_SurfaceShader->Bind();
		glUniformMatrix4fv(glGetUniformLocation(m_SurfaceShader->m_RendererID, "model"), 1, GL_FALSE, &model.m[0][0]);
		glUniformMatrix4fv(glGetUniformLocation(m_SurfaceShader->m_RendererID, "view"), 1, GL_FALSE, &view.m[0][0]);
		glUniformMatrix4fv(glGetUniformLocation(m_SurfaceShader->m_RendererID, "projection"), 1, GL_FALSE, &projection.m[0][0]);
		glUniform3f(glGetUniformLocation(m_SurfaceShader->m_RendererID, "viewPos"),
		            m_Camera.Position.x, m_Camera.Position.y, m_Camera.Position.z);
		glUniform1i(glGetUniformLocation(m_SurfaceShader->m_RendererID, "uImportedModel"), 0);
		glBindVertexArray(m_MeshVAO);
		glDrawElements(GL_TRIANGLES, m_MeshIndexCount, GL_UNSIGNED_INT, (void*)0);
		glBindVertexArray(0);
		m_SurfaceShader->Unbind();
	}

	// Tank wireframe: drawn on top (depth test off) so the box is always visible.
	if (!empty && m_TankVAO != 0) {
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

// ---------------------------------------------------------------------------
// Per-model GPU cache
// ---------------------------------------------------------------------------
void OpenGLLayer::EnsureGpuModel(const ModelData* data) {
	if (!data || data->vertices.empty() || data->indices.empty()) return;
	if (data->vertices.size() % 6u != 0 || data->indices.size() % 3u != 0 ||
	    data->indices.size() > static_cast<size_t>(std::numeric_limits<GLsizei>::max())) {
		return;
	}
	const size_t vertexCount = data->vertices.size() / 6u;
	for (uint32_t index : data->indices) {
		if (index >= vertexCount) return;
	}

	// Immutable ModelData: a size fingerprint is enough to detect a (rare)
	// in-place change. Normally this is a no-op cache hit.
	const uint64_t version =
	    (static_cast<uint64_t>(data->vertices.size() + data->edges.size()) << 32) |
	    static_cast<uint64_t>(data->indices.size());
	auto it = m_GpuModels.find(data);
	if (it != m_GpuModels.end() && it->second.dataVersion == version) return;

	GpuModel g;
	if (it != m_GpuModels.end()) {
		g = it->second;                       // re-upload into the same objects
	} else {
		glGenVertexArrays(1, &g.vao);
		glGenBuffers(1, &g.vbo);
		glGenBuffers(1, &g.ebo);
	}

	glBindVertexArray(g.vao);
	glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
	glBufferData(GL_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(data->vertices.size() * sizeof(float)),
	             data->vertices.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(data->indices.size() * sizeof(uint32_t)),
	             data->indices.data(), GL_STATIC_DRAW);
	// NOTE: do NOT unbind GL_ELEMENT_ARRAY_BUFFER here while the VAO is bound —
	// that would clear the VAO's index-buffer slot to 0.
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	g.indexCount = static_cast<GLsizei>(data->indices.size());

	if (!data->edges.empty()) {
		if (g.edgeVao == 0) {
			glGenVertexArrays(1, &g.edgeVao);
			glGenBuffers(1, &g.edgeVbo);
		}
		glBindVertexArray(g.edgeVao);
		glBindBuffer(GL_ARRAY_BUFFER, g.edgeVbo);
		glBufferData(GL_ARRAY_BUFFER,
		             static_cast<GLsizeiptr>(data->edges.size() * sizeof(float)),
		             data->edges.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
		glBindVertexArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		g.edgeVertexCount = static_cast<GLsizei>(data->edges.size() / 3);
	}

	g.dataVersion = version;
	m_GpuModels[data] = g;
}

void OpenGLLayer::ReleaseGpuModels() {
	for (auto& entry : m_GpuModels) {
		GpuModel& g = entry.second;
		if (g.vao) glDeleteVertexArrays(1, &g.vao);
		if (g.vbo) glDeleteBuffers(1, &g.vbo);
		if (g.ebo) glDeleteBuffers(1, &g.ebo);
		if (g.edgeVao) glDeleteVertexArrays(1, &g.edgeVao);
		if (g.edgeVbo) glDeleteBuffers(1, &g.edgeVbo);
	}
	m_GpuModels.clear();
}

const ModelInstance* OpenGLLayer::SelectedInstance() const {
	if (!m_Models || m_SelectedId < 0) return nullptr;
	for (const auto& inst : *m_Models) {
		if (inst && inst->id == m_SelectedId && inst->visible && inst->data) return inst.get();
	}
	return nullptr;
}

void OpenGLLayer::UpdateModels(const ModelSnapshotPtr& models, int selectedId) {
	m_Models = models;
	m_SelectedId = selectedId;

	std::unordered_set<const ModelData*> live;
	if (m_Models) {
		for (const auto& inst : *m_Models) {
			if (!inst || !inst->data) continue;
			live.insert(inst->data.get());
			EnsureGpuModel(inst->data.get());
		}
	}
	// Drop GPU buffers whose ModelData is no longer referenced by the snapshot.
	for (auto it = m_GpuModels.begin(); it != m_GpuModels.end(); ) {
		if (live.find(it->first) == live.end()) {
			GpuModel& g = it->second;
			glDeleteVertexArrays(1, &g.vao);
			glDeleteBuffers(1, &g.vbo);
			glDeleteBuffers(1, &g.ebo);
			if (g.edgeVao) glDeleteVertexArrays(1, &g.edgeVao);
			if (g.edgeVbo) glDeleteBuffers(1, &g.edgeVbo);
			it = m_GpuModels.erase(it);
		} else {
			++it;
		}
	}
}

void OpenGLLayer::UpdateModel(const ModelDataPtr& model) {
	// Compatibility wrapper: one unselected instance with an identity transform.
	if (!model) {
		UpdateModels(ModelSnapshotPtr(), -1);
		return;
	}
	auto inst = std::make_shared<ModelInstance>();
	inst->id = 0;
	inst->name = "compat";
	inst->data = model;
	inst->visible = true;
	auto snapshot = std::make_shared<ModelSnapshot>();
	snapshot->push_back(inst);   // shared_ptr<ModelInstance> -> shared_ptr<const ModelInstance>
	UpdateModels(snapshot, -1);
}

// ---------------------------------------------------------------------------
// Yellow stencil silhouette outline
// ---------------------------------------------------------------------------
void OpenGLLayer::DrawSelectedOutline(const ModelInstance& inst,
                                      const Math::Matrix4& view,
                                      const Math::Matrix4& projection) {
	if (m_OutlineProgram == 0) return;
	auto it = m_GpuModels.find(inst.data.get());
	if (it == m_GpuModels.end() || it->second.indexCount == 0) return;
	const GpuModel& g = it->second;

	const Math::Matrix4 model = inst.matrix();
	// Fractional expansion around the pivot. The object itself can be very
	// large or small, so keep the ring readable without making it a giant halo.
	const float inflate = 0.018f;

	glUseProgram(m_OutlineProgram);
	glUniformMatrix4fv(glGetUniformLocation(m_OutlineProgram, "model"), 1, GL_FALSE, &model.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(m_OutlineProgram, "view"), 1, GL_FALSE, &view.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(m_OutlineProgram, "projection"), 1, GL_FALSE, &projection.m[0][0]);
	glUniform3f(glGetUniformLocation(m_OutlineProgram, "uCenter"),
	            inst.position.x, inst.position.y, inst.position.z);
	glUniform3f(glGetUniformLocation(m_OutlineProgram, "uColor"),
	            kOutlineColor[0], kOutlineColor[1], kOutlineColor[2]);

	glEnable(GL_STENCIL_TEST);

	// Pass 1: write the object's silhouette into the stencil buffer.
	glStencilFunc(GL_ALWAYS, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glStencilMask(0xFF);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask(GL_FALSE);
	glDisable(GL_DEPTH_TEST);
	glUniform1f(glGetUniformLocation(m_OutlineProgram, "uInflate"), 0.0f);
	glBindVertexArray(g.vao);
	glDrawElements(GL_TRIANGLES, g.indexCount, GL_UNSIGNED_INT, (void*)0);

	// Pass 2: draw the pivot-expanded model in yellow only where the stencil
	// is NOT the object's silhouette (the ring around it).
	glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glStencilMask(0x00);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable(GL_DEPTH_TEST);
	glUniform1f(glGetUniformLocation(m_OutlineProgram, "uInflate"), inflate);
	glDrawElements(GL_TRIANGLES, g.indexCount, GL_UNSIGNED_INT, (void*)0);
	glBindVertexArray(0);

	glUseProgram(0);
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0xFF);
	glStencilFunc(GL_ALWAYS, 0, 0xFF);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
}

// ---------------------------------------------------------------------------
// Transform gizmo (world-axis translate arrows + rotate rings, no scale)
// ---------------------------------------------------------------------------
void OpenGLLayer::DrawGizmo(const ModelInstance& inst,
                            const Math::Matrix4& view,
                            const Math::Matrix4& projection) {
	if (m_GizmoProgram == 0 || m_GizmoVAO == 0) return;
	const std::vector<render::GizmoLine> segs =
	    render::MakeGizmo(inst.position, m_Camera.Position);

	std::vector<float> data;
	data.reserve(segs.size() * 12);   // 2 verts * (pos3 + color3)
	for (const render::GizmoLine& s : segs) {
		const int h = (s.handle >= 1 && s.handle <= 6) ? s.handle : 1;
		const float* c = kGizmoColors[h - 1];
		auto push = [&](const Math::Vector3& p) {
			data.push_back(p.x); data.push_back(p.y); data.push_back(p.z);
			data.push_back(c[0]); data.push_back(c[1]); data.push_back(c[2]);
		};
		push(s.a);
		push(s.b);
	}
	if (data.empty()) return;

	glBindBuffer(GL_ARRAY_BUFFER, m_GizmoVBO);
	glBufferData(GL_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(data.size() * sizeof(float)),
	             data.data(), GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glDisable(GL_DEPTH_TEST);          // gizmo always on top
	glDepthMask(GL_FALSE);
	glUseProgram(m_GizmoProgram);
	glUniformMatrix4fv(glGetUniformLocation(m_GizmoProgram, "view"), 1, GL_FALSE, &view.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(m_GizmoProgram, "projection"), 1, GL_FALSE, &projection.m[0][0]);
	glBindVertexArray(m_GizmoVAO);
	glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(data.size() / 6));
	glBindVertexArray(0);
	glUseProgram(0);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
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
