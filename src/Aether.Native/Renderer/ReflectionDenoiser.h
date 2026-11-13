// The reflection rays' denoiser: NVIDIA's NRD (REBLUR_SPECULAR), run with Aether's own D3D12 (no NRI).
//
// The rays are traced for one pixel of every 2 x 2 block (SceneRenderer's ReflectionRays pass), along directions
// sampled from the surface's GGX lobe. One graph pass then records:
//   1. Prepare (Aether's, NrdPasses.hlsl): NRD's guides and noisy signal at that quarter resolution
//   2. NRD's own dispatches (its pipelines, texture pools and constants, as GetComputeDispatches lists them)
//   3. Resolve (Aether's): NRD's output in the layout the reflection pass reads (rgb radiance, a view depth)
// NRD keeps its history in its permanent pool: the instance is made again when the size changes.
//
// Built only when NRD is there (AETHER_HAS_NRD: scripts/fetch-deps.ps1); otherwise Available() is false.
// Render thread only.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"
#include "../Resources/GpuResources.h"

#include <memory>
#include <mutex>
#include <vector>

namespace aether {

class ReflectionDenoiser {
public:
	static ReflectionDenoiser& Get();
	static bool Available(); // built with NRD

	struct Frame {
		const float* worldToView = nullptr;     // Mat4::m (column major, column vectors), this frame
		const float* viewToClip = nullptr;      // without the temporal antialiasing's jitter
		const float* worldToViewPrev = nullptr; // the frame before
		const float* viewToClipPrev = nullptr;
		const float* invViewProj = nullptr;     // what the reflection passes reconstruct positions with (jittered)
		uint32_t width = 0, height = 0;         // the full resolution (the rays have half of it)
		bool restart = false;                   // the history does not apply (another camera, a cut)
		float fps = 60.0f;                      // for an accumulation measured in seconds
	};

	// The quarter resolution target the reflection pass reads; made by Prepare for this size. Null: not usable.
	std::shared_ptr<GpuTexture> Prepare(ID3D12Device* device, uint32_t width, uint32_t height, UINT64 retireFence, uint64_t frame);
	// Records the three steps. The graph has put depth, surface, velocity and rays in NON_PIXEL_SHADER_RESOURCE and
	// the output (Prepare's texture) in UNORDERED_ACCESS; those are not transitioned here.
	bool Record(ID3D12GraphicsCommandList* list, const Frame& f, GpuTexture& depth, GpuTexture& surface, GpuTexture& velocity, GpuTexture& rays, GpuTexture& output, std::string& note);
	void Release(UINT64 fence, bool leak);
	json Stats();

private:
	struct Impl;
	Impl* impl_ = nullptr;
	std::mutex statsMutex_;
	json stats_; // (Stats is asked from other threads: a copy made on the render thread)
};

} // namespace aether
