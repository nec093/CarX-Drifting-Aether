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
	float4 g_QualityParams;   // x how far the sun's shadow edges are blurred (shadow map texels), y 1 = volumetric fog (no fog per surface), z bounced light strength (0 = off), w 1 = ray traced reflections
	float4 g_Atmosphere;      // x the physical sky's scale to scene units, y night sky floor, z eye altitude (km), w 1 = physical sky
	float4 g_Clouds;          // x coverage, yz how far the wind has carried them (noise uv), w 1 = clouds
	float4 g_ProbeGrid;          // x the probes' spacing on the ground, y in height, z a different number every frame, w 1 = probes on
	float4 g_ProbePositions[8];  // xyz where the probe stands, w 1 = usable
	float4 g_ReflectionBasis[18]; // per cube face (+X -X +Y -Y +Z -Z): the directions of its tile's centre, right edge and top edge (w of the first two: near, far)
	float4 g_GiGrid;             // xyz the grid cell of the GI volume's lowest corner, w 1 = probe grid GI on
	float4 g_GiPrev;             // xyz the same the frame before, w 1 = the probes hold something
	float4 g_GiParams;           // x metres between probes on the ground, y in height, z a different number every frame, w number of punctual lights
	float4 g_RayParams;          // x 1 = rays this frame (the structures are bound), y 1 = g_SunShadowMask holds the sun's visibility for the opaque pass, z tan of the sun's angular radius, w 1 = g_SunShadowUntraced holds the casters the rays cannot find
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
	float4 typeFlags;      // x type (1 point, 2 spot), y rendering layers the light reaches (asuint), z 1 = a head lamp (low beam pattern)
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
Texture2D g_SkyView : register(t0, space4);       // the physical sky around the camera, clouds included (PSSkyViewLut / PSSkyCloudsLut)
Texture2D g_Transmittance : register(t1, space4); // the physical sky's transmittance LUT
Texture2D g_MultiScatter : register(t2, space4);  // ... and its multiple scattering LUT
Texture2D g_ProbeAtlas : register(t4, space4);      // the reflection probes: per probe six tiles, 3 x 2; the probes 4 x 2 (a: 0 = sky)
Texture2D g_ProbeAtlasBlur : register(t5, space4);  // ... blurred, for rough surfaces
Texture2D g_GiIrradiance : register(t7, space4);    // probe grid GI: per probe a tile of 8 x 8 directions (octahedral), the light arriving round each
Texture2D g_GiDepth : register(t8, space4);         // ... r how far the probe sees that way on average (capped), g the same squared
Texture2D g_ProbeAtlasDepth : register(t6, space4); // ... and their depth (reversed Z per tile)
Texture2D g_CloudNoise : register(t3, space4);
Texture2D g_SunShadowMask : register(t9, space4);  // the ray traced sun shadow, denoised (LightingDenoiser): per pixel of the opaque picture
Texture2D g_SunShadowUntraced : register(t10, space4); // the shadow map of what the rays cannot find (half size, the same cascades)    // tiling noise the clouds' shape comes from (PSCloudNoise)

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
#define VIEW_REFLECTION 11
#define VIEW_BVH 12      // the scene as rays through the ray tracing structure find it (PSBvhDebug)
#define VIEW_BVH_COST 13 // ... and what each ray cost // what a mirror would show: sky and reflection probes

// Unity's ShadeSH9: irradiance / PI in the direction n (linear)
float3 AmbientSH(float3 n) {
	float4 n4 = float4(n, 1.0);
	float3 x1 = float3(dot(g_SH[0], n4), dot(g_SH[1], n4), dot(g_SH[2], n4));
	float4 b = n.xyzz * n.yzzx;
	float3 x2 = float3(dot(g_SH[3], b), dot(g_SH[4], b), dot(g_SH[5], b));
	float c = n.x * n.x - n.y * n.y;
	return max(x1 + x2 + g_SH[6].rgb * c, 0.0);
}

#include "aether_atmosphere.hlsli"
#include "aether_clouds.hlsli"

