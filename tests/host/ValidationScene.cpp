#include "ValidationScene.h"
#include "Scene/SceneStore.h"

namespace aether::host {

namespace {

struct Vertex {
	float position[3];
	float normal[3];
	float tangent[4];
	float uv[2];
};

enum : AeHandle {
	kMeshBox = 1,
	kTexChecker = 1,
	kMatGround = 1, kMatRed, kMatGreen, kMatBlue, kMatYellow, kMatWhite, kMatGlass, kMatUnsupported,
};

// Camera: at (0, 2, -6), looking along +Z, level. In Unity's terms worldToCameraMatrix = scale(1, 1, -1) * inverse(TRS).
const Vec3 kCameraPosition = {0, 2, -6};
const float kFovY = 60.0f * 3.14159265f / 180.0f;
const float kNear = 0.3f, kFar = 200.0f;

Mat4 Trs(Vec3 t, Vec3 s) {
	Mat4 m = Mat4::Identity();
	m.at(0, 0) = s.x;
	m.at(1, 1) = s.y;
	m.at(2, 2) = s.z;
	m.at(0, 3) = t.x;
	m.at(1, 3) = t.y;
	m.at(2, 3) = t.z;
	return m;
}

// A unit box (half size 0.5) with Unity's winding: seen from outside every triangle is clockwise, which in a
// left-handed space means cross(v1 - v0, v2 - v0) points along the outward normal.
void BuildBox(std::vector<Vertex>& vertices, std::vector<uint16_t>& indices) {
	const Vec3 normals[6] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
	for (const Vec3& n : normals) {
		const Vec3 v = std::fabs(n.y) > 0.5f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
		const Vec3 u = Cross(n, v); // then cross(u, v) = -n, which makes (v0, v1, v2) clockwise from outside
		const Vec3 c = n * 0.5f;
		const Vec3 corners[4] = {c - u * 0.5f - v * 0.5f, c - u * 0.5f + v * 0.5f, c + u * 0.5f + v * 0.5f, c + u * 0.5f - v * 0.5f};
		const float uvs[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
		const uint16_t base = uint16_t(vertices.size());
		for (int i = 0; i < 4; ++i) {
			Vertex vert = {};
			vert.position[0] = corners[i].x; vert.position[1] = corners[i].y; vert.position[2] = corners[i].z;
			vert.normal[0] = n.x; vert.normal[1] = n.y; vert.normal[2] = n.z;
			vert.tangent[0] = u.x; vert.tangent[1] = u.y; vert.tangent[2] = u.z; vert.tangent[3] = 1;
			vert.uv[0] = uvs[i][0]; vert.uv[1] = uvs[i][1];
			vertices.push_back(vert);
		}
		for (int i : {0, 1, 2, 0, 2, 3}) {
			indices.push_back(uint16_t(base + i));
		}
	}
}

ComPtr<ID3D12Resource> UploadBuffer(ID3D12Device* device, const void* data, size_t bytes) {
	D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> buffer;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)))) {
		return nullptr;
	}
	void* p = nullptr;
	buffer->Map(0, nullptr, &p);
	memcpy(p, data, bytes);
	buffer->Unmap(0, nullptr);
	return buffer;
}

AeMaterialDesc Material(AeHandle handle, uint32_t materialClass, float r, float g, float b, float a, uint32_t flags = 0) {
	AeMaterialDesc m = {};
	m.structSize = sizeof(m);
	m.material = handle;
	m.materialClass = materialClass;
	m.flags = flags;
	m.baseColor[0] = r; m.baseColor[1] = g; m.baseColor[2] = b; m.baseColor[3] = a;
	m.roughness = 0.8f;
	m.normalScale = 1;
	m.alphaCutoff = 0.5f;
	m.occlusion = 1;
	m.uvScale[0] = m.uvScale[1] = 1;
	m.renderQueue = (flags & kAeMaterialBlend) ? 3000 : 2000;
	return m;
}

} // namespace

