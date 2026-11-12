// Aether forward+ scene shaders: vertex shader, depth / shadow pixel shader, lit pixel shader.
#include "aether_scene.hlsli"
#include "aether_bvh.hlsli"

struct VSInput {
	float3 position : POSITION;
	float4 normal : NORMAL;    // Float32x3 or Float16x4 in the game's meshes
	float4 tangent : TANGENT;  // w = bitangent sign
	float4 color : COLOR;
	float2 uv : TEXCOORD0;
};

struct VSOutput {
	float4 position : SV_Position;
	float3 worldPos : WORLDPOS;
	float viewDepth : VIEWDEPTH; // distance in front of the camera
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	float2 uv : TEXCOORD0;
	float4 color : COLOR;
	nointerpolation uint instance : INSTANCE; // index in g_Instances
	float4 clipNow : CLIPNOW;       // this frame's clip position without the sub pixel offset
	float4 clipBefore : CLIPBEFORE; // where the vertex was in the frame before (its own motion and the camera's)
};

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID) {
	const uint instance = g_InstanceMap[g_InstanceIndex + instanceId];
	InstanceData inst = g_Instances[instance];
	MaterialData mat = g_Materials[g_MaterialIndex];
	VSOutput o;
	float4 world = mul(inst.world, float4(input.position, 1.0));
	o.worldPos = world.xyz;
	float4 view = mul(g_View, world);
	o.viewDepth = -view.z; // right-handed view space: the camera looks down -Z
	o.position = mul(g_Proj, view);
	float3x3 normalMatrix = float3x3(inst.normalRow0.xyz, inst.normalRow1.xyz, inst.normalRow2.xyz);
	o.normal = mul(normalMatrix, input.normal.xyz);
	o.tangent = float4(mul((float3x3)inst.world, input.tangent.xyz), input.tangent.w * inst.normalRow0.w);
	o.uv = input.uv * mat.uvScaleOffset.xy + mat.uvScaleOffset.zw;
	o.color = input.color;
	o.instance = instance;
	o.clipNow = float4(o.position.xy - g_Jitter.xy * o.position.w, o.position.zw);
	o.clipBefore = mul(g_PrevViewProj, mul(inst.prevWorld, float4(input.position, 1.0)));
	return o;
}

// Shadow cascades: the same vertex data through the cascade's matrix.
float4 VSShadow(VSInput input, uint instanceId : SV_InstanceID, out float2 uv : TEXCOORD0) : SV_Position {
	InstanceData inst = g_Instances[g_InstanceMap[g_InstanceIndex + instanceId]];
	MaterialData mat = g_Materials[g_MaterialIndex];
	uv = input.uv * mat.uvScaleOffset.xy + mat.uvScaleOffset.zw;
	return mul(g_ShadowViewProj[g_CascadeIndex], mul(inst.world, float4(input.position, 1.0)));
}

// Depth only, for alpha tested materials (opaque ones have no pixel shader in the depth passes).
void PSDepthAlphaTest(VSOutput input) {
	MaterialData mat = g_Materials[g_MaterialIndex];
	float alpha = g_BaseMap.Sample(s_Aniso, input.uv).a * mat.baseColor.a;
	clip(alpha - mat.params.z);
}

void PSShadowAlphaTest(float4 position : SV_Position, float2 uv : TEXCOORD0) {
	MaterialData mat = g_Materials[g_MaterialIndex];
	float alpha = g_BaseMap.Sample(s_Aniso, uv).a * mat.baseColor.a;
	clip(alpha - mat.params.z);
}

// The game's blob shadow: a box around the ground under a car. Every opaque surface inside the box is darkened by the
// box's texture, looked up with the surface's position in the box (x, z); the surface is found from the scene depth.
// Drawn without a depth buffer, far faces only (PassKind kDecal).
float4 PSBlobDecal(VSOutput input) : SV_Target {
	MaterialData mat = g_Materials[g_MaterialIndex];
	int view = (int)g_Misc.x;
	if (view != VIEW_NONE && view < VIEW_SUN) {
		discard; // (not part of the material debug views)
	}
	float depth = g_PostDepth.Load(int3(input.position.xy, 0)).r;
	if (depth <= 0.0) {
		discard; // the sky
	}
	float2 ndc = input.position.xy * g_ScreenSize.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 world = mul(g_InvViewProj, float4(ndc, depth, 1.0));
	InstanceData inst = g_Instances[input.instance];
	float3 surface = world.xyz / world.w;
	float3 rel = surface - float3(inst.world[0][3], inst.world[1][3], inst.world[2][3]);
	// (the normal rows are the columns of the inverse world matrix)
	float3 local = inst.normalRow0.xyz * rel.x + inst.normalRow1.xyz * rel.y + inst.normalRow2.xyz * rel.z;
	if (any(abs(local) > 0.5)) {
		discard; // outside the box (a unit cube in its own space)
	}
	float shade = g_BaseMap.SampleLevel(s_LinearClamp, local.xz + 0.5, 0.0).r * mat.baseColor.a;
	// fades out towards the box's top and bottom so that nothing is cut off at a hard edge
	shade *= saturate((0.5 - abs(local.y)) * 4.0);
	// Only what the car stands on: the box also holds the car's own sills, wheels and lower doors, and darkening those
	// drew a level line along the car's side at the height of the box's top. A surface that does not face along the
	// box's up axis (found from the depth buffer's slope) is left alone.
	float3 up = normalize(float3(inst.world[0][1], inst.world[1][1], inst.world[2][1]));
	float3 facing = normalize(cross(ddx(surface), ddy(surface)));
	shade *= smoothstep(0.6, 0.9, abs(dot(facing, up)));
	return float4(0.0, 0.0, 0.0, shade);
}

