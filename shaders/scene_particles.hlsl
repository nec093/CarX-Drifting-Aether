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
	// smoke (lit discs): the light at the puff, worked out once per corner
	float3 ambient : TEXCOORD4;   // from all round: the probes / the sky, and the lamps
	float3 sun : TEXCOORD5;       // the sun's (or the moon's) light there, shadows counted
	float3 sunInQuad : TEXCOORD6; // towards the sun, in the quad's own axes (right, up, towards the eye)
	float3 upInQuad : TEXCOORD7;  // the world's up, likewise
	float3 world : TEXCOORD9;     // this corner in the world
	nointerpolation float4 puff : TEXCOORD8; // the particle's uv numbers as they are (smoke: xy its place in the noise, z density, w its shade)
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
	if ((g_DrawFlags & PARTICLE_LIT) != 0u && dot(g_CameraPos.xyz - center, g_CameraPos.xyz - center) < half * half * 0.36) {
		// the camera is inside this puff: it would fill the screen and show next to nothing (see the fade in the pixel
		// shader). Not drawn at all: a cloud of such puffs took the frame rate down to 38.
		o.position = float4(0.0, 0.0, -1.0, 1.0);
	}
	o.ambient = 0.0;
	o.puff = p.uv;
	o.world = world;
	o.sun = 0.0;
	o.sunInQuad = float3(0.0, 0.0, 1.0);
	o.upInQuad = float3(0.0, 1.0, 0.0);
	if ((g_DrawFlags & PARTICLE_LIT) != 0u) {
		// Smoke and dust scatter what reaches them. Per corner, not per pixel: a puff is small against what lights it,
		// and a cloud of puffs covers every pixel many times over.
		uint cascade;
		float3 above = float3(0.0, 1.0, 0.0);
		o.ambient = AmbientLight(center, above);
		if (g_SunDirection.w > 0.5) {
			o.sun = g_SunColor.rgb * SunShadow(center, above, o.viewDepth, cascade);
		}
		// the lamps (a head lamp's beam in smoke, a street lamp over it): no shadows, the light a small sphere catches
		uint lamps = min((uint)g_GiParams.w, 48u);
		[loop] for (uint li = 0; li < lamps; ++li) {
			LightData lamp = g_Lights[li];
			float3 toLamp = lamp.positionRange.xyz - center;
			float d2 = dot(toLamp, toLamp);
			float range = lamp.positionRange.w;
			if (d2 >= range * range) {
				continue;
			}
			float w = saturate(1.0 - (d2 * d2) / (range * range * range * range));
			float attenuation = w * w / max(d2, 0.25);
			if (lamp.typeFlags.x > 1.5) {
				float outer = lamp.directionOuter.w, inner = lamp.colorInner.w;
				attenuation *= smoothstep(outer, max(inner, outer + 1e-4), dot(-toLamp * rsqrt(max(d2, 1e-6)), lamp.directionOuter.xyz));
				if (lamp.typeFlags.z > 0.5) {
					attenuation *= 0.15; // (a head lamp's cone is wide and mostly dim)
				}
			}
			o.ambient += lamp.colorInner.rgb * (attenuation / (4.0 * PI));
		}
		float3 r1 = right * (1.0 / max(length(right), 1e-6)), u1 = up * (1.0 / max(length(up), 1e-6));
		o.sunInQuad = float3(dot(g_SunDirection.xyz, r1), dot(g_SunDirection.xyz, u1), dot(g_SunDirection.xyz, toEye));
		o.upInQuad = float3(r1.y, u1.y, toEye.y);
	}
	return o;
}

