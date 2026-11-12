// Aether full screen passes: sky background and the final composite (exposure, tone mapping, output encoding).
#include "aether_scene.hlsli"
#include "aether_bvh.hlsli"

struct FullscreenOutput {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

// One triangle covering the screen. z = 0 is the far plane with reversed Z: the sky is drawn where nothing else is.
FullscreenOutput VSFullscreen(uint id : SV_VertexID) {
	FullscreenOutput o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}

float4 PSSky(FullscreenOutput input) : SV_Target {
	// the world position of this pixel on a plane in front of the camera gives the view direction
	float2 ndc = input.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 world = mul(g_InvViewProj, float4(ndc, 0.5, 1.0));
	float3 dir = normalize(world.xyz / world.w - g_CameraPos.xyz);
	float3 color = SkyRadiance(dir, 0.0);
	if (g_Atmosphere.w > 0.5) {
		// the physical sky: the air (g_SceneColor: its sky view LUT), the sun's disc, and the clouds in front of both
		// (g_PostAux: marched at half resolution by PSClouds)
		color = PhysicalSkyAir(dir) + PhysicalSunDisc(dir);
		if (g_Clouds.w > 0.5) {
			float4 cloud = g_PostAux.SampleLevel(s_LinearClamp, input.uv, 0.0);
			color = color * cloud.a + cloud.rgb;
		}
	}
	if ((int)g_Misc.x != VIEW_NONE) {
		color = float3(0.02, 0.02, 0.03);
	}
	return float4(color, 1.0);
}

// The four numbers a full screen pass gets from the renderer (SceneRenderer::Fullscreen), in the draw constants.
#define g_PassParams float4(asfloat(g_InstanceIndex), asfloat(g_MaterialIndex), asfloat(g_CascadeIndex), asfloat(g_DrawFlags))

float3 WorldFromDepth(float2 uv, float depth); // (defined with the ambient occlusion pass below)

// ---- the sky's diffuse light. Every texel of the octahedral map is a direction n; its value is the sky's radiance
// averaged over the hemisphere around n with the cosine as weight (irradiance / PI), which is what a diffuse surface
// facing n receives from the sky. The sky cube is read at its 8 x 8 level: 384 directions over the sphere.
float3 CubeDirection(int face, float2 st) {
	switch (face) {
	case 0: return float3(1.0, -st.y, -st.x);
	case 1: return float3(-1.0, -st.y, st.x);
	case 2: return float3(st.x, 1.0, st.y);
	case 3: return float3(st.x, -1.0, -st.y);
	case 4: return float3(st.x, -st.y, 1.0);
	default: return float3(-st.x, -st.y, -1.0);
	}
}

float4 PSIrradiance(FullscreenOutput input) : SV_Target {
	float3 n = OctDecode(input.uv);
	const int kSide = 8;
	float mip = max(g_Misc.z - 4.0, 0.0); // the level with 8 x 8 texels a face
	float3 sum = 0.0;
	float weight = 0.0;
	[loop] for (int face = 0; face < 6; ++face) {
		[loop] for (int y = 0; y < kSide; ++y) {
			[loop] for (int x = 0; x < kSide; ++x) {
				float2 st = (float2(x, y) + 0.5) / float(kSide) * 2.0 - 1.0;
				float3 dir = CubeDirection(face, st);
				float solidAngle = 1.0 / pow(dot(dir, dir), 1.5); // of the texel, up to a constant factor
				dir = normalize(dir);
				float w = saturate(dot(n, dir)) * solidAngle;
				// (the cube is addressed in its own frame: n is a world direction, the lookup turns it like the sky pass does)
				float3 radiance = g_Atmosphere.w > 0.5 ? PhysicalSky(dir) : g_Sky.SampleLevel(s_LinearClamp, float4(SkyDirection(dir), 0.0), mip).rgb * g_SkyParams.x;
				sum += radiance * w;
				weight += w;
			}
		}
	}
	return float4(sum / max(weight, 1e-5), 1.0);
}

// ---- temporal antialiasing. Every frame is drawn with a different sub pixel offset; this pass blends the new frame
// into the resolved picture of the frames before. Where a point of the still world was a frame ago follows from its
// depth and the previous camera; pixels of moving things (alpha 0) are taken to stand still on screen (true for the
// car the camera follows). The old colour is clipped to what the new frame has around the pixel, so that anything
// that does not fit (something moved, something was uncovered) is forgotten at once.
float Luma(float3 c) {
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float2 PreviousUv(float2 uv, float depth) {
	float3 world = WorldFromDepth(uv, depth);
	float4 clip = mul(g_PrevViewProj, float4(world, 1.0));
	return clip.xy / max(clip.w, 1e-5) * float2(0.5, -0.5) + 0.5;
}

float4 PSTaa(FullscreenOutput input) : SV_Target {
	int2 p = int2(input.position.xy);
	int2 last = int2(g_ScreenSize.xy) - 1;
	float4 center = g_SceneColor.Load(int3(p, 0));
	float3 m1 = 0.0, m2 = 0.0;
	float closest = 0.0;
	int2 closestAt = p;
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			int2 q = clamp(p + int2(x, y), int2(0, 0), last);
			float3 c = g_SceneColor.Load(int3(q, 0)).rgb;
			c *= 1.0 / (1.0 + Luma(c)); // (compressed: a single very bright pixel does not blow the statistics up)
			m1 += c;
			m2 += c * c;
			float d = g_PostDepth.Load(int3(q, 0)).r; // reversed Z: the nearest surface around (edges follow the foreground)
			if (d > closest) {
				closest = d;
				closestAt = q;
			}
		}
	}
	float3 mean = m1 / 9.0, deviation = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
	float3 current = center.rgb / (1.0 + Luma(center.rgb));
	// where this pixel's surface was on screen in the frame before: what the opaque pass wrote (the surface's own
	// motion and the camera's; g_PostAux2), or, where nothing opaque was drawn, the camera's motion alone.
	// (Before, a moving object's history was read at the same place on screen: a car that moved on screen was a
	// blend of where it had been over the last frames.)
	// (taken from the nearest surface around the pixel, not from the pixel itself: the antialiased rim of a moving car
	// lies on background pixels, and with the background's motion those pixels looked up the car's old place, which
	// left a trail along the car's outline)
	float4 motion = g_PostAux2.Load(int3(closestAt, 0));
	float2 velocity = motion.xy;
	float movingNow = motion.x < 100.0 ? motion.z : 0.0;
	float2 previousUv = velocity.x < 100.0 ? input.uv - velocity : PreviousUv(input.uv, closest);
	bool usable = g_Jitter.z > 0.5 && all(previousUv > 0.0) && all(previousUv < 1.0);
	float4 before = g_PostAux.SampleLevel(s_LinearClamp, previousUv, 0.0);
	float3 history = before.rgb;
	history *= 1.0 / (1.0 + Luma(history));
	history = clamp(history, mean - 1.25 * deviation, mean + 1.25 * deviation);
	float keep = usable ? 0.9 : 0.0;
	// The history's alpha says whether that place showed something that moves by itself (a car). Where it does not
	// agree with what is there now, the history is of something else: the road a car has just uncovered still had
	// the car in its history, and the colour clamp alone let part of it through as a trail along the car's outline.
	keep *= 1.0 - smoothstep(0.2, 0.6, abs(before.a - movingNow));
	float3 result = lerp(current, history, keep);
	result *= 1.0 / max(1.0 - Luma(result), 1e-3);
	return float4(result, movingNow);
}

