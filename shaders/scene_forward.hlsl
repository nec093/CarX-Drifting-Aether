// Aether forward+ scene shaders: vertex shader, depth / shadow pixel shader, lit pixel shader.
#include "aether_scene.hlsli"

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
};

VSOutput VSMain(VSInput input) {
	InstanceData inst = g_Instances[g_InstanceIndex];
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
	return o;
}

// Shadow cascades: the same vertex data through the cascade's matrix.
float4 VSShadow(VSInput input, out float2 uv : TEXCOORD0) : SV_Position {
	InstanceData inst = g_Instances[g_InstanceIndex];
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
};

float3 DirectLight(Surface s, float3 V, float3 L, float3 radiance) {
	float NoL = saturate(dot(s.normal, L));
	if (NoL <= 0.0) {
		return 0.0;
	}
	float3 H = normalize(V + L);
	float NoV = max(dot(s.normal, V), 1e-4);
	float NoH = saturate(dot(s.normal, H));
	float VoH = saturate(dot(V, H));
	float a = max(s.roughness * s.roughness, 0.002);
	float3 f0 = lerp(0.04, s.albedo, s.metallic);
	float3 diffuse = s.albedo * (1.0 - s.metallic) / PI;
	float3 F = F_Schlick(f0, VoH);
	float3 specular = D_GGX(NoH, a) * V_SmithGGX(NoV, NoL, a) * F;
	float3 base = (diffuse * (1.0 - F) + specular) * NoL;
	if (s.clearCoat > 0.0) {
		// a second, dielectric lobe on top (f0 0.04); the base layer loses what the coat reflects
		float ac = max(s.clearCoatRoughness * s.clearCoatRoughness, 0.002);
		float Fc = (0.04 + 0.96 * pow(1.0 - VoH, 5.0)) * s.clearCoat;
		float coat = D_GGX(NoH, ac) * V_SmithGGX(NoV, NoL, ac) * Fc * NoL;
		base = base * (1.0 - Fc) + coat;
	}
	return base * radiance;
}

float SunShadow(float3 worldPos, float3 normal, float viewDepth, out uint cascade) {
	cascade = 0;
	uint count = (uint)g_ShadowParams.x;
	if (count == 0 || g_SunColor.w < 0.5) {
		return 1.0;
	}
	for (uint i = 0; i < count; ++i) {
		cascade = i;
		if (viewDepth < g_CascadeSplits[i]) {
			break;
		}
	}
	if (viewDepth >= g_CascadeSplits[count - 1]) {
		return 1.0; // beyond the last cascade
	}
	// push the sample off the surface along the normal by about one shadow texel of this cascade
	float texelWorld = g_ShadowParams.w * (1.0 + (float)cascade * 1.5);
	float3 p = worldPos + normal * texelWorld;
	float4 sc = mul(g_ShadowMatrix[cascade], float4(p, 1.0));
	float depth = sc.z - g_ShadowParams.z;
	float texel = g_ShadowParams.y;
	float sum = 0.0;
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			sum += g_ShadowAtlas.SampleCmpLevelZero(s_Shadow, sc.xy + float2(x, y) * texel, depth);
		}
	}
	return sum / 9.0;
}

