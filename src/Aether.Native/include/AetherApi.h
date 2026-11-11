// AetherNative.dll: the C interface the managed mod (src/Aether.Managed/Native.cs) uses. Plain C, blittable types.
// Bump AETHER_API_VERSION on any change; the managed side refuses a DLL with another version.
#pragma once
#include <stdint.h>

#ifdef AETHER_NATIVE_EXPORTS
#define AETHER_API extern "C" __declspec(dllexport)
#else
#define AETHER_API extern "C" __declspec(dllimport)
#endif

#define AETHER_API_VERSION 1

// Render events for GL.IssuePluginEvent(Aether_GetRenderEventFunc(), id)
#define AETHER_EVENT_FRAME_BEGIN 1 // first thing in the frame (GPU timestamp)
#define AETHER_EVENT_FRAME_END 2   // after everything Unity draws, before Present (WaitForEndOfFrame)
#define AETHER_EVENT_SHUTDOWN 3    // releases Aether's device objects on the render thread; issue it right before Aether_Shutdown

// Aether_GetState
#define AETHER_STATE_NOT_INITIALIZED 0
#define AETHER_STATE_WAITING 1 // initialized, no frame rendered yet
#define AETHER_STATE_READY 2
#define AETHER_STATE_FAILED 3  // stopped after an error (the game keeps running)

AETHER_API int Aether_GetApiVersion(void);
// dataDir: shaders/, logs/, captures/, benchmarks/ live here. configDir: aether.json. Main thread. Returns 1 on success.
AETHER_API int Aether_Initialize(const wchar_t* dataDir, const wchar_t* configDir);
// Main thread. Waits (briefly) for AETHER_EVENT_SHUTDOWN to have run; stops the IPC server and the worker.
AETHER_API void Aether_Shutdown(void);
// 1 once Unity has called UnityPluginLoad on this DLL (the D3D12 interfaces are reachable).
AETHER_API int Aether_HasUnityInterfaces(void);
// 1 when Unity's renderer is Direct3D 12 and IUnityGraphicsD3D12v7 is available.
AETHER_API int Aether_HasD3D12(void);
AETHER_API int Aether_GetState(void);
AETHER_API void* Aether_GetRenderEventFunc(void);
// Main thread, every frame: attaches the overlay's input to the game window once it is known.
AETHER_API void Aether_Update(void);
// Main thread, every frame: Unity's numbers in the order of UnityStat (Profiler/Profiler.h). -1 = not available.
AETHER_API void Aether_SubmitUnityStats(const double* values, int count);
// Main thread, every frame: bit mask of ManagedRequest (Runtime.h) the managed side should publish now.
AETHER_API uint32_t Aether_TakeManagedRequests(void);
// Publishes a JSON document under a key ("managed", "scene", "materials", "renderTextures"). UTF-8.
AETHER_API void Aether_SetManagedState(const char* key, const char* jsonUtf8);
// severity: 0 debug, 1 info, 2 warning, 3 error, 4 fatal
AETHER_API void Aether_Log(int severity, const char* component, const char* messageUtf8);
AETHER_API int Aether_OverlayVisible(void);
AETHER_API void Aether_SetOverlayVisible(int visible);
// 1 while the overlay uses the mouse or the keyboard (the game should ignore its input meanwhile).
AETHER_API int Aether_OverlayWantsInput(void);
