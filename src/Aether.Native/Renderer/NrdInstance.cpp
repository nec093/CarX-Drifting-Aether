#include "NrdInstance.h"

#if AETHER_HAS_NRD

#include "../Core/Log.h"

#include <NRD.h>

#include <algorithm>

namespace aether {

namespace {

const uint32_t kBlockSize = 512; // descriptors per block for the dispatches' tables (a frame takes one or more)

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

ComPtr<ID3D12RootSignature> MakeRoot(ID3D12Device* device, UINT cbRegister, UINT cbSpace, UINT srvCount, UINT uavCount, UINT resourceBase, UINT resourceSpace,
	const D3D12_STATIC_SAMPLER_DESC* samplers, UINT samplerCount, const std::string& what) {
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
		Log::Get().Write(Severity::Error, "Denoiser", "E_NRD_ROOT", "A root signature for NRD could not be created: " + what, {},
			{{"hresult", HrToString(hr)}, {"message", error ? std::string(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize()) : ""}});
		return nullptr;
	}
	return root;
}

ComPtr<ID3D12PipelineState> MakePipeline(ID3D12Device* device, ID3D12RootSignature* root, const void* code, size_t size, const std::string& what) {
	D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
	d.pRootSignature = root;
	d.CS = {code, size};
	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
	if (FAILED(hr)) {
		Log::Get().Write(Severity::Error, "Denoiser", "E_NRD_PIPELINE", "An NRD pipeline could not be created: " + what, {}, {{"hresult", HrToString(hr)}});
		return nullptr;
	}
	return pso;
}

} // namespace

NrdInstance::~NrdInstance() {
	Release(0, true); // (the device's objects are let go by Release(fence) first; this only frees NRD's own memory)
}

std::shared_ptr<GpuTexture> NrdInstance::MakeTexture(DXGI_FORMAT format, uint32_t width, uint32_t height, const char* name, uint64_t frame) {
	GpuTextureDesc d;
	d.width = std::max(width, 1u);
	d.height = std::max(height, 1u);
	d.format = format;
	d.unorderedAccess = true;
	return GpuResources::Get().CreateTexture(d, name, ResourceCategory::RenderTarget, Lifetime::Persistent, frame);
}

bool NrdInstance::Create(ID3D12Device* device, const std::vector<std::pair<uint32_t, uint32_t>>& denoisers, uint32_t width, uint32_t height, uint64_t frame, const char* name) {
	Release(0, false);
	device_ = device;
	name_ = name;
	width_ = width;
	height_ = height;
	std::vector<nrd::DenoiserDesc> descs;
	for (const auto& [id, type] : denoisers) {
		descs.push_back({id, nrd::Denoiser(type)});
	}
	nrd::InstanceCreationDesc create = {};
	create.denoisers = descs.data();
	create.denoisersNum = uint32_t(descs.size());
	nrd::Instance* instance = nullptr;
	if (nrd::CreateInstance(create, instance) != nrd::Result::SUCCESS) {
		AE_LOG_CODE(Error, "Denoiser", "E_NRD_INSTANCE", "NRD's instance could not be created (%s)", name);
		return false;
	}
	instance_ = instance;
	const nrd::InstanceDesc* desc = nrd::GetInstanceDesc(*instance);
	desc_ = desc;
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
	nrdRoot_ = MakeRoot(device, desc->constantBufferRegisterIndex, desc->constantBufferAndSamplersSpaceIndex, desc->descriptorPoolDesc.perSetTexturesMaxNum,
		desc->descriptorPoolDesc.perSetStorageTexturesMaxNum, desc->resourcesBaseRegisterIndex, desc->resourcesSpaceIndex, samplers.data(), UINT(samplers.size()), name_);
	ownRoot_ = MakeRoot(device, 0, 0, 6, 6, 0, 0, nullptr, 0, name_ + " (own passes)");
	bool ok = nrdRoot_ && ownRoot_;
	for (uint32_t i = 0; ok && i < desc->pipelinesNum; ++i) {
		const nrd::ComputeShaderDesc& cs = desc->pipelines[i].computeShaderDXIL;
		pipelines_.push_back(MakePipeline(device, nrdRoot_.Get(), cs.bytecode, size_t(cs.size), desc->pipelines[i].shaderIdentifier));
		ok = pipelines_.back() != nullptr;
	}
	auto pool = [&](const nrd::TextureDesc* textures, uint32_t count, std::vector<std::shared_ptr<GpuTexture>>& out, const char* what) {
		for (uint32_t i = 0; ok && i < count; ++i) {
			const uint32_t f = textures[i].downsampleFactor;
			out.push_back(MakeTexture(ToDxgi(textures[i].format), (width + f - 1) / f, (height + f - 1) / f, what, frame));
			ok = out.back() != nullptr;
			poolBytes_ += ok ? out.back()->bytes : 0;
		}
	};
	pool(desc->permanentPool, desc->permanentPoolSize, permanent_, "Nrd.Permanent");
	pool(desc->transientPool, desc->transientPoolSize, transient_, "Nrd.Transient");
	if (!ok) {
		AE_LOG_CODE(Error, "Denoiser", "E_NRD_CREATE", "NRD could not be set up (%s): its pipelines, textures or descriptors", name);
		Release(0, false);
		return false;
	}
	AE_LOG(Info, "Denoiser", "NRD %s ready at %ux%u (%u pipelines, %u permanent and %u transient textures, %.1f MB)", name, width, height, desc->pipelinesNum,
		desc->permanentPoolSize, desc->transientPoolSize, double(poolBytes_) / (1024.0 * 1024.0));
	return true;
}

