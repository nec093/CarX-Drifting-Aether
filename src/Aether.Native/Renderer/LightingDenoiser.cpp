#include "LightingDenoiser.h"

#include "../Core/Log.h"
#include "NrdInstance.h"

#include <algorithm>

#if AETHER_HAS_NRD
#include <NRD.h>

#include "NrdLightPrepare.h" // kNrdLightPrepare: NrdPasses.hlsl CSLightPrepare, DXIL (generated when Aether is built)
#include "NrdLightResolve.h" // kNrdLightResolve
#endif

namespace aether {

LightingDenoiser& LightingDenoiser::Get() {
	static LightingDenoiser* d = new LightingDenoiser();
	return *d;
}

#if !AETHER_HAS_NRD

bool LightingDenoiser::Available() { return false; }
bool LightingDenoiser::Prepare(ID3D12Device*, uint32_t, uint32_t, UINT64, uint64_t) { return false; }
std::shared_ptr<GpuTexture> LightingDenoiser::Ambient() const { return nullptr; }
std::shared_ptr<GpuTexture> LightingDenoiser::Shadow() const { return nullptr; }
bool LightingDenoiser::Record(ID3D12GraphicsCommandList*, const Frame&, GpuTexture&, GpuTexture&, GpuTexture&, GpuTexture&, GpuTexture&, std::string&) { return false; }
void LightingDenoiser::Release(UINT64, bool) {}
json LightingDenoiser::Stats() { return {{"available", false}, {"reason", "built without NRD (scripts/fetch-deps.ps1)"}}; }

#else

namespace {

const float kDenoisingRange = 1500.0f;
const uint32_t kShadowId = 0, kDiffuseId = 1;

// (the layout of NrdPasses.hlsl's constant buffer)
struct PassConstants {
	float invViewProj[16];
	float worldToView[16];
	float size[4];
	float hitDistParams[4];
	float extra[4];
};

} // namespace

struct LightingDenoiser::Impl {
	ID3D12Device* device = nullptr;
	bool failed = false;
	NrdInstance nrd;
	ComPtr<ID3D12PipelineState> prepare, resolve;
	uint32_t width = 0, height = 0, qw = 0, qh = 0; // (the full size; the half the rays and NRD have)
	std::shared_ptr<GpuTexture> viewZ, normalRoughness, motion, penumbra, diffuseIn, shadowOut, diffuseOut, ambient, shadow;
	uint32_t frameIndex = 0;
	bool restart = true;
	uint64_t textureBytes = 0;