float3 PunctualLights(Surface s, float3 worldPos, float3 V, float2 pixel, float viewDepth, out uint lightCount) {
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
		if (light.typeFlags.x > 1.5) {
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

float4 PSForward(VSOutput input, bool frontFace : SV_IsFrontFace) : SV_Target {
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
	s.albedo = base.rgb * mat.baseColor.rgb;
	s.alpha = base.a * mat.baseColor.a;
	s.metallic = mat.emissiveMetallic.a;
	s.roughness = mat.params.x;
	s.occlusion = 1.0;
	s.clearCoat = mat.params2.x;
	s.clearCoatRoughness = mat.params2.y;
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
		// colour mask map: each channel tints with one of the material's colours, by that colour's alpha
		if (mat.params3.w > 0.5) {
			float3 mask = g_ExtraMap.Sample(s_Aniso, input.uv).rgb;
			s.albedo = lerp(s.albedo, base.rgb * mat.secondaryColor.rgb, mask.g * mat.secondaryColor.a);
		}
	} else if (materialClass == CLASS_VEHICLE_PAINT) {
		// two tone paint: the secondary colour shows at grazing angles (weight in its alpha)
		float facing = saturate(dot(normalize(input.normal), V));
		s.albedo = lerp(s.albedo, base.rgb * mat.secondaryColor.rgb, mat.secondaryColor.a * pow(1.0 - facing, 2.0));
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
	s.roughness = clamp(s.roughness, 0.03, 1.0);

	// ---- lighting
	uint cascade = 0, lightCount = 0;
	float shadow = SunShadow(input.worldPos, normalize(input.normal) * (outside ? 1.0 : -1.0), input.viewDepth, cascade);
	float3 direct = 0.0;
	if (g_SunDirection.w > 0.5) {
		direct += DirectLight(s, V, g_SunDirection.xyz, g_SunColor.rgb * shadow);
	}
	direct += PunctualLights(s, input.worldPos, V, input.position.xy, input.viewDepth, lightCount);

	float NoV = max(dot(N, V), 1e-4);
	float3 f0 = lerp(0.04, s.albedo, s.metallic);
	float3 ambientDiffuse = AmbientDiffuse(N) * g_Misc.w * s.albedo * (1.0 - s.metallic);
	float3 R = reflect(-V, N);
	float3 ambientSpecular = SkyRadiance(R, s.roughness) * EnvBRDFApprox(f0, s.roughness, NoV);
	float3 ambient = (ambientDiffuse + ambientSpecular) * s.occlusion;
	if (s.clearCoat > 0.0) {
		float Fc = (0.04 + 0.96 * pow(1.0 - NoV, 5.0)) * s.clearCoat;
		ambient = ambient * (1.0 - Fc) + SkyRadiance(R, s.clearCoatRoughness) * Fc * s.occlusion;
	}
	float3 color = direct + ambient + s.emissive;
	float alpha = s.alpha;

	if (materialClass == CLASS_VEHICLE_GLASS) {
		// glass: mostly what is behind it, plus the reflection, stronger at grazing angles
		float fresnel = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);
		float tint = max(mat.baseColor.a, 0.12);
		color = s.albedo * (AmbientDiffuse(N) * g_Misc.w) * 0.2 + SkyRadiance(R, 0.02) * fresnel + direct;
		alpha = saturate(tint + fresnel);
	} else if (materialClass == CLASS_UNLIT) {
		color = s.albedo + s.emissive;
	}

	if (g_Fog.a > 0.0) {
		float f = 1.0 - exp(-input.viewDepth * g_Fog.a);
		color = lerp(color, g_Fog.rgb, f);
	}

	// ---- debug views
	if (view != VIEW_NONE) {
		// the composite pass shows these as they are (no exposure, no tone mapping)
		if (view == VIEW_ALBEDO) color = s.albedo;
		else if (view == VIEW_NORMAL) color = N * 0.5 + 0.5;
		else if (view == VIEW_ROUGHNESS) color = s.roughness;
		else if (view == VIEW_METALLIC) color = s.metallic;
		else if (view == VIEW_DEPTH) color = frac(input.viewDepth * float3(1.0, 0.1, 0.01));
		else if (view == VIEW_LIGHTING) color = (direct + ambient) / max(s.albedo, 0.05) * g_Exposure.x;
		else if (view == VIEW_SHADOW) color = shadow * lerp(float3(1, 0.4, 0.4), float3(0.4, 0.4, 1), cascade / 3.0);
		else if (view == VIEW_MATERIAL_CLASS) color = kClassColors[min(materialClass, 13u)] * (0.3 + 0.7 * saturate(dot(N, V)));
		alpha = 1.0;
	}
	return float4(color, alpha);
}
