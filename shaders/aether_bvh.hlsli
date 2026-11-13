// Rays through the scene's triangles with DXR 1.1 (inline ray tracing, RayQuery): the hardware walks the
// acceleration structures src/Aether.Native/Renderer/Bvh.cpp builds (a bottom level structure per mesh, a top level
// one over the frame's instances). The triangles' data for shading a hit (texture coordinates, sub mesh) is in
// g_BvhTris, in the order the structures hold them: a hit's instance ID (its mesh's first triangle) plus its
// primitive index is its place there; its instance index is its place in g_TlasInstances.
// AE_NO_RAYS (a device without DXR 1.1): no ray hits anything.
// Included by scene_post.hlsl and scene_forward.hlsl after aether_scene.hlsli.

struct BvhTri {
	float3 v0;
	uint subMesh;
	float3 e1;
	float uvU;   // the first vertex's texture coordinate
	float3 e2;
	float uvV;
	float2 duv1; // the second vertex's texture coordinate minus the first's
	float2 duv2; // the third's minus the first's
};
struct TlasInstance {
	float4 row0, row1, row2; // world -> the mesh's space (for the hit's normal)
	uint triBase;            // the mesh's first triangle in g_BvhTris (also the instance's ID in the structure)
	uint skipSubMeshes;      // bit per sub mesh (the first 32): not there for rays (glass, helper geometry)
	uint materialOffset;     // where its sub meshes' materials start in g_RayMaterials; bit 31: mirrored
	uint doubleSided;        // bit per sub mesh (the first 32): hit from behind too
};

StructuredBuffer<BvhTri> g_BvhTris : register(t7);
#ifndef AE_NO_RAYS
RaytracingAccelerationStructure g_Scene : register(t8);
#endif
StructuredBuffer<TlasInstance> g_TlasInstances : register(t9);
StructuredBuffer<uint2> g_RayMaterials : register(t10); // per instance and sub mesh: x its index in g_Materials, y its base texture in g_SceneTextures (0xFFFFFFFF: none)
// every texture in the SRV heap, by its place there: a hit reads the base texture of whatever it landed on
Texture2D g_SceneTextures[16384] : register(t0, space5);

struct BvhHit {
	float t;        // distance along the ray (in units of the direction's length); < 0: nothing hit
	uint instance;  // index in g_TlasInstances
	uint tri;       // index in g_BvhTris
	float3 normal;  // the triangle's normal in the world, facing the ray's origin
	float2 bary;    // where in the triangle: the second and third vertex's shares
	uint back;      // 1: the triangle was met from behind (only with backFaces)
};

