#pragma once
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "FrameBuffer.hpp"
#include <functional>
#include <string>

class ImGuiLayer {
public:
    ImGuiLayer(GLFWwindow* window, const char* glslVersion = "#version 130");
    ~ImGuiLayer();

    void Begin();
    bool RenderViewport(FrameBuffer* frameBuffer);
    void RenderUI();
    void End(GLFWwindow* window);

    std::function<void()> SwitchProjection;
    // Optional extra controls drawn inside the "Options" window (simulation UI).
    std::function<void()> OnSimulationUI;

    ImGuiIO& GetIO() const { return ImGui::GetIO(); }
    bool show_demo_window = false;

    // Overlay shown in the top-right corner of the viewport (FPS + tank label).
    void SetViewportOverlay(float fps, const std::string& tankLabel) {
        m_FPS = fps;
        m_TankLabel = tankLabel;
    }

    bool IsViewportFocused() const { return m_IsViewportFocused; }
    bool IsViewportHovered() const { return m_IsViewportHovered; }

private:
    bool projection_mode = true;

    float m_FPS = 0.0f;
    std::string m_TankLabel;

    bool m_IsViewportFocused = false;
    bool m_IsViewportHovered = false;
};