void NrdInstance::BeginFrame(UINT64 retireFence) {
	frameFence_ = retireFence;
	block_ = UINT32_MAX; // (the next Take starts a block of this frame's)
	blockNext_ = 0;
}

uint32_t NrdInstance::Take(uint32_t count) {
	if (block_ == UINT32_MAX || blockNext_ + count > kBlockSize) {
		GpuResources& gpu = GpuResources::Get();
		block_ = gpu.AllocSrv(kBlockSize);
		blockNext_ = 0;
		if (block_ == UINT32_MAX) {
			return UINT32_MAX;
		}
		// (handed back at once: the free list keeps it until this frame's fence has passed)
		gpu.FreeSrv(block_, kBlockSize, frameFence_);
	}
	const uint32_t at = block_ + blockNext_;
	blockNext_ += count;
	return at;
}

ComPtr<ID3D12PipelineState> NrdInstance::OwnPipeline(const void* code, size_t size, const char* what) const {
	return ownRoot_ ? MakePipeline(device_, ownRoot_.Get(), code, size, name_ + ": " + what) : nullptr;
}

void NrdInstance::OwnPass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS constants, std::initializer_list<GpuTexture*> srvs,
	std::initializer_list<GpuTexture*> uavs, uint32_t width, uint32_t height) {
	GpuResources& gpu = GpuResources::Get();
	const uint32_t srvTable = Take(6), uavTable = Take(6);
	if (srvTable == UINT32_MAX || uavTable == UINT32_MAX) {
		return; // (no descriptors: the heap is full; said by GpuResources)
	}
	uint32_t i = 0;
	for (GpuTexture* t : srvs) {
		gpu.CopySrv(srvTable + i++, t->srv);
	}
	i = 0;
	for (GpuTexture* t : uavs) {
		gpu.CopySrv(uavTable + i++, t->uav);
	}
	ID3D12DescriptorHeap* heaps[] = {gpu.SrvHeap()};
	list->SetDescriptorHeaps(1, heaps);
	list->SetComputeRootSignature(ownRoot_.Get());
	list->SetComputeRootConstantBufferView(0, constants);
	list->SetComputeRootDescriptorTable(1, gpu.SrvGpu(srvTable));
	list->SetComputeRootDescriptorTable(2, gpu.SrvGpu(uavTable));
	list->SetPipelineState(pso);
	list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

bool NrdInstance::Run(ID3D12GraphicsCommandList* list, const void* common, const std::vector<std::pair<uint32_t, const void*>>& settings,
	const std::function<GpuTexture*(uint32_t)>& resource, uint32_t& dispatchCount, std::string& error) {
	if (!instance_) {
		error = "not ready";
		return false;
	}
	nrd::Instance& instance = *static_cast<nrd::Instance*>(instance_);
	const nrd::InstanceDesc& desc = *static_cast<const nrd::InstanceDesc*>(desc_);
	if (nrd::SetCommonSettings(instance, *static_cast<const nrd::CommonSettings*>(common)) != nrd::Result::SUCCESS) {
		error = "NRD refused the common settings";
		return false;
	}
	std::vector<nrd::Identifier> ids;
	for (const auto& [id, s] : settings) {
		if (nrd::SetDenoiserSettings(instance, id, s) != nrd::Result::SUCCESS) {
			error = "NRD refused a denoiser's settings";
			return false;
		}
		ids.push_back(id);
	}
	const nrd::DispatchDesc* dispatches = nullptr;
	uint32_t count = 0;
	if (nrd::GetComputeDispatches(instance, ids.data(), uint32_t(ids.size()), dispatches, count) != nrd::Result::SUCCESS) {
		error = "NRD gave no dispatches";
		return false;
	}
	GpuResources& gpu = GpuResources::Get();
	ID3D12DescriptorHeap* heaps[] = {gpu.SrvHeap()};
	list->SetDescriptorHeaps(1, heaps);
	D3D12_GPU_VIRTUAL_ADDRESS constants = 0;
	for (uint32_t k = 0; k < count; ++k) {
		const nrd::DispatchDesc& dispatch = dispatches[k];
		uint32_t srvCount = 0, uavCount = 0;
		for (uint32_t r = 0; r < dispatch.resourcesNum; ++r) {
			(dispatch.resources[r].descriptorType == nrd::DescriptorType::TEXTURE ? srvCount : uavCount)++;
		}
		const uint32_t srvTable = Take(std::max(srvCount, 1u)), uavTable = Take(std::max(uavCount, 1u));
		if (srvTable == UINT32_MAX || uavTable == UINT32_MAX) {
			error = "no descriptors (the heap is full)";
			return false;
		}
		uint32_t si = 0, ui = 0;
		for (uint32_t r = 0; r < dispatch.resourcesNum; ++r) {
			const nrd::ResourceDesc& res = dispatch.resources[r];
			GpuTexture* t = res.type == nrd::ResourceType::PERMANENT_POOL ? (res.indexInPool < permanent_.size() ? permanent_[res.indexInPool].get() : nullptr)
				: res.type == nrd::ResourceType::TRANSIENT_POOL ? (res.indexInPool < transient_.size() ? transient_[res.indexInPool].get() : nullptr)
				: resource(uint32_t(res.type));
			if (!t) {
				error = std::string("NRD asked for a resource Aether does not have: ") + nrd::GetResourceTypeString(res.type);
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
				error = "no upload memory";
				return false;
			}
			memcpy(a.cpu, dispatch.constantBufferData, dispatch.constantBufferDataSize);
			constants = a.gpu;
		}
		list->SetComputeRootSignature(nrdRoot_.Get());
		if (constants) {
			list->SetComputeRootConstantBufferView(0, constants);
		}
		list->SetComputeRootDescriptorTable(1, gpu.SrvGpu(srvTable));
		list->SetComputeRootDescriptorTable(2, gpu.SrvGpu(uavTable));
		list->SetPipelineState(pipelines_[dispatch.pipelineIndex].Get());
		list->Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		list->ResourceBarrier(1, &b);
	}
	dispatchCount = count;
	(void)desc;
	return true;
}

void NrdInstance::Release(UINT64 fence, bool leak) {
	GpuResources& gpu = GpuResources::Get();
	if (leak) {
		nrdRoot_.Detach();
		ownRoot_.Detach();
		for (auto& p : pipelines_) {
			p.Detach();
		}
	}
	nrdRoot_.Reset();
	ownRoot_.Reset();
	pipelines_.clear();
	if (!leak) {
		for (auto* pool : {&permanent_, &transient_}) {
			for (auto& t : *pool) {
				if (t) {
					gpu.RetireTexture(std::move(t), fence, 0);
				}
			}
		}
	}
	permanent_.clear();
	transient_.clear();
	block_ = UINT32_MAX;
	blockNext_ = 0;
	poolBytes_ = 0;
	if (instance_) {
		nrd::DestroyInstance(*static_cast<nrd::Instance*>(instance_));
	}
	instance_ = nullptr;
	desc_ = nullptr;
	width_ = height_ = 0;
}

} // namespace aether

