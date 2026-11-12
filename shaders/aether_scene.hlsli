// Aether scene shaders: shared declarations (Shader Model 5.1 baseline, no optional features).
// Layouts mirror src/Aether.Native/Renderer/SceneRenderer.cpp (FrameConstants, InstanceData, MaterialData, LightData).
// Conventions: docs/architecture/scene-data-contract.md (Unity world space, column vectors: mul(M, v), reversed Z).
#ifndef AETHER_SCENE_HLSLI
#define AETHER_SCENE_HLSLI

#define AE_MAX_CASCADES 4
#define PI 3.14159265359

cbuffer FrameConstants : register(b0) {
	float4x4 g_View;
	float4x4 g_Proj;
	float4x4 g_ViewProj;
	float4x4 g_InvViewProj;
	float4x4 g_ShadowViewProj[AE_MAX_CASCADES]; // world -> the cascade's clip space (shadow pass)
	float4x4 g_ShadowMatrix[AE_MAX_CASCADES];   // world -> atlas uv (xy), depth (z)
	float4 g_CascadeSplits;   // view depth where each cascade ends
	float4 g_ShadowParams;    // x cascade count, y 1 / atlas size, z depth bias, w normal offset (world units per cascade texel)
	float4 g_CameraPos;       // xyz, w time
	float4 g_ScreenSize;      // w, h, 1/w, 1/h
	float4 g_ClusterCounts;   // x, y, z counts, w the sun's rendering layers (asuint)
	float4 g_ClusterZ;        // near, far, z count / log(far / near), unused
	float4 g_SunDirection;    // xyz: towards the sun, w: 1 if there is a sun
	float4 g_SunColor;        // rgb (lux), w: 1 if it casts shadows
	float4 g_SH[7];           // Unity's SHAr, SHAg, SHAb, SHBr, SHBg, SHBb, SHC
	float4 g_Exposure;        // x fallback multiplier, y 1 = read the exposure texture, z bias multiplier (2^EV), w tone map (0 none, 1 ACES, 2 neutral)
	float4 g_Misc;            // x debug view, y sky texture present, z sky mip count, w ambient intensity
	float4 g_Fog;             // rgb colour, a density
	float4 g_OutputRect;      // composite: left, top, right, bottom in pixels of the back buffer
	float4 g_ToneCurve;       // the host's custom tone curve: 1 / white point, x0, x1
	float4 g_ToneSegA[3];     // toe, linear, shoulder: offset x, offset y, scale x, scale y
	float4 g_ToneSegB[3];     // ln A, B
	float4 g_FogHeight;       // base height, 1 / scale height, max distance (0 = none), 1 = colour from the sky (g_Fog.rgb is then the tint)
	float4 g_CascadeTexel;    // world size of one shadow texel, per cascade
	float4 g_AOParams;        // intensity, radius (m), strength on direct light, 1 = g_AO is valid
	float4 g_Smh[3];          // grading: shadows, midtones, highlights multipliers
	float4 g_SmhLimits;       // shadows start / end, highlights start / end (all 0 = off)
	float4 g_ExposureParams;  // Aether's own metering: min EV100, max EV100, compensation
	float4 g_ProbeCount;      // x: number of reflection probes this frame
	float4 g_ProbeRows[12];   // per probe three rows: world -> the probe's influence space
	float4 g_ProbeExtents[4]; // xyz half size (sphere: radius in x), w blend distance
	float4 g_ProbeCapture[4]; // xyz capture position, w intensity
	float4 g_ProbeInfo[4];    // x flags (1 box projection, 2 sphere), y mip count
	float4 g_SkyParams;       // x intensity, y cos, z sin of the sky's rotation about the up axis
	float4x4 g_PrevViewProj;  // the frame before: world -> clip, without jitter
	float4 g_Jitter;          // xy this frame's sub pixel offset in clip space, z 1 = the history is valid
	float4 g_PostParams;      // x bloom mix (0 = off), y bloom scatter, z motion blur intensity, w 1 = temporal antialiasing
	float4 g_SsrParams;       // x 1 = on, y smoothness below which there is none, z smoothness from which it is full, w screen fade distance
	float4 g_QualityParams;   // x how far the sun's shadow edges are blurred, in shadow map texels
};

