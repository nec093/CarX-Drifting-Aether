// Aether inside Unity: the Unity native plugin interface (IUnityGraphicsD3D12v7) as the HostGfx, the render events,
// and the DLL's C exports. See docs/architecture/dx12-integration.md for why each call is made where it is.
#include "../Renderer/Bvh.h"
#include "../Renderer/SceneRenderer.h"
#include "../Core/DevOptions.h"
#include "../Core/Common.h"
#include "../Core/Log.h"
#include "../ImGui/Overlay.h"
#include "../Profiler/Profiler.h"
#include "../Core/Config.h"
#include "../Renderer/Renderer.h"
#include "../Scene/SceneStore.h"
#include "../Runtime.h"
#include "../Session.h"
#include "../include/AetherApi.h"
#include "Host.h"

#include <IUnityGraphics.h>
#include <IUnityGraphicsD3D12.h>
#include <IUnityInterface.h>

using namespace aether;

namespace {

IUnityInterfaces* g_interfaces = nullptr;
IUnityGraphics* g_graphics = nullptr;
IUnityGraphicsD3D12v7* g_d3d12 = nullptr; // render thread only
std::atomic<bool> g_hasInterfaces{false}, g_hasD3D12{false};
HostGfx g_host;
bool g_hostReady = false;                  // render thread
ComPtr<ID3D12Fence> g_idleFence;           // Aether's own fence on Unity's queue (for waitIdle)
UINT64 g_idleValue = 0;
HANDLE g_idleEvent = nullptr;

static_assert(sizeof(HostResourceState) == sizeof(UnityGraphicsD3D12ResourceState), "HostResourceState must match Unity's struct");
static_assert(offsetof(HostResourceState, expected) == offsetof(UnityGraphicsD3D12ResourceState, expected), "HostResourceState must match Unity's struct");
static_assert(offsetof(HostResourceState, current) == offsetof(UnityGraphicsD3D12ResourceState, current), "HostResourceState must match Unity's struct");

void ConfigureEvents();

// Render thread. Fills g_host from Unity's interface the first time (and again if the device changed).
bool EnsureHost() {
	if (!g_d3d12) {
		return false;
	}
	ID3D12Device* device = g_d3d12->GetDevice();
	ID3D12CommandQueue* queue = g_d3d12->GetCommandQueue();
	if (!device || !queue) {
		return false;
	}
	if (g_hostReady && g_host.device == device && g_host.queue == queue) {
		return true;
	}
	g_host = HostGfx();
	g_host.name = "Unity";
	g_host.device = device;
	g_host.queue = queue;
	g_host.swapChain = [] { return g_d3d12 ? g_d3d12->GetSwapChain() : nullptr; };
	g_host.execute = [](ID3D12GraphicsCommandList* list, int count, HostResourceState* states) -> UINT64 {
		return g_d3d12->ExecuteCommandList(list, count, reinterpret_cast<UnityGraphicsD3D12ResourceState*>(states));
	};
	g_host.completedFenceValue = []() -> UINT64 {
		ID3D12Fence* fence = g_d3d12 ? g_d3d12->GetFrameFence() : nullptr;
		return fence ? fence->GetCompletedValue() : 0;
	};
	g_host.nextFenceValue = []() -> UINT64 { return g_d3d12 ? g_d3d12->GetNextFrameFenceValue() : 0; };
	g_idleFence.Reset();
	g_idleValue = 0;
	if (!g_idleEvent) {
		g_idleEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	}
	if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_idleFence)))) {
		g_idleFence->SetName(L"Aether.IdleFence");
		g_host.waitIdle = [](uint32_t timeoutMs) -> bool {
			if (!g_idleFence || !g_host.queue || !g_idleEvent) {
				return false;
			}
			const UINT64 value = ++g_idleValue;
			if (FAILED(g_host.queue->Signal(g_idleFence.Get(), value))) {
				return false;
			}
			if (g_idleFence->GetCompletedValue() >= value) {
				return true;
			}
			if (FAILED(g_idleFence->SetEventOnCompletion(value, g_idleEvent))) {
				return false;
			}
			return WaitForSingleObject(g_idleEvent, timeoutMs) == WAIT_OBJECT_0;
		};
	}
	g_hostReady = true;
	AE_LOG(Info, "Unity", "Unity's D3D12 device and command queue acquired (IUnityGraphicsD3D12v7)");
	if (DevOptions::Get().source != "defaults") {
		ConfigureEvents(); // the experiment switches were not loaded yet when the device was initialized
	}
	return true;
}