// ---- motion blur: every still point of the world is smeared along the way it moved on screen since the frame before
// (the camera's own motion). What moves by itself (marked in the velocity target: the car the camera follows) stays sharp and is not
// smeared into its surroundings.
float4 PSMotionBlur(FullscreenOutput input) : SV_Target {
	int2 p = int2(input.position.xy);
	float4 center = g_SceneColor.Load(int3(p, 0));
	// (g_PostAux: the opaque pass's velocity target; z = 1 marks what moves by itself)
	// (the blur needs the camera of the frame before, not the temporal history: it also runs without antialiasing)
	if (g_PostAux.Load(int3(p, 0)).z > 0.5) {
		return center;
	}
	float depth = g_PostDepth.Load(int3(p, 0)).r;
	float2 velocity = (input.uv - PreviousUv(input.uv, depth)) * g_PostParams.z;
	float pixels = length(velocity * g_ScreenSize.xy);
	if (pixels < 1.0) {
		return center;
	}
	velocity *= min(1.0, 48.0 / pixels); // at most 48 pixels
	float3 sum = center.rgb;
	float weight = 1.0;
	const int kTaps = 8;
	// (a different half step per pixel hides the steps between the taps)
	float dither = frac(52.9829189 * frac(dot(input.position.xy, float2(0.06711056, 0.00583715))));
	for (int i = 0; i < kTaps; ++i) {
		float t = (float(i) + dither) / float(kTaps) - 0.5;
		float2 tap = input.uv + velocity * t;
		float4 s = g_SceneColor.SampleLevel(s_LinearClamp, tap, 0.0);
		float w = g_PostAux.SampleLevel(s_PointClamp, tap, 0.0).z > 0.5 ? 0.0 : 1.0;
		sum += s.rgb * w;
		weight += w;
	}
	return float4(sum / weight, center.a);
}

// ---- reflections. For every smooth opaque surface (the opaque pass's second target says which) the eye's direction
// is mirrored, and what lies that way is looked for in three places:
//  1. on screen: the ray is followed through the depth buffer; where it meets something, that thing's colour is taken
//     from the picture of the frame before (g_SceneColor here: the temporal history);
//  2. in the scene's triangles (the ray tracing structure, aether_bvh.hlsli), when the screen does not have it. What
//     the ray hits takes its colour from the screen if that very place happens to be in the picture, else from a
//     reflection probe that sees it, else from its own material lit by the sun and the sky;
//  3. nothing hit: the sky.
// The surface was already shaded with the sky / probe reflection in that direction, so what is returned (and added to
// the picture) is the difference, by the share of mirrored light the surface returns.
// g_PostDepth: the scene depth, g_PostAux: the surface target.

// The colour of the place the picture of the frame before shows at a world position, if it shows that very place
// (its depth there agrees). a: 1 = found.
float4 ScreenRadianceAt(float3 worldPos) {
	if (g_SsrParams.x < 0.5) {
		return 0.0;
	}
	float4 c = mul(g_ViewProj, float4(worldPos, 1.0));
	if (c.w <= 0.05) {
		return 0.0;
	}
	float2 uv = c.xy / c.w * float2(0.5, -0.5) + 0.5;
	if (any(uv < 0.0) || any(uv > 1.0)) {
		return 0.0;
	}
	float d = g_PostDepth.SampleLevel(s_PointClamp, uv, 0.0).r;
	if (d <= 0.0) {
		return 0.0;
	}
	float sceneW = abs(1.0 / dot(g_InvViewProj[3], float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), d, 1.0)));
	if (abs(c.w - sceneW) > 0.02 * c.w + 0.03) {
		return 0.0; // something else is in front of it, or it is in front of what the picture shows there
	}
	float4 prev = mul(g_PrevViewProj, float4(worldPos, 1.0));
	if (prev.w <= 0.05) {
		return 0.0;
	}
	float2 previousUv = prev.xy / prev.w * float2(0.5, -0.5) + 0.5;
	if (any(previousUv < 0.0) || any(previousUv > 1.0)) {
		return 0.0;
	}
	return float4(min(g_SceneColor.SampleLevel(s_LinearClamp, previousUv, 0.0).rgb, 64.0), 1.0);
}

// What a ray from `o` along `d` (normalised) brings back.
// `sunDisc`: a ray that reaches the sky shows the sun where it looks at it (for mirrors: the glint of the sun in chrome).
float3 TracedRadiance(float3 o, float3 d, bool sunDisc) {
	// (a hit the texture's alpha cuts away is passed: a few times, then the sky)
	for (int attempt = 0; attempt < 3; ++attempt) {
		BvhHit hit = BvhTrace(o, d, 500.0, false);
		if (hit.t < 0.0) {
			break;
		}
		float3 P = o + d * hit.t;
		float4 screen = ScreenRadianceAt(P);
		if (screen.a > 0.5) {
			return screen.rgb;
		}
		float4 surface = BvhHitSurface(P, hit, hit.t, d);
		if (surface.a > 0.5) {
			return surface.rgb;
		}
		o = P + d * 0.02;
	}
	float3 sky = SkyRadiance(d, 0.0);
	if (sunDisc && g_SunDirection.w > 0.5) {
		// (wider than the real disc, two degrees across its soft edge: a wheel's curves shrink it to a point anyway, and a
		// point smaller than a pixel would come and go)
		float c = dot(d, g_SunDirection.xyz);
		// (a core and a wide glow round it: the glow is what reads as a gleam on a part that only nearly faces the sun)
		sky += g_SunColor.rgb * ((smoothstep(0.99863, 0.99985, c) * 200.0 + smoothstep(0.985, 0.9995, c) * 6.0) * CloudShadow(o));
	}
	return sky;
}

// How smooth a surface must be to mirror here: the scene's own setting, and never rougher than about 0.4 (what is
// mirrored here is sharp; a rough surface's blurred surroundings come from the probes).
float MirrorAmount(float4 surface) {
	float smoothness = 1.0 - surface.z;
	return surface.a * min(saturate((smoothness - g_SsrParams.y) / max(g_SsrParams.z - g_SsrParams.y, 1e-3)), saturate((0.4 - surface.z) / 0.2));
}

// The normal a mirror uses at a pixel: the surface's normals around it, averaged. A model's shading normals are
// straight lines between its vertices: at every triangle edge their slope changes, and a mirror (which doubles every
// angle and shows metres of surroundings in a hand's width of paint) turns each of those kinks into a wave that swims
// over the body when the camera moves. Averaged over about a triangle's width the kinks are gone and the panel's
// own curve is what is left. Only normals of the same surface count: about as far from the eye, and not turned away
// (a real crease stays a crease).
float3 MirrorNormal(int2 p, float3 N, float viewDepth) {
	static const int2 kTaps[12] = {
		int2(7, 2), int2(-3, 7), int2(-7, -3), int2(2, -7),
		int2(13, -6), int2(6, 13), int2(-13, 6), int2(-6, -13),
		int2(20, 9), int2(-9, 20), int2(-20, -9), int2(9, -20)};
	static const float kWeights[3] = {1.0, 0.7, 0.4};
	int2 last = int2(g_ScreenSize.xy) - 1;
	float3 sum = N * 1.5;
	for (int i = 0; i < 12; ++i) {
		int2 q = clamp(p + kTaps[i], int2(0, 0), last);
		float4 s = g_PostAux.Load(int3(q, 0));
		float d = g_PostDepth.Load(int3(q, 0)).r;
		if (s.a <= 0.0 || d <= 0.0) {
			continue;
		}
		float2 ndc = (float2(q) + 0.5) * g_ScreenSize.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
		float w = abs(1.0 / dot(g_InvViewProj[3], float4(ndc, d, 1.0)));
		float3 n = OctDecode(s.xy);
		// (only normals within a few degrees: the kinks at a panel's triangle edges are that small. A wider net also
		// caught the real shapes of small round parts, a wheel's spokes and lips, and their mirror images went soft.)
		float weight = kWeights[i / 4] * saturate(1.0 - abs(w - viewDepth) / (0.03 * viewDepth + 0.02)) * saturate((dot(n, N) - 0.985) / 0.012);
		sum += n * weight;
	}
	return normalize(sum);
}

