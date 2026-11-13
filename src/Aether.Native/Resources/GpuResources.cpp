#include "GpuResources.h"
#include "../Core/Config.h"
#include "../Core/Log.h"

#include <algorithm>

namespace aether {

static const uint32_t kRtvHeapSize = 512, kDsvHeapSize = 128; // (CPU only heaps: a view costs a few bytes)
static const size_t kArenaSize = 8u << 20; // per frame in flight: constants, instance and light data
static const size_t kMaxArenas = 8;

GpuResources& GpuResources::Get() {
	static GpuResources* g = new GpuResources();
	return *g;
}

bool GpuResources::Init(ID3D12Device* device) {
	if (device_ == device) {
		return true;
	}
	device_ = device;
	D3D12_DESCRIPTOR_HEAP_DESC srv = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
	D3D12_DESCRIPTOR_HEAP_DESC rtv = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapSize, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
	D3D12_DESCRIPTOR_HEAP_DESC dsv = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapSize, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
	D3D12_DESCRIPTOR_HEAP_DESC staging = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
	HRESULT hr = device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srvHeap_));
	if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&staging, IID_PPV_ARGS(&srvStaging_));
	if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvHeap_));
	if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&dsvHeap_));
	if (FAILED(hr)) {
		Log::Get().Write(Severity::Error, "GpuResources", "E_DESCRIPTOR_HEAP", "Descriptor heaps could not be created", {}, {{"hresult", HrToString(hr)}});
		srvHeap_.Reset();
		srvStaging_.Reset();
		rtvHeap_.Reset();
		dsvHeap_.Reset();
		device_ = nullptr;
		return false;
	}
	srvHeap_->SetName(L"Aether.SrvHeap");
	srvSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	rtvSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	dsvSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	srvFree_.clear();
	srvFree_.push_back({0, kSrvHeapSize});
	rtvNext_ = dsvNext_ = 0;
	rtvFree_.clear();
	dsvFree_.clear();
	std::lock_guard lock(statsMutex_);
	stats_ = Stats();
	return true;
}

void GpuResources::Shutdown(bool leak) {
	if (!device_) {
		return;
	}
	if (leak) {
		for (auto& r : retired_) r.object.Detach();
		for (auto& t : retiredTextures_) t.texture->resource.Detach();
		for (auto& a : arenas_) a->buffer.Detach();
		srvHeap_.Detach();
		srvStaging_.Detach();
		rtvHeap_.Detach();
		dsvHeap_.Detach();
	} else {
		for (auto& a : arenas_) {
			if (a->buffer && a->mapped) {
				a->buffer->Unmap(0, nullptr);
			}
		}
	}
	retired_.clear();
	retiredTextures_.clear();
	retiredRanges_.clear();
	arenas_.clear();
	arena_ = nullptr;
	srvHeap_.Reset();
	srvStaging_.Reset();
	rtvHeap_.Reset();
	dsvHeap_.Reset();
	device_ = nullptr;
	std::lock_guard lock(statsMutex_);
	stats_ = Stats();
}

// ---- descriptors

uint32_t GpuResources::AllocSrv(uint32_t count) {
	for (size_t i = 0; i < srvFree_.size(); ++i) {
		if (srvFree_[i].count >= count) {
			uint32_t start = srvFree_[i].start;
			srvFree_[i].start += count;
			srvFree_[i].count -= count;
			if (srvFree_[i].count == 0) {
				srvFree_.erase(srvFree_.begin() + ptrdiff_t(i));
			}
			std::lock_guard lock(statsMutex_);
			stats_.srvUsed += count;
			stats_.srvPeak = std::max(stats_.srvPeak, stats_.srvUsed);
			return start;
		}
	}
	AE_LOG_CODE(Error, "GpuResources", "E_DESCRIPTOR_HEAP_FULL", "The SRV heap is full (%u descriptors): a view could not be created", kSrvHeapSize);
	return UINT32_MAX;
}

