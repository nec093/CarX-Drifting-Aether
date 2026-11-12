cmake_minimum_required(VERSION 3.20)
project(AetherNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT: the DLL has no redistributable to install
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(AETHER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(AETHER_THIRD_PARTY "${AETHER_ROOT}/third_party")

# Unity's native plugin API headers (IUnityGraphicsD3D12.h ...). They ship with the Unity editor and are not committed.
set(UNITY_PLUGIN_API_DIR "C:/Program Files/Unity/Hub/Editor/2023.2.20f1/Editor/Data/PluginAPI" CACHE PATH "Unity editor PluginAPI folder")
if(NOT EXISTS "${UNITY_PLUGIN_API_DIR}/IUnityGraphicsD3D12.h")
	message(FATAL_ERROR "IUnityGraphicsD3D12.h not found in UNITY_PLUGIN_API_DIR (${UNITY_PLUGIN_API_DIR}). Pass -DUNITY_PLUGIN_API_DIR=<Unity editor>/Editor/Data/PluginAPI")
endif()

set(IMGUI_DIR "${AETHER_THIRD_PARTY}/imgui")
add_library(aether_imgui STATIC
	${IMGUI_DIR}/imgui.cpp
	${IMGUI_DIR}/imgui_demo.cpp
	${IMGUI_DIR}/imgui_draw.cpp
	${IMGUI_DIR}/imgui_tables.cpp
	${IMGUI_DIR}/imgui_widgets.cpp
	${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
	${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(aether_imgui PUBLIC ${IMGUI_DIR})
target_compile_definitions(aether_imgui PUBLIC
	IMGUI_DISABLE_DEMO_WINDOWS
	IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
	IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS)

add_library(aether_core STATIC
	Core/Log.cpp
	Core/Config.cpp
	Device/GpuInfo.cpp
	Resources/ResourceRegistry.cpp
	Resources/GpuResources.cpp
	Scene/SceneStore.cpp
	Renderer/RenderGraph.cpp
	Renderer/GpuTimer.cpp
	Renderer/SceneRenderer.cpp
	Renderer/Bvh.cpp
	Profiler/Profiler.cpp
	Shaders/ShaderManager.cpp
	Renderer/Renderer.cpp
	Renderer/Png.cpp
	ImGui/Overlay.cpp
	Ipc/PipeServer.cpp
	Ipc/Api.cpp
	Diagnostics.cpp
	Runtime.cpp
	Session.cpp)
target_include_directories(aether_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${AETHER_THIRD_PARTY})
target_compile_definitions(aether_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
# /EHa: structured exceptions unwind C++ frames (Session.cpp guards the frame entry points)
target_compile_options(aether_core PUBLIC /W4 /EHa /permissive- /utf-8 /wd4100 /wd4324)
target_link_libraries(aether_core PUBLIC aether_imgui d3d12 dxgi dxguid d3dcompiler windowscodecs ole32 user32 gdi32 dwmapi advapi32)

# the game DLL
add_library(AetherNative SHARED Device/UnityBridge.cpp)
target_compile_definitions(AetherNative PRIVATE AETHER_NATIVE_EXPORTS)
target_include_directories(AetherNative PRIVATE "${UNITY_PLUGIN_API_DIR}")
target_link_libraries(AetherNative PRIVATE aether_core)

# the test host: Aether on a window with its own device, no game needed
add_executable(AetherHost "${AETHER_ROOT}/tests/host/HostMain.cpp" "${AETHER_ROOT}/tests/host/ValidationScene.cpp")
target_link_libraries(AetherHost PRIVATE aether_core)
