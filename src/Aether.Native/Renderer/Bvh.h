// The scene's triangles for rays traced with DXR 1.1 (inline ray tracing): reflections that reach what is off
// screen, the GI probes, glass, the ray debug view.
//
// Every mesh's triangles are read on a worker thread from the vertex and index data the managed side reads back from
// the GPU (the game's meshes are not readable on the CPU), into one pooled GPU buffer (BvhTri: the corner, the two
// edges, the texture coordinates, the sub mesh). The hardware's structures are made from the same triangles in the
// same order: one bottom level structure per mesh, and every frame a top level one over the frame's instances. The
// pool is what a hit is shaded from (shaders/aether_bvh.hlsli).
//
// A mesh whose vertices change (skinned: people, flags, a car's suspension) is marked as changing before its
// geometry is sent. Its structure is built to be updated: every time new vertices arrive its triangles are taken
// from them again, only that mesh's part of the pool is rewritten, and its structure is updated in place.
//
// Threads: the Submit / Mark / Alias / Forget / Clear calls on the main thread, reading the triangles on the worker,
// everything else on the render thread.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"
#include "../Core/Math.h"
#include "../include/SceneContract.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace aether {

// (the layouts the shaders read: shaders/aether_bvh.hlsli)
struct BvhTri {
	float v0[3];
	uint32_t subMesh;
	float e1[3]; // v1 - v0
	float uvU;   // the first vertex's texture coordinate
	float e2[3]; // v2 - v0
	float uvV;
	float duv1[2]; // the second vertex's texture coordinate minus the first's
	float duv2[2]; // the third's minus the first's
};
struct TlasInstance {
	float worldToObject[12]; // three rows (the hit's normal)
	uint32_t triBase;        // the mesh's first triangle in the pool (also the instance's ID in the top level structure)
	uint32_t skipSubMeshes;  // bit per sub mesh (the first 32): its triangles are not there for rays (glass, helper geometry)
	uint32_t materialOffset; // bits 0..23: where its sub meshes' materials start in the frame's ray material table, 24..30: how many entries it has there, 31: mirrored (its triangles' front is the other side)
	uint32_t doubleSided;    // bit per sub mesh (the first 32): rays hit its triangles from behind too
};

class SceneBvh {
public:
	static SceneBvh& Get();

	// ---- main thread
	// A mesh's geometry as read back from the GPU. Copied; its triangles are read on the worker. False = refused.
	bool SubmitMesh(AeHandle mesh, const void* vertices, uint32_t vertexBytes, uint32_t stride, uint32_t positionOffset, uint32_t positionFormat,
		const void* indices, uint32_t indexBytes, uint32_t indexStride, const AeSubMesh* subMeshes, uint32_t subMeshCount,
		const void* uvs = nullptr, uint32_t uvBytes = 0, uint32_t uvStride = 0, uint32_t uvOffset = 0, uint32_t uvFormat = 0);
	// The same in two halves (each is one GPU readback on the managed side): the mesh is read when both are there.
	// For a mesh marked as changing, vertices sent again after that update it.
	// uvMode: 0 the mesh has no texture coordinates to send, 1 they are in this stream (uvOffset, uvFormat), 2 they
	// come in a stream of their own (SubmitUvs; the mesh waits for it).
	bool SubmitVertices(AeHandle mesh, const void* data, uint32_t bytes, uint32_t stride, uint32_t positionOffset, uint32_t positionFormat, uint32_t uvMode = 0, uint32_t uvOffset = 0, uint32_t uvFormat = 0);
	bool SubmitUvs(AeHandle mesh, const void* data, uint32_t bytes, uint32_t stride, uint32_t uvOffset, uint32_t uvFormat);
	bool SubmitIndices(AeHandle mesh, const void* data, uint32_t bytes, uint32_t indexStride, const AeSubMesh* subMeshes, uint32_t subMeshCount);
	void MarkChanging(AeHandle mesh);             // before its geometry is sent: its vertices will be sent again and again
	void Alias(AeHandle alias, AeHandle mesh);    // instances of `alias` are traced as `mesh` (a skinned renderer's vertex buffers take turns)
	void Forget(AeHandle mesh);                   // the mesh (or alias) is gone
	void Clear();                                 // scene unload: every mesh goes

	// ---- render thread
	// Takes in what the worker finished and, when the pool changed, uploads it (recorded into `list`).
	void BeginFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT64 fence, uint64_t frame);
	void BeginInstances();
	// False: the mesh is not there for rays (yet): its triangles never came (skinned meshes by default) or are not uploaded.
	bool AddInstance(AeHandle mesh, const Mat4& world, uint32_t skipSubMeshes, uint32_t materialOffset, uint32_t doubleSided, bool mirrored);
	bool HasGeometry() const { return uploadedTris_ > 0; }
	// Uploads this frame's instances and builds the structures: the bottom level ones this frame needs and has not
	// got (or whose triangles changed), the top level one over the instances. False: nothing to trace this frame (no
	// DXR 1.1 on the device, nothing built yet).
	bool EndInstances(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT64 fence);
	static bool Supported(ID3D12Device* device); // DXR 1.1 (inline ray tracing)
	D3D12_GPU_VIRTUAL_ADDRESS TrisVa() const { return trisVa_; }
	D3D12_GPU_VIRTUAL_ADDRESS InstancesVa() const { return instancesVa_; }
	D3D12_GPU_VIRTUAL_ADDRESS TlasVa() const { return tlasVa_; }
	void Release(bool leak);

	// ---- any thread
	json Stats();