// The rays of the reflection pass, at half resolution: every pixel here stands for one pixel of a 2 x 2 block of the
// picture (g_PassParams.xy says which this frame). rgb: what the mirrored ray brought back, a: how far the surface is
// from the eye (the reflection pass lays the result over the picture by that; 0: no ray was traced).
// g_SceneColor: the picture of the frame before (if there is one), g_PostDepth: the scene depth, g_PostAux: the surface target.
// A mirror: a smooth surface that returns much of the light (chrome, polished metal; not lacquer, which returns a
// tenth). Its mirrored picture is traced for every pixel, in the reflection pass itself: at half resolution a wheel's
// spokes showed a blurred one.
bool SharpMirror(float4 surface) {
	return surface.a > 0.35 && surface.z < 0.12;
}

float4 PSRayTrace(FullscreenOutput input) : SV_Target {
	// (always the same pixel of each 2 x 2 block: a different one every frame, for the temporal antialiasing to gather,
	// made sharp mirrored edges shimmer whenever the camera moved)
	int2 p = int2(input.position.xy) * 2;
	if (any(p >= int2(g_ScreenSize.xy))) {
		return 0.0;
	}
	float4 surface = g_PostAux.Load(int3(p, 0));
	float depth = g_PostDepth.Load(int3(p, 0)).r;
	if (MirrorAmount(surface) <= 0.002 || depth <= 0.0 || SharpMirror(surface)) {
		return 0.0;
	}
	float2 ndc = (float2(p) + 0.5) * g_ScreenSize.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 h = mul(g_InvViewProj, float4(ndc, depth, 1.0));
	float3 P = h.xyz / h.w;
	float viewDepth = abs(1.0 / h.w);
	float3 N = OctDecode(surface.xy);
	float3 R = reflect(-normalize(g_CameraPos.xyz - P), MirrorNormal(p, N, viewDepth));
	// (with the sun where the ray reaches it: the sun's image in the lacquer)
	return float4(min(TracedRadiance(P + N * (0.004 * viewDepth + 0.01), R, true), 300.0), viewDepth);
}

float4 PSSsr(FullscreenOutput input) : SV_Target {
	int2 p = int2(input.position.xy);
	float4 surface = g_PostAux.Load(int3(p, 0));
	float depth = g_PostDepth.Load(int3(p, 0)).r;
	float amount = MirrorAmount(surface);
	const bool onScreen = g_SsrParams.x > 0.5, rays = g_QualityParams.w > 0.5;
	if (amount <= 0.002 || depth <= 0.0 || (!onScreen && !rays)) {
		return 0.0;
	}
	float2 ndc = input.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 h = mul(g_InvViewProj, float4(ndc, depth, 1.0));
	float3 P = h.xyz / h.w;
	float viewDepth = abs(1.0 / h.w); // (for a perspective projection: the clip space w of the point)
	float3 N = OctDecode(surface.xy);
	float3 V = normalize(g_CameraPos.xyz - P);
	float3 shadedR = reflect(-V, N); // (what the surface was shaded with: along the direction its own normal gave)
	float3 R = reflect(-V, MirrorNormal(p, N, viewDepth));
	float3 origin = P + N * (0.004 * viewDepth + 0.01);
	// ---- 1. on screen. `share`: how much of the answer the screen gives (it fades towards the picture's edge).
	float share = 0.0;
	float3 mirrored = 0.0;
	if (onScreen) {
		// the ray: steps that grow with the distance, a different start per pixel so that the steps do not show as bands
		const int kSteps = 48;
		// (no different start per pixel on a mirror: there the screen's answer and the rays' alternated from pixel to
		// pixel where the march only just found something, and the mirror showed a stipple)
		float dither = SharpMirror(surface) ? 0.5 : frac(52.9829189 * frac(dot(input.position.xy, float2(0.06711056, 0.00583715))));
		float reach = clamp(viewDepth * 4.0, 4.0, 400.0);
		float before = 0.0, found = -1.0;
		for (int i = 0; i < kSteps; ++i) {
			float f = (float(i) + dither) / float(kSteps);
			float t = reach * f * f;
			float4 c = mul(g_ViewProj, float4(origin + R * t, 1.0));
			if (c.w <= 0.05) {
				break; // behind the eye
			}
			float2 uv = c.xy / c.w * float2(0.5, -0.5) + 0.5;
			if (any(uv < 0.0) || any(uv > 1.0)) {
				break; // off the screen: nothing known there
			}
			float d = g_PostDepth.SampleLevel(s_PointClamp, uv, 0.0).r;
			if (d > 0.0) {
				float sceneW = abs(1.0 / dot(g_InvViewProj[3], float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), d, 1.0)));
				float behind = c.w - sceneW;
				// (met: the ray went behind what the depth buffer shows there, by no more than about a step)
				if (behind > 0.0 && behind < (t - before) * 1.5 + 0.02 * sceneW) {
					found = t;
					break;
				}
			}
			before = t;
		}
		if (found >= 0.0) {
			// the place between the last two steps where the ray goes behind the surface
			float lo = before, hi = found;
			for (int k = 0; k < 5; ++k) {
				float mid = 0.5 * (lo + hi);
				float4 c = mul(g_ViewProj, float4(origin + R * mid, 1.0));
				float2 uv = c.xy / c.w * float2(0.5, -0.5) + 0.5;
				float d = g_PostDepth.SampleLevel(s_PointClamp, uv, 0.0).r;
				float sceneW = d > 0.0 ? abs(1.0 / dot(g_InvViewProj[3], float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), d, 1.0))) : 1e9;
				if (c.w > sceneW) {
					hi = mid;
				} else {
					lo = mid;
				}
			}
			float4 prev = mul(g_PrevViewProj, float4(origin + R * hi, 1.0));
			if (prev.w > 0.05) {
				float2 previousUv = prev.xy / prev.w * float2(0.5, -0.5) + 0.5;
				float2 edge = min(previousUv, 1.0 - previousUv);
				share = saturate(min(edge.x, edge.y) / max(g_SsrParams.w * 0.5, 1e-3));
				if (share > 0.0) {
					mirrored = min(g_SceneColor.SampleLevel(s_LinearClamp, previousUv, 0.0).rgb, 64.0);
				}
			}
		}
	}
	float3 shaded = EnvironmentSpecular(P, shadedR, surface.z);
	// ---- 2. and 3. the scene's triangles, where the screen did not answer (or only in part)
	float3 result = shaded;
	if (rays && share < 0.999 && SharpMirror(surface)) {
		result = min(TracedRadiance(origin, R, true), 600.0);
	} else if (rays && share < 0.999) {
		// the traced rays (g_PostAux2, half resolution): the four around this pixel, each counted by how near it lies and
		// by whether it was traced for the same surface (its distance from the eye agrees)
		float2 at = float2(p) * 0.5;
		int2 base = int2(floor(at));
		float2 f = at - float2(base);
		float3 sum = 0.0;
		float weight = 0.0;
		for (int k = 0; k < 4; ++k) {
			int2 o = int2(k & 1, k >> 1);
			float4 ray = g_PostAux2.Load(int3(max(base + o, 0), 0));
			float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y) + 0.02;
			w *= ray.a > 0.0 ? exp(-abs(ray.a - viewDepth) / (0.03 * viewDepth + 0.02)) : 0.0;
			sum += ray.rgb * w;
			weight += w;
		}
		if (weight > 1e-4) {
			result = sum / weight;
		}
	}
	result = lerp(result, mirrored, share);
	if (!SharpMirror(surface)) {
		// Lacquer and other smooth surfaces: a milder version of the curve below. What is bright in the surroundings (the
		// sky's light parts, clouds, the sun, lit walls) stands out in the paint, what is dark stays dark: the wet gloss
		// of a polished car. (Not physics either.)
		float mean = max(dot(AmbientDiffuse(float3(0.0, 1.0, 0.0)) * g_Misc.w, float3(0.2126, 0.7152, 0.0722)), 1e-4);
		float lum = max(dot(result, float3(0.2126, 0.7152, 0.0722)), 1e-5);
		result *= clamp(pow(lum / mean, 0.5), 0.35, 2.4); // (0.6 and up to 3 at first: a little too strong)
	}
	if (SharpMirror(surface)) {
		// Chrome's look is its contrast: what is darker than the sky's average goes darker, what is brighter goes
		// brighter. (A curve on the mirrored picture, not physics: the user's choice. Around the sky's mean the picture
		// is left as it is.)
		float mean = max(dot(AmbientDiffuse(float3(0.0, 1.0, 0.0)) * g_Misc.w, float3(0.2126, 0.7152, 0.0722)), 1e-4);
		float lum = max(dot(result, float3(0.2126, 0.7152, 0.0722)), 1e-5);
		result *= clamp(pow(lum / mean, 1.4), 0.04, 5.0); // (0.7 at first: the user asked for more)
	}
	return float4((result - shaded) * amount, 0.0);
}

