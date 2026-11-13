// Aether's scene renderer: a clustered forward ("forward+") pipeline over the scene store's data.
//   ShadowCascades -> DepthPrepass -> (light clustering on the CPU) -> Opaque -> Sky -> Transparent -> Composite
// Everything is recorded into the one command list the caller hands in; the passes are declared in the render graph.
// docs/architecture/forward-plus.md. Render thread only, except Stats().
#pragma once
#include <atomic>
#include <chrono>
#include "../Core/Common.h"
#include "../Core/Config.h"
#include "../Core/Json.h"
#include "../Core/Math.h"
#include "../Device/Host.h"
#include "../Profiler/Profiler.h"
#include "../Scene/SceneStore.h"
#include "GpuTimer.h"
#include "RenderGraph.h"

#include <map>

namespace aether {

class SceneRenderer {
public:
	static SceneRenderer& Get();

	struct Target {
		ID3D12Resource* backBuffer = nullptr;       // in RENDER_TARGET, stays there
		D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = {};
		DXGI_FORMAT backBufferFormat = DXGI_FORMAT_UNKNOWN;
		uint32_t width = 0, height = 0;
	};

	// Applies the pending scene changes and, when `draw` is set and the scene has a camera, records the frame.
	// `retireFence` is the fence value this frame's list will be submitted with.
	// Returns true when the back buffer was written.
	bool Render(const HostGfx& host, ID3D12GraphicsCommandList* list, const Target& target, bool draw, UINT64 retireFence, uint64_t frame, const SettingsData& settings);
	// After the list was submitted.
	void EndFrame(const HostGfx& host, UINT64 fence);
	void Release(bool leak);

	// ---- any thread
	json Stats();
	void SetSceneNight(bool night) { sceneNight_.store(night); }
	// (towards the sun or the moon, as the last frame had it; read without a lock: a torn read is three old or new floats)
	Vec3 SunDirection() const { return sunDirection_; }
	json GraphDescription() { return graph_.Describe(); }

private:
	enum PassKind { kDepth = 0, kDepthAlpha, kShadow, kShadowAlpha, kForward, kForwardBlend, kForwardAdditive, kDecal, kForwardProbe, kPassKindCount };
	enum Cull { kCullBack = 0, kCullFront, kCullNone };
	struct DrawItem {
		const SceneMesh* mesh;
		const SceneMaterial* material;
		uint32_t subMesh;
		uint32_t instanceIndex;
		uint32_t materialIndex;
		uint32_t cull;      // Cull
		float depth;        // view depth of the bounds' centre
		uint64_t sortKey;
		Vec3 center;        // world space bounds
		float radius;
		bool castsShadow;
		bool mirrored;
	};
	struct Cascade { Mat4 viewProj; Mat4 atlasMatrix; float splitFar; float texelWorld; Vec3 center; float radius; D3D12_VIEWPORT viewport; };

