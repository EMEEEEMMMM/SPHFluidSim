#include <glad/glad.h>
#include "ImguiLayer.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <iostream>
#include <cstdio>

ImGuiLayer::ImGuiLayer(GLFWwindow* window, const char* glslVersion) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);
}

ImGuiLayer::~ImGuiLayer() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::Begin() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
}

bool ImGuiLayer::RenderViewport(FrameBuffer* framebuffer) {
    bool resized = false;

    ImGui::Begin("ViewPort");
    {
        ImVec2 viewPortSize = ImGui::GetContentRegionAvail();
        
        if(viewPortSize.x * viewPortSize.y > 0 &&
            (static_cast<uint32_t>(viewPortSize.x) != framebuffer->GetWidth() ||
             static_cast<uint32_t>(viewPortSize.y) != framebuffer->GetHeight())
        )
        {
            resized = true;
            framebuffer->Resize(static_cast<uint32_t>(viewPortSize.x),
                                static_cast<uint32_t>(viewPortSize.y));
        }

        m_IsViewportFocused = ImGui::IsWindowFocused();
        m_IsViewportHovered = ImGui::IsWindowHovered();

        uint32_t textureID = framebuffer->GetColorAttachment();
        ImGui::Image(reinterpret_cast<void*>(static_cast<uintptr_t>(textureID)), 
                     viewPortSize, {0, 1}, {1, 0});

        // Overlay: FPS top-left, tank label right below it.
        char fpsBuf[64];
        snprintf(fpsBuf, sizeof(fpsBuf), "%.0f FPS", m_FPS);
        ImGui::SetCursorPos(ImVec2(8.0f, 30.0f));
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "%s", fpsBuf);
        ImGui::SetCursorPos(ImVec2(8.0f, 40.0f));
        ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), "%s", m_TankLabel.c_str());
    }
    ImGui::End();

    if (show_demo_window)
    {
        ImGui::ShowDemoWindow(&show_demo_window); 
    }

    return resized;
}

void ImGuiLayer::RenderUI() {
    ImGui::Begin("Options");
    if (ImGui::Button("Switch projection mode"))
    {
        if (SwitchProjection) {
            SwitchProjection();
        }
    }
    ImGui::Separator();
    if (OnSimulationUI) {
        OnSimulationUI();
    }
    ImGui::End();
}

void ImGuiLayer::End(GLFWwindow* window) {
    ImGui::Render();

    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }
}