float4 PSParticle(ParticleOutput input) : SV_Target {
	// the scene in front of the particle hides it; close behind it, the particle fades (no hard line where smoke meets the road)
	// (g_CascadeIndex: how many of the picture's pixels one pixel of this target is across; the smoke's own target: 2)
	float2 screen = input.position.xy * max(float(g_CascadeIndex), 1.0);
	float2 ndc = screen * g_ScreenSize.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float d = g_PostDepth.Load(int3(screen, 0)).r;
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
	if ((g_DrawFlags & PARTICLE_LIT) != 0u) {
		// (smoke round the camera: a puff counts by how far the eye is from its middle, none at 0.3 of its size)
		fade *= saturate((distance(g_CameraPos.xyz, input.center.xyz) / max(input.center.w, 0.05) - 0.3) / 0.5);
	}

	float4 c = input.color;
	const bool smoke = (g_DrawFlags & (PARTICLE_DISC | PARTICLE_LIT)) == (PARTICLE_DISC | PARTICLE_LIT);
	float body = 1.0; // how much of the puff the eye looks through here (1 its middle .. 0 its rim)
	float mottle = 1.0; // lighter and darker patches inside the smoke
	if ((g_DrawFlags & PARTICLE_DISC) != 0u) {
		float r = saturate(1.0 - dot(input.corner, input.corner));
		body = r;
		if (smoke && g_Atmosphere.w > 0.5) {
			// Smoke is one body, not a heap of balls: a puff is only a soft share of it, without an edge of its own
			// (a bell that ends at the quad's rim). What gives the smoke its shapes is a pattern fixed in space (the
			// clouds' noise, a volume, rising slowly), read at the pixel's own place in the world: neighbouring puffs
			// show the same billows and wisps where they overlap, so the eye sees a cloud and not its puffs.
			// (puff.xy: this puff's own small offset in the pattern, puff.z how dense it is.)
			float density = saturate(input.puff.z);
			float bell = saturate((exp(-3.2 * (1.0 - r)) - 0.0408) / 0.9592);
			float3 at = input.world * 0.11 + float3(input.puff.xy * 0.06, 0.0) + float3(0.0, -g_CameraPos.w * 0.012, g_CameraPos.w * 0.004);
			float2 noise = CloudNoise3(at.xzy);
			float fine = noise.g; // (the same lookup's small cells: a second lookup cost a third of the pass)
			// (thin smoke is all wisps, dense smoke keeps its body)
			float wisps = lerp(0.95, 0.5, density);
			body = bell;
			c.a *= bell * saturate(1.0 - wisps + wisps * (noise.r * 1.9 - 0.15 + (fine - 0.5) * 0.8));
			mottle = 0.78 + 0.4 * noise.r;
		} else {
			c.a *= r * r;
		}
	} else {
		c *= g_SceneColor.Sample(s_Aniso, input.uv);
	}
	if ((g_DrawFlags & PARTICLE_LIT) != 0u) {
		// The puff as a ball: its side towards the sun is lit, the other side is in its own shade (the denser the smoke,
		// the darker), its underside gets less of the sky. Looking towards the sun through its thin rim it glows: smoke
		// scatters mostly forwards.
		float2 q = input.corner;
		float3 n = float3(q, sqrt(saturate(1.0 - dot(q, q))));
		float towards = dot(n, input.sunInQuad) * 0.5 + 0.5;
		float dense = smoke ? saturate(input.puff.z) : 0.5;
		// (only a little of the puff as a ball: the light and shade of the smoke come from the whole cloud, below)
		float lit = lerp(1.0, towards * towards * (3.0 - 2.0 * towards), smoke ? 0.15 : 0.45 + 0.5 * dense);
		float glow = pow(saturate(-input.sunInQuad.z), 6.0) * (1.0 - body) * 1.6;
		float skyShare = lerp(smoke ? 0.8 : 0.42, 1.0, dot(n, input.upInQuad) * 0.5 + 0.5);
		// (smoke is not white paint: it sends back some 70 % of what reaches it)
		// the smoke's own shade (worked out per puff on the CPU: Smoke.cpp): what the smoke round the puff leaves of the
		// sky's light and of the sun's
		float skyReach = smoke ? saturate(floor(input.puff.w) * 0.01) : 1.0, sunReach = smoke ? saturate(frac(input.puff.w) / 0.99) : 1.0;
		c.rgb *= 0.72 * mottle * (input.ambient * skyShare * lerp(0.3, 1.0, skyReach) + input.sun * (sunReach * (0.06 + 0.8 * lit + glow) / PI));
	}
	c.a = saturate(c.a * fade);
	if ((g_DrawFlags & PARTICLE_ADDITIVE) != 0u) {
		return float4(c.rgb * c.a, 0.0);
	}
	return float4(c.rgb * c.a, c.a);
}
