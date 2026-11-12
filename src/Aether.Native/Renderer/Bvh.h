// A bounding volume hierarchy of the scene's triangles for rays traced in shaders (no DXR): reflections that reach
// what is off screen, later indirect light.
//
// Two levels. Every mesh gets its own tree over its triangles, in the mesh's own space, built once on a worker
// thread from the vertex and index data the managed side reads back from the GPU (the game's meshes are not readable
// on the CPU). All of those trees live in two pooled GPU buffers (nodes, triangles). Every frame a small tree over
// the frame's instances is built on the render thread: a ray walks that tree in the world, and at an instance moves
// into the mesh's space and walks the mesh's tree. A thing that moves is just another transform that frame.
//
// A mesh whose vertices change (skinned: people, flags, a car's suspension) is marked as changing before its
// geometry is sent. Its tree keeps its shape; every time new vertices arrive the triangles are taken from them again
// and the boxes are fitted around them again (a refit), and only that mesh's part of the GPU buffers is rewritten.
//
// Threads: the Submit / Mark / Alias / Forget / Clear calls on the main thread, builds and refits on the worker,
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
struct BvhNode {
	float bmin[3];
	uint32_t a; // inner node: its first child (the second is the next node); leaf: its first triangle / instance
	float bmax[3];
	uint32_t b; // 0: inner node; else a leaf with this many triangles / instances
};
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
	float worldToObject[12]; // three rows
	uint32_t root;           // the mesh's root node in the pooled nodes
	uint32_t skipSubMeshes;  // bit per sub mesh (the first 32): its triangles are not there for rays (glass, helper geometry)
	uint32_t materialOffset; // where its sub meshes' materials start in the frame's ray material table; bit 31: mirrored (its triangles' front is the other side)
	uint32_t doubleSided;    // bit per sub mesh (the first 32): rays hit its triangles from behind too
};

class SceneBvh {
public:
	static SceneBvh& Get();

	// ---- main thread
	// A mesh's geometry as read back from the GPU. Copied; the tree is built on the worker. False = refused.
	bool SubmitMesh(AeHandle mesh, const void* vertices, uint32_t vertexBytes, uint32_t stride, uint32_t positionOffset, uint32_t positionFormat,
		const void* indices, uint32_t indexBytes, uint32_t indexStride, const AeSubMesh* subMeshes, uint32_t subMeshCount,
		const void* uvs = nullptr, uint32_t uvBytes = 0, uint32_t uvStride = 0, uint32_t uvOffset = 0, uint32_t uvFormat = 0);
	// The same in two halves (each is one GPU readback on the managed side): the tree is built when both are there.
	// For a mesh marked as changing, vertices sent again after that refit its tree.
	// uvMode: 0 the mesh has no texture coordinates to send, 1 they are in this stream (uvOffset, uvFormat), 2 they
	// come in a stream of their own (SubmitUvs; the tree waits for it).
	bool SubmitVertices(AeHandle mesh, const void* data, uint32_t bytes, uint32_t stride, uint32_t positionOffset, uint32_t positionFormat, uint32_t uvMode = 0, uint32_t uvOffset = 0, uint32_t uvFormat = 0);
	bool SubmitUvs(AeHandle mesh, const void* data, uint32_t bytes, uint32_t stride, uint32_t uvOffset, uint32_t uvFormat);
	bool SubmitIndices(AeHandle mesh, const void* data, uint32_t bytes, uint32_t indexStride, const AeSubMesh* subMeshes, uint32_t subMeshCount);
	void MarkChanging(AeHandle mesh);             // before its geometry is sent: its vertices will be sent again and again
	void Alias(AeHandle alias, AeHandle mesh);    // instances of `alias` are traced as `mesh` (a skinned renderer's vertex buffers take turns)
	void Forget(AeHandle mesh);                   // the mesh (or alias) is gone
	void Clear();                                 // scene unload: every tree goes