void GpuResources::FreeSrv(uint32_t index, uint32_t count, UINT64 fence) {
	if (index != UINT32_MAX && count > 0) {
		retiredRanges_.push_back({{index, count}, fence});
	}
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuResources::SrvCpu(uint32_t index) const {
	D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
	h.ptr += SIZE_T(index) * srvSize_;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuResources::StagingCpu(uint32_t index) const {
	D3D12_CPU_DESCRIPTOR_HANDLE h = srvStaging_->GetCPUDescriptorHandleForHeapStart();
	h.ptr += SIZE_T(index) * srvSize_;
	return h;
}

void GpuResources::WriteSrv(uint32_t index, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc) {
	if (index >= kSrvHeapSize) {
		return;
	}
	device_->CreateShaderResourceView(resource, desc, StagingCpu(index));
	device_->CreateShaderResourceView(resource, desc, SrvCpu(index));
}

void GpuResources::CopySrv(uint32_t destination, uint32_t source) {
	if (destination >= kSrvHeapSize || source >= kSrvHeapSize) {
		return;
	}
	device_->CopyDescriptorsSimple(1, StagingCpu(destination), StagingCpu(source), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	device_->CopyDescriptorsSimple(1, SrvCpu(destination), StagingCpu(source), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuResources::SrvGpu(uint32_t index) const {
	D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap_->GetGPUDescriptorHandleForHeapStart();
	h.ptr += UINT64(index) * srvSize_;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuResources::RtvCpu(uint32_t index) const {
	D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
	h.ptr += SIZE_T(index) * rtvSize_;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuResources::DsvCpu(uint32_t index) const {
	D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
	h.ptr += SIZE_T(index) * dsvSize_;
	return h;
}

uint32_t GpuResources::AllocTarget(std::vector<uint32_t>& freeList, uint32_t& next, uint32_t capacity) {
	if (!freeList.empty()) {
		uint32_t i = freeList.back();
		freeList.pop_back();
		return i;
	}
	return next < capacity ? next++ : UINT32_MAX;
}

// ---- textures

std::shared_ptr<GpuTexture> GpuResources::CreateTexture(const GpuTextureDesc& desc, const char* name, ResourceCategory category, Lifetime lifetime, uint64_t frame) {
	if (!device_) {
		return nullptr;
	}
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = desc.width;
	rd.Height = desc.height;
	rd.DepthOrArraySize = desc.arraySize;
	rd.MipLevels = desc.mipLevels;
	rd.Format = desc.format;
	rd.SampleDesc.Count = 1;
	rd.Flags = (desc.renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE) | (desc.depthStencil ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE) |
		(desc.unorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
	if (desc.depthStencil && !desc.shaderResource) {
		rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
	}
	D3D12_CLEAR_VALUE clear = {};
	clear.Format = desc.targetFormat;
	if (desc.depthStencil) {
		clear.DepthStencil.Depth = desc.clearDepth;
	} else {
		memcpy(clear.Color, desc.clearColor, sizeof(clear.Color));
	}
	const bool target = desc.renderTarget || desc.depthStencil;
	// the state a texture is created in is the one its first use needs least work from
	D3D12_RESOURCE_STATES initial = desc.depthStencil ? D3D12_RESOURCE_STATE_DEPTH_WRITE : desc.renderTarget ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST;
	D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
	auto t = std::make_shared<GpuTexture>();
	HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, initial, target ? &clear : nullptr, IID_PPV_ARGS(&t->resource));
	if (FAILED(hr)) {
		Log::Get().Write(Severity::Error, "GpuResources", hr == E_OUTOFMEMORY ? "E_OUT_OF_MEMORY" : "E_RESOURCE_CREATE", std::string("Texture creation failed: ") + name, name,
			{{"hresult", HrToString(hr)}, {"width", desc.width}, {"height", desc.height}, {"dxgiFormat", int(desc.format)}});
		return nullptr;
	}
	t->desc = desc;
	t->state = initial;
	t->name = name;
	t->lifetime = lifetime;
	t->lastUsedFrame = frame;
	t->registryId = ResourceRegistry::Get().Register(device_, t->resource.Get(), name, category, D3D12_HEAP_TYPE_DEFAULT, frame);
	D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &rd);
	t->bytes = info.SizeInBytes != UINT64_MAX ? info.SizeInBytes : 0;

	if (desc.shaderResource) {
		t->srv = AllocSrv(1);
		if (t->srv != UINT32_MAX) {
			D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = desc.srvFormat != DXGI_FORMAT_UNKNOWN ? desc.srvFormat : desc.format;
			sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			if (desc.arraySize > 1) {
				sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				sd.Texture2DArray.MipLevels = desc.mipLevels;
				sd.Texture2DArray.ArraySize = desc.arraySize;
			} else {
				sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				sd.Texture2D.MipLevels = desc.mipLevels;
			}
			WriteSrv(t->srv, t->resource.Get(), &sd);
		}
	}
	if (desc.unorderedAccess) {
		t->uav = AllocSrv(1);
		if (t->uav != UINT32_MAX) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
			ud.Format = desc.srvFormat != DXGI_FORMAT_UNKNOWN ? desc.srvFormat : desc.format;
			ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			// (in both heaps, as WriteSrv does: CopySrv copies from the CPU only one)
			device_->CreateUnorderedAccessView(t->resource.Get(), nullptr, &ud, StagingCpu(t->uav));
			device_->CreateUnorderedAccessView(t->resource.Get(), nullptr, &ud, SrvCpu(t->uav));
		}
	}
	if (desc.renderTarget) {
		t->target = AllocTarget(rtvFree_, rtvNext_, kRtvHeapSize);
		if (t->target != UINT32_MAX) {
			D3D12_RENDER_TARGET_VIEW_DESC vd = {};
			vd.Format = desc.targetFormat;
			vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			device_->CreateRenderTargetView(t->resource.Get(), &vd, RtvCpu(t->target));
		}
	} else if (desc.depthStencil) {
		t->target = AllocTarget(dsvFree_, dsvNext_, kDsvHeapSize);
		if (t->target != UINT32_MAX) {
			D3D12_DEPTH_STENCIL_VIEW_DESC vd = {};
			vd.Format = desc.targetFormat;
			vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			device_->CreateDepthStencilView(t->resource.Get(), &vd, DsvCpu(t->target));
		}
	}
	if (target && t->target == UINT32_MAX) {
		AE_LOG_CODE(Error, "GpuResources", "E_DESCRIPTOR_HEAP_FULL", "No free render target / depth view for %s", name);
	}
	std::lock_guard lock(statsMutex_);
	stats_.texturesCreated++;
	(lifetime == Lifetime::Persistent ? stats_.persistentBytes : stats_.transientBytes) += t->bytes;
	stats_.rtvUsed = rtvNext_ - uint32_t(rtvFree_.size());
	stats_.dsvUsed = dsvNext_ - uint32_t(dsvFree_.size());
	return t;
}

void GpuResources::Transition(ID3D12GraphicsCommandList* list, GpuTexture& texture, D3D12_RESOURCE_STATES state) {
	if (texture.state == state) {
		return;
	}
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = texture.resource.Get();
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = texture.state;
	b.Transition.StateAfter = state;
	list->ResourceBarrier(1, &b);
	texture.state = state;
}

void GpuResources::RetireTexture(std::shared_ptr<GpuTexture> texture, UINT64 fence, uint64_t frame) {
	if (!texture) {
		return;
	}
	ResourceRegistry::Get().Unregister(texture->registryId, frame);
	texture->registryId = 0;
	{
		std::lock_guard lock(statsMutex_);
		uint64_t& bucket = texture->lifetime == Lifetime::Persistent ? stats_.persistentBytes : stats_.transientBytes;
		bucket = bucket >= texture->bytes ? bucket - texture->bytes : 0;
	}
	retiredTextures_.push_back({std::move(texture), fence});
}

void GpuResources::Retire(ComPtr<IUnknown> object, UINT64 fence) {
	if (object) {
		if (!retired_.empty() && fence < retired_.back().fence) {
			AE_LOG_CODE(Error, "Resources", "E_LIFETIME", "An object was retired against serial %llu after one retired against %llu (retirements must be in submission order)", static_cast<unsigned long long>(fence), static_cast<unsigned long long>(retired_.back().fence));
		}
		retired_.push_back({std::move(object), fence});
	}
}

void GpuResources::Collect(UINT64 completedFence, uint64_t frame) {
	(void)frame;
	uint64_t objects = 0, textures = 0;
	// the queues are in submission order: the fence values only grow
	while (!retired_.empty() && retired_.front().fence <= completedFence) {
		retired_.pop_front();
		++objects;
	}
	while (!retiredTextures_.empty() && retiredTextures_.front().fence <= completedFence) {
		GpuTexture& t = *retiredTextures_.front().texture;
		if (t.resource) {
			// Aether created this resource and holds the only reference: anything else means a stray AddRef (it would
			// leak) or a stray Release (the count would already be wrong before this point)
			t.resource->AddRef();
			const ULONG references = t.resource->Release();
			if (references != 1 || retiredTextures_.front().texture.use_count() != 1) {
				AE_LOG_CODE(Error, "Resources", "E_LIFETIME", "Texture %s is released with %lu COM references and %ld owners (expected 1 and 1)", t.name.c_str(), references, retiredTextures_.front().texture.use_count());
			}
		}
		for (uint32_t index : {t.srv, t.uav}) {
			if (index != UINT32_MAX) {
				srvFree_.push_back({index, 1});
				std::lock_guard lock(statsMutex_);
				stats_.srvUsed -= 1;
			}
		}
		if (t.target != UINT32_MAX) {
			(t.desc.depthStencil ? dsvFree_ : rtvFree_).push_back(t.target);
		}
		retiredTextures_.pop_front();
		++textures;
	}
	while (!retiredRanges_.empty() && retiredRanges_.front().fence <= completedFence) {
		Range r = retiredRanges_.front().range;
		retiredRanges_.pop_front();
		srvFree_.push_back(r);
		std::lock_guard lock(statsMutex_);
		stats_.srvUsed = stats_.srvUsed >= r.count ? stats_.srvUsed - r.count : 0;
	}
	// keep the free list from fragmenting for ever: merge neighbours now and then
	if (srvFree_.size() > 64) {
		std::sort(srvFree_.begin(), srvFree_.end(), [](const Range& a, const Range& b) { return a.start < b.start; });
		std::vector<Range> merged;
		for (const Range& r : srvFree_) {
			if (!merged.empty() && merged.back().start + merged.back().count == r.start) {
				merged.back().count += r.count;
			} else {
				merged.push_back(r);
			}
		}
		srvFree_.swap(merged);
	}
	for (auto& a : arenas_) {
		if (a->busy && a->fence <= completedFence) {
			a->busy = false;
		}
	}
	std::lock_guard lock(statsMutex_);
	stats_.objectsRetired += objects;
	stats_.texturesRetired += textures;
	stats_.pending = PendingRetirements();
	stats_.rtvUsed = rtvNext_ - uint32_t(rtvFree_.size());
	stats_.dsvUsed = dsvNext_ - uint32_t(dsvFree_.size());
}

// ---- frame memory

bool GpuResources::BeginFrame(UINT64 completedFence, uint64_t frame) {
	arena_ = nullptr;
	for (auto& a : arenas_) {
		if (a->busy && a->fence <= completedFence) {
			a->busy = false;
		}
		if (!a->busy && !arena_) {
			arena_ = a.get();
		}
	}
	if (!arena_) {
		if (arenas_.size() >= kMaxArenas) {
			return false;
		}
		auto a = std::make_unique<Arena>();
		D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = kArenaSize;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&a->buffer));
		void* p = nullptr;
		if (SUCCEEDED(hr)) {
			hr = a->buffer->Map(0, nullptr, &p);
		}
		if (FAILED(hr)) {
			Log::Get().Write(Severity::Error, "GpuResources", "E_RESOURCE_CREATE", "A frame upload arena could not be created", "FrameArena", {{"hresult", HrToString(hr)}});
			return false;
		}
		a->mapped = static_cast<uint8_t*>(p);
		a->size = kArenaSize;
		a->registryId = ResourceRegistry::Get().Register(device_, a->buffer.Get(), Format("FrameArena%zu", arenas_.size()).c_str(), ResourceCategory::Upload, D3D12_HEAP_TYPE_UPLOAD, frame);
		arena_ = a.get();
		arenas_.push_back(std::move(a));
	}
	arena_->used = 0;
	return true;
}

void GpuResources::EndFrame(UINT64 fence) {
	if (!arena_) {
		return;
	}
	arena_->busy = true;
	arena_->fence = fence;
	arena_->peak = std::max(arena_->peak, arena_->used);
	std::lock_guard lock(statsMutex_);
	stats_.arenaUsed = arena_->used;
	stats_.arenaPeak = std::max<uint64_t>(stats_.arenaPeak, arena_->used);
	stats_.arenaReserved = uint64_t(arenas_.size()) * kArenaSize;
	stats_.arenaCount = uint32_t(arenas_.size());
	arena_ = nullptr;
}

UploadAllocation GpuResources::Upload(size_t bytes, size_t alignment) {
	UploadAllocation out;
	if (!arena_) {
		return out;
	}
	size_t offset = (arena_->used + alignment - 1) & ~(alignment - 1);
	if (offset + bytes > arena_->size) {
		AE_LOG_CODE(Error, "GpuResources", "E_FRAME_ARENA_FULL", "The frame upload arena is full (%zu bytes asked, %zu of %zu used)", bytes, arena_->used, arena_->size);
		return out;
	}
	arena_->used = offset + bytes;
	out.cpu = arena_->mapped + offset;
	out.gpu = arena_->buffer->GetGPUVirtualAddress() + offset;
	out.resource = arena_->buffer.Get();
	out.offset = offset;
	return out;
}

// ---- accounting

uint64_t GpuResources::OwnedGpuBytes() {
	std::lock_guard lock(statsMutex_);
	return stats_.persistentBytes + stats_.transientBytes;
}

json GpuResources::Report() {
	Stats s;
	{
		std::lock_guard lock(statsMutex_);
		s = stats_;
	}
	SettingsData settings = Settings::Get().Snapshot();
	const uint64_t owned = s.persistentBytes + s.transientBytes;
	const uint64_t budget = uint64_t(settings.vramBudgetMB) << 20;
	const double used = budget ? double(owned) / double(budget) : 0.0;
	json j = {
		{"persistentBytes", s.persistentBytes},
		{"transientBytes", s.transientBytes},
		{"ownedGpuBytes", owned},
		{"frameArenas", {{"count", s.arenaCount}, {"reservedBytes", s.arenaReserved}, {"usedLastFrameBytes", s.arenaUsed}, {"peakUsedBytes", s.arenaPeak}, {"memory", "system (upload heaps)"}}},
		{"descriptors", {{"srvUsed", s.srvUsed}, {"srvPeak", s.srvPeak}, {"srvCapacity", kSrvHeapSize}, {"rtvUsed", s.rtvUsed}, {"rtvCapacity", kRtvHeapSize}, {"dsvUsed", s.dsvUsed}, {"dsvCapacity", kDsvHeapSize}}},
		{"texturesCreated", s.texturesCreated},
		{"texturesRetired", s.texturesRetired},
		{"objectsRetired", s.objectsRetired},
		{"pendingRetirements", s.pending},
		{"budget", {{"budgetBytes", budget}, {"usedFraction", used}, {"warnFraction", settings.vramWarnPercent / 100.0},
			{"state", budget == 0 ? "no budget set" : used >= 1.0 ? "over budget" : used >= settings.vramWarnPercent / 100.0 ? "warning" : "ok"},
			{"note", "A crossed threshold is reported, never acted on by freeing resources."}}},
	};
	return j;
}

} // namespace aether
