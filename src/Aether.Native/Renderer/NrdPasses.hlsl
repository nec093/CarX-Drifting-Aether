// Aether's passes around NRD: the reflections' (REBLUR_SPECULAR) and the ray traced lighting's (SIGMA_SHADOW for the
// sun's shadow, REBLUR_DIFFUSE for ambient occlusion and the light bounced off nearby surfaces). Each: compiled when Aether is built (DXC, DXIL
// embedded in the runtime) because they use NRD.hlsli, NVIDIA's shader source, which is never shipped as source.
//
// The reflection rays are traced for one pixel of every 2 x 2 block (PSRayTrace): NRD denoises at that quarter
// resolution, with guides read from the full resolution targets at the same pixels.
//   CSPrepare: the guides (view Z, normal and roughness, motion) and the noisy signal, packed the way NRD wants them
//   CSResolve: NRD's output back into the layout the reflection pass reads (rgb what the ray brought back, a the
//              surface's distance from the eye; 0: no ray)
#include "NRD.hlsli"

cbuffer NrdPassConstants : register(b0) {
	float4x4 g_InvViewProj;  // clip -> world as the reflection passes reconstruct positions (this frame's, jittered)
	float4x4 g_WorldToView;  // the view matrix NRD is given: view Z is taken in its space
	float4 g_Size;           // xy the full resolution, zw the quarter resolution the rays have
	float4 g_HitDistParams;  // xyz REBLUR's hit distance parameters, w NRD's denoising range
	float4 g_Extra;          // the lighting passes: x tan of the sun's angular radius, y ambient occlusion strength
};

Texture2D<float> t_Depth : register(t0);
Texture2D<float4> t_Surface : register(t1);   // normal (octahedral, xy), roughness, reflection weight
Texture2D<float4> t_Velocity : register(t2);  // xy screen motion since the frame before (uv: now - before)
Texture2D<float4> t_Rays : register(t3);      // rgb what the ray brought back, a its hit distance (< 0: no ray)
RWTexture2D<float> u_ViewZ : register(u0);
RWTexture2D<float4> u_NormalRoughness : register(u1);
RWTexture2D<float4> u_Motion : register(u2);
RWTexture2D<float4> u_Specular : register(u3);

