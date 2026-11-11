#include "GpuInfo.h"
#include "../Core/Log.h"

namespace aether {

GpuInfo& GpuInfo::Get() {
	static GpuInfo* g = new GpuInfo();
	return *g;
}

static const char* VendorName(UINT id) {
	switch (id) {
	case 0x10DE: return "NVIDIA";
	case 0x1002: case 0x1022: return "AMD";
	case 0x8086: return "Intel";
	case 0x1414: return "Microsoft";
	case 0x5143: return "Qualcomm";
	default: return "Unknown";
	}
}

static const char* FeatureLevelName(D3D_FEATURE_LEVEL l) {
	switch (l) {
	case D3D_FEATURE_LEVEL_12_2: return "12_2";
	case D3D_FEATURE_LEVEL_12_1: return "12_1";
	case D3D_FEATURE_LEVEL_12_0: return "12_0";
	case D3D_FEATURE_LEVEL_11_1: return "11_1";
	case D3D_FEATURE_LEVEL_11_0: return "11_0";
	default: return "unknown";
	}
}

static std::string ShaderModelName(D3D_SHADER_MODEL m) {
	return Format("%d.%d", (int(m) >> 4) & 0xF, int(m) & 0xF);
}

template <typename T>
static bool Feature(ID3D12Device* device, D3D12_FEATURE feature, T& data) {
	return SUCCEEDED(device->CheckFeatureSupport(feature, &data, sizeof(T)));
}

static bool FormatSupported(ID3D12Device* device, DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 need) {
	D3D12_FEATURE_DATA_FORMAT_SUPPORT s = {format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
	return Feature(device, D3D12_FEATURE_FORMAT_SUPPORT, s) && (s.Support1 & need) == need;
}

void GpuInfo::Reset() {
	std::lock_guard lock(mutex_);
	valid_ = false;
	adapter_.Reset();
	device_.Reset();
}

bool GpuInfo::Valid() {
	std::lock_guard lock(mutex_);
	return valid_;
}

void GpuInfo::Capture(ID3D12Device* device) {
	if (!device) {
		return;
	}
	json info;
	std::string vendor = "Unknown", name, driver;
	uint64_t dedicated = 0;
	ComPtr<IDXGIAdapter3> adapter3;

	// ---- adapter (by the device's LUID)
	LUID luid = device->GetAdapterLuid();
	ComPtr<IDXGIFactory4> factory;
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
	ComPtr<IDXGIAdapter1> adapter;
	if (SUCCEEDED(hr)) {
		hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
	}
	if (SUCCEEDED(hr) && adapter) {
		DXGI_ADAPTER_DESC1 d = {};
		adapter->GetDesc1(&d);
		vendor = VendorName(d.VendorId);
		name = WideToUtf8(d.Description);
		dedicated = d.DedicatedVideoMemory;
		LARGE_INTEGER umd = {};
		if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
			driver = Format("%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
		}
		info["adapter"] = {
			{"vendor", vendor},
			{"vendorId", Format("0x%04X", d.VendorId)},
			{"name", name},
			{"deviceId", Format("0x%04X", d.DeviceId)},
			{"subSysId", Format("0x%08X", d.SubSysId)},
			{"revision", d.Revision},
			{"driverVersion", driver},
			{"dedicatedVideoMemoryBytes", uint64_t(d.DedicatedVideoMemory)},
			{"dedicatedSystemMemoryBytes", uint64_t(d.DedicatedSystemMemory)},
			{"sharedSystemMemoryBytes", uint64_t(d.SharedSystemMemory)},
			{"software", (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0},
			{"luid", Format("%08X:%08X", unsigned(luid.HighPart), unsigned(luid.LowPart))},
		};
		adapter.As(&adapter3);
	} else {
		AE_LOG_CODE(Warning, "GpuInfo", "W_ADAPTER_LOOKUP", "Could not find the device's adapter: %s", HrToString(hr).c_str());
		info["adapter"] = {{"vendor", vendor}, {"error", HrToString(hr)}};
	}

	// ---- features
	json f;
	{
		static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
		D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {UINT(_countof(levels)), levels, D3D_FEATURE_LEVEL_11_0};
		if (Feature(device, D3D12_FEATURE_FEATURE_LEVELS, fl)) {
			f["maxFeatureLevel"] = FeatureLevelName(fl.MaxSupportedFeatureLevel);
		}
	}
	{
		// the runtime answers with the highest model it knows that is <= the one asked for, or fails for a model it does not know
		static const D3D_SHADER_MODEL models[] = {D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4,
			D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1};
		for (D3D_SHADER_MODEL m : models) {
			D3D12_FEATURE_DATA_SHADER_MODEL sm = {m};
			if (Feature(device, D3D12_FEATURE_SHADER_MODEL, sm)) {
				f["shaderModel"] = ShaderModelName(sm.HighestShaderModel);
				break;
			}
		}
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
	if (Feature(device, D3D12_FEATURE_D3D12_OPTIONS, o)) {
		f["resourceBindingTier"] = int(o.ResourceBindingTier);
		f["resourceHeapTier"] = int(o.ResourceHeapTier);
		f["tiledResourcesTier"] = int(o.TiledResourcesTier);
		f["conservativeRasterizationTier"] = int(o.ConservativeRasterizationTier);
		f["doublePrecisionFloatShaderOps"] = o.DoublePrecisionFloatShaderOps != 0;
		f["rovsSupported"] = o.ROVsSupported != 0;
		f["typedUAVLoadAdditionalFormats"] = o.TypedUAVLoadAdditionalFormats != 0;
		f["standardSwizzle64KB"] = o.StandardSwizzle64KBSupported != 0;
		f["maxGpuVirtualAddressBitsPerResource"] = o.MaxGPUVirtualAddressBitsPerResource;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {};
	if (Feature(device, D3D12_FEATURE_D3D12_OPTIONS1, o1)) {
		f["waveOps"] = o1.WaveOps != 0;
		f["waveLaneCountMin"] = o1.WaveLaneCountMin;
		f["waveLaneCountMax"] = o1.WaveLaneCountMax;
		f["int64ShaderOps"] = o1.Int64ShaderOps != 0;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {};
	if (Feature(device, D3D12_FEATURE_D3D12_OPTIONS3, o3)) {
		f["copyQueueTimestampQueries"] = o3.CopyQueueTimestampQueriesSupported != 0;
		f["castingFullyTypedFormat"] = o3.CastingFullyTypedFormatSupported != 0;
		f["viewInstancingTier"] = int(o3.ViewInstancingTier);
		f["barycentrics"] = o3.BarycentricsSupported != 0;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = {};
	if (Feature(device, D3D12_FEATURE_D3D12_OPTIONS4, o4)) {
		f["native16BitShaderOps"] = o4.Native16BitShaderOpsSupported != 0;
		f["msaa64KBAlignedTexture"] = o4.MSAA64KBAlignedTextureSupported != 0;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
	if (Feature(device, D3D12_FEATURE_D3D12_OPTIONS5, o5)) {
		f["raytracingTier"] = o5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED ? "none" : (o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 ? "1.1" : "1.0");
		f["renderPassesTier"] = int(o5.RenderPassesTier);
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
	if (Feature(device, D3D12_FEATURE_D3D12_OPTIONS6, o6)) {
		f["variableShadingRateTier"] = int(o6.VariableShadingRateTier);
		f["backgroundProcessing"] = o6.BackgroundProcessingSupported != 0;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
	if (Feature(device, D3D12_FEATURE_D3D12_OPTIONS7, o7)) {
		f["meshShaderTier"] = o7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED ? 0 : 1;
		f["samplerFeedbackTier"] = int(o7.SamplerFeedbackTier);
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = {};
	if (Feature(device, D3D12_FEATURE_D3D12_OPTIONS12, o12)) {
		f["enhancedBarriers"] = o12.EnhancedBarriersSupported != 0;
		f["relaxedFormatCasting"] = o12.RelaxedFormatCastingSupported != 0;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16 = {};
	if (Feature(device, D3D12_FEATURE_D3D12_OPTIONS16, o16)) {
		f["gpuUploadHeap"] = o16.GPUUploadHeapSupported != 0;
	}
	D3D12_FEATURE_DATA_ARCHITECTURE1 arch = {};
	if (Feature(device, D3D12_FEATURE_ARCHITECTURE1, arch)) {
		f["uma"] = arch.UMA != 0;
		f["cacheCoherentUMA"] = arch.CacheCoherentUMA != 0;
		f["tileBasedRenderer"] = arch.TileBasedRenderer != 0;
	}
	D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va = {};
	if (Feature(device, D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, va)) {
		f["maxGpuVirtualAddressBitsPerProcess"] = va.MaxGPUVirtualAddressBitsPerProcess;
	}
	{
		const D3D12_FORMAT_SUPPORT1 tex = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
		f["textureFormats"] = {
			{"BC1", FormatSupported(device, DXGI_FORMAT_BC1_UNORM, tex)},
			{"BC3", FormatSupported(device, DXGI_FORMAT_BC3_UNORM, tex)},
			{"BC4", FormatSupported(device, DXGI_FORMAT_BC4_UNORM, tex)},
			{"BC5", FormatSupported(device, DXGI_FORMAT_BC5_UNORM, tex)},
			{"BC6H", FormatSupported(device, DXGI_FORMAT_BC6H_UF16, tex)},
			{"BC7", FormatSupported(device, DXGI_FORMAT_BC7_UNORM, tex)},
			{"R11G11B10_FLOAT_RT", FormatSupported(device, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_FORMAT_SUPPORT1_RENDER_TARGET)},
			{"R16G16B16A16_FLOAT_RT", FormatSupported(device, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_FORMAT_SUPPORT1_RENDER_TARGET)},
		};
	}
	info["features"] = f;
	info["nodeCount"] = device->GetNodeCount();

	{
		std::lock_guard lock(mutex_);
		info_ = std::move(info);
		vendor_ = vendor;
		name_ = name;
		driver_ = driver;
		dedicated_ = dedicated;
		adapter_ = adapter3;
		device_ = device;
		valid_ = true;
	}
	AE_LOG(Info, "GpuInfo", "%s %s, driver %s, %.0f MB dedicated, shader model %s, binding tier %d, heap tier %d", vendor.c_str(), name.c_str(), driver.c_str(),
		double(dedicated) / (1024.0 * 1024.0), f.value("shaderModel", "?").c_str(), f.value("resourceBindingTier", 0), f.value("resourceHeapTier", 0));
}

json GpuInfo::ToJson() {
	json out;
	{
		std::lock_guard lock(mutex_);
		if (!valid_) {
			return {{"available", false}, {"reason", "the D3D12 device has not been seen yet (no frame rendered)"}};
		}
		out = info_;
	}
	out["available"] = true;
	HRESULT reason = S_OK;
	out["deviceState"] = DeviceState(&reason);
	return out;
}

std::string GpuInfo::Vendor() {
	std::lock_guard lock(mutex_);
	return vendor_;
}

std::string GpuInfo::Name() {
	std::lock_guard lock(mutex_);
	return name_;
}

uint64_t GpuInfo::DedicatedVideoMemory() {
	std::lock_guard lock(mutex_);
	return dedicated_;
}

json GpuInfo::Brief() {
	std::lock_guard lock(mutex_);
	if (!valid_) {
		return nullptr;
	}
	return {{"vendor", vendor_}, {"name", name_}, {"driver", driver_}};
}

VideoMemory GpuInfo::QueryVideoMemory() {
	ComPtr<IDXGIAdapter3> adapter;
	{
		std::lock_guard lock(mutex_);
		adapter = adapter_;
	}
	VideoMemory m;
	if (!adapter) {
		return m;
	}
	DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, nonLocal = {};
	if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
		return m;
	}
	adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
	m.valid = true;
	m.localBudget = local.Budget;
	m.localUsage = local.CurrentUsage;
	m.localReservation = local.CurrentReservation;
	m.localAvailableForReservation = local.AvailableForReservation;
	m.nonLocalBudget = nonLocal.Budget;
	m.nonLocalUsage = nonLocal.CurrentUsage;
	m.nonLocalReservation = nonLocal.CurrentReservation;
	m.nonLocalAvailableForReservation = nonLocal.AvailableForReservation;
	return m;
}

std::string GpuInfo::DeviceState(HRESULT* reason) {
	ComPtr<ID3D12Device> device;
	{
		std::lock_guard lock(mutex_);
		device = device_;
	}
	if (!device) {
		return "unknown";
	}
	HRESULT hr = device->GetDeviceRemovedReason();
	if (reason) {
		*reason = hr;
	}
	return hr == S_OK ? "ok" : "removed: " + HrToString(hr);
}

} // namespace aether