bool ValidationScene::Create(ID3D12Device* device, ID3D12CommandQueue* queue) {
	std::vector<Vertex> vertices;
	std::vector<uint16_t> indices;
	BuildBox(vertices, indices);
	boxVb_ = UploadBuffer(device, vertices.data(), vertices.size() * sizeof(Vertex));
	boxIb_ = UploadBuffer(device, indices.data(), indices.size() * sizeof(uint16_t));
	if (!boxVb_ || !boxIb_) {
		return false;
	}

	// a 64x64 checker texture, uploaded with the host's own list (the way Unity's textures already exist)
	const uint32_t size = 64;
	std::vector<uint8_t> pixels(size * size * 4);
	for (uint32_t y = 0; y < size; ++y) {
		for (uint32_t x = 0; x < size; ++x) {
			const uint8_t v = ((x / 8 + y / 8) & 1) ? 200 : 110;
			uint8_t* p = &pixels[(y * size + x) * 4];
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	}
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = size;
	td.Height = size;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	td.SampleDesc.Count = 1;
	D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&checker_)))) {
		return false;
	}
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT64 total = 0;
	device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &total);
	std::vector<uint8_t> staging(total);
	for (uint32_t y = 0; y < size; ++y) {
		memcpy(&staging[footprint.Offset + size_t(y) * footprint.Footprint.RowPitch], &pixels[y * size * 4], size * 4);
	}
	ComPtr<ID3D12Resource> upload = UploadBuffer(device, staging.data(), staging.size());
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	D3D12_TEXTURE_COPY_LOCATION dst = {checker_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
	D3D12_TEXTURE_COPY_LOCATION src = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
	src.PlacedFootprint = footprint;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = checker_.Get();
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	list->ResourceBarrier(1, &b);
	list->Close();
	ID3D12CommandList* lists[] = {list.Get()};
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.Get(), 1);
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	fence->SetEventOnCompletion(1, event);
	WaitForSingleObject(event, 5000);
	CloseHandle(event);

	// name, mesh, material, transform. Sizes are full extents of the unit box.
	instances_ = {
		{1, kMeshBox, kMatGround, Trs({0, -0.05f, 2}, {24, 0.1f, 24}), kAeInstanceReceiveShadows},
		{2, kMeshBox, kMatRed, Trs({-2, 0.5f, 0}, {1, 1, 1}), kAeInstanceCastShadows | kAeInstanceReceiveShadows},
		{3, kMeshBox, kMatGreen, Trs({2, 0.5f, 0}, {1, 1, 1}), kAeInstanceCastShadows | kAeInstanceReceiveShadows},
		{4, kMeshBox, kMatBlue, Trs({0, 2.5f, 0}, {1, 1, 1}), kAeInstanceCastShadows | kAeInstanceReceiveShadows},
		{5, kMeshBox, kMatYellow, Trs({0, 3, 6}, {16, 6, 0.2f}), kAeInstanceReceiveShadows},              // a wall behind everything
		{6, kMeshBox, kMatGlass, Trs({2, 0.5f, -1.5f}, {1.4f, 1.4f, 0.02f}), 0},                            // a see-through pane in front of the green box
		{7, kMeshBox, kMatUnsupported, Trs({-4, 0.5f, 0}, {1, 1, 1}), kAeInstanceCastShadows},              // the diagnostic material
		{8, kMeshBox, kMatWhite, Trs({4, 0.5f, 0}, {-1, 1, 1}), kAeInstanceCastShadows | kAeInstanceReceiveShadows}, // mirrored: negative determinant
	};
	return true;
}

void ValidationScene::Release() {
	boxVb_.Reset();
	boxIb_.Reset();
	checker_.Reset();
}

