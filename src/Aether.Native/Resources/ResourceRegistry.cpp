#include "ResourceRegistry.h"
#include "../Core/Log.h"

#include <algorithm>

namespace aether {

const char* CategoryName(ResourceCategory c) {
	switch (c) {
	case ResourceCategory::Texture: return "texture";
	case ResourceCategory::Buffer: return "buffer";
	case ResourceCategory::RenderTarget: return "renderTarget";
	case ResourceCategory::DepthBuffer: return "depthBuffer";
	case ResourceCategory::ShadowMap: return "shadowMap";
	case ResourceCategory::Reflection: return "reflection";
	case ResourceCategory::History: return "history";
	case ResourceCategory::Readback: return "readback";
	case ResourceCategory::Upload: return "upload";
	default: return "other";
	}
}

ResourceRegistry& ResourceRegistry::Get() {
	static ResourceRegistry* r = new ResourceRegistry();
	return *r;
}

uint64_t ResourceRegistry::Register(ID3D12Device* device, ID3D12Resource* resource, const char* name, ResourceCategory category, D3D12_HEAP_TYPE heap, uint64_t frame) {
	if (!resource) {
		return 0;
	}
	D3D12_RESOURCE_DESC d = resource->GetDesc();
	ResourceRecord r;
	r.name = name ? name : "";
	r.category = category;
	r.heap = heap;
	r.width = UINT(d.Width);
	r.height = d.Height;
	r.depthOrArray = d.DepthOrArraySize;
	r.mips = d.MipLevels;
	r.format = d.Format;
	r.createdFrame = frame;
	r.createdTime = QpcSeconds();
	if (device) {
		D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &d);
		r.bytes = info.SizeInBytes != UINT64_MAX ? info.SizeInBytes : 0;
	}
	if (r.bytes == 0 && d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
		r.bytes = d.Width;
	}
	if (name) {
		resource->SetName(Utf8ToWide(std::string("Aether.") + name).c_str());
	}

	std::lock_guard lock(mutex_);
	r.id = nextId_++;
	++allocations_;
	events_.push_back({true, r.id, r.name, r.bytes, frame, r.createdTime});
	if (events_.size() > 256) {
		events_.erase(events_.begin(), events_.begin() + 64);
	}
	records_.push_back(r);
	return r.id;
}

void ResourceRegistry::Unregister(uint64_t id, uint64_t frame) {
	if (id == 0) {
		return;
	}
	std::lock_guard lock(mutex_);
	auto it = std::find_if(records_.begin(), records_.end(), [&](const ResourceRecord& r) { return r.id == id; });
	if (it == records_.end()) {
		// every id is handed out once and unregistered once: a second time means two owners released the same resource
		AE_LOG_CODE(Error, "Resources", "E_LIFETIME", "Resource id %llu was unregistered twice (double release) at frame %llu", static_cast<unsigned long long>(id), static_cast<unsigned long long>(frame));
		return;
	}
	++releases_;
	events_.push_back({false, id, it->name, it->bytes, frame, QpcSeconds()});
	if (events_.size() > 256) {
		events_.erase(events_.begin(), events_.begin() + 64);
	}
	records_.erase(it);
}

void ResourceRegistry::Clear() {
	std::lock_guard lock(mutex_);
	releases_ += records_.size();
	records_.clear();
}

std::vector<ResourceRecord> ResourceRegistry::Snapshot() {
	std::lock_guard lock(mutex_);
	return records_;
}

static json RecordJson(const ResourceRecord& r, double now) {
	return {
		{"id", r.id},
		{"name", r.name},
		{"category", CategoryName(r.category)},
		{"bytes", r.bytes},
		{"width", r.width},
		{"height", r.height},
		{"depthOrArraySize", r.depthOrArray},
		{"mipLevels", r.mips},
		{"dxgiFormat", int(r.format)},
		{"heap", r.heap == D3D12_HEAP_TYPE_DEFAULT ? "default" : r.heap == D3D12_HEAP_TYPE_UPLOAD ? "upload" : r.heap == D3D12_HEAP_TYPE_READBACK ? "readback" : "custom"},
		{"createdFrame", r.createdFrame},
		{"ageSeconds", now - r.createdTime},
	};
}

json ResourceRegistry::List(size_t maxCount, const std::string& category) {
	std::vector<ResourceRecord> records = Snapshot();
	std::sort(records.begin(), records.end(), [](const ResourceRecord& a, const ResourceRecord& b) { return a.bytes > b.bytes; });
	json list = json::array();
	double now = QpcSeconds();
	size_t matched = 0;
	for (const ResourceRecord& r : records) {
		if (!category.empty() && category != CategoryName(r.category)) {
			continue;
		}
		++matched;
		if (list.size() < maxCount) {
			list.push_back(RecordJson(r, now));
		}
	}
	return {{"owner", "aether"}, {"count", matched}, {"returned", list.size()}, {"sortedBy", "bytes descending"}, {"resources", list}};
}

json ResourceRegistry::Summary() {
	std::lock_guard lock(mutex_);
	uint64_t bytes[size_t(ResourceCategory::Count)] = {};
	uint32_t counts[size_t(ResourceCategory::Count)] = {};
	uint64_t total = 0;
	for (const ResourceRecord& r : records_) {
		bytes[size_t(r.category)] += r.bytes;
		counts[size_t(r.category)]++;
		total += r.bytes;
	}
	json categories = json::object();
	for (size_t i = 0; i < size_t(ResourceCategory::Count); ++i) {
		categories[CategoryName(ResourceCategory(i))] = {{"bytes", bytes[i]}, {"count", counts[i]}};
	}
	json events = json::array();
	double now = QpcSeconds();
	size_t first = events_.size() > 32 ? events_.size() - 32 : 0;
	for (size_t i = first; i < events_.size(); ++i) {
		const Event& e = events_[i];
		events.push_back({{"type", e.allocation ? "allocation" : "release"}, {"id", e.id}, {"name", e.name}, {"bytes", e.bytes}, {"frame", e.frame}, {"secondsAgo", now - e.time}});
	}
	return {{"totalBytes", total}, {"resourceCount", records_.size()}, {"categories", categories}, {"allocations", allocations_}, {"releases", releases_}, {"recentEvents", events}};
}

uint64_t ResourceRegistry::TotalBytes() {
	std::lock_guard lock(mutex_);
	uint64_t total = 0;
	for (const ResourceRecord& r : records_) {
		total += r.bytes;
	}
	return total;
}

uint64_t ResourceRegistry::GpuBytes() {
	std::lock_guard lock(mutex_);
	uint64_t total = 0;
	for (const ResourceRecord& r : records_) {
		if (r.heap == D3D12_HEAP_TYPE_DEFAULT) {
			total += r.bytes;
		}
	}
	return total;
}

uint64_t ResourceRegistry::CategoryBytes(ResourceCategory c) {
	std::lock_guard lock(mutex_);
	uint64_t total = 0;
	for (const ResourceRecord& r : records_) {
		if (r.category == c) {
			total += r.bytes;
		}
	}
	return total;
}

size_t ResourceRegistry::Count() {
	std::lock_guard lock(mutex_);
	return records_.size();
}

} // namespace aether