// ---- BRDF (GGX, Smith height-correlated visibility approximation, Schlick Fresnel)
float D_GGX(float NoH, float a) {
	float a2 = a * a;
	float d = (NoH * a2 - NoH) * NoH + 1.0;
	return a2 / max(PI * d * d, 1e-7);
}

float V_SmithGGX(float NoV, float NoL, float a) {
	float gv = NoL * (NoV * (1.0 - a) + a);
	float gl = NoV * (NoL * (1.0 - a) + a);
	return 0.5 / max(gv + gl, 1e-5);
}

float3 F_Schlick(float3 f0, float VoH) {
	float f = pow(1.0 - VoH, 5.0);
	return f0 + (1.0 - f0) * f;
}

// Environment BRDF approximation (Karis, "Physically Based Shading on Mobile")
float3 EnvBRDFApprox(float3 f0, float roughness, float NoV) {
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	float4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	return f0 * ab.x + ab.y;
}

#define AE_MATERIALS 1 // 0: the shading as it was before the material work of phase 4 (for comparisons)

struct Surface {
	float3 albedo;
	float alpha;
	float3 normal;
	float metallic;
	float roughness;
	float occlusion;
	float3 emissive;
	float clearCoat;
	float clearCoatRoughness;
	float coatF0;      // the clear coat's reflectance at normal incidence
	float3 coatNormal; // the clear coat's own normal (the base layer's may be a metallic flake's)
	float3 f0;         // the base layer's reflectance at normal incidence
	float3 energy;     // what the single scattering specular lobe leaves out (light bounced between microfacets), as a factor
};

// The base layer's reflectance seen through a clear coat (index 1.5) instead of from air: the step in index at the
// base is smaller, so it reflects less (the fit is Filament's).
float3 F0UnderCoat(float3 f0) {
	return saturate(f0 * (f0 * (0.941892 - 0.263008 * f0) + 0.346479) - 0.0285998);
}

// Rough surfaces lose energy in a single scattering GGX lobe: light that bounces between microfacets is not counted.
// Given back as a factor on the lobe (Kulla and Conty's idea in Filament's form); the directional albedo of the lobe
// comes from the same fit the environment light uses.
float3 EnergyCompensation(float3 f0, float roughness, float NoV) {
#if !AE_MATERIALS
	return 1.0;
#endif
	float e = max(EnvBRDFApprox(1.0, roughness, NoV).x, 0.05);
	return 1.0 + f0 * (1.0 / e - 1.0);
}

// Specular antialiasing (Tokuyoshi and Kaplanyan, "Improved Geometric Specular Antialiasing"): where the normal
// changes within a pixel (small curved parts, far normal maps) a narrow lobe turns into flickering sparks; the
// roughness is raised by the variance of the normal over the pixel.
// `limit`: how far the roughness may be raised (as variance).
float FilterRoughness(float3 n, float roughness, float limit = 0.18) {
#if !AE_MATERIALS
	return roughness;
#endif
	float3 du = ddx(n), dv = ddy(n);
	float kernel = min(0.5 * (dot(du, du) + dot(dv, dv)), limit);
	float a = roughness * roughness;
	return sqrt(sqrt(saturate(a * a + kernel)));
}

// How much of the mirrored surroundings a place sees when `visibility` of its hemisphere is open (Lagarde, "Moving
// Frostbite to PBR"): a smooth surface looks through a narrow cone and is hidden later than the ambient light is.
float SpecularOcclusion(float NoV, float visibility, float roughness) {
#if !AE_MATERIALS
	return visibility;
#endif
	return saturate(pow(NoV + visibility, exp2(-16.0 * roughness - 1.0)) - 1.0 + visibility);
}

float Hash31(float3 p) {
	p = frac(p * float3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yzx + 33.33);
	return frac((p.x + p.y) * p.z);
}

float3 DirectLight(Surface s, float3 V, float3 L, float3 radiance) {
	float NoL = saturate(dot(s.normal, L));
	float coatNoL = s.clearCoat > 0.0 ? saturate(dot(s.coatNormal, L)) : 0.0;
	if (NoL <= 0.0 && coatNoL <= 0.0) {
		return 0.0;
	}
	float3 H = normalize(V + L);
	float NoV = max(dot(s.normal, V), 1e-4);
	float NoH = saturate(dot(s.normal, H));
	float VoH = saturate(dot(V, H));
	float a = max(s.roughness * s.roughness, 0.002);
	float3 f0 = s.f0;
	// diffuse: Burley's (Disney) model, as the game's own renderer has it: brighter than Lambert on rough surfaces
	// seen or lit at a grazing angle
	float fd90 = 0.5 + s.roughness * (1.0 + dot(L, V));
	float scatter = (1.0 + (fd90 - 1.0) * pow(1.0 - NoL, 5.0)) * (1.0 + (fd90 - 1.0) * pow(1.0 - NoV, 5.0)) / 1.03571;
	float3 diffuse = s.albedo * (1.0 - s.metallic) * scatter / PI;
	float3 F = F_Schlick(f0, VoH);
	float3 specular = D_GGX(NoH, a) * V_SmithGGX(NoV, NoL, a) * F * s.energy;
	float3 base = (diffuse + specular) * NoL;
	if (s.clearCoat > 0.0) {
		// a second, dielectric lobe on top (f0 0.04), on the coat's own smooth normal; the base layer loses what the
		// coat reflects
		float ac = max(s.clearCoatRoughness * s.clearCoatRoughness, 0.002);
		float Fc = (s.coatF0 + (1.0 - s.coatF0) * pow(1.0 - VoH, 5.0)) * s.clearCoat;
		float cNoV = max(dot(s.coatNormal, V), 1e-4);
		float cNoH = saturate(dot(s.coatNormal, H));
		float coat = D_GGX(cNoH, ac) * V_SmithGGX(cNoV, coatNoL, ac) * Fc * coatNoL;
		// (and a faint wide lobe round it: the haze every lacquer has, which is the glow round the sun's image)
		coat += D_GGX(cNoH, 0.03) * V_SmithGGX(cNoV, coatNoL, 0.03) * Fc * coatNoL * 0.05;
		base = base * (1.0 - Fc) + coat;
	}
	return base * radiance;
}

