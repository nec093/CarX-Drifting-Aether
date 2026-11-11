#include "Diagnostics.h"
#include "Core/Config.h"
#include "Core/Log.h"
#include "Device/GpuInfo.h"
#include "ImGui/Overlay.h"
#include "Profiler/Profiler.h"
#include "Renderer/Renderer.h"
#include "Resources/ResourceRegistry.h"
#include "Runtime.h"
#include "Shaders/ShaderManager.h"

namespace aether {

const char* const kAetherVersion = "0.1.0";

std::wstring NewCapturePath(const wchar_t* prefix, const wchar_t* extension) {
	static std::atomic<uint32_t> counter{0};
	SYSTEMTIME t;
	GetLocalTime(&t);
	wchar_t name[128];
	swprintf_s(name, L"%s-%04u%02u%02u-%02u%02u%02u-%u.%s", prefix, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, counter.fetch_add(1), extension);
	return Runtime::Get().Subdir(L"captures") + L"\\" + name;
}

json BuildStatus() {
	Runtime& rt = Runtime::Get();
	json j;
	j["aetherVersion"] = kAetherVersion;
	j["phase"] = 1;
	j["processId"] = GetCurrentProcessId();
	j["renderer"] = Renderer::Get().State();
	j["gpu"] = GpuInfo::Get().Brief();
	j["deviceState"] = GpuInfo::Get().DeviceState();
	j["overlayVisible"] = Overlay::Get().Visible();
	j["frameCount"] = Profiler::Get().FrameCount();
	j["logErrorCount"] = Log::Get().ErrorCount();
	j["shaderErrorCount"] = ShaderManager::Get().ErrorCount();
	j["aetherResourceCount"] = ResourceRegistry::Get().Count();
	j["aetherResourceBytes"] = ResourceRegistry::Get().TotalBytes();
	j["previousSessionEndedUncleanly"] = rt.PreviousSessionCrashed();
	j["mcpControl"] = Settings::Get().Snapshot().mcpControl;
	double age = -1;
	json managed = rt.ManagedState("managed", &age);
	j["managed"] = managed; // what the C# mod reported about itself (KSL, Unity, graphics API, HDRP)
	j["managedAgeSeconds"] = age;
	return j;
}

json BuildVramReport() {
	json j;
	VideoMemory vm = GpuInfo::Get().QueryVideoMemory();
	ResourceRegistry& reg = ResourceRegistry::Get();
	Profiler& prof = Profiler::Get();
	const uint64_t aetherGpu = reg.GpuBytes();

	if (vm.valid) {
		j["process"] = {
			{"source", "IDXGIAdapter3::QueryVideoMemoryInfo (this process, all renderers)"},
			{"dedicatedUsageBytes", vm.localUsage},
			{"dedicatedBudgetBytes", vm.localBudget},
			{"dedicatedUsageOfBudget", vm.localBudget ? double(vm.localUsage) / double(vm.localBudget) : 0.0},
			{"sharedUsageBytes", vm.nonLocalUsage},
			{"sharedBudgetBytes", vm.nonLocalBudget},
		};
	} else {
		j["process"] = {{"available", false}, {"reason", "the adapter is not known yet (no frame rendered) or the query failed"}};
	}
	j["adapterDedicatedBytes"] = GpuInfo::Get().DedicatedVideoMemory();

	json summary = reg.Summary();
	j["aether"] = {
		{"source", "Aether's resource registry (every resource Aether creates)"},
		{"gpuBytes", aetherGpu},
		{"systemMemoryBytes", reg.TotalBytes() - aetherGpu}, // upload / readback heaps
		{"textureBytes", reg.CategoryBytes(ResourceCategory::Texture)},
		{"bufferBytes", reg.CategoryBytes(ResourceCategory::Buffer)},
		{"renderTargetBytes", reg.CategoryBytes(ResourceCategory::RenderTarget) + reg.CategoryBytes(ResourceCategory::DepthBuffer)},
		{"shadowMapBytes", reg.CategoryBytes(ResourceCategory::ShadowMap)},
		{"reflectionBytes", reg.CategoryBytes(ResourceCategory::Reflection)},
		{"historyBytes", reg.CategoryBytes(ResourceCategory::History)},
		{"resourceCount", summary["resourceCount"]},
		{"categories", summary["categories"]},
		{"allocations", summary["allocations"]},
		{"releases", summary["releases"]},
		{"note", "The overlay's own buffers and font atlas are owned by the Dear ImGui backend and are not in the registry (a few MB while the overlay is open)."},
	};

	// what Unity reports about its own side (HDRP's render targets, the game's textures)
	json unity = json::object();
	auto add = [&](const char* name, int index) {
		double v = prof.UnityValue(index);
		if (v >= 0) {
			unity[name] = uint64_t(v);
		}
	};
	add("renderTexturesBytes", kUnityRenderTexturesBytes);
	add("renderTexturesCount", kUnityRenderTexturesCount);
	add("usedTexturesBytes", kUnityUsedTexturesBytes);
	add("usedTexturesCount", kUnityUsedTexturesCount);
	add("usedBuffersBytes", kUnityUsedBuffersBytes);
	add("usedBuffersCount", kUnityUsedBuffersCount);
	add("textureStreamingCurrentBytes", kUnityTextureStreamingCurrentBytes);
	add("textureTotalBytes", kUnityTextureTotalBytes);
	add("textureTargetBytes", kUnityTextureTargetBytes);
	add("gfxDriverBytes", kUnityGfxDriverBytes);
	unity["source"] = "Unity ProfilerRecorder / Texture statistics (only the counters this build of the game exposes)";
	j["unity"] = unity;

	if (vm.valid) {
		j["notAether"] = {
			{"dedicatedBytes", vm.localUsage > aetherGpu ? vm.localUsage - aetherGpu : 0},
			{"meaning", "process dedicated usage minus Aether's GPU resources: Unity / HDRP, the game's assets, other mods and driver overhead"},
		};
	}
	return j;
}

} // namespace aether