	void RetireTextures(UINT64 fence) {
		for (std::shared_ptr<GpuTexture>* t : {&viewZ, &normalRoughness, &motion, &penumbra, &diffuseIn, &shadowOut, &diffuseOut, &ambient, &shadow}) {
			if (*t) {
				GpuResources::Get().RetireTexture(std::move(*t), fence, 0);
			}
			t->reset();
		}
		textureBytes = 0;
	}
};

bool LightingDenoiser::Available() { return true; }

std::shared_ptr<GpuTexture> LightingDenoiser::Ambient() const { return impl_ ? impl_->ambient : nullptr; }
std::shared_ptr<GpuTexture> LightingDenoiser::Shadow() const { return impl_ ? impl_->shadow : nullptr; }

bool LightingDenoiser::Prepare(ID3D12Device* device, uint32_t width, uint32_t height, UINT64 retireFence, uint64_t frame) {
	if (!impl_) {
		impl_ = new Impl();
	}
	Impl& d = *impl_;
	if (d.failed) {
		return false;
	}
	if (d.device != device) {
		Release(retireFence, false);
		d.device = device;
	}
	if (d.nrd.Ready() && d.width == width && d.height == height && d.ambient) {
		return true;
	}
	d.RetireTextures(retireFence);
	d.nrd.Release(retireFence, false);
	d.width = width;
	d.height = height;
	d.qw = std::max((width + 1) / 2, 1u);
	d.qh = std::max((height + 1) / 2, 1u);
	bool ok = d.nrd.Create(device, {{kShadowId, uint32_t(nrd::Denoiser::SIGMA_SHADOW)}, {kDiffuseId, uint32_t(nrd::Denoiser::REBLUR_DIFFUSE)}}, d.qw, d.qh, frame,
		"SIGMA_SHADOW + REBLUR_DIFFUSE (sun shadow, ambient occlusion)");
	if (ok) {
		d.prepare = d.nrd.OwnPipeline(kNrdLightPrepare, sizeof(kNrdLightPrepare), "lighting prepare");
		d.resolve = d.nrd.OwnPipeline(kNrdLightResolve, sizeof(kNrdLightResolve), "lighting resolve");
		d.viewZ = NrdInstance::MakeTexture(DXGI_FORMAT_R32_FLOAT, d.qw, d.qh, "Nrd.Lighting.ViewZ", frame);
		d.normalRoughness = NrdInstance::MakeTexture(DXGI_FORMAT_R10G10B10A2_UNORM, d.qw, d.qh, "Nrd.Lighting.NormalRoughness", frame);
		d.motion = NrdInstance::MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, d.qw, d.qh, "Nrd.Lighting.Motion", frame);
		d.penumbra = NrdInstance::MakeTexture(DXGI_FORMAT_R16_FLOAT, d.qw, d.qh, "Nrd.Lighting.Penumbra", frame);
		d.diffuseIn = NrdInstance::MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, d.qw, d.qh, "Nrd.Lighting.DiffuseIn", frame);
		d.shadowOut = NrdInstance::MakeTexture(DXGI_FORMAT_R16_FLOAT, d.qw, d.qh, "Nrd.Lighting.ShadowOut", frame);
		d.diffuseOut = NrdInstance::MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, d.qw, d.qh, "Nrd.Lighting.DiffuseOut", frame);
		d.ambient = NrdInstance::MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, "Scene.RayAmbientOcclusion", frame);
		d.shadow = NrdInstance::MakeTexture(DXGI_FORMAT_R16_FLOAT, width, height, "Scene.RaySunShadow", frame);
		ok = d.prepare && d.resolve && d.viewZ && d.normalRoughness && d.motion && d.penumbra && d.diffuseIn && d.shadowOut && d.diffuseOut && d.ambient && d.shadow;
	}
	if (!ok) {
		d.failed = true;
		d.RetireTextures(retireFence);
		d.nrd.Release(retireFence, false);
		std::lock_guard lock(statsMutex_);
		stats_ = {{"ready", false}, {"failed", true}};
		return false;
	}
	for (auto* t : {&d.viewZ, &d.normalRoughness, &d.motion, &d.penumbra, &d.diffuseIn, &d.shadowOut, &d.diffuseOut, &d.ambient, &d.shadow}) {
		d.textureBytes += (*t)->bytes;
	}
	d.restart = true;
	return true;
}