// The sky is the host's own cube map: a plain mip chain (every level the average of the one below), turned about the
// up axis and scaled to the scene's units.
float3 SkyDirection(float3 d) {
	return float3(d.x * g_SkyParams.y - d.z * g_SkyParams.z, d.y, d.x * g_SkyParams.z + d.z * g_SkyParams.y);
}

float2 OctEncode(float3 n);

float3 SkyRadiance(float3 dir, float roughness) {
	if (g_Atmosphere.w > 0.5) {
		// the physical sky: smooth enough to be read as it is; a rough surface sees it blurred towards the sky's
		// cosine weighted mean around the direction (the irradiance map)
		float3 sky = PhysicalSky(dir);
		if (roughness > 0.0) {
			float3 blurred = g_Irradiance.SampleLevel(s_LinearClamp, OctEncode(dir) * (31.0 / 32.0) + (0.5 / 32.0), 0.0).rgb;
			sky = lerp(sky, blurred, saturate(roughness * roughness * 2.0));
		}
		return sky;
	}
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

// A reflection probe along a direction: which of its six tiles, and where in it. `tile`: the tile's size in texels
// (the lookup stays half a texel inside it: the neighbouring tile is another direction, or another probe).
// `along`: the cosine between the direction and the tile's axis.
float2 ProbeUv(uint slot, float3 d, float tile, out float along) {
	float3 a = abs(d);
	uint face = a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0u : 1u) : a.y >= a.z ? (d.y > 0.0 ? 2u : 3u) : (d.z > 0.0 ? 4u : 5u);
	float3 c = g_ReflectionBasis[face * 3].xyz, r = g_ReflectionBasis[face * 3 + 1].xyz, u = g_ReflectionBasis[face * 3 + 2].xyz;
	float dc = max(dot(d, c), 1e-5);
	along = dc / (length(c) * length(d));
	float k = dot(c, c) / dc;
	float2 ndc = float2(dot(d, r) / dot(r, r), dot(d, u) / dot(u, u)) * k;
	float2 uv = clamp(ndc * float2(0.5, -0.5) + 0.5, 0.5 / tile, 1.0 - 0.5 / tile);
	return (float2((slot % 4u) * 3u + face % 3u, (slot / 4u) * 2u + face / 3u) + uv) / float2(12.0, 4.0);
}

float4 ProbeSample(Texture2D atlas, uint slot, float3 d, float tile) {
	float along;
	return atlas.SampleLevel(s_LinearClamp, ProbeUv(slot, d, tile, along), 0.0);
}

// How far the thing a probe shows along a direction is from the probe (very far: the sky).
// (The depth is read filtered: a distance that jumps from texel to texel showed as blocks in the mirrored picture.)
float ProbeDistance(uint slot, float3 d) {
	float along;
	float2 uv = ProbeUv(slot, d, 128.0, along);
	float depth = g_ProbeAtlasDepth.SampleLevel(s_LinearClamp, uv, 0.0).r;
	if (depth <= 1e-6) {
		return 1e6;
	}
	float n = g_ReflectionBasis[0].w, f = g_ReflectionBasis[1].w;
	return n * f / (depth * (f - n) + n) / max(along, 1e-3);
}

// A probe was drawn from where it stands, not from the surface that mirrors: looked up along the mirrored direction
// as it is, everything sits where it would if it were infinitely far. So the mirrored ray is followed from the
// surface to about where the probe's depth says it ends, and the probe is read along the direction to that place.
// One estimate and one careful correction: iterating to the end made every depth edge a tear.
float3 ProbeParallax(uint slot, float3 worldPos, float3 R) {
	float3 o = worldPos - g_ProbePositions[slot].xyz;
	float t = ProbeDistance(slot, R);
	if (t > 1e5) {
		return R; // the sky
	}
	float3 q = o + R * t;
	float dq = length(q);
	float D = ProbeDistance(slot, q / dq);
	if (D < 1e5) {
		t = clamp(t + 0.5 * (D - dq), 0.5 * t, 2.0 * t);
	}
	return normalize(o + R * t);
}