void UNITY_INTERFACE_API OnRenderEvent(int eventId) {
	if (!session::Started() || !EnsureHost()) {
		return;
	}
	switch (eventId) {
	case AETHER_EVENT_FRAME_BEGIN: session::FrameBegin(g_host); break;
	case AETHER_EVENT_FRAME_END:
		if (DevOptions::Get().sleepMs > 0) {
			Sleep(DWORD(DevOptions::Get().sleepMs)); // experiment: the render thread is only held up
		}
		session::FrameEnd(g_host);
		break;
	case AETHER_EVENT_SCENE: session::SceneFrame(g_host); break;
	case AETHER_EVENT_SHUTDOWN:
		session::ReleaseOnRenderThread(g_host, 2000);
		g_idleFence.Reset();
		g_hostReady = false;
		break;
	default: break;
	}
}

void ConfigureEvents() {
	if (!g_d3d12) {
		return;
	}
	// Frame begin: a timestamp list goes through ExecuteCommandList. That is work submitted to Unity's queue, which
	// Unity only permits in an event configured with queue access ("DontCare" = "no work must be submitted"); its
	// worker threads are kept off the queue meanwhile. Nothing of Unity's needs to be flushed first.
	UnityD3D12PluginEventConfig begin = {};
	begin.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_Allow;
	begin.flags = kUnityD3D12EventConfigFlag_SyncWorkerThreads;
	const DevOptions& dev = DevOptions::Get();
	if (dev.beginFlags >= 0) begin.flags = UINT32(dev.beginFlags);
	begin.ensureActiveRenderTextureIsBound = false;
	g_d3d12->ConfigureEvent(AETHER_EVENT_FRAME_BEGIN, &begin);
	// Frame end / shutdown: everything Unity recorded so far must be submitted before Aether's list (the overlay goes on
	// top of the UI; at shutdown the GPU is then waited on), and Unity's workers stay off the queue meanwhile (the Dear
	// ImGui backend uploads its font atlas on the queue directly). Aether does not touch Unity's own command list state.
	UnityD3D12PluginEventConfig end = {};
	end.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_Allow;
	end.flags = kUnityD3D12EventConfigFlag_FlushCommandBuffers | kUnityD3D12EventConfigFlag_SyncWorkerThreads;
	end.ensureActiveRenderTextureIsBound = false;
	UnityD3D12PluginEventConfig scene = end;
	g_d3d12->ConfigureEvent(AETHER_EVENT_SHUTDOWN, &end);
	if (dev.endFlags >= 0) end.flags = UINT32(dev.endFlags);
	if (dev.sceneFlags >= 0) scene.flags = UINT32(dev.sceneFlags);
	g_d3d12->ConfigureEvent(AETHER_EVENT_FRAME_END, &end);
	// The scene event: the host's cameras must be submitted before Aether's image goes onto the back buffer (it
	// replaces theirs), and Aether's list before the host's UI. Same needs as the frame end.
	g_d3d12->ConfigureEvent(AETHER_EVENT_SCENE, &scene);
	AE_LOG(Info, "Unity", "Plugin events configured: begin flags %u, end flags %u, scene flags %u (queue access allowed in all three)", begin.flags, end.flags, scene.flags);
}

void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType type) {
	switch (type) {
	case kUnityGfxDeviceEventInitialize: {
		UnityGfxRenderer renderer = g_graphics ? g_graphics->GetRenderer() : kUnityGfxRendererNull;
		if (renderer == kUnityGfxRendererD3D12 && g_interfaces) {
			g_d3d12 = g_interfaces->Get<IUnityGraphicsD3D12v7>();
			g_hasD3D12.store(g_d3d12 != nullptr);
			if (g_d3d12) {
				ConfigureEvents();
				AE_LOG(Info, "Unity", "Graphics device initialized: Direct3D 12, IUnityGraphicsD3D12v7 available");
			} else {
				AE_LOG_CODE(Error, "Unity", "E_UNITY_INTERFACE", "Direct3D 12 is active but IUnityGraphicsD3D12v7 is not available in this Unity version");
			}
		} else {
			g_hasD3D12.store(false);
			AE_LOG_CODE(Error, "Unity", "E_NOT_D3D12", "Unity's renderer is not Direct3D 12 (UnityGfxRenderer %d): Aether stays off. Start the game with -force-d3d12.", int(renderer));
		}
		break;
	}
	case kUnityGfxDeviceEventShutdown:
		// Unity is destroying the device (render thread, GPU flushed by Unity): last chance to release properly
		if (g_hostReady) {
			session::ReleaseOnRenderThread(g_host, 500);
		}
		g_idleFence.Reset();
		g_hostReady = false;
		g_d3d12 = nullptr;
		g_hasD3D12.store(false);
		break;
	default: break;
	}
}

} // namespace

