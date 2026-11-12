#include "SceneStore.h"
#include "../Core/Log.h"
#include "../Resources/GpuResources.h"

#include <algorithm>

namespace aether {

static const char* kClassNames[kAeMaterialClassCount] = {"unsupported", "standard", "vehiclePaint", "vehicleGeneric", "vehicleGlass", "vehicleRim", "vehicleStitches",
	"colorMask", "vegetation", "transparent", "unlit", "vehicleLights", "vehicleTire", "skip", "blobShadow"};

const char* MaterialClassName(uint32_t materialClass) {
	return materialClass < kAeMaterialClassCount ? kClassNames[materialClass] : "invalid";
}

SceneStore& SceneStore::Get() {
	static SceneStore* s = new SceneStore();
	return *s;
}

// A pointer handed over by the managed side is checked before it is trusted: it must answer QueryInterface for
// ID3D12Resource. A bad pointer faults here, inside the guard, instead of in the middle of a frame.
static ID3D12Resource* QueryResource(void* p) {
	if (!p) {
		return nullptr;
	}
	ID3D12Resource* r = nullptr;
	__try {
		if (FAILED(static_cast<IUnknown*>(p)->QueryInterface(__uuidof(ID3D12Resource), reinterpret_cast<void**>(&r)))) {
			r = nullptr;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		r = nullptr;
	}
	return r;
}

void SceneStore::ContractError(const char* what) {
	uint32_t n = contractErrors_.fetch_add(1) + 1;
	{
		std::lock_guard lock(statsMutex_);
		lastContractError_ = what;
	}
	if (n <= 20) { // a broken caller repeats itself every frame: do not flood the log
		AE_LOG_CODE(Error, "Scene", "E_SCENE_CONTRACT", "Scene contract violation: %s", what);
	}
}

// ------------------------------------------------------------------------------------------------ main thread

bool SceneStore::UpsertMesh(const AeMeshDesc* desc, const AeSubMesh* subMeshes) {
	if (!desc || desc->structSize != sizeof(AeMeshDesc)) {
		ContractError("AeMeshDesc has the wrong size (managed and native sides are from different builds)");
		return false;
	}
	if (desc->mesh == 0 || desc->streamCount == 0 || desc->streamCount > AE_MAX_STREAMS || desc->attributeCount == 0 || desc->attributeCount > kAeSemanticCount ||
		(desc->indexStride != 2 && desc->indexStride != 4) || desc->subMeshCount == 0 || desc->subMeshCount > 1024 || !subMeshes) {
		ContractError("AeMeshDesc has an invalid field (handle, streams, attributes, index stride or sub mesh count)");
		return false;
	}
	PendingMesh p;
	p.desc = *desc;
	for (uint32_t i = 0; i < desc->attributeCount; ++i) {
		const AeVertexAttribute& a = desc->attributes[i];
		if (a.semantic >= kAeSemanticCount || a.stream >= desc->streamCount) {
			ContractError("AeMeshDesc has an attribute with an invalid semantic or stream");
			return false;
		}
	}
	for (uint32_t s = 0; s < desc->streamCount; ++s) {
		p.vb[s].Attach(QueryResource(desc->vertexBuffer[s]));
		if (!p.vb[s] || desc->streamStride[s] == 0) {
			ContractError("AeMeshDesc names a vertex buffer that is not an ID3D12Resource");
			return false;
		}
		p.desc.vertexBuffer[s] = nullptr; // the raw pointers are not kept
	}
	p.ib.Attach(QueryResource(desc->indexBuffer));
	if (!p.ib) {
		ContractError("AeMeshDesc names an index buffer that is not an ID3D12Resource");
		return false;
	}
	p.desc.indexBuffer = nullptr;
	p.subMeshes.assign(subMeshes, subMeshes + desc->subMeshCount);
	std::lock_guard lock(pendingMutex_);
	pending_.emplace_back(std::move(p));
	return true;
}

bool SceneStore::UpsertTexture(const AeTextureDesc* desc) {
	if (!desc || desc->structSize != sizeof(AeTextureDesc)) {
		ContractError("AeTextureDesc has the wrong size");
		return false;
	}
	if (desc->texture == 0 || desc->dimension > kAeTexture3D) {
		ContractError("AeTextureDesc has an invalid handle or dimension");
		return false;
	}
	PendingTexture p;
	p.desc = *desc;
	p.resource.Attach(QueryResource(desc->resource));
	if (!p.resource) {
		ContractError("AeTextureDesc names a texture that is not an ID3D12Resource");
		return false;
	}
	p.desc.resource = nullptr;
	std::lock_guard lock(pendingMutex_);
	pending_.emplace_back(std::move(p));
	return true;
}

bool SceneStore::UpsertMaterial(const AeMaterialDesc* desc) {
	if (!desc || desc->structSize != sizeof(AeMaterialDesc)) {
		ContractError("AeMaterialDesc has the wrong size");
		return false;
	}
	if (desc->material == 0 || desc->materialClass >= kAeMaterialClassCount || desc->maskLayout > kAeMaskRmo) {
		ContractError("AeMaterialDesc has an invalid handle, class or mask layout");
		return false;
	}
	std::lock_guard lock(pendingMutex_);
	pending_.emplace_back(PendingMaterial{*desc});
	return true;
}

bool SceneStore::UpsertInstance(const AeInstanceDesc* desc, const AeHandle* materials) {
	if (!desc || desc->structSize != sizeof(AeInstanceDesc)) {
		ContractError("AeInstanceDesc has the wrong size");
		return false;
	}
	if (desc->instance == 0 || desc->mesh == 0 || desc->materialCount > 1024 || (desc->materialCount > 0 && !materials)) {
		ContractError("AeInstanceDesc has an invalid handle or material count");
		return false;
	}
	PendingInstance p;
	p.desc = *desc;
	p.materials.assign(materials, materials + desc->materialCount);
	std::lock_guard lock(pendingMutex_);
	pending_.emplace_back(std::move(p));
	return true;
}

void SceneStore::Destroy(int kind, AeHandle handle) {
	if (kind < 0 || kind > 3 || handle == 0) {
		return;
	}
	std::lock_guard lock(pendingMutex_);
	pending_.emplace_back(PendingDestroy{kind, handle});
}

void SceneStore::Clear() {
	std::lock_guard lock(pendingMutex_);
	pending_.emplace_back(PendingClear{});
	staged_.valid = false;
	staged_.instances.clear();
	staged_.lights.clear();
	staged_.particleBatches.clear();
	staged_.particles.clear();
	stagedBatches_.clear();
	stagedParticles_.clear();
	stagedNew_ = true;
}

bool SceneStore::SetProbes(const AeProbe* probes, uint32_t count, uint32_t structSize) {
	if (structSize != sizeof(AeProbe) || (count > 0 && !probes) || count > 256) {
		ContractError("AeProbe has the wrong size or the probe list is invalid");
		return false;
	}
	std::lock_guard lock(pendingMutex_);
	stagedProbes_.assign(probes, probes + count);
	stagedProbesNew_ = true;
	return true;
}

bool SceneStore::SubmitParticles(const AeParticleBatch* batches, uint32_t batchCount, const AeParticle* particles, uint32_t particleCount, uint32_t particleSize) {
	if (particleSize != sizeof(AeParticle) || (batchCount > 0 && !batches) || (particleCount > 0 && !particles) || batchCount > 4096 || particleCount > (1u << 18)) {
		ContractError("AeParticle has the wrong size or the particle arrays are invalid");
		return false;
	}
	for (uint32_t i = 0; i < batchCount; ++i) {
		if (uint64_t(batches[i].first) + batches[i].count > particleCount) {
			ContractError("An AeParticleBatch reaches past the particle array");
			return false;
		}
	}
	std::lock_guard lock(pendingMutex_);
	stagedBatches_.assign(batches, batches + batchCount);
	stagedParticles_.assign(particles, particles + particleCount);
	return true;
}

bool SceneStore::SubmitFrame(const AeFrame* frame, const AeFrameInstance* instances, uint32_t instanceCount, const AeLight* lights, uint32_t lightCount) {
	if (!frame || frame->structSize != sizeof(AeFrame) || frame->contractVersion != AE_SCENE_CONTRACT_VERSION) {
		ContractError("AeFrame has the wrong size or contract version");
		return false;
	}
	if ((instanceCount > 0 && !instances) || (lightCount > 0 && !lights) || instanceCount > (1u << 20) || lightCount > 4096) {
		ContractError("AeFrame comes with an invalid instance or light array");
		return false;
	}
	std::lock_guard lock(pendingMutex_);
	staged_.frame = *frame;
	// assign keeps the vectors' capacity: no allocation once they reached the scene's size
	staged_.instances.assign(instances, instances + instanceCount);
	staged_.lights.assign(lights, lights + lightCount);
	// (the particles sent since the frame before belong to this frame; none sent = none)
	staged_.particleBatches.swap(stagedBatches_);
	staged_.particles.swap(stagedParticles_);
	stagedBatches_.clear();
	stagedParticles_.clear();
	staged_.valid = (frame->flags & kAeFrameCameraValid) != 0 && frame->width > 0 && frame->height > 0;
	stagedNew_ = true;
	return true;
}

// ------------------------------------------------------------------------------------------------ render thread

static DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
	case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
	case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
	case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
	case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
	case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
	case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
	case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
	case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
	case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
	case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
	// depth / stencil and video formats are not sampled by Aether's materials
	case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_UNKNOWN:
		return DXGI_FORMAT_UNKNOWN;
	default: return f;
	}
}

