#include "ReflectionDenoiser.h"

#include "../Core/Log.h"

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
const uint32_t kRingSize = 4096;       // descriptors for the dispatches' tables, used round robin

DXGI_FORMAT ToDxgi(nrd::Format f) {
	static const DXGI_FORMAT table[] = {
		DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT,
		DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SINT,
		DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
		DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16_FLOAT,
		DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_FLOAT,
		DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32_FLOAT,
		DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32_FLOAT,
		DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32_FLOAT,
		DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_SINT, DXGI_FORMAT_R32G32B32A32_FLOAT,
		DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UINT, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R9G9B9E5_SHAREDEXP,
	};
	static_assert(_countof(table) == size_t(nrd::Format::MAX_NUM), "one DXGI format per NRD format");
	return size_t(f) < _countof(table) ? table[size_t(f)] : DXGI_FORMAT_UNKNOWN;
}

// (the layout of NrdPasses.hlsl's constant buffer)
struct PassConstants {
	float invViewProj[16];
	float worldToView[16];
	float size[4];
	float hitDistParams[4];
};

ComPtr<ID3D12RootSignature> MakeRoot(ID3D12Device* device, UINT cbRegister, UINT cbSpace, UINT srvCount, UINT uavCount, UINT resourceBase, UINT resourceSpace,
	const D3D12_STATIC_SAMPLER_DESC* samplers, UINT samplerCount, const char* what) {
	D3D12_DESCRIPTOR_RANGE srv = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, std::max(srvCount, 1u), resourceBase, resourceSpace, 0};
	D3D12_DESCRIPTOR_RANGE uav = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, std::max(uavCount, 1u), resourceBase, resourceSpace, 0};
	D3D12_ROOT_PARAMETER params[3] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor = {cbRegister, cbSpace};
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable = {1, &srv};
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable = {1, &uav};
	D3D12_ROOT_SIGNATURE_DESC rs = {3, params, samplerCount, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
	ComPtr<ID3DBlob> blob, error;
	ComPtr<ID3D12RootSignature> root;
	HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
	if (SUCCEEDED(hr)) {
		hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
	}
	if (FAILED(hr)) {
		Log::Get().Write(Severity::Error, "Denoiser", "E_NRD_ROOT", std::string("A root signature for the reflection denoiser could not be created: ") + what, {},
			{{"hresult", HrToString(hr)}, {"message", error ? std::string(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize()) : ""}});
		return nullptr;
	}
	return root;
}

ComPtr<ID3D12PipelineState> MakePipeline(ID3D12Device* device, ID3D12RootSignature* root, const void* code, size_t size, const char* what) {
	D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
	d.pRootSignature = root;
	d.CS = {code, size};
	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
	if (FAILED(hr)) {
		Log::Get().Write(Severity::Error, "Denoiser", "E_NRD_PIPELINE", std::string("A reflection denoiser pipeline could not be created: ") + what, {}, {{"hresult", HrToString(hr)}});
		return nullptr;
	}
	return pso;
}

} // namespace

struct ReflectionDenoiser::Impl {
	ID3D12Device* device = nullptr;
	bool failed = false; // (pipelines that cannot be made are not tried again every frame)
	// NRD
	nrd::Instance* instance = nullptr;
	const nrd::InstanceDesc* desc = nullptr;
	ComPtr<ID3D12RootSignature> nrdRoot, ownRoot;
	std::vector<ComPtr<ID3D12PipelineState>> nrdPipelines;
	ComPtr<ID3D12PipelineState> prepare, resolve;
	// textures (the quarter resolution)
	uint32_t width = 0, height = 0, qw = 0, qh = 0;
	std::shared_ptr<GpuTexture> viewZ, normalRoughness, motion, specularIn, specularOut, output;
	std::vector<std::shared_ptr<GpuTexture>> permanent, transient;
	// descriptors
	uint32_t ring = UINT32_MAX, ringNext = 0;
	uint32_t frameIndex = 0;
	bool restart = true;
	// statistics
	uint32_t dispatches = 0;
	uint64_t poolBytes = 0;

	uint32_t Take(uint32_t count) {
		if (ringNext + count > kRingSize) {
			ringNext = 0;
		}
		const uint32_t at = ring + ringNext;
		ringNext += count;
		return at;
	}

	void RetireTextures(UINT64 fence, uint64_t frame) {
		GpuResources& gpu = GpuResources::Get();
		for (std::shared_ptr<GpuTexture>* t : {&viewZ, &normalRoughness, &motion, &specularIn, &specularOut, &output}) {
			if (*t) {
				gpu.RetireTexture(std::move(*t), fence, frame);
			}
			t->reset();
		}
		for (auto& t : permanent) {
			gpu.RetireTexture(std::move(t), fence, frame);
		}
		for (auto& t : transient) {
			gpu.RetireTexture(std::move(t), fence, frame);
		}
		permanent.clear();
		transient.clear();
		poolBytes = 0;
	}