#else

namespace aether {
NrdInstance::~NrdInstance() = default;
bool NrdInstance::Create(ID3D12Device*, const std::vector<std::pair<uint32_t, uint32_t>>&, uint32_t, uint32_t, uint64_t, const char*) { return false; }
bool NrdInstance::Run(ID3D12GraphicsCommandList*, const void*, const std::vector<std::pair<uint32_t, const void*>>&, const std::function<GpuTexture*(uint32_t)>&, uint32_t&, std::string& error) {
	error = "built without NRD";
	return false;
}
ComPtr<ID3D12PipelineState> NrdInstance::OwnPipeline(const void*, size_t, const char*) const { return nullptr; }
void NrdInstance::OwnPass(ID3D12GraphicsCommandList*, ID3D12PipelineState*, D3D12_GPU_VIRTUAL_ADDRESS, std::initializer_list<GpuTexture*>, std::initializer_list<GpuTexture*>, uint32_t, uint32_t) {}
std::shared_ptr<GpuTexture> NrdInstance::MakeTexture(DXGI_FORMAT, uint32_t, uint32_t, const char*, uint64_t) { return nullptr; }
void NrdInstance::Release(UINT64, bool) {}
uint32_t NrdInstance::Take(uint32_t) { return 0; }
} // namespace aether

#endif
