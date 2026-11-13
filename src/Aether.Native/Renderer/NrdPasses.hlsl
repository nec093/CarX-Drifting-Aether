// Aether's two passes around NRD's specular denoiser (REBLUR_SPECULAR): compiled when Aether is built (DXC, DXIL
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
