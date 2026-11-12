#include "Bvh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "../Core/Log.h"
#include "../Resources/GpuResources.h"

namespace aether {

namespace {

const uint32_t kLeafTriangles = 4;
const uint32_t kBins = 12;
const uint32_t kMaxDepth = 30; // (the shader's stack holds 32 entries)

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

struct Box {
	Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
	void Add(Vec3 p) {
		lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
		hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
	}
	void Add(const Box& b) {
		Add(b.lo);
		Add(b.hi);
	}
	float Area() const {
		const Vec3 d = hi - lo;
		return d.x < 0 ? 0.0f : 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
	}
};

float Axis(Vec3 v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

void StoreBox(BvhNode& n, const Box& b) {
	n.bmin[0] = b.lo.x; n.bmin[1] = b.lo.y; n.bmin[2] = b.lo.z;
	n.bmax[0] = b.hi.x; n.bmax[1] = b.hi.y; n.bmax[2] = b.hi.z;
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
		// new vertices for a tree that exists: a refit. (Not piled up: a worker that is behind skips to the newest.)
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
				RefitMesh(job, it->second, built);
				++refits_;
			}
		} else {
			Topology* keep = nullptr;
			if (job.changing) {
				keep = &topologies_[job.mesh];
				*keep = Topology();
				keep->generation = job.generation;
			}
			BuildMesh(job, built, keep);
			if (keep && built.nodes.empty()) {
				topologies_.erase(job.mesh);
			}
		}
		built.ms = NowMs() - start;
		std::lock_guard lock(mutex_);
		--pendingJobs_;
		if (built.generation == generation_.load() && !built.nodes.empty()) {
			built_.push_back(std::move(built));
		}
	}
}

