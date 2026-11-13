#include "Bvh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "../Core/Log.h"
#include "../Resources/GpuResources.h"

namespace aether {

namespace {

float HalfToFloat(uint16_t h) {
	const uint32_t sign = (h >> 15) & 1u, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
	float value;
	if (exponent == 0) {
		value = std::ldexp(float(mantissa), -24);
	} else if (exponent == 31) {
		value = mantissa ? 0.0f : 65504.0f; // (NaN and infinity have no place in a mesh)
	} else {
		value = std::ldexp(float(mantissa | 0x400u), int(exponent) - 25);
	}
	return sign ? -value : value;
}

// uv: the three vertices' texture coordinates (u, v each), or null.
void StoreTri(BvhTri& t, Vec3 v0, Vec3 v1, Vec3 v2, uint32_t subMesh, const float* uv) {
	const Vec3 e1 = v1 - v0, e2 = v2 - v0;
	t.v0[0] = v0.x; t.v0[1] = v0.y; t.v0[2] = v0.z;
	t.e1[0] = e1.x; t.e1[1] = e1.y; t.e1[2] = e1.z;
	t.e2[0] = e2.x; t.e2[1] = e2.y; t.e2[2] = e2.z;
	t.subMesh = subMesh;
	t.uvU = uv ? uv[0] : 0.0f;
	t.uvV = uv ? uv[1] : 0.0f;
	t.duv1[0] = uv ? uv[2] - uv[0] : 0.0f;
	t.duv1[1] = uv ? uv[3] - uv[1] : 0.0f;
	t.duv2[0] = uv ? uv[4] - uv[0] : 0.0f;
	t.duv2[1] = uv ? uv[5] - uv[1] : 0.0f;
}

// The texture coordinates out of a vertex buffer as it lies on the GPU (two floats or two halves at the start of the
// element). Empty: a format this does not read.
std::vector<float> ReadUvs(const void* vertices, uint32_t vertexBytes, uint32_t stride, uint32_t offset, uint32_t format) {
	std::vector<float> out;
	const bool floats = format == DXGI_FORMAT_R32G32_FLOAT || format == DXGI_FORMAT_R32G32B32_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
	const bool halves = format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (!vertices || stride == 0 || (!floats && !halves) || offset + (floats ? 8u : 4u) > stride) {
		return out;
	}
	const uint32_t vertexCount = vertexBytes / stride;
	out.resize(size_t(vertexCount) * 2);
	const uint8_t* v = static_cast<const uint8_t*>(vertices) + offset;
	for (uint32_t i = 0; i < vertexCount; ++i, v += stride) {
		if (floats) {
			memcpy(&out[size_t(i) * 2], v, 8);
		} else {
			uint16_t h[2];
			memcpy(h, v, sizeof(h));
			out[size_t(i) * 2] = HalfToFloat(h[0]);
			out[size_t(i) * 2 + 1] = HalfToFloat(h[1]);
		}
		if (!std::isfinite(out[size_t(i) * 2]) || !std::isfinite(out[size_t(i) * 2 + 1])) {
			out[size_t(i) * 2] = out[size_t(i) * 2 + 1] = 0.0f;
		}
	}
	return out;
}

double NowMs() {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The positions out of a vertex buffer as it lies on the GPU. Empty: a format this does not read.
std::vector<Vec3> ReadPositions(const void* vertices, uint32_t vertexBytes, uint32_t stride, uint32_t positionOffset, uint32_t positionFormat) {
	std::vector<Vec3> out;
	const bool floats = positionFormat == DXGI_FORMAT_R32G32B32_FLOAT || positionFormat == DXGI_FORMAT_R32G32B32A32_FLOAT;
	const bool halves = positionFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (!vertices || stride == 0 || (!floats && !halves) || positionOffset + (floats ? 12u : 6u) > stride) {
		return out;
	}
	const uint32_t vertexCount = vertexBytes / stride;
	out.resize(vertexCount);
	const uint8_t* v = static_cast<const uint8_t*>(vertices) + positionOffset;
	for (uint32_t i = 0; i < vertexCount; ++i, v += stride) {
		if (floats) {
			float p[3];
			memcpy(p, v, sizeof(p));
			out[i] = {p[0], p[1], p[2]};
		} else {
			uint16_t h[3];
			memcpy(h, v, sizeof(h));
			out[i] = {HalfToFloat(h[0]), HalfToFloat(h[1]), HalfToFloat(h[2])};
		}
	}
	return out;
}

} // namespace

SceneBvh& SceneBvh::Get() {
	static SceneBvh* s = new SceneBvh();
	return *s;
}

// ------------------------------------------------------------------------------------------------ main thread

void SceneBvh::Enqueue(Job&& job) {
	{
		std::lock_guard lock(mutex_);
		if (!worker_.joinable()) {
			stop_ = false;
			worker_ = std::thread([this] { WorkerMain(); });
		}
		jobs_.push_back(std::move(job));
		++pendingJobs_;
	}
	wake_.notify_one();
}

bool SceneBvh::SubmitMesh(AeHandle mesh, const void* vertices, uint32_t vertexBytes, uint32_t stride, uint32_t positionOffset, uint32_t positionFormat,
	const void* indices, uint32_t indexBytes, uint32_t indexStride, const AeSubMesh* subMeshes, uint32_t subMeshCount,
	const void* uvs, uint32_t uvBytes, uint32_t uvStride, uint32_t uvOffset, uint32_t uvFormat) {
	Job job;
	job.kind = JobKind::Build;
	job.generation = generation_.load();
	job.mesh = mesh;
	job.changing = changing_.count(mesh) != 0;
	job.positions = ReadPositions(vertices, vertexBytes, stride, positionOffset, positionFormat);
	if (job.positions.empty() || !indices || !subMeshes || (indexStride != 2 && indexStride != 4)) {
		++refused_;
		return false;
	}
	const uint32_t vertexCount = uint32_t(job.positions.size()), indexCount = indexBytes / indexStride;
	if (uvs) {
		job.uvs = ReadUvs(uvs, uvBytes, uvStride, uvOffset, uvFormat);
		if (job.uvs.size() != size_t(vertexCount) * 2) {
			job.uvs.clear(); // (not one per vertex: the hits of this mesh are coloured without its texture)
		}
	}
	const uint8_t* ib = static_cast<const uint8_t*>(indices);
	for (uint32_t s = 0; s < subMeshCount; ++s) {
		const AeSubMesh& sm = subMeshes[s];
		if (sm.indexCount < 3 || uint64_t(sm.indexStart) + sm.indexCount > indexCount) {
			continue;
		}
		for (uint32_t i = 0; i + 2 < sm.indexCount; i += 3) {
			uint32_t tri[3];
			bool valid = true;
			for (int k = 0; k < 3; ++k) {
				const uint32_t at = sm.indexStart + i + uint32_t(k);
				uint32_t index;
				if (indexStride == 2) {
					uint16_t i16;
					memcpy(&i16, ib + size_t(at) * 2, 2);
					index = i16;
				} else {
					memcpy(&index, ib + size_t(at) * 4, 4);
				}
				index += sm.baseVertex;
				valid = valid && index < vertexCount;
				tri[k] = index;
			}
			if (valid) {
				job.indices.insert(job.indices.end(), tri, tri + 3);
				job.triSubMesh.push_back(s);
			}
		}
	}
	if (job.indices.empty()) {
		++refused_;
		return false;
	}
	if (job.changing) {
		changingBuilt_.insert(mesh);
	}
	Enqueue(std::move(job));
	return true;
}

bool SceneBvh::SubmitVertices(AeHandle mesh, const void* data, uint32_t bytes, uint32_t stride, uint32_t positionOffset, uint32_t positionFormat, uint32_t uvMode, uint32_t uvOffset, uint32_t uvFormat) {
	if (!data || bytes == 0 || stride == 0) {
		return false;
	}
	if (changingBuilt_.count(mesh)) {
		// new vertices for a mesh that is there: an update. (Not piled up: a worker that is behind skips to the newest.)
		Job job;
		job.kind = JobKind::Refit;
		job.generation = generation_.load();
		job.mesh = mesh;
		job.positions = ReadPositions(data, bytes, stride, positionOffset, positionFormat);
		if (job.positions.empty()) {
			return false;
		}
		{
			std::lock_guard lock(mutex_);
			for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
				if (it->kind == JobKind::Refit && it->mesh == mesh) {
					jobs_.erase(it);
					--pendingJobs_;
					break;
				}
			}
		}
		Enqueue(std::move(job));
		return true;
	}
	Half& h = halves_[mesh];
	h.vertices.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
	h.stride = stride;
	h.positionOffset = positionOffset;
	h.positionFormat = positionFormat;
	h.uvMode = uvMode;
	if (uvMode == 1) {
		h.uvStride = stride;
		h.uvOffset = uvOffset;
		h.uvFormat = uvFormat;
	}
	return Complete(mesh);
}

bool SceneBvh::SubmitUvs(AeHandle mesh, const void* data, uint32_t bytes, uint32_t stride, uint32_t uvOffset, uint32_t uvFormat) {
	if (!data || bytes == 0 || stride == 0) {
		return false;
	}
	Half& h = halves_[mesh];
	h.uvs.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
	h.uvStride = stride;
	h.uvOffset = uvOffset;
	h.uvFormat = uvFormat;
	return Complete(mesh);
}

bool SceneBvh::SubmitIndices(AeHandle mesh, const void* data, uint32_t bytes, uint32_t indexStride, const AeSubMesh* subMeshes, uint32_t subMeshCount) {
	if (!data || bytes == 0 || !subMeshes || subMeshCount == 0) {
		return false;
	}
	Half& h = halves_[mesh];
	h.indices.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
	h.indexStride = indexStride;
	h.subMeshes.assign(subMeshes, subMeshes + subMeshCount);
	return Complete(mesh);
}

bool SceneBvh::Complete(AeHandle mesh) {
	auto it = halves_.find(mesh);
	if (it == halves_.end() || it->second.vertices.empty() || it->second.indices.empty() || (it->second.uvMode == 2 && it->second.uvs.empty())) {
		return true; // (another part is still to come)
	}
	const Half h = std::move(it->second);
	halves_.erase(it);
	const std::vector<uint8_t>& uvSource = h.uvMode == 2 ? h.uvs : h.vertices;
	return SubmitMesh(mesh, h.vertices.data(), uint32_t(h.vertices.size()), h.stride, h.positionOffset, h.positionFormat, h.indices.data(), uint32_t(h.indices.size()), h.indexStride,
		h.subMeshes.data(), uint32_t(h.subMeshes.size()), h.uvMode ? uvSource.data() : nullptr, uint32_t(uvSource.size()), h.uvStride, h.uvOffset, h.uvFormat);
}

void SceneBvh::MarkChanging(AeHandle mesh) {
	changing_.insert(mesh);
}

void SceneBvh::Alias(AeHandle alias, AeHandle mesh) {
	std::lock_guard lock(mutex_);
	pendingAliases_.push_back({alias, mesh});
}

void SceneBvh::Forget(AeHandle mesh) {
	halves_.erase(mesh);
	const bool wasChanging = changing_.erase(mesh) != 0;
	changingBuilt_.erase(mesh);
	{
		std::lock_guard lock(mutex_);
		pendingForget_.push_back(mesh);
	}
	if (wasChanging) {
		Job job;
		job.kind = JobKind::Forget;
		job.generation = generation_.load();
		job.mesh = mesh;
		Enqueue(std::move(job));
	}
}

void SceneBvh::Clear() {
	halves_.clear();
	changing_.clear();
	changingBuilt_.clear();
	std::lock_guard lock(mutex_);
	++generation_;
	pendingJobs_ -= uint32_t(jobs_.size());
	jobs_.clear();
	built_.clear();
	pendingAliases_.clear();
	pendingForget_.clear();
}

// ------------------------------------------------------------------------------------------------ worker

void SceneBvh::WorkerMain() {
	for (;;) {
		Job job;
		{
			std::unique_lock lock(mutex_);
			wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
			if (stop_) {
				return;
			}
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}
		Built built;
		built.generation = job.generation;
		built.mesh = job.mesh;
		built.refit = job.kind == JobKind::Refit;
		built.changing = job.changing;
		const double start = NowMs();
		if (job.kind == JobKind::Forget) {
			topologies_.erase(job.mesh);
		} else if (job.kind == JobKind::Refit) {
			auto it = topologies_.find(job.mesh);
			if (it != topologies_.end() && it->second.generation == job.generation && it->second.vertexCount == job.positions.size()) {
				RereadMesh(job, it->second, built);
				++refits_;
			}
		} else {
			Topology* keep = nullptr;
			if (job.changing) {
				keep = &topologies_[job.mesh];
				*keep = Topology();
				keep->generation = job.generation;
			}
			ReadMesh(job, built, keep);
			if (keep && built.tris.empty()) {
				topologies_.erase(job.mesh);
			}
		}
		built.ms = NowMs() - start;
		std::lock_guard lock(mutex_);
		--pendingJobs_;
		if (built.generation == generation_.load() && !built.tris.empty()) {
			built_.push_back(std::move(built));
		}
	}
}

// The mesh's triangles in the order of its index buffer. `keep`: which vertices each triangle uses is kept too (a
// mesh whose vertices will change: its triangles are read again from new vertices).
void SceneBvh::ReadMesh(const Job& job, Built& out, Topology* keep) {
	const bool hasUvs = !job.uvs.empty();
	out.tris.reserve(job.triSubMesh.size());
	for (size_t t = 0; t < job.triSubMesh.size(); ++t) {
		uint32_t index[3];
		float uv[6];
		for (int k = 0; k < 3; ++k) {
			index[k] = job.indices[t * 3 + size_t(k)];
			uv[k * 2] = hasUvs ? job.uvs[size_t(index[k]) * 2] : 0.0f;
			uv[k * 2 + 1] = hasUvs ? job.uvs[size_t(index[k]) * 2 + 1] : 0.0f;
		}
		const Vec3 v0 = job.positions[index[0]], v1 = job.positions[index[1]], v2 = job.positions[index[2]];
		const Vec3 n = Cross(v1 - v0, v2 - v0);
		// no area (or not a number): nothing a ray could hit. (Kept in a mesh that changes: it may open up later.)
		if (!std::isfinite(Dot(n, n)) || (!(Dot(n, n) > 1e-18f) && !keep)) {
			continue;
		}
		out.tris.emplace_back();
		StoreTri(out.tris.back(), v0, v1, v2, job.triSubMesh[t], uv);
		if (keep) {
			keep->triVertices.insert(keep->triVertices.end(), index, index + 3);
			keep->triSubMesh.push_back(job.triSubMesh[t]);
			keep->triUvs.insert(keep->triUvs.end(), uv, uv + 6);
		}
	}
	if (keep) {
		keep->vertexCount = uint32_t(job.positions.size());
	}
}

// The same triangles from new vertices.
void SceneBvh::RereadMesh(const Job& job, const Topology& topology, Built& out) {
	const size_t triCount = topology.triSubMesh.size();
	out.tris.resize(triCount);
	for (size_t i = 0; i < triCount; ++i) {
		const Vec3 v0 = job.positions[topology.triVertices[i * 3]], v1 = job.positions[topology.triVertices[i * 3 + 1]], v2 = job.positions[topology.triVertices[i * 3 + 2]];
		StoreTri(out.tris[i], v0, v1, v2, topology.triSubMesh[i], &topology.triUvs[i * 6]);
	}
}

// ------------------------------------------------------------------------------------------------ render thread

namespace {

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav) {
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = type;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = std::max<uint64_t>(bytes, 256);
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
	ComPtr<ID3D12Resource> buffer;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&buffer)))) {
		return nullptr;
	}
	return buffer;
}