// The direction a place in a tile stands for (the reverse of ProbeUv).
float3 ProbeDirection(uint face, float2 uv) {
	float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
	return normalize(g_ReflectionBasis[face * 3].xyz + g_ReflectionBasis[face * 3 + 1].xyz * ndc.x + g_ReflectionBasis[face * 3 + 2].xyz * ndc.y);
}

// Which probes a place reads: the usable probes on the grid around it count by how near they stand; the two that count
// most are blended by their weights. (One probe drawn by lot per pixel and frame, for the temporal antialiasing to
// average, was tried: whenever the camera moved the picture had too little history to average, and every mirrored
// surface flickered.) x, y: the two slots (-1: none), z: the share of the first.
float3 ProbesFor(float3 worldPos) {
	float w0 = 0.0, w1 = 0.0;
	int s0 = -1, s1 = -1;
	for (uint s = 0; s < 8; ++s) {
		float3 d = abs(worldPos - g_ProbePositions[s].xyz) / float3(g_ProbeGrid.x, g_ProbeGrid.y * 2.0, g_ProbeGrid.x);
		float w = g_ProbePositions[s].w * saturate(1.0 - d.x) * saturate(1.0 - d.z) * saturate(1.5 - d.y);
		if (w > w0) {
			w1 = w0;
			s1 = s0;
			w0 = w;
			s0 = int(s);
		} else if (w > w1) {
			w1 = w;
			s1 = int(s);
		}
	}
	return float3(float(s0), float(w1 > 0.0 ? s1 : -1), w0 / max(w0 + w1, 1e-5));
}

// The probe that counts most for a place (-1: none).
int ProbeFor(float3 worldPos) {
	return int(ProbesFor(worldPos).x);
}

// A probe's picture of what lies along R from a place (premultiplied: alpha is how much of the direction it covers).
float4 ProbeReflection(uint slot, float3 worldPos, float3 R, float roughness) {
	float3 Rc = ProbeParallax(slot, worldPos, R);
	return lerp(ProbeSample(g_ProbeAtlas, slot, Rc, 128.0), ProbeSample(g_ProbeAtlasBlur, slot, Rc, 32.0), saturate((roughness - 0.04) / 0.3));
}