// A tree over one mesh's triangles: split along the widest axis of the triangles' centres where the surface area
// heuristic (binned) says the two halves are cheapest; a leaf holds up to four triangles.
// `keep`: the tree's shape and each triangle's vertices are also kept (a mesh whose vertices will change).
void SceneBvh::BuildMesh(const Job& job, Built& out, Topology* keep) {
	struct Tri { Vec3 v0, v1, v2, centre; Box box; uint32_t subMesh; uint32_t index[3]; float uv[6]; };
	const bool hasUvs = !job.uvs.empty();
	std::vector<Tri> tris;
	tris.reserve(job.triSubMesh.size());
	for (size_t t = 0; t < job.triSubMesh.size(); ++t) {
		Tri tri;
		for (int k = 0; k < 3; ++k) {
			tri.index[k] = job.indices[t * 3 + size_t(k)];
		}
		tri.v0 = job.positions[tri.index[0]];
		tri.v1 = job.positions[tri.index[1]];
		tri.v2 = job.positions[tri.index[2]];
		const Vec3 n = Cross(tri.v1 - tri.v0, tri.v2 - tri.v0);
		// no area (or not a number): nothing a ray could hit. (Kept in a mesh that changes: it may open up later.)
		if (!(Dot(n, n) > 1e-18f) && !keep) {
			continue;
		}
		if (!std::isfinite(Dot(n, n))) {
			continue;
		}
		tri.box.Add(tri.v0);
		tri.box.Add(tri.v1);
		tri.box.Add(tri.v2);
		tri.centre = (tri.box.lo + tri.box.hi) * 0.5f;
		tri.subMesh = job.triSubMesh[t];
		for (int k = 0; k < 3; ++k) {
			tri.uv[k * 2] = hasUvs ? job.uvs[size_t(tri.index[k]) * 2] : 0.0f;
			tri.uv[k * 2 + 1] = hasUvs ? job.uvs[size_t(tri.index[k]) * 2 + 1] : 0.0f;
		}
		tris.push_back(tri);
	}
	if (tris.empty()) {
		return;
	}
	std::vector<BvhNode>& nodes = out.nodes;
	nodes.reserve(tris.size());
	nodes.push_back({});
	struct Work { uint32_t node, first, count, depth; };
	std::vector<Work> stack;
	stack.push_back({0, 0, uint32_t(tris.size()), 0});
	uint32_t maxDepth = 0;
	while (!stack.empty()) {
		const Work w = stack.back();
		stack.pop_back();
		maxDepth = std::max(maxDepth, w.depth);
		Box box, centres;
		for (uint32_t i = w.first; i < w.first + w.count; ++i) {
			box.Add(tris[i].box);
			centres.Add(tris[i].centre);
		}
		StoreBox(nodes[w.node], box);
		const Vec3 extent = centres.hi - centres.lo;
		const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
		const float width = Axis(extent, axis);
		if (w.count <= kLeafTriangles || w.depth >= kMaxDepth || !(width > 1e-9f)) {
			if (w.count <= 64 || w.depth >= kMaxDepth) {
				nodes[w.node].a = w.first;
				nodes[w.node].b = w.count;
				continue;
			}
		}
		uint32_t mid = w.first + w.count / 2;
		bool split = false;
		if (width > 1e-9f && w.depth + 8 < kMaxDepth) {
			// binned surface area heuristic along the widest axis
			Box binBox[kBins];
			uint32_t binCount[kBins] = {};
			const float lo = Axis(centres.lo, axis), scale = float(kBins) / width;
			for (uint32_t i = w.first; i < w.first + w.count; ++i) {
				const uint32_t b = std::min(uint32_t((Axis(tris[i].centre, axis) - lo) * scale), kBins - 1);
				binBox[b].Add(tris[i].box);
				++binCount[b];
			}
			float rightArea[kBins] = {};
			Box acc;
			uint32_t rightCount[kBins] = {};
			uint32_t n = 0;
			for (int b = int(kBins) - 1; b > 0; --b) {
				if (binCount[b]) {
					acc.Add(binBox[b]);
				}
				n += binCount[b];
				rightArea[b] = acc.Area();
				rightCount[b] = n;
			}
			acc = Box();
			n = 0;
			float best = 1e30f;
			uint32_t bestBin = 0;
			for (uint32_t b = 0; b + 1 < kBins; ++b) {
				if (binCount[b]) {
					acc.Add(binBox[b]);
				}
				n += binCount[b];
				if (n == 0 || rightCount[b + 1] == 0) {
					continue;
				}
				const float cost = acc.Area() * float(n) + rightArea[b + 1] * float(rightCount[b + 1]);
				if (cost < best) {
					best = cost;
					bestBin = b;
				}
			}
			if (best < 1e30f) {
				const auto middle = std::partition(tris.begin() + w.first, tris.begin() + w.first + w.count, [&](const Tri& t) {
					return std::min(uint32_t((Axis(t.centre, axis) - lo) * scale), kBins - 1) <= bestBin;
				});
				mid = uint32_t(middle - tris.begin());
				split = mid > w.first && mid < w.first + w.count;
			}
		}
		if (!split) {
			// (all centres in one place, or the tree is getting deep: halves by count keep it shallow)
			mid = w.first + w.count / 2;
			std::nth_element(tris.begin() + w.first, tris.begin() + mid, tris.begin() + w.first + w.count, [&](const Tri& a, const Tri& b) { return Axis(a.centre, axis) < Axis(b.centre, axis); });
		}
		const uint32_t left = uint32_t(nodes.size());
		nodes.push_back({});
		nodes.push_back({});
		nodes[w.node].a = left;
		nodes[w.node].b = 0;
		stack.push_back({left, w.first, mid - w.first, w.depth + 1});
		stack.push_back({left + 1, mid, w.first + w.count - mid, w.depth + 1});
	}
	out.depth = maxDepth;
	out.tris.resize(tris.size());
	for (size_t i = 0; i < tris.size(); ++i) {
		StoreTri(out.tris[i], tris[i].v0, tris[i].v1, tris[i].v2, tris[i].subMesh, tris[i].uv);
	}
	if (keep) {
		keep->vertexCount = uint32_t(job.positions.size());
		keep->nodes = nodes;
		keep->triVertices.resize(tris.size() * 3);
		keep->triSubMesh.resize(tris.size());
		keep->triUvs.resize(tris.size() * 6);
		for (size_t i = 0; i < tris.size(); ++i) {
			memcpy(&keep->triUvs[i * 6], tris[i].uv, sizeof(tris[i].uv));
			memcpy(&keep->triVertices[i * 3], tris[i].index, sizeof(tris[i].index));
			keep->triSubMesh[i] = tris[i].subMesh;
		}
	}
}

