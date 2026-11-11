#include "GpuTimer.h"
#include "../Resources/ResourceRegistry.h"

namespace aether {

bool GpuTimer::Init(ID3D12Device* device, ID3D12CommandQueue* queue, uint64_t frame) {
	if (heap_) {
		return true;
	}
	D3D12_QUERY_HEAP_DESC qh = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kFrames * kStamps, 0};
	HRESULT hr = device->CreateQueryHeap(&qh, IID_PPV_ARGS(&heap_));
	if (SUCCEEDED(hr)) {
		D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = kFrames * kStamps * sizeof(UINT64);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_));
	}
	void* p = nullptr;
	if (SUCCEEDED(hr)) {
		hr = readback_->Map(0, nullptr, &p);
	}
	if (SUCCEEDED(hr)) {
		hr = queue->GetTimestampFrequency(&frequency_);
	}
	if (FAILED(hr) || frequency_ == 0) {
		heap_.Reset();
		readback_.Reset();
		return false;
	}
	mapped_ = static_cast<UINT64*>(p);
	registryId_ = ResourceRegistry::Get().Register(device, readback_.Get(), "Scene.TimestampReadback", ResourceCategory::Readback, D3D12_HEAP_TYPE_READBACK, frame);
	for (Slot& s : slots_) {
		s = Slot();
	}
	current_ = -1;
	return true;
}

void GpuTimer::Release(bool leak) {
	if (leak) {
		heap_.Detach();
		readback_.Detach();
	} else if (readback_ && mapped_) {
		readback_->Unmap(0, nullptr);
	}
	heap_.Reset();
	readback_.Reset();
	mapped_ = nullptr;
	current_ = -1;
	registryId_ = 0;
}

bool GpuTimer::Begin() {
	current_ = -1;
	count_ = 0;
	if (!heap_) {
		return false;
	}
	for (uint32_t i = 0; i < kFrames; ++i) {
		if (!slots_[i].pending) {
			current_ = int(i);
			return true;
		}
	}
	return false;
}

uint32_t GpuTimer::Stamp(ID3D12GraphicsCommandList* list) {
	if (current_ < 0 || count_ >= kStamps) {
		return UINT32_MAX;
	}
	list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(current_) * kStamps + count_);
	return count_++;
}

void GpuTimer::Resolve(ID3D12GraphicsCommandList* list) {
	if (current_ >= 0 && count_ > 0) {
		list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(current_) * kStamps, count_, readback_.Get(), UINT64(current_) * kStamps * sizeof(UINT64));
	}
}

void GpuTimer::Submit(UINT64 fence, uint64_t tag) {
	if (current_ >= 0 && count_ > 0) {
		slots_[current_] = {true, fence, tag, count_};
	}
	current_ = -1;
}

void GpuTimer::Collect(UINT64 completedFence, const std::function<void(uint64_t, const std::vector<double>&)>& done) {
	if (!heap_ || !mapped_) {
		return;
	}
	std::vector<double> ms;
	for (uint32_t i = 0; i < kFrames; ++i) {
		Slot& s = slots_[i];
		if (!s.pending || s.fence > completedFence) {
			continue;
		}
		s.pending = false;
		const UINT64* v = mapped_ + i * kStamps;
		ms.assign(s.count, 0.0);
		for (uint32_t k = 0; k < s.count; ++k) {
			ms[k] = v[k] >= v[0] ? double(v[k] - v[0]) * 1000.0 / double(frequency_) : 0.0;
		}
		done(s.tag, ms);
	}
}

} // namespace aether