void RetireResource(ComPtr<ID3D12Resource>& resource, UINT64 fence) {
	if (resource) {
		ComPtr<IUnknown> u;
		resource.As(&u);
		GpuResources::Get().Retire(u, fence);
		resource.Reset();
	}
}

const D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

} // namespace

// Meshes that went away leave holes in the pool: when the holes outweigh what is left, it is written anew without
// them. (The structures do not depend on where a mesh's triangles lie: only the instances' IDs change.)
void SceneBvh::Compact() {
	std::vector<BvhTri> tris;
	tris.reserve(tris_.size() - size_t(deadTris_));
	for (auto& [handle, e] : meshes_) {
		const uint32_t triBase = uint32_t(tris.size());
		tris.insert(tris.end(), tris_.begin() + e.triBase, tris_.begin() + e.triBase + e.triCount);
		e.triBase = triBase;
	}
	tris_.swap(tris);
	deadTris_ = 0;
	dirtyRanges_.clear();
	// (nothing of the old layout may be traced any more: instances wait for the upload)
	uploadedTris_ = 0;
	dirty_ = true;
	++compactions_;
}

void SceneBvh::BeginFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT64 fence, uint64_t frame) {
	std::vector<Built> built;
	std::vector<std::pair<AeHandle, AeHandle>> aliases;
	std::vector<AeHandle> forget;
	uint64_t generation;
	{
		std::lock_guard lock(mutex_);
		built.swap(built_);
		aliases.swap(pendingAliases_);
		forget.swap(pendingForget_);
		generation = generation_.load();
		if (!jobs_.empty() && !worker_.joinable() && !stop_) {
			worker_ = std::thread([this] { WorkerMain(); });
		}
	}
	if (generation != poolGeneration_) {
		// the scene was unloaded: every mesh goes (the buffers once the GPU is done with them)
		poolGeneration_ = generation;
		tris_.clear();
		meshes_.clear();
		aliases_.clear();
		dirtyRanges_.clear();
		RetireResource(trisBuffer_, fence);
		RetireAllStructures(fence);
		trisVa_ = 0;
		uploadedTris_ = 0;
		deadTris_ = 0;
		dirty_ = false;
		buildMsTotal_ = 0;
	}
	for (const auto& [alias, mesh] : aliases) {
		aliases_[alias] = mesh;
	}
	for (AeHandle h : forget) {
		aliases_.erase(h);
		RetireBlas(h, fence);
		auto it = meshes_.find(h);
		if (it != meshes_.end()) {
			deadTris_ += it->second.triCount;
			meshes_.erase(it);
			for (auto a = aliases_.begin(); a != aliases_.end();) {
				a = a->second == h ? aliases_.erase(a) : std::next(a);
			}
		}
	}
	for (Built& b : built) {
		if (b.generation != generation) {
			continue;
		}
		auto existing = meshes_.find(b.mesh);
		if (b.refit) {
			// the same triangles from new vertices: written over the old ones where they lie
			if (existing == meshes_.end() || existing->second.triCount != b.tris.size()) {
				continue;
			}
			const MeshEntry& e = existing->second;
			std::copy(b.tris.begin(), b.tris.end(), tris_.begin() + e.triBase);
			dirtyRanges_.push_back({e.triBase, e.triCount});
			stale_.insert(b.mesh);
			refitMsLast_ = b.ms;
			continue;
		}
		if (existing != meshes_.end()) {
			continue;
		}
		MeshEntry e;
		e.triBase = uint32_t(tris_.size());
		e.triCount = uint32_t(b.tris.size());
		e.changing = b.changing;
		tris_.insert(tris_.end(), b.tris.begin(), b.tris.end());
		meshes_[b.mesh] = e;
		buildMsTotal_ += b.ms;
		dirty_ = true;
	}
	if (deadTris_ > 20000 && deadTris_ > tris_.size() / 2) {
		Compact();
	}
	// The pool goes to the GPU as a whole when it grew: not after every mesh (they arrive in bursts while a scene
	// loads), but soon enough that what has arrived can be traced.
	const bool quiet = pendingJobs_.load() == 0;
	bool uploaded = false;
	if (dirty_ && device && list && !tris_.empty() && (frame - lastUploadFrame_ > (quiet ? 20u : 180u) || !trisBuffer_)) {
		const uint64_t bytes = uint64_t(tris_.size()) * sizeof(BvhTri);
		ComPtr<ID3D12Resource> buffer = MakeBuffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, false);
		ComPtr<ID3D12Resource> staging = MakeBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
		void* mapped = nullptr;
		const D3D12_RANGE none = {0, 0};
		if (buffer && staging && SUCCEEDED(staging->Map(0, &none, &mapped))) {
			memcpy(mapped, tris_.data(), size_t(bytes));
			staging->Unmap(0, nullptr);
			list->CopyBufferRegion(buffer.Get(), 0, staging.Get(), 0, bytes);
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = buffer.Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			barrier.Transition.StateAfter = kShaderRead;
			list->ResourceBarrier(1, &barrier);
			buffer->SetName(L"Aether.RayTriangles");
			RetireResource(staging, fence);
			RetireResource(trisBuffer_, fence);
			trisBuffer_ = buffer;
			trisVa_ = trisBuffer_->GetGPUVirtualAddress();
			uploadedTris_ = tris_.size();
			dirty_ = false;
			dirtyRanges_.clear();
			lastUploadFrame_ = frame;
			++uploads_;
			uploaded = true;
		} else {
			AE_LOG_CODE(Error, "Bvh", "E_BVH_UPLOAD", "The ray tracing triangles could not be uploaded: reflections fall back to probes");
			dirty_ = false;
		}
	}
	// Updated meshes: only their part of the pool is written again, through a staging buffer of their own. (Not
	// through the frame's upload memory: that is 8 MB for everything the frame draws with, and a few skinned people
	// a frame filled it: the frame's own constants then had nowhere to go and the picture flickered.)
	if (!uploaded && !dirtyRanges_.empty() && trisBuffer_ && list && device) {
		// (a mesh updated twice since the last frame is written once; what is not on the GPU yet waits for the next whole upload)
		std::vector<Range> ranges;
		uint64_t bytes = 0;
		for (size_t i = dirtyRanges_.size(); i-- > 0;) {
			const Range& r = dirtyRanges_[i];
			bool seen = false;
			for (const Range& k : ranges) {
				seen = seen || k.tri == r.tri;
			}
			if (seen || uint64_t(r.tri) + r.triCount > uploadedTris_) {
				continue;
			}
			ranges.push_back(r);
			bytes += uint64_t(r.triCount) * sizeof(BvhTri);
		}
		ComPtr<ID3D12Resource> staging = bytes > 0 ? MakeBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false) : nullptr;
		void* mapped = nullptr;
		const D3D12_RANGE none = {0, 0};
		if (staging && SUCCEEDED(staging->Map(0, &none, &mapped))) {
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = trisBuffer_.Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = kShaderRead;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			list->ResourceBarrier(1, &barrier);
			uint64_t at = 0;
			for (const Range& r : ranges) {
				const uint64_t triBytes = uint64_t(r.triCount) * sizeof(BvhTri);
				memcpy(static_cast<uint8_t*>(mapped) + at, &tris_[r.tri], size_t(triBytes));
				list->CopyBufferRegion(trisBuffer_.Get(), uint64_t(r.tri) * sizeof(BvhTri), staging.Get(), at, triBytes);
				at += triBytes;
				++partialUploads_;
			}
			staging->Unmap(0, nullptr);
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			list->ResourceBarrier(1, &barrier);
			RetireResource(staging, fence);
		}
		dirtyRanges_.clear();
	}
	tlasVa_ = 0;
}

