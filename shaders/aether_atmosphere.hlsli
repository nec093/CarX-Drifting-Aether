// Aether's physically based sky: a planet's atmosphere with Rayleigh and Mie scattering and ozone absorption, in the
// scheme of Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (2020):
//   transmittance LUT (the light left after crossing the air from a height in a direction),
//   multiple scattering LUT (light scattered more than once, as an isotropic source per height and sun angle),
//   sky view LUT (the sky's radiance around the camera, every frame).
// Lengths are kilometres. Radiance is relative to a sun illuminance of 1 at the top of the atmosphere; the renderer
// scales it to the scene's units (g_Atmosphere.x).
// Included by aether_scene.hlsli (needs PI, the samplers and the two LUT textures declared there).

static const float kAtmoGround = 6360.0;
static const float kAtmoTop = 6460.0;
static const float3 kRayleighScattering = float3(5.802e-3, 13.558e-3, 33.1e-3);
static const float kMieScattering = 3.996e-3;
static const float kMieAbsorption = 4.40e-3;
static const float3 kOzoneAbsorption = float3(0.650e-3, 1.881e-3, 0.085e-3);
static const float3 kGroundAlbedo = float3(0.18, 0.17, 0.16);

void AtmosphereMedium(float height, out float3 rayleigh, out float mie, out float3 extinction) {
	float densityR = exp(-max(height, 0.0) / 8.0);
	float densityM = exp(-max(height, 0.0) / 1.2);
	rayleigh = kRayleighScattering * densityR;
	mie = kMieScattering * densityM;
	float3 ozone = kOzoneAbsorption * max(0.0, 1.0 - abs(height - 25.0) / 15.0);
	extinction = rayleigh + mie + kMieAbsorption * densityM + ozone;
}

// Distance along the ray (origin o relative to the planet's centre, unit direction d) to the sphere of radius r:
// the far hit from inside, the near one from outside; negative when there is none.
float AtmoRaySphere(float3 o, float3 d, float r, bool nearHit) {
	float b = dot(o, d);
	float c = dot(o, o) - r * r;
	float h = b * b - c;
	if (h < 0.0) {
		return -1.0;
	}
	h = sqrt(h);
	return nearHit ? -b - h : -b + h;
}

// ---- transmittance LUT: u = cosine of the zenith angle, v = height (square root spacing: more texels near the ground)
float2 TransmittanceUv(float radius, float cosZenith) {
	float v = sqrt(saturate((radius - kAtmoGround) / (kAtmoTop - kAtmoGround)));
	return float2(cosZenith * 0.5 + 0.5, v);
}

float3 TransmittanceToTop(float radius, float cosZenith) {
	return g_Transmittance.SampleLevel(s_LinearClamp, TransmittanceUv(radius, cosZenith), 0.0).rgb;
}

float3 IntegrateTransmittance(float radius, float cosZenith) {
	float3 o = float3(0.0, radius, 0.0);
	float3 d = float3(sqrt(saturate(1.0 - cosZenith * cosZenith)), cosZenith, 0.0);
	if (AtmoRaySphere(o, d, kAtmoGround, true) > 0.0) {
		return 0.0; // the ground is in the way
	}
	float end = AtmoRaySphere(o, d, kAtmoTop, false);
	const int kSteps = 40;
	float dt = end / float(kSteps);
	float3 depth = 0.0;
	for (int i = 0; i < kSteps; ++i) {
		float3 p = o + d * (dt * (float(i) + 0.5));
		float3 rayleigh, extinction;
		float mie;
		AtmosphereMedium(length(p) - kAtmoGround, rayleigh, mie, extinction);
		depth += extinction * dt;
	}
	return exp(-depth);
}

// ---- multiple scattering LUT: u = cosine of the sun's zenith angle, v = height
float3 MultipleScattering(float radius, float sunCos) {
	float2 uv = float2(sunCos * 0.5 + 0.5, saturate((radius - kAtmoGround) / (kAtmoTop - kAtmoGround)));
	return g_MultiScatter.SampleLevel(s_LinearClamp, uv, 0.0).rgb;
}