void ValidationScene::Register() {
	SceneStore& store = SceneStore::Get();
	AeMeshDesc mesh = {};
	mesh.structSize = sizeof(mesh);
	mesh.mesh = kMeshBox;
	mesh.vertexCount = 24;
	mesh.indexCount = 36;
	mesh.indexStride = 2;
	mesh.streamCount = 1;
	mesh.streamStride[0] = sizeof(Vertex);
	mesh.vertexBuffer[0] = boxVb_.Get();
	mesh.indexBuffer = boxIb_.Get();
	mesh.attributeCount = 4;
	mesh.attributes[0] = {kAeSemanticPosition, 0, 0, uint8_t(DXGI_FORMAT_R32G32B32_FLOAT)};
	mesh.attributes[1] = {kAeSemanticNormal, 0, 12, uint8_t(DXGI_FORMAT_R32G32B32_FLOAT)};
	mesh.attributes[2] = {kAeSemanticTangent, 0, 24, uint8_t(DXGI_FORMAT_R32G32B32A32_FLOAT)};
	mesh.attributes[3] = {kAeSemanticTexCoord0, 0, 40, uint8_t(DXGI_FORMAT_R32G32_FLOAT)};
	mesh.boundsExtents[0] = mesh.boundsExtents[1] = mesh.boundsExtents[2] = 0.5f;
	mesh.subMeshCount = 1;
	const AeSubMesh sub = {0, 36, 0};
	store.UpsertMesh(&mesh, &sub);

	AeTextureDesc tex = {};
	tex.structSize = sizeof(tex);
	tex.texture = kTexChecker;
	tex.resource = checker_.Get();
	tex.width = tex.height = 64;
	tex.mipCount = 1;
	tex.dimension = kAeTexture2D;
	store.UpsertTexture(&tex);

	AeMaterialDesc ground = Material(kMatGround, kAeMaterialStandard, 0.8f, 0.8f, 0.8f, 1);
	ground.textures[kAeSlotBase] = kTexChecker;
	ground.uvScale[0] = ground.uvScale[1] = 6;
	store.UpsertMaterial(&ground);
	AeMaterialDesc m = Material(kMatRed, kAeMaterialStandard, 0.9f, 0.05f, 0.05f, 1);
	store.UpsertMaterial(&m);
	m = Material(kMatGreen, kAeMaterialStandard, 0.05f, 0.9f, 0.05f, 1);
	store.UpsertMaterial(&m);
	m = Material(kMatBlue, kAeMaterialStandard, 0.05f, 0.05f, 0.9f, 1);
	store.UpsertMaterial(&m);
	m = Material(kMatYellow, kAeMaterialStandard, 0.9f, 0.9f, 0.05f, 1);
	store.UpsertMaterial(&m);
	m = Material(kMatWhite, kAeMaterialStandard, 0.9f, 0.9f, 0.9f, 1);
	store.UpsertMaterial(&m);
	m = Material(kMatGlass, kAeMaterialTransparent, 0.0f, 0.2f, 1.0f, 0.5f, kAeMaterialBlend | kAeMaterialDoubleSided);
	store.UpsertMaterial(&m);
	m = Material(kMatUnsupported, kAeMaterialUnsupported, 1, 1, 1, 1);
	store.UpsertMaterial(&m);

	for (const Instance& i : instances_) {
		AeInstanceDesc d = {};
		d.structSize = sizeof(d);
		d.instance = i.handle;
		d.mesh = i.mesh;
		d.flags = i.flags;
		d.materialCount = 1;
		store.UpsertInstance(&d, &i.material);
	}
	removed_ = false;
}

Mat4 ValidationScene::View() const {
	Mat4 v = Mat4::Identity();
	v.at(0, 3) = -kCameraPosition.x;
	v.at(1, 3) = -kCameraPosition.y;
	v.at(2, 2) = -1.0f;                 // right-handed view space: -Z is forward
	v.at(2, 3) = kCameraPosition.z;
	return v;
}

Mat4 ValidationScene::Proj(uint32_t width, uint32_t height) const {
	return PerspectiveReversedZ(kFovY, float(width) / float(height), kNear, kFar);
}

