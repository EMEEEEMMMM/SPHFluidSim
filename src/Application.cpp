#include "Application.hpp"
#include <iostream>
#include <cstdio>
#include <cmath>
#include <GLFW/glfw3.h>


Application::Application(int width, int height, const char* title)
    : m_Width(width), m_Height(height), m_Title(title) {
    m_ReconThread = std::thread([this] { ReconLoop(); });
}

Application::~Application() {
    {
        std::lock_guard<std::mutex> lk(m_ReconMutex);
        m_ReconQuit = true;
        m_ReconCV.notify_all();
    }
    if (m_ReconThread.joinable()) m_ReconThread.join();
    CleanUp();
}

bool Application::Init() {
    if (!glfwInit()) {
        std::cerr << "Failed to init GLFW\n";
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // For mac os:
    // #ifdef __APPLE__
    //     glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    // #endif

    m_Window = glfwCreateWindow(m_Width, m_Height, m_Title.c_str(), nullptr, nullptr);
    if (!m_Window) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_Window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD\n";
        return false;
    }

    glViewport(0, 0, m_Width, m_Height);

    m_OpenGLLayer = std::make_unique<OpenGLLayer>(m_Width, m_Height);
    m_ImGuiLayer = std::make_unique<ImGuiLayer>(m_Window);

    m_ImGuiLayer->SwitchProjection = [this]() {
        m_OpenGLLayer->ToggleProjectionMode();
    };

    m_ImGuiLayer->OnSimulationUI = [this]() {
        RenderSimulationUI();
    };

    m_Solver = std::make_unique<FluidSolver>();
    SetupScene(m_Scene);   // default: open tank + central water jet

    return true;
}