float3 MultipleScatteringGlobal(float radius, float sunCos) {
	return MultipleScattering(radius, sunCos);
}

float RayleighPhase(float c) {
	return 3.0 / (16.0 * PI) * (1.0 + c * c);
}

float MiePhase(float c) {
	const float g = 0.8;
	float k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
	return k * (1.0 + c * c) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
}

// Second order scattering and the share of light that scatters again, for a point at `radius` under a sun at `sunCos`
// (Hillaire, section 5.5): 64 directions around the point, each marched to the atmosphere's end or the ground.
float3 IntegrateMultipleScattering(float radius, float sunCos) {
	float3 o = float3(0.0, radius, 0.0);
	float3 sun = float3(sqrt(saturate(1.0 - sunCos * sunCos)), sunCos, 0.0);
	float3 secondOrder = 0.0, factor = 0.0;
	const int kSqrt = 8, kSteps = 20;
	for (int a = 0; a < kSqrt; ++a) {
		for (int b = 0; b < kSqrt; ++b) {
			float theta = PI * (float(a) + 0.5) / float(kSqrt);
			float phi = acos(1.0 - 2.0 * (float(b) + 0.5) / float(kSqrt));
			float3 d = float3(sin(phi) * cos(theta * 2.0), cos(phi), sin(phi) * sin(theta * 2.0));
			float ground = AtmoRaySphere(o, d, kAtmoGround, true);
			float end = ground > 0.0 ? ground : AtmoRaySphere(o, d, kAtmoTop, false);
			float dt = end / float(kSteps);
			float3 through = 1.0, light = 0.0, again = 0.0;
			for (int i = 0; i < kSteps; ++i) {
				float3 p = o + d * (dt * (float(i) + 0.5));
				float r = length(p);
				float3 rayleigh, extinction;
				float mie;
				AtmosphereMedium(r - kAtmoGround, rayleigh, mie, extinction);
				float3 scattering = rayleigh + mie;
				float3 step = exp(-extinction * dt);
				float3 share = (1.0 - step) / max(extinction, 1e-7);
				float3 fromSun = TransmittanceToTop(r, dot(p / r, sun));
				// (isotropic phase: this light has lost its direction)
				light += through * scattering * fromSun * (1.0 / (4.0 * PI)) * share;
				again += through * scattering * share;
				through *= step;
			}
			if (ground > 0.0) {
				float3 p = o + d * ground;
				float3 n = p / kAtmoGround;
				light += through * TransmittanceToTop(kAtmoGround, dot(n, sun)) * saturate(dot(n, sun)) * kGroundAlbedo / PI;
			}
			secondOrder += light / float(kSqrt * kSqrt);
			factor += again / float(kSqrt * kSqrt);
		}
	}
	return secondOrder / (1.0 - factor);
}

// ---- sky view LUT: u = the direction's heading (around the up axis), v = its elevation, with most of the texels near
// the horizon, where the sky changes fastest
float2 SkyViewUv(float3 dir) {
	float heading = atan2(dir.x, dir.z);
	float elevation = asin(clamp(dir.y, -1.0, 1.0));
	float v = 0.5 + 0.5 * sign(elevation) * sqrt(abs(elevation) / (PI * 0.5));
	return float2(heading / (2.0 * PI) + 0.5, v);
}

float3 SkyViewDirection(float2 uv) {
	float heading = (uv.x - 0.5) * 2.0 * PI;
	float s = uv.y * 2.0 - 1.0;
	float elevation = sign(s) * s * s * (PI * 0.5);
	float c = cos(elevation);
	return float3(c * sin(heading), sin(elevation), c * cos(heading));
}