void ValidationScene::SubmitFrame(uint32_t width, uint32_t height, uint32_t frameIndex, float time) {
	AeFrame f = {};
	f.structSize = sizeof(f);
	f.contractVersion = AE_SCENE_CONTRACT_VERSION;
	f.flags = kAeFrameCameraValid;
	f.sceneId = 1;
	f.cameraId = 1;
	f.width = width;
	f.height = height;
	f.frameIndex = frameIndex;
	memcpy(f.view, View().m, sizeof(f.view));
	memcpy(f.proj, Proj(width, height).m, sizeof(f.proj));
	f.cameraPosition[0] = kCameraPosition.x; f.cameraPosition[1] = kCameraPosition.y; f.cameraPosition[2] = kCameraPosition.z;
	f.cameraForward[2] = 1;
	f.nearPlane = kNear;
	f.farPlane = kFar;
	f.deltaTime = 1.0f / 60.0f;
	f.time = time;
	f.exposure = 0.35f;
	f.ambientIntensity = 1;
	// a flat ambient term (the constant band only): unity_SHA*.w
	f.ambientSH[0][3] = 0.25f;
	f.ambientSH[1][3] = 0.30f;
	f.ambientSH[2][3] = 0.40f;

	frameInstances_.clear();
	for (const Instance& i : instances_) {
		if (removed_ && i.handle == 4) {
			continue; // the blue box is away during the churn test
		}
		AeFrameInstance fi = {};
		fi.instance = i.handle;
		fi.flags = kAeFrameInstanceVisible;
		memcpy(fi.localToWorld, i.world.m, sizeof(fi.localToWorld));
		frameInstances_.push_back(fi);
	}
	AeLight lights[2] = {};
	// the sun travels towards +X and down: shadows fall on the +X side of the boxes
	const Vec3 travel = Normalize({0.8f, -0.6f, 0.0f});
	lights[0].type = kAeLightDirectional;
	lights[0].flags = kAeLightCastsShadows;
	lights[0].direction[0] = travel.x; lights[0].direction[1] = travel.y; lights[0].direction[2] = travel.z;
	lights[0].color[0] = lights[0].color[1] = lights[0].color[2] = 6.0f;
	// a spot light above the origin (clustered lighting)
	lights[1].type = kAeLightSpot;
	lights[1].position[0] = 0; lights[1].position[1] = 4; lights[1].position[2] = -2;
	lights[1].range = 8;
	lights[1].direction[1] = -1;
	lights[1].spotOuterCos = std::cos(0.6f);
	lights[1].spotInnerCos = std::cos(0.3f);
	lights[1].color[0] = 30; lights[1].color[1] = 10; lights[1].color[2] = 30;
	SceneStore::Get().SubmitFrame(&f, frameInstances_.data(), uint32_t(frameInstances_.size()), lights, 2);
}

void ValidationScene::Churn(uint32_t frameIndex) {
	SceneStore& store = SceneStore::Get();
	if (frameIndex == 100) {
		// an instance goes away, and the texture gets "a new resource" (what texture streaming does)
		store.Destroy(3, 4);
		removed_ = true;
		AeTextureDesc tex = {};
		tex.structSize = sizeof(tex);
		tex.texture = kTexChecker;
		tex.resource = checker_.Get();
		tex.width = tex.height = 64;
		tex.mipCount = 1;
		tex.dimension = kAeTexture2D;
		store.UpsertTexture(&tex);
	} else if (frameIndex == 110) {
		store.Clear(); // a scene unload: every handle retired
	} else if (frameIndex == 112) {
		Register();    // and the next scene arrives
	}
}

void ValidationScene::PrintProbes(uint32_t width, uint32_t height) const {
	const Probe probes[] = {
		{"red", {-2, 0.5f, -0.5f}},          // front face of the red box (left of centre)
		{"green_glass", {2, 0.5f, -1.5f}},   // the green box seen through the blue-tinted pane (right of centre)
		{"blue", {0, 2.5f, -0.5f}},          // front face of the blue box (above centre)
		{"wall", {-5, 4.5f, 5.9f}},          // the yellow wall where nothing is in front of it
		{"unsupported", {-4, 0.5f, -0.5f}},  // the diagnostic material
		{"mirrored", {4, 0.5f, -0.5f}},      // front face of the mirrored white box
		{"ground_lit", {-1.0f, 0, -2.0f}},   // ground in the sun
		{"ground_shadow", {-1.0f, 0, 0}},    // ground in the red box's shadow
	};
	const Mat4 viewProj = Proj(width, height) * View();
	for (const Probe& p : probes) {
		const Vec3 ndc = TransformProjective(viewProj, p.world);
		const int x = int((ndc.x * 0.5f + 0.5f) * float(width));
		const int y = int((1.0f - (ndc.y * 0.5f + 0.5f)) * float(height));
		printf("probe %s %d %d\n", p.name, x, y);
	}
}

} // namespace aether::host