// ---- Unity's plugin entry points (called by Unity when it loads the DLL through its plugin loader)

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces) {
	g_interfaces = unityInterfaces;
	g_graphics = unityInterfaces ? unityInterfaces->Get<IUnityGraphics>() : nullptr;
	g_hasInterfaces.store(g_graphics != nullptr);
	if (g_graphics) {
		g_graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
		// the device already exists when a plugin is loaded late: run the initialize step by hand
		OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
	}
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload() {
	if (g_graphics) {
		g_graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
	}
	g_graphics = nullptr;
	g_interfaces = nullptr;
	g_d3d12 = nullptr;
	g_hasInterfaces.store(false);
	g_hasD3D12.store(false);
}

// ---- the managed side's interface

AETHER_API int Aether_GetApiVersion(void) {
	return AETHER_API_VERSION;
}

AETHER_API int Aether_DevCaptureFlags() {
	return DevOptions::Get().captureFlags;
}

AETHER_API int Aether_Initialize(const wchar_t* dataDir, const wchar_t* configDir) {
	if (!dataDir || !configDir) {
		return 0;
	}
	session::Start(dataDir, configDir);
	AE_LOG(Info, "Unity", "Unity interfaces: %s; Direct3D 12 interface: %s", g_hasInterfaces.load() ? "yes" : "no (UnityPluginLoad was not called)", g_hasD3D12.load() ? "yes" : "no");
	return 1;
}

AETHER_API void Aether_Shutdown(void) {
	if (!session::Started()) {
		return;
	}
	if (Renderer::Get().Ready()) {
		// the managed side issued AETHER_EVENT_SHUTDOWN right before this call: let the render thread get to it
		if (!session::WaitReleased(1500)) {
			AE_LOG(Warning, "Unity", "The render thread did not run the shutdown event in time");
		}
	}
	session::Stop();
}

AETHER_API int Aether_HasUnityInterfaces(void) { return g_hasInterfaces.load() ? 1 : 0; }
AETHER_API int Aether_HasD3D12(void) { return g_hasD3D12.load() ? 1 : 0; }

AETHER_API int Aether_GetState(void) {
	if (!session::Started()) {
		return AETHER_STATE_NOT_INITIALIZED;
	}
	if (session::Dead() || Renderer::Get().Failed()) {
		return AETHER_STATE_FAILED;
	}
	return Renderer::Get().Ready() ? AETHER_STATE_READY : AETHER_STATE_WAITING;
}

AETHER_API void* Aether_GetRenderEventFunc(void) {
	return reinterpret_cast<void*>(&OnRenderEvent);
}

AETHER_API void Aether_Update(void) {
	session::MainThreadUpdate();
}

AETHER_API void Aether_SubmitUnityStats(const double* values, int count) {
	Profiler::Get().SubmitUnityStats(values, count);
}

AETHER_API uint32_t Aether_TakeManagedRequests(void) {
	return Runtime::Get().TakeManagedRequests();
}

AETHER_API void Aether_SetManagedState(const char* key, const char* jsonUtf8) {
	if (key && jsonUtf8) {
		try {
			Runtime::Get().SetManagedState(key, jsonUtf8);
		} catch (const std::exception& e) {
			AE_LOG_CODE(Error, "Unity", "E_MANAGED_STATE", "Could not store '%s': %s", key, e.what());
		}
	}
}

AETHER_API void Aether_Log(int severity, const char* component, const char* messageUtf8) {
	if (severity < 0 || severity > 4 || !messageUtf8) {
		return;
	}
	Log::Get().Write(Severity(severity), component ? component : "Managed", "", messageUtf8);
}

AETHER_API int Aether_OverlayVisible(void) { return Overlay::Get().Visible() ? 1 : 0; }
AETHER_API void Aether_SetOverlayVisible(int visible) { Overlay::Get().SetVisible(visible != 0); }
AETHER_API int Aether_OverlayWantsInput(void) { return Overlay::Get().WantsInput() ? 1 : 0; }
AETHER_API int Aether_OverlayInputFlags(void) { return Overlay::Get().InputFlags(); }
AETHER_API int Aether_BenchmarkWantsUncapped(void) { return Profiler::Get().BenchmarkWantsUncapped() ? 1 : 0; }

// ---- the scene data contract
AETHER_API double Aether_SettingNumber(const char* key, double fallback) {
	if (!key) {
		return fallback;
	}
	const json all = Settings::Get().ToJson();
	auto it = all.find(key);
	if (it == all.end()) {
		return fallback;
	}
	return it->is_boolean() ? (it->get<bool>() ? 1.0 : 0.0) : it->is_number() ? it->get<double>() : fallback;
}
AETHER_API int Aether_SceneContractVersion(void) { return AE_SCENE_CONTRACT_VERSION; }
AETHER_API int Aether_SceneStructSize(int which) {
	static const int sizes[] = {int(sizeof(AeMeshDesc)), int(sizeof(AeTextureDesc)), int(sizeof(AeMaterialDesc)), int(sizeof(AeInstanceDesc)), int(sizeof(AeFrameInstance)), int(sizeof(AeLight)), int(sizeof(AeFrame)), int(sizeof(AeSubMesh))};
	return which >= 0 && which < int(_countof(sizes)) ? sizes[which] : 0;
}
AETHER_API int Aether_SceneUpsertMesh(const AeMeshDesc* desc, const AeSubMesh* subMeshes) { return SceneStore::Get().UpsertMesh(desc, subMeshes) ? 1 : 0; }
AETHER_API int Aether_SceneUpsertTexture(const AeTextureDesc* desc) { return SceneStore::Get().UpsertTexture(desc) ? 1 : 0; }
AETHER_API int Aether_SceneUpsertMaterial(const AeMaterialDesc* desc) { return SceneStore::Get().UpsertMaterial(desc) ? 1 : 0; }
AETHER_API int Aether_SceneUpsertInstance(const AeInstanceDesc* desc, const AeHandle* materials) { return SceneStore::Get().UpsertInstance(desc, materials) ? 1 : 0; }
AETHER_API void Aether_SceneDestroy(int kind, AeHandle handle) {
	SceneStore::Get().Destroy(kind, handle);
	if (kind == 0) {
		SceneBvh::Get().Forget(handle); // (a mesh: its ray tracing tree goes with it)
	}
}
AETHER_API void Aether_SetSceneNight(int night) { SceneRenderer::Get().SetSceneNight(night != 0); }
AETHER_API void Aether_SceneMeshChanging(AeHandle mesh) { SceneBvh::Get().MarkChanging(mesh); }
AETHER_API void Aether_SceneMeshAlias(AeHandle alias, AeHandle mesh) { SceneBvh::Get().Alias(alias, mesh); }
AETHER_API void Aether_SceneMeshForget(AeHandle mesh) { SceneBvh::Get().Forget(mesh); }
AETHER_API void Aether_SceneClear(void) {
	SceneStore::Get().Clear();
	SceneBvh::Get().Clear();
}
AETHER_API int Aether_SceneMeshVertices(AeHandle mesh, const void* data, uint32_t bytes, uint32_t stride, uint32_t positionOffset, uint32_t positionFormat, uint32_t uvMode, uint32_t uvOffset, uint32_t uvFormat) {
	return SceneBvh::Get().SubmitVertices(mesh, data, bytes, stride, positionOffset, positionFormat, uvMode, uvOffset, uvFormat) ? 1 : 0;
}
AETHER_API int Aether_SceneMeshUvs(AeHandle mesh, const void* data, uint32_t bytes, uint32_t stride, uint32_t uvOffset, uint32_t uvFormat) {
	return SceneBvh::Get().SubmitUvs(mesh, data, bytes, stride, uvOffset, uvFormat) ? 1 : 0;
}
AETHER_API int Aether_SceneMeshIndices(AeHandle mesh, const void* data, uint32_t bytes, uint32_t indexStride, const AeSubMesh* subMeshes, uint32_t subMeshCount) {
	return SceneBvh::Get().SubmitIndices(mesh, data, bytes, indexStride, subMeshes, subMeshCount) ? 1 : 0;
}
AETHER_API void Aether_SceneBvhReset(void) { SceneBvh::Get().Clear(); }
AETHER_API int Aether_SceneSetProbes(const AeProbe* probes, uint32_t count, uint32_t structSize) {
	return SceneStore::Get().SetProbes(probes, count, structSize) ? 1 : 0;
}
AETHER_API int Aether_SceneSubmitParticles(const AeParticleBatch* batches, uint32_t batchCount, const AeParticle* particles, uint32_t particleCount, uint32_t particleSize) {
	return SceneStore::Get().SubmitParticles(batches, batchCount, particles, particleCount, particleSize) ? 1 : 0;
}
AETHER_API int Aether_SceneSubmitFrame(const AeFrame* frame, const AeFrameInstance* instances, uint32_t instanceCount, const AeLight* lights, uint32_t lightCount) {
	return SceneStore::Get().SubmitFrame(frame, instances, instanceCount, lights, lightCount) ? 1 : 0;
}
AETHER_API int Aether_GetRenderMode(void) {
	const std::string mode = Settings::Get().Snapshot().renderMode;
	return mode == "aether" ? 1 : mode == "compare" ? 2 : mode == "standalone" ? 3 : 0;
}
