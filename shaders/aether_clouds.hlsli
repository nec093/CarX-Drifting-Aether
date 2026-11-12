// Aether's clouds: a layer of cumulus between two heights above the ground, for the physical sky.
//
// The shape comes from a noise that tiles in all three directions (g_CloudNoise, made once by PSCloudNoise: R the big
// billowing shapes, G the small cells that eat into their edges). It is a volume, kept as 64 slices laid out 8 x 8 in
// one texture (kCloudTile texels each): looked up with the place in the layer, so a cloud has a different section at
// every height (the first version drew one flat pattern out through the layer's height, and every cloud had
// streaked, extruded sides). A second, much slower lookup of the same noise is the weather: where the sky is busy and
// where it is clear, and how high the clouds there tower.
// Light: the sun through the cloud towards each sample (a few steps), scattered forward and back and many times over;
// the sky's own light from above and the ground's from below.
// Lengths are kilometres, like the atmosphere's. Included by aether_scene.hlsli after aether_atmosphere.hlsli.

static const float kCloudBase = 1.3; // above the ground
static const float kCloudTop = 3.3;
static const float kCloudExtinction = 16.0; // per km at full density
static const float kCloudTile = 128.0;      // texels of one slice of the noise
static const float kCloudPeriod = 7.0;      // km after which the shapes repeat

// The noise volume at uvw (each 0 .. 1 is one repeat): r shape, g detail.
float2 CloudNoise3(float3 uvw) {
	float z = frac(uvw.z) * 64.0 - 0.5;
	float z0 = floor(z);
	float f = z - z0;
	float2 slices = fmod(float2(z0, z0 + 1.0) + 64.0, 64.0);
	// (inside its slice the lookup stays half a texel off the edge: the next slice over is another height altogether)
	float2 uv = clamp(frac(uvw.xy), 0.5 / kCloudTile, 1.0 - 0.5 / kCloudTile);
	float2 t0 = (float2(fmod(slices.x, 8.0), floor(slices.x / 8.0)) + uv) / 8.0;
	float2 t1 = (float2(fmod(slices.y, 8.0), floor(slices.y / 8.0)) + uv) / 8.0;
	return lerp(g_CloudNoise.SampleLevel(s_LinearClamp, t0, 0.0).rg, g_CloudNoise.SampleLevel(s_LinearClamp, t1, 0.0).rg, f);
}

// The weather over a place (km over the ground): x how much of the sky the clouds take there (0 .. 1), y how high
// they tower (0 low and flat .. 1 the whole layer).
float2 CloudWeather(float2 xz) {
	float2 at = xz / 38.0 + g_Clouds.yz * 0.2;
	float w = g_CloudNoise.SampleLevel(s_LinearClamp, (float2(3.0, 2.0) + clamp(frac(at), 0.5 / kCloudTile, 1.0 - 0.5 / kCloudTile)) / 8.0, 0.0).r;
	float w2 = g_CloudNoise.SampleLevel(s_LinearClamp, (float2(6.0, 5.0) + clamp(frac(at * 1.7 + 0.31), 0.5 / kCloudTile, 1.0 - 0.5 / kCloudTile)) / 8.0, 0.0).r;
	// (the coverage setting moves the whole sky; the weather opens gaps and closes ranks around it)
	float coverage = saturate(g_Clouds.x * 1.25 + (w - 0.5) * 0.9 * saturate(g_Clouds.x * 4.0) * saturate((1.0 - g_Clouds.x) * 4.0));
	return float2(coverage, saturate(0.25 + w2 * 0.9));
}

// How much cloud there is at a place: xz over the ground in km, height above the ground in km. `detailed`: with the
// small cells that break up the edges (not needed where only the amount of cloud on the way to the sun is asked).
float CloudDensity(float2 xz, float height, bool detailed) {
	float h = (height - kCloudBase) / (kCloudTop - kCloudBase);
	if (h <= 0.0 || h >= 1.0) {
		return 0.0;
	}
	float2 weather = CloudWeather(xz);
	if (weather.x <= 0.01) {
		return 0.0;
	}
	// cumulus: a level base, then billows up to a top that the weather sets
	float top = lerp(0.35, 1.0, weather.y);
	float profile = saturate(h / 0.06) * saturate((top - h) / (0.45 * top));
	if (profile <= 0.0) {
		return 0.0;
	}
	float3 uvw = float3(xz / kCloudPeriod + g_Clouds.yz, height / kCloudPeriod);
	// (the big masses, and the same noise again at more than twice the pitch: the heaps on the heaps that make a
	// cumulus look like a cauliflower and not like a ball of cotton)
	float shape = CloudNoise3(uvw).r * 0.68 + CloudNoise3(uvw * 2.6 + float3(0.37, 0.11, 0.53)).r * 0.32;
	shape = saturate((shape - 0.5) * 1.4 + 0.5); // (two noises mixed crowd towards the middle: spread out again)
	// (rounder towards the top: more of the shape counts low down, where a cloud is one mass)
	float threshold = 1.0 - weather.x * lerp(0.95, 0.7, h);
	float d = saturate((shape * profile - threshold * profile * 0.999) / max(1.0 - threshold, 0.08));
	if (d <= 0.0) {
		return 0.0;
	}
	if (detailed) {
		// the small cells eat into the edges (the thin parts), wispier towards the top; they drift a little faster
		// (two sizes of cell: knobs of some tens of metres, and the fine fray of the very edge)
		float detail = CloudNoise3(uvw * 6.1 + float3(g_Clouds.yz * 1.6, 0.0)).g * 0.62 + CloudNoise3(uvw * 17.0 + float3(g_Clouds.yz * 2.3, 0.4)).g * 0.38;
		// (tried stronger, with the density squared after it: at a coverage of 0.6 hardly a cloud was left)
		float bite = detail * lerp(0.38, 0.62, h) * (1.0 - d);
		d = saturate((d - bite) / max(1.0 - bite, 0.25));
	}
	// A cloud is not a solid: thin at its edge, dense in its body (the square), and denser towards the top (the base
	// of a cumulus is ragged and thin).
	return d * (0.3 + 0.7 * d) * lerp(0.6, 1.0, saturate(h * 2.5)) * 1.35;
}