// False when the resource cannot be viewed the way the description says.
static bool ViewDesc(const AeTextureDesc& desc, const D3D12_RESOURCE_DESC& rd, D3D12_SHADER_RESOURCE_VIEW_DESC& out) {
	out = {};
	out.Format = TypedFormat(rd.Format);
	out.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	if (out.Format == DXGI_FORMAT_UNKNOWN || rd.SampleDesc.Count != 1) {
		return false;
	}
	const UINT mips = UINT(-1);
	switch (desc.dimension) {
	case kAeTexture2D:
		if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
		if (rd.DepthOrArraySize > 1) { // Unity makes some 2D textures as one-slice arrays' bigger cousins: view slice 0
			out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			out.Texture2DArray.MipLevels = mips;
			out.Texture2DArray.ArraySize = 1;
		} else {
			out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			out.Texture2D.MipLevels = mips;
		}
		return true;
	case kAeTexture2DArray:
		if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
		out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		out.Texture2DArray.MipLevels = mips;
		out.Texture2DArray.ArraySize = rd.DepthOrArraySize;
		return true;
	case kAeTextureCube:
	case kAeTextureCubeArray:
		if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.DepthOrArraySize < 6) return false;
		// always viewed as a cube array (a single cube is an array of one): one shader declaration for both
		out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
		out.TextureCubeArray.MipLevels = mips;
		out.TextureCubeArray.NumCubes = rd.DepthOrArraySize / 6;
		return true;
	case kAeTexture3D:
		if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) return false;
		out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		out.Texture3D.MipLevels = mips;
		return true;
	}
	return false;
}

