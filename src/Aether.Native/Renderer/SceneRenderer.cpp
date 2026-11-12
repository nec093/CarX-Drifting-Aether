#include "SceneRenderer.h"
#include "../Core/Log.h"
#include "../Resources/GpuResources.h"
#include "../Shaders/ShaderManager.h"

#include <algorithm>

namespace aether {

namespace {

const DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R11G11B10_FLOAT;
const uint32_t kClusterX = 16, kClusterY = 9, kClusterZ = 24;
const uint32_t kMaxLightsPerCluster = 64;
const uint32_t kGlobalsTableSize = 4; // shadow atlas, sky, sky irradiance, ambient occlusion
const uint32_t kPostTableSize = 4;    // per full screen pass: colour / main input, depth, two auxiliary inputs
const float kExposureAdaptationRate = 2.0f; // per second: the automatic exposure covers 63 % of a change in half a second
const uint32_t kPostTables = 64;     // 0..3: ambient occlusion, its blur, composite, exposure metering; the rest handed out per pass (Fullscreen)
const uint32_t kIrradianceSize = 32;  // octahedral map of the sky's diffuse light
const char* const kFullscreenEntry[] = {"PSSky", "PSComposite", "PSAmbientOcclusion", "PSAmbientOcclusionBlur", "PSExposure", "PSIrradiance", "PSTaa", "PSMotionBlur", "PSBloomDown", "PSBloomUp", "PSSsr"};
const uint32_t kBloomLevels = 5;
// xy: how far each pixel moved on screen since the frame before (uv units; x = 1000: nothing drawn there),
// z: 1 = the surface belongs to something that moves by itself (a car). (The scene colour target has no alpha
// channel, so this cannot live there: a mask written to its alpha read back as 1 everywhere.)
const DXGI_FORMAT kVelocityFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
const DXGI_FORMAT kSurfaceFormat = DXGI_FORMAT_R16G16B16A16_UNORM; // normal (octahedral, xy), roughness of the reflecting lobe, its weight
const uint32_t kMaxProbes = 4;        // reflection probes blended per frame (the nearest to the camera)
const uint32_t kDefaultsSlot = 7; // vertex buffer slot that feeds the semantics a mesh does not have
// Unity's meshes are clockwise when seen from the front, and the contract's projection keeps that on screen.
const BOOL kFrontCounterClockwise = FALSE;

// Mirrors of the HLSL layouts in shaders/aether_scene.hlsli
struct FrameConstants {
	float view[16], proj[16], viewProj[16], invViewProj[16];
	float shadowViewProj[4][16];
	float shadowMatrix[4][16];
	float cascadeSplits[4];
	float shadowParams[4];
	float cameraPos[4];
	float screenSize[4];
	float clusterCounts[4];
	float clusterZ[4];
	float sunDirection[4];
	float sunColor[4];
	float sh[7][4];
	float exposure[4];
	float misc[4];
	float fog[4];
	float outputRect[4];
	float toneCurve[4];   // 1 / white point, x0, x1, unused
	float toneSegA[3][4]; // per segment (toe, linear, shoulder): offset x, offset y, scale x, scale y
	float toneSegB[3][4]; // ln A, B
	float fogHeight[4];   // base height, 1 / scale height, max distance (0 = none), 1 = colour from the sky
	float cascadeTexel[4]; // world size of one shadow texel, per cascade
	float aoParams[4];    // intensity, radius (m), strength on direct light, 1 = the AO texture is valid
	float smh[3][4];      // shadows, midtones, highlights multipliers
	float smhLimits[4];   // shadows start / end, highlights start / end (all 0 = off)
	float exposureParams[4]; // metering: min EV100, max EV100, compensation
	float probeCount[4];     // x: number of probes
	float probeRows[4][3][4]; // world -> probe space, three rows each
	float probeExtents[4][4]; // xyz half size (sphere: radius in x), w blend distance
	float probeCapture[4][4]; // xyz capture position, w intensity
	float probeInfo[4][4];    // x flags (AeProbeFlags), y mip count
	float skyParams[4];       // intensity, cos and sin of the rotation about the up axis
	float prevViewProj[16];   // the frame before: world -> clip, without jitter
	float jitter[4];          // this frame's sub pixel offset in clip space (xy), z: 1 = the history is valid
	float postParams[4];      // bloom mix (0 = off), bloom scatter, motion blur intensity, 1 = temporal antialiasing
	float ssrParams[4];       // 1 = on, smoothness below which there is none, smoothness from which it is full, screen fade distance
	float qualityParams[4];   // x: how far the sun's shadow edges are blurred, in shadow map texels
};

// HDRP's "Custom" tone mapping is John Hable's piecewise filmic curve (toe, linear part, shoulder), with the parameters
// HDRP's volume component exposes. Computes the three segments the shader evaluates.
void HableCurve(const float user[6], float curve[4], float segA[3][4], float segB[3][4]) {
	auto saturate = [](float v) { return std::min(std::max(v, 0.0f), 1.0f); };
	float toeStrength = saturate(user[0]);
	float toeLength = std::pow(saturate(user[1]), 2.2f);
	// (the host's naming, not Hable's: the strength is the 0..1 transition into the shoulder, the length is the number
	// of stops the shoulder adds to the curve's range)
	float shoulderStrength = std::min(std::max(user[2], 1e-5f), 1.0f - 1e-5f);
	float shoulderLength = std::max(0.0f, user[3]);
	float shoulderAngle = saturate(user[4]);
	float gamma = user[5] > 0.0f ? user[5] : 1.0f;

	float x0 = toeLength * 0.5f;
	float y0 = (1.0f - toeStrength) * x0;
	const float remainingY = 1.0f - y0;
	const float initialW = x0 + remainingY;
	const float y1Offset = (1.0f - shoulderStrength) * remainingY;
	float x1 = x0 + y1Offset;
	float y1 = y0 + y1Offset;
	const float W = initialW + (std::exp2(shoulderLength) - 1.0f);
	float overshootX = (W * 2.0f) * shoulderAngle * shoulderLength;
	float overshootY = 0.5f * shoulderAngle * shoulderLength;

	// in the curve's own space the white point is 1
	x0 /= W;
	x1 /= W;
	overshootX /= W;
	const float dx = x1 - x0, dy = y1 - y0;
	const float m = dx > 1e-6f ? dy / dx : 1.0f;
	const float b = y0 - x0 * m;
	auto derivative = [&](float x) { return gamma * m * std::pow(std::max(m * x + b, 1e-6f), gamma - 1.0f); };
	auto solveAB = [](float x, float y, float slope, float& lnA, float& B) {
		B = (slope * x) / std::max(y, 1e-6f);
		lnA = std::log(std::max(y, 1e-6f)) - B * std::log(std::max(x, 1e-6f));
	};
	struct Segment { float offsetX = 0, offsetY = 0, scaleX = 1, scaleY = 1, lnA = 0, B = 1; } seg[3];
	// linear part (with the gamma folded in)
	seg[1].offsetX = -(b / m);
	seg[1].lnA = gamma * std::log(m);
	seg[1].B = gamma;
	const float toeM = derivative(x0), shoulderM = derivative(x1);
	y0 = std::max(1e-5f, std::pow(y0, gamma));
	y1 = std::max(1e-5f, std::pow(y1, gamma));
	overshootY = std::pow(1.0f + overshootY, gamma) - 1.0f;
	solveAB(x0, y0, toeM, seg[0].lnA, seg[0].B);
	// the shoulder is the toe's shape mirrored around the overshoot point
	const float sx = (1.0f + overshootX) - x1, sy = (1.0f + overshootY) - y1;
	solveAB(sx, sy, shoulderM, seg[2].lnA, seg[2].B);
	seg[2].offsetX = 1.0f + overshootX;
	seg[2].offsetY = 1.0f + overshootY;
	seg[2].scaleX = -1.0f;
	seg[2].scaleY = -1.0f;
	auto eval = [](const Segment& s, float x) {
		const float xs = (x - s.offsetX) * s.scaleX;
		const float ys = xs > 0.0f ? std::exp(s.lnA + s.B * std::log(xs)) : 0.0f;
		return ys * s.scaleY + s.offsetY;
	};
	// normalise so that the white point maps to 1
	const float invScale = 1.0f / std::max(eval(seg[2], 1.0f), 1e-6f);
	for (Segment& s : seg) {
		s.offsetY *= invScale;
		s.scaleY *= invScale;
	}
	curve[0] = 1.0f / W;
	curve[1] = x0;
	curve[2] = x1;
	curve[3] = 0.0f;
	for (int i = 0; i < 3; ++i) {
		segA[i][0] = seg[i].offsetX; segA[i][1] = seg[i].offsetY; segA[i][2] = seg[i].scaleX; segA[i][3] = seg[i].scaleY;
		segB[i][0] = seg[i].lnA; segB[i][1] = seg[i].B; segB[i][2] = segB[i][3] = 0.0f;
	}
}

struct InstanceData {
	float world[16];
	float normalRow0[4], normalRow1[4], normalRow2[4];
	float reserved[4]; // x: rendering layers (as uint), y: 1 = moved since the frame before
	float prevWorld[16]; // the instance's world matrix of the frame before
};

struct MaterialData {
	float baseColor[4];
	float secondaryColor[4];
	float emissiveMetallic[4];
	float params[4];
	float params2[4];
	float uvScaleOffset[4];
	float params3[4];
};

struct LightData {
	float positionRange[4];
	float directionOuter[4];
	float colorInner[4];
	float typeFlags[4];
};

struct DefaultVertex {
	float normal[4];
	float tangent[4];
	float color[4];
	float uv[2];
};

struct Plane { float x, y, z, d; };

void FrustumPlanes(const Mat4& m, Plane out[6]) {
	auto row = [&](int r, float s, int r2) {
		return Plane{m.at(3, 0) + s * m.at(r, 0) * float(r2), m.at(3, 1) + s * m.at(r, 1) * float(r2), m.at(3, 2) + s * m.at(r, 2) * float(r2), m.at(3, 3) + s * m.at(r, 3) * float(r2)};
	};
	out[0] = row(0, 1, 1);   // left:   w + x >= 0
	out[1] = row(0, -1, 1);  // right:  w - x >= 0
	out[2] = row(1, 1, 1);   // bottom: w + y >= 0
	out[3] = row(1, -1, 1);  // top:    w - y >= 0
	out[4] = row(2, -1, 1);  // near (reversed Z): w - z >= 0
	out[5] = Plane{m.at(2, 0), m.at(2, 1), m.at(2, 2), m.at(2, 3)}; // far: z >= 0
}

bool SphereVisible(const Plane planes[6], Vec3 c, float r) {
	for (int i = 0; i < 6; ++i) {
		const Plane& p = planes[i];
		float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
		if (p.x * c.x + p.y * c.y + p.z * c.z + p.d < -r * len) {
			return false;
		}
	}
	return true;
}

const char* kSemanticNames[kAeSemanticCount] = {"POSITION", "NORMAL", "TANGENT", "COLOR", "TEXCOORD", "TEXCOORD"};
const UINT kSemanticIndex[kAeSemanticCount] = {0, 0, 0, 0, 0, 1};

} // namespace

SceneRenderer& SceneRenderer::Get() {
	static SceneRenderer* s = new SceneRenderer();
	return *s;
}

// ------------------------------------------------------------------------------------------------ device objects

bool SceneRenderer::EnsureObjects(const HostGfx& host, uint64_t frame) {
	GpuResources& gpu = GpuResources::Get();
	if (device_ != host.device) {
		device_ = host.device;
		objectsReady_ = false;
	}
	if (!gpu.Init(device_)) {
		return false;
	}
	if (!timer_.Ready()) {
		timer_.Init(device_, host.queue, frame);
	}
	const uint64_t generation = ShaderManager::Get().Generation();
	if (objectsReady_ && generation == shaderGeneration_) {
		return !failedShaders_;
	}
	// (re)build: shaders first, the pipelines are made lazily per vertex layout
	for (auto& [key, pso] : pipelines_) {
		gpu.Retire(pso, retireFence_);
	}
	for (auto& [key, pso] : fullscreenPipelines_) {
		gpu.Retire(pso, retireFence_);
	}
	pipelines_.clear();
	fullscreenPipelines_.clear();
	shaderGeneration_ = generation;
	ShaderManager& sm = ShaderManager::Get();
	vsMain_ = sm.Compile("scene_forward.hlsl", "VSMain", "vs_5_1");
	vsShadow_ = sm.Compile("scene_forward.hlsl", "VSShadow", "vs_5_1");
	psForward_ = sm.Compile("scene_forward.hlsl", "PSForward", "ps_5_1");
	psDepthAlpha_ = sm.Compile("scene_forward.hlsl", "PSDepthAlphaTest", "ps_5_1");
	psShadowAlpha_ = sm.Compile("scene_forward.hlsl", "PSShadowAlphaTest", "ps_5_1");
	vsFullscreen_ = sm.Compile("scene_post.hlsl", "VSFullscreen", "vs_5_1");
	psDecal_ = sm.Compile("scene_forward.hlsl", "PSBlobDecal", "ps_5_1");
	// (the particle shaders are not a reason to stop the scene: without them there are no particles)
	vsParticle_ = sm.Compile("scene_particles.hlsl", "VSParticle", "vs_5_1");
	psParticle_ = sm.Compile("scene_particles.hlsl", "PSParticle", "ps_5_1");
	particlePipeline_.Reset();
	particlePipelineFailed_ = false;
	failedShaders_ = !vsMain_ || !vsShadow_ || !psForward_ || !psDepthAlpha_ || !psShadowAlpha_ || !vsFullscreen_ || !psDecal_;
	static_assert(_countof(kFullscreenEntry) == kFullscreenKindCount, "one entry point per full screen pass");
	for (int k = 0; k < kFullscreenKindCount; ++k) {
		psFullscreen_[k] = sm.Compile("scene_post.hlsl", kFullscreenEntry[k], "ps_5_1");
		failedShaders_ |= !psFullscreen_[k];
	}
	if (failedShaders_) {
		AE_LOG_CODE(Error, "SceneRenderer", "E_SCENE_SHADERS", "Scene shaders did not compile: Aether draws no scene until they do (see get_shader_errors, then reload_shader)");
	}

	if (!root_) {
		D3D12_DESCRIPTOR_RANGE materialRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kAeSlotCount, 0, 1, 0};
		// space2: t0 shadow atlas, t1 sky, t2 exposure, t3 ambient occlusion | t4..t6 the inputs of a full screen pass
		D3D12_DESCRIPTOR_RANGE globalsRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kGlobalsTableSize, 0, 2, 0};
		D3D12_DESCRIPTOR_RANGE postRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kPostTableSize, 4, 2, 0};
		D3D12_DESCRIPTOR_RANGE probesRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaxProbes, 0, 3, 0}; // space3: reflection probe cubes
		D3D12_ROOT_PARAMETER params[12] = {};
		params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t5: the instance map
		params[11].Descriptor = {5, 0};
		params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[10].DescriptorTable = {1, &probesRange};
		params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].Descriptor = {0, 0};
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[1].Constants = {1, 0, 4};
		for (UINT i = 0; i < 5; ++i) {
			params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[2 + i].Descriptor = {i, 0};
		}
		params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[7].DescriptorTable = {1, &materialRange};
		params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[8].DescriptorTable = {1, &globalsRange};
		params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[9].DescriptorTable = {1, &postRange};
		params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_STATIC_SAMPLER_DESC samplers[4] = {};
		samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
		samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[0].MaxAnisotropy = 8;
		samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
		samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		samplers[1] = samplers[0];
		samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[1].MaxAnisotropy = 1;
		samplers[1].ShaderRegister = 1;
		samplers[2] = samplers[1];
		samplers[2].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplers[2].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE; // outside the atlas: lit
		samplers[2].ShaderRegister = 2;
		samplers[3] = samplers[1];
		samplers[3].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		samplers[3].ShaderRegister = 3;
		D3D12_ROOT_SIGNATURE_DESC rs = {UINT(_countof(params)), params, UINT(_countof(samplers)), samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
		ComPtr<ID3DBlob> blob, error;
		HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
		if (SUCCEEDED(hr)) {
			hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
		}
		if (FAILED(hr)) {
			Log::Get().Write(Severity::Error, "SceneRenderer", "E_DEVICE_OBJECTS", "The scene root signature could not be created", {},
				{{"hresult", HrToString(hr)}, {"message", error ? std::string(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize()) : ""}});
			return false;
		}
		root_->SetName(L"Aether.SceneRoot");
	}
	if (!defaultsVb_) {
		const DefaultVertex v = {{0, 0, 1, 0}, {1, 0, 0, 1}, {1, 1, 1, 1}, {0, 0}};
		D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = sizeof(v);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&defaultsVb_));
		void* p = nullptr;
		if (SUCCEEDED(hr)) {
			hr = defaultsVb_->Map(0, nullptr, &p);
		}
		if (FAILED(hr)) {
			return false;
		}
		memcpy(p, &v, sizeof(v));
		defaultsVb_->Unmap(0, nullptr);
		defaultsView_ = {defaultsVb_->GetGPUVirtualAddress(), UINT(sizeof(v)), UINT(sizeof(v))};
		defaultsVbId_ = ResourceRegistry::Get().Register(device_, defaultsVb_.Get(), "Scene.DefaultVertex", ResourceCategory::Buffer, D3D12_HEAP_TYPE_UPLOAD, frame);
	}
	if (!white_) {
		GpuTextureDesc d;
		d.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		white_ = gpu.CreateTexture(d, "Scene.DefaultWhite", ResourceCategory::Texture, Lifetime::Persistent, frame);
		flatNormal_ = gpu.CreateTexture(d, "Scene.DefaultNormal", ResourceCategory::Texture, Lifetime::Persistent, frame);
		black_ = gpu.CreateTexture(d, "Scene.DefaultBlack", ResourceCategory::Texture, Lifetime::Persistent, frame);
		if (!white_ || !flatNormal_ || !black_) {
			return false;
		}
		defaultsUploaded_ = false;
		SceneStore::Get().SetDefaultTextures(white_->srv, flatNormal_->srv, black_->srv);
		// null views for the global slots that have nothing bound (reads return 0)
		nullCubeSrv_ = gpu.AllocSrv(1);
		nullTextureSrv_ = gpu.AllocSrv(1);
		D3D12_SHADER_RESOURCE_VIEW_DESC cube = {};
		cube.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		cube.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
		cube.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		cube.TextureCubeArray.MipLevels = 1;
		cube.TextureCubeArray.NumCubes = 1;
		D3D12_SHADER_RESOURCE_VIEW_DESC tex = {};
		tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		tex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		tex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		tex.Texture2D.MipLevels = 1;
		if (nullCubeSrv_ != UINT32_MAX && nullTextureSrv_ != UINT32_MAX) {
			gpu.WriteSrv(nullCubeSrv_, nullptr, &cube);
			gpu.WriteSrv(nullTextureSrv_, nullptr, &tex);
		}
		fallbackTable_ = gpu.AllocSrv(kAeSlotCount);
		if (fallbackTable_ != UINT32_MAX) {
			for (uint32_t slot = 0; slot < kAeSlotCount; ++slot) {
				gpu.CopySrv(fallbackTable_ + slot, slot == kAeSlotNormal ? flatNormal_->srv : white_->srv);
			}
		}
	}
	objectsReady_ = true;
	return !failedShaders_;
}

