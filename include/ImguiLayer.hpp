#pragma once
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "FrameBuffer.hpp"
#include <functional>

class ImGuiLayer {
public:
    ImGuiLayer(GLFWwindow* window, const char* glslVersion = "#version 130");
    ~ImGuiLayer();

    void Begin();
    bool RenderViewport(FrameBuffer* frameBuffer);
    void RenderUI();
    void End(GLFWwindow* window);

    std::function<void()> SwitchProjection;

    ImGuiIO& GetIO() const { return ImGui::GetIO(); }
    bool show_demo_window = false;

    bool IsViewportFocused() const { return m_IsViewportFocused; }
    bool IsViewportHovered() const { return m_IsViewportHovered; }

private:
    bool projection_mode = true;
    
    bool m_IsViewportFocused = false;
    bool m_IsViewportHovered = false;
};