cbuffer DrawConstants : register(b1) {
	uint g_InstanceIndex; // where this draw's instances start in g_InstanceMap
	uint g_MaterialIndex;
	uint g_CascadeIndex;
	uint g_DrawFlags;     // bit 0: the instance is mirrored
};

struct InstanceData {
	float4x4 world;
	float4 normalRow0; // rows of the inverse transpose of the world's upper 3x3; w of row0 = handedness (-1 for mirrored)
	float4 normalRow1;
	float4 normalRow2;
	float4 layers; // x: Renderer.renderingLayerMask as uint bits (asuint), y: 1 = moved since the frame before
	float4x4 prevWorld; // the world matrix of the frame before
};

struct MaterialData {
	float4 baseColor;
	float4 secondaryColor;
	float4 emissiveMetallic; // rgb emissive, a metallic
	float4 params;           // x roughness, y normal scale, z alpha cutoff (< 0 = no alpha test), w occlusion strength
	float4 params2;          // x clear coat, y clear coat roughness, z material class, w flags
	float4 uvScaleOffset;
	float4 params3;          // x mask layout (0 none, 1 HDRP, 2 RMO), y has mask texture, z has normal texture, w has extra texture
};

struct LightData {
	float4 positionRange;  // xyz world position, w range
	float4 directionOuter; // xyz the way the light travels, w cos(outer half angle)
	float4 colorInner;     // rgb candela, w cos(inner half angle)
	float4 typeFlags;      // x type (1 point, 2 spot), y rendering layers the light reaches (asuint)
};

#ifdef AE_PARTICLES
// (the particle pass binds its particles where the other passes have their instances)
struct ParticleData {
	float4 positionSize;    // xyz world space centre, w size (metres across)
	float4 color;           // linear rgb, a opacity
	float4 rotationStretch; // x rotation about the view direction; yzw from the centre to the far end (0 = a square)
	float4 uv;              // offset xy, size zw
};
StructuredBuffer<ParticleData> g_Particles : register(t0);
#else
StructuredBuffer<InstanceData> g_Instances : register(t0);
#endif
StructuredBuffer<MaterialData> g_Materials : register(t1);
StructuredBuffer<LightData> g_Lights : register(t2);
StructuredBuffer<uint2> g_ClusterGrid : register(t3); // x offset into g_LightIndices, y count
StructuredBuffer<uint> g_LightIndices : register(t4);
StructuredBuffer<uint> g_InstanceMap : register(t5); // per drawn instance: its index in g_Instances (a draw covers a run of these)

Texture2D g_BaseMap : register(t0, space1);
Texture2D g_NormalMap : register(t1, space1);
Texture2D g_MaskMap : register(t2, space1);
Texture2D g_EmissiveMap : register(t3, space1);
Texture2D g_ExtraMap : register(t4, space1);

Texture2D g_ShadowAtlas : register(t0, space2);
TextureCubeArray g_Sky : register(t1, space2);
Texture2D g_Irradiance : register(t2, space2);  // the sky's diffuse light per direction (octahedral map, PSIrradiance)
TextureCubeArray g_Probes[4] : register(t0, space3); // reflection probe cubes (viewed like the sky: arrays of one)
Texture2D g_AO : register(t3, space2);         // screen space ambient occlusion (white when off)
// the inputs of a full screen pass
Texture2D g_SceneColor : register(t4, space2); // the pass's main input
Texture2D g_PostDepth : register(t5, space2);  // scene depth (reversed Z)
Texture2D g_PostAux : register(t6, space2);
Texture2D g_PostAux2 : register(t7, space2);

SamplerState s_Aniso : register(s0);
SamplerState s_LinearClamp : register(s1);
SamplerComparisonState s_Shadow : register(s2);
SamplerState s_PointClamp : register(s3);

