cmake_minimum_required(VERSION 3.20)
project(NifMerge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
add_compile_options(/utf-8 /MP /W3 /permissive-)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)

# ---- Autodesk FBX SDK (static, /MD) ------------------------------------------------------------
set(FBXSDK_ROOT "C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.11" CACHE PATH "Autodesk FBX SDK install directory")
if(NOT EXISTS "${FBXSDK_ROOT}/include/fbxsdk.h")
    message(FATAL_ERROR "Autodesk FBX SDK not found at FBXSDK_ROOT='${FBXSDK_ROOT}'")
endif()
add_library(fbxsdk INTERFACE)
target_include_directories(fbxsdk SYSTEM INTERFACE "${FBXSDK_ROOT}/include")
set(_fbxlib "${FBXSDK_ROOT}/lib/x64")
target_link_libraries(fbxsdk INTERFACE
    "$<IF:$<CONFIG:Debug>,${_fbxlib}/debug/libfbxsdk-md.lib,${_fbxlib}/release/libfbxsdk-md.lib>"
    "$<IF:$<CONFIG:Debug>,${_fbxlib}/debug/libxml2-md.lib,${_fbxlib}/release/libxml2-md.lib>"
    "$<IF:$<CONFIG:Debug>,${_fbxlib}/debug/zlib-md.lib,${_fbxlib}/release/zlib-md.lib>"
    wininet bcrypt)

# ---- nifly (vendored) -------------------------------------------------------------------------
file(GLOB NIFLY_SOURCES CONFIGURE_DEPENDS third_party/nifly/src/*.cpp)
add_library(nifly STATIC ${NIFLY_SOURCES})
target_include_directories(nifly PUBLIC third_party/nifly/include third_party/nifly/external)
target_compile_options(nifly PRIVATE /W0)

# ---- Dear ImGui (vendored) --------------------------------------------------------------------
add_library(imgui STATIC
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_demo.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)

# ---- Core library -----------------------------------------------------------------------------
add_library(nifmerge_core STATIC
    src/core/NifIO.cpp src/core/Merge.cpp src/core/FbxExport.cpp src/core/SidecarExport.cpp
    src/core/Pipeline.cpp src/core/TextUtil.cpp)
target_include_directories(nifmerge_core PUBLIC src)
target_link_libraries(nifmerge_core PUBLIC nifly fbxsdk)

# ---- Application (GUI with no arguments, CLI with arguments) ----------------------------------
add_executable(NifMerge src/app/main.cpp src/app/Gui.cpp)
target_link_libraries(NifMerge PRIVATE nifmerge_core imgui shell32 ole32)

# ---- Tests ------------------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