// The same tree around new vertices: every triangle taken from its three vertices again, every box fitted again
// around what is under it (children come after their parent in the array, so one pass from the end does it).
void SceneBvh::RefitMesh(const Job& job, const Topology& topology, Built& out) {
	const size_t triCount = topology.triSubMesh.size();
	out.tris.resize(triCount);
	std::vector<Box> triBox(triCount);
	for (size_t i = 0; i < triCount; ++i) {
		const Vec3 v0 = job.positions[topology.triVertices[i * 3]], v1 = job.positions[topology.triVertices[i * 3 + 1]], v2 = job.positions[topology.triVertices[i * 3 + 2]];
		StoreTri(out.tris[i], v0, v1, v2, topology.triSubMesh[i], &topology.triUvs[i * 6]);
		triBox[i].Add(v0);
		triBox[i].Add(v1);
		triBox[i].Add(v2);
	}
	out.nodes = topology.nodes;
	std::vector<Box> boxes(out.nodes.size());
	for (size_t k = out.nodes.size(); k-- > 0;) {
		const BvhNode& n = out.nodes[k];
		Box box;
		if (n.b) {
			for (uint32_t i = 0; i < n.b; ++i) {
				box.Add(triBox[n.a + i]);
			}
		} else {
			box.Add(boxes[n.a]);
			box.Add(boxes[n.a + 1]);
		}
		boxes[k] = box;
		StoreBox(out.nodes[k], box);
	}
}

// ------------------------------------------------------------------------------------------------ render thread

// Meshes that went away leave holes in the pools: when the holes outweigh what is left, the pools are written anew
// without them.
void SceneBvh::Compact() {
	std::vector<BvhNode> nodes;
	std::vector<BvhTri> tris;
	nodes.reserve(nodes_.size() - size_t(deadNodes_));
	tris.reserve(tris_.size() - size_t(deadTris_));
	for (auto& [handle, e] : meshes_) {
		const uint32_t nodeBase = uint32_t(nodes.size()), triBase = uint32_t(tris.size());
		for (uint32_t i = 0; i < e.nodeCount; ++i) {
			BvhNode n = nodes_[e.root + i];
			n.a = n.b ? n.a - e.triBase + triBase : n.a - e.root + nodeBase;
			nodes.push_back(n);
		}
		tris.insert(tris.end(), tris_.begin() + e.triBase, tris_.begin() + e.triBase + e.triCount);
		e.root = nodeBase;
		e.triBase = triBase;
	}
	nodes_.swap(nodes);
	tris_.swap(tris);
	deadNodes_ = deadTris_ = 0;
	dirtyRanges_.clear();
	// (nothing of the old layout may be traced any more: instances wait for the upload)
	uploadedNodes_ = uploadedTris_ = 0;
	dirty_ = true;
	++compactions_;
}

