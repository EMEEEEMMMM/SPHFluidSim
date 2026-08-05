#include "Application.hpp"
#include <iostream>
#include <GLFW/glfw3.h>


Application::Application(int width, int height, const char* title)
    : m_Width(width), m_Height(height), m_Title(title) {}

Application::~Application() {
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

    return true;
}

void Application::MainLoop() {
    while (!glfwWindowShouldClose(m_Window)) {
        glfwPollEvents();

        float currentFrame = glfwGetTime();
        m_DeltaTime = currentFrame - m_LastFrame;
        m_LastFrame = currentFrame;

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

int Application::Run() {
    if (!Init()) return -1;
    MainLoop();
    return 0;
}