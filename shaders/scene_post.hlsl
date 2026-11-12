// Aether full screen passes: sky background and the final composite (exposure, tone mapping, output encoding).
#include "aether_scene.hlsli"

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
				sum += g_Sky.SampleLevel(s_LinearClamp, float4(SkyDirection(dir), 0.0), mip).rgb * w;
				weight += w;
			}
		}
	}
	return float4(sum / max(weight, 1e-5) * g_SkyParams.x, 1.0);
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

// ---- screen space reflections. For every smooth opaque surface (the opaque pass's second target says which): the ray
// the eye's direction is mirrored into is followed through the depth buffer; where it meets something on screen, that
// thing's colour is taken from the picture of the frame before (g_SceneColor here: the temporal history, found through
// the camera of the frame before). The surface was already shaded with the sky / probe reflection in that direction,
// so what is returned (and added to the picture) is the difference, by the share of mirrored light the surface returns.
// g_PostDepth: the scene depth, g_PostAux: the surface target.
float4 PSSsr(FullscreenOutput input) : SV_Target {
	int2 p = int2(input.position.xy);
	float4 surface = g_PostAux.Load(int3(p, 0));
	float depth = g_PostDepth.Load(int3(p, 0)).r;
	float smoothness = 1.0 - surface.z;
	float amount = surface.a * saturate((smoothness - g_SsrParams.y) / max(g_SsrParams.z - g_SsrParams.y, 1e-3));
	if (amount <= 0.002 || depth <= 0.0 || g_SsrParams.x < 0.5) {
		return 0.0;
	}
	float2 ndc = input.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 h = mul(g_InvViewProj, float4(ndc, depth, 1.0));
	float3 P = h.xyz / h.w;
	float viewDepth = abs(1.0 / h.w); // (for a perspective projection: the clip space w of the point)
	float3 N = OctDecode(surface.xy);
	float3 V = normalize(g_CameraPos.xyz - P);
	float3 R = reflect(-V, N);
	// the ray: steps that grow with the distance, a different start per pixel so that the steps do not show as bands
	const int kSteps = 48;
	float dither = frac(52.9829189 * frac(dot(input.position.xy, float2(0.06711056, 0.00583715))));
	float reach = clamp(viewDepth * 4.0, 4.0, 400.0);
	float3 origin = P + N * (0.004 * viewDepth + 0.01);
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
	if (found < 0.0) {
		return 0.0;
	}
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
	float3 hit = origin + R * hi;
	float4 prev = mul(g_PrevViewProj, float4(hit, 1.0));
	if (prev.w <= 0.05) {
		return 0.0;
	}
	float2 previousUv = prev.xy / prev.w * float2(0.5, -0.5) + 0.5;
	float2 edge = min(previousUv, 1.0 - previousUv);
	float fade = saturate(min(edge.x, edge.y) / max(g_SsrParams.w * 0.5, 1e-3));
	if (fade <= 0.0) {
		return 0.0;
	}
	float3 mirrored = min(g_SceneColor.SampleLevel(s_LinearClamp, previousUv, 0.0).rgb, 64.0);
	float3 shaded = EnvironmentSpecular(P, R, surface.z);
	return float4((mirrored - shaded) * amount * fade, 0.0);
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
float4 PSAmbientOcclusion(FullscreenOutput input) : SV_Target {
	int2 pixel = int2(input.position.xy);
	float depth, dl, dr, du, dd;
	float3 P = WorldAtPixel(pixel, depth);
	if (depth <= 0.0) {
		return 1.0; // the far plane: sky
	}
	float3 Pl = WorldAtPixel(pixel + int2(-1, 0), dl), Pr = WorldAtPixel(pixel + int2(1, 0), dr);
	float3 Pu = WorldAtPixel(pixel + int2(0, -1), du), Pd = WorldAtPixel(pixel + int2(0, 1), dd);
	float3 dx = abs(dl - depth) < abs(dr - depth) ? P - Pl : Pr - P;
	float3 dy = abs(du - depth) < abs(dd - depth) ? P - Pu : Pd - P;
	float3 N = normalize(cross(dy, dx));
	float3 toCamera = g_CameraPos.xyz - P;
	if (dot(N, toCamera) < 0.0) {
		N = -N;
	}
	float viewDepth = max(-mul(g_View, float4(P, 1.0)).z, 0.01);
	float radius = g_AOParams.y;
	// the radius on screen, in pixels; far away it shrinks to nothing, up close it is capped (cost and cache misses)
	float radiusPixels = min(radius * g_Proj[1][1] * 0.5 * g_ScreenSize.y / viewDepth, 64.0);
	if (radiusPixels < 1.5) {
		return 1.0;
	}
	float effectiveRadius = radiusPixels * viewDepth / (g_Proj[1][1] * 0.5 * g_ScreenSize.y);
	float noise = frac(52.9829189 * frac(dot(input.position.xy, float2(0.06711056, 0.00583715)))); // interleaved gradient noise
	const int kSamples = 10;
	float occlusion = 0.0;
	for (int i = 0; i < kSamples; ++i) {
		float t = (float(i) + 0.5) / float(kSamples);
		float angle = (float(i) * 2.399963 + noise * 6.2831853);
		float2 offset = float2(cos(angle), sin(angle)) * (t * radiusPixels);
		int2 samplePixel = clamp(int2(input.position.xy + offset), int2(0, 0), int2(g_ScreenSize.xy) - 1);
		float sampleDepth;
		float3 S = WorldAtPixel(samplePixel, sampleDepth);
		if (sampleDepth <= 0.0) {
			continue;
		}
		float3 v = S - P;
		float distance2 = dot(v, v);
		float cosine = dot(N, v) * rsqrt(distance2 + 1e-6);
		float falloff = saturate(1.0 - distance2 / (effectiveRadius * effectiveRadius));
		occlusion += saturate(cosine - 0.08) * falloff;
	}
	float ao = saturate(1.0 - g_AOParams.x * 2.0 * occlusion / float(kSamples));
	return ao;
}

// 4 x 4 box over the noise pattern, weighted by how close each sample's depth is to the centre's.
float4 PSAmbientOcclusionBlur(FullscreenOutput input) : SV_Target {
	int2 pixel = int2(input.position.xy);
	float depth = g_PostDepth.Load(int3(pixel, 0)).r;
	if (depth <= 0.0) {
		return 1.0;
	}
	float sum = 0.0, weight = 0.0;
	for (int y = -2; y <= 1; ++y) {
		for (int x = -2; x <= 1; ++x) {
			int2 p = clamp(pixel + int2(x, y), int2(0, 0), int2(g_ScreenSize.xy) - 1);
			float d = g_PostDepth.Load(int3(p, 0)).r;
			float w = saturate(1.0 - abs(d - depth) / (depth * 0.03 + 1e-7));
			sum += g_SceneColor.Load(int3(p, 0)).r * w;
			weight += w;
		}
	}
	return weight > 0.0 ? sum / weight : g_SceneColor.Load(int3(pixel, 0)).r;
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
	}
	return float4(LinearToSRGB(color), 1.0);
}
