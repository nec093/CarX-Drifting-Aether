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
	float4 g_ClusterCounts;   // x, y, z counts, w unused
	float4 g_ClusterZ;        // near, far, z count / log(far / near), unused
	float4 g_SunDirection;    // xyz: towards the sun, w: 1 if there is a sun
	float4 g_SunColor;        // rgb (lux), w: 1 if it casts shadows
	float4 g_SH[7];           // Unity's SHAr, SHAg, SHAb, SHBr, SHBg, SHBb, SHC
	float4 g_Exposure;        // x fallback multiplier, y 1 = read the exposure texture, z bias multiplier (2^EV), w tone map (0 none, 1 ACES, 2 neutral)
	float4 g_Misc;            // x debug view, y sky texture present, z sky mip count, w ambient intensity
	float4 g_Fog;             // rgb colour, a density
	float4 g_OutputRect;      // composite: left, top, right, bottom in pixels of the back buffer
};

cbuffer DrawConstants : register(b1) {
	uint g_InstanceIndex;
	uint g_MaterialIndex;
	uint g_CascadeIndex;
	uint g_DrawFlags;     // bit 0: the instance is mirrored
};

struct InstanceData {
	float4x4 world;
	float4 normalRow0; // rows of the inverse transpose of the world's upper 3x3; w of row0 = handedness (-1 for mirrored)
	float4 normalRow1;
	float4 normalRow2;
	float4 reserved;
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
	float4 typeFlags;      // x type (1 point, 2 spot)
};

StructuredBuffer<InstanceData> g_Instances : register(t0);
StructuredBuffer<MaterialData> g_Materials : register(t1);
StructuredBuffer<LightData> g_Lights : register(t2);
StructuredBuffer<uint2> g_ClusterGrid : register(t3); // x offset into g_LightIndices, y count
StructuredBuffer<uint> g_LightIndices : register(t4);

Texture2D g_BaseMap : register(t0, space1);
Texture2D g_NormalMap : register(t1, space1);
Texture2D g_MaskMap : register(t2, space1);
Texture2D g_EmissiveMap : register(t3, space1);
Texture2D g_ExtraMap : register(t4, space1);

Texture2D g_ShadowAtlas : register(t0, space2);
TextureCubeArray g_Sky : register(t1, space2);
Texture2D g_ExposureTex : register(t2, space2);
Texture2D g_SceneColor : register(t3, space2);

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

// Unity's ShadeSH9: irradiance / PI in the direction n (linear)
float3 AmbientSH(float3 n) {
	float4 n4 = float4(n, 1.0);
	float3 x1 = float3(dot(g_SH[0], n4), dot(g_SH[1], n4), dot(g_SH[2], n4));
	float4 b = n.xyzz * n.yzzx;
	float3 x2 = float3(dot(g_SH[3], b), dot(g_SH[4], b), dot(g_SH[5], b));
	float c = n.x * n.x - n.y * n.y;
	return max(x1 + x2 + g_SH[6].rgb * c, 0.0);
}

float3 SkyRadiance(float3 dir, float roughness) {
	if (g_Misc.y > 0.5) {
		float mip = roughness * max(g_Misc.z - 1.0, 0.0);
		return g_Sky.SampleLevel(s_LinearClamp, float4(dir, 0.0), mip).rgb;
	}
	// no sky texture: the ambient probe is all there is to know about the sky
	return AmbientSH(dir);
}

// Diffuse ambient light (irradiance / PI) arriving at a surface with normal n.
// With HDRP's sky cube: its roughest mip, which is the sky blurred over about a hemisphere. That is the light HDRP's
// own dynamic ambient is made of, in the same units and exposure as the sky Aether draws. RenderSettings.ambientProbe
// (the fallback) is only what Unity baked for the scene and can be far from the sky on screen.
float3 AmbientDiffuse(float3 n) {
	if (g_Misc.y > 0.5) {
		return g_Sky.SampleLevel(s_LinearClamp, float4(n, 0.0), max(g_Misc.z - 1.0, 0.0)).rgb;
	}
	return AmbientSH(n);
}

#endif