	bool MakeObjects() {
		// Aether's two passes: b0, t0..t3, u0..u3 (space 0)
		ownRoot = MakeRoot(device, 0, 0, 4, 4, 0, 0, nullptr, 0, "prepare / resolve");
		prepare = ownRoot ? MakePipeline(device, ownRoot.Get(), kNrdPrepare, sizeof(kNrdPrepare), "prepare") : nullptr;
		resolve = ownRoot ? MakePipeline(device, ownRoot.Get(), kNrdResolve, sizeof(kNrdResolve), "resolve") : nullptr;
		return prepare && resolve;
	}

	bool MakeInstance() {
		const nrd::DenoiserDesc denoiser = {0, nrd::Denoiser::REBLUR_SPECULAR};
		nrd::InstanceCreationDesc create = {};
		create.denoisers = &denoiser;
		create.denoisersNum = 1;
		if (nrd::CreateInstance(create, instance) != nrd::Result::SUCCESS) {
			instance = nullptr;
			AE_LOG_CODE(Error, "Denoiser", "E_NRD_INSTANCE", "NRD's instance could not be created: reflections are not denoised");
			return false;
		}
		desc = nrd::GetInstanceDesc(*instance);
		if (!nrdRoot) {
			std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(desc->samplersNum);
			for (uint32_t i = 0; i < desc->samplersNum; ++i) {
				D3D12_STATIC_SAMPLER_DESC& s = samplers[i];
				s = {};
				s.Filter = desc->samplers[i] == nrd::Sampler::LINEAR_CLAMP ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
				s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
				s.MaxLOD = D3D12_FLOAT32_MAX;
				s.ShaderRegister = desc->samplersBaseRegisterIndex + i;
				s.RegisterSpace = desc->constantBufferAndSamplersSpaceIndex;
				s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			}
			nrdRoot = MakeRoot(device, desc->constantBufferRegisterIndex, desc->constantBufferAndSamplersSpaceIndex, desc->descriptorPoolDesc.perSetTexturesMaxNum,
				desc->descriptorPoolDesc.perSetStorageTexturesMaxNum, desc->resourcesBaseRegisterIndex, desc->resourcesSpaceIndex, samplers.data(), UINT(samplers.size()), "NRD");
			if (!nrdRoot) {
				return false;
			}
			nrdPipelines.resize(desc->pipelinesNum);
			for (uint32_t i = 0; i < desc->pipelinesNum; ++i) {
				const nrd::ComputeShaderDesc& cs = desc->pipelines[i].computeShaderDXIL;
				nrdPipelines[i] = MakePipeline(device, nrdRoot.Get(), cs.bytecode, size_t(cs.size), desc->pipelines[i].shaderIdentifier);
				if (!nrdPipelines[i]) {
					return false;
				}
			}
		}
		return true;
	}

	bool MakeTextures(uint64_t frame) {
		GpuResources& gpu = GpuResources::Get();
		auto make = [&](DXGI_FORMAT format, uint32_t w, uint32_t h, const char* name) {
			GpuTextureDesc d;
			d.width = std::max(w, 1u);
			d.height = std::max(h, 1u);
			d.format = format;
			d.unorderedAccess = true;
			auto t = gpu.CreateTexture(d, name, ResourceCategory::RenderTarget, Lifetime::Persistent, frame);
			poolBytes += t ? t->bytes : 0;
			return t;
		};
		viewZ = make(DXGI_FORMAT_R32_FLOAT, qw, qh, "Nrd.ViewZ");
		normalRoughness = make(DXGI_FORMAT_R10G10B10A2_UNORM, qw, qh, "Nrd.NormalRoughness");
		motion = make(DXGI_FORMAT_R16G16B16A16_FLOAT, qw, qh, "Nrd.Motion");
		specularIn = make(DXGI_FORMAT_R16G16B16A16_FLOAT, qw, qh, "Nrd.SpecularIn");
		specularOut = make(DXGI_FORMAT_R16G16B16A16_FLOAT, qw, qh, "Nrd.SpecularOut");
		output = make(DXGI_FORMAT_R16G16B16A16_FLOAT, qw, qh, "Nrd.Reflection");
		bool ok = viewZ && normalRoughness && motion && specularIn && specularOut && output;
		for (uint32_t i = 0; ok && i < desc->permanentPoolSize; ++i) {
			const nrd::TextureDesc& t = desc->permanentPool[i];
			permanent.push_back(make(ToDxgi(t.format), (qw + t.downsampleFactor - 1) / t.downsampleFactor, (qh + t.downsampleFactor - 1) / t.downsampleFactor, "Nrd.Permanent"));
			ok = ok && permanent.back();
		}
		for (uint32_t i = 0; ok && i < desc->transientPoolSize; ++i) {
			const nrd::TextureDesc& t = desc->transientPool[i];
			transient.push_back(make(ToDxgi(t.format), (qw + t.downsampleFactor - 1) / t.downsampleFactor, (qh + t.downsampleFactor - 1) / t.downsampleFactor, "Nrd.Transient"));
			ok = ok && transient.back();
		}
		return ok;
	}

