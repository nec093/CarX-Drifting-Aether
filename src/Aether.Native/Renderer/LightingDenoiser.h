// The ray traced lighting's denoiser: NVIDIA NRD's SIGMA_SHADOW (the sun's shadow, soft by the sun's size) and
// REBLUR_DIFFUSE (ambient occlusion from short rays, and the light those rays found on nearby surfaces), at half
// resolution (the rays are traced for one pixel of every 2 x 2 block), run through NrdInstance; the resolve lays the
// result over the full picture by depth.
//
// SceneRenderer traces one sun ray and one short ray per 2 x 2 block after the depth prepass (which also writes the normal
// and the motion: the guides); one graph pass then records:
//   1. Prepare (Aether's, NrdPasses.hlsl CSLightPrepare): NRD's guides and noisy inputs
//   2. NRD's dispatches
//   3. Resolve (CSLightResolve): what the opaque pass reads: the sun's visibility, and g_AO's layout (rgb bounced
//      light, a ambient occlusion)
// Built only with NRD (AETHER_HAS_NRD). Render thread only.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"
#include "../Resources/GpuResources.h"

#include <memory>
#include <mutex>

namespace aether {

class LightingDenoiser {
public:
	static LightingDenoiser& Get();
	static bool Available(); // built with NRD

	struct Frame {
		const float* worldToView = nullptr;     // Mat4::m (column major, column vectors), this frame
		const float* viewToClip = nullptr;      // without the temporal antialiasing's jitter
		const float* worldToViewPrev = nullptr; // the frame before
		const float* viewToClipPrev = nullptr;
		const float* invViewProj = nullptr;     // what the passes reconstruct positions with (jittered)
		float sunDirection[3] = {0, 1, 0};      // towards the sun
		float tanSunRadius = 0.0047f;           // the sun's angular radius (its tangent): the penumbra's width
		float aoRadius = 1.0f;                  // m: the short rays' length
		float aoStrength = 1.0f;                // the setting aoStrength
		bool gi = false;                        // the rays are long and bring all the ambient light (rayTracedGi)
		bool restart = false;                   // the history does not apply (another camera, a cut)
		float fps = 60.0f;
		UINT64 retireFence = 0;                 // this frame's: the descriptors it uses are reused after it
	};

	// The two full resolution results for this size (made here; the graph imports them). False: not usable.
	bool Prepare(ID3D12Device* device, uint32_t width, uint32_t height, UINT64 retireFence, uint64_t frame);
	std::shared_ptr<GpuTexture> Ambient() const; // rgb bounced light, a ambient occlusion (g_AO)
	std::shared_ptr<GpuTexture> Shadow() const;  // the sun's visibility
	// Records the three steps. The graph has put the inputs in NON_PIXEL_SHADER_RESOURCE and the two results in
	// UNORDERED_ACCESS; those are not transitioned here.
	bool Record(ID3D12GraphicsCommandList* list, const Frame& f, GpuTexture& depth, GpuTexture& normal, GpuTexture& velocity, GpuTexture& sunRays, GpuTexture& aoRays,
		std::string& note);
	void Release(UINT64 fence, bool leak);
	json Stats();

private:
	struct Impl;
	Impl* impl_ = nullptr;
	std::mutex statsMutex_;
	json stats_;
};

} // namespace aether
