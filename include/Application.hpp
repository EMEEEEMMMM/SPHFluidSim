#pragma once
#include <glad/glad.h>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <GLFW/glfw3.h>
#include "OpenGLLayer.hpp"
#include "ImguiLayer.hpp"
#include "FluidSolver.hpp"
#include "GaussianRenderer.hpp"
#include "Math/Vector3.hpp"

class Application {
public:
    Application(int width = 1280, int height = 720, const char* title = "App");
    ~Application();
    int Run();

private:
    bool Init();
    void MainLoop();
    void CleanUp();
    void RenderSimulationUI();
    void SetupScene(int scene);   // 0 = dam break, 1 = open tank + inflow
    int m_Scene = 1;
    int m_AppliedScene = -1;

    int m_Width;
    int m_Height;
    std::string m_Title;
    GLFWwindow* m_Window = nullptr;
    std::unique_ptr<OpenGLLayer> m_OpenGLLayer;
    std::unique_ptr<ImGuiLayer> m_ImGuiLayer;
    std::unique_ptr<FluidSolver> m_Solver;

    bool m_Pause = true;         // start paused, inspect before simulating
    int m_StepsPerFrame = 1;

    // Gaussian-splat surface (docs/theory.md 3DGS path) — async worker thread
    // so the main loop never blocks on the SPH field computation. The main
    // thread uploads the latest finished splat list every frame.
    std::thread m_GaussThread;
    std::mutex m_GaussMutex;
    std::condition_variable m_GaussCV;
    std::atomic<bool> m_GaussQuit{false};
    std::atomic<bool> m_GaussPending{false};
    std::vector<Math::Vector3> m_GaussInputX;    // particle snapshot for the worker
    std::vector<Math::Vector3> m_GaussInputV;
    std::vector<float> m_GaussInputRho;
    std::vector<Splat> m_Splats;                 // latest finished splats (worker->main)
    bool m_HaveSplats = false;
    uint64_t m_GaussVersion = 0;                 // bump when new splats are published
    uint64_t m_UploadedVersion = 0;              // last list uploaded to GL
    void GaussLoop();                            // worker thread body

    int m_ShowSurfaceMode = 0;   // 0 = particles, 1 = surface (gaussians), 2 = both
    int m_LastShowSurfaceMode = 0;
    int m_GaussInterval = 3;     // recompute geometry every N frames
    int m_GaussCounter = 0;
    GaussianRenderer::Params m_GaussParams;      // UI-tunable, copied into the worker's renderer
    std::mutex m_CameraMutex;
    Math::Vector3 m_CameraPos;                   // latest camera position (for specular)

    float m_DeltaTime = 0.0f;
    float m_LastFrame = 0.0f;
    float m_FPS = 0.0f;
    std::string m_TankLabel;

    float m_LastX = 0.0f;
    float m_LastY = 0.0f;
    bool m_FirstMouse = true;
    bool m_IsCameraActive = false;
};