// ---- the physical sky's LUTs (aether_atmosphere.hlsli)
float4 PSTransmittanceLut(FullscreenOutput input) : SV_Target {
	float radius = kAtmoGround + input.uv.y * input.uv.y * (kAtmoTop - kAtmoGround);
	return float4(IntegrateTransmittance(max(radius, kAtmoGround + 0.0005), input.uv.x * 2.0 - 1.0), 1.0);
}

float4 PSMultiScatterLut(FullscreenOutput input) : SV_Target {
	float radius = clamp(kAtmoGround + input.uv.y * (kAtmoTop - kAtmoGround), kAtmoGround + 0.0005, kAtmoTop - 0.0005);
	return float4(IntegrateMultipleScattering(radius, input.uv.x * 2.0 - 1.0), 1.0);
}

float4 PSSkyViewLut(FullscreenOutput input) : SV_Target {
	return float4(IntegrateSkyView(SkyViewDirection(input.uv), g_SunDirection.xyz, g_Atmosphere.z), 1.0);
}

// ---- clouds (aether_clouds.hlsli)
uint CloudHash(int2 p) {
	uint n = uint(p.x) * 1597334677u ^ uint(p.y) * 3812015801u;
	n = (n ^ (n >> 15)) * 2747636419u;
	return n ^ (n >> 13);
}

int2 CloudWrap(int2 p, int period) {
	return ((p % period) + period) % period;
}

// smooth noise that repeats every `period` cells
float CloudValueNoise(float2 p, int period) {
	int2 i = int2(floor(p));
	float2 f = frac(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = float(CloudHash(CloudWrap(i, period)) & 0xFFFFu) / 65535.0;
	float b = float(CloudHash(CloudWrap(i + int2(1, 0), period)) & 0xFFFFu) / 65535.0;
	float c = float(CloudHash(CloudWrap(i + int2(0, 1), period)) & 0xFFFFu) / 65535.0;
	float d = float(CloudHash(CloudWrap(i + int2(1, 1), period)) & 0xFFFFu) / 65535.0;
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// cells (1 at a cell's point, falling off around it), repeating every `period` cells
float CloudWorley(float2 p, int period) {
	int2 i = int2(floor(p));
	float2 f = frac(p);
	float nearest = 4.0;
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			uint h = CloudHash(CloudWrap(i + int2(x, y), period));
			float2 site = float2(float(h & 0xFFFFu), float(h >> 16)) / 65535.0;
			float2 d = float2(x, y) + site - f;
			nearest = min(nearest, dot(d, d));
		}
	}
	return 1.0 - saturate(sqrt(nearest));
}

// the tiling noise the clouds' shape comes from: R the big shapes, G the detail
float4 PSCloudNoise(FullscreenOutput input) : SV_Target {
	float2 uv = input.uv;
	float value = CloudValueNoise(uv * 4.0, 4) * 0.5 + CloudValueNoise(uv * 8.0, 8) * 0.25 + CloudValueNoise(uv * 16.0, 16) * 0.125 + CloudValueNoise(uv * 32.0, 32) * 0.0625;
	value /= 0.9375;
	float cells = CloudWorley(uv * 5.0, 5) * 0.55 + CloudWorley(uv * 11.0, 11) * 0.3 + CloudWorley(uv * 23.0, 23) * 0.15;
	// (spread over the whole 0 .. 1 range: the coverage setting is then about the share of the sky the clouds take)
	float shape = saturate((value * 0.62 + cells * 0.58 - 0.30) / 0.42);
	float detail = CloudWorley(uv * 16.0, 16) * 0.5 + CloudWorley(uv * 32.0, 32) * 0.3 + CloudWorley(uv * 64.0, 64) * 0.2;
	return float4(shape, detail, 0.0, 1.0);
}

// The sky with the clouds in it, in the sky view LUT's layout and units (g_SceneColor: the air's sky view LUT).
float4 PSSkyCloudsLut(FullscreenOutput input) : SV_Target {
	float3 dir = SkyViewDirection(input.uv);
	float3 air = g_SceneColor.SampleLevel(s_Aniso, float2(input.uv.x, clamp(input.uv.y, 0.5 / 108.0, 1.0 - 0.5 / 108.0)), 0.0).rgb;
	float4 cloud = MarchClouds(dir, g_Atmosphere.z, 12, 0.5);
	return float4(air * cloud.a + cloud.rgb / max(g_Atmosphere.x, 1e-6), 1.0);
}

// The clouds as the camera sees them (g_SceneColor: the air's sky view LUT): rgb their light, a what they let through.
float4 PSClouds(FullscreenOutput input) : SV_Target {
	float2 ndc = input.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 world = mul(g_InvViewProj, float4(ndc, 0.5, 1.0));
	float3 dir = normalize(world.xyz / world.w - g_CameraPos.xyz);
	float2 noiseAt = input.position.xy + (g_PostParams.w > 0.5 ? frac(g_CameraPos.w * 5.17) * float2(31.0, 59.0) : float2(0.0, 0.0));
	float jitter = frac(52.9829189 * frac(dot(noiseAt, float2(0.06711056, 0.00583715))));
	return MarchClouds(dir, g_Atmosphere.z, 24, jitter);
}