uint32_t SceneStore::LayoutFor(const AeMeshDesc& desc) {
	for (uint32_t i = 0; i < layouts_.size(); ++i) {
		const SceneLayout& l = layouts_[i];
		if (l.attributeCount == desc.attributeCount && l.streamCount == desc.streamCount && memcmp(l.attributes, desc.attributes, sizeof(AeVertexAttribute) * desc.attributeCount) == 0) {
			return i;
		}
	}
	SceneLayout l;
	l.attributeCount = desc.attributeCount;
	l.streamCount = desc.streamCount;
	for (uint32_t i = 0; i < desc.attributeCount; ++i) {
		l.attributes[i] = desc.attributes[i];
		l.has[desc.attributes[i].semantic] = true;
	}
	layouts_.push_back(l);
	return uint32_t(layouts_.size() - 1);
}

void SceneStore::RetireMesh(SceneMesh& mesh, UINT64 fence) {
	GpuResources& gpu = GpuResources::Get();
	for (auto& vb : mesh.vertexBuffer) {
		if (vb) {
			gpu.Retire(std::move(vb), fence);
		}
	}
	if (mesh.indexBuffer) {
		gpu.Retire(std::move(mesh.indexBuffer), fence);
	}
}

void SceneStore::RetireTexture(SceneTexture& texture, UINT64 fence) {
	GpuResources& gpu = GpuResources::Get();
	gpu.FreeSrv(texture.srv, 1, fence);
	texture.srv = UINT32_MAX;
	if (texture.resource) {
		gpu.Retire(std::move(texture.resource), fence);
	}
}

