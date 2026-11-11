// The native side of the scene data contract (include/SceneContract.h).
//
// Threads and ownership (docs/architecture/scene-data-contract.md):
//  - The main thread calls the Upsert / Destroy / SubmitFrame functions. They only copy the description into a
//    pending list (taking a COM reference on any Unity resource named in it) under a short lock. Nothing on the
//    device is touched there.
//  - The render thread calls Apply() at the start of its scene frame: it drains the pending list into the tables
//    below, creates the views, and from then on reads the tables without a lock.
//  - A Unity resource is never released by Aether beyond the reference Aether itself took, and that reference is
//    only dropped through the GPU resource manager's retirement queue (after the frame fence has passed).
//  - A destroyed handle disappears from the tables at the next Apply(); instances that still name it are skipped.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"
#include "../Core/Math.h"
#include "../include/SceneContract.h"

#include <unordered_map>
#include <variant>

namespace aether {

const char* MaterialClassName(uint32_t materialClass);

struct SceneMesh {
	AeMeshDesc desc = {};
	ComPtr<ID3D12Resource> vertexBuffer[AE_MAX_STREAMS];
	ComPtr<ID3D12Resource> indexBuffer;
	std::vector<AeSubMesh> subMeshes;
	uint32_t layoutId = 0; // index into SceneStore::Layouts()
	uint64_t bytes = 0;
};

struct SceneTexture {
	AeTextureDesc desc = {};
	ComPtr<ID3D12Resource> resource;
	uint32_t srv = UINT32_MAX;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	uint64_t bytes = 0;
	uint64_t lastUsedFrame = 0;
	uint32_t updates = 0; // how often the handle got a new resource (texture streaming changes the mip set)
};

struct SceneMaterial {
	AeMaterialDesc desc = {};
	uint32_t table = UINT32_MAX; // first of kAeSlotCount descriptors in the SRV heap
	bool tableDirty = true;
	uint32_t frameIndex = 0;     // index in this frame's material buffer
	uint64_t frameStamp = 0;
};

struct SceneInstance {
	AeInstanceDesc desc = {};
	std::vector<AeHandle> materials;
	Mat4 world = Mat4::Identity();
	bool visible = false;
	uint64_t frameStamp = 0; // frame of the last transform
};

// A vertex input layout shared by many meshes (the pipeline state cache is keyed by its index).
struct SceneLayout {
	uint32_t attributeCount = 0;
	AeVertexAttribute attributes[kAeSemanticCount] = {};
	uint32_t streamCount = 0;
	bool has[kAeSemanticCount] = {};
};

struct SceneFrame {
	AeFrame frame = {};
	std::vector<AeFrameInstance> instances;
	std::vector<AeLight> lights;
	bool valid = false;
};

class SceneStore {
public:
	static SceneStore& Get();

	// ---- main thread (the contract's entry points). Return false when the description is refused.
	bool UpsertMesh(const AeMeshDesc* desc, const AeSubMesh* subMeshes);
	bool UpsertTexture(const AeTextureDesc* desc);
	bool UpsertMaterial(const AeMaterialDesc* desc);
	bool UpsertInstance(const AeInstanceDesc* desc, const AeHandle* materials);
	void Destroy(int kind, AeHandle handle); // kind: 0 mesh, 1 texture, 2 material, 3 instance
	void Clear();                            // scene unload: every handle goes
	bool SubmitFrame(const AeFrame* frame, const AeFrameInstance* instances, uint32_t instanceCount, const AeLight* lights, uint32_t lightCount);

	// ---- render thread
	// Drains the pending list into the tables. `fence` is the value the frame's command lists will carry: replaced
	// and destroyed resources are retired against it. Returns the frame to render (not valid = nothing to draw).
	const SceneFrame& Apply(UINT64 retireFence, uint64_t frame);
	void SetDefaultTextures(uint32_t whiteSrv, uint32_t flatNormalSrv, uint32_t blackSrv);
	// Rewrites the descriptor tables of materials whose textures changed. Call after Apply.
	void UpdateMaterialTables(UINT64 retireFence);
	// Drops every table entry (device objects are going away). leak = do not release the COM references.
	void ReleaseAll(bool leak);

	const std::unordered_map<AeHandle, SceneMesh>& Meshes() const { return meshes_; }
	std::unordered_map<AeHandle, SceneMaterial>& Materials() { return materials_; }
	std::unordered_map<AeHandle, SceneInstance>& Instances() { return instances_; }
	const SceneMesh* Mesh(AeHandle h) const;
	SceneMaterial* Material(AeHandle h);
	SceneTexture* Texture(AeHandle h);
	const std::vector<SceneLayout>& Layouts() const { return layouts_; }

	// True when Apply has something to do or the tables hold handles (the scene path must keep running).
	bool HasWork();

	// ---- any thread
	json Summary();                 // counts, shared memory, unsupported materials, contract errors
	json MaterialReport(size_t max); // per material: class, flags, textures bound, unsupported properties
	void PublishStats(uint64_t frame); // render thread: refreshes what Summary / MaterialReport return

private:
	struct PendingMesh { AeMeshDesc desc; std::vector<AeSubMesh> subMeshes; ComPtr<ID3D12Resource> vb[AE_MAX_STREAMS]; ComPtr<ID3D12Resource> ib; };
	struct PendingTexture { AeTextureDesc desc; ComPtr<ID3D12Resource> resource; };
	struct PendingMaterial { AeMaterialDesc desc; };
	struct PendingInstance { AeInstanceDesc desc; std::vector<AeHandle> materials; };
	struct PendingDestroy { int kind; AeHandle handle; };
	struct PendingClear {};
	using Pending = std::variant<PendingMesh, PendingTexture, PendingMaterial, PendingInstance, PendingDestroy, PendingClear>;

	uint32_t LayoutFor(const AeMeshDesc& desc);
	void RetireMesh(SceneMesh& mesh, UINT64 fence);
	void RetireTexture(SceneTexture& texture, UINT64 fence);
	void MarkMaterialsUsing(AeHandle texture);
	void ContractError(const char* what);

	// main thread -> render thread
	std::mutex pendingMutex_;
	std::vector<Pending> pending_;
	SceneFrame staged_;
	bool stagedNew_ = false;

	// render thread
	std::unordered_map<AeHandle, SceneMesh> meshes_;
	std::unordered_map<AeHandle, SceneTexture> textures_;
	std::unordered_map<AeHandle, SceneMaterial> materials_;
	std::unordered_map<AeHandle, SceneInstance> instances_;
	std::vector<SceneLayout> layouts_;
	SceneFrame current_;
	uint32_t defaultSrv_[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
	uint64_t upserts_ = 0, destroys_ = 0, clears_ = 0, rejectedMeshes_ = 0;

	// diagnostics
	std::mutex statsMutex_;
	json summary_ = json::object();
	json materialReport_ = json::array();
	std::atomic<uint32_t> contractErrors_{0};
	std::string lastContractError_;
};

} // namespace aether