void SceneBvh::BeginFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT64 fence, uint64_t frame) {
	GpuResources& gpu = GpuResources::Get();
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
	auto retireBuffers = [&]() {
		if (nodesBuffer_) {
			ComPtr<IUnknown> a, b;
			nodesBuffer_.As(&a);
			trisBuffer_.As(&b);
			gpu.Retire(a, fence);
			gpu.Retire(b, fence);
		}
		nodesBuffer_.Reset();
		trisBuffer_.Reset();
	};
	if (generation != poolGeneration_) {
		// the scene was unloaded: every tree goes (the buffers once the GPU is done with them)
		poolGeneration_ = generation;
		nodes_.clear();
		tris_.clear();
		meshes_.clear();
		aliases_.clear();
		dirtyRanges_.clear();
		retireBuffers();
		nodesVa_ = trisVa_ = 0;
		uploadedNodes_ = uploadedTris_ = 0;
		deadNodes_ = deadTris_ = 0;
		dirty_ = false;
		buildMsTotal_ = 0;
		maxDepth_ = 0;
	}
	for (const auto& [alias, mesh] : aliases) {
		aliases_[alias] = mesh;
	}
	for (AeHandle h : forget) {
		aliases_.erase(h);
		auto it = meshes_.find(h);
		if (it != meshes_.end()) {
			deadNodes_ += it->second.nodeCount;
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
			// the same tree with new numbers: written over the old ones where they lie
			if (existing == meshes_.end() || existing->second.nodeCount != b.nodes.size() || existing->second.triCount != b.tris.size()) {
				continue;
			}
			const MeshEntry& e = existing->second;
			for (size_t i = 0; i < b.nodes.size(); ++i) {
				BvhNode n = b.nodes[i];
				n.a += n.b ? e.triBase : e.root;
				nodes_[e.root + i] = n;
			}
			std::copy(b.tris.begin(), b.tris.end(), tris_.begin() + e.triBase);
			dirtyRanges_.push_back({e.root, e.nodeCount, e.triBase, e.triCount});
			refitMsLast_ = b.ms;
			continue;
		}
		if (existing != meshes_.end()) {
			continue;
		}
		const uint32_t nodeBase = uint32_t(nodes_.size()), triBase = uint32_t(tris_.size());
		for (BvhNode n : b.nodes) {
			n.a += n.b ? triBase : nodeBase;
			nodes_.push_back(n);
		}
		tris_.insert(tris_.end(), b.tris.begin(), b.tris.end());
		MeshEntry e;
		e.root = nodeBase;
		e.nodeCount = uint32_t(b.nodes.size());
		e.triBase = triBase;
		e.triCount = uint32_t(b.tris.size());
		e.changing = b.changing;
		meshes_[b.mesh] = e;
		buildMsTotal_ += b.ms;
		maxDepth_ = std::max(maxDepth_, b.depth);
		dirty_ = true;
	}
	if (deadTris_ > 20000 && deadTris_ > tris_.size() / 2) {
		Compact();
	}
	// The pools go to the GPU as a whole when they grew: not after every mesh (they arrive in bursts while a scene
	// loads), but soon enough that what has arrived can be traced.
	const bool quiet = pendingJobs_.load() == 0;
	bool uploaded = false;
	if (dirty_ && device && list && !nodes_.empty() && (frame - lastUploadFrame_ > (quiet ? 20u : 180u) || !nodesBuffer_)) {
		auto make = [&](const void* data, uint64_t bytes, ComPtr<ID3D12Resource>& out) -> bool {
			D3D12_HEAP_PROPERTIES heap = {};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			rd.Width = bytes;
			rd.Height = 1;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = 1;
			rd.SampleDesc.Count = 1;
			rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ComPtr<ID3D12Resource> buffer, staging;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
				return false;
			}
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)))) {
				return false;
			}
			void* mapped = nullptr;
			const D3D12_RANGE none = {0, 0};
			if (FAILED(staging->Map(0, &none, &mapped))) {
				return false;
			}
			memcpy(mapped, data, size_t(bytes));
			staging->Unmap(0, nullptr);
			list->CopyBufferRegion(buffer.Get(), 0, staging.Get(), 0, bytes);
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = buffer.Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			list->ResourceBarrier(1, &barrier);
			buffer->SetName(L"Aether.Bvh");
			ComPtr<IUnknown> s;
			staging.As(&s);
			gpu.Retire(s, fence);
			out = buffer;
			return true;
		};
		ComPtr<ID3D12Resource> newNodes, newTris;
		if (make(nodes_.data(), uint64_t(nodes_.size()) * sizeof(BvhNode), newNodes) && make(tris_.data(), uint64_t(tris_.size()) * sizeof(BvhTri), newTris)) {
			retireBuffers();
			nodesBuffer_ = newNodes;
			trisBuffer_ = newTris;
			nodesVa_ = nodesBuffer_->GetGPUVirtualAddress();
			trisVa_ = trisBuffer_->GetGPUVirtualAddress();
			uploadedNodes_ = nodes_.size();
			uploadedTris_ = tris_.size();
			dirty_ = false;
			dirtyRanges_.clear();
			lastUploadFrame_ = frame;
			++uploads_;
			uploaded = true;
		} else {
			AE_LOG_CODE(Error, "Bvh", "E_BVH_UPLOAD", "The ray tracing buffers could not be created: reflections fall back to probes");
			dirty_ = false;
		}
	}
	// Refitted meshes: only their part of the buffers is written again, through a staging buffer of their own. (Not
	// through the frame's upload memory: that is 8 MB for everything the frame draws with, and a few skinned people
	// a frame filled it: the frame's own constants then had nowhere to go and the picture flickered.)
	if (!uploaded && !dirtyRanges_.empty() && nodesBuffer_ && list && device) {
		// (a mesh refitted twice since the last frame is written once; what is not on the GPU yet waits for the next whole upload)
		std::vector<Range> ranges;
		uint64_t bytes = 0;
		for (size_t i = dirtyRanges_.size(); i-- > 0;) {
			const Range& r = dirtyRanges_[i];
			bool seen = false;
			for (const Range& k : ranges) {
				seen = seen || k.node == r.node;
			}
			if (seen || uint64_t(r.node) + r.nodeCount > uploadedNodes_ || uint64_t(r.tri) + r.triCount > uploadedTris_) {
				continue;
			}
			ranges.push_back(r);
			bytes += uint64_t(r.nodeCount) * sizeof(BvhNode) + uint64_t(r.triCount) * sizeof(BvhTri);
		}
		ComPtr<ID3D12Resource> staging;
		void* mapped = nullptr;
		if (bytes > 0) {
			D3D12_HEAP_PROPERTIES heap = {};
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			rd.Width = bytes;
			rd.Height = 1;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = 1;
			rd.SampleDesc.Count = 1;
			rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			const D3D12_RANGE none = {0, 0};
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging))) || FAILED(staging->Map(0, &none, &mapped))) {
				staging.Reset();
			}
		}
		if (staging && mapped) {
			const D3D12_RESOURCE_STATES shader = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			D3D12_RESOURCE_BARRIER barriers[2] = {};
			for (int i = 0; i < 2; ++i) {
				barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[i].Transition.pResource = i == 0 ? nodesBuffer_.Get() : trisBuffer_.Get();
				barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barriers[i].Transition.StateBefore = shader;
				barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			list->ResourceBarrier(2, barriers);
			uint64_t at = 0;
			for (const Range& r : ranges) {
				const uint64_t nodeBytes = uint64_t(r.nodeCount) * sizeof(BvhNode), triBytes = uint64_t(r.triCount) * sizeof(BvhTri);
				memcpy(static_cast<uint8_t*>(mapped) + at, &nodes_[r.node], size_t(nodeBytes));
				list->CopyBufferRegion(nodesBuffer_.Get(), uint64_t(r.node) * sizeof(BvhNode), staging.Get(), at, nodeBytes);
				at += nodeBytes;
				memcpy(static_cast<uint8_t*>(mapped) + at, &tris_[r.tri], size_t(triBytes));
				list->CopyBufferRegion(trisBuffer_.Get(), uint64_t(r.tri) * sizeof(BvhTri), staging.Get(), at, triBytes);
				at += triBytes;
				++partialUploads_;
			}
			staging->Unmap(0, nullptr);
			for (int i = 0; i < 2; ++i) {
				std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
			}
			list->ResourceBarrier(2, barriers);
			ComPtr<IUnknown> s;
			staging.As(&s);
			gpu.Retire(s, fence);
		}
		dirtyRanges_.clear();
	}
	ready_ = false;
}

