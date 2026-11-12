// Aether's clouds: one layer of cumulus between two heights above the ground, for the physical sky.
// The shape comes from a tiling noise texture (g_CloudNoise: R the big shapes, G the detail that eats into their
// edges; made once by PSCloudNoise), laid out flat over the world and given a height profile, so a ray through the
// layer sees volume and parallax without a 3D texture. Light: the sun through the cloud towards each sample (a few
// steps), scattered forward and back, plus the sky's own light from all around.
// Lengths are kilometres, like the atmosphere's. Included by aether_scene.hlsli after aether_atmosphere.hlsli.

static const float kCloudBase = 1.4; // above the ground
static const float kCloudTop = 2.9;
static const float kCloudExtinction = 14.0; // per km at full density

// How much cloud there is at a place: p.xz over the ground in km, height above the ground in km.
float CloudDensity(float2 xz, float height, bool detailed) {
	float h = saturate((height - kCloudBase) / (kCloudTop - kCloudBase));
	// flat bottoms, round tops
	float profile = saturate(h * 8.0) * saturate((1.0 - h) * 2.2);
	if (profile <= 0.0) {
		return 0.0;
	}
	// (the noise is flat: the lookup slides sideways with the height, so that a cloud's side is not the same pattern
	// drawn out into vertical streaks)
	float2 uv = xz * 0.055 + g_Clouds.yz + h * float2(0.021, -0.013);
	float shape = g_CloudNoise.SampleLevel(s_Aniso, uv, 0.0).r;
	// The coverage setting says how much of the noise counts as cloud. (A ray crosses the layer at a slant and so
	// meets more cloud than a look straight up would: the threshold is set higher than the bare share. The density
	// rises quickly behind the threshold: cumulus has an edge, not a gradient.)
	float threshold = 1.0 - g_Clouds.x * 0.72;
	float d = saturate((shape - threshold) / 0.16) * profile;
	if (detailed && d > 0.0) {
		float detail = g_CloudNoise.SampleLevel(s_Aniso, uv * 5.3 + g_Clouds.yz * 2.0 + h * float2(-0.17, 0.11), 0.0).g;
		d = saturate(d - detail * 0.45 * (1.0 - d * 0.7));
	}
	return d;
}

float CloudPhase(float c) {
	// forward and a little backward scattering (two Henyey-Greenstein lobes)
	const float g1 = 0.78, g2 = -0.25;
	float a = (1.0 - g1 * g1) / (4.0 * PI * pow(max(1.0 + g1 * g1 - 2.0 * g1 * c, 1e-4), 1.5));
	float b = (1.0 - g2 * g2) / (4.0 * PI * pow(max(1.0 + g2 * g2 - 2.0 * g2 * c, 1e-4), 1.5));
	return lerp(a, b, 0.35);
}

// The clouds along a ray from the eye (`altitude` km above the ground) towards `dir`: rgb the light they send
// (scene units), a what they let through of what is behind them. `steps`: 24 for the picture, fewer for the sky view LUT.
float4 MarchClouds(float3 dir, float altitude, int steps, float jitter) {
	if (g_Clouds.w < 0.5 || g_Clouds.x <= 0.01 || dir.y < 0.015) {
		return float4(0.0, 0.0, 0.0, 1.0);
	}
	// the layer as two spheres around the planet (so that it bends down towards the horizon)
	float3 o = float3(0.0, kAtmoGround + altitude, 0.0);
	float t0 = AtmoRaySphere(o, dir, kAtmoGround + kCloudBase, false);
	float t1 = AtmoRaySphere(o, dir, kAtmoGround + kCloudTop, false);
	if (t0 <= 0.0 || t1 <= t0) {
		return float4(0.0, 0.0, 0.0, 1.0);
	}
	t1 = min(t1, t0 + 12.0);
	float3 sun = g_SunDirection.xyz;
	float sunUp = saturate(sun.y * 6.0 + 0.3); // (the sun below the horizon still lights the cloud bases for a while)
	float3 sunLight = TransmittanceToTop(kAtmoGround + kCloudBase, sun.y) * g_Atmosphere.x * sunUp;
	// the sky's light on the clouds: what the air scatters around them
	float3 skyLight = MultipleScatteringGlobal(kAtmoGround + kCloudBase, sun.y) * (4.0 * PI) * g_Atmosphere.x + g_Atmosphere.yyy * float3(0.6, 0.75, 1.0) * PI;
	float phase = CloudPhase(dot(dir, sun));
	float dt = (t1 - t0) / float(steps);
	float through = 1.0;
	float3 light = 0.0;
	for (int i = 0; i < steps; ++i) {
		float t = t0 + dt * (float(i) + jitter);
		float3 p = o + dir * t;
		float height = length(p) - kAtmoGround;
		float2 xz = p.xz + g_CameraPos.xz * 0.001;
		float d = CloudDensity(xz, height, true);
		if (d <= 0.001) {
			continue;
		}
		// the sun through the cloud towards this sample
		float depthToSun = 0.0;
		float reach = 0.12;
		float3 q = p;
		for (int k = 0; k < 4; ++k) {
			q += sun * reach;
			depthToSun += CloudDensity(q.xz + g_CameraPos.xz * 0.001, length(q) - kAtmoGround, false) * reach;
			reach *= 1.7;
		}
		float sunThrough = exp(-depthToSun * kCloudExtinction);
		// (dense parts are darker on the side away from the sun but never black: light scattered many times)
		float powder = 1.0 - exp(-d * 4.0);
		float3 inScatter = sunLight * (sunThrough * phase * lerp(1.0, powder * 2.0, 0.35) + 0.09 * exp(-depthToSun * kCloudExtinction * 0.25))
			+ skyLight * (0.6 + 0.4 * saturate((height - kCloudBase) / (kCloudTop - kCloudBase))) / (4.0 * PI);
		float extinction = d * kCloudExtinction;
		float step = exp(-extinction * dt);
		light += through * inScatter * (1.0 - step); // (albedo 1: clouds absorb next to nothing)
		through *= step;
		if (through < 0.02) {
			through = 0.0;
			break;
		}
	}
	// far clouds sink into the haze: towards the sky's own colour in that direction
	float haze = 1.0 - exp(-t0 * 0.035);
	float3 skyBehind = PhysicalSkyAir(dir);
	light = lerp(light, skyBehind * (1.0 - through), haze);
	return float4(light, through);
}

// The sun's light that the clouds let through to a point on the ground (world position in metres).
float CloudShadow(float3 worldPos) {
	if (g_Clouds.w < 0.5 || g_Clouds.x <= 0.01 || g_SunDirection.y < 0.03) {
		return 1.0;
	}
	float3 sun = g_SunDirection.xyz;
	float3 p = worldPos * 0.001;
	// where the way to the sun crosses the middle of the layer
	float mid = (kCloudBase + kCloudTop) * 0.5;
	float2 xz = p.xz + sun.xz * ((mid - p.y) / sun.y);
	float d = CloudDensity(xz, mid, false);
	return exp(-d * kCloudExtinction * 0.45 * (kCloudTop - kCloudBase) / max(sun.y, 0.15));
}