float3 OctDecode(float2 uv) {
	float2 e = uv * 2.0 - 1.0;
	float3 n = float3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
	if (n.y < 0.0) {
		n.xz = (1.0 - abs(n.zx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
	}
	return normalize(n);
}

float4 WorldAt(int2 p, float depth) {
	float2 ndc = (float2(p) + 0.5) / g_Size.xy * float2(2.0, -2.0) + float2(-1.0, 1.0);
	return mul(g_InvViewProj, float4(ndc, depth, 1.0));
}

[numthreads(8, 8, 1)]
void CSPrepare(uint3 id : SV_DispatchThreadID) {
	const uint2 x = id.xy;
	if (any(x >= uint2(g_Size.zw))) {
		return;
	}
	const int2 p = min(int2(x) * 2, int2(g_Size.xy) - 1); // (the pixel of the block the ray was traced for)
	const float depth = t_Depth.Load(int3(p, 0));
	const float4 surface = t_Surface.Load(int3(p, 0));
	const float4 ray = t_Rays.Load(int3(x, 0));
	const bool valid = depth > 0.0 && ray.a >= 0.0;
	// (where there is nothing, a point half way into the depth range: only the side of the eye it is on is used)
	const float4 h = WorldAt(p, valid ? depth : 0.5);
	float viewZ = mul(g_WorldToView, float4(h.xyz / h.w, 1.0)).z;
	if (!valid) {
		viewZ = (viewZ < 0.0 ? -2.0 : 2.0) * g_HitDistParams.w; // outside the denoising range: NRD leaves it alone
	}
	const float roughness = saturate(surface.z);
	u_ViewZ[x] = viewZ;
	u_NormalRoughness[x] = NRD_FrontEnd_PackNormalAndRoughness(OctDecode(surface.xy), roughness, 0.0);
	u_Motion[x] = float4(-t_Velocity.Load(int3(p, 0)).xy, 0.0, 0.0); // (NRD: before = now + motion)
	const float normHitDist = REBLUR_FrontEnd_GetNormHitDist(max(ray.a, 0.0), viewZ, g_HitDistParams.xyz, roughness);
	u_Specular[x] = valid ? REBLUR_FrontEnd_PackRadianceAndNormHitDist(ray.rgb, normHitDist, true) : 0.0;
}

Texture2D<float4> t_Denoised : register(t0); // (CSResolve's inputs: t0 NRD's output, t1 the depth, t2 the rays)
Texture2D<float> t_ResolveDepth : register(t1);
Texture2D<float4> t_ResolveRays : register(t2);
RWTexture2D<float4> u_Reflection : register(u0);

[numthreads(8, 8, 1)]
void CSResolve(uint3 id : SV_DispatchThreadID) {
	const uint2 x = id.xy;
	if (any(x >= uint2(g_Size.zw))) {
		return;
	}
	const int2 p = min(int2(x) * 2, int2(g_Size.xy) - 1);
	const float depth = t_ResolveDepth.Load(int3(p, 0));
	const float4 ray = t_ResolveRays.Load(int3(x, 0));
	if (depth <= 0.0 || ray.a < 0.0) {
		u_Reflection[x] = 0.0;
		return;
	}
	const float4 denoised = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(t_Denoised.Load(int3(x, 0)));
	const float4 h = WorldAt(p, depth);
	u_Reflection[x] = float4(min(max(denoised.rgb, 0.0), 300.0), abs(1.0 / h.w));
}


// ---- the ray traced lighting (LightingDenoiser): the sun's shadow (SIGMA_SHADOW) and the ambient occlusion with the
// light bounced off nearby surfaces (REBLUR_DIFFUSE: its normalized hit distance is the occlusion), denoised at half
// resolution (the rays were traced for the top left pixel of every 2 x 2 block) and laid over the full picture by
// depth. The guides come from the depth prepass. g_Size: xy the full size, zw the half.
Texture2D<float> t_LightDepth : register(t0);
Texture2D<float2> t_LightNormal : register(t1);   // the prepass's normal (octahedral)
Texture2D<float4> t_LightVelocity : register(t2); // xy screen motion since the frame before (uv: now - before)
Texture2D<float> t_SunRays : register(t3);        // the distance to what hides the sun (65504: nothing; 0: the surface faces away)
Texture2D<float4> t_AoRays : register(t4);        // rgb the light the short ray found, a its hit distance (the ray's length: nothing)
RWTexture2D<float> u_LightViewZ : register(u0);
RWTexture2D<float4> u_LightNormalRoughness : register(u1);
RWTexture2D<float4> u_LightMotion : register(u2);
RWTexture2D<float> u_Penumbra : register(u3);
RWTexture2D<float4> u_Diffuse : register(u4);

float ViewZAt(int2 p, float depth) {
	const float4 h = WorldAt(p, depth);
	return mul(g_WorldToView, float4(h.xyz / h.w, 1.0)).z;
}

[numthreads(8, 8, 1)]
void CSLightPrepare(uint3 id : SV_DispatchThreadID) {
	const uint2 x = id.xy;
	if (any(x >= uint2(g_Size.zw))) {
		return;
	}
	const int2 p = min(int2(x) * 2, int2(g_Size.xy) - 1);
	const float depth = t_LightDepth.Load(int3(p, 0));
	const bool valid = depth > 0.0;
	float viewZ = ViewZAt(p, valid ? depth : 0.5);
	if (!valid) {
		viewZ = (viewZ < 0.0 ? -2.0 : 2.0) * g_HitDistParams.w; // (the sky: outside the denoising range)
	}
	u_LightViewZ[x] = viewZ;
	u_LightNormalRoughness[x] = NRD_FrontEnd_PackNormalAndRoughness(OctDecode(t_LightNormal.Load(int3(p, 0))), 1.0, 0.0);
	const float4 velocity = t_LightVelocity.Load(int3(p, 0));
	u_LightMotion[x] = float4(velocity.x < 100.0 ? -velocity.xy : 0.0, 0.0, 0.0); // (NRD: before = now + motion)
	u_Penumbra[x] = SIGMA_FrontEnd_PackPenumbra(t_SunRays.Load(int3(x, 0)), g_Extra.x);
	const float4 ao = t_AoRays.Load(int3(x, 0));
	u_Diffuse[x] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(ao.rgb, REBLUR_FrontEnd_GetNormHitDist(ao.a, viewZ, g_HitDistParams.xyz, 1.0), true);
}

Texture2D<float4> t_ShadowOut : register(t0);  // half size
Texture2D<float4> t_DiffuseOut : register(t1); // half size
Texture2D<float> t_ResolveLightDepth : register(t2);
Texture2D<float> t_HalfViewZ : register(t3);   // the view Z each half size texel was denoised for
RWTexture2D<float4> u_AmbientOcclusion : register(u0); // rgb the bounced light, a the ambient occlusion (as the forward shader reads g_AO)
RWTexture2D<float> u_SunShadow : register(u1);         // the sun's visibility

[numthreads(8, 8, 1)]
void CSLightResolve(uint3 id : SV_DispatchThreadID) {
	const int2 p = int2(id.xy);
	if (any(p >= int2(g_Size.xy))) {
		return;
	}
	const float depth = t_ResolveLightDepth.Load(int3(p, 0));
	if (depth <= 0.0) {
		u_AmbientOcclusion[p] = float4(0.0, 0.0, 0.0, 1.0);
		u_SunShadow[p] = 1.0;
		return;
	}
	// the four half size texels round this pixel, each by how near it lies and by whether it was denoised for the same
	// surface (its view Z agrees)
	const float viewZ = ViewZAt(p, depth);
	const float2 at = float2(p) * 0.5;
	const int2 base = int2(floor(at));
	const float2 f = at - float2(base);
	const int2 last = int2(g_Size.zw) - 1;
	float4 diffuseSum = 0.0;
	float shadowSum = 0.0, weightSum = 0.0;
	[unroll] for (int k = 0; k < 4; ++k) {
		const int2 o = int2(k & 1, k >> 1);
		const int2 q = min(base + o, last);
		float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y) + 0.01;
		w *= exp(-abs(t_HalfViewZ.Load(int3(q, 0)) - viewZ) / (0.02 * abs(viewZ) + 0.02));
		diffuseSum += REBLUR_BackEnd_UnpackRadianceAndNormHitDist(t_DiffuseOut.Load(int3(q, 0))) * w;
		shadowSum += saturate(SIGMA_BackEnd_UnpackShadow(t_ShadowOut.Load(int3(q, 0)).x)) * w;
		weightSum += w;
	}
	float4 diffuse = float4(0.0, 0.0, 0.0, 1.0);
	float shadow = 1.0;
	if (weightSum > 1e-5) {
		diffuse = diffuseSum / weightSum;
		shadow = shadowSum / weightSum;
	}
	// the occlusion: how far the short rays got before they met something (1: nothing within the radius)
	float visibility = max(saturate(diffuse.w), 0.03);
	const float strength = g_Extra.y; // (Aether's setting: up to 1 how much of the occlusion shows, above 1 darker)
	visibility = strength <= 1.0 ? lerp(1.0, visibility, saturate(strength)) : pow(visibility, strength);
	u_AmbientOcclusion[p] = float4(clamp(diffuse.rgb, 0.0, g_Extra.z > 0.5 ? 60000.0 : 16.0), visibility);
	u_SunShadow[p] = saturate(shadow);
}