// A head lamp's low beam, for traffic that keeps to the left (a right-hand drive car): how much of the lamp's
// strength goes in a direction. `d`: from the lamp to the lit place, `forward`: where the lamp points.
// What makes a low beam: a sharp cut-off just under the horizontal, so that nothing goes into the eyes of who comes
// the other way (on the right); on the near side (the left) the cut-off rises at 15 degrees, to light the verge and
// the signs; the brightest part sits right under the cut-off, a little to the left; below it the light spreads wide
// and fades towards the car.
float HeadlampBeam(float3 d, float3 forward) {
	float along = dot(d, forward);
	if (along <= 0.05) {
		return 0.0;
	}
	float3 right = normalize(cross(float3(0.0, 1.0, 0.0), forward));
	float3 up = cross(forward, right);
	float h = atan2(dot(d, right), along); // to the right: positive
	float v = atan2(dot(d, up), along);
	// the cut-off: 0.57 degrees under the horizontal (1 %), rising on the left
	float cutoff = -0.010 + max(-h - 0.010, 0.0) * 0.268;
	cutoff = min(cutoff, 0.06);
	float below = smoothstep(cutoff + 0.004, cutoff - 0.006, v);
	// under the cut-off: the hot spot, the spread to both sides, the foreground
	float dv = cutoff - v;
	float spot = exp(-pow((h + 0.03) / 0.16, 2.0)) * exp(-pow(dv / 0.045, 2.0));
	float spread = 0.28 * exp(-pow(h / 0.55, 2.0)) * exp(-pow(dv / 0.16, 2.0));
	float near = 0.05 * exp(-pow(h / 0.8, 2.0)) * saturate(1.0 - dv / 0.9);
	// above it: the little that a real lamp's lens scatters upwards
	float stray = 0.012 * exp(-pow(h / 0.6, 2.0)) * exp(-pow(max(v - cutoff, 0.0) / 0.25, 2.0));
	return (spot + spread + near) * below + stray;
}

float3 PunctualLights(Surface s, float3 worldPos, float3 V, float2 pixel, float viewDepth, uint layers, out uint lightCount) {
	lightCount = 0;
	float3 result = 0.0;
	uint3 counts = (uint3)g_ClusterCounts.xyz;
	if (counts.x == 0) {
		return result;
	}
	uint cx = min((uint)(pixel.x * g_ScreenSize.z * counts.x), counts.x - 1);
	uint cy = min((uint)(pixel.y * g_ScreenSize.w * counts.y), counts.y - 1);
	float z = clamp(viewDepth, g_ClusterZ.x, g_ClusterZ.y);
	uint cz = min((uint)(log(z / g_ClusterZ.x) * g_ClusterZ.z), counts.z - 1);
	uint2 cell = g_ClusterGrid[(cz * counts.y + cy) * counts.x + cx];
	lightCount = cell.y;
	for (uint i = 0; i < cell.y; ++i) {
		LightData light = g_Lights[g_LightIndices[cell.x + i]];
		if ((asuint(light.typeFlags.y) & layers) == 0u) {
			continue; // light layers: this light does not reach the renderer
		}
		float3 toLight = light.positionRange.xyz - worldPos;
		float d2 = dot(toLight, toLight);
		float range = light.positionRange.w;
		if (d2 >= range * range) {
			continue;
		}
		float3 L = toLight * rsqrt(max(d2, 1e-6));
		// inverse square with a smooth window to zero at the range
		float w = saturate(1.0 - (d2 * d2) / (range * range * range * range));
		float attenuation = w * w / max(d2, 0.0001);
		if (light.typeFlags.z > 0.5) {
			attenuation *= HeadlampBeam(-L, light.directionOuter.xyz);
		} else if (light.typeFlags.x > 1.5) {
			float cosAngle = dot(-L, light.directionOuter.xyz);
			float outer = light.directionOuter.w, inner = light.colorInner.w;
			attenuation *= smoothstep(outer, max(inner, outer + 1e-4), cosAngle);
		}
		if (attenuation > 0.0) {
			result += DirectLight(s, V, L, light.colorInner.rgb * attenuation);
		}
	}
	return result;
}

float3 UnpackNormal(float4 packed, float scale) {
	// works for RG (BC5), AG (DXT5nm: x in alpha, red = 1) and plain RGB normal maps
	packed.x *= packed.a;
	float3 n;
	n.xy = (packed.xy * 2.0 - 1.0) * scale;
	n.z = sqrt(saturate(1.0 - dot(n.xy, n.xy)));
	return n;
}