void SceneBvh::BeginInstances() {
	instances_.clear();
	instanceWorld_.clear();
	instanceMesh_.clear();
	frameInstanceTris_ = 0;
}

void SceneBvh::AddInstance(AeHandle mesh, const Mat4& world, uint32_t skipSubMeshes, uint32_t materialOffset, uint32_t doubleSided, bool mirrored) {
	auto alias = aliases_.find(mesh);
	auto it = meshes_.find(alias != aliases_.end() ? alias->second : mesh);
	// (only meshes whose triangles the GPU already has)
	if (it == meshes_.end() || uint64_t(it->second.triBase) + it->second.triCount > uploadedTris_) {
		return;
	}
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 4; ++c) {
			if (!std::isfinite(world.at(r, c))) {
				return;
			}
		}
	}
	const Mat4 inv = Inverse(world);
	TlasInstance instance = {};
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 4; ++c) {
			instance.worldToObject[r * 4 + c] = inv.at(r, c);
		}
	}
	instance.triBase = it->second.triBase;
	instance.skipSubMeshes = skipSubMeshes;
	instance.materialOffset = (materialOffset & 0x7FFFFFFFu) | (mirrored ? 0x80000000u : 0u);
	instance.doubleSided = doubleSided;
	instances_.push_back(instance);
	instanceWorld_.push_back(world);
	instanceMesh_.push_back(it->first);
	frameInstanceTris_ += it->second.triCount;
}

