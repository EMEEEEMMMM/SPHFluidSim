#pragma once
#include <glad/glad.h>
#include <memory>
#include <string>
#include <GLFW/glfw3.h>
#include "OpenGLLayer.hpp"
#include "ImguiLayer.hpp"

class Application {
public:
    Application(int width = 1280, int height = 720, const char* title = "App");
    ~Application();
    int Run();

private:
    bool Init();
    void MainLoop();
    void CleanUp();

    int m_Width;
    int m_Height;
    std::string m_Title;
    GLFWwindow* m_Window = nullptr;
    std::unique_ptr<OpenGLLayer> m_OpenGLLayer;
    std::unique_ptr<ImGuiLayer> m_ImGuiLayer;

    float m_DeltaTime = 0.0f;
    float m_LastFrame = 0.0f;

    float m_LastX = 0.0f;
    float m_LastY = 0.0f;
    bool m_FirstMouse = true;
    bool m_IsCameraActive = false;
};