// ---- volumetric fog. For every pixel (at half resolution): the way from the camera to the surface, in steps; at
// each step the fog there scatters towards the camera what reaches it: the sun, unless the shadow map or a cloud is
// in the way, and the sky's light from all around. rgb: that light, a: what the fog lets through from behind.
// g_PostDepth: the scene depth.
float FogShadow(float3 p, float viewDepth) {
	uint count = (uint)g_ShadowParams.x;
	if (count == 0 || g_SunColor.w < 0.5 || viewDepth >= g_CascadeSplits[count - 1]) {
		return 1.0;
	}
	uint cascade = 0;
	for (uint i = 0; i < count; ++i) {
		cascade = i;
		if (viewDepth < g_CascadeSplits[i]) {
			break;
		}
	}
	float4 sc = mul(g_ShadowMatrix[cascade], float4(p, 1.0));
	return g_ShadowAtlas.SampleCmpLevelZero(s_Shadow, sc.xy, sc.z - g_ShadowParams.z);
}

float4 PSVolumetricFog(FullscreenOutput input) : SV_Target {
	float depth = g_PostDepth.SampleLevel(s_PointClamp, input.uv, 0.0).r;
	if (depth <= 0.0 || g_Fog.a <= 0.0) {
		return float4(0.0, 0.0, 0.0, 1.0); // the sky has its own haze
	}
	float3 end = WorldFromDepth(input.uv, depth);
	float3 toEnd = end - g_CameraPos.xyz;
	float distance = length(toEnd);
	float3 dir = toEnd / max(distance, 1e-4);
	if (g_FogHeight.z > 0.0) {
		distance = min(distance, g_FogHeight.z);
	}
	float3 forward = -float3(g_View[2][0], g_View[2][1], g_View[2][2]);
	float c = dot(dir, g_SunDirection.xyz);
	// forward scattering haze (Henyey-Greenstein)
	const float g = 0.45;
	float phase = (1.0 - g * g) / (4.0 * PI * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5));
	float3 tint = g_FogHeight.w > 0.5 ? g_Fog.rgb : float3(1.0, 1.0, 1.0);
	float3 sun = g_SunDirection.w > 0.5 ? g_SunColor.rgb * phase : float3(0.0, 0.0, 0.0);
	// the sky's light arriving in the fog from all around: about the sky's mean radiance
	float3 sky = (AmbientDiffuse(float3(0.0, 1.0, 0.0)) * 0.6 + AmbientDiffuse(dir) * 0.4) * g_Misc.w;
	if (g_FogHeight.w < 0.5) {
		sky = g_Fog.rgb; // (a fog colour set by the scene: used as the fog's ambient light)
	}
	const int kSteps = 24;
	float2 noiseAt = input.position.xy + (g_PostParams.w > 0.5 ? frac(g_CameraPos.w * 3.71) * float2(23.0, 41.0) : float2(0.0, 0.0));
	float jitter = frac(52.9829189 * frac(dot(noiseAt, float2(0.06711056, 0.00583715))));
	float dt = distance / float(kSteps);
	float through = 1.0;
	float3 light = 0.0;
	for (int i = 0; i < kSteps; ++i) {
		float t = dt * (float(i) + jitter);
		float3 p = g_CameraPos.xyz + dir * t;
		float density = g_Fog.a * exp(-max(p.y - g_FogHeight.x, 0.0) * g_FogHeight.y);
		float lit = FogShadow(p, t * dot(dir, forward)) * CloudShadow(p);
		float step = exp(-density * dt);
		light += through * (sun * lit + sky) * tint * (1.0 - step);
		through *= step;
	}
	return float4(light, through);
}

// Lays the fog over the picture (blended: picture * a + rgb). g_SceneColor: the fog at half resolution; read at the
// one of its four nearest texels whose depth is closest to this pixel's, so that the fog of what is behind does not
// spill over the edges of what is in front.
float4 PSVolumetricFogApply(FullscreenOutput input) : SV_Target {
	float depth = g_PostDepth.Load(int3(input.position.xy, 0)).r;
	if (depth <= 0.0) {
		return float4(0.0, 0.0, 0.0, 1.0);
	}
	float2 half = g_ScreenSize.zw * 2.0; // a texel of the half resolution picture
	float2 base = (floor(input.uv / half - 0.5) + 0.5) * half;
	float4 best = 0.0;
	float bestDifference = 1e9;
	float4 mean = 0.0;
	for (int y = 0; y <= 1; ++y) {
		for (int x = 0; x <= 1; ++x) {
			float2 uv = base + float2(x, y) * half;
			float d = g_PostDepth.SampleLevel(s_PointClamp, uv, 0.0).r;
			float4 fog = g_SceneColor.SampleLevel(s_PointClamp, uv, 0.0);
			mean += fog * 0.25;
			float difference = abs(d - depth);
			if (difference < bestDifference) {
				bestDifference = difference;
				best = fog;
			}
		}
	}
	// (all four on the same surface: their mean, which is smoother)
	return bestDifference < depth * 0.02 ? lerp(best, mean, 0.5) : best;
}

// ---- bloom. Down: every level is half the size of the one above, read with 13 taps (no flicker from single bright
// pixels; the first level also weighs each group of taps by its brightness). Up: the level below scaled up with a
// 3 x 3 tent, mixed with this level's own picture by the scatter value.
float4 PSBloomDown(FullscreenOutput input) : SV_Target {
	float2 t = g_PassParams.zw * 0.5; // a texel of the source
	float2 uv = input.uv;
	float3 a = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(-2, -2), 0).rgb;
	float3 b = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(0, -2), 0).rgb;
	float3 c = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(2, -2), 0).rgb;
	float3 d = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(-2, 0), 0).rgb;
	float3 e = g_SceneColor.SampleLevel(s_LinearClamp, uv, 0).rgb;
	float3 f = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(2, 0), 0).rgb;
	float3 g = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(-2, 2), 0).rgb;
	float3 h = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(0, 2), 0).rgb;
	float3 i = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(2, 2), 0).rgb;
	float3 j = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(-1, -1), 0).rgb;
	float3 k = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(1, -1), 0).rgb;
	float3 l = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(-1, 1), 0).rgb;
	float3 m = g_SceneColor.SampleLevel(s_LinearClamp, uv + t * float2(1, 1), 0).rgb;
	float3 g0 = (j + k + l + m) * 0.25, g1 = (a + b + d + e) * 0.25, g2 = (b + c + e + f) * 0.25, g3 = (d + e + g + h) * 0.25, g4 = (e + f + h + i) * 0.25;
	if (g_PassParams.x > 0.5) {
		float w0 = 1.0 / (1.0 + Luma(g0)), w1 = 1.0 / (1.0 + Luma(g1)), w2 = 1.0 / (1.0 + Luma(g2)), w3 = 1.0 / (1.0 + Luma(g3)), w4 = 1.0 / (1.0 + Luma(g4));
		float3 sum = g0 * w0 * 0.5 + (g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) * 0.125;
		return float4(sum / (w0 * 0.5 + (w1 + w2 + w3 + w4) * 0.125), 1.0);
	}
	return float4(g0 * 0.5 + (g1 + g2 + g3 + g4) * 0.125, 1.0);
}