void SceneStore::MarkMaterialsUsing(AeHandle texture) {
	for (auto& [handle, m] : materials_) {
		for (AeHandle t : m.desc.textures) {
			if (t == texture) {
				m.tableDirty = true;
				break;
			}
		}
	}
}

const SceneFrame& SceneStore::Apply(UINT64 fence, uint64_t frame) {
	std::vector<Pending> pending;
	{
		std::lock_guard lock(pendingMutex_);
		pending.swap(pending_);
		if (stagedProbesNew_) {
			current_.probes = stagedProbes_;
			stagedProbesNew_ = false;
		}
		if (stagedNew_) {
			std::swap(current_.frame, staged_.frame);
			current_.instances.swap(staged_.instances);
			current_.lights.swap(staged_.lights);
			current_.particleBatches.swap(staged_.particleBatches);
			current_.particles.swap(staged_.particles);
			current_.valid = staged_.valid;
			stagedNew_ = false;
		} else {
			current_.valid = false; // the same frame is not rendered twice
		}
	}
	GpuResources& gpu = GpuResources::Get();
	ID3D12Device* device = gpu.Device();
	for (Pending& item : pending) {
		if (auto* m = std::get_if<PendingMesh>(&item)) {
			// The views Aether draws with must fit inside Unity's buffers: a mesh whose description claims more than
			// the buffers hold (a dynamic mesh resized since, a pooled buffer) would make the GPU read out of bounds.
			bool fits = true;
			for (uint32_t s = 0; s < m->desc.streamCount && fits; ++s) {
				const D3D12_RESOURCE_DESC bd = m->vb[s]->GetDesc();
				fits = bd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && bd.Width >= UINT64(m->desc.vertexCount) * m->desc.streamStride[s];
			}
			if (fits) {
				const D3D12_RESOURCE_DESC bd = m->ib->GetDesc();
				fits = bd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && bd.Width >= UINT64(m->desc.indexCount) * m->desc.indexStride;
			}
			for (const AeSubMesh& sm : m->subMeshes) {
				fits = fits && UINT64(sm.indexStart) + sm.indexCount <= m->desc.indexCount;
			}
			if (!fits) {
				ContractError(Format("mesh %u describes more vertices or indices than its buffers hold: not drawn", m->desc.mesh).c_str());
				for (auto& vb : m->vb) {
					if (vb) {
						gpu.Retire(std::move(vb), fence);
					}
				}
				gpu.Retire(std::move(m->ib), fence);
				auto old = meshes_.find(m->desc.mesh);
				if (old != meshes_.end()) {
					RetireMesh(old->second, fence);
					meshes_.erase(old);
				}
				++rejectedMeshes_;
				continue;
			}
			SceneMesh& mesh = meshes_[m->desc.mesh];
			RetireMesh(mesh, fence);
			mesh.desc = m->desc;
			mesh.subMeshes = std::move(m->subMeshes);
			mesh.bytes = 0;
			for (uint32_t s = 0; s < m->desc.streamCount; ++s) {
				mesh.bytes += m->vb[s]->GetDesc().Width;
				mesh.vertexBuffer[s] = std::move(m->vb[s]);
			}
			mesh.bytes += m->ib->GetDesc().Width;
			mesh.indexBuffer = std::move(m->ib);
			mesh.layoutId = LayoutFor(mesh.desc);
			++upserts_;
		} else if (auto* t = std::get_if<PendingTexture>(&item)) {
			D3D12_RESOURCE_DESC rd = t->resource->GetDesc();
			D3D12_SHADER_RESOURCE_VIEW_DESC view;
			if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || !ViewDesc(t->desc, rd, view)) {
				ContractError(Format("texture %u cannot be sampled (DXGI format %d, dimension %u): left unbound", t->desc.texture, int(rd.Format), t->desc.dimension).c_str());
				gpu.Retire(std::move(t->resource), fence);
				continue;
			}
			SceneTexture& tex = textures_[t->desc.texture];
			const bool existed = tex.resource != nullptr;
			RetireTexture(tex, fence);
			tex.desc = t->desc;
			tex.format = view.Format;
			tex.srv = gpu.AllocSrv(1);
			gpu.WriteSrv(tex.srv, t->resource.Get(), &view);
			D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &rd);
			tex.bytes = info.SizeInBytes != UINT64_MAX ? info.SizeInBytes : 0;
			tex.resource = std::move(t->resource);
			tex.lastUsedFrame = frame;
			tex.updates += existed ? 1 : 0;
			MarkMaterialsUsing(t->desc.texture);
			++upserts_;
		} else if (auto* mat = std::get_if<PendingMaterial>(&item)) {
			SceneMaterial& sm = materials_[mat->desc.material];
			sm.desc = mat->desc;
			sm.tableDirty = true;
			++upserts_;
		} else if (auto* inst = std::get_if<PendingInstance>(&item)) {
			SceneInstance& i = instances_[inst->desc.instance];
			i.desc = inst->desc;
			i.materials = std::move(inst->materials);
			++upserts_;
		} else if (auto* d = std::get_if<PendingDestroy>(&item)) {
			++destroys_;
			switch (d->kind) {
			case 0: {
				auto it = meshes_.find(d->handle);
				if (it != meshes_.end()) {
					RetireMesh(it->second, fence);
					meshes_.erase(it);
				}
				break;
			}
			case 1: {
				auto it = textures_.find(d->handle);
				if (it != textures_.end()) {
					RetireTexture(it->second, fence);
					textures_.erase(it);
					MarkMaterialsUsing(d->handle);
				}
				break;
			}
			case 2: {
				auto it = materials_.find(d->handle);
				if (it != materials_.end()) {
					gpu.FreeSrv(it->second.table, kAeSlotCount, fence);
					materials_.erase(it);
				}
				break;
			}
			case 3: instances_.erase(d->handle); break;
			}
		} else if (std::holds_alternative<PendingClear>(item)) {
			++clears_;
			for (auto& [h, mesh] : meshes_) RetireMesh(mesh, fence);
			for (auto& [h, tex] : textures_) RetireTexture(tex, fence);
			for (auto& [h, material] : materials_) gpu.FreeSrv(material.table, kAeSlotCount, fence);
			meshes_.clear();
			textures_.clear();
			materials_.clear();
			instances_.clear();
		}
	}
	if (current_.valid) {
		for (const AeFrameInstance& fi : current_.instances) {
			auto it = instances_.find(fi.instance);
			if (it == instances_.end()) {
				continue; // created and destroyed out of step with the frame: skipped, not an error
			}
			// Moving = its transform changed within the last 45 frames, not only since the frame before: a car's
			// transform is set by the physics step, which does not run once per rendered frame, and a frame can be
			// rendered twice from the same data. A car that counted as still on such frames was smeared by the
			// motion blur (and left a trail in the temporal antialiasing) on exactly those frames.
			if (it->second.frameStamp == 0) {
				memcpy(it->second.prevWorld.m, fi.localToWorld, sizeof(fi.localToWorld)); // first sight: it did not move
			} else {
				it->second.prevWorld = it->second.world;
			}
			if (memcmp(it->second.world.m, fi.localToWorld, sizeof(fi.localToWorld)) != 0) {
				it->second.movedFrame = frame;
			}
			it->second.moved = it->second.movedFrame != 0 && frame - it->second.movedFrame < 45;
			memcpy(it->second.world.m, fi.localToWorld, sizeof(fi.localToWorld));
			it->second.visible = (fi.flags & kAeFrameInstanceVisible) != 0;
			it->second.shadowOnly = (fi.flags & kAeFrameInstanceShadowOnly) != 0;
			it->second.frameStamp = frame;
		}
	}
	return current_;
}

