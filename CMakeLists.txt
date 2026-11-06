cmake_minimum_required(VERSION 3.16)
project(SPHFluidSim)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(POLICY CMP0072)
  cmake_policy(SET CMP0072 NEW)
endif()

if(NOT DEFINED CMAKE_PREFIX_PATH AND NOT DEFINED Qt6_DIR)
  foreach(_qt_candidate "D:/msys64/ucrt64" "C:/msys64/ucrt64"
                        "D:/Qt/6.12.0/mingw_64" "D:/Qt/6.9.0/mingw_64")
    if(EXISTS "${_qt_candidate}/lib/cmake/Qt6")
      set(CMAKE_PREFIX_PATH "${_qt_candidate}" CACHE PATH "Qt 6 prefix")
      break()
    endif()
  endforeach()
endif()

find_package(OpenGL REQUIRED)
find_package(OpenMP REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Quick QuickControls2 OpenGL OpenGLWidgets)

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

find_package(assimp CONFIG QUIET)
if(TARGET assimp::assimp)
    set(ASSIMP_TARGET assimp::assimp)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ASSIMP REQUIRED IMPORTED_TARGET assimp)
    set(ASSIMP_TARGET PkgConfig::ASSIMP)
endif()
message(STATUS "Assimp: ${ASSIMP_TARGET}")

set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/glad")
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderProgram")

add_library(sph_core STATIC
    src/FluidSolver.cpp
    src/FrameBuffer.cpp
    src/ShaderProgram/Shader.cpp
    src/OpenGLLayer.cpp
    src/SurfaceReconstructor.cpp
    src/SurfaceReconstructorCL.cpp
    src/Render/CameraRig.cpp
    src/Render/GlView.cpp
    ${GLAD_DIR}/src/glad.c
)
target_include_directories(sph_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderProgram
    ${GLAD_DIR}/include
    ${OpenCL_INCLUDE_DIRS}
)
target_link_libraries(sph_core PUBLIC
    OpenGL::GL
    OpenMP::OpenMP_CXX
    ${OpenCL_LIBRARIES}
)
target_compile_options(sph_core PRIVATE -fopenmp)

add_library(sph_session STATIC
    include/Model.hpp
    include/Session/SimulationSession.hpp
    src/Model.cpp
    src/Session/SimulationSession.cpp
)
target_link_libraries(sph_session PUBLIC
    sph_core
    Qt6::Core
    Qt6::Widgets
    ${ASSIMP_TARGET})

add_library(sph_theme STATIC
    include/Ui/Theme.hpp
    src/Ui/Theme.cpp
)
target_include_directories(sph_theme PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sph_theme PUBLIC Qt6::Core Qt6::Gui)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/HeadlessRunner.cpp
    include/Render/QtOffscreenContext.hpp
    src/Render/QtOffscreenContext.cpp
    src/Ui/Icons.cpp
    include/Ui/Icons.hpp
    include/Ui/Widgets/Controls.hpp
    include/Ui/Widgets/MainWindow.hpp
    include/Ui/Widgets/Panels.hpp
    include/Ui/Widgets/RenderDialogs.hpp
    include/Ui/Widgets/Viewport.hpp
    src/Ui/Widgets/Controls.cpp
    src/Ui/Widgets/MainWindow.cpp
    src/Ui/Widgets/Panels.cpp
    src/Ui/Widgets/RenderDialogs.cpp
    src/Ui/Widgets/Viewport.cpp
    include/Ui/Qml/CaptureHelper.hpp
    include/Ui/Qml/IconItem.hpp
    include/Ui/Qml/QmlFrontend.hpp
    include/Ui/Qml/ViewportItem.hpp
    src/Ui/Qml/CaptureHelper.cpp
    src/Ui/Qml/IconItem.cpp
    src/Ui/Qml/QmlFrontend.cpp
    src/Ui/Qml/ViewportItem.cpp
    include/Render/RenderJob.hpp
    src/Render/RenderJob.cpp
    qml/qml.qrc
)
target_link_libraries(${PROJECT_NAME} PRIVATE
    sph_session
    sph_theme
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::OpenGL
    Qt6::OpenGLWidgets
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        gdi32
        user32
        kernel32
        psapi
        dwmapi
    )
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHADER_DIR}/vertex_shader.vert" "${SHADER_DIR}/fragment_shader.frag"
            "${SHADER_DIR}/surface_vertex.glsl" "${SHADER_DIR}/surface_fragment.glsl"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
    COMMENT "Copying shaders next to the executable"
)

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
add_headless_test(test_timeline)
add_headless_test(verify_hydrostatic)
add_headless_test(benchmark)
add_headless_test(surface_reconstruct)

add_executable(test_model_import tests/test_model_import.cpp src/Model.cpp)
target_include_directories(test_model_import PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_model_import PRIVATE Qt6::Core ${ASSIMP_TARGET})

add_executable(test_params_json tests/test_params_json.cpp)
target_link_libraries(test_params_json PRIVATE sph_session Qt6::Core Qt6::Widgets)

target_sources(surface_reconstruct PRIVATE src/SurfaceReconstructor.cpp src/SurfaceReconstructorCL.cpp)
target_include_directories(surface_reconstruct PRIVATE ${OpenCL_INCLUDE_DIRS})
target_link_libraries(surface_reconstruct PRIVATE ${OpenCL_LIBRARIES})

enable_testing()
add_test(NAME solver      COMMAND test_solver)
add_test(NAME inflow      COMMAND test_inflow)
add_test(NAME drop        COMMAND test_drop)
add_test(NAME boundary    COMMAND test_boundary)
add_test(NAME timeline    COMMAND test_timeline)
add_test(NAME hydrostatic COMMAND verify_hydrostatic 200)
add_test(NAME surface     COMMAND surface_reconstruct)
add_test(NAME dfsph       COMMAND test_solver 120 0.003 0 0.1 DFSPH)
add_test(NAME model_import COMMAND test_model_import)
add_test(NAME params_json COMMAND test_params_json)