void SceneRenderer::UploadDefaults(ID3D12GraphicsCommandList* list) {
	if (defaultsUploaded_) {
		return;
	}
	GpuResources& gpu = GpuResources::Get();
	struct Item { GpuTexture* texture; uint8_t rgba[4]; };
	Item items[3] = {{white_.get(), {255, 255, 255, 255}}, {flatNormal_.get(), {128, 128, 255, 255}}, {black_.get(), {0, 0, 0, 255}}};
	for (Item& item : items) {
		UploadAllocation a = gpu.Upload(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		if (!a.cpu) {
			return; // tried again next frame
		}
		memcpy(a.cpu, item.rgba, 4);
		gpu.Transition(list, *item.texture, D3D12_RESOURCE_STATE_COPY_DEST);
		D3D12_TEXTURE_COPY_LOCATION dst = {item.texture->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
		D3D12_TEXTURE_COPY_LOCATION src = {a.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
		src.PlacedFootprint.Offset = a.offset;
		src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		gpu.Transition(list, *item.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	defaultsUploaded_ = true;
}

bool SceneRenderer::EnsureShadowAtlas(uint32_t cascadeSize, uint64_t frame, UINT64 retireFence) {
	if (shadowAtlas_ && shadowCascadeSize_ == cascadeSize) {
		return true;
	}
	GpuResources& gpu = GpuResources::Get();
	if (shadowAtlas_) {
		gpu.RetireTexture(std::move(shadowAtlas_), retireFence, frame);
	}
	GpuTextureDesc d;
	d.width = d.height = cascadeSize * 2; // 2 x 2 cascades
	d.format = DXGI_FORMAT_R32_TYPELESS;
	d.srvFormat = DXGI_FORMAT_R32_FLOAT;
	d.targetFormat = DXGI_FORMAT_D32_FLOAT;
	d.depthStencil = true;
	d.clearDepth = 1.0f; // standard depth in the shadow maps
	shadowAtlas_ = gpu.CreateTexture(d, "Scene.ShadowAtlas", ResourceCategory::ShadowMap, Lifetime::Persistent, frame);
	shadowCascadeSize_ = shadowAtlas_ ? cascadeSize : 0;
	return shadowAtlas_ != nullptr;
}

ID3D12PipelineState* SceneRenderer::Pipeline(uint32_t layoutId, PassKind kind, uint32_t cull) {
	const uint64_t key = (uint64_t(layoutId) << 16) | (uint64_t(kind) << 8) | cull;
	auto it = pipelines_.find(key);
	if (it != pipelines_.end()) {
		return it->second.Get();
	}
	const SceneLayout& layout = SceneStore::Get().Layouts()[layoutId];
	// the five semantics the shaders read: from the mesh where it has them, else from the defaults buffer
	D3D12_INPUT_ELEMENT_DESC elements[5];
	static const AeSemantic wanted[5] = {kAeSemanticPosition, kAeSemanticNormal, kAeSemanticTangent, kAeSemanticColor, kAeSemanticTexCoord0};
	static const UINT defaultOffset[5] = {0, 0, 16, 32, 48};
	static const DXGI_FORMAT defaultFormat[5] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32_FLOAT};
	bool hasPosition = false;
	for (int i = 0; i < 5; ++i) {
		const AeVertexAttribute* found = nullptr;
		for (uint32_t a = 0; a < layout.attributeCount; ++a) {
			if (layout.attributes[a].semantic == wanted[i]) {
				found = &layout.attributes[a];
				break;
			}
		}
		D3D12_INPUT_ELEMENT_DESC& e = elements[i];
		e = {};
		e.SemanticName = kSemanticNames[wanted[i]];
		e.SemanticIndex = kSemanticIndex[wanted[i]];
		if (found) {
			e.Format = DXGI_FORMAT(found->dxgiFormat);
			e.InputSlot = found->stream;
			e.AlignedByteOffset = found->offset;
			e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
			hasPosition |= wanted[i] == kAeSemanticPosition;
		} else {
			e.Format = defaultFormat[i];
			e.InputSlot = kDefaultsSlot;
			e.AlignedByteOffset = defaultOffset[i];
			e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
			e.InstanceDataStepRate = 1;
		}
	}
	ComPtr<ID3D12PipelineState> pso;
	if (hasPosition) {
		const bool shadow = kind == kShadow || kind == kShadowAlpha;
		const bool forward = kind >= kForward;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.pRootSignature = root_.Get();
		ID3DBlob* vs = shadow ? vsShadow_.Get() : vsMain_.Get();
		d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
		ID3DBlob* ps = kind == kDecal ? psDecal_.Get() : forward ? psForward_.Get() : kind == kDepthAlpha ? psDepthAlpha_.Get() : kind == kShadowAlpha ? psShadowAlpha_.Get() : nullptr;
		if (ps) {
			d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
		}
		d.SampleMask = UINT_MAX;
		d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		d.RasterizerState.CullMode = shadow || cull == kCullNone ? D3D12_CULL_MODE_NONE : cull == kCullFront ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
		d.RasterizerState.FrontCounterClockwise = kFrontCounterClockwise;
		d.RasterizerState.DepthClipEnable = shadow ? FALSE : TRUE; // casters in front of a cascade's near plane still cast
		if (shadow) {
			d.RasterizerState.DepthBias = 200;
			d.RasterizerState.SlopeScaledDepthBias = 2.0f;
		}
		// (a decal box is drawn without a depth buffer: its pixel shader reads the scene depth and decides itself)
		d.DepthStencilState.DepthEnable = kind == kDecal ? FALSE : TRUE;
		d.DepthStencilState.DepthWriteMask = forward ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
		d.DepthStencilState.DepthFunc = shadow ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		d.DSVFormat = kind == kDecal ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_D32_FLOAT;
		D3D12_RENDER_TARGET_BLEND_DESC& blend = d.BlendState.RenderTarget[0];
		blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		if (kind == kForwardBlend || kind == kForwardAdditive || kind == kDecal) {
			blend.BlendEnable = TRUE;
			blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
			blend.DestBlend = kind == kForwardAdditive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOp = D3D12_BLEND_OP_ADD;
			blend.SrcBlendAlpha = D3D12_BLEND_ONE;
			blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		}
		if (forward) {
			// the opaque pass also writes what the reflection pass needs to know about each surface
			d.NumRenderTargets = kind == kForward ? 3 : 1;
			d.RTVFormats[0] = kSceneColorFormat;
			d.RTVFormats[1] = kind == kForward ? kSurfaceFormat : DXGI_FORMAT_UNKNOWN;
			d.RTVFormats[2] = kind == kForward ? kVelocityFormat : DXGI_FORMAT_UNKNOWN;
		}
		d.InputLayout = {elements, 5};
		d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.SampleDesc.Count = 1;
		HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
		if (FAILED(hr)) {
			Log::Get().Write(Severity::Error, "SceneRenderer", "E_PIPELINE_STATE", "A scene pipeline state could not be created: meshes with this vertex layout are not drawn in this pass", {},
				{{"hresult", HrToString(hr)}, {"layout", layoutId}, {"pass", int(kind)}});
			pso.Reset();
		}
	}
	pipelines_[key] = pso; // a failure is remembered too: not retried every draw
	return pso.Get();
}

// Particles: quads made in the vertex shader from the particle buffer, blended over the picture with premultiplied
// alpha (an additive particle returns alpha 0). No depth buffer bound: the pixel shader tests against the scene depth
// itself, which also lets a quad fade out where it cuts into a surface.
ID3D12PipelineState* SceneRenderer::ParticlePipeline() {
	if (particlePipeline_ || particlePipelineFailed_ || !vsParticle_ || !psParticle_) {
		return particlePipeline_.Get();
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
	d.pRootSignature = root_.Get();
	d.VS = {vsParticle_->GetBufferPointer(), vsParticle_->GetBufferSize()};
	d.PS = {psParticle_->GetBufferPointer(), psParticle_->GetBufferSize()};
	d.SampleMask = UINT_MAX;
	d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	d.RasterizerState.DepthClipEnable = TRUE;
	D3D12_RENDER_TARGET_BLEND_DESC& blend = d.BlendState.RenderTarget[0];
	blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	blend.BlendEnable = TRUE;
	blend.SrcBlend = D3D12_BLEND_ONE;
	blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOp = D3D12_BLEND_OP_ADD;
	blend.SrcBlendAlpha = D3D12_BLEND_ZERO; // (the colour target's alpha is the temporal passes' mask: left as it is)
	blend.DestBlendAlpha = D3D12_BLEND_ONE;
	blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	d.NumRenderTargets = 1;
	d.RTVFormats[0] = kSceneColorFormat;
	d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.SampleDesc.Count = 1;
	HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&particlePipeline_));
	if (FAILED(hr)) {
		Log::Get().Write(Severity::Error, "SceneRenderer", "E_PIPELINE_STATE", "The particle pipeline state could not be created: no particles", {}, {{"hresult", HrToString(hr)}});
		particlePipeline_.Reset();
		particlePipelineFailed_ = true;
	}
	return particlePipeline_.Get();
}

ID3D12PipelineState* SceneRenderer::FullscreenPipeline(FullscreenKind kind, DXGI_FORMAT format) {
	const bool composite = kind == kFullscreenComposite;
	const bool sky = kind == kFullscreenSky;
	const uint32_t key = (uint32_t(kind) << 16) | uint32_t(format);
	auto it = fullscreenPipelines_.find(key);
	if (it != fullscreenPipelines_.end()) {
		return it->second.Get();
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
	d.pRootSignature = root_.Get();
	d.VS = {vsFullscreen_->GetBufferPointer(), vsFullscreen_->GetBufferSize()};
	ID3DBlob* ps = psFullscreen_[kind].Get();
	d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
	d.SampleMask = UINT_MAX;
	d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	if (kind == kFullscreenSsr) {
		// added to the picture (the shader returns the reflection minus what the surface already shows there)
		D3D12_RENDER_TARGET_BLEND_DESC& blend = d.BlendState.RenderTarget[0];
		blend.BlendEnable = TRUE;
		blend.SrcBlend = D3D12_BLEND_ONE;
		blend.DestBlend = D3D12_BLEND_ONE;
		blend.BlendOp = D3D12_BLEND_OP_ADD;
		blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
		blend.DestBlendAlpha = D3D12_BLEND_ONE;
		blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	}
	if (sky) {
		d.DepthStencilState.DepthEnable = TRUE;
		d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; // only where nothing was drawn (depth 0)
		d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	}
	d.NumRenderTargets = 1;
	d.RTVFormats[0] = sky ? kSceneColorFormat : format;
	d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
	if (FAILED(hr)) {
		Log::Get().Write(Severity::Error, "SceneRenderer", "E_PIPELINE_STATE", "A full screen pipeline state could not be created", {},
			{{"hresult", HrToString(hr)}, {"rtvFormat", int(format)}, {"kind", int(kind)}});
		pso.Reset();
	}
	fullscreenPipelines_[key] = pso;
	return pso.Get();
}

// ------------------------------------------------------------------------------------------------ frame preparation

void SceneRenderer::BuildDrawLists(const SceneFrame& scene, uint64_t frame) {
	SceneStore& store = SceneStore::Get();
	opaque_.clear();
	decals_.clear();
	transparent_.clear();
	casters_.clear();
	instanceData_.clear();
	materialData_.clear();
	instanceCount_ = 0;

	// material 0: the diagnostic material for a sub mesh whose material is missing
	MaterialData missing = {};
	missing.baseColor[0] = missing.baseColor[1] = missing.baseColor[2] = missing.baseColor[3] = 1;
	missing.params[0] = 1;
	missing.params[2] = -1;
	missing.params2[2] = float(kAeMaterialUnsupported);
	materialData_.resize(sizeof(MaterialData));
	memcpy(materialData_.data(), &missing, sizeof(missing));
	materialCount_ = 1;

	const Mat4 view = Mat4::From(scene.frame.view);
	const Mat4 viewProj = Mat4::From(scene.frame.proj) * view;
	Plane planes[6];
	FrustumPlanes(viewProj, planes);

	for (auto& [handle, inst] : store.Instances()) {
		if (inst.frameStamp != frame || !inst.visible) {
			continue;
		}
		const SceneMesh* mesh = store.Mesh(inst.desc.mesh);
		if (!mesh) {
			continue; // not arrived yet, or destroyed: the instance waits
		}
		const Mat4& world = inst.world;
		const Vec3 center = TransformPoint(world, {mesh->desc.boundsCenter[0], mesh->desc.boundsCenter[1], mesh->desc.boundsCenter[2]});
		const float scale = std::max({Length(Vec3{world.at(0, 0), world.at(1, 0), world.at(2, 0)}), Length(Vec3{world.at(0, 1), world.at(1, 1), world.at(2, 1)}), Length(Vec3{world.at(0, 2), world.at(1, 2), world.at(2, 2)})});
		const float radius = Length(Vec3{mesh->desc.boundsExtents[0], mesh->desc.boundsExtents[1], mesh->desc.boundsExtents[2]}) * scale;
		const bool inView = !inst.shadowOnly && SphereVisible(planes, center, radius);
		const bool castsShadow = (inst.desc.flags & kAeInstanceCastShadows) != 0;
		if (!inView && !castsShadow) {
			continue;
		}
		const bool mirrored = Determinant3x3(world) < 0;
		// instance data
		InstanceData id = {};
		memcpy(id.world, world.m, sizeof(id.world));
		const Mat4 inv = Inverse(world);
		for (int c = 0; c < 3; ++c) {
			id.normalRow0[c] = inv.at(c, 0);
			id.normalRow1[c] = inv.at(c, 1);
			id.normalRow2[c] = inv.at(c, 2);
		}
		id.normalRow0[3] = mirrored ? -1.0f : 1.0f;
		memcpy(&id.reserved[0], &inst.desc.renderingLayerMask, sizeof(uint32_t)); // read with asuint in the shader
		id.reserved[1] = inst.moved ? 1.0f : 0.0f;
		memcpy(id.prevWorld, inst.prevWorld.m, sizeof(id.prevWorld));
		const uint32_t instanceIndex = instanceCount_++;
		instanceData_.resize(size_t(instanceCount_) * sizeof(InstanceData));
		memcpy(instanceData_.data() + size_t(instanceIndex) * sizeof(InstanceData), &id, sizeof(id));

		const Vec3 viewPos = TransformPoint(view, center);
		const uint32_t subMeshes = std::min<uint32_t>(uint32_t(mesh->subMeshes.size()), uint32_t(inst.materials.size()));
		for (uint32_t s = 0; s < subMeshes; ++s) {
			if (mesh->subMeshes[s].indexCount == 0) {
				continue;
			}
			SceneMaterial* material = inst.materials[s] ? store.Material(inst.materials[s]) : nullptr;
			if (material && material->desc.materialClass == kAeMaterialSkip) {
				continue;
			}
			uint32_t materialIndex = 0;
			uint32_t flags = 0;
			if (material) {
				if (material->frameStamp != frame) {
					const AeMaterialDesc& m = material->desc;
					MaterialData md = {};
					memcpy(md.baseColor, m.baseColor, sizeof(md.baseColor));
					memcpy(md.secondaryColor, m.secondaryColor, sizeof(md.secondaryColor));
					md.emissiveMetallic[0] = m.emissive[0];
					md.emissiveMetallic[1] = m.emissive[1];
					md.emissiveMetallic[2] = m.emissive[2];
					md.emissiveMetallic[3] = m.metallic;
					md.params[0] = m.roughness;
					md.params[1] = m.normalScale;
					md.params[2] = (m.flags & kAeMaterialAlphaTest) ? m.alphaCutoff : -1.0f;
					md.params[3] = m.occlusion;
					md.params2[0] = m.clearCoat;
					md.params2[1] = m.clearCoatRoughness;
					md.params2[2] = float(m.materialClass);
					md.params2[3] = float(m.flags);
					md.uvScaleOffset[0] = m.uvScale[0];
					md.uvScaleOffset[1] = m.uvScale[1];
					md.uvScaleOffset[2] = m.uvOffset[0];
					md.uvScaleOffset[3] = m.uvOffset[1];
					md.params3[0] = float(m.maskLayout);
					md.params3[1] = m.textures[kAeSlotMask] && m.maskLayout != kAeMaskNone ? 1.0f : 0.0f;
					md.params3[2] = m.textures[kAeSlotNormal] ? 1.0f : 0.0f;
					md.params3[3] = m.textures[kAeSlotExtra] ? 1.0f : 0.0f;
					material->frameIndex = materialCount_++;
					material->frameStamp = frame;
					materialData_.resize(size_t(materialCount_) * sizeof(MaterialData));
					memcpy(materialData_.data() + size_t(material->frameIndex) * sizeof(MaterialData), &md, sizeof(md));
					for (AeHandle t : m.textures) {
						if (SceneTexture* tex = t ? store.Texture(t) : nullptr) {
							tex->lastUsedFrame = frame;
						}
					}
				}
				materialIndex = material->frameIndex;
				flags = material->desc.flags;
			}
			DrawItem item;
			item.mesh = mesh;
			item.material = material;
			item.subMesh = s;
			item.instanceIndex = instanceIndex;
			item.materialIndex = materialIndex;
			item.cull = (flags & kAeMaterialDoubleSided) ? kCullNone : mirrored ? kCullFront : kCullBack;
			item.mirrored = mirrored;
			item.depth = -viewPos.z;
			item.center = center;
			item.radius = radius;
			item.castsShadow = castsShadow && !(flags & (kAeMaterialBlend | kAeMaterialNoShadow));
			item.sortKey = (uint64_t(mesh->layoutId) << 48) | (uint64_t(item.cull) << 46) | (uint64_t((flags & kAeMaterialAlphaTest) ? 1 : 0) << 45) |
				((uint64_t(reinterpret_cast<uintptr_t>(material)) >> 4 & 0xFFFFF) << 24) | (uint64_t(reinterpret_cast<uintptr_t>(mesh)) >> 4 & 0xFFFFFF);
			if (item.castsShadow) {
				casters_.push_back(item);
			}
			if (!inView) {
				continue;
			}
			if (material && material->desc.materialClass == kAeMaterialBlobShadow) {
				// a box around the place it darkens: its far side is drawn (the camera may be inside it), without depth test
				item.cull = mirrored ? kCullBack : kCullFront;
				decals_.push_back(item);
			} else if (flags & kAeMaterialBlend) {
				transparent_.push_back(item);
			} else {
				opaque_.push_back(item);
			}
		}
	}
	// equal keys next to each other, sub mesh by sub mesh: DrawItems turns such runs into one instanced draw
	auto byKey = [](const DrawItem& a, const DrawItem& b) { return a.sortKey != b.sortKey ? a.sortKey < b.sortKey : a.subMesh < b.subMesh; };
	std::sort(opaque_.begin(), opaque_.end(), byKey);
	std::sort(casters_.begin(), casters_.end(), byKey);
	std::sort(transparent_.begin(), transparent_.end(), [](const DrawItem& a, const DrawItem& b) { return a.depth > b.depth; }); // back to front
}

void SceneRenderer::BuildCascades(const SceneFrame& scene, const SettingsData& settings) {
	cascades_.clear();
	hasSun_ = sunShadows_ = false;
	sunColor_ = {0, 0, 0};
	// the brightest directional light is the sun
	float best = 0;
	for (const AeLight& l : scene.lights) {
		if (l.type != kAeLightDirectional) {
			continue;
		}
		float lum = l.color[0] * 0.2126f + l.color[1] * 0.7152f + l.color[2] * 0.0722f;
		if (!hasSun_ || lum > best) {
			best = lum;
			hasSun_ = true;
			sunDirection_ = Normalize(Vec3{-l.direction[0], -l.direction[1], -l.direction[2]});
			sunColor_ = {l.color[0], l.color[1], l.color[2]};
			sunShadows_ = (l.flags & kAeLightCastsShadows) != 0;
			sunLayerMask_ = l.layerMask;
		}
	}
	if (!hasSun_ || !sunShadows_ || !settings.shadows || !shadowAtlas_) {
		sunShadows_ = false;
		return;
	}
	const bool low = settings.quality == "low";
	uint32_t count = low ? 2 : 3;
	float distance = std::min(scene.frame.farPlane, low ? 40.0f : settings.quality == "medium" ? 60.0f : settings.quality == "high" ? 80.0f : 120.0f);
	// The host's own shadow settings, when it sent them: its distance (scaled down on the lower presets), its number
	// of cascades and where it splits them.
	const AeFrame& hf = scene.frame;
	const bool hostShadows = hf.shadowDistance > 0.0f && hf.shadowCascades >= 1;
	if (hostShadows) {
		count = std::min<uint32_t>(hf.shadowCascades, low ? 2u : 4u);
		distance = std::min(scene.frame.farPlane, hf.shadowDistance * (low ? 0.25f : settings.quality == "medium" ? 0.5f : 1.0f));
	}
	const float nearPlane = std::max(scene.frame.nearPlane, 0.05f);
	const Mat4 proj = Mat4::From(scene.frame.proj);
	const Mat4 invView = Inverse(Mat4::From(scene.frame.view));
	const Vec3 travel = sunDirection_ * -1.0f;
	const Mat4 lightView = LookAlong({0, 0, 0}, travel, {0, 1, 0});
	const float size = float(shadowCascadeSize_);
	float sliceNear = nearPlane;
	for (uint32_t i = 0; i < count; ++i) {
		// practical split scheme: mostly logarithmic
		const float t = float(i + 1) / float(count);
		const float logSplit = nearPlane * std::pow(distance / nearPlane, t);
		const float linSplit = nearPlane + (distance - nearPlane) * t;
		float sliceFar = 0.8f * logSplit + 0.2f * linSplit;
		if (hostShadows) {
			// the host's split fractions belong to its own cascade count: the last cascade always ends at the distance
			const float fraction = (i + 1 < count && i < 3 && hf.shadowSplits[i] > 0.0f) ? hf.shadowSplits[i] : 1.0f;
			sliceFar = std::max(sliceNear + 0.5f, std::min(distance, distance * fraction));
			if (i + 1 == count) {
				sliceFar = distance;
			}
		}
		// the slice's eight corners in world space (view space: x = +-d / P00, y = +-d / P11, z = -d)
		Vec3 corners[8];
		int n = 0;
		for (float d : {sliceNear, sliceFar}) {
			for (float sx : {-1.0f, 1.0f}) {
				for (float sy : {-1.0f, 1.0f}) {
					corners[n++] = TransformPoint(invView, {sx * d / proj.at(0, 0), sy * d / proj.at(1, 1), -d});
				}
			}
		}
		Vec3 center = {0, 0, 0};
		for (const Vec3& c : corners) {
			center = center + c * (1.0f / 8.0f);
		}
		float radius = 0;
		for (const Vec3& c : corners) {
			radius = std::max(radius, Length(c - center));
		}
		radius = std::ceil(radius * 16.0f) / 16.0f; // a stable size while the camera turns
		Vec3 lc = TransformPoint(lightView, center);
		const float texel = 2.0f * radius / size;
		lc.x = std::floor(lc.x / texel) * texel; // move in whole texels: no shimmering edges
		lc.y = std::floor(lc.y / texel) * texel;
		const float depthBefore = 250.0f; // casters this far towards the sun still reach the slice
		const Mat4 ortho = OrthoRH(lc.x - radius, lc.x + radius, lc.y - radius, lc.y + radius, -lc.z - radius - depthBefore, -lc.z + radius);
		Cascade c;
		c.viewProj = ortho * lightView;
		const uint32_t col = i % 2, rowIndex = i / 2;
		Mat4 toAtlas = Mat4::Identity();
		toAtlas.at(0, 0) = 0.25f;
		toAtlas.at(1, 1) = -0.25f;
		toAtlas.at(0, 3) = 0.25f + 0.5f * float(col);
		toAtlas.at(1, 3) = 0.25f + 0.5f * float(rowIndex);
		c.atlasMatrix = toAtlas * c.viewProj;
		c.splitFar = sliceFar;
		c.texelWorld = texel;
		c.center = center;
		c.radius = radius;
		c.viewport = {float(col) * size, float(rowIndex) * size, size, size, 0.0f, 1.0f};
		cascades_.push_back(c);
		sliceNear = sliceFar;
	}
}

void SceneRenderer::BuildClusters(const SceneFrame& scene) {
	const uint32_t clusters = kClusterX * kClusterY * kClusterZ;
	clusterGrid_.assign(size_t(clusters) * 2, 0);
	lightIndices_.clear();
	lightData_.clear();
	punctualCount_ = 0;
	struct ViewLight { Vec3 pos; float range; };
	std::vector<ViewLight> viewLights;
	const Mat4 view = Mat4::From(scene.frame.view);
	for (const AeLight& l : scene.lights) {
		if (l.type == kAeLightDirectional || l.range <= 0) {
			continue;
		}
		LightData d = {};
		memcpy(d.positionRange, l.position, sizeof(l.position));
		d.positionRange[3] = l.range;
		memcpy(d.directionOuter, l.direction, sizeof(l.direction));
		d.directionOuter[3] = l.spotOuterCos;
		memcpy(d.colorInner, l.color, sizeof(l.color));
		d.colorInner[3] = l.spotInnerCos;
		d.typeFlags[0] = float(l.type);
		memcpy(&d.typeFlags[1], &l.layerMask, sizeof(uint32_t)); // rendering layers the light reaches (asuint)
		lightData_.resize(size_t(punctualCount_ + 1) * sizeof(LightData));
		memcpy(lightData_.data() + size_t(punctualCount_) * sizeof(LightData), &d, sizeof(d));
		viewLights.push_back({TransformPoint(view, {l.position[0], l.position[1], l.position[2]}), l.range});
		++punctualCount_;
	}
	if (punctualCount_ == 0) {
		lightData_.resize(sizeof(LightData)); // a root SRV needs something to point at
		lightIndices_.push_back(0);
		return;
	}
	const Mat4 proj = Mat4::From(scene.frame.proj);
	const float zNear = std::max(scene.frame.nearPlane, 0.05f);
	const float zFar = std::min(std::max(scene.frame.farPlane, zNear * 2.0f), 500.0f);
	const float ratio = zFar / zNear;
	std::vector<std::vector<uint16_t>> lists(clusters);
	for (uint32_t li = 0; li < punctualCount_; ++li) {
		const ViewLight& l = viewLights[li];
		const float depth = -l.pos.z;
		if (depth + l.range < zNear || depth - l.range > zFar) {
			continue;
		}
		const float d0 = std::max(depth - l.range, zNear), d1 = std::min(depth + l.range, zFar);
		const uint32_t z0 = std::min(uint32_t(std::log(d0 / zNear) / std::log(ratio) * kClusterZ), kClusterZ - 1);
		const uint32_t z1 = std::min(uint32_t(std::log(d1 / zNear) / std::log(ratio) * kClusterZ), kClusterZ - 1);
		for (uint32_t z = z0; z <= z1; ++z) {
			const float sliceNear = zNear * std::pow(ratio, float(z) / kClusterZ), sliceFar = zNear * std::pow(ratio, float(z + 1) / kClusterZ);
			for (uint32_t y = 0; y < kClusterY; ++y) {
				// cluster row 0 is the top of the screen (ndc y = +1)
				const float ny0 = 1.0f - 2.0f * float(y + 1) / kClusterY, ny1 = 1.0f - 2.0f * float(y) / kClusterY;
				const float yMin = std::min(ny0 * sliceNear, ny0 * sliceFar) / proj.at(1, 1), yMax = std::max(ny1 * sliceNear, ny1 * sliceFar) / proj.at(1, 1);
				for (uint32_t x = 0; x < kClusterX; ++x) {
					const float nx0 = -1.0f + 2.0f * float(x) / kClusterX, nx1 = -1.0f + 2.0f * float(x + 1) / kClusterX;
					const float xMin = std::min(nx0 * sliceNear, nx0 * sliceFar) / proj.at(0, 0), xMax = std::max(nx1 * sliceNear, nx1 * sliceFar) / proj.at(0, 0);
					// squared distance from the light to the cluster's view space box
					const float dx = std::max({xMin - l.pos.x, 0.0f, l.pos.x - xMax});
					const float dy = std::max({yMin - l.pos.y, 0.0f, l.pos.y - yMax});
					const float dz = std::max({sliceNear - depth, 0.0f, depth - sliceFar});
					if (dx * dx + dy * dy + dz * dz <= l.range * l.range) {
						auto& list = lists[(z * kClusterY + y) * kClusterX + x];
						if (list.size() < kMaxLightsPerCluster) {
							list.push_back(uint16_t(li));
						}
					}
				}
			}
		}
	}
	for (uint32_t c = 0; c < clusters; ++c) {
		clusterGrid_[size_t(c) * 2] = uint32_t(lightIndices_.size());
		clusterGrid_[size_t(c) * 2 + 1] = uint32_t(lists[c].size());
		for (uint16_t li : lists[c]) {
			lightIndices_.push_back(li);
		}
	}
	if (lightIndices_.empty()) {
		lightIndices_.push_back(0);
	}
}

bool SceneRenderer::UploadFrameData(const SceneFrame& scene, const Target& target, const SettingsData& settings) {
	GpuResources& gpu = GpuResources::Get();
	SceneStore& store = SceneStore::Get();
	const AeFrame& f = scene.frame;
	FrameConstants fc = {};
	const Mat4 view = Mat4::From(f.view);
	Mat4 proj = Mat4::From(f.proj);
	const Mat4 cleanViewProj = proj * view;
	// ---- post-processing as the host has it (frame), where Aether's own switches allow it
	// (Aether's own switch: which antialiasing the game's graphics menu is set to is not looked at)
	taa_ = settings.antialiasing;
	// (the host mixes the picture with its bloom pyramid by the control value itself: at 1 the picture is the pyramid)
	bloomMix_ = settings.bloom && f.bloomIntensity > 0 ? std::min(f.bloomIntensity * settings.bloomStrength, 1.0f) : 0.0f;
	bloomScatter_ = 0.05f + 0.9f * std::min(std::max(f.bloomScatter, 0.0f), 1.0f);
	motionBlur_ = settings.motionBlur ? std::max(f.motionBlurIntensity * settings.motionBlurStrength, 0.0f) : 0.0f;
	if (f.cameraId != historyCamera_) {
		historyValid_ = false; // another camera: nothing of the frame before applies
		historyCamera_ = f.cameraId;
	}
	if (taa_) {
		// a different sub pixel offset every frame (Halton 2, 3 over 8 frames): the frames average to an antialiased picture
		auto halton = [](uint32_t i, uint32_t base) { float r = 0, fr = 1; while (i > 0) { fr /= float(base); r += fr * float(i % base); i /= base; } return r; };
		const uint32_t phase = uint32_t(frame_ % 8) + 1;
		const float jx = (halton(phase, 2) - 0.5f) * 2.0f / float(target.width), jy = (halton(phase, 3) - 0.5f) * 2.0f / float(target.height);
		proj.at(0, 2) += jx * proj.at(3, 2);
		proj.at(1, 2) += jy * proj.at(3, 2);
		fc.jitter[0] = jx;
		fc.jitter[1] = jy;
	}
	fc.jitter[2] = historyValid_ ? 1.0f : 0.0f;
	// screen space reflections read the picture of the frame before (the temporal history): none without it
	ssr_ = settings.ssr && f.ssrEnabled != 0 && taa_;
	fc.ssrParams[0] = ssr_ && historyValid_ ? 1.0f : 0.0f;
	fc.ssrParams[1] = f.ssrMinSmoothness;
	fc.ssrParams[2] = std::max(f.ssrSmoothnessFadeStart, f.ssrMinSmoothness);
	fc.ssrParams[3] = f.ssrScreenFadeDistance;
	fc.qualityParams[0] = settings.shadowSoftness;
	memcpy(fc.prevViewProj, prevViewProj_.m, 64);
	prevViewProj_ = cleanViewProj;
	fc.postParams[0] = bloomMix_;
	fc.postParams[1] = bloomScatter_;
	fc.postParams[2] = motionBlur_;
	fc.postParams[3] = taa_ ? 1.0f : 0.0f;
	const Mat4 viewProj = proj * view;
	const Mat4 invViewProj = Inverse(viewProj);
	memcpy(fc.view, view.m, 64);
	memcpy(fc.proj, proj.m, 64);
	memcpy(fc.viewProj, viewProj.m, 64);
	memcpy(fc.invViewProj, invViewProj.m, 64);
	for (size_t i = 0; i < cascades_.size() && i < 4; ++i) {
		memcpy(fc.shadowViewProj[i], cascades_[i].viewProj.m, 64);
		memcpy(fc.shadowMatrix[i], cascades_[i].atlasMatrix.m, 64);
		fc.cascadeSplits[i] = cascades_[i].splitFar;
	}
	fc.shadowParams[0] = float(cascades_.size());
	fc.shadowParams[1] = shadowCascadeSize_ ? 1.0f / float(shadowCascadeSize_ * 2) : 0.0f;
	fc.shadowParams[2] = 0.0004f;
	fc.shadowParams[3] = cascades_.empty() ? 0.0f : cascades_[0].texelWorld * 1.5f;
	for (size_t i = 0; i < cascades_.size() && i < 4; ++i) {
		fc.cascadeTexel[i] = cascades_[i].texelWorld;
	}
	aoActive_ = settings.ssao && f.aoIntensity > 0.0f && f.aoRadius > 0.0f;
	fc.aoParams[0] = f.aoIntensity;
	fc.aoParams[1] = f.aoRadius;
	fc.aoParams[2] = f.aoDirectStrength;
	fc.aoParams[3] = aoActive_ ? 1.0f : 0.0f;
	memcpy(fc.smh[0], f.smhShadows, sizeof(f.smhShadows));
	memcpy(fc.smh[1], f.smhMidtones, sizeof(f.smhMidtones));
	memcpy(fc.smh[2], f.smhHighlights, sizeof(f.smhHighlights));
	memcpy(fc.smhLimits, f.smhLimits, sizeof(f.smhLimits));
	fc.cameraPos[0] = f.cameraPosition[0];
	fc.cameraPos[1] = f.cameraPosition[1];
	fc.cameraPos[2] = f.cameraPosition[2];
	fc.cameraPos[3] = f.time;
	fc.screenSize[0] = float(target.width);
	fc.screenSize[1] = float(target.height);
	fc.screenSize[2] = 1.0f / float(target.width);
	fc.screenSize[3] = 1.0f / float(target.height);
	const float zNear = std::max(f.nearPlane, 0.05f), zFar = std::min(std::max(f.farPlane, zNear * 2.0f), 500.0f);
	fc.clusterCounts[0] = punctualCount_ ? float(kClusterX) : 0.0f;
	fc.clusterCounts[1] = float(kClusterY);
	fc.clusterCounts[2] = float(kClusterZ);
	fc.clusterZ[0] = zNear;
	fc.clusterZ[1] = zFar;
	fc.clusterZ[2] = float(kClusterZ) / std::log(zFar / zNear);
	fc.sunDirection[0] = sunDirection_.x;
	fc.sunDirection[1] = sunDirection_.y;
	fc.sunDirection[2] = sunDirection_.z;
	fc.sunDirection[3] = hasSun_ ? 1.0f : 0.0f;
	memcpy(&fc.clusterCounts[3], &sunLayerMask_, sizeof(uint32_t)); // g_ClusterCounts.w: the sun's rendering layers (asuint)
	fc.sunColor[0] = sunColor_.x;
	fc.sunColor[1] = sunColor_.y;
	fc.sunColor[2] = sunColor_.z;
	fc.sunColor[3] = sunShadows_ && !cascades_.empty() ? 1.0f : 0.0f;
	memcpy(fc.sh, f.ambientSH, sizeof(fc.sh));
	SceneTexture* sky = settings.sky && f.skyTexture ? store.Texture(f.skyTexture) : nullptr;
	fc.exposure[0] = f.exposure > 0 ? f.exposure : 1.0f;
	// exposure source (y): 0 the constant in x, 2 Aether's own metering of this frame (the host exposes automatically)
	fc.exposure[1] = 0.0f;
	ownExposure_ = false;
	if (f.exposureMode == kAeExposureFixed) {
		fc.exposure[0] = 1.0f / (1.2f * std::pow(2.0f, f.exposureFixedEV - f.exposureCompensation));
	} else if (f.exposureMode == kAeExposureAutomatic) {
		ownExposure_ = true;
		fc.exposure[1] = 2.0f;
		fc.exposureParams[0] = f.exposureMinEV;
		fc.exposureParams[1] = f.exposureMaxEV;
		fc.exposureParams[2] = f.exposureCompensation;
		// how far this frame's metering moves the exposure towards what it measured (1: all the way, no value to start
		// from). The metering of one frame alone follows every highlight that crosses the screen, which shows as flicker.
		const auto now = std::chrono::steady_clock::now();
		const float dt = std::clamp(std::chrono::duration<float>(now - exposureTime_).count(), 0.0f, 0.25f);
		exposureTime_ = now;
		fc.exposureParams[3] = exposureValid_ ? 1.0f - std::exp(-dt * kExposureAdaptationRate) : 1.0f;
	}
	fc.exposure[2] = std::pow(2.0f, settings.exposureEV + f.postExposure);
	// tone mapping: 0 none, 1 ACES, 2 neutral, 3 the host's custom curve
	float tone = settings.tonemap == "aces" ? 1.0f : settings.tonemap == "neutral" ? 2.0f : 0.0f;
	if (settings.tonemap == "host") {
		switch (f.tonemapMode) {
		case kAeTonemapNone: tone = 0.0f; break;
		case kAeTonemapNeutral: tone = 2.0f; break;
		case kAeTonemapCustom: tone = 3.0f; break;
		default: tone = 1.0f; break; // ACES, an external LUT (not reproduced) or nothing known
		}
	}
	if (tone == 3.0f) {
		HableCurve(f.tonemapCurve, fc.toneCurve, fc.toneSegA, fc.toneSegB);
	}
	fc.exposure[3] = tone;
	const auto& views = DebugViewNames();
	fc.misc[0] = float(std::find(views.begin(), views.end(), settings.debugView) - views.begin());
	fc.misc[1] = sky && sky->srv != UINT32_MAX ? 1.0f : 0.0f;
	frameSky_ = fc.misc[1] > 0.5f;
	fc.misc[2] = sky ? float(std::max<uint32_t>(sky->desc.mipCount, 1)) : 1.0f;
	fc.misc[3] = f.ambientIntensity;
	fc.skyParams[0] = f.skyIntensity > 0 ? f.skyIntensity : 1.0f;
	fc.skyParams[1] = std::cos(f.skyRotation);
	fc.skyParams[2] = std::sin(f.skyRotation);
	const bool fogFromSky = (f.fogFlags & kAeFogColorFromSky) != 0;
	memcpy(fc.fog, fogFromSky ? f.fogTint : f.fogColor, sizeof(f.fogColor));
	fc.fog[3] = f.fogDensity;
	fc.fogHeight[0] = f.fogBaseHeight;
	// HDRP's height fog: the layer's depth times 0.144765 is the height over which the density falls to 1 / e
	fc.fogHeight[1] = 1.0f / (std::max(0.01f, f.fogMaxHeight - f.fogBaseHeight) * 0.144765f);
	fc.fogHeight[2] = f.fogMaxDistance;
	fc.fogHeight[3] = fogFromSky ? 1.0f : 0.0f;

	auto upload = [&](const void* data, size_t bytes, size_t align) -> D3D12_GPU_VIRTUAL_ADDRESS {
		UploadAllocation a = gpu.Upload(std::max<size_t>(bytes, 16), align);
		if (!a.cpu) {
			return 0;
		}
		memcpy(a.cpu, data, bytes);
		return a.gpu;
	};
	frameCb_ = upload(&fc, sizeof(fc), 256);
	if (instanceData_.empty()) {
		instanceData_.resize(sizeof(InstanceData));
	}
	instancesVa_ = upload(instanceData_.data(), instanceData_.size(), 16);
	// ---- particles: a batch that covers what is behind it is drawn back to front
	particleBatches_.clear();
	particleData_.clear();
	particlesVa_ = 0;
	if (settings.particles && !scene.particles.empty()) {
		particleData_ = scene.particles;
		particleBatches_ = scene.particleBatches;
		const float cx = f.cameraPosition[0], cy = f.cameraPosition[1], cz = f.cameraPosition[2];
		for (const AeParticleBatch& b : particleBatches_) {
			if ((b.flags & kAeParticleAdditive) == 0 && b.count > 1) {
				std::sort(particleData_.begin() + b.first, particleData_.begin() + b.first + b.count, [&](const AeParticle& x, const AeParticle& y) {
					const float dx = (x.position[0] - cx) * (x.position[0] - cx) + (x.position[1] - cy) * (x.position[1] - cy) + (x.position[2] - cz) * (x.position[2] - cz);
					const float dy = (y.position[0] - cx) * (y.position[0] - cx) + (y.position[1] - cy) * (y.position[1] - cy) + (y.position[2] - cz) * (y.position[2] - cz);
					return dx > dy;
				});
			}
		}
		particlesVa_ = upload(particleData_.data(), particleData_.size() * sizeof(AeParticle), 16);
	}
	materialsVa_ = upload(materialData_.data(), materialData_.size(), 16);
	lightsVa_ = upload(lightData_.data(), lightData_.size(), 16);
	gridVa_ = upload(clusterGrid_.data(), clusterGrid_.size() * sizeof(uint32_t), 16);
	indicesVa_ = upload(lightIndices_.data(), lightIndices_.size() * sizeof(uint32_t), 16);
	// the instance map is written while the passes are recorded: room for every list once per pass that draws it
	instanceMapCapacity_ = uint32_t(opaque_.size() * 2 + transparent_.size() + decals_.size() + casters_.size() * 4 + 4);
	const UploadAllocation map = gpu.Upload(size_t(instanceMapCapacity_) * sizeof(uint32_t), 16);
	instanceMap_ = static_cast<uint32_t*>(map.cpu);
	instanceMapVa_ = map.gpu;
	instanceMapUsed_ = itemsDrawn_ = itemsInstanced_ = 0;
	if (!frameCb_ || !instancesVa_ || !materialsVa_ || !lightsVa_ || !gridVa_ || !indicesVa_ || !instanceMap_) {
		return false;
	}

	// this frame's global table: shadow atlas, sky, exposure. A fresh table every frame: the previous frames' lists
	// may still read theirs.
	// ---- reflection probes: the nearest ones whose cube map the store has
	probesTable_ = gpu.AllocSrv(kMaxProbes);
	probesInFrame_ = 0;
	if (probesTable_ != UINT32_MAX) {
		struct Candidate { const AeProbe* probe; SceneTexture* texture; float distance; };
		std::vector<Candidate> candidates;
		for (const AeProbe& p : scene.probes) {
			SceneTexture* texture = settings.sky ? store.Texture(p.texture) : nullptr;
			if (!texture || texture->srv == UINT32_MAX || texture->desc.dimension != kAeTextureCube) {
				continue;
			}
			const Vec3 d = Vec3{p.capturePosition[0], p.capturePosition[1], p.capturePosition[2]} - Vec3{f.cameraPosition[0], f.cameraPosition[1], f.cameraPosition[2]};
			candidates.push_back({&p, texture, Length(d)});
		}
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
		for (uint32_t i = 0; i < kMaxProbes; ++i) {
			if (i < candidates.size()) {
				const AeProbe& p = *candidates[i].probe;
				memcpy(fc.probeRows[i], p.worldToLocal, sizeof(p.worldToLocal));
				memcpy(fc.probeExtents[i], p.extents, sizeof(p.extents));
				fc.probeExtents[i][3] = p.blendDistance;
				memcpy(fc.probeCapture[i], p.capturePosition, sizeof(p.capturePosition));
				fc.probeCapture[i][3] = p.intensity;
				fc.probeInfo[i][0] = float(p.flags);
				fc.probeInfo[i][1] = float(std::max<uint32_t>(candidates[i].texture->desc.mipCount, 1));
				gpu.CopySrv(probesTable_ + i, candidates[i].texture->srv);
				++probesInFrame_;
			} else {
				gpu.CopySrv(probesTable_ + i, nullCubeSrv_);
			}
		}
		fc.probeCount[0] = float(probesInFrame_);
	}
	globalsTable_ = gpu.AllocSrv(kGlobalsTableSize);
	postTable_ = gpu.AllocSrv(kPostTableSize * kPostTables);
	if (globalsTable_ == UINT32_MAX || postTable_ == UINT32_MAX) {
		return false;
	}
	gpu.CopySrv(globalsTable_, shadowAtlas_ ? shadowAtlas_->srv : nullTextureSrv_);
	gpu.CopySrv(globalsTable_ + 1, fc.misc[1] > 0.5f ? sky->srv : nullCubeSrv_);
	gpu.CopySrv(globalsTable_ + 2, irradiance_ ? irradiance_->srv : nullTextureSrv_);
	postNext_ = 4;
	gpu.CopySrv(globalsTable_ + 3, white_->srv); // replaced by the ambient occlusion texture when that pass runs
	for (uint32_t i = 0; i < kPostTableSize * kPostTables; ++i) {
		gpu.CopySrv(postTable_ + i, nullTextureSrv_);
	}
	if (sky) {
		sky->lastUsedFrame = frame_;
	}
	return true;
}

void SceneRenderer::BindCommon(ID3D12GraphicsCommandList* list, bool globals) {
	GpuResources& gpu = GpuResources::Get();
	ID3D12DescriptorHeap* heaps[] = {gpu.SrvHeap()};
	list->SetDescriptorHeaps(1, heaps);
	list->SetGraphicsRootSignature(root_.Get());
	list->SetGraphicsRootConstantBufferView(0, frameCb_);
	list->SetGraphicsRootShaderResourceView(2, instancesVa_);
	list->SetGraphicsRootShaderResourceView(3, materialsVa_);
	list->SetGraphicsRootShaderResourceView(4, lightsVa_);
	list->SetGraphicsRootShaderResourceView(5, gridVa_);
	list->SetGraphicsRootShaderResourceView(6, indicesVa_);
	list->SetGraphicsRootShaderResourceView(11, instanceMapVa_);
	if (globals) {
		list->SetGraphicsRootDescriptorTable(8, gpu.SrvGpu(globalsTable_));
		list->SetGraphicsRootDescriptorTable(10, gpu.SrvGpu(probesTable_));
	}
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void SceneRenderer::DrawItems(RenderGraph::PassContext& ctx, const std::vector<DrawItem>& items, PassKind opaqueKind, PassKind alphaKind, int cascade) {
	GpuResources& gpu = GpuResources::Get();
	ID3D12GraphicsCommandList* list = ctx.list;
	ID3D12PipelineState* boundPso = nullptr;
	const SceneMesh* boundMesh = nullptr;
	uint32_t boundTable = UINT32_MAX;
	// a run of items that differ in nothing but the instance is one draw
	struct Batch { const DrawItem* first = nullptr; ID3D12PipelineState* pso = nullptr; uint32_t table = 0, start = 0, count = 0; } batch;
	auto flush = [&]() {
		if (!batch.first) {
			return;
		}
		const DrawItem& item = *batch.first;
		if (batch.pso != boundPso) {
			list->SetPipelineState(batch.pso);
			boundPso = batch.pso;
		}
		if (item.mesh != boundMesh) {
			const SceneMesh& mesh = *item.mesh;
			D3D12_VERTEX_BUFFER_VIEW views[AE_MAX_STREAMS];
			for (uint32_t s = 0; s < mesh.desc.streamCount; ++s) {
				views[s] = {mesh.vertexBuffer[s]->GetGPUVirtualAddress(), mesh.desc.vertexCount * mesh.desc.streamStride[s], mesh.desc.streamStride[s]};
			}
			list->IASetVertexBuffers(0, mesh.desc.streamCount, views);
			list->IASetVertexBuffers(kDefaultsSlot, 1, &defaultsView_);
			// (sizes were checked against the buffers when the mesh was registered: SceneStore::Apply)
			D3D12_INDEX_BUFFER_VIEW ib = {mesh.indexBuffer->GetGPUVirtualAddress(), mesh.desc.indexCount * mesh.desc.indexStride, mesh.desc.indexStride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
			list->IASetIndexBuffer(&ib);
			boundMesh = item.mesh;
		}
		if (batch.table != boundTable && batch.table != UINT32_MAX) {
			list->SetGraphicsRootDescriptorTable(7, gpu.SrvGpu(batch.table));
			boundTable = batch.table;
		}
		const uint32_t constants[4] = {batch.start, item.materialIndex, uint32_t(std::max(cascade, 0)), (item.mirrored ? 1u : 0u) | (opaqueKind == kForward ? 0u : 2u)}; // bit 1: no screen space occlusion (blended passes)
		list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
		const AeSubMesh& sm = item.mesh->subMeshes[item.subMesh];
		list->DrawIndexedInstanced(sm.indexCount, batch.count, sm.indexStart, INT(sm.baseVertex), 0);
		ctx.drawCalls++;
		ctx.triangles += sm.indexCount / 3 * batch.count;
		itemsDrawn_ += batch.count;
		itemsInstanced_ += batch.count > 1 ? batch.count : 0;
		batch.first = nullptr;
	};
	for (const DrawItem& item : items) {
		if (cascade >= 0) {
			// only what can reach this cascade's box: a sphere against the cascade's bounding sphere, stretched towards the sun
			const Cascade& c = cascades_[size_t(cascade)];
			const Vec3 d = item.center - c.center;
			const float along = Dot(d, sunDirection_);
			const Vec3 lateral = d - sunDirection_ * along;
			if (Length(lateral) > c.radius + item.radius || along < -(c.radius + item.radius)) {
				continue;
			}
		}
		const uint32_t flags = item.material ? item.material->desc.flags : 0;
		PassKind kind = (flags & kAeMaterialAlphaTest) ? alphaKind : opaqueKind;
		if (opaqueKind == kForwardBlend && (flags & kAeMaterialAdditive)) {
			kind = kForwardAdditive;
		}
		ID3D12PipelineState* pso = Pipeline(item.mesh->layoutId, kind, item.cull);
		if (!pso) {
			continue;
		}
		if (instanceMapUsed_ >= instanceMapCapacity_) {
			break; // cannot happen: the map has room for every list in every pass
		}
		// a missing material, or one whose table could not be made yet, samples the default textures
		const uint32_t table = item.material && item.material->table != UINT32_MAX ? item.material->table : fallbackTable_;
		const bool joins = batch.first && batch.pso == pso && batch.table == table && batch.first->mesh == item.mesh && batch.first->subMesh == item.subMesh &&
			batch.first->materialIndex == item.materialIndex && batch.first->mirrored == item.mirrored;
		if (joins) {
			++batch.count;
		} else {
			flush();
			batch.first = &item;
			batch.pso = pso;
			batch.table = table;
			batch.start = instanceMapUsed_;
			batch.count = 1;
		}
		instanceMap_[instanceMapUsed_++] = item.instanceIndex;
	}
	flush();
}

bool SceneRenderer::Fullscreen(RenderGraph::PassContext& ctx, FullscreenKind kind, GpuTexture* output, GpuTexture* in0, GpuTexture* in1, GpuTexture* in2, const float* params, GpuTexture* in3) {
	GpuResources& gpu = GpuResources::Get();
	ID3D12PipelineState* pso = output ? FullscreenPipeline(kind, output->desc.targetFormat) : nullptr;
	if (!pso || postNext_ >= kPostTables) {
		ctx.note = "no pipeline";
		return false;
	}
	const uint32_t table = postTable_ + kPostTableSize * postNext_++;
	GpuTexture* inputs[kPostTableSize] = {in0, in1, in2, in3};
	for (uint32_t i = 0; i < kPostTableSize; ++i) {
		if (inputs[i]) {
			gpu.CopySrv(table + i, inputs[i]->srv);
		}
	}
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.RtvCpu(output->target);
	const D3D12_VIEWPORT viewport = {0, 0, float(output->desc.width), float(output->desc.height), 0, 1};
	const D3D12_RECT scissor = {0, 0, LONG(output->desc.width), LONG(output->desc.height)};
	ctx.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	ctx.list->RSSetViewports(1, &viewport);
	ctx.list->RSSetScissorRects(1, &scissor);
	BindCommon(ctx.list, true);
	ctx.list->SetGraphicsRootDescriptorTable(9, gpu.SrvGpu(table));
	// the draw constants carry the pass's own four numbers (g_PassParams); z and w default to 1 / the output's size
	float constants[4] = {0, 0, 1.0f / float(output->desc.width), 1.0f / float(output->desc.height)};
	if (params) {
		memcpy(constants, params, sizeof(constants));
	}
	ctx.list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
	ctx.list->SetPipelineState(pso);
	ctx.list->DrawInstanced(3, 1, 0, 0);
	ctx.drawCalls = 1;
	ctx.triangles = 1;
	return true;
}

// ------------------------------------------------------------------------------------------------ the frame

bool SceneRenderer::Render(const HostGfx& host, ID3D12GraphicsCommandList* list, const Target& target, bool draw, UINT64 retireFence, uint64_t frame, const SettingsData& settings) {
	GpuResources& gpu = GpuResources::Get();
	SceneStore& store = SceneStore::Get();
	frame_ = frame;
	retireFence_ = retireFence;
	const bool objects = EnsureObjects(host, frame);
	if (!gpu.Ready()) {
		return false;
	}
	const UINT64 completed = host.completedFenceValue();
	gpu.Collect(completed, frame);
	timer_.Collect(completed, [this](uint64_t, const std::vector<double>& ms) {
		std::vector<PassTiming> timings;
		for (size_t i = 0; i < timedPasses_.size(); ++i) {
			PassTiming t;
			t.name = timedPasses_[i];
			const uint32_t b = passStampBegin_[i], e = passStampEnd_[i];
			t.executed = b != UINT32_MAX && e != UINT32_MAX && e < ms.size();
			t.gpuMs = t.executed ? ms[e] - ms[b] : -1.0;
			timings.push_back(t);
		}
		Profiler::Get().SceneGpuResult(timings, ms.empty() ? 0.0 : ms.back());
	});

	// the scene's pending changes are applied whether or not anything is drawn: handles must not pile up
	const SceneFrame& scene = store.Apply(retireFence, frame);
	if ((frame & 31) == 0) {
		store.PublishStats(frame);
	}
	if (!draw || !objects || !scene.valid || !target.backBuffer || target.width == 0 || target.height == 0) {
		return false;
	}
	if (!gpu.BeginFrame(completed, frame)) {
		return false; // no free frame arena: the host fence is not advancing
	}
	UploadDefaults(list);
	store.UpdateMaterialTables(retireFence);

	const uint32_t cascadeSize = settings.quality == "low" ? 1024u : settings.quality == "medium" ? 1536u : settings.quality == "high" ? 2048u : 3072u;
	if (settings.shadows) {
		EnsureShadowAtlas(cascadeSize, frame, retireFence);
	}
	BuildDrawLists(scene, frame);
	BuildCascades(scene, settings);
	BuildClusters(scene);
	if (!UploadFrameData(scene, target, settings)) {
		return false;
	}

	// ---- the frame as a graph
	graph_.Begin(frame);
	GpuTextureDesc colorDesc;
	colorDesc.width = target.width;
	colorDesc.height = target.height;
	colorDesc.format = colorDesc.targetFormat = kSceneColorFormat;
	colorDesc.renderTarget = true;
	GpuTextureDesc depthDesc;
	depthDesc.width = target.width;
	depthDesc.height = target.height;
	depthDesc.format = DXGI_FORMAT_R32_TYPELESS;
	depthDesc.srvFormat = DXGI_FORMAT_R32_FLOAT;
	depthDesc.targetFormat = DXGI_FORMAT_D32_FLOAT;
	depthDesc.depthStencil = true;
	depthDesc.clearDepth = 0.0f;
	const RenderGraph::ResourceId color = graph_.CreateTransient("SceneColor", colorDesc, ResourceCategory::RenderTarget);
	const RenderGraph::ResourceId depth = graph_.CreateTransient("SceneDepth", depthDesc, ResourceCategory::DepthBuffer);
	GpuTextureDesc surfaceDesc = colorDesc;
	surfaceDesc.format = surfaceDesc.targetFormat = kSurfaceFormat;
	const RenderGraph::ResourceId surface = graph_.CreateTransient("SceneSurface", surfaceDesc, ResourceCategory::RenderTarget);
	GpuTextureDesc velocityDesc = colorDesc;
	velocityDesc.format = velocityDesc.targetFormat = kVelocityFormat;
	const RenderGraph::ResourceId velocity = graph_.CreateTransient("SceneVelocity", velocityDesc, ResourceCategory::RenderTarget);
	const RenderGraph::ResourceId shadow = shadowAtlas_ ? graph_.Import(shadowAtlas_) : RenderGraph::kInvalid;
	const RenderGraph::ResourceId backBuffer = graph_.ImportExternal("BackBuffer", target.backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
	const bool shadows = !cascades_.empty();
	const D3D12_VIEWPORT viewport = {0, 0, float(target.width), float(target.height), 0, 1};
	const D3D12_RECT scissor = {0, 0, LONG(target.width), LONG(target.height)};
	const bool compare = settings.renderMode == "compare";

	graph_.AddPass("ShadowCascades", shadows, [&](RenderGraph::Builder& b) { b.Write(shadow, D3D12_RESOURCE_STATE_DEPTH_WRITE); }, [&, shadow](RenderGraph::PassContext& ctx) {
		GpuTexture* atlas = ctx.Texture(shadow);
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = gpu.DsvCpu(atlas->target);
		ctx.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		ctx.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		BindCommon(ctx.list, false);
		for (size_t i = 0; i < cascades_.size(); ++i) {
			const D3D12_VIEWPORT& vp = cascades_[i].viewport;
			const D3D12_RECT rect = {LONG(vp.TopLeftX), LONG(vp.TopLeftY), LONG(vp.TopLeftX + vp.Width), LONG(vp.TopLeftY + vp.Height)};
			ctx.list->RSSetViewports(1, &vp);
			ctx.list->RSSetScissorRects(1, &rect);
			DrawItems(ctx, casters_, kShadow, kShadowAlpha, int(i));
		}
	});
	graph_.AddPass("DepthPrepass", true, [&](RenderGraph::Builder& b) { b.Write(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE); }, [&, depth](RenderGraph::PassContext& ctx) {
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = gpu.DsvCpu(ctx.Texture(depth)->target);
		ctx.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		ctx.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
		ctx.list->RSSetViewports(1, &viewport);
		ctx.list->RSSetScissorRects(1, &scissor);
		BindCommon(ctx.list, false);
		DrawItems(ctx, opaque_, kDepth, kDepthAlpha, -1);
	});
	// ---- the sky's diffuse light: the sky cube convolved with the cosine lobe, per direction (a small octahedral map)
	if (!irradiance_) {
		GpuTextureDesc d;
		d.width = d.height = kIrradianceSize;
		d.format = d.targetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		d.renderTarget = true;
		irradiance_ = gpu.CreateTexture(d, "Scene.SkyIrradiance", ResourceCategory::RenderTarget, Lifetime::Persistent, frame);
	}
	const bool skyLight = irradiance_ && frameSky_;
	const RenderGraph::ResourceId irradiance = irradiance_ ? graph_.Import(irradiance_) : RenderGraph::kInvalid;
	graph_.AddPass("SkyIrradiance", skyLight, [&](RenderGraph::Builder& b) { b.Write(irradiance, D3D12_RESOURCE_STATE_RENDER_TARGET); }, [&, irradiance](RenderGraph::PassContext& ctx) {
		Fullscreen(ctx, kFullscreenIrradiance, ctx.Texture(irradiance), nullptr);
	});
	// ---- screen space ambient occlusion: from the prepass depth, then a depth aware blur
	GpuTextureDesc aoDesc;
	aoDesc.width = target.width;
	aoDesc.height = target.height;
	aoDesc.format = aoDesc.targetFormat = DXGI_FORMAT_R8_UNORM;
	aoDesc.renderTarget = true;
	aoDesc.clearColor[0] = aoDesc.clearColor[1] = aoDesc.clearColor[2] = aoDesc.clearColor[3] = 1.0f;
	const bool ao = aoActive_;
	const RenderGraph::ResourceId aoRaw = ao ? graph_.CreateTransient("SceneAO.Raw", aoDesc, ResourceCategory::RenderTarget) : RenderGraph::kInvalid;
	const RenderGraph::ResourceId aoFinal = ao ? graph_.CreateTransient("SceneAO", aoDesc, ResourceCategory::RenderTarget) : RenderGraph::kInvalid;
	auto fullscreenAo = [&, viewport, scissor](RenderGraph::PassContext& ctx, FullscreenKind kind, RenderGraph::ResourceId output, uint32_t table) {
		ID3D12PipelineState* pso = FullscreenPipeline(kind, DXGI_FORMAT_R8_UNORM);
		if (!pso) {
			ctx.note = "no pipeline";
			return;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.RtvCpu(ctx.Texture(output)->target);
		ctx.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		ctx.list->RSSetViewports(1, &viewport);
		ctx.list->RSSetScissorRects(1, &scissor);
		BindCommon(ctx.list, true);
		ctx.list->SetGraphicsRootDescriptorTable(9, gpu.SrvGpu(table));
		ctx.list->SetPipelineState(pso);
		ctx.list->DrawInstanced(3, 1, 0, 0);
		ctx.drawCalls = 1;
		ctx.triangles = 1;
	};
	graph_.AddPass("AmbientOcclusion", ao, [&](RenderGraph::Builder& b) {
		b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		b.Write(aoRaw, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}, [&, depth, aoRaw, fullscreenAo](RenderGraph::PassContext& ctx) {
		gpu.CopySrv(postTable_ + 1, ctx.Texture(depth)->srv);
		fullscreenAo(ctx, kFullscreenAo, aoRaw, postTable_);
		ctx.note = Format("radius %.2f m, intensity %.2f", scene.frame.aoRadius, scene.frame.aoIntensity);
	});
	graph_.AddPass("AmbientOcclusionBlur", ao, [&](RenderGraph::Builder& b) {
		b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		b.Read(aoRaw, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		b.Write(aoFinal, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}, [&, depth, aoRaw, aoFinal, fullscreenAo](RenderGraph::PassContext& ctx) {
		gpu.CopySrv(postTable_ + kPostTableSize, ctx.Texture(aoRaw)->srv);
		gpu.CopySrv(postTable_ + kPostTableSize + 1, ctx.Texture(depth)->srv);
		fullscreenAo(ctx, kFullscreenAoBlur, aoFinal, postTable_ + kPostTableSize);
	});
	graph_.AddPass("Opaque", true, [&](RenderGraph::Builder& b) {
		b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(surface, D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(velocity, D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		if (ao) {
			b.Read(aoFinal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		if (shadow != RenderGraph::kInvalid) {
			b.Read(shadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		if (skyLight) {
			b.Read(irradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	}, [&, color, depth, surface, velocity, ao, aoFinal](RenderGraph::PassContext& ctx) {
		if (ao) {
			gpu.CopySrv(globalsTable_ + 3, ctx.Texture(aoFinal)->srv);
		}
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.RtvCpu(ctx.Texture(color)->target), dsv = gpu.DsvCpu(ctx.Texture(depth)->target);
		const D3D12_CPU_DESCRIPTOR_HANDLE targets[3] = {rtv, gpu.RtvCpu(ctx.Texture(surface)->target), gpu.RtvCpu(ctx.Texture(velocity)->target)};
		ctx.list->OMSetRenderTargets(3, targets, FALSE, &dsv);
		const float clear[4] = {0, 0, 0, 0};
		ctx.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
		ctx.list->ClearRenderTargetView(targets[1], clear, 0, nullptr); // (weight 0: nothing reflects there)
		const float nothing[4] = {1000.0f, 1000.0f, 0, 0};
		ctx.list->ClearRenderTargetView(targets[2], nothing, 0, nullptr);
		ctx.list->RSSetViewports(1, &viewport);
		ctx.list->RSSetScissorRects(1, &scissor);
		BindCommon(ctx.list, true);
		DrawItems(ctx, opaque_, kForward, kForward, -1);
		ctx.note = Format("%u punctual lights in %ux%ux%u clusters", punctualCount_, kClusterX, kClusterY, kClusterZ);
	});
	// ---- screen space reflections: what a smooth surface mirrors, where that is on screen. Read from the picture of the
	// frame before (the temporal history, as the host does it), added on top of the sky / probe reflection the
	// surface was shaded with.
	if (ssr_ && historyValid_ && history_[historyIndex_] && history_[historyIndex_]->desc.width == target.width && history_[historyIndex_]->desc.height == target.height) {
		const RenderGraph::ResourceId before = graph_.Import(history_[historyIndex_]);
		graph_.AddPass("ScreenSpaceReflections", true, [&, color, depth, surface, before](RenderGraph::Builder& b) {
			b.Read(before, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(surface, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, color, depth, surface, before](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenSsr, ctx.Texture(color), ctx.Texture(before), ctx.Texture(depth), ctx.Texture(surface));
		});
	}
	graph_.AddPass("Sky", true, [&](RenderGraph::Builder& b) {
		b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}, [&, color, depth](RenderGraph::PassContext& ctx) {
		ID3D12PipelineState* pso = FullscreenPipeline(kFullscreenSky, kSceneColorFormat);
		if (!pso) {
			ctx.note = "no pipeline";
			return;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.RtvCpu(ctx.Texture(color)->target), dsv = gpu.DsvCpu(ctx.Texture(depth)->target);
		ctx.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
		ctx.list->RSSetViewports(1, &viewport);
		ctx.list->RSSetScissorRects(1, &scissor);
		BindCommon(ctx.list, true);
		ctx.list->SetPipelineState(pso);
		ctx.list->DrawInstanced(3, 1, 0, 0);
		ctx.drawCalls = 1;
		ctx.triangles = 1;
	});
	// ---- decal boxes (the cars' blob shadows): they darken whatever opaque surface lies inside them, found from the depth
	graph_.AddPass("Decals", !decals_.empty(), [&](RenderGraph::Builder& b) {
		b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}, [&, color, depth](RenderGraph::PassContext& ctx) {
		if (postNext_ >= kPostTables) {
			return;
		}
		const uint32_t table = postTable_ + kPostTableSize * postNext_++;
		gpu.CopySrv(table + 1, ctx.Texture(depth)->srv);
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.RtvCpu(ctx.Texture(color)->target);
		ctx.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		ctx.list->RSSetViewports(1, &viewport);
		ctx.list->RSSetScissorRects(1, &scissor);
		BindCommon(ctx.list, true);
		ctx.list->SetGraphicsRootDescriptorTable(9, gpu.SrvGpu(table));
		DrawItems(ctx, decals_, kDecal, kDecal, -1);
	});
	graph_.AddPass("Transparent", !transparent_.empty(), [&](RenderGraph::Builder& b) {
		b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		if (shadow != RenderGraph::kInvalid) {
			b.Read(shadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		if (skyLight) {
			b.Read(irradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	}, [&, color, depth](RenderGraph::PassContext& ctx) {
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.RtvCpu(ctx.Texture(color)->target), dsv = gpu.DsvCpu(ctx.Texture(depth)->target);
		ctx.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
		ctx.list->RSSetViewports(1, &viewport);
		ctx.list->RSSetScissorRects(1, &scissor);
		BindCommon(ctx.list, true);
		DrawItems(ctx, transparent_, kForwardBlend, kForwardBlend, -1);
	});
	// ---- particles (the game's particle systems, tyre smoke): over everything drawn so far
	particlesDrawn_ = 0;
	graph_.AddPass("Particles", particlesVa_ != 0 && !particleBatches_.empty(), [&, color, depth](RenderGraph::Builder& b) {
		b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (shadow != RenderGraph::kInvalid) {
			b.Read(shadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		if (skyLight) {
			b.Read(irradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	}, [&, color, depth](RenderGraph::PassContext& ctx) {
		ID3D12PipelineState* pso = ParticlePipeline();
		if (!pso) {
			ctx.note = "no pipeline";
			return;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.RtvCpu(ctx.Texture(color)->target);
		ctx.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		ctx.list->RSSetViewports(1, &viewport);
		ctx.list->RSSetScissorRects(1, &scissor);
		BindCommon(ctx.list, true);
		ctx.list->SetGraphicsRootShaderResourceView(2, particlesVa_); // (the particle buffer takes the instance buffer's place)
		ctx.list->SetPipelineState(pso);
		for (const AeParticleBatch& batch : particleBatches_) {
			if (batch.count == 0 || postNext_ >= kPostTables) {
				continue;
			}
			const uint32_t table = postTable_ + kPostTableSize * postNext_++;
			SceneTexture* texture = batch.texture ? store.Texture(batch.texture) : nullptr;
			uint32_t flags = batch.flags;
			if (texture && texture->srv != UINT32_MAX) {
				gpu.CopySrv(table, texture->srv);
				texture->lastUsedFrame = frame;
			} else {
				flags |= kAeParticleDisc;
			}
			gpu.CopySrv(table + 1, ctx.Texture(depth)->srv);
			ctx.list->SetGraphicsRootDescriptorTable(9, gpu.SrvGpu(table));
			const uint32_t constants[4] = {batch.first, 0, 0, flags};
			ctx.list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
			ctx.list->DrawInstanced(6, batch.count, 0, 0);
			ctx.drawCalls += 1;
			ctx.triangles += batch.count * 2;
			particlesDrawn_ += batch.count;
		}
		ctx.note = Format("%u particles in %u batches", particlesDrawn_, uint32_t(particleBatches_.size()));
	});
	// ---- post-processing. `post` is the picture as it stands; every step that runs replaces it.
	RenderGraph::ResourceId post = color;
	// temporal antialiasing: this frame (drawn with a sub pixel offset) blended into the picture of the frames before
	const bool taa = taa_;
	if (taa) {
		for (auto& h : history_) {
			if (h && (h->desc.width != target.width || h->desc.height != target.height)) {
				gpu.RetireTexture(std::move(h), retireFence, frame);
				historyValid_ = false;
			}
			if (!h) {
				GpuTextureDesc d = colorDesc;
				// (with an alpha channel, unlike the scene colour: it keeps whether the pixel showed something that moves by itself)
				d.format = d.targetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
				h = gpu.CreateTexture(d, "Scene.History", ResourceCategory::RenderTarget, Lifetime::Persistent, frame);
				historyValid_ = false;
			}
		}
	}
	if (taa && history_[0] && history_[1]) {
		historyIndex_ ^= 1;
		const RenderGraph::ResourceId current = graph_.Import(history_[historyIndex_]), previous = graph_.Import(history_[historyIndex_ ^ 1]);
		graph_.AddPass("TemporalAA", true, [&, color, depth, velocity, current, previous](RenderGraph::Builder& b) {
			b.Read(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(velocity, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(previous, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(current, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, color, depth, velocity, current, previous](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenTaa, ctx.Texture(current), ctx.Texture(color), ctx.Texture(depth), ctx.Texture(previous), nullptr, ctx.Texture(velocity));
		});
		post = current;
		historyValid_ = true;
	} else {
		historyValid_ = false;
	}
	// motion blur: along the way every still point of the world moved on screen since the frame before (the camera's motion)
	if (motionBlur_ > 0.0f) {
		const RenderGraph::ResourceId blurred = graph_.CreateTransient("SceneMotionBlur", colorDesc, ResourceCategory::RenderTarget);
		const RenderGraph::ResourceId source = post;
		graph_.AddPass("MotionBlur", true, [&, source, depth, velocity, blurred](RenderGraph::Builder& b) {
			b.Read(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(velocity, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(blurred, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, source, depth, velocity, blurred](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenMotionBlur, ctx.Texture(blurred), ctx.Texture(source), ctx.Texture(depth), ctx.Texture(velocity));
		});
		post = blurred;
	}
	// bloom: the picture scaled down level by level, then added up again from the smallest (the host's scheme: every
	// level is the level below it scaled up, mixed with its own by the scatter value)
	RenderGraph::ResourceId bloom = RenderGraph::kInvalid;
	if (bloomMix_ > 0.0f && target.width >= 64 && target.height >= 64) {
		RenderGraph::ResourceId down[kBloomLevels], up[kBloomLevels];
		for (uint32_t i = 0; i < kBloomLevels; ++i) {
			GpuTextureDesc d = colorDesc;
			d.width = std::max(1u, target.width >> (i + 1));
			d.height = std::max(1u, target.height >> (i + 1));
			down[i] = graph_.CreateTransient(i == 0 ? "Bloom.Down0" : i == 1 ? "Bloom.Down1" : i == 2 ? "Bloom.Down2" : i == 3 ? "Bloom.Down3" : "Bloom.Down4", d, ResourceCategory::RenderTarget);
			up[i] = i + 1 < kBloomLevels ? graph_.CreateTransient(i == 0 ? "Bloom.Up0" : i == 1 ? "Bloom.Up1" : i == 2 ? "Bloom.Up2" : "Bloom.Up3", d, ResourceCategory::RenderTarget) : down[i];
			const RenderGraph::ResourceId source = i == 0 ? post : down[i - 1], output = down[i];
			const float first = i == 0 ? 1.0f : 0.0f;
			graph_.AddPass(i == 0 ? "BloomDown0" : i == 1 ? "BloomDown1" : i == 2 ? "BloomDown2" : i == 3 ? "BloomDown3" : "BloomDown4", true, [&, source, output](RenderGraph::Builder& b) {
				b.Read(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				b.Write(output, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}, [&, source, output, first](RenderGraph::PassContext& ctx) {
				GpuTexture* out = ctx.Texture(output);
				const float params[4] = {first, 0, 1.0f / float(out->desc.width), 1.0f / float(out->desc.height)};
				Fullscreen(ctx, kFullscreenBloomDown, out, ctx.Texture(source), nullptr, nullptr, params);
			});
		}
		for (int i = int(kBloomLevels) - 2; i >= 0; --i) {
			const RenderGraph::ResourceId low = up[i + 1], high = down[i], output = up[i];
			graph_.AddPass(i == 0 ? "BloomUp0" : i == 1 ? "BloomUp1" : i == 2 ? "BloomUp2" : "BloomUp3", true, [&, low, high, output](RenderGraph::Builder& b) {
				b.Read(low, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				b.Read(high, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				b.Write(output, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}, [&, low, high, output](RenderGraph::PassContext& ctx) {
				GpuTexture* out = ctx.Texture(output);
				GpuTexture* lowTexture = ctx.Texture(low);
				const float params[4] = {bloomScatter_, 0, 1.0f / float(lowTexture->desc.width), 1.0f / float(lowTexture->desc.height)};
				Fullscreen(ctx, kFullscreenBloomUp, out, lowTexture, ctx.Texture(high), nullptr, params);
			});
		}
		bloom = up[0];
	}
	// ---- Aether's own exposure: the scene's mean luminance into a 1 x 1 target (when the host's is not available)
	GpuTextureDesc exposureDesc;
	exposureDesc.width = exposureDesc.height = 1;
	exposureDesc.format = exposureDesc.targetFormat = DXGI_FORMAT_R16_FLOAT;
	exposureDesc.renderTarget = true;
	const bool ownExposure = ownExposure_;
	// two targets that live across frames: this frame's value starts from the one of the frame before (g_ExposureParams.w)
	RenderGraph::ResourceId exposureTarget = RenderGraph::kInvalid, exposurePrevious = RenderGraph::kInvalid;
	if (ownExposure) {
		for (auto& e : exposureHistory_) {
			if (!e) {
				e = gpu.CreateTexture(exposureDesc, "Scene.Exposure", ResourceCategory::RenderTarget, Lifetime::Persistent, frame);
			}
		}
		exposureIndex_ ^= 1;
		exposureTarget = graph_.Import(exposureHistory_[exposureIndex_]);
		exposurePrevious = graph_.Import(exposureHistory_[exposureIndex_ ^ 1]);
	}
	exposureValid_ = ownExposure; // from the next frame on there is a value to start from
	graph_.AddPass("Exposure", ownExposure, [&, post, exposureTarget, exposurePrevious](RenderGraph::Builder& b) {
		b.Read(post, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		b.Read(exposurePrevious, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		b.Write(exposureTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}, [&, post, exposureTarget, exposurePrevious](RenderGraph::PassContext& ctx) {
		const RenderGraph::ResourceId color = post;
		ID3D12PipelineState* pso = FullscreenPipeline(kFullscreenExposure, DXGI_FORMAT_R16_FLOAT);
		if (!pso) {
			ctx.note = "no pipeline";
			return;
		}
		const uint32_t table = postTable_ + kPostTableSize * 3;
		gpu.CopySrv(table, ctx.Texture(color)->srv);
		gpu.CopySrv(table + 2, ctx.Texture(exposurePrevious)->srv);
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.RtvCpu(ctx.Texture(exposureTarget)->target);
		const D3D12_VIEWPORT one = {0, 0, 1, 1, 0, 1};
		const D3D12_RECT oneRect = {0, 0, 1, 1};
		ctx.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		ctx.list->RSSetViewports(1, &one);
		ctx.list->RSSetScissorRects(1, &oneRect);
		BindCommon(ctx.list, true);
		ctx.list->SetGraphicsRootDescriptorTable(9, gpu.SrvGpu(table));
		ctx.list->SetPipelineState(pso);
		ctx.list->DrawInstanced(3, 1, 0, 0);
		ctx.drawCalls = 1;
		ctx.triangles = 1;
		ctx.note = Format("metered, EV100 %.2f .. %.2f, compensation %.2f", scene.frame.exposureMinEV, scene.frame.exposureMaxEV, scene.frame.exposureCompensation);
	});
	graph_.AddPass("Composite", true, [&, post, bloom](RenderGraph::Builder& b) {
		b.Read(post, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (ownExposure) {
			b.Read(exposureTarget, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		if (bloom != RenderGraph::kInvalid) {
			b.Read(bloom, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		b.Write(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}, [&, post, bloom, ownExposure, exposureTarget](RenderGraph::PassContext& ctx) {
		const RenderGraph::ResourceId color = post;
		ID3D12PipelineState* pso = FullscreenPipeline(kFullscreenComposite, target.backBufferFormat);
		if (!pso) {
			ctx.note = "no pipeline for this back buffer format";
			return;
		}
		const uint32_t compositeTable = postTable_ + kPostTableSize * 2;
		gpu.CopySrv(compositeTable, ctx.Texture(color)->srv);
		if (ownExposure) {
			gpu.CopySrv(compositeTable + 2, ctx.Texture(exposureTarget)->srv);
		}
		if (bloom != RenderGraph::kInvalid) {
			gpu.CopySrv(compositeTable + 3, ctx.Texture(bloom)->srv);
		}
		ctx.list->OMSetRenderTargets(1, &target.backBufferRtv, FALSE, nullptr);
		ctx.list->RSSetViewports(1, &viewport);
		// compare mode: the right half is Aether's, the left half stays the host's picture
		const D3D12_RECT rect = {compare ? LONG(target.width / 2) : 0, 0, LONG(target.width), LONG(target.height)};
		ctx.list->RSSetScissorRects(1, &rect);
		BindCommon(ctx.list, true);
		ctx.list->SetGraphicsRootDescriptorTable(9, gpu.SrvGpu(compositeTable));
		ctx.list->SetPipelineState(pso);
		ctx.list->DrawInstanced(3, 1, 0, 0);
		ctx.drawCalls = 1;
		ctx.triangles = 1;
		ctx.note = compare ? "right half (compare mode)" : "full frame";
	});

	if (!graph_.Compile()) {
		graph_.End(retireFence); // (the pool is still looked after: a frame that could not get its textures must not keep the old ones)
		return false;
	}
	const bool timing = settings.gpuTiming && timer_.Begin();
	timedPasses_.clear();
	passStampBegin_.assign(graph_.PassCount(), UINT32_MAX);
	passStampEnd_.assign(graph_.PassCount(), UINT32_MAX);
	graph_.Execute(list, [&](size_t index, bool begin) {
		if (timing) {
			(begin ? passStampBegin_ : passStampEnd_)[index] = timer_.Stamp(list);
		}
	});
	if (timing) {
		timer_.Resolve(list);
	}

	// ---- results for the profiler and the diagnostics
	std::vector<PassTiming> timings;
	uint32_t draws = 0, triangles = 0;
	for (const RenderGraph::PassResult& r : graph_.Results()) {
		PassTiming t;
		t.name = r.name;
		t.executed = r.executed;
		t.cpuMs = r.cpuMs;
		t.drawCalls = r.drawCalls;
		t.note = r.note;
		timings.push_back(t);
		timedPasses_.push_back(r.name);
		draws += r.drawCalls;
		triangles += r.triangles;
	}
	Profiler::Get().SceneFrame(timings, draws, triangles);

	uint32_t maxPerCluster = 0, usedClusters = 0;
	for (size_t c = 0; c + 1 < clusterGrid_.size(); c += 2) {
		maxPerCluster = std::max(maxPerCluster, clusterGrid_[c + 1]);
		usedClusters += clusterGrid_[c + 1] ? 1 : 0;
	}
	uint32_t unsupportedDraws = 0;
	for (const DrawItem& i : opaque_) {
		unsupportedDraws += (!i.material || i.material->desc.materialClass == kAeMaterialUnsupported) ? 1 : 0;
	}
	json stats = {
		{"frame", frame},
		{"resolution", {target.width, target.height}},
		{"visibleInstances", instanceCount_},
		{"materialsInFrame", materialCount_},
		{"opaqueDraws", opaque_.size()},
		{"transparentDraws", transparent_.size()},
		{"shadowCasterDraws", casters_.size()},
		{"unsupportedMaterialDraws", unsupportedDraws},
		{"drawCalls", draws},
		{"instancing", {{"instancesDrawn", itemsDrawn_}, {"inInstancedDraws", itemsInstanced_}, {"drawCallsSaved", itemsDrawn_ > draws ? itemsDrawn_ - draws : 0}}},
		{"triangles", triangles},
		{"pipelineStates", pipelines_.size() + fullscreenPipelines_.size()},
		{"lights", {{"directional", hasSun_ ? 1 : 0}, {"punctual", punctualCount_}, {"clusters", kClusterX * kClusterY * kClusterZ}, {"clustersWithLights", usedClusters},
			{"maxLightsInACluster", maxPerCluster}, {"lightIndexEntries", lightIndices_.size()}, {"clusterGrid", {kClusterX, kClusterY, kClusterZ}}}},
		{"shadows", {{"cascades", cascades_.size()}, {"cascadeSize", shadowCascadeSize_}, {"atlasBytes", shadowAtlas_ ? shadowAtlas_->bytes : 0}}},
		{"reflectionProbes", {{"host", scene.probes.size()}, {"inFrame", probesInFrame_}}},
		{"particles", {{"submitted", scene.particles.size()}, {"batches", scene.particleBatches.size()}, {"drawn", particlesDrawn_}}},
		{"sceneColorFormat", int(kSceneColorFormat)},
	};
	{
		std::lock_guard lock(statsMutex_);
		stats_ = std::move(stats);
	}
	return true;
}

void SceneRenderer::EndFrame(const HostGfx& host, UINT64 fence) {
	(void)host;
	GpuResources& gpu = GpuResources::Get();
	timer_.Submit(fence, frame_);
	graph_.End(fence);
	if (globalsTable_ != UINT32_MAX) {
		gpu.FreeSrv(globalsTable_, kGlobalsTableSize, fence);
		if (probesTable_ != UINT32_MAX) {
			gpu.FreeSrv(probesTable_, kMaxProbes, fence);
			probesTable_ = UINT32_MAX;
		}
		globalsTable_ = UINT32_MAX;
	}
	if (postTable_ != UINT32_MAX) {
		gpu.FreeSrv(postTable_, kPostTableSize * kPostTables, fence);
		postTable_ = UINT32_MAX;
	}
	gpu.EndFrame(fence);
}

void SceneRenderer::Release(bool leak) {
	GpuResources& gpu = GpuResources::Get();
	graph_.ReleasePool(0, leak); // retired here, dropped by gpu.Shutdown below
	if (leak) {
		for (auto& [k, pso] : pipelines_) pso.Detach();
		for (auto& [k, pso] : fullscreenPipelines_) pso.Detach();
		root_.Detach();
		defaultsVb_.Detach();
		for (auto* t : {&white_, &flatNormal_, &black_, &shadowAtlas_}) {
			if (*t) {
				(*t)->resource.Detach();
			}
		}
	}
	pipelines_.clear();
	fullscreenPipelines_.clear();
	root_.Reset();
	defaultsVb_.Reset();
	white_.reset();
	flatNormal_.reset();
	black_.reset();
	shadowAtlas_.reset();
	irradiance_.reset();
	history_[0].reset();
	history_[1].reset();
	exposureHistory_[0].reset();
	exposureHistory_[1].reset();
	exposureValid_ = false;
	shadowCascadeSize_ = 0;
	defaultsUploaded_ = false;
	objectsReady_ = false;
	timer_.Release(leak);
	SceneStore::Get().ReleaseAll(leak);
	gpu.Shutdown(leak);
	device_ = nullptr;
	globalsTable_ = postTable_ = nullCubeSrv_ = nullTextureSrv_ = fallbackTable_ = UINT32_MAX;
	std::lock_guard lock(statsMutex_);
	stats_ = json::object();
}

json SceneRenderer::Stats() {
	std::lock_guard lock(statsMutex_);
	return stats_;
}

} // namespace aether