void Application::MainLoop() {
    while (!glfwWindowShouldClose(m_Window)) {
        glfwPollEvents();

        float currentFrame = glfwGetTime();
        m_DeltaTime = currentFrame - m_LastFrame;
        m_LastFrame = currentFrame;

        // smoothed FPS for the viewport overlay
        float fps = (m_DeltaTime > 1e-4f) ? 1.0f / m_DeltaTime : 0.0f;
        m_FPS = (m_FPS <= 0.0f) ? fps : m_FPS * 0.9f + fps * 0.1f;
        m_ImGuiLayer->SetViewportOverlay(m_FPS, m_TankLabel);

        bool rightMouseDown = glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

        if (m_ImGuiLayer->IsViewportFocused() && rightMouseDown) {
            m_IsCameraActive = true;
        } else if (!rightMouseDown) {
            m_IsCameraActive = false;
            m_FirstMouse = true;
        }

        if (m_IsCameraActive) {
            glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            Camera& camera = m_OpenGLLayer->GetCamera();

            if (glfwGetKey(m_Window, GLFW_KEY_W) == GLFW_PRESS) camera.ProcessKeyboard(FORWARD, m_DeltaTime);
            if (glfwGetKey(m_Window, GLFW_KEY_S) == GLFW_PRESS) camera.ProcessKeyboard(BACKWARD, m_DeltaTime);
            if (glfwGetKey(m_Window, GLFW_KEY_A) == GLFW_PRESS) camera.ProcessKeyboard(LEFT, m_DeltaTime);
            if (glfwGetKey(m_Window, GLFW_KEY_D) == GLFW_PRESS) camera.ProcessKeyboard(RIGHT, m_DeltaTime);
            if (glfwGetKey(m_Window, GLFW_KEY_SPACE) == GLFW_PRESS) camera.ProcessKeyboard(UPWARD, m_DeltaTime);
            if (glfwGetKey(m_Window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) camera.ProcessKeyboard(DOWNWARD, m_DeltaTime);

            double xpos, ypos;
            glfwGetCursorPos(m_Window, &xpos, &ypos);
            if (m_FirstMouse) {
                m_LastX = (float)xpos;
                m_LastY = (float)ypos;
                m_FirstMouse = false;
            }
            float xoffset = (float)xpos - m_LastX;
            float yoffset = m_LastY - (float)ypos;
            m_LastX = (float)xpos;
            m_LastY = (float)ypos;

            camera.ProcessMouseMovement(xoffset, yoffset);
        } else {
            glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }

        if (m_ImGuiLayer->IsViewportFocused() && !m_IsCameraActive) {
            float scrollY = ImGui::GetIO().MouseWheel;
            if (scrollY != 0.0f) {
                m_OpenGLLayer->GetCamera().ProcessMouseScroll(scrollY);
            }
        }

        // IISPH simulation: fixed steps per frame (real-time speed control).
        if (!m_Pause) {
            for (int s = 0; s < m_StepsPerFrame; s++) m_Solver->Step();
        }
        m_OpenGLLayer->UpdateParticles(m_Solver->Positions(), m_Solver->Colors());

        // Surface reconstruction (Akinci 2012), async: ask the worker thread for
        // a fresh mesh every m_ReconInterval frames, then render the latest
        // finished mesh (upload to GL only when a new one arrived).
        m_OpenGLLayer->SetSurfaceVisible(m_ShowSurfaceMode > 0);
        m_OpenGLLayer->SetParticlesVisible(m_ShowSurfaceMode != 1);  // hide particles in pure-surface mode
        if (m_ShowSurfaceMode > 0) {
            if (++m_ReconCounter >= m_ReconInterval) {
                m_ReconCounter = 0;
                std::lock_guard<std::mutex> lk(m_ReconMutex);
                if (!m_ReconPending && !m_ReconQuit) {
                    m_ReconInput.assign(m_Solver->Positions().begin(), m_Solver->Positions().end());
                    m_ReconPending = true;
                    m_ReconCV.notify_one();
                }
            }
            {
                std::lock_guard<std::mutex> lk(m_ReconMutex);
                if (m_HaveMesh && m_UploadedVersion != m_ReconVersion) {
                    m_OpenGLLayer->UpdateSurfaceMesh(m_ReconVerts, m_ReconIndices);
                    m_UploadedVersion = m_ReconVersion;
                }
            }
        }

        m_OpenGLLayer->Render();

        m_ImGuiLayer->Begin();

        if (m_ImGuiLayer->RenderViewport(m_OpenGLLayer->GetFrameBuffer())) {
            auto fb = m_OpenGLLayer->GetFrameBuffer();
            glViewport(0, 0, fb->GetWidth(), fb->GetHeight());
        }

        m_ImGuiLayer->RenderUI();
        m_ImGuiLayer->End(m_Window);

        glfwSwapBuffers(m_Window);
    }
}

void Application::CleanUp() {
    m_ImGuiLayer.reset();
    m_OpenGLLayer.reset();

    if (m_Window) {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }
    glfwTerminate();
}

// Worker thread: wait for a particle snapshot, reconstruct the surface, publish
// the mesh. Never touches GL or the solver — it only reads the snapshot.
void Application::ReconLoop() {
    SurfaceReconstructor rec;
    float lastThreshold = -1.0f, lastGrid = -1.0f;
    while (true) {
        std::vector<Math::Vector3> input;
        float thr, gs;
        {
            std::unique_lock<std::mutex> lk(m_ReconMutex);
            m_ReconCV.wait(lk, [this] { return m_ReconQuit.load() || m_ReconPending.load(); });
            if (m_ReconQuit.load()) return;
            m_ReconPending = false;
            input.swap(m_ReconInput);
            thr = m_SurfaceThreshold;   // read under the lock (ImGui may write it)
            gs = m_GridScale;
        }
        if (thr != lastThreshold) { rec.SetSurfaceThreshold(thr); lastThreshold = thr; }
        if (gs != lastGrid) { rec.SetGridScale(gs); lastGrid = gs; }
        const bool ok = rec.Reconstruct(input, m_Solver->Spacing());
        if (ok) {
            std::lock_guard<std::mutex> lk(m_ReconMutex);
            m_ReconVerts = rec.Vertices();
            m_ReconIndices = rec.Indices();
            m_HaveMesh = true;
            m_ReconVersion++;
        }
    }
}

int Application::Run() {
    if (!Init()) return -1;
    MainLoop();
    return 0;
}

void Application::SetupScene(int scene) {
    m_Scene = scene;
    // Particle cap for the open-tank inflow scene = particles needed to fill
    // the fixed 0.6 m^3 water volume at the CURRENT spacing (so switching to a
    // finer resolution keeps the same water volume, just more particles).
    const float sp = m_Solver->GetParams().spacing;
    const int volCap = (int)(0.6f / (sp * sp * sp)) + 8;
    if (scene == 0) {
        // dam break in an OPEN-TOP tank (2 x 1.2 x 1 m, no lid): splash may
        // leave the tank. A one-shot button spawns a spherical drop above the
        // rim that falls in (see RenderSimulationUI).
        m_Solver->InitDamOpen(2.0f, 1.2f, 1.0f, 0.8f);
        m_OpenGLLayer->SetTankSize(2.0f, 1.2f, 1.0f, 0.0f, 0.0f, true);   // open top
    } else {
        // open-top cubic container (side 1 m, centred at x=1, z=0.5) with a
        // continuous central jet falling 0.3 m from above
        m_Solver->InitInflow(1.0f, 0.3f, 100, 0.08f, 1.0f, 0.5f, volCap);
        m_OpenGLLayer->SetTankSize(1.0f, 1.0f, 1.0f, 1.0f, 0.5f, true);
    }
    m_OpenGLLayer->SetParticleScale(0.6f * m_Solver->Spacing());
    m_OpenGLLayer->UpdateParticles(m_Solver->Positions(), m_Solver->Colors());
    char tb[128];
    if (scene == 0) {
        snprintf(tb, sizeof(tb), "tank 2.0 x 1.2 x 1.0 m (open)  |  N=%d", m_Solver->FluidCount());
    } else {
        snprintf(tb, sizeof(tb), "open tank 1.0^3 (inflow)  |  N=%d", m_Solver->FluidCount());
    }
    m_TankLabel = tb;
    m_ShowSurfaceMode = 0;   // particles view by default; switch to surface when the pool forms
    m_AppliedScene = scene;  // CRITICAL: otherwise the UI rebuilds the scene every frame
}

void Application::RenderSimulationUI() {
    // Called inside ImGuiLayer's "Options" window (via OnSimulationUI).
    auto& p = m_Solver->GetParams();
    ImGui::Text("Fluid particles:  %d", m_Solver->FluidCount());
    ImGui::Text("Boundary particles: %d", m_Solver->BoundaryCount());
    ImGui::Text("Sim time:          %.3f s", m_Solver->SimTime());
    ImGui::Text("Avg density error: %.4f %%", m_Solver->AvgDensityError() * 100.0f);
    ImGui::Text("Pressure iters:    %d", m_Solver->LastIterations());

    ImGui::Separator();
    ImGui::Checkbox("Pause", &m_Pause);
    ImGui::Text("Scene");
    ImGui::RadioButton("dam break (open)", &m_Scene, 0);
    ImGui::SameLine();
    ImGui::RadioButton("inflow", &m_Scene, 1);
    if (m_Scene != m_AppliedScene) { SetupScene(m_Scene); }
    if (m_Scene == 0) {
        // one-shot: spawn a spherical drop above the tank rim, it falls in
        if (m_Solver->HasDrop()) {
            ImGui::TextDisabled("Drop spawned (one-shot)");
        } else {
            if (ImGui::Button("Spawn water drop")) {
                m_Solver->SpawnDrop(0.2f, 1.0f, 1.6f, 0.5f);
            }
        }
    }
    ImGui::Separator();
    ImGui::SliderInt("Steps / frame", &m_StepsPerFrame, 1, 8);
    if (ImGui::Button("Step once")) {
        m_Solver->Step();
        m_OpenGLLayer->UpdateParticles(m_Solver->Positions(), m_Solver->Colors());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        m_Solver->Reset();
        m_OpenGLLayer->UpdateParticles(m_Solver->Positions(), m_Solver->Colors());
    }

    ImGui::Separator();
    ImGui::Text("Solver parameters");
    // Resolution: particle spacing. Changing it re-initializes the scene.
    // Particle count = water volume / spacing^3 (dam: 0.512 m^3).
    // Higher resolutions exist for the GPU path: the OpenCL scalar-field
    // kernels only become GPU-bound at large particle counts — at 8k-64k the
    // whole reconstruction is a few ms and the GPU sits mostly idle (task
    // manager will show ~1% utilisation, which is normal, not a bug).
    static const char* resNames[] = {
        "low (0.040 m, ~8k)",
        "medium (0.030 m, ~19k)",
        "high (0.025 m, ~33k)",
        "ultra (0.020 m, ~64k)",
        "cinema (0.015 m, ~150k)",
        "epic (0.0125 m, ~260k)",
        "extreme (0.010 m, ~500k)" };
    static const float resSpacing[] = { 0.040f, 0.030f, 0.025f, 0.020f, 0.015f, 0.0125f, 0.010f };
    constexpr int kNumRes = 7;
    int curRes = 0;
    for (int r = 0; r < kNumRes; r++) if (std::fabs(p.spacing - resSpacing[r]) < 1e-4f) curRes = r;
    if (ImGui::Combo("resolution", &curRes, resNames, kNumRes)) {
        p.spacing = resSpacing[curRes];
        m_Solver->Reset();
        m_OpenGLLayer->SetParticleScale(0.6f * p.spacing);
        m_OpenGLLayer->UpdateParticles(m_Solver->Positions(), m_Solver->Colors());
        char tb[64];
        snprintf(tb, sizeof(tb), "tank %.1f x %.1f x %.1f m  |  %d particles",
                 2.0f, 1.2f, 1.0f, m_Solver->FluidCount());
        m_TankLabel = tb;
    }
    ImGui::Separator();
    ImGui::Text("Pressure solver");
    const char* solverNames[] = { "IISPH (Ihmsen 2013)", "DFSPH (Bender & Koschier 2017)" };
    int curSolver = p.solver;
    if (ImGui::Combo("solver", &curSolver, solverNames, 2)) {
        p.solver = curSolver;
        m_Solver->Reset();   // solver state arrays differ; restart the scene
    }
    if (p.solver == 0) ImGui::SliderFloat("omega (relaxation)", &p.omega, 0.1f, 0.9f);
    ImGui::SliderFloat("eta (density tol.)", &p.eta, 0.0001f, 0.01f, "%.4f");
    if (p.solver == 1) ImGui::SliderFloat("eta_v (divergence tol.)", &p.etaV, 0.0001f, 0.01f, "%.4f");
    ImGui::SliderInt("max pressure iters", &p.maxIter, 2, 100);
    ImGui::SliderFloat("dt [s]", &p.dt, 0.0005f, 0.01f, "%.4f");
    ImGui::Checkbox("CFL step limiter", &p.useCFL);

    ImGui::Separator();
    ImGui::Text("Non-pressure forces (paper Sec.5)");
    ImGui::SliderFloat("XSPH viscosity c", &p.viscosity, 0.0f, 1.0f, "%.3f");
    ImGui::SliderFloat("surface tension k [Akinci13]", &p.surfaceTension, 0.0f, 0.5f, "%.3f");
    ImGui::SliderFloat("wall adhesion kb [Akinci13]", &p.surfaceTensionBoundary, 0.0f, 0.1f, "%.3f");

    ImGui::Separator();
    ImGui::Text("Surface reconstruction (Akinci 2012 [32])");
    ImGui::RadioButton("particles", &m_ShowSurfaceMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("surface", &m_ShowSurfaceMode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("both", &m_ShowSurfaceMode, 2);
    if (m_ShowSurfaceMode > 0) {
        ImGui::SliderFloat("surface threshold", &m_SurfaceThreshold, 0.3f, 0.95f, "%.2f");
        ImGui::SliderFloat("grid scale (0.5 = r/2 HQ, 1.0 = r, 2.0 = fast)", &m_GridScale, 0.5f, 2.0f, "%.1f");
        ImGui::SliderInt("rebuild every N frames", &m_ReconInterval, 1, 20);
        ImGui::Text("triangles: %d", m_OpenGLLayer->SurfaceTriangleCount());
    }
}