float4 PSBloomUp(FullscreenOutput input) : SV_Target {
	float2 t = g_PassParams.zw; // a texel of the lower level
	float3 low = 0.0;
	low += g_SceneColor.SampleLevel(s_LinearClamp, input.uv + t * float2(-1, -1), 0).rgb;
	low += g_SceneColor.SampleLevel(s_LinearClamp, input.uv + t * float2(0, -1), 0).rgb * 2.0;
	low += g_SceneColor.SampleLevel(s_LinearClamp, input.uv + t * float2(1, -1), 0).rgb;
	low += g_SceneColor.SampleLevel(s_LinearClamp, input.uv + t * float2(-1, 0), 0).rgb * 2.0;
	low += g_SceneColor.SampleLevel(s_LinearClamp, input.uv, 0).rgb * 4.0;
	low += g_SceneColor.SampleLevel(s_LinearClamp, input.uv + t * float2(1, 0), 0).rgb * 2.0;
	low += g_SceneColor.SampleLevel(s_LinearClamp, input.uv + t * float2(-1, 1), 0).rgb;
	low += g_SceneColor.SampleLevel(s_LinearClamp, input.uv + t * float2(0, 1), 0).rgb * 2.0;
	low += g_SceneColor.SampleLevel(s_LinearClamp, input.uv + t * float2(1, 1), 0).rgb;
	low *= 1.0 / 16.0;
	float3 high = g_PostDepth.SampleLevel(s_LinearClamp, input.uv, 0).rgb; // (second input: this level's own picture)
	return float4(lerp(high, low, g_PassParams.x), 1.0);
}

// ---- screen space ambient occlusion
float3 WorldFromDepth(float2 uv, float depth) {
	float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 world = mul(g_InvViewProj, float4(ndc, depth, 1.0));
	return world.xyz / world.w;
}

float3 WorldAtPixel(int2 pixel, out float depth) {
	depth = g_PostDepth.Load(int3(pixel, 0)).r;
	return WorldFromDepth((float2(pixel) + 0.5) * g_ScreenSize.zw, depth);
}

// Obscurance from the depth buffer alone: the surface normal comes from the depth's own slopes (the nearer neighbour
// on each axis, so that silhouettes do not bend it), samples go out on a rotated spiral within the host's radius.
// Ground truth ambient occlusion: a port of the main pass of Intel's XeGTAO (MIT licence, third_party/XeGTAO), which
// implements Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect Occlusion". For a few directions on
// screen (slices) the highest thing in the depth buffer on either side of the pixel is found (the horizons); the part
// of the hemisphere between the horizons, weighted by the cosine to the normal, is what the sky can reach.
// Here: a pixel shader on the full resolution depth buffer (no depth mips), view space through Aether's matrices,
// followed by Aether's own blur and temporal antialiasing instead of XeGTAO's denoiser.
float GtaoFastACos(float x) {
	float a = abs(x);
	float r = (-0.156583 * a + 1.570796) * sqrt(1.0 - a);
	return x >= 0.0 ? r : PI - r;
}

float3 ViewAtPixel(int2 pixel, out float depth) {
	float3 world = WorldAtPixel(pixel, depth);
	return mul(g_View, float4(world, 1.0)).xyz;
}

// The picture of the frame before at the place a view space position was then (g_PostAux: the temporal history).
float3 BouncedFrom(float3 viewPos) {
	// view -> world: the view matrix is a rotation and a translation
	float3 rel = viewPos - float3(g_View[0][3], g_View[1][3], g_View[2][3]);
	float3 world = float3(dot(float3(g_View[0][0], g_View[1][0], g_View[2][0]), rel), dot(float3(g_View[0][1], g_View[1][1], g_View[2][1]), rel), dot(float3(g_View[0][2], g_View[1][2], g_View[2][2]), rel));
	float4 clip = mul(g_PrevViewProj, float4(world, 1.0));
	if (clip.w <= 0.05) {
		return 0.0;
	}
	float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
	if (any(uv < 0.0) || any(uv > 1.0)) {
		return 0.0;
	}
	return min(g_PostAux.SampleLevel(s_LinearClamp, uv, 0.0).rgb, 16.0);
}

