#include "RenderGraph.h"
#include "../Core/Log.h"

namespace aether {

static const uint64_t kPoolKeepFrames = 120; // a pooled texture nobody asked for this long is retired

static const char* StateName(D3D12_RESOURCE_STATES s) {
	switch (s) {
	case D3D12_RESOURCE_STATE_COMMON: return "COMMON";
	case D3D12_RESOURCE_STATE_RENDER_TARGET: return "RENDER_TARGET";
	case D3D12_RESOURCE_STATE_DEPTH_WRITE: return "DEPTH_WRITE";
	case D3D12_RESOURCE_STATE_DEPTH_READ: return "DEPTH_READ";
	case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE: return "PIXEL_SHADER_RESOURCE";
	case D3D12_RESOURCE_STATE_COPY_SOURCE: return "COPY_SOURCE";
	case D3D12_RESOURCE_STATE_COPY_DEST: return "COPY_DEST";
	default: break;
	}
	if (s == (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
		return "DEPTH_READ|PIXEL_SHADER_RESOURCE";
	}
	return "other";
}

static bool IsWriteState(D3D12_RESOURCE_STATES s) {
	return (s & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) != 0;
}

void RenderGraph::Builder::Add(ResourceId id, D3D12_RESOURCE_STATES state, bool write) {
	graph->passes_[pass].uses.push_back({id, state, write});
}

void RenderGraph::Error(const std::string& what) {
	errors_.push_back(what);
	if (errorLogBudget_ > 0) {
		--errorLogBudget_;
		AE_LOG_CODE(Error, "RenderGraph", "E_RENDER_GRAPH", "%s", what.c_str());
	}
}

void RenderGraph::Begin(uint64_t frame) {
	frame_ = frame;
	resources_.clear();
	passes_.clear();
	results_.clear();
	errors_.clear();
	compiled_ = false;
}

RenderGraph::ResourceId RenderGraph::Import(const std::shared_ptr<GpuTexture>& texture) {
	Resource r;
	r.kind = Resource::kImported;
	r.texture = texture;
	r.name = texture ? texture->name : "null";
	if (texture) {
		r.desc = texture->desc;
	}
	resources_.push_back(std::move(r));
	return ResourceId(resources_.size() - 1);
}

RenderGraph::ResourceId RenderGraph::ImportExternal(const char* name, ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
	Resource r;
	r.kind = Resource::kExternal;
	r.name = name;
	r.external = resource;
	r.externalState = state;
	resources_.push_back(std::move(r));
	return ResourceId(resources_.size() - 1);
}

RenderGraph::ResourceId RenderGraph::CreateTransient(const char* name, const GpuTextureDesc& desc, ResourceCategory category) {
	Resource r;
	r.kind = Resource::kTransient;
	r.name = name;
	r.desc = desc;
	r.category = category;
	resources_.push_back(std::move(r));
	return ResourceId(resources_.size() - 1);
}

void RenderGraph::AddPass(const char* name, bool enabled, const std::function<void(Builder&)>& setup, std::function<void(PassContext&)> execute) {
	Pass p;
	p.name = name;
	p.enabled = enabled;
	p.execute = std::move(execute);
	passes_.push_back(std::move(p));
	if (enabled && setup) {
		Builder b;
		b.graph = this;
		b.pass = passes_.size() - 1;
		setup(b);
	}
}

GpuTexture* RenderGraph::Physical(ResourceId id) const {
	return id < resources_.size() ? resources_[id].texture.get() : nullptr;
}

bool RenderGraph::Compile() {
	// ---- validation and lifetimes
	for (size_t p = 0; p < passes_.size(); ++p) {
		const Pass& pass = passes_[p];
		if (!pass.enabled) {
			continue;
		}
		for (size_t u = 0; u < pass.uses.size(); ++u) {
			const Use& use = pass.uses[u];
			if (use.id >= resources_.size()) {
				Error("pass " + pass.name + " uses a resource that was not declared");
				continue;
			}
			Resource& r = resources_[use.id];
			for (size_t v = 0; v < u; ++v) {
				if (pass.uses[v].id == use.id) {
					Error("pass " + pass.name + " declares " + r.name + " twice (one state per resource and pass)");
				}
			}
			if (use.write != IsWriteState(use.state)) {
				Error("pass " + pass.name + (use.write ? " writes " : " reads ") + r.name + " in state " + StateName(use.state) + ", which is not a " + (use.write ? "write" : "read") + " state");
			}
			if (r.kind == Resource::kExternal && use.state != r.externalState) {
				Error("pass " + pass.name + " wants the host's " + r.name + " in " + StateName(use.state) + " but the host hands it over in " + StateName(r.externalState) + " (Aether does not transition host resources)");
			}
			if (r.kind == Resource::kImported && !r.texture) {
				Error("pass " + pass.name + " uses " + r.name + ", which has no texture");
			}
			if (r.firstPass < 0) {
				r.firstPass = int(p);
				if (r.kind == Resource::kTransient && !use.write) {
					Error("pass " + pass.name + " reads the transient " + r.name + " before any pass wrote it (its content is undefined)");
				}
			}
			r.lastPass = int(p);
		}
	}
	if (!errors_.empty()) {
		return false;
	}

	// ---- physical textures for the transients: reuse a pooled texture of the same description that is free during
	// the transient's lifetime (free = not used this frame, or its previous user's last pass is before our first)
	GpuResources& gpu = GpuResources::Get();
	for (Resource& r : resources_) {
		if (r.kind != Resource::kTransient || r.firstPass < 0) {
			continue;
		}
		int found = -1;
		for (size_t i = 0; i < pool_.size(); ++i) {
			PoolEntry& e = pool_[i];
			if (!(e.texture->desc == r.desc)) {
				continue;
			}
			if (e.busyFrame != frame_ || e.busyUntilPass < r.firstPass) {
				found = int(i);
				r.aliased = e.busyFrame == frame_;
				break;
			}
		}
		if (found < 0) {
			auto texture = gpu.CreateTexture(r.desc, r.name.c_str(), r.category, Lifetime::Transient, frame_);
			if (!texture) {
				Error("no memory for the transient " + r.name);
				return false;
			}
			pool_.push_back({texture, frame_, -1, 0});
			found = int(pool_.size() - 1);
		}
		PoolEntry& e = pool_[size_t(found)];
		e.busyFrame = frame_;
		e.busyUntilPass = r.lastPass;
		e.lastUsedFrame = frame_;
		r.poolIndex = found;
		r.texture = e.texture;
	}
	compiled_ = true;

	// ---- the description for diagnostics
	json passes = json::array();
	for (size_t p = 0; p < passes_.size(); ++p) {
		const Pass& pass = passes_[p];
		json reads = json::array(), writes = json::array();
		for (const Use& use : pass.uses) {
			json u = {{"resource", resources_[use.id].name}, {"state", StateName(use.state)}};
			(use.write ? writes : reads).push_back(u);
		}
		passes.push_back({{"index", p}, {"name", pass.name}, {"enabled", pass.enabled}, {"owner", "Aether"}, {"reads", reads}, {"writes", writes}});
	}
	json resources = json::array();
	for (const Resource& r : resources_) {
		json j = {{"name", r.name}, {"kind", r.kind == Resource::kExternal ? "external (host)" : r.kind == Resource::kTransient ? "transient" : "persistent"},
			{"firstPass", r.firstPass}, {"lastPass", r.lastPass}};
		if (r.kind == Resource::kExternal) {
			j["stateOnEntry"] = StateName(r.externalState);
			j["stateOnExit"] = StateName(r.externalState);
		} else if (r.texture) {
			j["width"] = r.texture->desc.width;
			j["height"] = r.texture->desc.height;
			j["dxgiFormat"] = int(r.texture->desc.format);
			j["bytes"] = r.texture->bytes;
			j["physical"] = r.texture->name;
			j["sharesPhysicalTexture"] = r.aliased;
		}
		resources.push_back(j);
	}
	uint64_t poolBytes = 0;
	for (const PoolEntry& e : pool_) {
		poolBytes += e.texture->bytes;
	}
	std::lock_guard lock(describeMutex_);
	description_ = {{"frame", frame_}, {"passes", passes}, {"resources", resources}, {"transientPool", {{"textures", pool_.size()}, {"bytes", poolBytes}}}};
	return true;
}

void RenderGraph::Execute(ID3D12GraphicsCommandList* list, const std::function<void(size_t, bool)>& onPass) {
	results_.assign(passes_.size(), PassResult());
	if (!compiled_) {
		return;
	}
	GpuResources& gpu = GpuResources::Get();
	for (size_t p = 0; p < passes_.size(); ++p) {
		Pass& pass = passes_[p];
		PassResult& result = results_[p];
		result.name = pass.name;
		if (!pass.enabled) {
			result.note = "off";
			continue;
		}
		const double start = QpcSeconds();
		for (const Use& use : pass.uses) {
			Resource& r = resources_[use.id];
			if (r.kind != Resource::kExternal && r.texture) {
				gpu.Transition(list, *r.texture, use.state);
				r.texture->lastUsedFrame = frame_;
			}
		}
		if (onPass) {
			onPass(p, true);
		}
		PassContext ctx;
		ctx.list = list;
		ctx.graph = this;
		pass.execute(ctx);
		if (onPass) {
			onPass(p, false);
		}
		result.executed = true;
		result.drawCalls = ctx.drawCalls;
		result.triangles = ctx.triangles;
		result.dispatches = ctx.dispatches;
		result.note = ctx.note;
		result.cpuMs = (QpcSeconds() - start) * 1000.0;
	}
}

void RenderGraph::End(UINT64 fence) {
	GpuResources& gpu = GpuResources::Get();
	for (size_t i = 0; i < pool_.size();) {
		if (pool_[i].lastUsedFrame + kPoolKeepFrames < frame_) {
			gpu.RetireTexture(std::move(pool_[i].texture), fence, frame_);
			pool_.erase(pool_.begin() + ptrdiff_t(i));
		} else {
			++i;
		}
	}
	// the frame's logical resources drop their references; the pool keeps the textures
	for (Resource& r : resources_) {
		if (r.kind == Resource::kTransient) {
			r.texture.reset();
		}
		r.external = nullptr;
	}
	compiled_ = false;
}

void RenderGraph::ReleasePool(UINT64 fence, bool leak) {
	GpuResources& gpu = GpuResources::Get();
	for (PoolEntry& e : pool_) {
		if (leak) {
			e.texture->resource.Detach();
		} else {
			gpu.RetireTexture(std::move(e.texture), fence, frame_);
		}
	}
	pool_.clear();
	resources_.clear();
	passes_.clear();
	compiled_ = false;
}

json RenderGraph::Describe() {
	std::lock_guard lock(describeMutex_);
	return description_;
}

} // namespace aether