bool SceneBvh::Supported(ID3D12Device* device) {
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
	ComPtr<ID3D12Device5> device5;
	return device && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))) && o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 &&
		SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5)));
}

bool SceneBvh::EndInstances(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT64 fence) {
	const double start = NowMs();
	tlasVa_ = instancesVa_ = 0;
	bool ok = false;
	if (!instances_.empty() && trisVa_ && device && list) {
		const UploadAllocation a = GpuResources::Get().Upload(instances_.size() * sizeof(TlasInstance), 16);
		if (a.cpu) {
			memcpy(a.cpu, instances_.data(), instances_.size() * sizeof(TlasInstance));
			instancesVa_ = a.gpu;
			ok = BuildStructures(device, list, fence);
		}
	}
	uint32_t changing = 0;
	for (const auto& [handle, e] : meshes_) {
		changing += e.changing ? 1u : 0u;
	}
	std::lock_guard lock(statsMutex_);
	stats_ = {
		{"supported", support_ == 1}, {"meshes", meshes_.size()}, {"meshesChanging", changing}, {"meshesPending", pendingJobs_.load()}, {"meshesRefused", refused_.load()},
		{"triangles", tris_.size()}, {"trianglesOfGoneMeshes", deadTris_}, {"triangleBytes", uploadedTris_ * sizeof(BvhTri)},
		{"uploads", uploads_}, {"compactions", compactions_}, {"readMsTotal", buildMsTotal_},
		{"updates", refits_.load()}, {"updateUploads", partialUploads_}, {"lastUpdateMs", refitMsLast_},
		{"bottomLevel", blas_.size()}, {"bottomLevelBytes", blasBytes_}, {"bottomLevelBuilds", blasBuilds_}, {"bottomLevelUpdates", blasUpdates_},
		{"topLevelBytes", tlas_ ? tlas_->GetDesc().Width : 0},
		{"frame", {{"instances", instances_.size()}, {"instanceTriangles", frameInstanceTris_}, {"traced", ok}, {"cpuMs", NowMs() - start}}},
	};
	return ok;
}

