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
	Renderer/ReflectionDenoiser.cpp
	Renderer/NrdInstance.cpp
	Renderer/LightingDenoiser.cpp
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

# NRD (NVIDIA Real-time Denoisers, v4.18.0): the reflection denoiser. Under the NVIDIA RTX SDKs License, which allows
# it only in object code: it is not committed, scripts\fetch-deps.ps1 puts it (and MathLib, ShaderMake) in build\deps.
# Its shaders are compiled with the Windows SDK's DXC (ShaderMake would download one). Without it Aether builds and
# the setting reflectionDenoiser = nrd does nothing.
set(AETHER_DEPS_DIR "${AETHER_ROOT}/build/deps" CACHE PATH "Folder with NRD, MathLib and ShaderMake (scripts/fetch-deps.ps1)")
set(AETHER_NRD_DIR "${AETHER_DEPS_DIR}/NRD-4.18.0" CACHE PATH "NRD source folder")
set(AETHER_DXC_EXE "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe" CACHE FILEPATH "DXC for NRD's and Aether's build time shaders")
if(EXISTS "${AETHER_NRD_DIR}/Include/NRD.h" AND EXISTS "${AETHER_DXC_EXE}")
	set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
	set(NRD_EMBEDS_DXIL_SHADERS ON CACHE BOOL "" FORCE)
	set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "" FORCE)
	set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
	set(SHADERMAKE_FIND_DXC OFF CACHE BOOL "" FORCE)
	set(SHADERMAKE_FIND_DXC_VK OFF CACHE BOOL "" FORCE)
	set(SHADERMAKE_FIND_SLANG OFF CACHE BOOL "" FORCE)
	set(SHADERMAKE_DXC_PATH "${AETHER_DXC_EXE}" CACHE FILEPATH "" FORCE)
	set(FETCHCONTENT_SOURCE_DIR_SHADERMAKE "${AETHER_DEPS_DIR}/ShaderMake-3f01623a76b092ac127d2debd79a672f55d77665" CACHE PATH "" FORCE)
	set(FETCHCONTENT_SOURCE_DIR_MATHLIB "${AETHER_DEPS_DIR}/MathLib-11" CACHE PATH "" FORCE)
	add_subdirectory("${AETHER_NRD_DIR}" "${CMAKE_BINARY_DIR}/nrd" EXCLUDE_FROM_ALL)
	# Aether's passes around NRD (they use NRD.hlsli): compiled now, embedded as DXIL
	set(AETHER_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
	add_custom_command(
		OUTPUT "${AETHER_GENERATED_DIR}/NrdPrepare.h" "${AETHER_GENERATED_DIR}/NrdResolve.h" "${AETHER_GENERATED_DIR}/NrdLightPrepare.h" "${AETHER_GENERATED_DIR}/NrdLightResolve.h"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${AETHER_GENERATED_DIR}"
		COMMAND "${AETHER_DXC_EXE}" -nologo -T cs_6_5 -E CSPrepare -O3 -I "${AETHER_NRD_DIR}/Shaders" "${CMAKE_CURRENT_SOURCE_DIR}/Renderer/NrdPasses.hlsl" -Fh "${AETHER_GENERATED_DIR}/NrdPrepare.h" -Vn kNrdPrepare
		COMMAND "${AETHER_DXC_EXE}" -nologo -T cs_6_5 -E CSResolve -O3 -I "${AETHER_NRD_DIR}/Shaders" "${CMAKE_CURRENT_SOURCE_DIR}/Renderer/NrdPasses.hlsl" -Fh "${AETHER_GENERATED_DIR}/NrdResolve.h" -Vn kNrdResolve
		COMMAND "${AETHER_DXC_EXE}" -nologo -T cs_6_5 -E CSLightPrepare -O3 -I "${AETHER_NRD_DIR}/Shaders" "${CMAKE_CURRENT_SOURCE_DIR}/Renderer/NrdPasses.hlsl" -Fh "${AETHER_GENERATED_DIR}/NrdLightPrepare.h" -Vn kNrdLightPrepare
		COMMAND "${AETHER_DXC_EXE}" -nologo -T cs_6_5 -E CSLightResolve -O3 -I "${AETHER_NRD_DIR}/Shaders" "${CMAKE_CURRENT_SOURCE_DIR}/Renderer/NrdPasses.hlsl" -Fh "${AETHER_GENERATED_DIR}/NrdLightResolve.h" -Vn kNrdLightResolve
		DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Renderer/NrdPasses.hlsl" "${AETHER_NRD_DIR}/Shaders/NRD.hlsli"
		COMMENT "Aether: NRD passes (DXIL)")
	add_custom_target(aether_nrd_passes DEPENDS "${AETHER_GENERATED_DIR}/NrdPrepare.h" "${AETHER_GENERATED_DIR}/NrdResolve.h" "${AETHER_GENERATED_DIR}/NrdLightPrepare.h" "${AETHER_GENERATED_DIR}/NrdLightResolve.h")
	add_dependencies(aether_core aether_nrd_passes)
	target_include_directories(aether_core PRIVATE "${AETHER_GENERATED_DIR}")
	target_compile_definitions(aether_core PUBLIC AETHER_HAS_NRD=1)
	target_link_libraries(aether_core PUBLIC NRD)
	message(STATUS "Aether: NRD ${AETHER_NRD_DIR}")
else()
	message(WARNING "NRD not found in AETHER_NRD_DIR (${AETHER_NRD_DIR}): the reflection denoiser is not built (scripts/fetch-deps.ps1)")
endif()

# the game DLL
add_library(AetherNative SHARED Device/UnityBridge.cpp Renderer/Smoke.cpp)
target_compile_definitions(AetherNative PRIVATE AETHER_NATIVE_EXPORTS)
target_include_directories(AetherNative PRIVATE "${UNITY_PLUGIN_API_DIR}")
target_link_libraries(AetherNative PRIVATE aether_core)

# the test host: Aether on a window with its own device, no game needed
add_executable(AetherHost "${AETHER_ROOT}/tests/host/HostMain.cpp" "${AETHER_ROOT}/tests/host/ValidationScene.cpp")
target_link_libraries(AetherHost PRIVATE aether_core)

# DXC for the shader model 6 shaders (DXR, compiled at run time): the Windows SDK's redistributable dxcompiler.dll and
# dxil.dll, beside the runtime (scripts\deploy.ps1 puts them in the mod's native folder). Without them Aether runs and
# the DXR option traces with the shader walk.
set(AETHER_DXC_DIR "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64" CACHE PATH "Folder with the redistributable dxcompiler.dll and dxil.dll")
if(EXISTS "${AETHER_DXC_DIR}/dxcompiler.dll" AND EXISTS "${AETHER_DXC_DIR}/dxil.dll")
	add_custom_command(TARGET AetherNative POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AETHER_DXC_DIR}/dxcompiler.dll" "${AETHER_DXC_DIR}/dxil.dll" "$<TARGET_FILE_DIR:AetherNative>")
else()
	message(WARNING "dxcompiler.dll / dxil.dll not found in AETHER_DXC_DIR (${AETHER_DXC_DIR}): DXR shaders will not compile at run time")
endif()