float3 EnvironmentSpecular(float3 worldPos, float3 R, float roughness) {
	float3 result = SkyRadiance(R, roughness);
	if (g_ProbeGrid.w > 0.5) {
		// the still surroundings from the nearest probes, over the sky (premultiplied: alpha is how much of the
		// direction is covered). The blurred copy as the surface gets rougher.
		// Every usable probe on the grid around the place, by how near it stands (the weights fall to nothing at the
		// next grid point, so a probe comes in and goes out without a step). (Only the two that count most were
		// blended before: where the second and the third changed places the reflection jumped, and a floor showed a
		// line between a brighter and a darker part.)
		float4 near = 0.0;
		float sum = 0.0;
		for (uint s = 0; s < 8; ++s) {
			float3 d = abs(worldPos - g_ProbePositions[s].xyz) / float3(g_ProbeGrid.x, g_ProbeGrid.y * 2.0, g_ProbeGrid.x);
			float w = g_ProbePositions[s].w * saturate(1.0 - d.x) * saturate(1.0 - d.z) * saturate(1.5 - d.y);
			if (w > 0.001) {
				near += ProbeReflection(s, worldPos, R, roughness) * w;
				sum += w;
			}
		}
		if (sum > 0.0) {
			// (Not brought up to full strength where the weights add up to less than one: that is the edge of what the
			// probes cover, a couple of cells around the camera. Normalised there, the last probe with any weight
			// counted in full right up to the edge and the sky took over at a line across the floor; left as it is,
			// the probes fade out towards the edge.)
			near /= max(sum, 1.0);
			result = result * (1.0 - near.a) + near.rgb;
		}
	}
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

// A colour the picture can hold: NaN and infinity become black, the rest is kept between 0 and 30000. (A single
// pixel of either was spread by the temporal history and the bloom pyramid into black squares of every size, and by
// the lens flare into black ghosts. The scene targets are R11G11B10_FLOAT: above 65024 is infinity.) Tested on the
// bits: DXC compiles with fast math, which may take isnan and NaN comparisons away.
float3 SafeColor(float3 c) {
	uint3 bits = asuint(c) & 0x7FFFFFFFu;
	c = bits >= 0x7F800000u ? 0.0 : c;
	return clamp(c, 0.0, 30000.0);
}

float SafeValue(float v) {
	return (asuint(v) & 0x7FFFFFFFu) >= 0x7F800000u ? 0.0 : v;
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

// ---- probe grid GI (in the manner of "Dynamic Diffuse Global Illumination", Majercik et al. 2019)
// Probes stand on a grid fixed in the world, one in the middle of each cell; the volume of 24 x 8 x 24 of them
// follows the camera a cell at a time. A probe keeps its place in the textures whatever the volume does: its cell
// modulo the counts. Each has a tile of 8 x 8 directions (octahedral) in g_GiIrradiance and in g_GiDepth, which
// PSGiRays / PSGiIrradiance / PSGiDepth (scene_post.hlsl) keep up to date.
static const float3 kGiCounts = float3(24.0, 8.0, 24.0);
static const float kGiTile = 8.0;
static bool s_NoSkyFallback = false; // set by the pass whose rays bring the sky themselves: no unoccluded sky in AmbientLight
static bool s_GiOff = false; // set by the pass that must not read the probes (they hold nothing yet)

float3 GiStorage(float3 cell) {
	return cell - kGiCounts * floor(cell / kGiCounts);
}

float3 GiProbePosition(float3 cell) {
	return (cell + 0.5) * g_GiParams.xyx;
}

float2 GiTileUv(float3 cell, float3 dir) {
	float3 s = GiStorage(cell);
	float2 tile = float2(s.x + s.y * kGiCounts.x, s.z);
	// (half a texel inside the tile: the next tile is another probe)
	float2 inTile = clamp(OctEncode(dir) * kGiTile, 0.5, kGiTile - 0.5);
	return (tile * kGiTile + inTile) / (float2(kGiCounts.x * kGiCounts.y, kGiCounts.z) * kGiTile);
}

// The indirect light on a surface at P facing n, from the eight probes round it (irradiance / PI, like
// AmbientDiffuse). a: how far the answer holds. Beyond the volume the probes at its edge answer (the place is taken
// to the edge), and the answer gives way to the open sky over the next 8 cells (24 m) sideways and 3 above or below.
// (Before, the sky took over within the volume's last two cells: indoors, where the sky is many times brighter than
// what reaches the floor, that showed as a straight edge across a car park's floor some 36 m away.)
// A probe counts by how near it is, less when it is behind the surface, and hardly when it does not see the place:
// when the place is further from it than what the probe sees in that direction (walls between them).
float4 GiDiffuse(float3 P, float3 n) {
	if (g_GiGrid.w < 0.5 || s_GiOff) {
		return 0.0;
	}
	float3 spacing = g_GiParams.xyx;
	float3 biased = P + n * 0.3; // (off the surface: a place on a wall is not behind that wall for the probe)
	float3 g = biased / spacing - 0.5;
	float3 rel = g - g_GiGrid.xyz;
	float3 inside = clamp(rel, 0.0, kGiCounts - 1.001);
	float3 beyond = abs(rel - inside); // (in cells: 0 inside the volume)
	float fade = (1.0 - smoothstep(0.0, 8.0, max(beyond.x, beyond.z))) * (1.0 - smoothstep(0.0, 3.0, beyond.y));
	if (fade <= 0.0) {
		return 0.0;
	}
	g = inside + g_GiGrid.xyz;
	biased = (g + 0.5) * spacing;
	float3 base = floor(g);
	float3 f = g - base;
	float3 sum = 0.0;
	float weight = 0.0;
	for (int i = 0; i < 8; ++i) {
		float3 o = float3(i & 1, (i >> 1) & 1, i >> 2);
		float3 cell = base + o;
		float3 probe = GiProbePosition(cell);
		float3 tri = lerp(1.0 - f, f, o);
		float w = tri.x * tri.y * tri.z + 0.001;
		float wrap = (dot(normalize(probe - P), n) + 1.0) * 0.5;
		w *= wrap * wrap + 0.2;
		float3 pd = biased - probe;
		float r = length(pd);
		float2 m = g_GiDepth.SampleLevel(s_LinearClamp, GiTileUv(cell, pd / max(r, 1e-4)), 0.0).rg;
		if (r > m.x) {
			// (Chebyshev: the share of what the probe sees that way that lies at least this far)
			float variance = abs(m.y - m.x * m.x);
			float c = variance / (variance + (r - m.x) * (r - m.x));
			w *= max(c * c * c, 0.0);
		}
		w = max(w, 1e-6);
		if (w < 0.2) {
			w *= w * w * 25.0; // (small shares are made smaller still: a probe that hardly counts should not tint)
		}
		sum += g_GiIrradiance.SampleLevel(s_LinearClamp, GiTileUv(cell, n), 0.0).rgb * w;
		weight += w;
	}
	return float4(sum / max(weight, 1e-8), fade);
}

// The light that reaches a surface from all round it, not straight from a lamp: the probes where they stand, the
// open sky beyond them.
float3 AmbientLight(float3 P, float3 n) {
	float3 sky = AmbientDiffuse(n) * g_Misc.w;
	float4 gi = GiDiffuse(P, n);
	if (s_NoSkyFallback) {
		return gi.rgb * gi.a;
	}
	return lerp(sky, gi.rgb, gi.a);
}

#endif

// The sun's shadow at a world position (1 = lit), 3 x 3 taps. The cascade is the finest one whose box holds the
// point. (It used to be chosen by the point's depth in the view being drawn. That is the camera's depth only in the
// camera's own passes: a reflection probe's faces, drawn from where the probe stands, looked their points up in the
// wrong cascade or in none, and the probes had no shadows in them: what they showed was too bright, and so was
// every reflection that took its colour from them.) `viewDepth` is not used any more.
// The sun's shadow from a shadow map: `atlas` holds the cascades 2 x 2 in its tiles (g_ShadowMatrix), `scale` its
// texel's size against the main atlas's (g_SunShadowUntraced is half the size: 2).
float SunShadowIn(Texture2D atlas, float scale, float3 worldPos, float3 normal, out uint cascade) {
	cascade = 0;
	uint count = (uint)g_ShadowParams.x;
	if (count == 0 || g_SunColor.w < 0.5) {
		return 1.0;
	}
	float4 sc = 0.0;
	bool found = false;
	for (uint i = 0; i < count && !found; ++i) {
		// push the sample off the surface along the normal by about one shadow texel of this cascade
		float3 p = worldPos + normal * (g_CascadeTexel[i] * 1.5 * scale);
		float4 c = mul(g_ShadowMatrix[i], float4(p, 1.0));
		// (the cascades are tiles of the atlas, 2 x 2: inside this one's tile, a little off its edge so that the
		// taps stay in it)
		float2 local = (c.xy - float2(i % 2u, i / 2u) * 0.5) * 2.0;
		if (all(local > 0.02) && all(local < 0.98) && c.z > 0.0 && c.z < 1.0) {
			cascade = i;
			sc = c;
			found = true;
		}
	}
	if (!found) {
		return 1.0; // beyond the last cascade
	}
	float depth = sc.z - g_ShadowParams.z;
	float texel = g_ShadowParams.y * scale;
	// 3 x 3 taps (each already the mean of four comparisons) spread over the softness the user chose; 5 x 5 where
	// that would leave gaps between the taps
	float softness = max(g_QualityParams.x, 0.0);
	int reach = softness > 1.6 ? 2 : 1;
	float spacing = softness / float(reach) * texel;
	float sum = 0.0;
	for (int y = -reach; y <= reach; ++y) {
		for (int x = -reach; x <= reach; ++x) {
			sum += atlas.SampleCmpLevelZero(s_Shadow, sc.xy + float2(x, y) * spacing, depth);
		}
	}
	sum *= 9.0 / float((2 * reach + 1) * (2 * reach + 1));
	return sum / 9.0;
}

float SunShadow(float3 worldPos, float3 normal, float viewDepth, out uint cascade) {
	return SunShadowIn(g_ShadowAtlas, 1.0, worldPos, normal, cascade);
}
