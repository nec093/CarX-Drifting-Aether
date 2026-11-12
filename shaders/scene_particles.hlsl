// Aether's particle pass: the game's particle systems and tyre smoke as camera facing quads.
// The quads are made here from the frame's particle buffer (six vertices per particle, no vertex buffer). There is no
// depth buffer bound: the pixel shader compares with the scene depth itself, so a quad fades out where it cuts into a
// surface instead of ending in a hard line. Output is premultiplied (an additive particle returns alpha 0).
#define AE_PARTICLES
#include "aether_scene.hlsli"

// AeParticleFlags (SceneContract.h), in g_DrawFlags
#define PARTICLE_ADDITIVE 1u
#define PARTICLE_LIT 2u
#define PARTICLE_DISC 4u

struct ParticleOutput {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float2 corner : TEXCOORD1;  // -1..1 across the quad
	float4 color : COLOR0;
	float4 center : TEXCOORD2;  // xyz the particle's centre (world), w its size
	float viewDepth : TEXCOORD3;
};

ParticleOutput VSParticle(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
	static const float2 kCorners[6] = {float2(-1, -1), float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(1, -1)};
	ParticleData p = g_Particles[g_InstanceIndex + instance];
	float2 c = kCorners[vertex];
	float3 center = p.positionSize.xyz;
	float half = p.positionSize.w * 0.5;
	float3 toEye = normalize(g_CameraPos.xyz - center);
	float3 right, up;
	float3 stretch = p.rotationStretch.yzw;
	if (dot(stretch, stretch) > 1e-10) {
		// stretched along a world direction (sparks, streaks): as wide as its size, as long as the vector
		up = stretch;
		float3 side = cross(toEye, stretch);
		right = side * (half / max(length(side), 1e-6));
	} else {
		float3 r0 = cross(float3(0.0, 1.0, 0.0), toEye);
		r0 = r0 * (1.0 / max(length(r0), 1e-5));
		float3 u0 = cross(toEye, r0);
		float s, co;
		sincos(p.rotationStretch.x, s, co);
		right = (r0 * co + u0 * s) * half;
		up = (u0 * co - r0 * s) * half;
	}
	float3 world = center + right * c.x + up * c.y;
	ParticleOutput o;
	o.position = mul(g_ViewProj, float4(world, 1.0));
	o.uv = (c * float2(0.5, -0.5) + 0.5) * p.uv.zw + p.uv.xy;
	o.corner = c;
	o.color = p.color;
	o.center = float4(center, p.positionSize.w);
	o.viewDepth = o.position.w;
	return o;
}

float4 PSParticle(ParticleOutput input) : SV_Target {
	// the scene in front of the particle hides it; close behind it, the particle fades (no hard line where smoke meets the road)
	float2 ndc = input.position.xy * g_ScreenSize.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float d = g_PostDepth.Load(int3(input.position.xy, 0)).r;
	float fade = 1.0;
	if (d > 0.0) {
		float sceneDepth = abs(1.0 / dot(g_InvViewProj[3], float4(ndc, d, 1.0)));
		float gap = sceneDepth - input.viewDepth;
		if (gap <= 0.0) {
			discard;
		}
		fade = saturate(gap / max(input.center.w * 0.35, 0.02));
	}
	// ... and so does one the camera is about to pass through
	fade *= saturate((input.viewDepth - 0.15) / 0.6);

	float4 c = input.color;
	if ((g_DrawFlags & PARTICLE_DISC) != 0u) {
		float r = saturate(1.0 - dot(input.corner, input.corner));
		c.a *= r * r;
	} else {
		c *= g_SceneColor.Sample(s_Aniso, input.uv);
	}
	if ((g_DrawFlags & PARTICLE_LIT) != 0u) {
		// smoke and dust scatter what reaches them: the sky's light from all around, the sun's where it is not in shadow
		uint cascade;
		float shadow = SunShadow(input.center.xyz, float3(0.0, 1.0, 0.0), input.viewDepth, cascade);
		float3 light = AmbientDiffuse(float3(0.0, 1.0, 0.0)) * g_Misc.w;
		if (g_SunDirection.w > 0.5) {
			light += g_SunColor.rgb * shadow * (0.5 / PI);
		}
		c.rgb *= light;
	}
	c.a = saturate(c.a * fade);
	if ((g_DrawFlags & PARTICLE_ADDITIVE) != 0u) {
		return float4(c.rgb * c.a, 0.0);
	}
	return float4(c.rgb * c.a, c.a);
}