void SceneBvh::BeginInstances() {
	instances_.clear();
	instanceBoxes_.clear();
	frameInstanceTris_ = 0;
}

void SceneBvh::AddInstance(AeHandle mesh, const Mat4& world, uint32_t skipSubMeshes, uint32_t materialOffset, uint32_t doubleSided, bool mirrored) {
	auto alias = aliases_.find(mesh);
	auto it = meshes_.find(alias != aliases_.end() ? alias->second : mesh);
	// (only meshes whose tree the GPU already has)
	if (it == meshes_.end() || uint64_t(it->second.root) + it->second.nodeCount > uploadedNodes_) {
		return;
	}
	const Mat4 inv = Inverse(world);
	TlasInstance instance = {};
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 4; ++c) {
			instance.worldToObject[r * 4 + c] = inv.at(r, c);
		}
	}
	instance.root = it->second.root;
	instance.skipSubMeshes = skipSubMeshes;
	instance.materialOffset = (materialOffset & 0x7FFFFFFFu) | (mirrored ? 0x80000000u : 0u);
	instance.doubleSided = doubleSided;
	// the tree's own box (exact also for a skinned mesh, whose pose the mesh's stated bounds know nothing of) in the
	// world: its centre moved, its half size through the absolute matrix
	const BvhNode& root = nodes_[it->second.root];
	const Vec3 localCentre = {(root.bmin[0] + root.bmax[0]) * 0.5f, (root.bmin[1] + root.bmax[1]) * 0.5f, (root.bmin[2] + root.bmax[2]) * 0.5f};
	const float half[3] = {(root.bmax[0] - root.bmin[0]) * 0.5f, (root.bmax[1] - root.bmin[1]) * 0.5f, (root.bmax[2] - root.bmin[2]) * 0.5f};
	InstanceBox box;
	const Vec3 centre = TransformPoint(world, localCentre);
	const float c[3] = {centre.x, centre.y, centre.z};
	for (int r = 0; r < 3; ++r) {
		const float h = std::abs(world.at(r, 0)) * half[0] + std::abs(world.at(r, 1)) * half[1] + std::abs(world.at(r, 2)) * half[2] + 1e-4f;
		box.bmin[r] = c[r] - h;
		box.bmax[r] = c[r] + h;
		if (!std::isfinite(h) || !std::isfinite(c[r])) {
			return;
		}
	}
	instances_.push_back(instance);
	instanceBoxes_.push_back(box);
	frameInstanceTris_ += it->second.triCount;
}

