// The GPU resource manager: everything Aether allocates on the device goes through here.
//  - descriptors: one shader-visible CBV/SRV/UAV heap, an RTV heap and a DSV heap, with range allocators;
//  - textures Aether owns (render targets, depth, shadow atlas, small uploaded textures), with their resource state
//    tracked so that transitions are explicit;
//  - frame memory: upload arenas (constants, instance data, light lists), one per frame in flight;
//  - retirement: nothing that a submitted command list may reference is released before the host's frame fence
//    passed that list's value (Retire / Collect);
//  - accounting: persistent / frame / transient, allocated vs reserved, against a configurable budget.
// Resources Aether only borrows from Unity (mesh buffers, textures) are referenced by the scene store, never owned
// here; they are counted as "shared". Render thread only, except Report().
// docs/architecture/gpu-resources.md
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"
#include "ResourceRegistry.h"

#include <deque>

namespace aether {

enum class Lifetime { Persistent, Transient, Frame };

struct GpuTextureDesc {
	uint32_t width = 1, height = 1;
	uint16_t arraySize = 1, mipLevels = 1;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;       // of the resource
	DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;    // of the views (depth textures: typeless resource, typed views)
	DXGI_FORMAT targetFormat = DXGI_FORMAT_UNKNOWN; // RTV or DSV format
	bool renderTarget = false, depthStencil = false, shaderResource = true;
	bool unorderedAccess = false; // written by compute shaders (a UAV in the SRV heap: GpuTexture::uav)
	float clearColor[4] = {0, 0, 0, 0};
	float clearDepth = 0; // reversed Z: far
	bool operator==(const GpuTextureDesc& o) const {
		return width == o.width && height == o.height && arraySize == o.arraySize && mipLevels == o.mipLevels && format == o.format && srvFormat == o.srvFormat &&
			targetFormat == o.targetFormat && renderTarget == o.renderTarget && depthStencil == o.depthStencil && shaderResource == o.shaderResource && unorderedAccess == o.unorderedAccess;
	}
};

struct GpuTexture {
	ComPtr<ID3D12Resource> resource;
	GpuTextureDesc desc;
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON; // as the next recorded command will find it
	uint32_t srv = UINT32_MAX;    // index in the SRV heap
	uint32_t target = UINT32_MAX; // index in the RTV or DSV heap
	uint32_t uav = UINT32_MAX;    // index in the SRV heap (desc.unorderedAccess)
	uint64_t registryId = 0;
	uint64_t bytes = 0;
	std::string name;
	Lifetime lifetime = Lifetime::Persistent;
	uint64_t lastUsedFrame = 0;
};

struct UploadAllocation {
	void* cpu = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
	ID3D12Resource* resource = nullptr;
	uint64_t offset = 0;
};

class GpuResources {
public:
	static GpuResources& Get();
	bool Init(ID3D12Device* device);
	// leak = true: the GPU may still be using the objects (shutdown without a confirmed idle GPU)
	void Shutdown(bool leak);
	bool Ready() const { return device_ != nullptr; }
	ID3D12Device* Device() const { return device_; }

	// ---- descriptors
	static const uint32_t kSrvHeapSize = 16384;
	ID3D12DescriptorHeap* SrvHeap() const { return srvHeap_.Get(); }
	uint32_t AllocSrv(uint32_t count = 1); // UINT32_MAX when the heap is full
	void FreeSrv(uint32_t index, uint32_t count, UINT64 fence); // reusable once the fence has passed
	// Views are written to a CPU-only staging heap and to the shader-visible heap at the same index: a shader-visible
	// heap is write-only for the CPU, so descriptor copies (material tables, per-frame tables) read from the staging one.
	void WriteSrv(uint32_t index, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc);
	void CopySrv(uint32_t destination, uint32_t source);
	D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(uint32_t index) const;
	D3D12_CPU_DESCRIPTOR_HANDLE RtvCpu(uint32_t index) const;
	D3D12_CPU_DESCRIPTOR_HANDLE DsvCpu(uint32_t index) const;

	// ---- textures Aether owns
	std::shared_ptr<GpuTexture> CreateTexture(const GpuTextureDesc& desc, const char* name, ResourceCategory category, Lifetime lifetime, uint64_t frame);
	// Records the transition (nothing if the texture is already there).
	void Transition(ID3D12GraphicsCommandList* list, GpuTexture& texture, D3D12_RESOURCE_STATES state);
	// Hands the texture to the retirement queue: freed when the fence has passed.
	void RetireTexture(std::shared_ptr<GpuTexture> texture, UINT64 fence, uint64_t frame);

	// ---- retirement of anything else a command list may still use (also Unity's objects Aether held a reference on)
	void Retire(ComPtr<IUnknown> object, UINT64 fence);
	// Releases what the GPU is done with. Call once per frame with the host fence's completed value.
	void Collect(UINT64 completedFence, uint64_t frame);
	size_t PendingRetirements() const { return retired_.size() + retiredTextures_.size() + retiredRanges_.size(); }

	// ---- frame memory
	// A fresh arena for this frame's lists (null when none is free: the host fence is not advancing).
	bool BeginFrame(UINT64 completedFence, uint64_t frame);
	// The arena used this frame is busy until `fence`.
	void EndFrame(UINT64 fence);
	UploadAllocation Upload(size_t bytes, size_t alignment = 256);

	// ---- accounting (any thread)
	json Report();
	uint64_t OwnedGpuBytes();

private:
	struct Range { uint32_t start, count; };
	struct RetiredRange { Range range; UINT64 fence; };
	struct RetiredObject { ComPtr<IUnknown> object; UINT64 fence; };
	struct RetiredTexture { std::shared_ptr<GpuTexture> texture; UINT64 fence; };
	struct Arena {
		ComPtr<ID3D12Resource> buffer;
		uint8_t* mapped = nullptr;
		size_t size = 0, used = 0, peak = 0;
		UINT64 fence = 0;
		bool busy = false;
		uint64_t registryId = 0;
	};
	uint32_t AllocTarget(std::vector<uint32_t>& freeList, uint32_t& next, uint32_t capacity);

	ID3D12Device* device_ = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(uint32_t index) const;
	D3D12_CPU_DESCRIPTOR_HANDLE StagingCpu(uint32_t index) const;
	ComPtr<ID3D12DescriptorHeap> srvHeap_, srvStaging_, rtvHeap_, dsvHeap_;
	uint32_t srvSize_ = 0, rtvSize_ = 0, dsvSize_ = 0;
	std::vector<Range> srvFree_;
	std::deque<RetiredRange> retiredRanges_;
	std::vector<uint32_t> rtvFree_, dsvFree_;
	uint32_t rtvNext_ = 0, dsvNext_ = 0;
	std::deque<RetiredObject> retired_;
	std::deque<RetiredTexture> retiredTextures_;
	std::vector<std::unique_ptr<Arena>> arenas_;
	Arena* arena_ = nullptr;

	// statistics shared with other threads
	std::mutex statsMutex_;
	struct Stats {
		uint32_t srvUsed = 0, srvPeak = 0, rtvUsed = 0, dsvUsed = 0;
		uint64_t texturesCreated = 0, texturesRetired = 0, objectsRetired = 0;
		uint64_t arenaReserved = 0, arenaUsed = 0, arenaPeak = 0;
		uint32_t arenaCount = 0;
		size_t pending = 0;
		uint64_t persistentBytes = 0, transientBytes = 0;
	} stats_;
};

} // namespace aether