float CloudPhase(float c) {
	// forward and a little backward scattering (two Henyey-Greenstein lobes)
	const float g1 = 0.8, g2 = -0.3;
	float a = (1.0 - g1 * g1) / (4.0 * PI * pow(max(1.0 + g1 * g1 - 2.0 * g1 * c, 1e-4), 1.5));
	float b = (1.0 - g2 * g2) / (4.0 * PI * pow(max(1.0 + g2 * g2 - 2.0 * g2 * c, 1e-4), 1.5));
	return lerp(a, b, 0.3);
}

// The clouds along a ray from the eye (`altitude` km above the ground) towards `dir`: rgb the light they send
// (scene units), a what they let through of what is behind them. `steps`: 40 for the picture, fewer for the sky view LUT.
float4 MarchClouds(float3 dir, float altitude, int steps, float jitter) {
	if (g_Clouds.w < 0.5 || g_Clouds.x <= 0.01 || dir.y < 0.012) {
		return float4(0.0, 0.0, 0.0, 1.0);
	}
	// the layer as two spheres around the planet (so that it bends down towards the horizon)
	float3 o = float3(0.0, kAtmoGround + altitude, 0.0);
	float t0 = AtmoRaySphere(o, dir, kAtmoGround + kCloudBase, false);
	float t1 = AtmoRaySphere(o, dir, kAtmoGround + kCloudTop, false);
	if (t0 <= 0.0 || t1 <= t0) {
		return float4(0.0, 0.0, 0.0, 1.0);
	}
	t1 = min(t1, t0 + 9.0);
	float3 sun = g_SunDirection.xyz;
	float sunUp = saturate(sun.y * 6.0 + 0.3); // (the sun below the horizon still lights the cloud bases for a while)
	float3 sunLight = TransmittanceToTop(kAtmoGround + kCloudBase, sun.y) * g_Atmosphere.x * sunUp;
	// the sky's light on the clouds: what the air scatters around them
	float3 skyLight = MultipleScatteringGlobal(kAtmoGround + kCloudBase, sun.y) * (4.0 * PI) * g_Atmosphere.x + g_Atmosphere.yyy * float3(0.6, 0.75, 1.0) * PI;
	float cosSun = dot(dir, sun);
	float phase = CloudPhase(cosSun);
	float dt = (t1 - t0) / float(steps);
	float through = 1.0;
	float3 light = 0.0;
	float2 drift = g_CameraPos.xz * 0.001;
	for (int i = 0; i < steps; ++i) {
		float t = t0 + dt * (float(i) + jitter);
		float3 p = o + dir * t;
		float height = length(p) - kAtmoGround;
		float d = CloudDensity(p.xz + drift, height, true);
		if (d <= 0.002) {
			continue;
		}
		// the sun through the cloud towards this sample: a few steps that grow
		float depthToSun = 0.0;
		float reach = 0.06;
		float3 q = p;
		for (int k = 0; k < 6; ++k) {
			q += sun * reach;
			depthToSun += CloudDensity(q.xz + drift, length(q) - kAtmoGround, false) * reach;
			reach *= 1.9; // (about 1.4 km in all: a tower shades the cloud behind it)
		}
		float tau = depthToSun * kCloudExtinction;
		// Light that reaches the sample straight, and light that got there after being scattered again and again
		// (which goes much deeper and has forgotten where it came from: without it the shaded side of a cloud is black).
		float direct = exp(-tau);
		float scattered = 0.3 * exp(-tau * 0.35) + 0.1 * exp(-tau * 0.1);
		// (the edges towards the sun are darker than the body just behind them: little has scattered there yet)
		float powder = 1.0 - exp(-d * 6.0 - tau * 2.0);
		float h = saturate((height - kCloudBase) / (kCloudTop - kCloudBase));
		float3 inScatter = sunLight * (direct * phase * lerp(1.0, powder * 2.0, 0.5 * saturate(cosSun * 0.5 + 0.5)) + scattered / (4.0 * PI) * 1.6)
			// the sky from above (more near the top), the lit ground from below
			// (little of the sky reaches the base through the cloud above it: a cumulus is grey underneath)
			+ skyLight * (0.12 + 0.88 * h * h) / (4.0 * PI)
			+ sunLight * (saturate(sun.y) * 0.06 / PI) * (1.0 - h);
		float extinction = d * kCloudExtinction;
		float step = exp(-extinction * dt);
		light += through * inScatter * (1.0 - step); // (albedo 1: clouds absorb next to nothing)
		through *= step;
		if (through < 0.015) {
			through = 0.0;
			break;
		}
	}
	// far clouds sink into the haze: towards the sky's own colour in that direction
	float haze = 1.0 - exp(-t0 * 0.03);
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
	// the cloud on the way to the sun, taken at two heights of the layer
	float low = kCloudBase + (kCloudTop - kCloudBase) * 0.22, high = kCloudBase + (kCloudTop - kCloudBase) * 0.5;
	float d = CloudDensity(p.xz + sun.xz * ((low - p.y) / sun.y), low, false) * 0.6 + CloudDensity(p.xz + sun.xz * ((high - p.y) / sun.y), high, false) * 0.4;
	return exp(-d * kCloudExtinction * 0.3 * (kCloudTop - kCloudBase) / max(sun.y, 0.15));
}
