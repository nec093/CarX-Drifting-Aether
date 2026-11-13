#include "Bvh.h"
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
// probe grid GI: probes around the camera on a grid fixed in the world (aether_scene.hlsli has the same numbers)
const int kGiCounts[3] = {24, 8, 24};
const float kGiSpacing = 3.0f, kGiSpacingY = 2.5f; // metres between probes on the ground / in height
const uint32_t kGiTile = 8;                         // a probe's tile: 8 x 8 directions
// (11: the GI probes' irradiance, 12: their distances)
const uint32_t kGlobalsTableSize = 13; // (8: the reflection probes' atlas, 9: its blurred copy, 10: its depth) shadow atlas, sky, sky irradiance, ambient occlusion | physical sky: sky (with clouds), transmittance LUT, multiple scattering LUT, cloud noise
const uint32_t kPostTableSize = 4;    // per full screen pass: colour / main input, depth, two auxiliary inputs
const float kExposureAdaptationRate = 2.0f; // per second: the automatic exposure covers 63 % of a change in half a second
const uint32_t kPostTables = 64;     // 0..3: ambient occlusion, its blur, composite, exposure metering; the rest handed out per pass (Fullscreen)
const uint32_t kIrradianceSize = 32;  // octahedral map of the sky's diffuse light
const char* const kFullscreenEntry[] = {"PSSky", "PSComposite", "PSAmbientOcclusion", "PSAmbientOcclusionBlur", "PSExposure", "PSIrradiance", "PSTaa", "PSMotionBlur", "PSBloomDown", "PSBloomUp", "PSSsr", "PSTransmittanceLut", "PSMultiScatterLut", "PSSkyViewLut", "PSCloudNoise", "PSSkyCloudsLut", "PSClouds", "PSVolumetricFog", "PSVolumetricFogApply", "PSReflectionBlur", "PSBvhDebug", "PSRayTrace", "PSGiRays", "PSGiIrradiance", "PSGiDepth", "PSLensFlare", "PSSmokeApply"};
// reflection probes: each is six square tiles (3 x 2: +X -X +Y | -Y +Z -Z); the atlas holds them 4 x 2
const uint32_t kProbeFace = 128, kProbeBlurFace = 32;
const float kProbeCell = 12.0f;      // the grid's spacing on the ground (m)
const float kProbeCellHeight = 3.0f; // ... and in height
const float kReflectionNear = 0.3f, kReflectionFar = 200.0f;
const DXGI_FORMAT kReflectionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // a: 1 = something was drawn there (0: the sky)
const uint32_t kBloomLevels = 5;
// xy: how far each pixel moved on screen since the frame before (uv units; x = 1000: nothing drawn there),
// z: 1 = the surface belongs to something that moves by itself (a car). (The scene colour target has no alpha
// channel, so this cannot live there: a mask written to its alpha read back as 1 everywhere.)
const DXGI_FORMAT kVelocityFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
const DXGI_FORMAT kOcclusionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // rgb: light bounced off nearby surfaces, a: ambient occlusion
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
	float atmosphere[4];      // x: the physical sky's scale to scene units, y: night sky floor, z: eye altitude (km), w: 1 = physical sky
	float clouds[4];          // x: coverage, yz: how far the wind has carried them (noise uv), w: 1 = clouds
	float probeGrid[4];          // x: the grid's spacing, y: its spacing in height, z: a different number every frame, w: 1 = probes on
	float probePositions[8][4];  // xyz: where the probe stands, w: 1 = usable
	float reflectionBasis[6][3][4]; // per cube face: the directions of the tile's centre, its right edge and its top edge (w of the first two: near, far)
	float giGrid[4];             // xyz: the grid cell of the GI volume's lowest corner, w: 1 = probe grid GI on
	float giPrev[4];             // xyz: the same the frame before, w: 1 = the probes hold something
	float giParams[4];           // x: metres between probes on the ground, y: in height, z: a different number every frame, w: number of punctual lights
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
	smokePipeline_.Reset();
	particlePipelineFailed_ = false;
	atmosphereLutsValid_ = false; // (the LUT shaders may have changed)
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
		// (two ranges in one table: t0..t3 of space2, then t0..t3 of space4 for the physical sky's textures)
		D3D12_DESCRIPTOR_RANGE globalsRange[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 2, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 9, 0, 4, 4}}; // (space4 t4..t6: the reflection probes)
		D3D12_DESCRIPTOR_RANGE postRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kPostTableSize, 4, 2, 0};
		D3D12_DESCRIPTOR_RANGE probesRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaxProbes, 0, 3, 0}; // space3: reflection probe cubes
		D3D12_ROOT_PARAMETER params[18] = {};
		// space5: every texture in the SRV heap, by its index there (a ray's hit reads the base texture of whatever
		// material it landed on: there is no telling which beforehand)
		static const D3D12_DESCRIPTOR_RANGE allTexturesRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GpuResources::kSrvHeapSize, 0, 5, 0};
		params[17].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[17].DescriptorTable = {1, &allTexturesRange};
		params[17].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		params[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t10: the ray instances' materials
		params[16].Descriptor = {10, 0};
		params[16].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		// t6..t9: the ray tracing buffers (pooled nodes, pooled triangles, this frame's instance tree, its instances)
		for (UINT i = 0; i < 4; ++i) {
			params[12 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[12 + i].Descriptor = {6 + i, 0};
			params[12 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		}
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
		params[8].DescriptorTable = {2, globalsRange};
		params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // (the particles' vertex shader lights them: shadow atlas, GI probes)
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
		const bool probe = kind == kForwardProbe;
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
		d.DepthStencilState.DepthWriteMask = forward && !probe ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL; // (the reflection cube has no depth prepass)
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
			d.RTVFormats[0] = probe ? kReflectionFormat : kSceneColorFormat;
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
ID3D12PipelineState* SceneRenderer::ParticlePipeline(bool smokeTarget) {
	ComPtr<ID3D12PipelineState>& pipeline = smokeTarget ? smokePipeline_ : particlePipeline_;
	if (pipeline || particlePipelineFailed_ || !vsParticle_ || !psParticle_) {
		return pipeline.Get();
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
	if (smokeTarget) {
		// (the smoke's own target: its alpha gathers how much of the picture the smoke covers)
		blend.SrcBlendAlpha = D3D12_BLEND_ONE;
		blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	}
	d.NumRenderTargets = 1;
	d.RTVFormats[0] = smokeTarget ? DXGI_FORMAT_R16G16B16A16_FLOAT : kSceneColorFormat;
	d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.SampleDesc.Count = 1;
	HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pipeline));
	if (FAILED(hr)) {
		Log::Get().Write(Severity::Error, "SceneRenderer", "E_PIPELINE_STATE", "The particle pipeline state could not be created: no particles", {}, {{"hresult", HrToString(hr)}});
		pipeline.Reset();
		particlePipelineFailed_ = true;
	}
	return pipeline.Get();
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
	if (kind == kFullscreenFogApply) {
		// picture * what the fog lets through + the fog's own light (the shader returns the light in rgb, the rest in a)
		D3D12_RENDER_TARGET_BLEND_DESC& blend = d.BlendState.RenderTarget[0];
		blend.BlendEnable = TRUE;
		blend.SrcBlend = D3D12_BLEND_ONE;
		blend.DestBlend = D3D12_BLEND_SRC_ALPHA;
		blend.BlendOp = D3D12_BLEND_OP_ADD;
		blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
		blend.DestBlendAlpha = D3D12_BLEND_ONE;
		blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	}
	if (kind == kFullscreenGiIrradiance || kind == kFullscreenGiDepth) {
		// what the probes held, moved a step towards this frame's rays (the shader gives the step in a: 1 = start anew)
		D3D12_RENDER_TARGET_BLEND_DESC& blend = d.BlendState.RenderTarget[0];
		blend.BlendEnable = TRUE;
		blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		blend.BlendOp = D3D12_BLEND_OP_ADD;
		blend.SrcBlendAlpha = D3D12_BLEND_ONE;
		blend.DestBlendAlpha = D3D12_BLEND_ZERO;
		blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	}
	if (kind == kFullscreenSmokeApply) {
		// the smoke over the picture (premultiplied; a: how much of the picture it covers)
		D3D12_RENDER_TARGET_BLEND_DESC& blend = d.BlendState.RenderTarget[0];
		blend.BlendEnable = TRUE;
		blend.SrcBlend = D3D12_BLEND_ONE;
		blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		blend.BlendOp = D3D12_BLEND_OP_ADD;
		blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
		blend.DestBlendAlpha = D3D12_BLEND_ONE;
		blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	}
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
	probeItems_.clear();
	rayMaterials_.clear();
	const Vec3 cameraNow = {scene.frame.cameraPosition[0], scene.frame.cameraPosition[1], scene.frame.cameraPosition[2]};
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
		// (the reflection cube: what is near enough and large enough to be seen in a reflection)
		// (the reflection probes stand near the camera: what is near enough and large enough to show in them, and stands still)
		const float probeDistance = Length(center - cameraNow);
		const bool inProbe = reflection_ && probeJobCount_ > 0 && !inst.shadowOnly && !inst.moved && probeDistance - radius < kReflectionFar &&
			radius > 0.2f && radius > std::max(probeDistance - 30.0f, 0.0f) * 0.01f;
		// (rays may find anything the camera would show, in view or not)
		const bool inRays = raysWanted_ && !inst.shadowOnly;
		if (!inView && !castsShadow && !inProbe && !inRays) {
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
			if (inProbe && material && !(flags & kAeMaterialBlend)) {
				const uint32_t cls = material->desc.materialClass;
				const bool vehicle = cls == kAeMaterialVehiclePaint || cls == kAeMaterialVehicleGeneric || cls == kAeMaterialVehicleGlass || cls == kAeMaterialVehicleRim ||
					cls == kAeMaterialVehicleStitches || cls == kAeMaterialVehicleLights || cls == kAeMaterialVehicleTire;
				if (!vehicle && cls != kAeMaterialBlobShadow && cls != kAeMaterialUnsupported) {
					probeItems_.push_back(item);
				}
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
		if (inRays) {
			// Not there for rays: what is not a surface (the cars' blob shadow boxes, helper geometry, materials Aether does
			// not translate) and what light goes through (glass, anything blended): a bit per sub mesh. The others'
			// materials go into the table a hit looks its colour up in.
			uint32_t skip = 0, kept = 0, twoSided = 0;
			const uint32_t materialOffset = uint32_t(rayMaterials_.size() / 2);
			const uint32_t count = uint32_t(inst.materials.size());
			for (uint32_t s = 0; s < count; ++s) {
				const SceneMaterial* material = inst.materials[s] ? store.Material(inst.materials[s]) : nullptr;
				const uint32_t cls = material ? material->desc.materialClass : uint32_t(kAeMaterialUnsupported);
				const bool solid = material && cls != kAeMaterialUnsupported && cls != kAeMaterialSkip && cls != kAeMaterialBlobShadow && !(material->desc.flags & kAeMaterialBlend);
				if (solid) {
					++kept;
				} else if (s < 32) {
					skip |= 1u << s;
				}
				if (material && (material->desc.flags & kAeMaterialDoubleSided) && s < 32) {
					twoSided |= 1u << s;
				}
				rayMaterials_.push_back(material && material->frameStamp == frame ? material->frameIndex : 0);
				// (the texture a hit on this sub mesh is coloured with)
				SceneTexture* base = material && material->desc.textures[kAeSlotBase] ? store.Texture(material->desc.textures[kAeSlotBase]) : nullptr;
				rayMaterials_.push_back(base && base->srv != UINT32_MAX && base->desc.dimension != kAeTextureCube ? base->srv : UINT32_MAX);
				if (base) {
					base->lastUsedFrame = frame;
				}
			}
			if (kept > 0) {
				SceneBvh::Get().AddInstance(inst.desc.mesh, world, skip, materialOffset, twoSided, mirrored);
			}
		}
	}
	// equal keys next to each other, sub mesh by sub mesh: DrawItems turns such runs into one instanced draw
	if (rayMaterials_.empty()) {
		rayMaterials_.push_back(0);
		rayMaterials_.push_back(UINT32_MAX);
	}
	auto byKey = [](const DrawItem& a, const DrawItem& b) { return a.sortKey != b.sortKey ? a.sortKey < b.sortKey : a.subMesh < b.subMesh; };
	std::sort(opaque_.begin(), opaque_.end(), byKey);
	std::sort(casters_.begin(), casters_.end(), byKey);
	std::sort(probeItems_.begin(), probeItems_.end(), byKey);
	std::sort(transparent_.begin(), transparent_.end(), [](const DrawItem& a, const DrawItem& b) { return a.depth > b.depth; }); // back to front
}

// ---- reflection probes: where they stand and which faces are drawn this frame
//
// The probes stand on a grid in the world. The four places around the camera (a little ahead of it when it moves)
// are wanted; a wanted place without a probe takes the slot that has gone unwanted longest. A new probe's faces come
// first (two a frame); when every wanted probe is whole, the one drawn longest ago is drawn again, face by face: the
// light changes with the time of day.
void SceneRenderer::UpdateProbes(const SceneFrame& scene, uint64_t frame) {
	const Vec3 camera = {scene.frame.cameraPosition[0], scene.frame.cameraPosition[1], scene.frame.cameraPosition[2]};
	// Where the camera is heading: its travel, smoothed. (A view that jumps, another camera or the player swinging
	// the view round the car, is not travel: taken at face value it sent the probes metres away for a few frames, the
	// ones around the camera were given up for them, and the picture showed the sky's reflection until they were
	// drawn again: a flash.)
	if (probeCameraValid_) {
		const Vec3 step = camera - probeCameraBefore_;
		if (Length(step) < 3.0f) { // (3 m in a frame is 650 km/h at 60 frames a second: more is a cut)
			probeAhead_ = probeAhead_ * 0.92f + step * (30.0f * 0.08f);
			const float len = Length(probeAhead_);
			if (len > 24.0f) {
				probeAhead_ = probeAhead_ * (24.0f / len);
			}
		}
	} else {
		probeAhead_ = {0, 0, 0};
	}
	probeCameraBefore_ = camera;
	probeCameraValid_ = true;
	// The four places around the camera are always wanted; then the four around where it is heading, as far as there
	// are slots nobody wants this frame (so that they are drawn before the camera gets there).
	const int cy = int(std::floor(camera.y / kProbeCellHeight + 0.5f));
	int wanted[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	for (int k = 0; k < 8; ++k) {
		const Vec3 at = k < 4 ? camera : camera + probeAhead_;
		const int cx = int(std::floor(at.x / kProbeCell)), cz = int(std::floor(at.z / kProbeCell));
		const int cell[3] = {cx + (k & 1), cy, cz + ((k >> 1) & 1)};
		int slot = -1;
		for (uint32_t s = 0; s < kProbeSlots; ++s) {
			if (lightProbes_[s].used && lightProbes_[s].cell[0] == cell[0] && lightProbes_[s].cell[1] == cell[1] && lightProbes_[s].cell[2] == cell[2]) {
				slot = int(s);
			}
		}
		if (slot < 0) {
			// the slot unwanted longest (an unused one first); never one wanted this frame
			uint64_t oldest = UINT64_MAX;
			for (uint32_t s = 0; s < kProbeSlots; ++s) {
				const uint64_t age = lightProbes_[s].used ? lightProbes_[s].lastWanted : 0;
				if (age < oldest && !(lightProbes_[s].used && lightProbes_[s].lastWanted == frame)) {
					oldest = age;
					slot = int(s);
				}
			}
			// (for a place ahead, a probe that still stands near the camera is not given up either)
			if (slot >= 0 && k >= 4 && lightProbes_[slot].used && Length(lightProbes_[slot].position - camera) < kProbeCell * 2.0f) {
				slot = -1;
			}
			if (slot < 0) {
				continue;
			}
			LightProbe& p = lightProbes_[slot];
			p = LightProbe();
			p.used = true;
			memcpy(p.cell, cell, sizeof(cell));
			// (a metre above the grid's level: the level nearest the camera's height is about where the ground is)
			p.position = {float(cell[0]) * kProbeCell, float(cell[1]) * kProbeCellHeight + 1.0f, float(cell[2]) * kProbeCell};
		}
		lightProbes_[slot].lastWanted = frame;
		wanted[k] = slot;
	}
	// this frame's faces: unfinished probes first (the nearest to the camera), else the probe drawn longest ago
	for (uint32_t j = 0; j < _countof(probeJobs_); ++j) {
		int best = -1;
		float bestScore = 0;
		// (Every probe that holds a picture, not only the ones wanted this frame: the others are still read where
		// they stand near enough, and left alone they kept showing the light of the hour they were drawn in. With the
		// camera at rest the daylight stayed in the reflections after night fell, until the view moved.)
		for (uint32_t s = 0; s < kProbeSlots; ++s) {
			const LightProbe& p = lightProbes_[s];
			if (!p.used) {
				continue;
			}
			const bool whole = p.facesDone == 0x3Fu;
			const bool wantedNow = p.lastWanted == frame;
			const float score = whole ? float(frame - p.lastDrawn) : (wantedNow ? 1e9f : 1e8f) - Length(p.position - camera);
			if (best < 0 || score > bestScore) {
				best = int(s);
				bestScore = score;
			}
		}
		if (best < 0) {
			break;
		}
		LightProbe& p = lightProbes_[best];
		uint32_t face = p.nextFace % 6;
		if (p.facesDone != 0x3Fu) {
			while (p.facesDone & (1u << face)) {
				face = (face + 1) % 6;
			}
		}
		ProbeFaceJob& job = probeJobs_[probeJobCount_++];
		job.slot = uint32_t(best);
		job.face = face;
		// (bit 0 of cb, until the constants are uploaded: this face is drawn for the first time, the probe is not whole before the frame is over)
		job.cb = (p.facesDone & (1u << face)) ? 0 : 1;
		p.facesDone |= 1u << face;
		if (p.facesDone == 0x3Fu && p.wholeSince == 0) {
			p.wholeSince = frame;
		}
		p.nextFace = face + 1;
		p.lastDrawn = frame;
	}
}

// ---- the physical sky's sun (the atmosphere's numbers are the same as in shaders/aether_atmosphere.hlsli)

// Where the sun stands at `hours` (0 .. 24) for a place at 35 degrees north in early summer; north is +Z, east +X.
static Vec3 SunDirectionAt(float hours, float headingDegrees) {
	const float kPi = 3.14159265f;
	const float latitude = 35.0f * kPi / 180.0f, declination = 15.0f * kPi / 180.0f;
	const float hourAngle = (hours - 12.0f) * 15.0f * kPi / 180.0f;
	const float sinElevation = std::sin(latitude) * std::sin(declination) + std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
	const float elevation = std::asin(std::min(std::max(sinElevation, -1.0f), 1.0f));
	const float cosElevation = std::max(std::cos(elevation), 1e-4f);
	float cosAzimuth = (std::sin(declination) - sinElevation * std::sin(latitude)) / (cosElevation * std::cos(latitude));
	float azimuth = std::acos(std::min(std::max(cosAzimuth, -1.0f), 1.0f)); // from north, towards the east
	if (hourAngle > 0.0f) {
		azimuth = 2.0f * kPi - azimuth; // afternoon: in the west
	}
	azimuth += headingDegrees * kPi / 180.0f;
	return Normalize(Vec3{cosElevation * std::sin(azimuth), sinElevation, cosElevation * std::cos(azimuth)});
}

// What is left of the sun's light at the ground when the sun stands at `cosZenith` (1 = overhead).
static Vec3 AtmosphereTransmittance(float cosZenith) {
	const double ground = 6360.0, top = 6460.0;
	const double r0 = ground + 0.05;
	const double c = std::min(std::max(double(cosZenith), -1.0), 1.0), s = std::sqrt(std::max(0.0, 1.0 - c * c));
	// the ray (0, r0) + t (s, c): to the top sphere; behind the ground: nothing
	const double b = r0 * c;
	if (c < 0.0 && b * b - (r0 * r0 - ground * ground) > 0.0) {
		return {0, 0, 0};
	}
	const double end = -b + std::sqrt(std::max(0.0, b * b - (r0 * r0 - top * top)));
	const int steps = 64;
	double depth[3] = {0, 0, 0};
	for (int i = 0; i < steps; ++i) {
		const double t = end * (i + 0.5) / steps;
		const double x = s * t, y = r0 + c * t;
		const double h = std::max(std::sqrt(x * x + y * y) - ground, 0.0);
		const double densityR = std::exp(-h / 8.0), densityM = std::exp(-h / 1.2), ozone = std::max(0.0, 1.0 - std::abs(h - 25.0) / 15.0);
		const double rayleigh[3] = {5.802e-3, 13.558e-3, 33.1e-3}, absorb[3] = {0.650e-3, 1.881e-3, 0.085e-3};
		for (int k = 0; k < 3; ++k) {
			depth[k] += (rayleigh[k] * densityR + (3.996e-3 + 4.40e-3) * densityM + absorb[k] * ozone) * end / steps;
		}
	}
	return {float(std::exp(-depth[0])), float(std::exp(-depth[1])), float(std::exp(-depth[2]))};
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
	// ---- the physical sky: the sun stands where the time of day puts it, and has the colour the air leaves it.
	// The scene's own sun only sets the scale: at the height the scene had its sun, the physical sun is as bright as
	// that one was, so the scene's exposure and the cars' lamps keep their meaning.
	physicalSky_ = settings.skyMode == "physical" && settings.sky;
	if (physicalSky_) {
		const float sceneCos = hasSun_ ? std::max(sunDirection_.y, 0.1f) : 0.7f;
		const Vec3 atScene = AtmosphereTransmittance(sceneCos);
		const float atSceneLum = std::max(atScene.x * 0.2126f + atScene.y * 0.7152f + atScene.z * 0.0722f, 1e-3f);
		atmosphereScale_ = (hasSun_ && best > 0 ? best : 30.0f) / atSceneLum;
		if (!hasSun_) {
			sunShadows_ = true;
			sunLayerMask_ = ~0u;
		}
		// (the scene at night while the time of day setting says day: the sky's hour is midnight)
		const bool sceneNight = settings.followSceneNight && sceneNight_.load();
		sceneSunLum_ = hasSun_ ? best : 0.0f;
		sunDirection_ = SunDirectionAt(settings.timeOfDay, settings.sunHeading);
		if (sceneNight && sunDirection_.y > -0.21f) {
			sunDirection_ = SunDirectionAt(0.0f, settings.sunHeading);
		}
		// Night: once the twilight is over (the sun 7 to 12 degrees under the horizon) the light in the sky is the
		// moon's. A full moon, which stands opposite the sun. Everything that knows of "the sun" (shadows, the sky's
		// scattering, the clouds, the disc in the sky) then works with the moon, at the moon's strength: the same
		// physics, dimmer. The strength is not the real one (a full moon gives 2.5 millionths of the sun's light): it
		// is set so that the moon's light and its shadows read against the night sky's own glow.
		moonShare_ = std::min(std::max((-sunDirection_.y - 0.12f) / 0.09f, 0.0f), 1.0f);
		const float kMoonLight = 1.6e-4f;
		Vec3 tint = {1.06f, 0.99f, 0.90f}; // the sun above the air is a little warmer than the white the picture is balanced for (same luminance)
		float strength = atmosphereScale_;
		if (moonShare_ > 0.0f) {
			sunDirection_ = sunDirection_ * -1.0f;
			tint = {0.80f, 0.90f, 1.08f}; // (moonlight is sunlight off grey rock, slightly warm in truth: it is the dark adapted eye that sees it blue)
			strength = atmosphereScale_ * kMoonLight * moonShare_ * settings.moonlight * 2.0f;
			if (sceneNight && sceneSunLum_ > 0.0f) {
				// The scene's night lighting has a light for the moon of its own, and its lamps are set against that
				// light. So the moon is as bright as the scene's: a moon scaled from the day's sun was far dimmer, and
				// next to it every lamp of the map was a glare.
				const Vec3 at = AtmosphereTransmittance(sunDirection_.y);
				strength = sceneSunLum_ / std::max(at.x * tint.x * 0.2126f + at.y * tint.y * 0.7152f + at.z * tint.z * 0.0722f, 1e-3f) * moonShare_;
				// (The scene's own night light is a strong one, a twenty-fourth of its sun, where a full moon gives
				// a four hundred thousandth: used as it is, the night was an overcast afternoon. The user's wish is a
				// real night: the moon gives a sixth of the scene's night light. What that light meant, how
				// bright the scene's lamps are against it, is kept by the exposure: see UploadFrameData.)
				// (0.04 at first: with no lamp near, the garage was black. Tried through the exposure setting: two stops
				// more is a night one can still see the yard in.)
				strength *= 0.16f * settings.moonlight; // (the user found 0.16 still too bright: the setting's default is half)
			}
			moonStrength_ = strength;
		}
		const Vec3 through = AtmosphereTransmittance(sunDirection_.y);
		sunColor_ = {through.x * tint.x * strength, through.y * tint.y * strength, through.z * tint.z * strength};
		hasSun_ = sunDirection_.y > -0.02f && (sunColor_.x + sunColor_.y + sunColor_.z) > 0.0f;
		if (!hasSun_) {
			sunColor_ = {0, 0, 0};
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
		float range = l.range;
		if ((l.flags & kAeLightHeadlamp) && l.type == kAeLightSpot) {
			// a head lamp: the shader shapes the beam itself (a low beam's pattern), inside a wide cone and as far
			// as a low beam reaches
			d.typeFlags[2] = 1.0f;
			d.directionOuter[3] = 0.5f; // cos 60 degrees
			d.colorInner[3] = 0.6f;
			range = std::max(range, 90.0f);
			d.positionRange[3] = range;
		}
		lightData_.resize(size_t(punctualCount_ + 1) * sizeof(LightData));
		memcpy(lightData_.data() + size_t(punctualCount_) * sizeof(LightData), &d, sizeof(d));
		viewLights.push_back({TransformPoint(view, {l.position[0], l.position[1], l.position[2]}), range});
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
	lensFlare_ = settings.lensFlare && settings.lensFlareStrength > 0.0f && target.width >= 64 && target.height >= 64;
	lensFlareStrength_ = settings.lensFlareStrength;
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
	fc.ssrParams[0] = ssr_ && historyValid_ && history_[historyIndex_] && history_[historyIndex_]->desc.width == target.width && history_[historyIndex_]->desc.height == target.height ? 1.0f : 0.0f;
	fc.ssrParams[1] = f.ssrMinSmoothness;
	fc.ssrParams[2] = std::max(f.ssrSmoothnessFadeStart, f.ssrMinSmoothness);
	fc.ssrParams[3] = f.ssrScreenFadeDistance;
	fc.qualityParams[0] = settings.shadowSoftness;
	// volumetric fog: where there is fog and a sun to light it
	volumetricFog_ = settings.volumetricFog && f.fogDensity * settings.fogStrength > 0.0f;
	fc.qualityParams[1] = volumetricFog_ ? 1.0f : 0.0f;
	fc.qualityParams[3] = rayReflections_ ? 1.0f : 0.0f; // (smooth surfaces mirror what rays through the scene find)
	// bounced light: read from the picture of the frame before, so only with the temporal history
	// ---- reflection probes: the faces' directions (the same for every probe) and where the usable probes stand
	if (reflection_) {
		static const Vec3 kForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		static const Vec3 kUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
		const Mat4 faceProj = PerspectiveReversedZ(3.14159265f * 0.5f, 1.0f, kReflectionNear, kReflectionFar);
		for (int i = 0; i < 6; ++i) {
			const Mat4 inv = Inverse(faceProj * LookAlong({0, 0, 0}, kForward[i], kUp[i]));
			// (the tile's centre, right edge and top edge as directions: what the lookups turn a direction into a place with)
			const Vec3 c = TransformProjective(inv, {0, 0, 0.5f}), r = TransformProjective(inv, {1, 0, 0.5f}), u = TransformProjective(inv, {0, 1, 0.5f});
			const Vec3 basis[3] = {c, r - c, u - c};
			fc.reflectionBasis[i][0][3] = kReflectionNear; // (the cubes' depth range, for reading distances from their depth)
			fc.reflectionBasis[i][1][3] = kReflectionFar;
			for (int k = 0; k < 3; ++k) {
				fc.reflectionBasis[i][k][0] = basis[k].x;
				fc.reflectionBasis[i][k][1] = basis[k].y;
				fc.reflectionBasis[i][k][2] = basis[k].z;
			}
		}
		fc.probeGrid[0] = kProbeCell;
		fc.probeGrid[1] = kProbeCellHeight;
		fc.probeGrid[2] = float(frame_ % 1024) * 0.61803f;
		fc.probeGrid[3] = 1.0f;
		for (uint32_t s = 0; s < kProbeSlots; ++s) {
			const LightProbe& p = lightProbes_[s];
			fc.probePositions[s][0] = p.position.x;
			fc.probePositions[s][1] = p.position.y;
			fc.probePositions[s][2] = p.position.z;
			// (usable: every face drawn, not counting the ones being drawn this frame for the first time)
			uint32_t done = p.facesDone;
			for (uint32_t j = 0; j < probeJobCount_; ++j) {
				if (probeJobs_[j].slot == s && p.lastDrawn == frame_ && (probeJobs_[j].cb & 1ull) != 0) {
					done &= ~(1u << probeJobs_[j].face);
				}
			}
			// (a probe that has just become whole comes in over half a second, not at once)
			fc.probePositions[s][3] = p.used && done == 0x3Fu && p.wholeSince ? std::min(float(frame_ - p.wholeSince) / 30.0f, 1.0f) : 0.0f;
		}
	}
	// ---- probe grid GI: the volume follows the camera a whole cell at a time (the probes stay where they are in the
	// world, and each keeps its place in the textures: its cell modulo the counts)
	gi_ = bvhReady_ && settings.probeGi;
	if (gi_ && !giRays_) {
		GpuTextureDesc g;
		g.width = uint32_t(kGiCounts[0] * kGiCounts[1]) * kGiTile;
		g.height = uint32_t(kGiCounts[2]) * kGiTile;
		g.format = g.targetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		g.renderTarget = true;
		giRays_ = gpu.CreateTexture(g, "Gi.Rays", ResourceCategory::RenderTarget, Lifetime::Persistent, frame_);
		giIrradiance_ = gpu.CreateTexture(g, "Gi.Irradiance", ResourceCategory::RenderTarget, Lifetime::Persistent, frame_);
		giDepth_ = gpu.CreateTexture(g, "Gi.Distance", ResourceCategory::RenderTarget, Lifetime::Persistent, frame_);
		giHistory_ = false;
	}
	gi_ = gi_ && giRays_ && giIrradiance_ && giDepth_;
	if (gi_) {
		const float spacing[3] = {kGiSpacing, kGiSpacingY, kGiSpacing};
		for (int k = 0; k < 3; ++k) {
			fc.giPrev[k] = float(giMin_[k]);
			// (a probe stands in the middle of its cell: the camera between the two middle probes)
			giMin_[k] = int(std::floor(scene.frame.cameraPosition[k] / spacing[k] - 0.5f)) - kGiCounts[k] / 2 + 1;
			fc.giGrid[k] = float(giMin_[k]);
		}
		fc.giGrid[3] = 1.0f;
		fc.giPrev[3] = giHistory_ ? 1.0f : 0.0f;
		fc.giParams[0] = kGiSpacing;
		fc.giParams[1] = kGiSpacingY;
		fc.giParams[2] = float(frame_ % 4096);
		giHistory_ = true;
	} else {
		giHistory_ = false;
	}
	fc.giParams[3] = float(punctualCount_); // (also read by the particles: with or without the probes)
	indirect_ = settings.indirectLight && settings.ssao && taa_ && historyValid_;
	fc.qualityParams[2] = indirect_ ? settings.indirectStrength : 0.0f;
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
	// (Aether's own strength and radius: the scene's volume is not asked any more)
	aoActive_ = settings.ssao && settings.aoStrength > 0.0f;
	fc.aoParams[0] = settings.aoStrength;
	fc.aoParams[1] = settings.aoRadius;
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
	// (Night is exposed darker than the meter asks: metered to the usual middle grey a moonlit yard looked like an
	// overcast afternoon. 1.2 stops under, tried against 2.2: dark, with the lamps as the bright things in it.)
	fc.exposure[2] = std::pow(2.0f, settings.exposureEV + f.postExposure);
	// tone mapping: 0 none, 1 ACES, 2 neutral, 3 the host's custom curve
	float tone = settings.tonemap == "aces" ? 1.0f : settings.tonemap == "neutral" ? 2.0f : settings.tonemap == "agx" ? 4.0f : 0.0f;
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
	const bool skyTexture = sky && sky->srv != UINT32_MAX;
	fc.misc[1] = skyTexture || physicalSky_ ? 1.0f : 0.0f; // (a sky there is: its texture, or the physical one)
	// (at night the air is lit by the moon: the sky's scattering at the moon's strength)
	fc.atmosphere[0] = moonShare_ > 0.0f ? moonStrength_ : atmosphereScale_;
	// what a moonless night leaves (airglow, stars): keeps the night from being black. (An eighth of the moon's unit
	// when that is what the scene's own night light sets.)
	fc.atmosphere[1] = std::max(atmosphereScale_ * 2.0e-5f, moonShare_ >= 1.0f ? moonStrength_ * 0.125f : 0.0f); // what a moonless night leaves (airglow, stars): keeps the night from being black
	fc.atmosphere[2] = std::min(std::max(f.cameraPosition[1], 0.0f) * 0.001f + 0.05f, 60.0f);
	fc.atmosphere[3] = physicalSky_ ? 1.0f : 0.0f;
	if (physicalSky_) {
		// (the direction also when the sun is down: the twilight sky needs it)
		fc.sunDirection[0] = sunDirection_.x;
		fc.sunDirection[1] = sunDirection_.y;
		fc.sunDirection[2] = sunDirection_.z;
		// the scene's exposure range is made for its own sky: evenings and nights need far more room
		fc.exposureParams[0] -= 14.0f;
		if (moonShare_ > 0.0f && sceneSunLum_ > 0.0f) {
			// Night: the meter must not open up until the dark is a grey afternoon (it did, and every lamp was a
			// glare in it). It stops where a matt grey surface in the scene's own light would be a middle grey:
			// at the scene's night light when the scene is at night (its lamps are set against that), eight stops
			// under its daylight otherwise. What the moon lights is then some stops darker than that, as at night.
			const bool sceneNight = settings.followSceneNight && sceneNight_.load();
			const float reference = std::log2(std::max(sceneSunLum_ * 0.2f / 3.14159265f * 8.0f, 1e-6f));
			const float floorEv = sceneNight ? reference - 0.5f : reference - 8.0f;
			fc.exposureParams[0] = std::max(fc.exposureParams[0], fc.exposureParams[0] * (1.0f - moonShare_) + floorEv * moonShare_);
			fc.exposureParams[1] = std::max(fc.exposureParams[1], fc.exposureParams[0]);
		}
		GpuTextureDesc d;
		d.format = d.targetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		d.renderTarget = true;
		if (!transmittanceLut_) {
			d.width = 256;
			d.height = 64;
			transmittanceLut_ = gpu.CreateTexture(d, "Sky.Transmittance", ResourceCategory::RenderTarget, Lifetime::Persistent, frame_);
			atmosphereLutsValid_ = false;
		}
		if (!multiScatterLut_) {
			d.width = d.height = 32;
			multiScatterLut_ = gpu.CreateTexture(d, "Sky.MultipleScattering", ResourceCategory::RenderTarget, Lifetime::Persistent, frame_);
			atmosphereLutsValid_ = false;
		}
		if (!skyViewLut_) {
			d.width = 192;
			d.height = 108;
			skyViewLut_ = gpu.CreateTexture(d, "Sky.View", ResourceCategory::RenderTarget, Lifetime::Persistent, frame_);
		}
		if (!cloudNoise_) {
			GpuTextureDesc n;
			n.width = n.height = 1024; // 64 slices of 128 x 128, laid out 8 x 8: a noise volume (aether_clouds.hlsli)
			n.format = n.targetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			n.renderTarget = true;
			cloudNoise_ = gpu.CreateTexture(n, "Sky.CloudNoise", ResourceCategory::RenderTarget, Lifetime::Persistent, frame_);
			atmosphereLutsValid_ = false;
		}
		if (!skyCloudsLut_) {
			d.width = 384;
			d.height = 216;
			skyCloudsLut_ = gpu.CreateTexture(d, "Sky.WithClouds", ResourceCategory::RenderTarget, Lifetime::Persistent, frame_);
		}
		if (!transmittanceLut_ || !multiScatterLut_ || !skyViewLut_ || !cloudNoise_ || !skyCloudsLut_) {
			physicalSky_ = false;
			fc.atmosphere[3] = 0.0f;
			fc.misc[1] = skyTexture ? 1.0f : 0.0f;
		}
	}
	// clouds: with the physical sky, where the setting asks for any (set here: the constants are uploaded right below)
	clouds_ = physicalSky_ && settings.cloudCoverage > 0.01f;
	{
		const auto now = std::chrono::steady_clock::now();
		cloudDrift_ += std::min(std::chrono::duration<float>(now - cloudClock_).count(), 0.25f) * settings.cloudSpeed * 0.0012f;
		cloudDrift_ -= std::floor(cloudDrift_ / 64.0f) * 64.0f;
		cloudClock_ = now;
	}
	fc.clouds[0] = clouds_ ? settings.cloudCoverage : 0.0f;
	fc.clouds[1] = cloudDrift_;
	fc.clouds[2] = cloudDrift_ * 0.37f;
	fc.clouds[3] = clouds_ ? 1.0f : 0.0f;
	frameSky_ = fc.misc[1] > 0.5f;
	fc.misc[2] = sky ? float(std::max<uint32_t>(sky->desc.mipCount, 1)) : 1.0f;
	fc.misc[3] = f.ambientIntensity;
	fc.skyParams[0] = f.skyIntensity > 0 ? f.skyIntensity : 1.0f;
	fc.skyParams[1] = std::cos(f.skyRotation);
	fc.skyParams[2] = std::sin(f.skyRotation);
	const bool fogFromSky = (f.fogFlags & kAeFogColorFromSky) != 0;
	memcpy(fc.fog, fogFromSky ? f.fogTint : f.fogColor, sizeof(f.fogColor));
	fc.fog[3] = f.fogDensity * settings.fogStrength;
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
	// (a probe's face is drawn with the frame's constants, seen from the probe: no screen space inputs, no clusters,
	// fog per surface, and no probes: they are what is being drawn)
	for (uint32_t j = 0; j < probeJobCount_; ++j) {
		static const Vec3 kForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		static const Vec3 kUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
		ProbeFaceJob& job = probeJobs_[j];
		FrameConstants face = fc;
		const Vec3 eye = lightProbes_[job.slot].position;
		face.cameraPos[0] = eye.x;
		face.cameraPos[1] = eye.y;
		face.cameraPos[2] = eye.z;
		const Mat4 faceView = LookAlong(eye, kForward[job.face], kUp[job.face]);
		const Mat4 faceProj = PerspectiveReversedZ(3.14159265f * 0.5f, 1.0f, kReflectionNear, kReflectionFar);
		const Mat4 faceViewProj = faceProj * faceView;
		memcpy(face.view, faceView.m, sizeof(face.view));
		memcpy(face.proj, faceProj.m, sizeof(face.proj));
		memcpy(face.viewProj, faceViewProj.m, sizeof(face.viewProj));
		memcpy(face.invViewProj, Inverse(faceViewProj).m, sizeof(face.invViewProj));
		memcpy(face.prevViewProj, faceViewProj.m, sizeof(face.prevViewProj));
		face.screenSize[0] = face.screenSize[1] = float(kProbeFace);
		face.screenSize[2] = face.screenSize[3] = 1.0f / float(kProbeFace);
		face.jitter[0] = face.jitter[1] = face.jitter[2] = 0;
		face.clusterCounts[0] = 0;
		face.aoParams[3] = 0;
		face.qualityParams[1] = 0;
		face.qualityParams[2] = 0;
		face.misc[0] = 0;
		face.ssrParams[0] = 0;
		face.probeGrid[3] = 0;
		face.probeCount[0] = 0;
		FrustumPlanes(faceViewProj, reinterpret_cast<Plane*>(job.planes));
		job.cb = upload(&face, sizeof(face), 256);
		if (!job.cb) {
			probeJobCount_ = 0;
		}
	}
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
	rayMaterialsVa_ = upload(rayMaterials_.data(), rayMaterials_.size() * sizeof(uint32_t), 16);
	lightsVa_ = upload(lightData_.data(), lightData_.size(), 16);
	gridVa_ = upload(clusterGrid_.data(), clusterGrid_.size() * sizeof(uint32_t), 16);
	indicesVa_ = upload(lightIndices_.data(), lightIndices_.size() * sizeof(uint32_t), 16);
	// the instance map is written while the passes are recorded: room for every list once per pass that draws it
	instanceMapCapacity_ = uint32_t(opaque_.size() * 2 + transparent_.size() + decals_.size() + casters_.size() * 4 + probeItems_.size() * 2 + 4);
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
			// (not with the physical sky: the scene's probes were baked with the scene's own sky and sun in them)
			SceneTexture* texture = settings.sky && !physicalSky_ ? store.Texture(p.texture) : nullptr;
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
	gpu.CopySrv(globalsTable_ + 1, skyTexture ? sky->srv : nullCubeSrv_);
	gpu.CopySrv(globalsTable_ + 4, physicalSky_ ? (clouds_ ? skyCloudsLut_->srv : skyViewLut_->srv) : nullTextureSrv_);
	gpu.CopySrv(globalsTable_ + 5, physicalSky_ ? transmittanceLut_->srv : nullTextureSrv_);
	gpu.CopySrv(globalsTable_ + 6, physicalSky_ ? multiScatterLut_->srv : nullTextureSrv_);
	gpu.CopySrv(globalsTable_ + 7, physicalSky_ ? cloudNoise_->srv : nullTextureSrv_);
	gpu.CopySrv(globalsTable_ + 8, reflection_ && reflectionAtlas_[0] ? reflectionAtlas_[0]->srv : nullTextureSrv_);
	gpu.CopySrv(globalsTable_ + 9, reflection_ && reflectionAtlas_[1] ? reflectionAtlas_[1]->srv : nullTextureSrv_);
	gpu.CopySrv(globalsTable_ + 10, reflection_ && reflectionDepth_ ? reflectionDepth_->srv : nullTextureSrv_);
	gpu.CopySrv(globalsTable_ + 11, gi_ ? giIrradiance_->srv : nullTextureSrv_);
	gpu.CopySrv(globalsTable_ + 12, gi_ ? giDepth_->srv : nullTextureSrv_);
	// (the table the cube's own faces are drawn with: the same, without the cube and without screen space occlusion)
	probeGlobalsTable_ = reflection_ ? gpu.AllocSrv(kGlobalsTableSize) : UINT32_MAX;
	if (probeGlobalsTable_ != UINT32_MAX) {
		gpu.CopySrv(probeGlobalsTable_, shadowAtlas_ ? shadowAtlas_->srv : nullTextureSrv_);
		gpu.CopySrv(probeGlobalsTable_ + 1, skyTexture ? sky->srv : nullCubeSrv_);
		gpu.CopySrv(probeGlobalsTable_ + 2, irradiance_ ? irradiance_->srv : nullTextureSrv_);
		gpu.CopySrv(probeGlobalsTable_ + 3, white_->srv);
		gpu.CopySrv(probeGlobalsTable_ + 4, physicalSky_ ? (clouds_ ? skyCloudsLut_->srv : skyViewLut_->srv) : nullTextureSrv_);
		gpu.CopySrv(probeGlobalsTable_ + 5, physicalSky_ ? transmittanceLut_->srv : nullTextureSrv_);
		gpu.CopySrv(probeGlobalsTable_ + 6, physicalSky_ ? multiScatterLut_->srv : nullTextureSrv_);
		gpu.CopySrv(probeGlobalsTable_ + 7, physicalSky_ ? cloudNoise_->srv : nullTextureSrv_);
		for (uint32_t i = 0; i < 3; ++i) {
			gpu.CopySrv(probeGlobalsTable_ + 8 + i, nullTextureSrv_);
		}
		gpu.CopySrv(probeGlobalsTable_ + 11, gi_ ? giIrradiance_->srv : nullTextureSrv_);
		gpu.CopySrv(probeGlobalsTable_ + 12, gi_ ? giDepth_->srv : nullTextureSrv_);
	} else {
		reflection_ = false;
	}
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
	// (always something valid in the slots: only passes that run when bvhReady_ read them)
	SceneBvh& bvh = SceneBvh::Get();
	list->SetGraphicsRootShaderResourceView(12, bvhReady_ ? bvh.NodesVa() : instancesVa_);
	list->SetGraphicsRootShaderResourceView(13, bvhReady_ ? bvh.TrisVa() : instancesVa_);
	list->SetGraphicsRootShaderResourceView(14, bvhReady_ ? bvh.TlasNodesVa() : instancesVa_);
	list->SetGraphicsRootShaderResourceView(15, bvhReady_ ? bvh.TlasInstancesVa() : instancesVa_);
	list->SetGraphicsRootShaderResourceView(16, bvhReady_ && rayMaterialsVa_ ? rayMaterialsVa_ : instancesVa_);
	list->SetGraphicsRootDescriptorTable(17, gpu.SrvGpu(0));
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
		const uint32_t constants[4] = {batch.start, item.materialIndex, uint32_t(std::max(cascade, 0)), (item.mirrored ? 1u : 0u) | (opaqueKind == kForward || opaqueKind == kForwardProbe ? 0u : 2u)}; // bit 1: no screen space occlusion (blended passes)
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
		if (cullPlanes_ && !SphereVisible(reinterpret_cast<const Plane*>(cullPlanes_), item.center, item.radius)) {
			continue;
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
	// ---- reflection probes: their textures (made once), then which places want a probe and which faces are drawn now
	reflection_ = settings.reflectionProbe;
	for (int i = 0; i < 2 && reflection_; ++i) {
		if (!reflectionAtlas_[i]) {
			const uint32_t tile = i == 0 ? kProbeFace : kProbeBlurFace;
			GpuTextureDesc d;
			d.width = tile * 12;
			d.height = tile * 4;
			d.format = d.targetFormat = kReflectionFormat;
			d.renderTarget = true;
			reflectionAtlas_[i] = gpu.CreateTexture(d, i == 0 ? "Scene.ReflectionProbes" : "Scene.ReflectionProbes.Blur", ResourceCategory::RenderTarget, Lifetime::Persistent, frame);
			for (LightProbe& p : lightProbes_) {
				p = LightProbe();
			}
		}
		reflection_ = reflectionAtlas_[i] != nullptr;
	}
	if (reflection_ && !reflectionDepth_) {
		GpuTextureDesc d;
		d.width = kProbeFace * 12;
		d.height = kProbeFace * 4;
		d.format = DXGI_FORMAT_R32_TYPELESS;
		d.srvFormat = DXGI_FORMAT_R32_FLOAT;
		d.targetFormat = DXGI_FORMAT_D32_FLOAT;
		d.depthStencil = true;
		d.clearDepth = 0.0f;
		reflectionDepth_ = gpu.CreateTexture(d, "Scene.ReflectionProbes.Depth", ResourceCategory::DepthBuffer, Lifetime::Persistent, frame);
		reflection_ = reflectionDepth_ != nullptr;
	}
	probeJobCount_ = 0;
	if (reflection_) {
		UpdateProbes(scene, frame);
	}
	// ---- ray tracing: the trees that arrived; the tree over this frame's instances is filled while the draw lists are
	// built (the same walk over the instances) and built after it
	raysWanted_ = settings.rayTracedReflections || settings.probeGi || settings.debugView == "bvh" || settings.debugView == "bvhCost";
	SceneBvh::Get().BeginFrame(device_, list, retireFence, frame);
	SceneBvh::Get().BeginInstances();
	raysWanted_ = raysWanted_ && SceneBvh::Get().HasTrees();
	BuildDrawLists(scene, frame);
	bvhReady_ = raysWanted_ && SceneBvh::Get().EndInstances();
	rayReflections_ = bvhReady_ && settings.rayTracedReflections;
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
	// ---- the physical sky's LUTs: transmittance and multiple scattering once, the sky around the camera every frame
	if (physicalSky_) {
		const RenderGraph::ResourceId transmittance = graph_.Import(transmittanceLut_), multiScatter = graph_.Import(multiScatterLut_), skyView = graph_.Import(skyViewLut_);
		const RenderGraph::ResourceId cloudNoise = graph_.Import(cloudNoise_), skyClouds = graph_.Import(skyCloudsLut_);
		if (!atmosphereLutsValid_) {
			graph_.AddPass("SkyTransmittance", true, [&, transmittance](RenderGraph::Builder& b) { b.Write(transmittance, D3D12_RESOURCE_STATE_RENDER_TARGET); }, [&, transmittance](RenderGraph::PassContext& ctx) {
				Fullscreen(ctx, kFullscreenTransmittanceLut, ctx.Texture(transmittance), nullptr);
			});
			graph_.AddPass("SkyMultipleScattering", true, [&, transmittance, multiScatter](RenderGraph::Builder& b) {
				b.Read(transmittance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				b.Write(multiScatter, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}, [&, multiScatter](RenderGraph::PassContext& ctx) {
				Fullscreen(ctx, kFullscreenMultiScatterLut, ctx.Texture(multiScatter), nullptr);
			});
			graph_.AddPass("CloudNoise", true, [&, cloudNoise](RenderGraph::Builder& b) { b.Write(cloudNoise, D3D12_RESOURCE_STATE_RENDER_TARGET); }, [&, cloudNoise](RenderGraph::PassContext& ctx) {
				Fullscreen(ctx, kFullscreenCloudNoise, ctx.Texture(cloudNoise), nullptr);
			});
			atmosphereLutsValid_ = true;
		}
		graph_.AddPass("SkyView", true, [&, transmittance, multiScatter, skyView](RenderGraph::Builder& b) {
			b.Read(transmittance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(multiScatter, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(skyView, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, skyView](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenSkyView, ctx.Texture(skyView), nullptr);
		});
		// the sky with the clouds in it (for reflections and the sky's diffuse light): the air's sky, then the clouds over it
		if (clouds_) {
			graph_.AddPass("SkyClouds", true, [&, transmittance, multiScatter, skyView, cloudNoise, skyClouds](RenderGraph::Builder& b) {
				b.Read(transmittance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				b.Read(multiScatter, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				b.Read(skyView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				b.Read(cloudNoise, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				b.Write(skyClouds, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}, [&, skyView, skyClouds](RenderGraph::PassContext& ctx) {
				Fullscreen(ctx, kFullscreenSkyClouds, ctx.Texture(skyClouds), ctx.Texture(skyView));
			});
		}
		// (every later pass reads these through the globals table: this puts them into the state for that)
		graph_.AddPass("SkyViewReady", true, [&, transmittance, multiScatter, skyView, cloudNoise, skyClouds](RenderGraph::Builder& b) {
			b.Read(transmittance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(multiScatter, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(skyView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(cloudNoise, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(skyClouds, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}, [](RenderGraph::PassContext&) {});
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
	aoDesc.format = aoDesc.targetFormat = kOcclusionFormat;
	aoDesc.renderTarget = true;
	aoDesc.clearColor[0] = aoDesc.clearColor[1] = aoDesc.clearColor[2] = aoDesc.clearColor[3] = 1.0f;
	const bool ao = aoActive_;
	const RenderGraph::ResourceId aoRaw = ao ? graph_.CreateTransient("SceneAO.Raw", aoDesc, ResourceCategory::RenderTarget) : RenderGraph::kInvalid;
	const RenderGraph::ResourceId aoFinal = ao ? graph_.CreateTransient("SceneAO", aoDesc, ResourceCategory::RenderTarget) : RenderGraph::kInvalid;
	auto fullscreenAo = [&, viewport, scissor](RenderGraph::PassContext& ctx, FullscreenKind kind, RenderGraph::ResourceId output, uint32_t table) {
		ID3D12PipelineState* pso = FullscreenPipeline(kind, kOcclusionFormat);
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
	// (bounced light: the occlusion pass reads the picture of the frame before where its samples land)
	const bool bounce = ao && indirect_ && history_[historyIndex_] && history_[historyIndex_]->desc.width == target.width && history_[historyIndex_]->desc.height == target.height;
	const RenderGraph::ResourceId bounceSource = bounce ? graph_.Import(history_[historyIndex_]) : RenderGraph::kInvalid;
	graph_.AddPass("AmbientOcclusion", ao, [&, bounce, bounceSource](RenderGraph::Builder& b) {
		b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (bounce) {
			b.Read(bounceSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		b.Write(aoRaw, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}, [&, depth, aoRaw, fullscreenAo, bounce, bounceSource](RenderGraph::PassContext& ctx) {
		gpu.CopySrv(postTable_ + 1, ctx.Texture(depth)->srv);
		gpu.CopySrv(postTable_ + 2, bounce ? ctx.Texture(bounceSource)->srv : black_->srv);
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
	// ---- probe grid GI: every probe sends 64 rays (one per direction of its tile, moved about inside it from frame
	// to frame); what they bring back is gathered into the probes' irradiance and distances, a small step per frame
	if (gi_) {
		const RenderGraph::ResourceId giRays = graph_.Import(giRays_), giIrradiance = graph_.Import(giIrradiance_), giDepth = graph_.Import(giDepth_);
		graph_.AddPass("GiRays", true, [&, giRays, giIrradiance, giDepth](RenderGraph::Builder& b) {
			// (a hit is lit by the probes of the frame before as well: light bounces on, a bounce per frame)
			b.Read(giIrradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(giDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (shadow != RenderGraph::kInvalid) {
				b.Read(shadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			if (skyLight) {
				b.Read(irradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			b.Write(giRays, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, giRays](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenGiRays, ctx.Texture(giRays), nullptr);
			ctx.note = Format("%d probes, %u rays each", kGiCounts[0] * kGiCounts[1] * kGiCounts[2], kGiTile * kGiTile);
		});
		graph_.AddPass("GiIrradiance", true, [&, giRays, giIrradiance](RenderGraph::Builder& b) {
			b.Read(giRays, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(giIrradiance, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, giRays, giIrradiance](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenGiIrradiance, ctx.Texture(giIrradiance), ctx.Texture(giRays));
		});
		graph_.AddPass("GiDistance", true, [&, giRays, giDepth](RenderGraph::Builder& b) {
			b.Read(giRays, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(giDepth, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, giRays, giDepth](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenGiDepth, ctx.Texture(giDepth), ctx.Texture(giRays));
		});
		graph_.AddPass("GiReady", true, [&, giIrradiance, giDepth](RenderGraph::Builder& b) {
			b.Read(giIrradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(giDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}, [](RenderGraph::PassContext&) {});
	}
	// ---- reflection probes: this frame's faces, then the blurred copy of the atlas
	RenderGraph::ResourceId reflectionId[3] = {RenderGraph::kInvalid, RenderGraph::kInvalid, RenderGraph::kInvalid};
	const RenderGraph::ResourceId reflectionDepth = reflection_ ? graph_.Import(reflectionDepth_) : RenderGraph::kInvalid;
	if (reflection_) {
		for (int i = 0; i < 2; ++i) {
			reflectionId[i] = graph_.Import(reflectionAtlas_[i]);
		}
		const RenderGraph::ResourceId sharp = reflectionId[0], blurred = reflectionId[1];
		graph_.AddPass("ReflectionProbes", probeJobCount_ > 0, [&, sharp, reflectionDepth](RenderGraph::Builder& b) {
			b.Write(sharp, D3D12_RESOURCE_STATE_RENDER_TARGET);
			b.Write(reflectionDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			if (shadow != RenderGraph::kInvalid) {
				b.Read(shadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			if (skyLight) {
				b.Read(irradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
		}, [&, sharp, reflectionDepth](RenderGraph::PassContext& ctx) {
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.RtvCpu(ctx.Texture(sharp)->target), dsv = gpu.DsvCpu(ctx.Texture(reflectionDepth)->target);
			ctx.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
			BindCommon(ctx.list, true);
			ctx.list->SetGraphicsRootDescriptorTable(8, gpu.SrvGpu(probeGlobalsTable_));
			const LONG size = LONG(kProbeFace);
			std::string note;
			for (uint32_t j = 0; j < probeJobCount_; ++j) {
				const ProbeFaceJob& job = probeJobs_[j];
				// (only this face's tile is cleared: the atlas's other tiles are other faces and other probes)
				const LONG x = LONG((job.slot % 4) * 3 + job.face % 3) * size, y = LONG((job.slot / 4) * 2 + job.face / 3) * size;
				const D3D12_RECT rect = {x, y, x + size, y + size};
				const D3D12_VIEWPORT vp = {float(x), float(y), float(size), float(size), 0, 1};
				const float clear[4] = {0, 0, 0, 0};
				ctx.list->ClearRenderTargetView(rtv, clear, 1, &rect);
				ctx.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &rect);
				ctx.list->RSSetViewports(1, &vp);
				ctx.list->RSSetScissorRects(1, &rect);
				ctx.list->SetGraphicsRootConstantBufferView(0, job.cb);
				cullPlanes_ = job.planes;
				DrawItems(ctx, probeItems_, kForwardProbe, kForwardProbe, -1);
				note += Format("%sprobe %u face %u", j ? ", " : "", job.slot, job.face);
			}
			cullPlanes_ = nullptr;
			ctx.note = note + Format(" (%u still things near the camera)", uint32_t(probeItems_.size()));
		});
		graph_.AddPass("ReflectionProbeBlur", probeJobCount_ > 0, [&, sharp, blurred](RenderGraph::Builder& b) {
			b.Read(sharp, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(blurred, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, sharp, blurred](RenderGraph::PassContext& ctx) {
			// x: the blur's angle (radians), y: the tile size of the input, z: the tile size of the output
			const float params[4] = {0.22f, float(kProbeFace), float(kProbeBlurFace), 0};
			Fullscreen(ctx, kFullscreenReflectionBlur, ctx.Texture(blurred), ctx.Texture(sharp), nullptr, nullptr, params);
		});
	}
	graph_.AddPass("Opaque", true, [&](RenderGraph::Builder& b) {
		for (int i = 0; i < 3; ++i) {
			if (reflectionId[i] != RenderGraph::kInvalid) {
				b.Read(reflectionId[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
		}
		if (reflectionDepth != RenderGraph::kInvalid) {
			b.Read(reflectionDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); // (distances: where a mirrored ray from a surface ends)
		}
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
	const bool screenReflections = ssr_ && historyValid_ && history_[historyIndex_] && history_[historyIndex_]->desc.width == target.width && history_[historyIndex_]->desc.height == target.height;
	if (screenReflections || rayReflections_) {
		const RenderGraph::ResourceId before = screenReflections ? graph_.Import(history_[historyIndex_]) : RenderGraph::kInvalid;
		// The rays are traced for one pixel of every 2 x 2 (a different one each frame, so that the temporal antialiasing
		// gathers all four): a quarter of the rays. The reflection pass lays them over the full picture by depth.
		const float rayOffset[4] = {float(frame & 1), float((frame >> 1) & 1), 0, 0};
		RenderGraph::ResourceId rays = RenderGraph::kInvalid;
		if (rayReflections_) {
			GpuTextureDesc rayDesc = colorDesc;
			rayDesc.width = std::max((target.width + 1) / 2, 1u);
			rayDesc.height = std::max((target.height + 1) / 2, 1u);
			rayDesc.format = rayDesc.targetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // rgb what the ray brought back, a how far the surface is (0: no ray)
			rays = graph_.CreateTransient("SceneReflectionRays", rayDesc, ResourceCategory::RenderTarget);
			graph_.AddPass("ReflectionRays", true, [&, depth, surface, before, rays](RenderGraph::Builder& b) {
				if (before != RenderGraph::kInvalid) {
					b.Read(before, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				}
				b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				b.Read(surface, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				b.Write(rays, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}, [&, depth, surface, before, rays, rayOffset](RenderGraph::PassContext& ctx) {
				Fullscreen(ctx, kFullscreenRayTrace, ctx.Texture(rays), before != RenderGraph::kInvalid ? ctx.Texture(before) : nullptr, ctx.Texture(depth), ctx.Texture(surface), rayOffset);
			});
		}
		graph_.AddPass("Reflections", true, [&, color, depth, surface, before, rays](RenderGraph::Builder& b) {
			if (before != RenderGraph::kInvalid) {
				b.Read(before, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			if (rays != RenderGraph::kInvalid) {
				b.Read(rays, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(surface, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, color, depth, surface, before, rays, rayOffset](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenSsr, ctx.Texture(color), before != RenderGraph::kInvalid ? ctx.Texture(before) : nullptr, ctx.Texture(depth), ctx.Texture(surface), rayOffset,
				rays != RenderGraph::kInvalid ? ctx.Texture(rays) : nullptr);
			ctx.note = rayReflections_ ? (screenReflections ? "screen, then rays" : "rays") : "screen";
		});
	}
	// ---- the clouds as the camera sees them: marched at half resolution, laid over the sky by the sky pass
	RenderGraph::ResourceId cloudLayer = RenderGraph::kInvalid;
	const RenderGraph::ResourceId airSky = physicalSky_ ? graph_.Import(skyViewLut_) : RenderGraph::kInvalid;
	if (clouds_) {
		GpuTextureDesc cloudDesc = colorDesc;
		cloudDesc.width = std::max(target.width / 2, 1u);
		cloudDesc.height = std::max(target.height / 2, 1u);
		cloudDesc.format = cloudDesc.targetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		cloudLayer = graph_.CreateTransient("SceneClouds", cloudDesc, ResourceCategory::RenderTarget);
		graph_.AddPass("Clouds", true, [&, airSky, cloudLayer](RenderGraph::Builder& b) {
			b.Read(airSky, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(cloudLayer, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, airSky, cloudLayer](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenClouds, ctx.Texture(cloudLayer), ctx.Texture(airSky));
		});
	}
	graph_.AddPass("Sky", true, [&, airSky, cloudLayer](RenderGraph::Builder& b) {
		b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		if (airSky != RenderGraph::kInvalid) {
			b.Read(airSky, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		if (cloudLayer != RenderGraph::kInvalid) {
			b.Read(cloudLayer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	}, [&, color, depth, airSky, cloudLayer](RenderGraph::PassContext& ctx) {
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
		// (the physical sky: the air's sky in the pass's first input, the cloud layer in its third)
		if (postNext_ < kPostTables) {
			const uint32_t table = postTable_ + kPostTableSize * postNext_++;
			if (airSky != RenderGraph::kInvalid) {
				gpu.CopySrv(table, ctx.Texture(airSky)->srv);
			}
			if (cloudLayer != RenderGraph::kInvalid) {
				gpu.CopySrv(table + 2, ctx.Texture(cloudLayer)->srv);
			}
			ctx.list->SetGraphicsRootDescriptorTable(9, gpu.SrvGpu(table));
		}
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
	// ---- volumetric fog: the light the fog scatters towards the camera on the way to each pixel (the sun through the
	// shadow map and the clouds, the sky from all around), marched at half resolution, then laid over the picture
	if (volumetricFog_) {
		GpuTextureDesc fogDesc = colorDesc;
		fogDesc.width = std::max(target.width / 2, 1u);
		fogDesc.height = std::max(target.height / 2, 1u);
		fogDesc.format = fogDesc.targetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		const RenderGraph::ResourceId fog = graph_.CreateTransient("SceneFog", fogDesc, ResourceCategory::RenderTarget);
		graph_.AddPass("VolumetricFog", true, [&, depth, fog](RenderGraph::Builder& b) {
			b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (shadow != RenderGraph::kInvalid) {
				b.Read(shadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			if (skyLight) {
				b.Read(irradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			b.Write(fog, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, depth, fog](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenFog, ctx.Texture(fog), nullptr, ctx.Texture(depth));
		});
		graph_.AddPass("VolumetricFogApply", true, [&, color, depth, fog](RenderGraph::Builder& b) {
			b.Read(fog, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, color, depth, fog](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenFogApply, ctx.Texture(color), ctx.Texture(fog), ctx.Texture(depth));
		});
	}
	// ---- particles (the game's particle systems, tyre smoke): over everything drawn so far
	particlesDrawn_ = 0;
	// ---- the tyre smoke: big soft puffs that cover every pixel many times over. Drawn at half the picture's size
	// (a quarter of the pixels) into a target of their own, which is then laid over the picture. (At full size a
	// camera inside a cloud of smoke took over 3 ms more per frame at 3840 x 2054.)
	auto isSmoke = [](const AeParticleBatch& batch) { return batch.texture == 0 && (batch.flags & (kAeParticleLit | kAeParticleDisc)) == (kAeParticleLit | kAeParticleDisc) && batch.count > 0; };
	bool smokeHalf = false;
	for (const AeParticleBatch& batch : particleBatches_) {
		smokeHalf = smokeHalf || isSmoke(batch);
	}
	smokeHalf = smokeHalf && particlesVa_ != 0 && target.width >= 64 && target.height >= 64;
	if (smokeHalf) {
		GpuTextureDesc smokeDesc = colorDesc;
		smokeDesc.width = (target.width + 1) / 2;
		smokeDesc.height = (target.height + 1) / 2;
		smokeDesc.format = smokeDesc.targetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		const RenderGraph::ResourceId smoke = graph_.CreateTransient("SceneSmoke", smokeDesc, ResourceCategory::RenderTarget);
		graph_.AddPass("Smoke", true, [&, smoke, depth](RenderGraph::Builder& b) {
			b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(smoke, D3D12_RESOURCE_STATE_RENDER_TARGET);
			if (shadow != RenderGraph::kInvalid) {
				b.Read(shadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			if (skyLight) {
				b.Read(irradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
		}, [&, smoke, depth, isSmoke](RenderGraph::PassContext& ctx) {
			ID3D12PipelineState* pso = ParticlePipeline(true);
			GpuTexture* out = ctx.Texture(smoke);
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.RtvCpu(out->target);
			const float nothing[4] = {0, 0, 0, 0};
			ctx.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			ctx.list->ClearRenderTargetView(rtv, nothing, 0, nullptr);
			if (!pso) {
				ctx.note = "no pipeline";
				return;
			}
			const D3D12_VIEWPORT half = {0, 0, float(out->desc.width), float(out->desc.height), 0, 1};
			const D3D12_RECT halfRect = {0, 0, LONG(out->desc.width), LONG(out->desc.height)};
			ctx.list->RSSetViewports(1, &half);
			ctx.list->RSSetScissorRects(1, &halfRect);
			BindCommon(ctx.list, true);
			ctx.list->SetGraphicsRootShaderResourceView(2, particlesVa_);
			ctx.list->SetPipelineState(pso);
			uint32_t drawn = 0;
			for (const AeParticleBatch& batch : particleBatches_) {
				if (!isSmoke(batch) || postNext_ >= kPostTables) {
					continue;
				}
				const uint32_t table = postTable_ + kPostTableSize * postNext_++;
				gpu.CopySrv(table + 1, ctx.Texture(depth)->srv);
				ctx.list->SetGraphicsRootDescriptorTable(9, gpu.SrvGpu(table));
				const uint32_t constants[4] = {batch.first, 0, 2 /* a pixel here is two of the picture's */, batch.flags};
				ctx.list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
				ctx.list->DrawInstanced(6, batch.count, 0, 0);
				ctx.drawCalls += 1;
				ctx.triangles += batch.count * 2;
				drawn += batch.count;
			}
			particlesDrawn_ += drawn;
			ctx.note = Format("%u puffs at half size", drawn);
		});
		graph_.AddPass("SmokeApply", true, [&, smoke, color](RenderGraph::Builder& b) {
			b.Read(smoke, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, smoke, color](RenderGraph::PassContext& ctx) {
			Fullscreen(ctx, kFullscreenSmokeApply, ctx.Texture(color), ctx.Texture(smoke));
		});
	}
	graph_.AddPass("Particles", particlesVa_ != 0 && !particleBatches_.empty(), [&, color, depth](RenderGraph::Builder& b) {
		b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (shadow != RenderGraph::kInvalid) {
			b.Read(shadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		if (skyLight) {
			b.Read(irradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	}, [&, color, depth, smokeHalf, isSmoke](RenderGraph::PassContext& ctx) {
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
			if (batch.count == 0 || postNext_ >= kPostTables || (smokeHalf && isSmoke(batch))) {
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
	// ---- debug views of the ray tracing structure: the scene as rays from the camera find it (its geometry, or what each ray cost)
	const bool bvhView = settings.debugView == "bvh" || settings.debugView == "bvhCost";
	graph_.AddPass("BvhDebug", bvhView && bvhReady_, [&, color](RenderGraph::Builder& b) { b.Write(color, D3D12_RESOURCE_STATE_RENDER_TARGET); }, [&, color](RenderGraph::PassContext& ctx) {
		const float params[4] = {settings.debugView == "bvhCost" ? 1.0f : 0.0f, 0, 0, 0};
		Fullscreen(ctx, kFullscreenBvhDebug, ctx.Texture(color), nullptr, nullptr, nullptr, params);
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
	RenderGraph::ResourceId flareSource = RenderGraph::kInvalid; // (the picture at an eighth of its size, blurred: what the lens flare's ghosts are made of)
	if ((bloomMix_ > 0.0f || lensFlare_) && target.width >= 64 && target.height >= 64) {
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
		if (bloomMix_ > 0.0f) {
			bloom = up[0];
		}
		flareSource = down[2];
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
	// ---- lens flare: what a camera's lens adds round the sun and bright lamps, at half the picture's size; the
	// composite adds it (after the temporal history was taken: it must not be carried into the next frame)
	RenderGraph::ResourceId flare = RenderGraph::kInvalid;
	if (lensFlare_ && flareSource != RenderGraph::kInvalid) {
		GpuTextureDesc d = colorDesc;
		d.width = std::max(1u, target.width / 2);
		d.height = std::max(1u, target.height / 2);
		flare = graph_.CreateTransient("LensFlare", d, ResourceCategory::RenderTarget);
		graph_.AddPass("LensFlare", true, [&, flare, flareSource, ownExposure, exposureTarget](RenderGraph::Builder& b) {
			b.Read(flareSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Read(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (ownExposure) {
				b.Read(exposureTarget, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			b.Write(flare, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}, [&, flare, flareSource, ownExposure, exposureTarget](RenderGraph::PassContext& ctx) {
			GpuTexture* out = ctx.Texture(flare);
			const float params[4] = {lensFlareStrength_, 0, 1.0f / float(out->desc.width), 1.0f / float(out->desc.height)};
			Fullscreen(ctx, kFullscreenLensFlare, out, ctx.Texture(flareSource), ctx.Texture(depth), ownExposure ? ctx.Texture(exposureTarget) : nullptr, params);
		});
	}
	graph_.AddPass("Composite", true, [&, post, bloom, flare](RenderGraph::Builder& b) {
		b.Read(post, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (ownExposure) {
			b.Read(exposureTarget, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		if (bloom != RenderGraph::kInvalid) {
			b.Read(bloom, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		if (flare != RenderGraph::kInvalid) {
			b.Read(flare, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		b.Write(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}, [&, post, bloom, flare, ownExposure, exposureTarget](RenderGraph::PassContext& ctx) {
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
		if (flare != RenderGraph::kInvalid) {
			gpu.CopySrv(compositeTable + 1, ctx.Texture(flare)->srv);
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
		{"sky", {{"sceneNight", sceneNight_.load()}, {"moonShare", moonShare_}, {"sceneSunLuminance", sceneSunLum_}, {"lightLuminance", sunColor_.x * 0.2126f + sunColor_.y * 0.7152f + sunColor_.z * 0.0722f}, {"atmosphereScale", atmosphereScale_}}},
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
	if (probeGlobalsTable_ != UINT32_MAX) {
		gpu.FreeSrv(probeGlobalsTable_, kGlobalsTableSize, fence);
		probeGlobalsTable_ = UINT32_MAX;
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
	transmittanceLut_.reset();
	multiScatterLut_.reset();
	skyViewLut_.reset();
	cloudNoise_.reset();
	giRays_.reset();
	giIrradiance_.reset();
	giDepth_.reset();
	gi_ = giHistory_ = false;
	skyCloudsLut_.reset();
	for (auto& t : reflectionAtlas_) {
		t.reset();
	}
	for (LightProbe& p : lightProbes_) {
		p = LightProbe();
	}
	probeCameraValid_ = false;
	SceneBvh::Get().Release(leak);
	bvhReady_ = false;
	reflectionDepth_.reset();
	probeGlobalsTable_ = UINT32_MAX;
	atmosphereLutsValid_ = false;
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
