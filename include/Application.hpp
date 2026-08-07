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
#include "SurfaceReconstructor.hpp"
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

    int m_Width;
    int m_Height;
    std::string m_Title;
    GLFWwindow* m_Window = nullptr;
    std::unique_ptr<OpenGLLayer> m_OpenGLLayer;
    std::unique_ptr<ImGuiLayer> m_ImGuiLayer;
    std::unique_ptr<FluidSolver> m_Solver;

    bool m_Pause = true;         // start paused, inspect before simulating
    int m_StepsPerFrame = 1;

    // Surface reconstruction (Akinci et al. 2012) — async worker thread so the
    // main loop never blocks on the scalar-field build (~130 ms at r/2, 32k
    // particles). The main thread renders the latest finished mesh every frame.
    std::thread m_ReconThread;
    std::mutex m_ReconMutex;
    std::condition_variable m_ReconCV;
    std::atomic<bool> m_ReconQuit{false};
    std::atomic<bool> m_ReconPending{false};
    std::vector<Math::Vector3> m_ReconInput;   // particle snapshot for the worker
    std::vector<float> m_ReconVerts;           // latest finished mesh (worker->main)
    std::vector<unsigned> m_ReconIndices;
    bool m_HaveMesh = false;
    uint64_t m_ReconVersion = 0;               // bump when a new mesh is published
    uint64_t m_UploadedVersion = 0;            // last mesh uploaded to GL
    void ReconLoop();                          // worker thread body

    int m_ShowSurfaceMode = 0;   // 0 = particles, 1 = surface, 2 = both
    float m_SurfaceThreshold = 0.8f;   // color-field surface threshold
    float m_GridScale = 0.5f;          // field grid spacing multiplier (1.0 = paper's r; 0.5 = r/2 high quality)
    int m_ReconInterval = 3;           // ask the worker for a new mesh every N frames
    int m_ReconCounter = 0;

    float m_DeltaTime = 0.0f;
    float m_LastFrame = 0.0f;
    float m_FPS = 0.0f;
    std::string m_TankLabel;

    float m_LastX = 0.0f;
    float m_LastY = 0.0f;
    bool m_FirstMouse = true;
    bool m_IsCameraActive = false;
};