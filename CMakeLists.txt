cmake_minimum_required(VERSION 3.10)
project(IISPH)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0072)
  cmake_policy(SET CMP0072 NEW)
endif()
find_package(OpenGL REQUIRED)
find_package(OpenMP REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

# OpenCL for the GPU surface reconstruction (Akinci et al. 2012 GPU pipeline).
# Prefer CMake's FindOpenCL; fall back to the CUDA toolkit's shipped headers +
# import library (NVIDIA OpenCL.dll comes with the driver).
find_package(OpenCL)
if(NOT OpenCL_FOUND)
    set(_cuda_root "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.1")
    find_path(OpenCL_INCLUDE_DIR CL/cl.h PATHS "${_cuda_root}/include" NO_DEFAULT_PATH)
    find_library(OpenCL_LIBRARY NAMES OpenCL.lib OpenCL PATHS "${_cuda_root}/lib/x64" NO_DEFAULT_PATH)
    if(OpenCL_INCLUDE_DIR AND OpenCL_LIBRARY)
        set(OpenCL_FOUND TRUE)
        set(OpenCL_INCLUDE_DIRS ${OpenCL_INCLUDE_DIR})
        set(OpenCL_LIBRARIES ${OpenCL_LIBRARY})
    endif()
endif()
if(OpenCL_FOUND)
    message(STATUS "OpenCL found: ${OpenCL_LIBRARIES}")
else()
    message(WARNING "OpenCL not found — GPU surface reconstruction disabled (CPU path only)")
endif()

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
    ${OpenCL_INCLUDE_DIRS}
)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/Application.cpp
    src/FrameBuffer.cpp
    src/ShaderProgram/Shader.cpp
    src/OpenGLLayer.cpp
    src/ImguiLayer.cpp
    src/FluidSolver.cpp
    src/SurfaceReconstructor.cpp
    src/SurfaceReconstructorCL.cpp
    src/HeadlessRunner.cpp
    ${IMGUI_SOURCES}
    ${GLAD_DIR}/src/glad.c
)

target_link_libraries(${PROJECT_NAME}
    glfw
    OpenGL::GL
    OpenMP::OpenMP_CXX
    -fopenmp
    ${OpenCL_LIBRARIES}
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

target_compile_options(${PROJECT_NAME} PRIVATE -fopenmp)

# --- headless verification tests (no GUI) -----------------------------------
function(add_headless_test name)
    add_executable(${name} tests/${name}.cpp src/FluidSolver.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(${name} PRIVATE OpenMP::OpenMP_CXX)
    target_compile_options(${name} PRIVATE -fopenmp)
endfunction()
add_headless_test(test_solver)
add_headless_test(test_inflow)
add_headless_test(test_drop)
add_headless_test(test_boundary)
add_headless_test(verify_hydrostatic)
add_headless_test(benchmark)
add_headless_test(surface_reconstruct)
# the surface test also needs the reconstructor
target_sources(surface_reconstruct PRIVATE src/SurfaceReconstructor.cpp src/SurfaceReconstructorCL.cpp)
target_link_libraries(surface_reconstruct PRIVATE ${OpenCL_LIBRARIES})

# ctest integration: `ctest --test-dir build` runs the numerical regressions.
# (Previously there were no add_test() calls at all, so ctest reported
# "No tests were found" and nothing could fail the suite.)
enable_testing()
add_test(NAME solver      COMMAND test_solver)
add_test(NAME inflow      COMMAND test_inflow)
add_test(NAME drop        COMMAND test_drop)
add_test(NAME boundary    COMMAND test_boundary)
add_test(NAME hydrostatic COMMAND verify_hydrostatic 200)
add_test(NAME surface     COMMAND surface_reconstruct)
# DFSPH path: separate case because the warm start and the divergence solver
# are only exercised with solver == 1 (and the test asserts the iteration count).
add_test(NAME dfsph       COMMAND test_solver 120 0.003 0 0.1 DFSPH)
