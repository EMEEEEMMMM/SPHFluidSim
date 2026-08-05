cmake_minimum_required(VERSION 3.10)
project(IISPH)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0072)
  cmake_policy(SET CMP0072 NEW)
endif()
find_package(OpenGL REQUIRED)

find_package(glfw3 CONFIG REQUIRED)

set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/glad")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

include_directories(
    include
    src/ShaderProgram
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${GLFW_INCLUDE_DIRS}
    ${GLAD_DIR}/include
)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/Application.cpp
    src/FrameBuffer.cpp
    src/ShaderProgram/Shader.cpp
    src/OpenGLLayer.cpp
    src/ImguiLayer.cpp
    ${IMGUI_SOURCES}
    ${GLAD_DIR}/src/glad.c
)

target_link_libraries(${PROJECT_NAME}
    glfw
    OpenGL::GL
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME}
        gdi32
        user32
        kernel32
        psapi
        dwmapi
    )
endif()