static const float3 kClassColors[14] = {
	float3(1, 0, 1), float3(0.6, 0.6, 0.6), float3(0.1, 0.4, 1), float3(0.2, 0.8, 0.9), float3(0.5, 0.9, 1), float3(0.9, 0.9, 0.2), float3(1, 0.5, 0.1),
	float3(0.9, 0.3, 0.3), float3(0.2, 0.8, 0.2), float3(0.7, 0.4, 0.9), float3(1, 1, 1), float3(1, 0.8, 0.5), float3(0.25, 0.25, 0.25), float3(0, 0, 0)};

#define GLASS_F0 0.13 // glass reflectance at normal incidence (matched by eye against the game's picture)

// surface: what the reflection pass needs to know about an opaque surface (the opaque pass's second target):
// xy the normal (octahedral), z the roughness of the lobe that mirrors, w how much of the mirrored light it returns.
float4 ShadeForward(VSOutput input, bool frontFace, out float4 surface) {
	surface = 0.0;
	MaterialData mat = g_Materials[g_MaterialIndex];
	const uint materialClass = (uint)mat.params2.z;
	const int view = (int)g_Misc.x;
	float3 V = normalize(g_CameraPos.xyz - input.worldPos);
	// a mirrored instance (negative determinant) has its winding reversed: what the rasterizer calls the back is its outside
	const bool outside = frontFace != ((g_DrawFlags & 1u) != 0u);

	// ---- the diagnostic material: a shader Aether does not translate yet is visible, not missing
	if (materialClass == CLASS_UNSUPPORTED) {
		float2 c = floor(input.position.xy / 8.0);
		float checker = fmod(c.x + c.y, 2.0);
		float3 n = normalize(input.normal);
		return float4(lerp(float3(1.0, 0.0, 1.0), float3(0.1, 0.0, 0.1), checker) * (0.35 + 0.65 * saturate(dot(n, g_SunDirection.xyz))) * 2.0, 1.0);
	}

	float4 base = g_BaseMap.Sample(s_Aniso, input.uv);
	Surface s;
	float paintMirror = 0.0; // body paint: 1 = chrome
	s.albedo = base.rgb * mat.baseColor.rgb;
	s.alpha = base.a * mat.baseColor.a;
	s.metallic = mat.emissiveMetallic.a;
	s.roughness = mat.params.x;
	s.occlusion = 1.0;
	s.clearCoat = mat.params2.x;
	s.clearCoatRoughness = mat.params2.y;
	s.coatF0 = 0.04;
	s.emissive = mat.emissiveMetallic.rgb * g_EmissiveMap.Sample(s_Aniso, input.uv).rgb;

	if (mat.params.z >= 0.0) {
		clip(s.alpha - mat.params.z);
	}

	// ---- mask / pack map
	if (mat.params3.y > 0.5) {
		float4 m = g_MaskMap.Sample(s_Aniso, input.uv);
		if (mat.params3.x < 1.5) { // HDRP: R metallic, G occlusion, A smoothness
			s.metallic *= m.r;
			s.roughness = 1.0 - (1.0 - s.roughness) * m.a;
			s.occlusion = lerp(1.0, m.g, mat.params.w);
		} else { // the game's pack maps: R roughness, G metallic, B occlusion
			s.roughness *= m.r;
			s.metallic *= m.g;
			s.occlusion = lerp(1.0, m.b, mat.params.w);
		}
	}

	// ---- class specific surface inputs
	if (materialClass == CLASS_VEHICLE_GENERIC || materialClass == CLASS_VEHICLE_TIRE) {
		// colour mask map (checked against the game's textures: the masks are in R, then G): each channel tints with one
		// of the material's colours, by that colour's alpha. Without the map the base colour is not a tint at all.
		s.albedo = base.rgb;
		if (mat.params3.w > 0.5) {
			float3 mask = g_ExtraMap.Sample(s_Aniso, input.uv).rgb;
			s.albedo = lerp(s.albedo, base.rgb * mat.baseColor.rgb, mask.r * mat.baseColor.a);
			s.albedo = lerp(s.albedo, base.rgb * mat.secondaryColor.rgb, mask.g * mat.secondaryColor.a);
		}
	} else if (materialClass == CLASS_COLOR_MASK) {
		// the game's Colormask graph: the mask map's B channel says where the base colour tints (R metallic, G occlusion,
		// A smoothness as in HDRP's mask map; checked against the game's textures)
		float tintMask = mat.params3.y > 0.5 ? g_MaskMap.Sample(s_Aniso, input.uv).b : 1.0;
		s.albedo = base.rgb * lerp(1.0, mat.baseColor.rgb, tintMask);
	} else if (materialClass == CLASS_VEHICLE_LIGHTS) {
		// one light group per channel of the mask texture, switched by the material's four intensities
		float4 groups = g_EmissiveMap.Sample(s_Aniso, input.uv);
		s.emissive = base.rgb * dot(groups, max(mat.secondaryColor, 0.0)) * mat.emissiveMetallic.rgb;
		if (AE_MATERIALS) {
			// A lamp is behind a lens: smooth clear plastic or glass over everything the texture shows. (Without it a
			// head lamp and a tail lamp were matt painted shapes.) A little more than a lens returns head on, like the
			// car's lacquer.
			s.clearCoat = 1.0;
			s.clearCoatRoughness = 0.03;
			s.coatF0 = 0.10;
		}
	} else if (materialClass == CLASS_VEHICLE_STITCHES) {
		// the body's panel seams (door and bonnet gaps, the filler cap): a strip lying over the body, seen through by its
		// pack texture. In the game's own picture (same view, side by side) a seam is a dark groove in the paint, in the
		// shade as much as in the sun. The strip's material carries no paint colour (white by default, and the game
		// gives these renderers no property block), so drawing it as paint made every seam a white outline.
		// Drawn as what it shows: a gap that swallows the light.
		float4 pack = g_MaskMap.Sample(s_Aniso, input.uv); // (read back from the game's texture: B is 1 along the seam and 0 at the strip's edges, R falls towards the seam)
		s.albedo = 0.01;
		s.metallic = 0.0;
		s.roughness = 1.0;
		s.clearCoat = 0.0;
		s.alpha = pack.b * 0.6; // (the depth of the groove is matched by eye against the game's picture)
	} else if (materialClass == CLASS_VEHICLE_PAINT || materialClass == CLASS_VEHICLE_RIM) {
		// the game's paint: a colour, and a second one that takes over towards grazing angles (none set = one colour)
		float facing = saturate(dot(normalize(input.normal), V));
		float twoTone = any(mat.secondaryColor.rgb > 0.0) ? pow(1.0 - facing, 2.0) : 0.0;
		float3 paint = lerp(mat.baseColor.rgb, mat.secondaryColor.rgb, twoTone);
		// the body's texture is its livery (vinyls, stickers): over the paint by its alpha. baseColor.a = a livery is bound
		s.albedo = materialClass == CLASS_VEHICLE_PAINT && mat.baseColor.a > 0.5 ? lerp(paint, base.rgb, base.a) : paint;
		s.alpha = 1.0;
		if (AE_MATERIALS && materialClass == CLASS_VEHICLE_PAINT) {
			// Aether's own paint. What kind it is is read from the game's three numbers (seen in the paint menu with
			// the metallic debug view: "metallic" arrives as roughness 0.33, metalness 0.75, clear coat 0.33):
			// - chrome: all metal and smooth. A bare mirror, no lacquer, no flakes.
			// - matt: the game gives it no coat and it is rough. Bare pigment.
			// - everything else is lacquered: a full, smooth clear coat whatever strength the game's graph uses.
			paintMirror = smoothstep(0.8, 0.95, s.metallic) * (1.0 - smoothstep(0.1, 0.3, s.roughness));
			float lacquered = max(saturate(s.clearCoat * 4.0), 1.0 - smoothstep(0.25, 0.45, s.roughness));
			s.clearCoat = lacquered * (1.0 - paintMirror);
			s.clearCoatRoughness = 0.03;
			// (lacquer reflects 4 % head on; the user's choice is a glossier car than that)
			s.coatF0 = 0.14; // (0.10 at first, then 0.17 on the user's wish for stronger reflections, then a little less)
		}
	}

	// ---- normal
	float3 N = normalize(input.normal);
	if (mat.params3.z > 0.5) {
		float3 T = normalize(input.tangent.xyz - N * dot(N, input.tangent.xyz));
		float3 B = cross(N, T) * input.tangent.w;
		float3 n = UnpackNormal(g_NormalMap.Sample(s_Aniso, input.uv), mat.params.y);
		N = normalize(T * n.x + B * n.y + N * n.z);
	}
	if (!outside) {
		N = -N; // double sided materials light their back faces
	}
	s.normal = N;
	s.coatNormal = N;
	s.roughness = FilterRoughness(N, clamp(s.roughness, 0.03, 1.0));
	// (Body paint's lacquer is filtered very little: on every curved panel the filter took the coat from a mirror to
	// a satin finish, and the sun in the paint was a wide soft glow instead of a small bright image. What is left
	// keeps the sun's image a few pixels wide, so that it does not flicker.)
	s.clearCoatRoughness = FilterRoughness(N, max(s.clearCoatRoughness, 0.03), materialClass == CLASS_VEHICLE_PAINT ? 0.00004 : 0.18);

	// ---- body paint, in the body's own space (what is drawn there stays put on a moving car)
	if (AE_MATERIALS && materialClass == CLASS_VEHICLE_PAINT) {
		InstanceData body = g_Instances[input.instance];
		float3 rel = input.worldPos - float3(body.world[0][3], body.world[1][3], body.world[2][3]);
		float3 local = body.normalRow0.xyz * rel.x + body.normalRow1.xyz * rel.y + body.normalRow2.xyz * rel.z; // metres
		// (The coat is a level mirror. An orange peel, waves a few millimetres long leaning its normal, was tried and
		// taken out again: the user wants the paint as mirror-like as it can be.)
		// Metallic paint: the flakes in the base layer. Each is a small mirror lying a little off the surface, so the
		// base layer's highlight breaks up into sparks that come and go with the view, under a coat that stays
		// smooth. Where a pixel covers several flakes they are no longer drawn one by one: together they are a wider lobe.
		float flakeAmount = s.metallic * (1.0 - paintMirror) * (mat.baseColor.a > 0.5 ? 1.0 - base.a : 1.0); // (none under the livery)
		if (flakeAmount > 0.02) {
			const float kFlakeSize = 0.0005;  // metres (larger than real flakes: what a 4K picture can show up close)
			const float kFlakeLean = 0.12;    // how far a flake's normal leans
			// The sparkle is what says "metallic", near and far: real flakes are far smaller than a pixel and a few
			// of them always catch the light. So the cells grow with the distance (doubling, about a pixel wide: a grain, not tiles:
			// there is always a grain to see, coarser from afar and weaker.
			float pixel = length(fwidth(local));
			float level = max(ceil(log2(max(pixel * 0.22 / kFlakeSize, 1e-3))), 0.0);
			float3 cell = floor(local / (kFlakeSize * exp2(level))) + level * 13.0;
			float3 lean = float3(Hash31(cell), Hash31(cell + 17.3), Hash31(cell + 41.7)) * 2.0 - 1.0;
			// (one flake in four lies well off the surface: those are the sparks; the rest make the grain)
			float off = Hash31(cell + 71.1) > 0.75 ? 1.0 : 0.4;
			float strength = kFlakeLean * flakeAmount * off / (1.0 + 0.5 * level);
			s.normal = normalize(N + lean * strength);
		}
	}

	float NoV = max(dot(N, V), 1e-4);
	float3 f0 = lerp(0.04, s.albedo, s.metallic);
	if (AE_MATERIALS && materialClass == CLASS_VEHICLE_PAINT) {
		// Aether's paint, from the base up. The game hands over a colour, how metallic and how rough.
		// - The pigment: a diffuse layer in the paint's colour.
		// - Metallic paint: aluminium flakes lying in that pigment. They are bright mirrors (far brighter than the
		//   colour: a dark metallic grey is not a dark metal); the light reaches them and leaves through the pigment,
		//   which tints it. The more flakes, the less pigment is seen between them.
		// - The clear coat over both (set up above). Under it the pigment has no highlight of its own left (lacquer and
		//   binder have nearly the same index); the flakes keep theirs.
		float flakes = s.metallic * (1.0 - paintMirror) * (mat.baseColor.a > 0.5 ? 1.0 - base.a : 1.0);
		float3 flakeColor = saturate(s.albedo); // (not brightened: tried, and a dark grey came out silver)
		f0 = flakeColor * flakes + 0.04 * (1.0 - s.clearCoat) * (1.0 - flakes);
		// chrome: the colour is the mirror's
		f0 = lerp(f0, max(s.albedo, 0.5) * float3(0.62, 0.63, 0.65), paintMirror); // (chrome returns a little over half the light, not all of it)
		// (metallic paint is dark where the flakes do not mirror anything bright towards the eye: little pigment shows)
		s.albedo *= (1.0 - 0.85 * flakes) * (1.0 - paintMirror);
		// Depth. Pigment under lacquer looks like a wet stone: no light scatters off its surface any more, so the
		// colour is darker and purer (the curve), and darker again towards the panel's edges, where the eye looks
		// through more of the paint (the flop). Against that the coat mirrors more and more of the surroundings
		// towards the edges: a panel that goes from a deep colour in the middle to a bright mirror at the rim is what
		// reads as depth; one even colour with a soft sheen is what reads as plastic.
		float facing = saturate(dot(N, V));
		float lacquer = s.clearCoat * (1.0 - paintMirror);
		s.albedo = lerp(s.albedo, pow(max(s.albedo, 1e-4), 1.45), lacquer);
		s.albedo *= lerp(1.0, lerp(0.28, 1.08, pow(facing, 0.9)), lacquer); // (stronger than at first: the user found the colour still flat)
		// (metallic: the flakes mirror most where they are looked at head on and little from the side)
		f0 *= lerp(1.0, lerp(0.35, 1.15, facing), saturate(flakes * 2.0) * lacquer);
		s.metallic = 0.0;
		// (the flakes do not lie level: their lobe is never a mirror's; the coat is what mirrors)
		s.roughness = max(s.roughness, lerp(s.clearCoatRoughness, 0.17, saturate(flakes * 4.0)));
	} else if (AE_MATERIALS && materialClass == CLASS_VEHICLE_RIM && s.metallic > 0.5) {
		// A wheel's metal. The game hands a chromed or polished rim over as a white, fully metallic, smooth surface:
		// taken as it is, that is a mirror returning all the light, which no metal is, and the wheel came out as a
		// pale shape the colour of the sky. Chromium returns a little over half, slightly more of the blue; the
		// darker the game's colour, the more of it is kept (a painted metallic rim is not chrome).
		float chrome = smoothstep(0.6, 0.95, min(s.albedo.r, min(s.albedo.g, s.albedo.b))) * s.metallic;
		f0 = lerp(f0, float3(0.55, 0.56, 0.58), chrome);
		s.roughness = lerp(s.roughness, min(s.roughness, 0.06), chrome); // (plated: smoother than the game's number says)
		if (s.clearCoat > 0.0) {
			s.roughness = max(s.roughness, s.clearCoatRoughness);
		}
	} else if (AE_MATERIALS && s.clearCoat > 0.0) {
		// under the coat: the base reflects less, and is never smoother than what it is seen through
		f0 = lerp(f0, F0UnderCoat(f0), s.clearCoat);
		s.roughness = max(s.roughness, s.clearCoatRoughness);
	}
	s.f0 = f0;
	s.energy = EnergyCompensation(f0, s.roughness, max(dot(s.normal, V), 1e-4));

	// ---- lighting
	uint cascade = 0, lightCount = 0;
	float shadow = SunShadow(input.worldPos, normalize(input.normal) * (outside ? 1.0 : -1.0), input.viewDepth, cascade);
	shadow *= CloudShadow(input.worldPos); // (the physical sky's clouds drift over the ground)
	float3 direct = 0.0;
	float3 sunLight = 0.0;
	const uint layers = asuint(g_Instances[input.instance].layers.x);
	if (g_SunDirection.w > 0.5 && (asuint(g_ClusterCounts.w) & layers) != 0u) {
		sunLight = DirectLight(s, V, g_SunDirection.xyz, g_SunColor.rgb * shadow);
		direct += sunLight;
	}
	direct += PunctualLights(s, input.worldPos, V, input.position.xy, input.viewDepth, layers, lightCount);

	float3 ambientDiffuse = AmbientLight(input.worldPos, N) * s.albedo * (1.0 - s.metallic);
	float3 R = reflect(-V, N);
	// a rough surface reflects mostly what lies around its normal, not around the mirror direction
	// (the base layer's normal: on metallic paint each flake mirrors its own part of the surroundings)
	float alphaR = s.roughness * s.roughness;
	float3 baseR = reflect(-V, s.normal);
	float3 dominantR = normalize(lerp(s.normal, baseR, (sqrt(1.0 - alphaR) + alphaR) * (1.0 - alphaR)));
	float3 ambientSpecular = EnvironmentSpecular(input.worldPos, dominantR, s.roughness) * EnvBRDFApprox(f0, s.roughness, NoV) * s.energy;
	// what the surroundings hide: the texture's occlusion and the screen space one. The ambient light takes all of it;
	// the mirrored light by how narrow its lobe is; direct light a part (the host's setting).
	float visibility = s.occlusion;
	float3 bounced = 0.0;
	if (g_AOParams.w > 0.5 && (g_DrawFlags & 2u) == 0u) {
		float4 occlusion = g_AO.Load(int3(input.position.xy, 0)); // rgb: light bounced off nearby surfaces, a: ambient occlusion
		visibility *= occlusion.a;
		// (the bounced light is diffuse light like the sky's, and is not occluded again: it was gathered around this very place)
		bounced = occlusion.rgb * g_QualityParams.z * s.albedo * (1.0 - s.metallic) * s.occlusion;
		float directAo = lerp(1.0, occlusion.a, g_AOParams.z);
		direct *= directAo;
		sunLight *= directAo;
	}
	float reflectionOcclusion = SpecularOcclusion(NoV, visibility, s.clearCoat > 0.0 ? s.clearCoatRoughness : s.roughness);
	float3 ambient = ambientDiffuse * visibility + ambientSpecular * SpecularOcclusion(NoV, visibility, s.roughness) + bounced;
	if (s.clearCoat > 0.0) {
		float Fc = (s.coatF0 + (1.0 - s.coatF0) * pow(1.0 - NoV, 5.0)) * s.clearCoat;
		ambient = ambient * (1.0 - Fc) + EnvironmentSpecular(input.worldPos, reflect(-V, s.coatNormal), s.clearCoatRoughness) * Fc * reflectionOcclusion;
	}
	float3 color = direct + ambient + s.emissive;
	float alpha = s.alpha;
	if (s.clearCoat > 0.0) {
		// the coat is what mirrors: its Fresnel term is the share of the mirrored light
		surface = float4(OctEncode(s.coatNormal), s.clearCoatRoughness, saturate((s.coatF0 + (1.0 - s.coatF0) * pow(1.0 - NoV, 5.0)) * s.clearCoat * reflectionOcclusion));
	} else {
		surface = float4(OctEncode(N), s.roughness, saturate(dot(EnvBRDFApprox(f0, s.roughness, NoV), 1.0 / 3.0) * reflectionOcclusion));
	}

	if (materialClass == CLASS_VEHICLE_GLASS) {
		// glass: what is behind it, dimmed by the tint, plus what it mirrors (stronger at grazing angles).
		// The mirrored light is not part of what the tint covers: it is added in full. (Before, it went through the
		// alpha blend with the rest and was scaled down by the glass's own opacity: a window mirrored a few percent
		// of what it does in the game's picture.) The reflectance at normal incidence is matched by eye against the
		// game's picture of the same view: its glass graph has a clear coat and a specular colour, no values to read.
		float fresnel = GLASS_F0 + (1.0 - GLASS_F0) * pow(1.0 - NoV, 5.0);
		float tint = max(mat.baseColor.a, 0.12);
		// (the direct term is mostly the diffuse response of the glass colour, which glass does not have: a small part of
		// it stands in for the sun's highlight)
		float3 body = s.albedo * AmbientLight(input.worldPos, N) * 0.05;
		// what the pane mirrors: a ray through the scene's triangles where those are there to trace (the pane is not in
		// the reflection pass: that pass works on the opaque picture), else the sky and the probes
		float3 mirroredFrom;
		if (g_QualityParams.w > 0.5) {
			BvhHit hit = BvhTrace(input.worldPos + N * (0.004 * input.viewDepth + 0.01), R, 300.0, false);
			float4 mirroredSurface = hit.t < 0.0 ? float4(0.0, 0.0, 0.0, 0.0) : BvhHitSurface(input.worldPos + R * hit.t, hit, hit.t, R);
			// (a hit the texture's alpha cuts away: the sky, as if nothing were there. One ray per pixel here.)
			mirroredFrom = mirroredSurface.a > 0.5 ? min(mirroredSurface.rgb, 64.0) : SkyRadiance(R, 0.0);
		} else {
			mirroredFrom = EnvironmentSpecular(input.worldPos, R, 0.03);
		}
		float3 mirrored = mirroredFrom * fresnel;
		// the sun and the lamps in the glass: a mirror's highlight (no diffuse part: glass has none)
		Surface pane = s;
		pane.albedo = 0.0;
		pane.metallic = 1.0;
		pane.f0 = GLASS_F0;
		pane.energy = 1.0;
		pane.clearCoat = 0.0;
		pane.roughness = max(s.roughness, 0.05);
		uint paneLights = 0;
		if (g_SunDirection.w > 0.5 && (asuint(g_ClusterCounts.w) & layers) != 0u) {
			mirrored += DirectLight(pane, V, g_SunDirection.xyz, g_SunColor.rgb * shadow);
		}
		mirrored += PunctualLights(pane, input.worldPos, V, input.position.xy, input.viewDepth, layers, paneLights);
		alpha = saturate(tint + fresnel);
		color = (body * tint + mirrored) / max(alpha, 1e-3); // (blended by alpha: the sum reaches the picture as it is)
	} else if (materialClass == CLASS_UNLIT) {
		color = s.albedo + s.emissive;
	}

	// (with the volumetric fog on, the opaque picture gets its fog from that pass; what is blended over it still gets it here)
	if (g_Fog.a > 0.0 && (g_QualityParams.y < 0.5 || (g_DrawFlags & 2u) != 0u)) {
		// exponential height fog: full density up to the base height, falling off above it
		float3 toPixel = input.worldPos - g_CameraPos.xyz;
		float dist = length(toPixel);
		float3 viewDir = toPixel / max(dist, 1e-4);
		if (g_FogHeight.z > 0.0) {
			dist = min(dist, g_FogHeight.z);
		}
		float h0 = max(g_CameraPos.y - g_FogHeight.x, 0.0), h1 = max(input.worldPos.y - g_FogHeight.x, 0.0);
		float k = g_FogHeight.y;
		float dh = h1 - h0;
		// the mean of exp(-k h) along the ray
		float meanDensity = abs(dh) < 1e-3 ? exp(-k * h0) : (exp(-k * h0) - exp(-k * h1)) / (k * dh);
		float f = 1.0 - exp(-dist * g_Fog.a * meanDensity);
		float3 fogColor = g_FogHeight.w > 0.5 ? SkyRadiance(viewDir, 0.7) * g_Fog.rgb : g_Fog.rgb;
		color = lerp(color, fogColor, f);
	}

	if ((g_DrawFlags & 2u) == 0u) {
		alpha = 1.0; // (opaque: the alpha is not used; what moves by itself is marked in the velocity target, see PSForward)
	}

	// ---- debug views
	if (view != VIEW_NONE) {
		// the composite pass shows these as they are (no exposure, no tone mapping)
		if (view == VIEW_ALBEDO) color = s.albedo;
		else if (view == VIEW_NORMAL) color = N * 0.5 + 0.5;
		else if (view == VIEW_ROUGHNESS) color = s.roughness;
		else if (view == VIEW_METALLIC) {
			color = s.metallic;
			if (materialClass == CLASS_VEHICLE_PAINT) {
				// body paint: the game's own three numbers as they arrive (red roughness, green metalness, blue clear
				// coat), with squares of 0.5 to read them against
				color = fmod(floor(input.position.x / 48.0) + floor(input.position.y / 48.0), 2.0) > 0.5 ? float3(mat.params.x, mat.emissiveMetallic.a, mat.params2.x) : float3(0.5, 0.5, 0.5);
			}
		}
		else if (view == VIEW_DEPTH) color = frac(input.viewDepth * float3(1.0, 0.1, 0.01));
		else if (view == VIEW_LIGHTING) color = (direct + ambient) / max(s.albedo, 0.05) * g_Exposure.x;
		else if (view == VIEW_SHADOW) color = shadow * lerp(float3(1, 0.4, 0.4), float3(0.4, 0.4, 1), cascade / 3.0);
		else if (view == VIEW_MATERIAL_CLASS) color = kClassColors[min(materialClass, 13u)] * (0.3 + 0.7 * saturate(dot(N, V)));
		else if (view == VIEW_SUN) color = sunLight;
		else if (view == VIEW_AMBIENT) color = ambient;
		else if (view == VIEW_REFLECTION) color = EnvironmentSpecular(input.worldPos, R, 0.0);
		alpha = 1.0;
		surface.a = 0.0;
	}
	return float4(color, alpha);
}

struct ForwardOutput {
	float4 color : SV_Target0;
	float4 surface : SV_Target1; // (only the opaque pass has a second and a third target)
	float4 velocity : SV_Target2; // xy how far the pixel moved on screen since the frame before (uv units), z 1 = part of something that moves by itself
};

ForwardOutput PSForward(VSOutput input, bool frontFace : SV_IsFrontFace) {
	ForwardOutput o;
	o.color = ShadeForward(input, frontFace, o.surface);
	float2 now = input.clipNow.xy / max(input.clipNow.w, 1e-5), before = input.clipBefore.xy / max(input.clipBefore.w, 1e-5);
	o.velocity = float4(input.clipBefore.w > 1e-4 ? (now - before) * float2(0.5, -0.5) : float2(0.0, 0.0), g_Instances[input.instance].layers.y > 0.5 ? 1.0 : 0.0, 0.0);
	return o;
}