bool SceneStore::HasWork() {
	if (!instances_.empty() || !meshes_.empty() || !textures_.empty() || !materials_.empty()) {
		return true;
	}
	std::lock_guard lock(pendingMutex_);
	return !pending_.empty() || stagedNew_;
}

void SceneStore::SetDefaultTextures(uint32_t whiteSrv, uint32_t flatNormalSrv, uint32_t blackSrv) {
	defaultSrv_[0] = whiteSrv;
	defaultSrv_[1] = flatNormalSrv;
	defaultSrv_[2] = blackSrv;
	for (auto& [h, material] : materials_) {
		material.tableDirty = true;
	}
}

void SceneStore::UpdateMaterialTables(UINT64 fence) {
	GpuResources& gpu = GpuResources::Get();
	if (defaultSrv_[0] == UINT32_MAX) {
		return;
	}
	for (auto& [handle, m] : materials_) {
		if (!m.tableDirty) {
			continue;
		}
		// A table a submitted list may still read is never rewritten: a changed material gets a new one.
		uint32_t table = gpu.AllocSrv(kAeSlotCount);
		if (table == UINT32_MAX) {
			continue; // heap full: keeps the old table and tries again next frame
		}
		for (uint32_t slot = 0; slot < kAeSlotCount; ++slot) {
			uint32_t src = slot == kAeSlotNormal ? defaultSrv_[1] : defaultSrv_[0];
			SceneTexture* t = m.desc.textures[slot] ? Texture(m.desc.textures[slot]) : nullptr;
			if (t && t->srv != UINT32_MAX) {
				src = t->srv;
			}
			gpu.CopySrv(table + slot, src);
		}
		if (m.table != UINT32_MAX) {
			gpu.FreeSrv(m.table, kAeSlotCount, fence); // lists submitted so far may still read the old one
		}
		m.table = table;
		m.tableDirty = false;
	}
}