	GpuTexture* Resource(const nrd::ResourceDesc& r) {
		switch (r.type) {
		case nrd::ResourceType::IN_MV: return motion.get();
		case nrd::ResourceType::IN_NORMAL_ROUGHNESS: return normalRoughness.get();
		case nrd::ResourceType::IN_VIEWZ: return viewZ.get();
		case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST: return specularIn.get();
		case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST: return specularOut.get();
		case nrd::ResourceType::PERMANENT_POOL: return r.indexInPool < permanent.size() ? permanent[r.indexInPool].get() : nullptr;
		case nrd::ResourceType::TRANSIENT_POOL: return r.indexInPool < transient.size() ? transient[r.indexInPool].get() : nullptr;
		default: return nullptr;
		}
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
		if (!d.MakeObjects()) {
			d.failed = true;
			return nullptr;
		}
		d.ring = GpuResources::Get().AllocSrv(kRingSize);
		if (d.ring == UINT32_MAX) {
			AE_LOG_CODE(Error, "Denoiser", "E_NRD_DESCRIPTORS", "No room in the descriptor heap for the reflection denoiser");
			d.failed = true;
			return nullptr;
		}
	}
	if (d.instance && d.width == width && d.height == height && d.output) {
		return d.output;
	}
	// (NRD cannot be resized: a new instance and new textures for the new size, the history starts again)
	d.RetireTextures(retireFence, frame);
	if (d.instance) {
		nrd::DestroyInstance(*d.instance);
		d.instance = nullptr;
	}
	d.width = width;
	d.height = height;
	d.qw = std::max((width + 1) / 2, 1u);
	d.qh = std::max((height + 1) / 2, 1u);
	if (!d.MakeInstance() || !d.MakeTextures(frame)) {
		d.failed = true;
		d.RetireTextures(retireFence, frame);
		std::lock_guard lock(statsMutex_);
		stats_ = {{"ready", false}, {"failed", true}};
		return nullptr;
	}
	d.restart = true;
	AE_LOG(Info, "Denoiser", "NRD REBLUR_SPECULAR ready at %ux%u (%u pipelines, %u permanent and %u transient textures, %.1f MB)", d.qw, d.qh, d.desc->pipelinesNum,
		d.desc->permanentPoolSize, d.desc->transientPoolSize, double(d.poolBytes) / (1024.0 * 1024.0));
	return d.output;
}

