// GPU / adapter information and D3D12 feature support, all determined at run time from the host's device.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"

namespace aether {

struct VideoMemory {
	bool valid = false;
	uint64_t localBudget = 0, localUsage = 0, localReservation = 0, localAvailableForReservation = 0;          // dedicated VRAM, this process
	uint64_t nonLocalBudget = 0, nonLocalUsage = 0, nonLocalReservation = 0, nonLocalAvailableForReservation = 0; // shared GPU memory, this process
};

class GpuInfo {
public:
	static GpuInfo& Get();
	// Render thread, once the device is known. Safe to call again (device change).
	void Capture(ID3D12Device* device);
	void Reset();
	bool Valid();
	json ToJson();              // static description + features (cached at Capture)
	std::string Vendor();       // "NVIDIA" | "AMD" | "Intel" | "Microsoft" | "Unknown"
	std::string Name();
	uint64_t DedicatedVideoMemory();
	// Process-wide numbers from DXGI (includes Unity / HDRP). Any thread.
	VideoMemory QueryVideoMemory();
	// "ok" or the device removed reason. Any thread.
	std::string DeviceState(HRESULT* reason = nullptr);
	json Brief();               // vendor, name, driver: attached to error log entries

private:
	std::mutex mutex_;
	bool valid_ = false;
	json info_;
	std::string vendor_, name_, driver_;
	uint64_t dedicated_ = 0;
	ComPtr<IDXGIAdapter3> adapter_;
	ComPtr<ID3D12Device> device_;
};

} // namespace aether