bool LightingDenoiser::Record(ID3D12GraphicsCommandList* list, const Frame& f, GpuTexture& depth, GpuTexture& normal, GpuTexture& velocity, GpuTexture& sunRays,
	GpuTexture& aoRays, std::string& note) {
	if (!impl_ || !impl_->nrd.Ready() || impl_->failed) {
		note = "not ready";
		return false;
	}
	Impl& d = *impl_;
	GpuResources& gpu = GpuResources::Get();
	d.nrd.BeginFrame(f.retireFence);

	// ---- settings
	nrd::SigmaSettings sigma = {};
	memcpy(sigma.lightDirection, f.sunDirection, sizeof(sigma.lightDirection));
	nrd::ReblurSettings reblur = {};
	// (the occlusion is the hit distance against the rays' length: the normalisation is that length alone)
	reblur.hitDistanceParameters.A = std::max(f.aoRadius, 0.05f);
	reblur.hitDistanceParameters.B = 0.0001f;
	reblur.hitDistanceParameters.C = 1.0f;
	reblur.maxAccumulatedFrameNum = std::min<uint32_t>(nrd::GetMaxAccumulatedFrameNum(0.5f, std::max(f.fps, 10.0f)), nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
	reblur.maxFastAccumulatedFrameNum = std::max<uint32_t>(reblur.maxAccumulatedFrameNum / 5, 1);
	nrd::CommonSettings common = {};
	memcpy(common.worldToViewMatrix, f.worldToView, 64);
	memcpy(common.viewToClipMatrix, f.viewToClip, 64);
	memcpy(common.worldToViewMatrixPrev, f.worldToViewPrev, 64);
	memcpy(common.viewToClipMatrixPrev, f.viewToClipPrev, 64);
	common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] = uint16_t(d.qw);
	common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] = uint16_t(d.qh);
	common.denoisingRange = kDenoisingRange;
	common.frameIndex = d.frameIndex++;
	common.accumulationMode = d.restart || f.restart ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
	d.restart = false;

	// ---- 1. prepare
	PassConstants pc = {};
	memcpy(pc.invViewProj, f.invViewProj, 64);
	memcpy(pc.worldToView, f.worldToView, 64);
	pc.size[0] = float(d.width);
	pc.size[1] = float(d.height);
	pc.size[2] = float(d.qw);
	pc.size[3] = float(d.qh);
	pc.hitDistParams[0] = reblur.hitDistanceParameters.A;
	pc.hitDistParams[1] = reblur.hitDistanceParameters.B;
	pc.hitDistParams[2] = reblur.hitDistanceParameters.C;
	pc.hitDistParams[3] = kDenoisingRange;
	pc.extra[0] = f.tanSunRadius;
	pc.extra[1] = f.aoStrength;
	pc.extra[2] = f.gi ? 1.0f : 0.0f;
	const UploadAllocation cb = gpu.Upload(sizeof(pc), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	if (!cb.cpu) {
		note = "no upload memory";
		return false;
	}
	memcpy(cb.cpu, &pc, sizeof(pc));
	for (GpuTexture* t : {d.viewZ.get(), d.normalRoughness.get(), d.motion.get(), d.penumbra.get(), d.diffuseIn.get()}) {
		gpu.Transition(list, *t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	d.nrd.OwnPass(list, d.prepare.Get(), cb.gpu, {&depth, &normal, &velocity, &sunRays, &aoRays},
		{d.viewZ.get(), d.normalRoughness.get(), d.motion.get(), d.penumbra.get(), d.diffuseIn.get()}, d.qw, d.qh);
	D3D12_RESOURCE_BARRIER uav = {};
	uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	list->ResourceBarrier(1, &uav);

	// ---- 2. NRD
	uint32_t count = 0;
	std::string error;
	const bool ran = d.nrd.Run(list, &common, {{kShadowId, &sigma}, {kDiffuseId, &reblur}}, [&](uint32_t type) -> GpuTexture* {
		switch (nrd::ResourceType(type)) {
		case nrd::ResourceType::IN_MV: return d.motion.get();
		case nrd::ResourceType::IN_NORMAL_ROUGHNESS: return d.normalRoughness.get();
		case nrd::ResourceType::IN_VIEWZ: return d.viewZ.get();
		case nrd::ResourceType::IN_PENUMBRA: return d.penumbra.get();
		case nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY: return d.shadowOut.get();
		case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST: return d.diffuseIn.get();
		case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST: return d.diffuseOut.get();
		default: return nullptr;
		}
	}, count, error);
	if (!ran) {
		note = error;
		return false;
	}

	// ---- 3. resolve
	gpu.Transition(list, *d.shadowOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	gpu.Transition(list, *d.diffuseOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	gpu.Transition(list, *d.viewZ, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	d.nrd.OwnPass(list, d.resolve.Get(), cb.gpu, {d.shadowOut.get(), d.diffuseOut.get(), &depth, d.viewZ.get()}, {d.ambient.get(), d.shadow.get()}, d.width, d.height);
	list->ResourceBarrier(1, &uav);
	note = Format("NRD SIGMA_SHADOW + REBLUR_DIFFUSE %ux%u, %u dispatches", d.qw, d.qh, count + 2);
	std::lock_guard lock(statsMutex_);
	stats_ = {{"ready", true}, {"resolution", {d.qw, d.qh}}, {"dispatches", count + 2}, {"textureBytes", d.nrd.TextureBytes() + d.textureBytes}};
	return true;
}

void LightingDenoiser::Release(UINT64 fence, bool leak) {
	if (!impl_) {
		return;
	}
	Impl& d = *impl_;
	if (leak) {
		d.prepare.Detach();
		d.resolve.Detach();
	} else {
		d.RetireTextures(fence);
	}
	d.prepare.Reset();
	d.resolve.Reset();
	d.nrd.Release(fence, leak);
	d.device = nullptr;
	d.width = d.height = d.qw = d.qh = 0;
	d.failed = false;
	d.restart = true;
	std::lock_guard lock(statsMutex_);
	stats_ = {};
}

json LightingDenoiser::Stats() {
	json j = {{"available", true}};
	std::lock_guard lock(statsMutex_);
	for (auto& [k, v] : stats_.items()) {
		j[k] = v;
	}
	return j;
}

#endif

} // namespace aether