bool ReflectionDenoiser::Record(ID3D12GraphicsCommandList* list, const Frame& f, GpuTexture& depth, GpuTexture& surface, GpuTexture& velocity, GpuTexture& rays, GpuTexture& output, std::string& note) {
	if (!impl_ || !impl_->instance || impl_->failed) {
		note = "not ready";
		return false;
	}
	Impl& d = *impl_;
	GpuResources& gpu = GpuResources::Get();
	ID3D12DescriptorHeap* heaps[] = {gpu.SrvHeap()};
	list->SetDescriptorHeaps(1, heaps);
	auto uavBarrier = [&]() {
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		list->ResourceBarrier(1, &b);
	};

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
	if (nrd::SetCommonSettings(*d.instance, common) != nrd::Result::SUCCESS || nrd::SetDenoiserSettings(*d.instance, 0, &reblur) != nrd::Result::SUCCESS) {
		note = "NRD refused the settings";
		return false;
	}

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
	auto ownPass = [&](ID3D12PipelineState* pso, std::initializer_list<GpuTexture*> srvs, std::initializer_list<GpuTexture*> uavs) {
		const uint32_t srvTable = d.Take(4), uavTable = d.Take(4);
		uint32_t i = 0;
		for (GpuTexture* t : srvs) {
			gpu.CopySrv(srvTable + i++, t->srv);
		}
		i = 0;
		for (GpuTexture* t : uavs) {
			gpu.CopySrv(uavTable + i++, t->uav);
		}
		list->SetComputeRootSignature(d.ownRoot.Get());
		list->SetComputeRootConstantBufferView(0, cb.gpu);
		list->SetComputeRootDescriptorTable(1, gpu.SrvGpu(srvTable));
		list->SetComputeRootDescriptorTable(2, gpu.SrvGpu(uavTable));
		list->SetPipelineState(pso);
		list->Dispatch((d.qw + 7) / 8, (d.qh + 7) / 8, 1);
	};
	for (GpuTexture* t : {d.viewZ.get(), d.normalRoughness.get(), d.motion.get(), d.specularIn.get()}) {
		gpu.Transition(list, *t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	ownPass(d.prepare.Get(), {&depth, &surface, &velocity, &rays}, {d.viewZ.get(), d.normalRoughness.get(), d.motion.get(), d.specularIn.get()});

	// ---- 2. NRD
	const nrd::Identifier id = 0;
	const nrd::DispatchDesc* dispatches = nullptr;
	uint32_t count = 0;
	if (nrd::GetComputeDispatches(*d.instance, &id, 1, dispatches, count) != nrd::Result::SUCCESS) {
		note = "NRD gave no dispatches";
		return false;
	}
	D3D12_GPU_VIRTUAL_ADDRESS constants = 0;
	for (uint32_t k = 0; k < count; ++k) {
		const nrd::DispatchDesc& dispatch = dispatches[k];
		const nrd::PipelineDesc& pipeline = d.desc->pipelines[dispatch.pipelineIndex];
		uint32_t srvCount = 0, uavCount = 0;
		for (uint32_t r = 0; r < dispatch.resourcesNum; ++r) {
			(dispatch.resources[r].descriptorType == nrd::DescriptorType::TEXTURE ? srvCount : uavCount)++;
		}
		const uint32_t srvTable = d.Take(std::max(srvCount, 1u)), uavTable = d.Take(std::max(uavCount, 1u));
		uint32_t si = 0, ui = 0;
		for (uint32_t r = 0; r < dispatch.resourcesNum; ++r) {
			const nrd::ResourceDesc& res = dispatch.resources[r];
			GpuTexture* t = d.Resource(res);
			if (!t) {
				note = std::string("NRD asked for a resource Aether does not have: ") + nrd::GetResourceTypeString(res.type);
				return false;
			}
			if (res.descriptorType == nrd::DescriptorType::TEXTURE) {
				gpu.Transition(list, *t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				gpu.CopySrv(srvTable + si++, t->srv);
			} else {
				gpu.Transition(list, *t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				gpu.CopySrv(uavTable + ui++, t->uav);
			}
		}
		if (dispatch.constantBufferDataSize > 0 && (!dispatch.constantBufferDataMatchesPreviousDispatch || !constants)) {
			const UploadAllocation a = gpu.Upload(dispatch.constantBufferDataSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
			if (!a.cpu) {
				note = "no upload memory";
				return false;
			}
			memcpy(a.cpu, dispatch.constantBufferData, dispatch.constantBufferDataSize);
			constants = a.gpu;
		}
		list->SetComputeRootSignature(d.nrdRoot.Get());
		list->SetComputeRootConstantBufferView(0, constants ? constants : cb.gpu);
		list->SetComputeRootDescriptorTable(1, gpu.SrvGpu(srvTable));
		list->SetComputeRootDescriptorTable(2, gpu.SrvGpu(uavTable));
		list->SetPipelineState(d.nrdPipelines[dispatch.pipelineIndex].Get());
		list->Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);
		uavBarrier();
		(void)pipeline;
	}
	d.dispatches = count;

	// ---- 3. resolve
	gpu.Transition(list, *d.specularOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ownPass(d.resolve.Get(), {d.specularOut.get(), &depth, &rays}, {&output});
	uavBarrier();
	note = Format("NRD REBLUR_SPECULAR %ux%u, %u dispatches", d.qw, d.qh, count + 2);
	std::lock_guard lock(statsMutex_);
	stats_ = {{"ready", true}, {"resolution", {d.qw, d.qh}}, {"dispatches", count + 2}, {"textureBytes", d.poolBytes}, {"maxAccumulatedFrames", reblur.maxAccumulatedFrameNum}};
	return true;
}

void ReflectionDenoiser::Release(UINT64 fence, bool leak) {
	if (!impl_) {
		return;
	}
	Impl& d = *impl_;
	if (leak) {
		d.nrdRoot.Detach();
		d.ownRoot.Detach();
		d.prepare.Detach();
		d.resolve.Detach();
		for (auto& p : d.nrdPipelines) {
			p.Detach();
		}
	}
	d.RetireTextures(fence, 0);
	if (d.ring != UINT32_MAX && !leak) {
		GpuResources::Get().FreeSrv(d.ring, kRingSize, fence);
	}
	if (d.instance) {
		nrd::DestroyInstance(*d.instance);
	}
	// (the object stays: Prepare may be the caller)
	d.nrdRoot.Reset();
	d.ownRoot.Reset();
	d.prepare.Reset();
	d.resolve.Reset();
	d.nrdPipelines.clear();
	d.instance = nullptr;
	d.desc = nullptr;
	d.device = nullptr;
	d.ring = UINT32_MAX;
	d.ringNext = 0;
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
