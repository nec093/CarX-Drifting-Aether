// One NVIDIA NRD instance run with Aether's own D3D12 (no NRI): its denoisers, their pipelines, the texture pools,
// the descriptors for the dispatches' tables (blocks taken per frame, reused only once that frame's fence has passed),
// and the dispatches NRD lists every frame. The caller owns the
// inputs and outputs (NRD's IN_ / OUT_ resources) and hands them over by type for each run; it also uses the helpers
// here for its own compute passes around NRD (one root signature: b0, t0..t5, u0..u5).
//
// Built only with NRD (AETHER_HAS_NRD). Render thread only. NRD cannot be resized: Create again for a new size.
#pragma once
#include "../Core/Common.h"
#include "../Resources/GpuResources.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aether {

class NrdInstance {
public:
	NrdInstance() = default;
	~NrdInstance();
	NrdInstance(const NrdInstance&) = delete;
	NrdInstance& operator=(const NrdInstance&) = delete;

	// `denoisers`: (identifier, nrd::Denoiser) pairs. False: not usable (the reason is in the log).
	bool Create(ID3D12Device* device, const std::vector<std::pair<uint32_t, uint32_t>>& denoisers, uint32_t width, uint32_t height, uint64_t frame, const char* name);
	bool Ready() const { return instance_ != nullptr; }
	// Before a frame's Run / OwnPass: the descriptors taken from here on go back for reuse once `retireFence` has passed.
	// (A ring reused round robin was overwritten while the GPU was still several frames behind with the lists that read
	// it: the device hung.)
	void BeginFrame(UINT64 retireFence);
	uint32_t Width() const { return width_; }
	uint32_t Height() const { return height_; }
	uint64_t TextureBytes() const { return poolBytes_; }

	// Sets the settings (`common`: nrd::CommonSettings; `settings`: per identifier, the denoiser's settings struct) and
	// records the dispatches of `identifiers`. `resource(nrd::ResourceType)` returns the caller's texture for an IN_ /
	// OUT_ type. Textures are transitioned here (GpuResources::Transition). False: `error` says why.
	bool Run(ID3D12GraphicsCommandList* list, const void* common, const std::vector<std::pair<uint32_t, const void*>>& settings,
		const std::function<GpuTexture*(uint32_t)>& resource, uint32_t& dispatches, std::string& error);

	// The callers' own passes: one root signature for all of them (b0 constants, t0..t5, u0..u5, space 0).
	ID3D12RootSignature* OwnRoot() const { return ownRoot_.Get(); }
	ComPtr<ID3D12PipelineState> OwnPipeline(const void* code, size_t size, const char* what) const;
	// Records one dispatch of an own pass over width x height (8 x 8 groups). `constants`: a GPU address (b0).
	void OwnPass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS constants, std::initializer_list<GpuTexture*> srvs,
		std::initializer_list<GpuTexture*> uavs, uint32_t width, uint32_t height);
	// A texture for the caller's NRD inputs and outputs: shader resource and unordered access.
	static std::shared_ptr<GpuTexture> MakeTexture(DXGI_FORMAT format, uint32_t width, uint32_t height, const char* name, uint64_t frame);

	void Release(UINT64 fence, bool leak);

private:
	uint32_t Take(uint32_t count);
	ID3D12Device* device_ = nullptr;
	void* instance_ = nullptr; // nrd::Instance*
	const void* desc_ = nullptr; // const nrd::InstanceDesc*
	ComPtr<ID3D12RootSignature> nrdRoot_, ownRoot_;
	std::vector<ComPtr<ID3D12PipelineState>> pipelines_;
	std::vector<std::shared_ptr<GpuTexture>> permanent_, transient_;
	uint32_t width_ = 0, height_ = 0;
	uint32_t block_ = UINT32_MAX, blockNext_ = 0;
	UINT64 frameFence_ = 0;
	uint64_t poolBytes_ = 0;
	std::string name_;
};

} // namespace aether
