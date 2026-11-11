// The seam between Aether's renderer and whoever owns the D3D12 device. In the game that is Unity
// (IUnityGraphicsD3D12v7, Device/UnityBridge.cpp); in tests it is a small window with its own device (tests/host).
// This is not a graphics API abstraction: everything behind it is D3D12.
#pragma once
#include "../Core/Common.h"

namespace aether {

struct HostResourceState {
	ID3D12Resource* resource;
	D3D12_RESOURCE_STATES expected; // state the resource must be in when the command list starts
	D3D12_RESOURCE_STATES current;  // state the command list leaves it in
};

// All members are called on the host's render / submission thread only.
struct HostGfx {
	const char* name = "none";
	ID3D12Device* device = nullptr;      // owned by the host
	ID3D12CommandQueue* queue = nullptr; // the host's direct queue (owned by the host)
	// The swap chain the host presents (may change; asked every frame). Not AddRef'ed for the caller.
	std::function<IDXGISwapChain*()> swapChain;
	// Hands a closed direct command list to the host for execution in submission order. The host transitions the
	// declared resources to `expected` first and records `current` afterwards. Returns the frame fence value that is
	// signalled once the list has run (the list and its allocator must stay untouched until then).
	std::function<UINT64(ID3D12GraphicsCommandList*, int, HostResourceState*)> execute;
	std::function<UINT64()> completedFenceValue; // the host frame fence's completed value
	std::function<UINT64()> nextFenceValue;      // the value the current frame will signal
	// Waits until the GPU has finished everything submitted to the queue so far (the host's own fence signal on the
	// queue). Only meaningful once the host has really submitted Aether's lists (in Unity: inside an event configured
	// to flush command buffers). False on timeout.
	std::function<bool(uint32_t timeoutMs)> waitIdle;
};

} // namespace aether