// material classes (AeMaterialClass)
#define CLASS_UNSUPPORTED 0
#define CLASS_STANDARD 1
#define CLASS_VEHICLE_PAINT 2
#define CLASS_VEHICLE_GENERIC 3
#define CLASS_VEHICLE_GLASS 4
#define CLASS_VEHICLE_RIM 5
#define CLASS_VEHICLE_STITCHES 6
#define CLASS_COLOR_MASK 7
#define CLASS_VEGETATION 8
#define CLASS_TRANSPARENT 9
#define CLASS_UNLIT 10
#define CLASS_VEHICLE_LIGHTS 11
#define CLASS_VEHICLE_TIRE 12
#define CLASS_BLOB_SHADOW 14

// debug views (setting debugView)
#define VIEW_NONE 0
#define VIEW_ALBEDO 1
#define VIEW_NORMAL 2
#define VIEW_ROUGHNESS 3
#define VIEW_METALLIC 4
#define VIEW_DEPTH 5
#define VIEW_LIGHTING 6
#define VIEW_SHADOW 7
#define VIEW_MATERIAL_CLASS 8
// lit views: one lighting term alone, exposed and tone mapped like the full picture (comparable with it)
#define VIEW_SUN 9
#define VIEW_AMBIENT 10
#define VIEW_REFLECTION 11 // what a mirror would show: sky and reflection probes

// Unity's ShadeSH9: irradiance / PI in the direction n (linear)
float3 AmbientSH(float3 n) {
	float4 n4 = float4(n, 1.0);
	float3 x1 = float3(dot(g_SH[0], n4), dot(g_SH[1], n4), dot(g_SH[2], n4));
	float4 b = n.xyzz * n.yzzx;
	float3 x2 = float3(dot(g_SH[3], b), dot(g_SH[4], b), dot(g_SH[5], b));
	float c = n.x * n.x - n.y * n.y;
	return max(x1 + x2 + g_SH[6].rgb * c, 0.0);
}

// The sky is the host's own cube map: a plain mip chain (every level the average of the one below), turned about the
// up axis and scaled to the scene's units.
float3 SkyDirection(float3 d) {
	return float3(d.x * g_SkyParams.y - d.z * g_SkyParams.z, d.y, d.x * g_SkyParams.z + d.z * g_SkyParams.y);
}

float3 SkyRadiance(float3 dir, float roughness) {
	if (g_Misc.y > 0.5) {
		// a rougher surface reads a level whose texels are about as wide as its reflection lobe: counted from the 1 x 1
		// end of the chain, so that the result does not depend on the cube's resolution
		float last = max(g_Misc.z - 1.0, 0.0);
		float mip = roughness <= 0.0 ? 0.0 : clamp(last - 2.0 + 1.2 * log2(max(roughness, 1e-3)), 0.0, last);
		return g_Sky.SampleLevel(s_LinearClamp, float4(SkyDirection(dir), 0.0), mip).rgb * g_SkyParams.x;
	}
	// no sky texture: the ambient probe is all there is to know about the sky
	return AmbientSH(dir);
}

// What the surroundings reflect towards the eye along R: the sky, replaced inside a reflection probe's influence
// volume by that probe's cube map (fading in over its blend distance, re-projected onto its box when it says so).
// (probe cube maps are Unity's: 7 convolved levels, perceptual roughness mapped to them like this)
float ProbeMip(float roughness, float mipCount) {
	return min(roughness * (1.7 - 0.7 * roughness) * 6.0, max(mipCount - 1.0, 0.0));
}