	// ---- render thread
	// Takes in what the worker finished and, when the pools changed, uploads them (recorded into `list`).
	void BeginFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT64 fence, uint64_t frame);
	void BeginInstances();
	void AddInstance(AeHandle mesh, const Mat4& world, uint32_t skipSubMeshes, uint32_t materialOffset, uint32_t doubleSided, bool mirrored);
	bool HasTrees() const { return uploadedNodes_ > 0; }
	// Builds the tree over this frame's instances and uploads it. False: nothing to trace.
	bool EndInstances();
	bool Ready() const { return ready_; }
	D3D12_GPU_VIRTUAL_ADDRESS NodesVa() const { return nodesVa_; }
	D3D12_GPU_VIRTUAL_ADDRESS TrisVa() const { return trisVa_; }
	D3D12_GPU_VIRTUAL_ADDRESS TlasNodesVa() const { return tlasNodesVa_; }
	D3D12_GPU_VIRTUAL_ADDRESS TlasInstancesVa() const { return tlasInstancesVa_; }
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
	struct Built { uint64_t generation = 0; AeHandle mesh = 0; bool refit = false; std::vector<BvhNode> nodes; std::vector<BvhTri> tris; double ms = 0; uint32_t depth = 0; bool changing = false; };
	// what the worker keeps of a changing mesh: the tree's shape and which vertices each triangle uses
	struct Topology { uint64_t generation = 0; uint32_t vertexCount = 0; std::vector<BvhNode> nodes; std::vector<uint32_t> triVertices; std::vector<uint32_t> triSubMesh; std::vector<float> triUvs; /* six per triangle */ };
	struct MeshEntry { uint32_t root = 0, nodeCount = 0, triBase = 0, triCount = 0; bool changing = false; };
	struct Range { uint32_t node = 0, nodeCount = 0, tri = 0, triCount = 0; };
	void WorkerMain();
	static void BuildMesh(const Job& job, Built& out, Topology* keep);
	static void RefitMesh(const Job& job, const Topology& topology, Built& out);
	void Enqueue(Job&& job);
	bool Complete(AeHandle mesh);
	void Compact();

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

	// render thread
	uint64_t poolGeneration_ = 0;
	std::vector<BvhNode> nodes_;
	std::vector<BvhTri> tris_;
	std::unordered_map<AeHandle, MeshEntry> meshes_;
	std::unordered_map<AeHandle, AeHandle> aliases_;
	std::vector<Range> dirtyRanges_;
	uint64_t deadTris_ = 0, deadNodes_ = 0;
	bool dirty_ = false;
	uint64_t lastUploadFrame_ = 0;
	ComPtr<ID3D12Resource> nodesBuffer_, trisBuffer_;
	D3D12_GPU_VIRTUAL_ADDRESS nodesVa_ = 0, trisVa_ = 0, tlasNodesVa_ = 0, tlasInstancesVa_ = 0;
	uint64_t uploadedNodes_ = 0, uploadedTris_ = 0;
	bool ready_ = false;
	struct InstanceBox { float bmin[3], bmax[3]; };
	std::vector<TlasInstance> instances_;
	std::vector<InstanceBox> instanceBoxes_;
	std::vector<BvhNode> tlasNodes_;
	std::vector<uint32_t> tlasOrder_;

	// statistics
	std::mutex statsMutex_;
	struct Statistics {
		uint32_t meshes = 0, changing = 0, pending = 0, refused = 0, instances = 0, tlasNodes = 0, maxDepth = 0, uploads = 0, compactions = 0;
		uint64_t nodes = 0, tris = 0, deadTris = 0, gpuBytes = 0, frameInstanceTris = 0, refits = 0, partialUploads = 0;
		double buildMsTotal = 0, refitMsLast = 0, tlasMs = 0;
	} stats_;
	double buildMsTotal_ = 0, refitMsLast_ = 0;
	uint32_t maxDepth_ = 0, uploads_ = 0, compactions_ = 0;
	uint64_t frameInstanceTris_ = 0, partialUploads_ = 0;
};

} // namespace aether
