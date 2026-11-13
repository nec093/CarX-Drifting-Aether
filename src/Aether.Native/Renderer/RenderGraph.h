// Aether's render graph: a frame is a list of passes that declare which resources they read and write and in which
// D3D12 state. The graph validates the declarations, works out each resource's lifetime, gives transient resources a
// physical texture from a pool (two transients whose lifetimes do not overlap share one), records the state
// transitions in front of each pass and runs the passes in order. docs/architecture/renderer.md
//
// Deliberately simple: passes run in the order they were added (the order is validated, not searched for), and one
// command list carries the whole frame. Render thread only, except Describe().
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"
#include "../Resources/GpuResources.h"

namespace aether {

class RenderGraph {
public:
	using ResourceId = uint32_t;
	static const ResourceId kInvalid = UINT32_MAX;

	struct PassContext {
		ID3D12GraphicsCommandList* list = nullptr;
		RenderGraph* graph = nullptr;
		GpuTexture* Texture(ResourceId id) const { return graph->Physical(id); }
		uint32_t drawCalls = 0, triangles = 0, dispatches = 0; // filled by the pass, reported with its timing
		std::string note;
	};

	class Builder {
	public:
		void Read(ResourceId id, D3D12_RESOURCE_STATES state) { Add(id, state, false); }
		void Write(ResourceId id, D3D12_RESOURCE_STATES state) { Add(id, state, true); }
	private:
		friend class RenderGraph;
		void Add(ResourceId id, D3D12_RESOURCE_STATES state, bool write);
		RenderGraph* graph = nullptr;
		size_t pass = 0;
	};

	// ---- declaring a frame
	void Begin(uint64_t frame);
	// A texture Aether owns that lives across frames (shadow atlas).
	ResourceId Import(const std::shared_ptr<GpuTexture>& texture);
	// A resource the host owns (the back buffer). The graph never transitions it: every pass must use it in `state`,
	// the state the host hands it over in and expects it back in.
	ResourceId ImportExternal(const char* name, ID3D12Resource* resource, D3D12_RESOURCE_STATES state);
	// A texture that only lives inside this frame.
	ResourceId CreateTransient(const char* name, const GpuTextureDesc& desc, ResourceCategory category);
	void AddPass(const char* name, bool enabled, const std::function<void(Builder&)>& setup, std::function<void(PassContext&)> execute);

	// ---- running it
	// Validates and allocates. False (with the reasons in Errors() and the log) = the frame must not be executed.
	bool Compile();
	// Runs the passes. `onPass(index, begin)` brackets each executed pass (timestamps).
	void Execute(ID3D12GraphicsCommandList* list, const std::function<void(size_t, bool)>& onPass);
	// The frame's transients go back to the pool; pool textures unused for a while are retired against `fence`.
	void End(UINT64 fence);
	// Everything in the pool is retired (resolution change, shutdown).
	void ReleasePool(UINT64 fence, bool leak);

	struct PassResult { std::string name; bool executed = false; uint32_t drawCalls = 0, triangles = 0, dispatches = 0; double cpuMs = 0; std::string note; };
	const std::vector<PassResult>& Results() const { return results_; }
	const std::string& PassName(size_t index) const { return passes_[index].name; }
	const std::vector<std::string>& Errors() const { return errors_; }
	GpuTexture* Physical(ResourceId id) const;
	size_t PassCount() const { return passes_.size(); }

	// ---- any thread: the last compiled frame as data (MCP get_render_graph)
	json Describe();

private:
	struct Use { ResourceId id; D3D12_RESOURCE_STATES state; bool write; };
	struct Pass { std::string name; bool enabled; std::vector<Use> uses; std::function<void(PassContext&)> execute; };
	struct Resource {
		std::string name;
		enum Kind { kImported, kExternal, kTransient } kind = kImported;
		std::shared_ptr<GpuTexture> texture; // imported, or the pool texture given to a transient
		ID3D12Resource* external = nullptr;
		D3D12_RESOURCE_STATES externalState = D3D12_RESOURCE_STATE_COMMON;
		GpuTextureDesc desc;
		ResourceCategory category = ResourceCategory::RenderTarget;
		int firstPass = -1, lastPass = -1;
		int poolIndex = -1;
		bool aliased = false; // shares its physical texture with another transient of this frame
	};
	struct PoolEntry { std::shared_ptr<GpuTexture> texture; uint64_t lastUsedFrame = 0; int busyUntilPass = -1; uint64_t busyFrame = 0; };
	void Error(const std::string& what);

	uint64_t frame_ = 0;
	bool grewThisFrame_ = false; // Compile created a pooled texture
	std::vector<Resource> resources_;
	std::vector<Pass> passes_;
	std::vector<PassResult> results_;
	std::vector<std::string> errors_;
	std::vector<PoolEntry> pool_;
	bool compiled_ = false;
	uint32_t errorLogBudget_ = 10;

	std::mutex describeMutex_;
	json description_ = json::object();
};

} // namespace aether