float3 EnvironmentSpecular(float3 worldPos, float3 R, float roughness) {
	float3 result = SkyRadiance(R, roughness);
	uint count = (uint)g_ProbeCount.x;
	for (uint i = 0; i < count; ++i) {
		float4 r0 = g_ProbeRows[i * 3], r1 = g_ProbeRows[i * 3 + 1], r2 = g_ProbeRows[i * 3 + 2];
		float3 local = float3(dot(r0.xyz, worldPos) + r0.w, dot(r1.xyz, worldPos) + r1.w, dot(r2.xyz, worldPos) + r2.w);
		float3 extents = g_ProbeExtents[i].xyz;
		float blend = max(g_ProbeExtents[i].w, 1e-3);
		uint flags = (uint)g_ProbeInfo[i].x;
		float weight;
		if ((flags & 2u) != 0u) {
			weight = saturate((extents.x - length(local)) / blend);
		} else {
			float3 inside = extents - abs(local);
			weight = saturate(min(inside.x, min(inside.y, inside.z)) / blend);
		}
		if (weight <= 0.0) {
			continue;
		}
		float3 dir = R;
		if ((flags & 1u) != 0u && (flags & 2u) == 0u) {
			// where the reflected ray leaves the box, seen from where the cube was captured
			float3 localR = float3(dot(r0.xyz, R), dot(r1.xyz, R), dot(r2.xyz, R));
			float3 safeR = sign(localR) * max(abs(localR), 1e-5);
			float3 tFar = max((extents - local) / safeR, (-extents - local) / safeR);
			float t = min(tFar.x, min(tFar.y, tFar.z));
			dir = worldPos + R * t - g_ProbeCapture[i].xyz;
		}
		float mip = ProbeMip(roughness, g_ProbeInfo[i].y);
		float3 probe = g_Probes[i].SampleLevel(s_LinearClamp, float4(dir, 0.0), mip).rgb * g_ProbeCapture[i].w;
		result = lerp(result, probe, weight);
	}
	return result;
}

// Diffuse ambient light (irradiance / PI) arriving at a surface with normal n.
// With a sky cube: that sky integrated over the hemisphere around n, in the same units as the sky Aether draws.
// RenderSettings.ambientProbe (the fallback) is only what Unity baked for the scene and can be far from the sky on screen.
// Octahedral mapping of the sphere to the unit square (the irradiance map).
float2 OctEncode(float3 n) {
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	float2 e = n.y >= 0.0 ? n.xz : (1.0 - abs(n.zx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
	return e * 0.5 + 0.5;
}

float3 OctDecode(float2 uv) {
	float2 e = uv * 2.0 - 1.0;
	float3 n = float3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
	if (n.y < 0.0) {
		n.xz = (1.0 - abs(n.zx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
	}
	return normalize(n);
}

float3 AmbientDiffuse(float3 n) {
	if (g_Misc.y > 0.5) {
		// the sky convolved with the cosine lobe (irradiance / PI), made every frame from the sky cube: PSIrradiance
		float2 uv = OctEncode(n) * (31.0 / 32.0) + (0.5 / 32.0);
		return g_Irradiance.SampleLevel(s_LinearClamp, uv, 0.0).rgb;
	}
	return AmbientSH(n);
}

#endif

// The sun's shadow at a world position (1 = lit): the cascade for the view depth, 3 x 3 taps.
float SunShadow(float3 worldPos, float3 normal, float viewDepth, out uint cascade) {
	cascade = 0;
	uint count = (uint)g_ShadowParams.x;
	if (count == 0 || g_SunColor.w < 0.5) {
		return 1.0;
	}
	for (uint i = 0; i < count; ++i) {
		cascade = i;
		if (viewDepth < g_CascadeSplits[i]) {
			break;
		}
	}
	if (viewDepth >= g_CascadeSplits[count - 1]) {
		return 1.0; // beyond the last cascade
	}
	// push the sample off the surface along the normal by about one shadow texel of this cascade
	float texelWorld = g_CascadeTexel[cascade] * 1.5;
	float3 p = worldPos + normal * texelWorld;
	float4 sc = mul(g_ShadowMatrix[cascade], float4(p, 1.0));
	float depth = sc.z - g_ShadowParams.z;
	float texel = g_ShadowParams.y;
	// 3 x 3 taps (each already the mean of four comparisons) spread over the softness the user chose; 5 x 5 where
	// that would leave gaps between the taps
	float softness = max(g_QualityParams.x, 0.0);
	int reach = softness > 1.6 ? 2 : 1;
	float spacing = softness / float(reach) * texel;
	float sum = 0.0;
	for (int y = -reach; y <= reach; ++y) {
		for (int x = -reach; x <= reach; ++x) {
			sum += g_ShadowAtlas.SampleCmpLevelZero(s_Shadow, sc.xy + float2(x, y) * spacing, depth);
		}
	}
	sum *= 9.0 / float((2 * reach + 1) * (2 * reach + 1));
	return sum / 9.0;
}