bool SceneBvh::EndInstances() {
	GpuResources& gpu = GpuResources::Get();
	const double start = NowMs();
	ready_ = false;
	tlasNodes_.clear();
	const uint32_t count = uint32_t(instances_.size());
	if (count > 0 && nodesVa_) {
		tlasOrder_.resize(count);
		for (uint32_t i = 0; i < count; ++i) {
			tlasOrder_[i] = i;
		}
		// halves by count along the widest axis of the boxes' centres; one instance per leaf
		struct Work { uint32_t node, first, count; };
		std::vector<Work> stack;
		tlasNodes_.push_back({});
		stack.push_back({0, 0, count});
		auto centre = [&](uint32_t i, int axis) { return (instanceBoxes_[i].bmin[axis] + instanceBoxes_[i].bmax[axis]) * 0.5f; };
		while (!stack.empty()) {
			const Work w = stack.back();
			stack.pop_back();
			float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, clo[3] = {1e30f, 1e30f, 1e30f}, chi[3] = {-1e30f, -1e30f, -1e30f};
			for (uint32_t k = w.first; k < w.first + w.count; ++k) {
				const InstanceBox& b = instanceBoxes_[tlasOrder_[k]];
				for (int a = 0; a < 3; ++a) {
					lo[a] = std::min(lo[a], b.bmin[a]);
					hi[a] = std::max(hi[a], b.bmax[a]);
					const float c = (b.bmin[a] + b.bmax[a]) * 0.5f;
					clo[a] = std::min(clo[a], c);
					chi[a] = std::max(chi[a], c);
				}
			}
			{
				BvhNode& n = tlasNodes_[w.node];
				memcpy(n.bmin, lo, sizeof(lo));
				memcpy(n.bmax, hi, sizeof(hi));
				if (w.count == 1) {
					n.a = w.first;
					n.b = 1;
					continue;
				}
			}
			const float ex = chi[0] - clo[0], ey = chi[1] - clo[1], ez = chi[2] - clo[2];
			const int axis = ex >= ey && ex >= ez ? 0 : ey >= ez ? 1 : 2;
			const uint32_t mid = w.first + w.count / 2;
			std::nth_element(tlasOrder_.begin() + w.first, tlasOrder_.begin() + mid, tlasOrder_.begin() + w.first + w.count, [&](uint32_t x, uint32_t y) { return centre(x, axis) < centre(y, axis); });
			const uint32_t left = uint32_t(tlasNodes_.size());
			tlasNodes_.push_back({});
			tlasNodes_.push_back({});
			tlasNodes_[w.node].a = left;
			tlasNodes_[w.node].b = 0;
			stack.push_back({left, w.first, mid - w.first});
			stack.push_back({left + 1, mid, w.first + w.count - mid});
		}
		// (the leaves name places in the sorted order: the instances are uploaded in that order)
		const UploadAllocation nodes = gpu.Upload(tlasNodes_.size() * sizeof(BvhNode), 16);
		const UploadAllocation instances = gpu.Upload(size_t(count) * sizeof(TlasInstance), 16);
		if (nodes.cpu && instances.cpu) {
			memcpy(nodes.cpu, tlasNodes_.data(), tlasNodes_.size() * sizeof(BvhNode));
			TlasInstance* out = static_cast<TlasInstance*>(instances.cpu);
			for (uint32_t k = 0; k < count; ++k) {
				out[k] = instances_[tlasOrder_[k]];
			}
			tlasNodesVa_ = nodes.gpu;
			tlasInstancesVa_ = instances.gpu;
			ready_ = true;
		}
	}
	uint32_t changing = 0;
	for (const auto& [handle, e] : meshes_) {
		changing += e.changing ? 1u : 0u;
	}
	std::lock_guard lock(statsMutex_);
	stats_.meshes = uint32_t(meshes_.size());
	stats_.changing = changing;
	stats_.pending = pendingJobs_.load();
	stats_.refused = refused_.load();
	stats_.instances = count;
	stats_.tlasNodes = uint32_t(tlasNodes_.size());
	stats_.maxDepth = maxDepth_;
	stats_.uploads = uploads_;
	stats_.compactions = compactions_;
	stats_.nodes = nodes_.size();
	stats_.tris = tris_.size();
	stats_.deadTris = deadTris_;
	stats_.gpuBytes = uploadedNodes_ * sizeof(BvhNode) + uploadedTris_ * sizeof(BvhTri);
	stats_.frameInstanceTris = frameInstanceTris_;
	stats_.refits = refits_.load();
	stats_.partialUploads = partialUploads_;
	stats_.buildMsTotal = buildMsTotal_;
	stats_.refitMsLast = refitMsLast_;
	stats_.tlasMs = NowMs() - start;
	return ready_;
}