void SceneBvh::RetireBlas(AeHandle mesh, UINT64 fence) {
	auto it = blas_.find(mesh);
	if (it != blas_.end()) {
		blasBytes_ -= it->second.buffer ? it->second.buffer->GetDesc().Width : 0;
		RetireResource(it->second.buffer, fence);
		RetireResource(it->second.scratch, fence);
		blas_.erase(it);
	}
	stale_.erase(mesh);
}

void SceneBvh::RetireAllStructures(UINT64 fence) {
	for (auto& [mesh, b] : blas_) {
		RetireResource(b.buffer, fence);
		RetireResource(b.scratch, fence);
	}
	blas_.clear();
	stale_.clear();
	blasBytes_ = 0;
	RetireResource(tlas_, fence);
	RetireResource(tlasScratch_, fence);
	tlasVa_ = 0;
}

bool SceneBvh::BuildStructures(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT64 fence) {
	if (support_ < 0) {
		support_ = Supported(device) ? 1 : 0;
		if (support_ == 0) {
			AE_LOG_CODE(Warning, "Bvh", "W_DXR_UNAVAILABLE", "The device has no DXR 1.1 (inline ray tracing): no ray traced reflections, GI probes or glass reflections");
		}
	}
	ComPtr<ID3D12Device5> device5;
	ComPtr<ID3D12GraphicsCommandList4> list4;
	if (support_ != 1 || FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || FAILED(list->QueryInterface(IID_PPV_ARGS(&list4)))) {
		return false;
	}
	// ---- the bottom level: the meshes this frame uses that have no structure yet, or whose triangles changed (an
	// update in place for a mesh built to be updated). New ones at most about two million triangles a frame: a scene's
	// arrival is spread over a few frames, not one long one.
	uint64_t budget = 2000000;
	uint32_t built = 0;
	for (AeHandle mesh : instanceMesh_) {
		const auto e = meshes_.find(mesh);
		if (e == meshes_.end() || e->second.triCount == 0) {
			continue;
		}
		Blas& b = blas_[mesh];
		const bool stale = stale_.count(mesh) != 0;
		const bool update = stale && b.buffer && b.updatable && b.triCount == e->second.triCount;
		if ((b.buffer && b.triCount == e->second.triCount && !stale) || (!update && budget < e->second.triCount && built > 0)) {
			continue;
		}
		if (!update) {
			budget -= std::min<uint64_t>(budget, e->second.triCount);
		}
		const uint32_t count = e->second.triCount;
		// the vertices, three per triangle, as the pool has them (the corner and its two edges)
		ComPtr<ID3D12Resource> vertices = MakeBuffer(device, uint64_t(count) * 36, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
		float* v = nullptr;
		const D3D12_RANGE none = {0, 0};
		if (!vertices || FAILED(vertices->Map(0, &none, reinterpret_cast<void**>(&v)))) {
			break;
		}
		for (uint32_t i = 0; i < count; ++i) {
			const BvhTri& t = tris_[e->second.triBase + i];
			for (int k = 0; k < 3; ++k) {
				v[i * 9 + k] = t.v0[k];
				v[i * 9 + 3 + k] = t.v0[k] + t.e1[k];
				v[i * 9 + 6 + k] = t.v0[k] + t.e2[k];
			}
		}
		vertices->Unmap(0, nullptr);
		D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
		geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE; // (opaque or not is the instance's: FORCE_OPAQUE / FORCE_NON_OPAQUE)
		geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		geometry.Triangles.VertexCount = count * 3;
		geometry.Triangles.VertexBuffer = {vertices->GetGPUVirtualAddress(), 12};
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		// (a changing mesh: built fast and to be updated; the rest: built once, for the fastest rays)
		inputs.Flags = e->second.changing ? (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD)
										  : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		if (update) {
			inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
		}
		inputs.NumDescs = 1;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.pGeometryDescs = &geometry;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
		desc.Inputs = inputs;
		if (update) {
			desc.SourceAccelerationStructureData = desc.DestAccelerationStructureData = b.buffer->GetGPUVirtualAddress();
			desc.ScratchAccelerationStructureData = b.scratch->GetGPUVirtualAddress();
			list4->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
			++blasUpdates_;
		} else {
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
			device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
			ComPtr<ID3D12Resource> result = MakeBuffer(device, info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
			// (a structure to be updated keeps its scratch: an update needs one every time)
			ComPtr<ID3D12Resource> scratch = MakeBuffer(device, std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
			if (!result || !scratch) {
				RetireResource(vertices, fence);
				AE_LOG_CODE(Error, "Bvh", "E_DXR_MEMORY", "A bottom level acceleration structure could not be created");
				break;
			}
			desc.DestAccelerationStructureData = result->GetGPUVirtualAddress();
			desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
			list4->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
			result->SetName(L"Aether.Dxr.Blas");
			if (b.buffer) {
				blasBytes_ -= b.buffer->GetDesc().Width;
				RetireResource(b.buffer, fence);
			}
			RetireResource(b.scratch, fence);
			b.buffer = result;
			b.updatable = e->second.changing;
			if (b.updatable) {
				b.scratch = scratch;
			} else {
				RetireResource(scratch, fence);
			}
			b.triCount = count;
			blasBytes_ += result->GetDesc().Width;
			++blasBuilds_;
		}
		stale_.erase(mesh);
		RetireResource(vertices, fence);
		++built;
	}
	if (built > 0) {
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; // (every structure written above: the top level build reads them)
		list->ResourceBarrier(1, &barrier);
	}
	// ---- the top level: the frame's instances in the order g_TlasInstances has them. An instance whose mesh has no
	// structure yet keeps its place (mask 0: never hit).
	const uint32_t count = uint32_t(instances_.size());
	D3D12_GPU_VIRTUAL_ADDRESS anyBlas = 0;
	for (const auto& [mesh, b] : blas_) {
		if (b.buffer) {
			anyBlas = b.buffer->GetGPUVirtualAddress();
			break;
		}
	}
	if (!anyBlas) {
		return false;
	}
	const UploadAllocation descs = GpuResources::Get().Upload(size_t(count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
	if (!descs.cpu) {
		return false;
	}
	auto* out = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(descs.cpu);
	for (uint32_t i = 0; i < count; ++i) {
		D3D12_RAYTRACING_INSTANCE_DESC d = {};
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 4; ++c) {
				d.Transform[r][c] = instanceWorld_[i].at(r, c);
			}
		}
		const auto b = blas_.find(instanceMesh_[i]);
		const TlasInstance& t = instances_[i];
		const bool has = b != blas_.end() && b->second.buffer && t.triBase < (1u << 24);
		d.InstanceID = has ? t.triBase : 0;
		d.InstanceMask = has ? 0xFF : 0;
		d.AccelerationStructure = has ? b->second.buffer->GetGPUVirtualAddress() : anyBlas;
		// Opaque unless some of its sub meshes are not there for rays or are double sided: those are decided per
		// triangle in the shader (and their back faces are not culled by the ray's flag).
		d.Flags = t.skipSubMeshes == 0 && t.doubleSided == 0 ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE : (D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE | D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE);
		out[i] = d;
	}
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.NumDescs = count;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.InstanceDescs = descs.gpu;
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
	// (one structure rebuilt in place every frame: the queue runs the frames one after another; it grows with room to spare)
	if (!tlas_ || tlas_->GetDesc().Width < info.ResultDataMaxSizeInBytes) {
		RetireResource(tlas_, fence);
		tlas_ = MakeBuffer(device, info.ResultDataMaxSizeInBytes * 3 / 2, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
		if (tlas_) {
			tlas_->SetName(L"Aether.Dxr.Tlas");
		}
	}
	if (!tlasScratch_ || tlasScratch_->GetDesc().Width < info.ScratchDataSizeInBytes) {
		RetireResource(tlasScratch_, fence);
		tlasScratch_ = MakeBuffer(device, info.ScratchDataSizeInBytes * 3 / 2, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
	}
	if (!tlas_ || !tlasScratch_) {
		AE_LOG_CODE(Error, "Bvh", "E_DXR_MEMORY", "The top level acceleration structure could not be created");
		return false;
	}
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
	desc.Inputs = inputs;
	desc.DestAccelerationStructureData = tlas_->GetGPUVirtualAddress();
	desc.ScratchAccelerationStructureData = tlasScratch_->GetGPUVirtualAddress();
	list4->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = tlas_.Get();
	list->ResourceBarrier(1, &barrier);
	tlasVa_ = tlas_->GetGPUVirtualAddress();
	return true;
}

void SceneBvh::Release(bool leak) {
	// The device's objects go; what was read stays (the pool is uploaded again when there is a device again).
	{
		std::lock_guard lock(mutex_);
		stop_ = true;
	}
	wake_.notify_all();
	if (worker_.joinable()) {
		worker_.join();
	}
	{
		std::lock_guard lock(mutex_);
		stop_ = false;
	}
	if (leak) {
		trisBuffer_.Detach();
		tlas_.Detach();
		tlasScratch_.Detach();
		for (auto& [mesh, b] : blas_) {
			b.buffer.Detach();
			b.scratch.Detach();
		}
	}
	trisBuffer_.Reset();
	tlas_.Reset();
	tlasScratch_.Reset();
	blas_.clear();
	stale_.clear();
	blasBytes_ = 0;
	trisVa_ = instancesVa_ = tlasVa_ = 0;
	uploadedTris_ = 0;
	dirtyRanges_.clear();
	dirty_ = !tris_.empty();
	support_ = -1;
}

json SceneBvh::Stats() {
	std::lock_guard lock(statsMutex_);
	return stats_.is_null() ? json{{"meshes", 0}} : stats_;
}

} // namespace aether