void SceneStore::ReleaseAll(bool leak) {
	if (leak) {
		for (auto& [h, mesh] : meshes_) {
			for (auto& vb : mesh.vertexBuffer) vb.Detach();
			mesh.indexBuffer.Detach();
		}
		for (auto& [h, tex] : textures_) tex.resource.Detach();
	}
	meshes_.clear();
	textures_.clear();
	materials_.clear();
	instances_.clear();
	layouts_.clear();
	current_ = SceneFrame();
	defaultSrv_[0] = defaultSrv_[1] = defaultSrv_[2] = UINT32_MAX;
	std::lock_guard lock(pendingMutex_);
	if (leak) {
		for (Pending& item : pending_) {
			if (auto* m = std::get_if<PendingMesh>(&item)) {
				for (auto& vb : m->vb) vb.Detach();
				m->ib.Detach();
			} else if (auto* t = std::get_if<PendingTexture>(&item)) {
				t->resource.Detach();
			}
		}
	}
	pending_.clear();
	staged_ = SceneFrame();
	stagedNew_ = false;
}

const SceneMesh* SceneStore::Mesh(AeHandle h) const {
	auto it = meshes_.find(h);
	return it != meshes_.end() ? &it->second : nullptr;
}

SceneMaterial* SceneStore::Material(AeHandle h) {
	auto it = materials_.find(h);
	return it != materials_.end() ? &it->second : nullptr;
}

SceneTexture* SceneStore::Texture(AeHandle h) {
	auto it = textures_.find(h);
	return it != textures_.end() ? &it->second : nullptr;
}

// ------------------------------------------------------------------------------------------------ diagnostics