void SceneBvh::Release(bool leak) {
	// The device's objects go; what was built stays (the pools are uploaded again when there is a device again).
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
		nodesBuffer_.Detach();
		trisBuffer_.Detach();
	}
	nodesBuffer_.Reset();
	trisBuffer_.Reset();
	nodesVa_ = trisVa_ = tlasNodesVa_ = tlasInstancesVa_ = 0;
	uploadedNodes_ = uploadedTris_ = 0;
	dirtyRanges_.clear();
	dirty_ = !nodes_.empty();
	ready_ = false;
}

json SceneBvh::Stats() {
	std::lock_guard lock(statsMutex_);
	return {
		{"meshes", stats_.meshes}, {"meshesChanging", stats_.changing}, {"meshesPending", stats_.pending}, {"meshesRefused", stats_.refused},
		{"triangles", stats_.tris}, {"trianglesOfGoneMeshes", stats_.deadTris}, {"nodes", stats_.nodes}, {"gpuBytes", stats_.gpuBytes},
		{"uploads", stats_.uploads}, {"compactions", stats_.compactions}, {"deepestTree", stats_.maxDepth}, {"buildMsTotal", stats_.buildMsTotal},
		{"refits", stats_.refits}, {"refitUploads", stats_.partialUploads}, {"lastRefitMs", stats_.refitMsLast},
		{"frame", {{"instances", stats_.instances}, {"instanceTriangles", stats_.frameInstanceTris}, {"topLevelNodes", stats_.tlasNodes}, {"topLevelBuildMs", stats_.tlasMs}}},
	};
}

} // namespace aether