private:
	enum class JobKind { Build, Refit, Forget };
	struct Job {
		JobKind kind = JobKind::Build;
		uint64_t generation = 0;
		AeHandle mesh = 0;
		bool changing = false;
		std::vector<Vec3> positions;
		std::vector<float> uvs; // two per vertex (empty: none)
		std::vector<uint32_t> indices;
		std::vector<uint32_t> triSubMesh;
	};
	struct Built { uint64_t generation = 0; AeHandle mesh = 0; bool refit = false; std::vector<BvhTri> tris; double ms = 0; bool changing = false; };
	// what the worker keeps of a changing mesh: which vertices each triangle uses
	struct Topology { uint64_t generation = 0; uint32_t vertexCount = 0; std::vector<uint32_t> triVertices; std::vector<uint32_t> triSubMesh; std::vector<float> triUvs; /* six per triangle */ };
	struct MeshEntry { uint32_t triBase = 0, triCount = 0; bool changing = false; };
	struct Range { uint32_t tri = 0, triCount = 0; };
	void WorkerMain();
	static void ReadMesh(const Job& job, Built& out, Topology* keep);
	static void RereadMesh(const Job& job, const Topology& topology, Built& out);
	void Enqueue(Job&& job);
	bool Complete(AeHandle mesh);
	void Compact();
	bool BuildStructures(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT64 fence);

	// main thread only
	struct Half { std::vector<uint8_t> vertices, indices, uvs; uint32_t stride = 0, positionOffset = 0, positionFormat = 0, indexStride = 0, uvMode = 0, uvStride = 0, uvOffset = 0, uvFormat = 0; std::vector<AeSubMesh> subMeshes; };
	std::unordered_map<AeHandle, Half> halves_;
	std::unordered_set<AeHandle> changing_, changingBuilt_;

	// main thread <-> worker <-> render thread
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<Job> jobs_;
	std::vector<Built> built_;
	std::vector<std::pair<AeHandle, AeHandle>> pendingAliases_;
	std::vector<AeHandle> pendingForget_;
	std::thread worker_;
	bool stop_ = false;
	std::atomic<uint64_t> generation_{1};
	std::atomic<uint32_t> pendingJobs_{0};
	std::atomic<uint32_t> refused_{0};
	std::atomic<uint64_t> refits_{0};

	// worker only
	std::unordered_map<AeHandle, Topology> topologies_;

	// render thread: the pool
	uint64_t poolGeneration_ = 0;
	std::vector<BvhTri> tris_;
	std::unordered_map<AeHandle, MeshEntry> meshes_;
	std::unordered_map<AeHandle, AeHandle> aliases_;
	std::vector<Range> dirtyRanges_;
	uint64_t deadTris_ = 0;
	bool dirty_ = false;
	uint64_t lastUploadFrame_ = 0;
	ComPtr<ID3D12Resource> trisBuffer_;
	D3D12_GPU_VIRTUAL_ADDRESS trisVa_ = 0, instancesVa_ = 0;
	uint64_t uploadedTris_ = 0;
	// render thread: this frame's instances
	std::vector<TlasInstance> instances_;
	std::vector<Mat4> instanceWorld_;
	std::vector<AeHandle> instanceMesh_; // (the mesh it uses, aliases resolved)
	// render thread: the structures
	struct Blas { ComPtr<ID3D12Resource> buffer, scratch; uint32_t triCount = 0; bool updatable = false; };
	void RetireBlas(AeHandle mesh, UINT64 fence);
	void RetireAllStructures(UINT64 fence);
	int support_ = -1; // -1 not asked, 0 no, 1 yes
	std::unordered_map<AeHandle, Blas> blas_;
	std::unordered_set<AeHandle> stale_; // its triangles changed since its structure was built
	ComPtr<ID3D12Resource> tlas_, tlasScratch_;
	D3D12_GPU_VIRTUAL_ADDRESS tlasVa_ = 0;

	// statistics
	std::mutex statsMutex_;
	json stats_;
	double buildMsTotal_ = 0, refitMsLast_ = 0;
	uint32_t uploads_ = 0, compactions_ = 0;
	uint64_t frameInstanceTris_ = 0, partialUploads_ = 0, blasBytes_ = 0, blasBuilds_ = 0, blasUpdates_ = 0;
};

} // namespace aether
