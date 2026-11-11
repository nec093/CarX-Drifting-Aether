// Aether's scene renderer: a clustered forward ("forward+") pipeline over the scene store's data.
//   ShadowCascades -> DepthPrepass -> (light clustering on the CPU) -> Opaque -> Sky -> Transparent -> Composite
// Everything is recorded into the one command list the caller hands in; the passes are declared in the render graph.
// docs/architecture/forward-plus.md. Render thread only, except Stats().
#pragma once
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
	json GraphDescription() { return graph_.Describe(); }

private:
	enum PassKind { kDepth = 0, kDepthAlpha, kShadow, kShadowAlpha, kForward, kForwardBlend, kForwardAdditive, kPassKindCount };
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
	ID3D12PipelineState* FullscreenPipeline(bool composite, DXGI_FORMAT format);
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
	ComPtr<ID3DBlob> vsMain_, vsShadow_, psForward_, psDepthAlpha_, psShadowAlpha_, vsFullscreen_, psSky_, psComposite_;
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
	std::vector<DrawItem> opaque_, transparent_, casters_;
	std::vector<uint8_t> instanceData_, materialData_;
	uint32_t instanceCount_ = 0, materialCount_ = 0;
	std::vector<Cascade> cascades_;
	std::vector<uint32_t> clusterGrid_, lightIndices_;
	std::vector<uint8_t> lightData_;
	uint32_t punctualCount_ = 0;
	Vec3 sunDirection_ = {0, 1, 0};
	Vec3 sunColor_ = {0, 0, 0};
	bool hasSun_ = false, sunShadows_ = false;
	D3D12_GPU_VIRTUAL_ADDRESS frameCb_ = 0, instancesVa_ = 0, materialsVa_ = 0, lightsVa_ = 0, gridVa_ = 0, indicesVa_ = 0;
	uint32_t globalsTable_ = UINT32_MAX, postTable_ = UINT32_MAX;
	uint64_t frame_ = 0;
	UINT64 retireFence_ = 0;

	std::mutex statsMutex_;
	json stats_ = json::object();
};

} // namespace aether
