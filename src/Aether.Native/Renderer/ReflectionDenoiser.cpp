#include "ReflectionDenoiser.h"

#include "../Core/Log.h"
#include "NrdInstance.h"

#include <algorithm>

#if AETHER_HAS_NRD
#include <NRD.h>

#include "NrdPrepare.h" // kNrdPrepare: NrdPasses.hlsl CSPrepare, DXIL (generated when Aether is built)
#include "NrdResolve.h" // kNrdResolve
#endif

namespace aether {

ReflectionDenoiser& ReflectionDenoiser::Get() {
	static ReflectionDenoiser* d = new ReflectionDenoiser();
	return *d;
}

#if !AETHER_HAS_NRD

bool ReflectionDenoiser::Available() { return false; }
std::shared_ptr<GpuTexture> ReflectionDenoiser::Prepare(ID3D12Device*, uint32_t, uint32_t, UINT64, uint64_t) { return nullptr; }
bool ReflectionDenoiser::Record(ID3D12GraphicsCommandList*, const Frame&, GpuTexture&, GpuTexture&, GpuTexture&, GpuTexture&, GpuTexture&, std::string&) { return false; }
void ReflectionDenoiser::Release(UINT64, bool) {}
json ReflectionDenoiser::Stats() { return {{"available", false}, {"reason", "built without NRD (scripts/fetch-deps.ps1)"}}; }

#else

namespace {

const float kDenoisingRange = 1500.0f; // (m: beyond the far plane; what is not denoised is marked twice as far)

// (the layout of NrdPasses.hlsl's constant buffer)
struct PassConstants {
	float invViewProj[16];
	float worldToView[16];
	float size[4];
	float hitDistParams[4];
	float extra[4];
};

} // namespace

struct ReflectionDenoiser::Impl {
	ID3D12Device* device = nullptr;
	bool failed = false; // (what cannot be made is not tried again every frame)
	NrdInstance nrd;
	ComPtr<ID3D12PipelineState> prepare, resolve;
	uint32_t width = 0, height = 0, qw = 0, qh = 0;
	std::shared_ptr<GpuTexture> viewZ, normalRoughness, motion, specularIn, specularOut, output;
	uint32_t frameIndex = 0;
	bool restart = true;
	uint64_t textureBytes = 0;

	void RetireTextures(UINT64 fence) {
		for (std::shared_ptr<GpuTexture>* t : {&viewZ, &normalRoughness, &motion, &specularIn, &specularOut, &output}) {
			if (*t) {
				GpuResources::Get().RetireTexture(std::move(*t), fence, 0);
			}
			t->reset();
		}
		textureBytes = 0;
	}
};

bool ReflectionDenoiser::Available() { return true; }

std::shared_ptr<GpuTexture> ReflectionDenoiser::Prepare(ID3D12Device* device, uint32_t width, uint32_t height, UINT64 retireFence, uint64_t frame) {
	if (!impl_) {
		impl_ = new Impl();
	}
	Impl& d = *impl_;
	if (d.failed) {
		return nullptr;
	}
	if (d.device != device) {
		Release(retireFence, false);
		d.device = device;
	}
	if (d.nrd.Ready() && d.width == width && d.height == height && d.output) {
		return d.output;
	}
	// (NRD cannot be resized: a new instance and new textures for the new size, the history starts again)
	d.RetireTextures(retireFence);
	d.nrd.Release(retireFence, false);
	d.width = width;
	d.height = height;
	d.qw = std::max((width + 1) / 2, 1u);
	d.qh = std::max((height + 1) / 2, 1u);
	bool ok = d.nrd.Create(device, {{0, uint32_t(nrd::Denoiser::REBLUR_SPECULAR)}}, d.qw, d.qh, frame, "REBLUR_SPECULAR (reflections)");
	if (ok) {
		d.prepare = d.nrd.OwnPipeline(kNrdPrepare, sizeof(kNrdPrepare), "reflection prepare");
		d.resolve = d.nrd.OwnPipeline(kNrdResolve, sizeof(kNrdResolve), "reflection resolve");
		d.viewZ = NrdInstance::MakeTexture(DXGI_FORMAT_R32_FLOAT, d.qw, d.qh, "Nrd.Reflection.ViewZ", frame);
		d.normalRoughness = NrdInstance::MakeTexture(DXGI_FORMAT_R10G10B10A2_UNORM, d.qw, d.qh, "Nrd.Reflection.NormalRoughness", frame);
		d.motion = NrdInstance::MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, d.qw, d.qh, "Nrd.Reflection.Motion", frame);
		d.specularIn = NrdInstance::MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, d.qw, d.qh, "Nrd.Reflection.In", frame);
		d.specularOut = NrdInstance::MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, d.qw, d.qh, "Nrd.Reflection.Out", frame);
		d.output = NrdInstance::MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, d.qw, d.qh, "Nrd.Reflection", frame);
		ok = d.prepare && d.resolve && d.viewZ && d.normalRoughness && d.motion && d.specularIn && d.specularOut && d.output;
	}
	if (!ok) {
		d.failed = true;
		d.RetireTextures(retireFence);
		d.nrd.Release(retireFence, false);
		std::lock_guard lock(statsMutex_);
		stats_ = {{"ready", false}, {"failed", true}};
		return nullptr;
	}
	for (auto* t : {&d.viewZ, &d.normalRoughness, &d.motion, &d.specularIn, &d.specularOut, &d.output}) {
		d.textureBytes += (*t)->bytes;
	}
	d.restart = true;
	return d.output;
}

