// Every GPU resource Aether creates is registered here: name, category, size, when it was made. This is the base of the
// VRAM monitor (Aether's own share of the process's video memory) and, from Phase 2, of the GPU resource manager.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"

namespace aether {

enum class ResourceCategory { Texture, Buffer, RenderTarget, DepthBuffer, ShadowMap, Reflection, History, Readback, Upload, Other, Count };
const char* CategoryName(ResourceCategory c);

struct ResourceRecord {
	uint64_t id = 0;
	std::string name;
	ResourceCategory category = ResourceCategory::Other;
	uint64_t bytes = 0;
	uint32_t width = 0, height = 0, depthOrArray = 0, mips = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT;
	uint64_t createdFrame = 0;
	double createdTime = 0; // QpcSeconds
};

class ResourceRegistry {
public:
	static ResourceRegistry& Get();
	// Returns the record id (0 if resource is null). Size comes from the device's allocation info for the resource's description.
	uint64_t Register(ID3D12Device* device, ID3D12Resource* resource, const char* name, ResourceCategory category, D3D12_HEAP_TYPE heap, uint64_t frame);
	void Unregister(uint64_t id, uint64_t frame);
	void Clear();

	json List(size_t maxCount = 500, const std::string& category = {});
	json Summary(); // totals per category, counts, allocation / release counters, recent events
	uint64_t TotalBytes();
	uint64_t GpuBytes(); // without readback / upload heaps (system memory)
	uint64_t CategoryBytes(ResourceCategory c);
	size_t Count();
	std::vector<ResourceRecord> Snapshot();

private:
	struct Event { bool allocation; uint64_t id; std::string name; uint64_t bytes; uint64_t frame; double time; };
	std::mutex mutex_;
	std::vector<ResourceRecord> records_;
	std::vector<Event> events_; // ring, newest last (bounded)
	uint64_t nextId_ = 1;
	uint64_t allocations_ = 0, releases_ = 0;
};

} // namespace aether