// The sky's radiance towards `dir` for an eye `altitude` kilometres above the ground, the sun at `sun` (unit vectors,
// world space, y up). Without the sun's own disc. Below the horizon: the ground, lit by the sun and the sky.
float3 IntegrateSkyView(float3 dir, float3 sun, float altitude) {
	float3 o = float3(0.0, kAtmoGround + max(altitude, 0.001), 0.0);
	float ground = AtmoRaySphere(o, dir, kAtmoGround, true);
	float end = ground > 0.0 ? ground : AtmoRaySphere(o, dir, kAtmoTop, false);
	if (end <= 0.0) {
		return 0.0;
	}
	float c = dot(dir, sun);
	float phaseR = RayleighPhase(c), phaseM = MiePhase(c);
	const int kSteps = 32;
	float3 through = 1.0, light = 0.0;
	float before = 0.0;
	for (int i = 0; i < kSteps; ++i) {
		// (steps grow with the distance: the air near the eye matters most)
		float f = (float(i) + 1.0) / float(kSteps);
		float t = end * f * f;
		float dt = t - before;
		float3 p = o + dir * (before + dt * 0.5);
		before = t;
		float r = length(p);
		float3 up = p / r;
		float3 rayleigh, extinction;
		float mie;
		AtmosphereMedium(r - kAtmoGround, rayleigh, mie, extinction);
		float sunCos = dot(up, sun);
		float3 fromSun = TransmittanceToTop(r, sunCos);
		float3 inScatter = (rayleigh * phaseR + mie * phaseM) * fromSun + (rayleigh + mie) * MultipleScattering(r, sunCos);
		float3 step = exp(-extinction * dt);
		light += through * inScatter * (1.0 - step) / max(extinction, 1e-7);
		through *= step;
	}
	if (ground > 0.0) {
		float3 p = o + dir * ground;
		float3 n = p / kAtmoGround;
		float sunCos = dot(n, sun);
		float3 lit = TransmittanceToTop(kAtmoGround, sunCos) * saturate(sunCos) + MultipleScattering(kAtmoGround, sunCos) * PI;
		light += through * lit * kGroundAlbedo / PI;
	}
	return light;
}

// The sky around the camera, read from the sky view LUT (g_SkyView), in the scene's units.
float3 PhysicalSky(float3 dir) {
	float2 uv = SkyViewUv(dir);
	uv.y = clamp(uv.y, 0.5 / 108.0, 1.0 - 0.5 / 108.0); // (also fine for the 216 row version with the clouds)
	// (the heading wraps around: read with the wrapping sampler, s_Aniso)
	float3 sky = g_SkyView.SampleLevel(s_Aniso, uv, 0.0).rgb;
	return sky * g_Atmosphere.x + g_Atmosphere.yyy * float3(0.6, 0.75, 1.0);
}

// The air's sky alone (no clouds), for the passes that have the air's sky view LUT as their first input (g_SceneColor).
float3 PhysicalSkyAir(float3 dir) {
	float2 uv = SkyViewUv(dir);
	uv.y = clamp(uv.y, 0.5 / 108.0, 1.0 - 0.5 / 108.0);
	return g_SceneColor.SampleLevel(s_Aniso, uv, 0.0).rgb * g_Atmosphere.x + g_Atmosphere.yyy * float3(0.6, 0.75, 1.0);
}

// The sun's own disc as seen from the ground (0.53 degrees across, darker towards its rim), in the scene's units.
float3 PhysicalSunDisc(float3 dir) {
	float c = dot(dir, g_SunDirection.xyz);
	const float kCosRadius = 0.99998927; // cos(0.2655 degrees)
	if (g_SunDirection.w < 0.5 || c < kCosRadius || dir.y < -0.01) {
		return 0.0;
	}
	float edge = saturate((c - kCosRadius) / (1.0 - kCosRadius));
	float limb = 0.4 + 0.6 * sqrt(edge);
	// illuminance / solid angle of the disc (6.8e-5 sr); kept below what the temporal passes can handle
	return min(g_SunColor.rgb * (limb / 6.8e-5), 4000.0 * max(g_SunColor.r, max(g_SunColor.g, g_SunColor.b)));
}
