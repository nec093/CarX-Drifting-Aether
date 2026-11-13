// GPU pass timing with timestamp queries for one command list per frame: stamps are recorded between passes, resolved
// at the end of the list, and read back once the host's frame fence has passed the list's value. Render thread only.
#pragma once
#include "../Core/Common.h"

namespace aether {

class GpuTimer {
public:
	static const uint32_t kFrames = 16, kStamps = 128; // (two stamps a pass: the scene graph has about forty passes)

	bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, uint64_t frame);
	void Release(bool leak);
	bool Ready() const { return heap_ != nullptr; }

	// Picks a free frame slot. False = every slot is still waiting for the GPU (no timing this frame).
	bool Begin();
	// Records a timestamp and returns its index in this frame (UINT32_MAX when timing is off or the slot is full).
	uint32_t Stamp(ID3D12GraphicsCommandList* list);
	// Resolves this frame's stamps into the readback buffer. Call last in the list.
	void Resolve(ID3D12GraphicsCommandList* list);
	// The list was submitted with this fence value; `tag` comes back with the results.
	void Submit(UINT64 fence, uint64_t tag);
	// Calls `done(tag, milliseconds since the frame's first stamp, one per stamp)` for every finished frame.
	void Collect(UINT64 completedFence, const std::function<void(uint64_t, const std::vector<double>&)>& done);

private:
	struct Slot { bool pending = false; UINT64 fence = 0; uint64_t tag = 0; uint32_t count = 0; };
	ComPtr<ID3D12QueryHeap> heap_;
	ComPtr<ID3D12Resource> readback_;
	UINT64* mapped_ = nullptr;
	UINT64 frequency_ = 0;
	uint64_t registryId_ = 0;
	Slot slots_[kFrames];
	int current_ = -1;
	uint32_t count_ = 0;
};

} // namespace aether