void SceneStore::PublishStats(uint64_t frame) {
	uint64_t meshBytes = 0, textureBytes = 0;
	uint32_t textureUpdates = 0, staleTextures = 0;
	for (const auto& [h, m] : meshes_) meshBytes += m.bytes;
	for (const auto& [h, t] : textures_) {
		textureBytes += t.bytes;
		textureUpdates += t.updates;
		staleTextures += (frame > t.lastUsedFrame + 600) ? 1 : 0;
	}
	uint32_t classCounts[kAeMaterialClassCount] = {};
	uint32_t unsupportedProps = 0, blended = 0, alphaTest = 0;
	json report = json::array();
	for (const auto& [h, m] : materials_) {
		classCounts[m.desc.materialClass]++;
		unsupportedProps += m.desc.unsupportedProperties;
		blended += (m.desc.flags & kAeMaterialBlend) ? 1 : 0;
		alphaTest += (m.desc.flags & kAeMaterialAlphaTest) ? 1 : 0;
		if (report.size() < 600) {
			json textures = json::array();
			static const char* slots[kAeSlotCount] = {"base", "normal", "mask", "emissive", "extra"};
			for (uint32_t s = 0; s < kAeSlotCount; ++s) {
				if (m.desc.textures[s]) {
					auto it = textures_.find(m.desc.textures[s]);
					textures.push_back({{"slot", slots[s]}, {"texture", m.desc.textures[s]}, {"bound", it != textures_.end() && it->second.srv != UINT32_MAX},
						{"width", it != textures_.end() ? it->second.desc.width : 0}, {"height", it != textures_.end() ? it->second.desc.height : 0}, {"dxgiFormat", it != textures_.end() ? int(it->second.format) : 0}});
				}
			}
			report.push_back({{"material", h}, {"class", MaterialClassName(m.desc.materialClass)}, {"shaderId", m.desc.shaderId}, {"renderQueue", m.desc.renderQueue},
				{"alphaTest", (m.desc.flags & kAeMaterialAlphaTest) != 0}, {"blend", (m.desc.flags & kAeMaterialBlend) != 0}, {"doubleSided", (m.desc.flags & kAeMaterialDoubleSided) != 0},
				{"clearCoat", m.desc.clearCoat}, {"metallic", m.desc.metallic}, {"roughness", m.desc.roughness},
				{"baseColor", {m.desc.baseColor[0], m.desc.baseColor[1], m.desc.baseColor[2], m.desc.baseColor[3]}},
				{"maskLayout", m.desc.maskLayout == kAeMaskHdrp ? "hdrp" : m.desc.maskLayout == kAeMaskRmo ? "rmo" : "none"},
				{"unsupportedProperties", m.desc.unsupportedProperties}, {"textures", textures}});
		}
	}
	json classes = json::object();
	for (uint32_t c = 0; c < kAeMaterialClassCount; ++c) {
		if (classCounts[c]) {
			classes[MaterialClassName(c)] = classCounts[c];
		}
	}
	uint32_t live = 0, visible = 0;
	for (const auto& [h, i] : instances_) {
		if (i.frameStamp == frame) {
			++live;
			visible += i.visible ? 1 : 0;
		}
	}
	const AeFrame& f = current_.frame;
	json summary = {
		{"contractVersion", AE_SCENE_CONTRACT_VERSION},
		{"frameValid", current_.valid},
		{"sceneId", f.sceneId},
		{"camera", {{"id", f.cameraId}, {"width", f.width}, {"height", f.height}, {"near", f.nearPlane}, {"far", f.farPlane},
			{"position", {f.cameraPosition[0], f.cameraPosition[1], f.cameraPosition[2]}}, {"forward", {f.cameraForward[0], f.cameraForward[1], f.cameraForward[2]}}}},
		{"instances", {{"registered", instances_.size()}, {"inThisFrame", live}, {"enabled", visible}}},
		{"meshes", {{"count", meshes_.size()}, {"vertexLayouts", layouts_.size()}, {"sharedBytes", meshBytes}, {"rejected", rejectedMeshes_}}},
		{"textures", {{"count", textures_.size()}, {"sharedBytes", textureBytes}, {"resourceChanges", textureUpdates}, {"notUsedFor600Frames", staleTextures}}},
		{"materials", {{"count", materials_.size()}, {"byClass", classes}, {"unsupported", classCounts[kAeMaterialUnsupported]}, {"unmappedProperties", unsupportedProps}, {"alphaBlended", blended}, {"alphaTested", alphaTest}}},
		{"lights", current_.lights.size()},
		{"sky", f.skyTexture != 0},
		{"exposureFromTexture", f.exposureTexture != 0},
		{"sharedBytes", meshBytes + textureBytes},
		{"sharedNote", "Unity's own mesh buffers and textures that Aether references (zero copy). They are Unity's memory, not Aether's."},
		{"commands", {{"upserts", upserts_}, {"destroys", destroys_}, {"sceneClears", clears_}}},
		{"contractErrors", contractErrors_.load()},
	};
	std::lock_guard lock(statsMutex_);
	if (!lastContractError_.empty()) {
		summary["lastContractError"] = lastContractError_;
	}
	summary_ = std::move(summary);
	materialReport_ = std::move(report);
}

json SceneStore::Summary() {
	std::lock_guard lock(statsMutex_);
	json j = summary_;
	j["contractErrors"] = contractErrors_.load();
	return j;
}

json SceneStore::MaterialReport(size_t max) {
	std::lock_guard lock(statsMutex_);
	if (materialReport_.size() <= max) {
		return materialReport_;
	}
	json out = json::array();
	for (size_t i = 0; i < max; ++i) {
		out.push_back(materialReport_[i]);
	}
	return out;
}

} // namespace aether