	bool EnsureObjects(const HostGfx& host, uint64_t frame);
	bool EnsureShadowAtlas(uint32_t cascadeSize, uint64_t frame, UINT64 retireFence);
	void UploadDefaults(ID3D12GraphicsCommandList* list);
	ID3D12PipelineState* Pipeline(uint32_t layoutId, PassKind kind, uint32_t cull);
	// full screen passes: one pixel shader each (scene_post.hlsl), named in kFullscreenEntry (SceneRenderer.cpp)
	enum FullscreenKind { kFullscreenSky = 0, kFullscreenComposite, kFullscreenAo, kFullscreenAoBlur, kFullscreenExposure, kFullscreenIrradiance, kFullscreenTaa, kFullscreenMotionBlur, kFullscreenBloomDown, kFullscreenBloomUp, kFullscreenSsr, kFullscreenTransmittanceLut, kFullscreenMultiScatterLut, kFullscreenSkyView, kFullscreenCloudNoise, kFullscreenSkyClouds, kFullscreenClouds, kFullscreenFog, kFullscreenFogApply, kFullscreenReflectionBlur, kFullscreenBvhDebug, kFullscreenRayTrace, kFullscreenGiRays, kFullscreenGiIrradiance, kFullscreenGiDepth, kFullscreenLensFlare, kFullscreenSmokeApply, kFullscreenKindCount };
	// draws a full screen pass into `output` (its whole size) with up to three inputs; `params` reach the shader as g_PassParams
	bool Fullscreen(RenderGraph::PassContext& ctx, FullscreenKind kind, GpuTexture* output, GpuTexture* in0, GpuTexture* in1 = nullptr, GpuTexture* in2 = nullptr, const float* params = nullptr, GpuTexture* in3 = nullptr);
	uint32_t postNext_ = 0; // the next free table of this frame's post tables
	std::shared_ptr<GpuTexture> irradiance_; // the sky's diffuse light per direction (octahedral map)
	// temporal antialiasing: the resolved picture of the frame before, and the camera it was drawn with
	std::shared_ptr<GpuTexture> history_[2];
	uint32_t historyIndex_ = 0, historyCamera_ = 0;
	bool historyValid_ = false;
	Mat4 prevViewProj_ = Mat4::Identity();
	bool taa_ = false;
	float bloomMix_ = 0, bloomScatter_ = 0, motionBlur_ = 0;
	bool ownExposure_ = false;
	bool ssr_ = false; // screen space reflections this frame (the host has them on, and Aether's switch)
	// The physical sky (setting skyMode = physical): Aether's own atmosphere instead of the scene's sky texture, lit by
	// a sun whose place follows the time of day. The first two LUTs are made once, the sky view every frame.
	bool physicalSky_ = false;
	bool atmosphereLutsValid_ = false;
	float atmosphereScale_ = 1.0f; // the atmosphere's unit (sun illuminance 1 above the air) in scene units
	std::atomic<bool> sceneNight_{false}; // the game's own day / night switch is on night
	float moonStrength_ = 0.0f;    // the atmosphere's unit while the moon is the light (scene units)
	float sceneSunLum_ = 0.0f;     // the scene's own brightest directional light (its sun by day, its moon at night)
	float moonShare_ = 0.0f;       // 0: the light in the sky is the sun; up to 1: it is the moon (night)
	std::shared_ptr<GpuTexture> transmittanceLut_, multiScatterLut_, skyViewLut_;
	// clouds (physical sky): the noise their shape comes from (made once), and the sky with the clouds in it, which
	// is what reflections and the sky's diffuse light read
	std::shared_ptr<GpuTexture> cloudNoise_, skyCloudsLut_;
	bool clouds_ = false;
	bool volumetricFog_ = false; // this frame: the fog is marched through the shadow map instead of computed per surface
	// Reflection probes: the still surroundings drawn into cubes that stay where they are in the world (a grid of
	// places around the camera), a face or two per frame, kept while the camera is near. Every cube is six tiles in
	// one atlas, with its depth (where a mirrored ray from a surface ends) and a blurred copy for rough surfaces.
	// No vehicles and nothing that moves in them; the sky is not in them either (alpha 0 = look the sky up).
	bool reflection_ = false;
	struct LightProbe {
		int cell[3] = {0, 0, 0};
		Vec3 position = {0, 0, 0};
		uint32_t facesDone = 0;  // bit per face drawn since the probe took this place (0x3F: usable)
		uint32_t nextFace = 0;
		uint64_t lastWanted = 0; // the last frame the camera was near
		uint64_t lastDrawn = 0;
		uint64_t wholeSince = 0; // the frame its last missing face was drawn (0: not whole yet): it fades in from there
		bool used = false;
	};
	static const uint32_t kProbeSlots = 8;
	LightProbe lightProbes_[kProbeSlots];
	Vec3 probeCameraBefore_ = {0, 0, 0};
	Vec3 probeAhead_ = {0, 0, 0}; // where the camera is heading (its smoothed travel over about half a second)
	bool probeCameraValid_ = false;
	struct ProbeFaceJob { uint32_t slot = 0, face = 0; D3D12_GPU_VIRTUAL_ADDRESS cb = 0; float planes[6][4] = {}; };
	ProbeFaceJob probeJobs_[2];
	uint32_t probeJobCount_ = 0;
	void UpdateProbes(const SceneFrame& scene, uint64_t frame);
	std::shared_ptr<GpuTexture> reflectionAtlas_[2], reflectionDepth_; // [0] the cubes, [1] blurred
	std::vector<DrawItem> probeItems_;
	const float (*cullPlanes_)[4] = nullptr; // DrawItems: only what these planes let through
	uint32_t probeGlobalsTable_ = UINT32_MAX;
	// probe grid GI: this frame's rays, the probes' irradiance and their distances (each probe a tile of 8 x 8)
	std::shared_ptr<GpuTexture> giRays_, giIrradiance_, giDepth_;
	bool gi_ = false, giHistory_ = false;
	bool lensFlare_ = false;
	float lensFlareStrength_ = 0.0f;
	int giMin_[3] = {0, 0, 0}; // the grid cell of the volume's lowest corner
	bool bvhReady_ = false;      // this frame: rays can be traced through the scene's triangles (Bvh.h)
	bool raysWanted_ = false;    // this frame: something traces rays (reflections, a debug view)
	bool rayReflections_ = false; // this frame: smooth surfaces mirror what rays find
	std::vector<uint32_t> rayMaterials_; // per ray instance and sub mesh, two numbers: its index in this frame's material buffer, its base texture's place in the SRV heap (UINT32_MAX: none)
	D3D12_GPU_VIRTUAL_ADDRESS rayMaterialsVa_ = 0;
	bool indirect_ = false;      // this frame: the occlusion pass also gathers light bounced off what it finds
	std::chrono::steady_clock::time_point cloudClock_ = std::chrono::steady_clock::now();
	float cloudDrift_ = 0.0f;
	// automatic exposure: the value of the frame before (1 x 1), which this frame's metering moves from
	std::shared_ptr<GpuTexture> exposureHistory_[2];
	uint32_t exposureIndex_ = 0;
	bool exposureValid_ = false;
	std::chrono::steady_clock::time_point exposureTime_{};
	bool frameSky_ = false; // this frame has the host's sky cube
	uint32_t probesTable_ = UINT32_MAX, probesInFrame_ = 0;
	// instancing: every draw covers a run of this frame's instance map (indices into the instance buffer)
	uint32_t* instanceMap_ = nullptr;
	uint32_t instanceMapUsed_ = 0, instanceMapCapacity_ = 0, itemsDrawn_ = 0, itemsInstanced_ = 0;
	D3D12_GPU_VIRTUAL_ADDRESS instanceMapVa_ = 0;
	ID3D12PipelineState* FullscreenPipeline(FullscreenKind kind, DXGI_FORMAT format);
	bool aoActive_ = false;
	void BuildDrawLists(const SceneFrame& scene, uint64_t frame);
	void BuildCascades(const SceneFrame& scene, const SettingsData& settings);
	void BuildClusters(const SceneFrame& scene);
	bool UploadFrameData(const SceneFrame& scene, const Target& target, const SettingsData& settings);
	void BindCommon(ID3D12GraphicsCommandList* list, bool globals);
	void DrawItems(RenderGraph::PassContext& ctx, const std::vector<DrawItem>& items, PassKind opaqueKind, PassKind alphaKind, int cascade);