float4 PSAmbientOcclusion(FullscreenOutput input) : SV_Target {
	int2 pixel = int2(input.position.xy);
	int2 last = int2(g_ScreenSize.xy) - 1;
	float depth, dl, dr, du, dd;
	float3 P = ViewAtPixel(pixel, depth);
	if (depth <= 0.0) {
		return float4(0.0, 0.0, 0.0, 1.0); // the far plane: sky
	}
	// the normal from the depth buffer: across each axis, the neighbour on the same surface (the nearer in depth)
	float3 Pl = ViewAtPixel(pixel + int2(-1, 0), dl), Pr = ViewAtPixel(pixel + int2(1, 0), dr);
	float3 Pu = ViewAtPixel(pixel + int2(0, -1), du), Pd = ViewAtPixel(pixel + int2(0, 1), dd);
	float3 dx = abs(dl - depth) < abs(dr - depth) ? P - Pl : Pr - P;
	float3 dy = abs(du - depth) < abs(dd - depth) ? P - Pu : Pd - P;
	float3 N = normalize(cross(dy, dx));
	float3 viewVec = normalize(-P);
	if (dot(N, viewVec) < 0.0) {
		N = -N;
	}
	float viewDepth = max(-P.z, 0.01); // distance in front of the camera (it looks down -Z)
	// (moved a little towards the camera: depth buffer precision)
	P *= 0.99999;

	// the radius in world units and on screen; on screen it is capped (cost, and cache misses on the depth buffer)
	const float kMaxPixels = 64.0;
	float pixelSize = viewDepth * 2.0 / (g_Proj[1][1] * g_ScreenSize.y); // one pixel at this depth, in metres
	float effectRadius = min(g_AOParams.y, kMaxPixels * pixelSize);
	float screenRadius = effectRadius / pixelSize;
	if (screenRadius < 1.5) {
		return float4(0.0, 0.0, 0.0, 1.0);
	}
	const float kFalloffRange = 0.615, kDistributionPower = 2.0, kFinalPower = 2.2;
	float falloffRange = kFalloffRange * effectRadius;
	float falloffFrom = effectRadius * (1.0 - kFalloffRange);
	float falloffMul = -1.0 / falloffRange;
	float falloffAdd = falloffFrom / falloffRange + 1.0;

	// noise per pixel (and per frame while the temporal antialiasing is there to average it)
	float2 noiseAt = input.position.xy + (g_PostParams.w > 0.5 ? frac(g_CameraPos.w * 7.31) * float2(47.0, 17.0) : float2(0.0, 0.0));
	float noiseSlice = frac(52.9829189 * frac(dot(noiseAt, float2(0.06711056, 0.00583715))));
	float noiseSample = frac(noiseSlice * 7.0 + 0.618034);

	const int kSlices = 3, kSteps = 3;
	const float kTooClose = 1.3; // offsets under about a pixel are pushed out: the centre pixel says nothing
	float minS = kTooClose / screenRadius;
	float visibility = saturate((10.0 - screenRadius) / 100.0) * 0.5; // (fades out for small radii on screen)
	// Bounced light (Aether's addition to the occlusion search): whatever raises a horizon blocks a piece of the sky
	// there, and sends its own light instead. Its colour is read from the picture of the frame before; its weight is
	// how much it raised the horizon, times the cosine to the normal.
	const bool bounce = g_QualityParams.z > 0.0 && g_Jitter.z > 0.5;
	float3 bounced = 0.0;
	for (int slice = 0; slice < kSlices; ++slice) {
		float phi = (float(slice) + noiseSlice) / float(kSlices) * PI;
		float cosPhi = cos(phi), sinPhi = sin(phi);
		float2 omega = float2(cosPhi, -sinPhi) * screenRadius; // on screen (y down), in pixels
		float3 directionVec = float3(cosPhi, sinPhi, 0.0);     // the same direction in view space (y up)
		float3 orthoDirectionVec = directionVec - dot(directionVec, viewVec) * viewVec;
		float3 axisVec = normalize(cross(orthoDirectionVec, viewVec));
		float3 projectedNormal = N - axisVec * dot(N, axisVec);
		float signNorm = sign(dot(orthoDirectionVec, projectedNormal));
		float projectedLength = length(projectedNormal);
		float cosNorm = saturate(dot(projectedNormal, viewVec) / max(projectedLength, 1e-5));
		float n = signNorm * GtaoFastACos(cosNorm);
		// (the lowest a horizon counts: the tangent plane on that side)
		float lowHorizonCos0 = cos(n + 0.5 * PI), lowHorizonCos1 = cos(n - 0.5 * PI);
		float horizonCos0 = lowHorizonCos0, horizonCos1 = lowHorizonCos1;
		for (int step = 0; step < kSteps; ++step) {
			float stepNoise = frac(noiseSample + float(slice + step * kSteps) * 0.6180339887);
			float s = pow((float(step) + stepNoise) / float(kSteps), kDistributionPower) + minS;
			float2 offset = round(s * omega); // (snapped to pixel centres: the slope is measured between real depths)
			float d0, d1;
			float3 S0 = ViewAtPixel(clamp(pixel + int2(offset), int2(0, 0), last), d0);
			float3 S1 = ViewAtPixel(clamp(pixel - int2(offset), int2(0, 0), last), d1);
			float3 delta0 = S0 - P, delta1 = S1 - P;
			float dist0 = length(delta0), dist1 = length(delta1);
			// (a sample past the radius counts less and less; the sky counts as not there)
			float weight0 = d0 > 0.0 ? saturate(dist0 * falloffMul + falloffAdd) : 0.0;
			float weight1 = d1 > 0.0 ? saturate(dist1 * falloffMul + falloffAdd) : 0.0;
			float shc0 = lerp(lowHorizonCos0, dot(delta0 / max(dist0, 1e-6), viewVec), weight0);
			float shc1 = lerp(lowHorizonCos1, dot(delta1 / max(dist1, 1e-6), viewVec), weight1);
			if (bounce) {
				float rise0 = max(shc0 - horizonCos0, 0.0), rise1 = max(shc1 - horizonCos1, 0.0);
				if (rise0 > 0.0) {
					bounced += BouncedFrom(S0) * rise0 * saturate(dot(N, delta0 / max(dist0, 1e-6)));
				}
				if (rise1 > 0.0) {
					bounced += BouncedFrom(S1) * rise1 * saturate(dot(N, delta1 / max(dist1, 1e-6)));
				}
			}
			horizonCos0 = max(horizonCos0, shc0);
			horizonCos1 = max(horizonCos1, shc1);
		}
		projectedLength = lerp(projectedLength, 1.0, 0.05); // (XeGTAO's correction for steep slopes)
		float h0 = -GtaoFastACos(horizonCos1);
		float h1 = GtaoFastACos(horizonCos0);
		float iarc0 = (cosNorm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
		float iarc1 = (cosNorm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
		visibility += projectedLength * (iarc0 + iarc1);
	}
	visibility = pow(saturate(visibility / float(kSlices)), kFinalPower);
	visibility = max(visibility, 0.03);
	// the scene's own strength setting decides how much of the occlusion shows
	// (bounced: the horizons' rises add up to at most 2 per slice; as light per unit of the sky's light, like the irradiance map)
	return float4(bounced / float(kSlices), lerp(1.0, visibility, saturate(g_AOParams.x)));
}

// 4 x 4 box over the noise pattern, weighted by how close each sample's depth is to the centre's.
float4 PSAmbientOcclusionBlur(FullscreenOutput input) : SV_Target {
	int2 pixel = int2(input.position.xy);
	float depth = g_PostDepth.Load(int3(pixel, 0)).r;
	if (depth <= 0.0) {
		return float4(0.0, 0.0, 0.0, 1.0);
	}
	// (rgb: the bounced light, a: the occlusion; both blurred across pixels of the same surface)
	float4 sum = 0.0;
	float weight = 0.0;
	for (int y = -2; y <= 1; ++y) {
		for (int x = -2; x <= 1; ++x) {
			int2 p = clamp(pixel + int2(x, y), int2(0, 0), int2(g_ScreenSize.xy) - 1);
			float d = g_PostDepth.Load(int3(p, 0)).r;
			float w = saturate(1.0 - abs(d - depth) / (depth * 0.03 + 1e-7));
			sum += g_SceneColor.Load(int3(p, 0)) * w;
			weight += w;
		}
	}
	return weight > 0.0 ? sum / weight : g_SceneColor.Load(int3(pixel, 0));
}

// Automatic exposure without the host: the geometric mean of the scene's luminance over a 32 x 18 grid (each point the
// mean of four pixels), as an EV100 kept inside the host's limits, written as the exposure multiplier. The value moves
// from the one of the frame before (g_PostAux, 1 x 1) by g_ExposureParams.w: one frame's metering alone follows every
// highlight that crosses a grid point, which shows as a picture that flickers.
float4 PSExposure(FullscreenOutput input) : SV_Target {
	const int kGridX = 32, kGridY = 18;
	float sum = 0.0;
	for (int y = 0; y < kGridY; ++y) {
		for (int x = 0; x < kGridX; ++x) {
			// a pixel corner: the linear sampler returns the mean of the four pixels around it
			float2 uv = floor((float2(x, y) + 0.5) / float2(kGridX, kGridY) * g_ScreenSize.xy) / g_ScreenSize.xy;
			float3 c = g_SceneColor.SampleLevel(s_LinearClamp, uv, 0.0).rgb;
			sum += log2(max(dot(c, float3(0.2126, 0.7152, 0.0722)), 1e-4));
		}
	}
	float mean = exp2(sum / float(kGridX * kGridY));
	float ev = log2(mean * 100.0 / 12.5) - g_ExposureParams.z;
	if (g_Atmosphere.w > 0.5) {
		// the physical sky: evenings and nights are not exposed all the way up to a daylight picture
		ev = g_ExposureParams.y - (g_ExposureParams.y - ev) * 0.8;
	}
	ev = clamp(ev, g_ExposureParams.x, g_ExposureParams.y);
	if (g_ExposureParams.w < 1.0) {
		float previous = g_PostAux.Load(int3(0, 0, 0)).r;
		if (previous > 0.0 && previous < 1e6) { // (a NaN fails both: this frame's value alone)
			float previousEv = clamp(-log2(1.2 * previous), g_ExposureParams.x, g_ExposureParams.y);
			ev = lerp(previousEv, ev, g_ExposureParams.w);
		}
	}
	float exposure = 1.0 / (1.2 * exp2(ev));
	return float4(exposure, ev, 0.0, 1.0);
}

// AgX (Troy Sobotka), in Benjamin Wrensch's minimal form (MIT): the scene's colours go through a matrix that pulls
// bright saturated colours towards white the way film does, a log curve with a fitted S shape, and back.
float3 TonemapAgX(float3 color) {
	const float3x3 kIn = float3x3(0.842479062253094, 0.0784335999999992, 0.0792237451477643,
		0.0423282422610123, 0.878468636469772, 0.0791661274605434,
		0.0423756549057051, 0.0784336, 0.879142973793104);
	const float3x3 kOut = float3x3(1.19687900512017, -0.0980208811401368, -0.0990297440797205,
		-0.0528968517574562, 1.15190312990417, -0.0989611768448433,
		-0.0529716355144438, -0.0980434501171241, 1.15107367264116);
	const float kMinEv = -12.47393, kMaxEv = 4.026069;
	float3 v = mul(kIn, max(color, 0.0));
	v = clamp(log2(max(v, 1e-10)), kMinEv, kMaxEv);
	v = (v - kMinEv) / (kMaxEv - kMinEv);
	// the S curve (a polynomial fit of AgX's default contrast)
	float3 v2 = v * v, v4 = v2 * v2;
	v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
	v = mul(kOut, v);
	return pow(saturate(v), 2.2); // (the curve's output is display encoded: back to linear for the sRGB step after it)
}

float3 TonemapACES(float3 x) {
	// Narkowicz's fit of the ACES filmic curve
	return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float3 TonemapNeutral(float3 x) {
	// Reinhard on luminance, keeps the hue
	float l = dot(x, float3(0.2126, 0.7152, 0.0722));
	return x / (1.0 + l);
}

// The host's custom curve (HDRP "Custom" tone mapping: Hable's piecewise filmic curve), per channel.
float ToneSegment(float x, int i) {
	float4 a = g_ToneSegA[i];
	float xs = (x - a.x) * a.z;
	float ys = xs > 0.0 ? exp(g_ToneSegB[i].x + g_ToneSegB[i].y * log(xs)) : 0.0;
	return ys * a.w + a.y;
}

float3 TonemapHostCurve(float3 c) {
	float3 x = max(c, 0.0) * g_ToneCurve.x;
	float3 r;
	r.x = ToneSegment(x.x, x.x < g_ToneCurve.y ? 0 : x.x < g_ToneCurve.z ? 1 : 2);
	r.y = ToneSegment(x.y, x.y < g_ToneCurve.y ? 0 : x.y < g_ToneCurve.z ? 1 : 2);
	r.z = ToneSegment(x.z, x.z < g_ToneCurve.y ? 0 : x.z < g_ToneCurve.z ? 1 : 2);
	return r;
}

float3 LinearToSRGB(float3 c) {
	c = saturate(c);
	return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

// The final composite into the host's back buffer (a UNORM target: the encoding is done here).
float4 PSComposite(FullscreenOutput input) : SV_Target {
	// the scene target has the size of the back buffer: pixel for pixel
	float3 color = g_SceneColor.Load(int3(input.position.xy, 0)).rgb;
	if ((int)g_Misc.x == VIEW_NONE || (int)g_Misc.x >= VIEW_SUN) {
		float exposure = g_Exposure.x;
		if (g_Exposure.y > 1.5) {
			exposure = g_PostAux.Load(int3(0, 0, 0)).r; // Aether's own metering (PSExposure)
		}
		if (g_PostParams.x > 0.0) {
			// bloom, the host's way: the picture mixed with its blurred pyramid
			float3 bloom = g_PostAux2.SampleLevel(s_LinearClamp, input.uv, 0.0).rgb;
			color = lerp(color, bloom, g_PostParams.x);
		}
		color *= exposure * g_Exposure.z;
		// the host's shadows / midtones / highlights grading (before the tone curve, like the host)
		if (g_SmhLimits.y > 0.0 || g_SmhLimits.w > 0.0) {
			float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
			float shadows = 1.0 - smoothstep(g_SmhLimits.x, g_SmhLimits.y, luma);
			float highlights = smoothstep(g_SmhLimits.z, g_SmhLimits.w, luma);
			float midtones = 1.0 - shadows - highlights;
			color = color * (g_Smh[0].rgb * shadows + g_Smh[1].rgb * midtones + g_Smh[2].rgb * highlights);
		}
		int mode = (int)g_Exposure.w;
		if (mode == 1) color = TonemapACES(color);
		else if (mode == 2) color = TonemapNeutral(color);
		else if (mode == 3) color = TonemapHostCurve(color);
		else if (mode == 4) color = TonemapAgX(color);
	}
	return float4(LinearToSRGB(color), 1.0);
}

// The reflection probes blurred: every texel of the output's tiles is the mean of the same probe over a cone around the
// texel's direction. Premultiplied: the sky's share (alpha 0) stays the sky's.
// g_SceneColor: the probes' atlas. g_PassParams: x the cone's angle (radians), y the input's tile size, z the output's.
float4 PSReflectionBlur(FullscreenOutput input) : SV_Target {
	float tile = g_PassParams.z;
	uint2 cell = uint2(input.position.xy / tile);
	uint slot = min(cell.x / 3u + (cell.y / 2u) * 4u, 7u);
	uint face = cell.x % 3u + (cell.y % 2u) * 3u;
	float2 uv = frac(input.position.xy / tile);
	float3 d = ProbeDirection(face, uv);
	float3 t1 = normalize(cross(d, abs(d.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0)));
	float3 t2 = cross(d, t1);
	const int kTaps = 32;
	float4 sum = 0.0;
	float weight = 0.0;
	for (int i = 0; i < kTaps; ++i) {
		// (a sunflower: even over the disc)
		float r = sqrt((float(i) + 0.5) / float(kTaps));
		float phi = float(i) * 2.39996323;
		float2 o = float2(cos(phi), sin(phi)) * r * tan(g_PassParams.x);
		float w = 1.0 - 0.6 * r * r;
		sum += ProbeSample(g_SceneColor, slot, normalize(d + t1 * o.x + t2 * o.y), g_PassParams.y) * w;
		weight += w;
	}
	return sum / weight;
}

// Debug view: one ray per pixel from the camera through the ray tracing structure. What it shows must be what the
// rasterised picture shows (the same shapes in the same places); the cost view is what a ray had to test on its way.
// g_PassParams.x: 1 = the cost view.
float4 PSBvhDebug(FullscreenOutput input) : SV_Target {
	float2 ndc = input.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 h = mul(g_InvViewProj, float4(ndc, 0.5, 1.0));
	float3 d = normalize(h.xyz / h.w - g_CameraPos.xyz);
	BvhHit hit = BvhTrace(g_CameraPos.xyz, d, 2000.0, false);
	if (g_PassParams.x > 0.5) {
		// blue (few tests) through green and yellow to red (300 and more)
		float c = saturate(float(hit.steps) / 300.0);
		return float4(saturate(float3(c * 3.0 - 1.0, 1.0 - abs(c * 3.0 - 1.5) * 0.9, 1.0 - c * 3.0)), 1.0);
	}
	if (hit.t < 0.0) {
		return float4(0.02, 0.03, 0.06, 1.0);
	}
	float light = 0.25 + 0.75 * saturate(dot(hit.normal, g_SunDirection.xyz));
	return float4((hit.normal * 0.5 + 0.5) * light, 1.0);
}
