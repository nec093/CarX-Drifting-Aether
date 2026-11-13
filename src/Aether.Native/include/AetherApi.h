// AetherNative.dll: the C interface the managed mod (src/Aether.Managed/Native.cs) uses. Plain C, blittable types.
// Bump AETHER_API_VERSION on any change; the managed side refuses a DLL with another version.
#pragma once
#include <stdint.h>
#include "SceneContract.h"

#ifdef AETHER_NATIVE_EXPORTS
#define AETHER_API extern "C" __declspec(dllexport)
#else
#define AETHER_API extern "C" __declspec(dllimport)
#endif

#define AETHER_API_VERSION 7

// Render events for GL.IssuePluginEvent(Aether_GetRenderEventFunc(), id)
#define AETHER_EVENT_FRAME_BEGIN 1 // first thing in the frame (GPU timestamp)
#define AETHER_EVENT_FRAME_END 2   // after everything Unity draws, before Present (WaitForEndOfFrame)
#define AETHER_EVENT_SHUTDOWN 3    // releases Aether's device objects on the render thread; issue it right before Aether_Shutdown
#define AETHER_EVENT_SCENE 4       // Aether's scene renderer: after the game's cameras rendered, before its UI (right after Aether_SceneSubmitFrame)

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
// bit 0: the overlay uses the mouse, bit 1: the keyboard (0 while it is hidden)
AETHER_API int Aether_OverlayInputFlags(void);
// 1 while a benchmark that asked for it is running: the managed side switches VSync and the frame rate cap off, and back after.
AETHER_API int Aether_BenchmarkWantsUncapped(void);

// ---- the scene data contract (SceneContract.h, docs/architecture/scene-data-contract.md). Main thread only.
// Each returns 1 when the description was accepted, 0 when it was refused (logged as E_SCENE_CONTRACT).
AETHER_API int Aether_SceneContractVersion(void);
// sizeof of a contract struct (0 mesh, 1 texture, 2 material, 3 instance, 4 frame instance, 5 light, 6 frame, 7 sub mesh):
// the managed side compares them with its own layouts before it sends anything.
AETHER_API int Aether_SceneStructSize(int which);
AETHER_API int Aether_SceneUpsertMesh(const AeMeshDesc* desc, const AeSubMesh* subMeshes);
AETHER_API int Aether_SceneUpsertTexture(const AeTextureDesc* desc);
AETHER_API int Aether_SceneUpsertMaterial(const AeMaterialDesc* desc);
AETHER_API int Aether_SceneUpsertInstance(const AeInstanceDesc* desc, const AeHandle* materials);
// kind: 0 mesh, 1 texture, 2 material, 3 instance
AETHER_API void Aether_SceneDestroy(int kind, AeHandle handle);
// Every handle goes (scene unload, or the capture stops).
AETHER_API void Aether_SceneClear(void);
// The host's reflection probes (the whole list; structSize = sizeof(AeProbe)). Kept until the next call.
AETHER_API int Aether_SceneSetProbes(const AeProbe* probes, uint32_t count, uint32_t structSize);
// This frame's particles (replaces the ones of the frame before; call it before Aether_SceneSubmitFrame, with 0 batches when there are none).
AETHER_API int Aether_SceneSubmitParticles(const AeParticleBatch* batches, uint32_t batchCount, const AeParticle* particles, uint32_t particleCount, uint32_t particleSize);
AETHER_API int Aether_SceneSubmitFrame(const AeFrame* frame, const AeFrameInstance* instances, uint32_t instanceCount, const AeLight* lights, uint32_t lightCount);
// 0 hdrp (the managed side captures nothing), 1 aether, 2 compare
// A mesh's geometry for ray tracing, read back from the GPU by the caller (the two halves in any order; the tree is
// built when both have arrived). positionFormat: the DXGI_FORMAT of the position element. Return 0 when refused.
// uvMode: 0 no texture coordinates, 1 they are in this stream (uvOffset, uvFormat: where and the DXGI_FORMAT), 2 they
// are in another stream, sent with Aether_SceneMeshUvs (the tree waits for it).
AETHER_API int Aether_SceneMeshVertices(AeHandle mesh, const void* data, uint32_t bytes, uint32_t stride, uint32_t positionOffset, uint32_t positionFormat, uint32_t uvMode, uint32_t uvOffset, uint32_t uvFormat);
AETHER_API int Aether_SceneMeshUvs(AeHandle mesh, const void* data, uint32_t bytes, uint32_t stride, uint32_t uvOffset, uint32_t uvFormat);
AETHER_API int Aether_SceneMeshIndices(AeHandle mesh, const void* data, uint32_t bytes, uint32_t indexStride, const AeSubMesh* subMeshes, uint32_t subMeshCount);
// A mesh whose vertices change (skinned): call before its geometry is sent. Vertices sent again later refit its tree.
AETHER_API void Aether_SceneMeshChanging(AeHandle mesh);
// Instances of `alias` are traced as `mesh` (a skinned renderer's vertex buffers take turns: one tree serves them all).
AETHER_API void Aether_SceneMeshAlias(AeHandle alias, AeHandle mesh);
// The ray tracing tree of a handle that is not a scene mesh (see above) goes. Scene meshes: Aether_SceneDestroy does it.
AETHER_API void Aether_SceneMeshForget(AeHandle mesh);
// Every tree goes (the set of loaded scenes changed): the caller sends the geometry of what is still there again.
AETHER_API void Aether_SceneBvhReset(void);
// The game's own day / night switch as the managed side finds it: 1 night, 0 day or unknown.
AETHER_API void Aether_SetSceneNight(int night);
// The tyre smoke's motion (Renderer/Smoke.cpp): one step for `count` puffs in place, the puffs left as quads in `out`.
struct AeParticle;
AETHER_API int Aether_SmokeStep(void* puffs, int count, const void* wheels, int wheelCount, const void* bodies, int bodyCount, float dt, float time, const float* wind, float opacity, struct AeParticle* out);
AETHER_API int Aether_GetRenderMode(void);
// Experiment switches for the scene capture (Core/DevOptions.h); 0 in normal operation.
AETHER_API int Aether_DevCaptureFlags();
// One of Aether's settings as a number (booleans as 0 / 1). Returns `fallback` for a name that is not a numeric setting.
AETHER_API double Aether_SettingNumber(const char* key, double fallback);