// The nearest triangle along o + d t for t in (0, tMax). `anyHit`: stop at the first one found (shadow rays).
// `backFaces`: single sided triangles met from behind count too (hit.back says so): for the GI probes, which must
// know when they look at the inside of something.
// Opaque instances are decided by the hardware (back faces culled by the ray's flag). The others (some sub meshes not
// there for rays, or double sided) come back as candidates and are decided here per triangle.
BvhHit BvhTraceEx(float3 o, float3 d, float tMax, bool anyHit, bool backFaces) {
	BvhHit hit;
	hit.t = -1.0;
	hit.instance = 0;
	hit.tri = 0;
	hit.normal = 0.0;
	hit.bary = 0.0;
	hit.back = 0;
#ifndef AE_NO_RAYS
	RayDesc ray;
	ray.Origin = o;
	ray.Direction = d;
	ray.TMin = 1e-4;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_NONE> q;
	uint flags = (backFaces ? RAY_FLAG_NONE : RAY_FLAG_CULL_BACK_FACING_TRIANGLES) | (anyHit ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE);
	q.TraceRayInline(g_Scene, flags, 0xFF, ray);
	[loop] while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) {
			continue;
		}
		TlasInstance inst = g_TlasInstances[q.CandidateInstanceIndex()];
		uint subMesh = g_BvhTris[q.CandidateInstanceID() + q.CandidatePrimitiveIndex()].subMesh;
		if (subMesh < 32u && ((inst.skipSubMeshes >> subMesh) & 1u) != 0u) {
			continue;
		}
		// A triangle seen from behind is not there, as in the rasterised picture (unless its material is double sided).
		bool behind = !q.CandidateTriangleFrontFace() && (subMesh >= 32u || ((inst.doubleSided >> subMesh) & 1u) == 0u);
		if (behind && !backFaces) {
			continue;
		}
		q.CommitNonOpaqueTriangleHit();
		if (anyHit) {
			q.Abort();
		}
	}
	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
		hit.t = q.CommittedRayT();
		hit.instance = q.CommittedInstanceIndex();
		hit.tri = q.CommittedInstanceID() + q.CommittedPrimitiveIndex();
		hit.bary = q.CommittedTriangleBarycentrics();
		TlasInstance inst = g_TlasInstances[hit.instance];
		BvhTri tri = g_BvhTris[hit.tri];
		bool doubleSided = tri.subMesh < 32u && ((inst.doubleSided >> tri.subMesh) & 1u) != 0u;
		hit.back = !q.CommittedTriangleFrontFace() && !doubleSided ? 1u : 0u;
		// the normal: the triangle's, taken to the world by the transpose of world -> mesh (its rows as columns)
		float3 n = cross(tri.e1, tri.e2);
		float3 world = normalize(inst.row0.xyz * n.x + inst.row1.xyz * n.y + inst.row2.xyz * n.z);
		hit.normal = dot(world, d) > 0.0 ? -world : world;
	}
#endif
	return hit;
}

BvhHit BvhTrace(float3 o, float3 d, float tMax, bool anyHit) {
	return BvhTraceEx(o, d, tMax, anyHit, false);
}