	ID3D12Device* device_ = nullptr;
	bool objectsReady_ = false;
	bool failedShaders_ = false;
	uint64_t shaderGeneration_ = 0;
	ComPtr<ID3D12RootSignature> root_;
	ComPtr<ID3DBlob> vsMain_, vsShadow_, psForward_, psDepthAlpha_, psShadowAlpha_, vsFullscreen_, psDecal_, vsParticle_, psParticle_;
	// particles: this frame's quads (sorted, uploaded as a buffer the vertex shader reads) and their batches
	ComPtr<ID3D12PipelineState> particlePipeline_, smokePipeline_; // (smoke: into its own target at half size)
	bool particlePipelineFailed_ = false;
	std::vector<AeParticle> particleData_;
	std::vector<AeParticleBatch> particleBatches_;
	D3D12_GPU_VIRTUAL_ADDRESS particlesVa_ = 0;
	uint32_t particlesDrawn_ = 0;
	ID3D12PipelineState* ParticlePipeline(bool smokeTarget = false);
	ComPtr<ID3DBlob> psFullscreen_[kFullscreenKindCount];
	std::map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines_;
	std::map<uint32_t, ComPtr<ID3D12PipelineState>> fullscreenPipelines_;
	ComPtr<ID3D12Resource> defaultsVb_;
	uint64_t defaultsVbId_ = 0;
	D3D12_VERTEX_BUFFER_VIEW defaultsView_ = {};
	std::shared_ptr<GpuTexture> white_, flatNormal_, black_;
	bool defaultsUploaded_ = false;
	std::shared_ptr<GpuTexture> shadowAtlas_;
	uint32_t shadowCascadeSize_ = 0;
	uint32_t nullCubeSrv_ = UINT32_MAX, nullTextureSrv_ = UINT32_MAX, fallbackTable_ = UINT32_MAX;

	RenderGraph graph_;
	GpuTimer timer_;
	std::vector<std::string> timedPasses_;
	std::vector<uint32_t> passStampBegin_, passStampEnd_;

	// per frame
	std::vector<DrawItem> opaque_, transparent_, casters_, decals_; // decals_: boxes that darken what lies inside them (blob shadows)
	std::vector<uint8_t> instanceData_, materialData_;
	uint32_t instanceCount_ = 0, materialCount_ = 0;
	std::vector<Cascade> cascades_;
	std::vector<uint32_t> clusterGrid_, lightIndices_;
	std::vector<uint8_t> lightData_;
	uint32_t punctualCount_ = 0;
	Vec3 sunDirection_ = {0, 1, 0};
	Vec3 sunColor_ = {0, 0, 0};
	bool hasSun_ = false, sunShadows_ = false;
	uint32_t sunLayerMask_ = 0xFFFFFFFFu;
	D3D12_GPU_VIRTUAL_ADDRESS frameCb_ = 0, instancesVa_ = 0, materialsVa_ = 0, lightsVa_ = 0, gridVa_ = 0, indicesVa_ = 0;
	uint32_t globalsTable_ = UINT32_MAX, postTable_ = UINT32_MAX;
	uint64_t frame_ = 0;
	UINT64 retireFence_ = 0;

	std::mutex statsMutex_;
	json stats_ = json::object();
};

} // namespace aether