bool ReflectionDenoiser::Record(ID3D12GraphicsCommandList* list, const Frame& f, GpuTexture& depth, GpuTexture& surface, GpuTexture& velocity, GpuTexture& rays, GpuTexture& output, std::string& note) {
	if (!impl_ || !impl_->nrd.Ready() || impl_->failed) {
		note = "not ready";
		return false;
	}
	Impl& d = *impl_;
	GpuResources& gpu = GpuResources::Get();
	d.nrd.BeginFrame(f.retireFence);

	// ---- settings
	nrd::ReblurSettings reblur = {};
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
	const UploadAllocation cb = gpu.Upload(sizeof(pc), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	if (!cb.cpu) {
		note = "no upload memory";
		return false;
	}
	memcpy(cb.cpu, &pc, sizeof(pc));
	for (GpuTexture* t : {d.viewZ.get(), d.normalRoughness.get(), d.motion.get(), d.specularIn.get()}) {
		gpu.Transition(list, *t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	d.nrd.OwnPass(list, d.prepare.Get(), cb.gpu, {&depth, &surface, &velocity, &rays}, {d.viewZ.get(), d.normalRoughness.get(), d.motion.get(), d.specularIn.get()}, d.qw, d.qh);
	D3D12_RESOURCE_BARRIER uav = {};
	uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	list->ResourceBarrier(1, &uav);

	// ---- 2. NRD
	uint32_t count = 0;
	std::string error;
	const bool ran = d.nrd.Run(list, &common, {{0u, &reblur}}, [&](uint32_t type) -> GpuTexture* {
		switch (nrd::ResourceType(type)) {
		case nrd::ResourceType::IN_MV: return d.motion.get();
		case nrd::ResourceType::IN_NORMAL_ROUGHNESS: return d.normalRoughness.get();
		case nrd::ResourceType::IN_VIEWZ: return d.viewZ.get();
		case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST: return d.specularIn.get();
		case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST: return d.specularOut.get();
		default: return nullptr;
		}
	}, count, error);
	if (!ran) {
		note = error;
		return false;
	}

	// ---- 3. resolve
	gpu.Transition(list, *d.specularOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	d.nrd.OwnPass(list, d.resolve.Get(), cb.gpu, {d.specularOut.get(), &depth, &rays}, {&output}, d.qw, d.qh);
	list->ResourceBarrier(1, &uav);
	note = Format("NRD REBLUR_SPECULAR %ux%u, %u dispatches", d.qw, d.qh, count + 2);
	std::lock_guard lock(statsMutex_);
	stats_ = {{"ready", true}, {"resolution", {d.qw, d.qh}}, {"dispatches", count + 2}, {"textureBytes", d.nrd.TextureBytes() + d.textureBytes}, {"maxAccumulatedFrames", reblur.maxAccumulatedFrameNum}};
	return true;
}

void ReflectionDenoiser::Release(UINT64 fence, bool leak) {
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

json ReflectionDenoiser::Stats() {
	const nrd::LibraryDesc* lib = nrd::GetLibraryDesc();
	json j = {{"available", true}, {"library", Format("NRD %u.%u.%u", lib->versionMajor, lib->versionMinor, lib->versionBuild)}};
	std::lock_guard lock(statsMutex_);
	for (auto& [k, v] : stats_.items()) {
		j[k] = v;
	}
	return j;
}

#endif

} // namespace aether