// What a ray hit, where the screen cannot tell (the caller looks there first, if it can): the surface's own colour
// from its material and base texture, lit by the sun (through the shadow map where that reaches, and the clouds) and
// the sky. rgb: the light it sends back, a: 0 = nothing there after all (the texture's alpha cuts it away: the caller
// goes on behind it).
// (The reflection probes are not asked any more: they stand on a blind grid, some inside things, and what they did
// not see was coloured without its texture: the mirrored ground came out as a pale band.)
// `t`: how far the ray went (the further, the coarser the texture level read: there are no derivatives here).
// `d`: the ray's direction.
// `punctual`: lit by the scene's lamps as well (all of them that reach it, without shadows): for the GI probes.
float4 BvhHitSurfaceLit(float3 P, BvhHit hit, float t, float3 d, bool punctual) {
	TlasInstance inst = g_TlasInstances[hit.instance];
	BvhTri tri = g_BvhTris[hit.tri];
	uint2 entry = g_RayMaterials[(inst.materialOffset & 0x7FFFFFFFu) + tri.subMesh];
	MaterialData mat = g_Materials[entry.x];
	uint cls = (uint)mat.params2.z;
	float4 tex = 1.0;
	if (entry.y != 0xFFFFFFFFu) {
		float2 uv = (float2(tri.uvU, tri.uvV) + tri.duv1 * hit.bary.x + tri.duv2 * hit.bary.y) * mat.uvScaleOffset.xy + mat.uvScaleOffset.zw;
		tex = g_SceneTextures[NonUniformResourceIndex(entry.y)].SampleLevel(s_Aniso, uv, 1.0 + 0.7 * log2(max(t, 1.0)));
	}
	if (mat.params.z >= 0.0 && tex.a * mat.baseColor.a < mat.params.z) {
		return 0.0; // alpha tested away (leaves, fences)
	}
	// the colour, the way the forward shader reads each kind of material (without the finer points)
	float3 albedo;
	if (cls == CLASS_VEHICLE_PAINT) {
		albedo = mat.baseColor.a > 0.5 ? lerp(mat.baseColor.rgb, tex.rgb, tex.a) : mat.baseColor.rgb; // (the texture is the livery)
	} else if (cls == CLASS_VEHICLE_RIM) {
		albedo = mat.baseColor.rgb;
	} else if (cls == CLASS_VEHICLE_GENERIC || cls == CLASS_VEHICLE_TIRE || cls == CLASS_VEHICLE_LIGHTS || cls == CLASS_COLOR_MASK) {
		albedo = tex.rgb;
	} else {
		albedo = tex.rgb * mat.baseColor.rgb;
	}
	float3 n = hit.normal;
	float3 light = AmbientLight(P, n);
	float sun = 0.0;
	if (g_SunDirection.w > 0.5) {
		uint cascade = 0;
		sun = SunShadow(P, n, 0.0, cascade) * CloudShadow(P);
		light += g_SunColor.rgb * (saturate(dot(n, g_SunDirection.xyz)) * sun / PI);
	}
	if (punctual) {
		uint count = min((uint)g_GiParams.w, 128u);
		[loop] for (uint li = 0; li < count; ++li) {
			LightData lamp = g_Lights[li];
			float3 toLight = lamp.positionRange.xyz - P;
			float d2 = dot(toLight, toLight);
			float range = lamp.positionRange.w;
			float nl = dot(n, toLight);
			if (d2 >= range * range || nl <= 0.0) {
				continue;
			}
			float3 L = toLight * rsqrt(max(d2, 1e-6));
			float w = saturate(1.0 - (d2 * d2) / (range * range * range * range));
			// (no nearer than half a metre: a lamp set into a wall would make one texel of a probe blaze)
			float attenuation = w * w / max(d2, 0.25);
			if (lamp.typeFlags.x > 1.5) {
				float outer = lamp.directionOuter.w, inner = lamp.colorInner.w;
				attenuation *= smoothstep(outer, max(inner, outer + 1e-4), dot(-L, lamp.directionOuter.xyz));
				if (lamp.typeFlags.z > 0.5) {
					attenuation *= 0.15; // (a head lamp's cone is wide and mostly dim: see HeadlampBeam)
				}
			}
			light += lamp.colorInner.rgb * (dot(n, L) * attenuation / PI);
		}
	}
	float3 lit = saturate(albedo) * light;
	// Metal has no colour of its own to light: it shows what it mirrors in turn. (Lit like paint, a chromed wheel
	// seen in its own barrel, or in the car's door, was a white shape.) No second ray: the sky where its mirror
	// direction looks up, and below the horizon the ground as the sun and the sky light it; half of it, for what a
	// wheel's own shapes and the car hide.
	float metal = cls == CLASS_VEHICLE_RIM ? mat.emissiveMetallic.a : cls == CLASS_STANDARD ? mat.emissiveMetallic.a * saturate(1.0 - mat.params.x * 2.0) : 0.0;
	if (metal > 0.01) {
		float3 r = reflect(d, n);
		float3 ground = 0.12 * (AmbientDiffuse(float3(0.0, 1.0, 0.0)) * g_Misc.w + g_SunColor.rgb * (saturate(g_SunDirection.y) * sun / PI));
		float3 mirrored = lerp(ground, SkyRadiance(r, 0.0), smoothstep(-0.05, 0.15, r.y));
		float3 f0 = cls == CLASS_VEHICLE_RIM ? lerp(saturate(albedo), float3(0.55, 0.56, 0.58), smoothstep(0.6, 0.95, min(albedo.r, min(albedo.g, albedo.b)))) : saturate(albedo);
		lit = lerp(lit, f0 * mirrored * 0.5, saturate(metal));
	}
	return float4(lit, 1.0);
}

float4 BvhHitSurface(float3 P, BvhHit hit, float t, float3 d) {
	return BvhHitSurfaceLit(P, hit, t, d, false);
}
