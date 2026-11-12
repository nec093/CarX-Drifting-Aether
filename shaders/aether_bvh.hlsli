// Rays through the scene's triangles, in a shader (no DXR): the two level bounding volume hierarchy that
// src/Aether.Native/Renderer/Bvh.cpp builds. A ray walks the tree over this frame's instances in the world; at an
// instance it moves into the mesh's own space and walks that mesh's tree down to its triangles.
// Included by scene_post.hlsl after aether_scene.hlsli.

struct BvhNode {
	float3 bmin;
	uint a; // inner node: its first child (the second is the next node); leaf: its first triangle / instance
	float3 bmax;
	uint b; // 0: inner node; else a leaf with this many triangles / instances
};
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
	float4 row0, row1, row2; // world -> the mesh's space
	uint root;               // the mesh's root node
	uint skipSubMeshes;      // bit per sub mesh (the first 32): not there for rays (glass, helper geometry)
	uint materialOffset;     // where its sub meshes' materials start in g_RayMaterials; bit 31: mirrored
	uint doubleSided;        // bit per sub mesh (the first 32): hit from behind too
};

StructuredBuffer<BvhNode> g_BvhNodes : register(t6);
StructuredBuffer<BvhTri> g_BvhTris : register(t7);
StructuredBuffer<BvhNode> g_TlasNodes : register(t8);
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
	uint steps;     // boxes and triangles tested (the ray's cost)
	uint back;      // 1: the triangle was met from behind (only asked for by BvhTraceEx)
};

// Where the ray enters the box, or a huge number when it misses it or the box lies beyond tMax.
float BvhBox(float3 o, float3 inv, float3 bmin, float3 bmax, float tMax) {
	float3 t0 = (bmin - o) * inv, t1 = (bmax - o) * inv;
	float3 lo = min(t0, t1), hi = max(t0, t1);
	float tn = max(max(lo.x, lo.y), max(lo.z, 0.0)), tf = min(min(hi.x, hi.y), min(hi.z, tMax));
	return tn <= tf ? tn : 1e30;
}

float3 BvhSafeInverse(float3 d) {
	return 1.0 / (abs(d) > 1e-8 ? d : (d >= 0.0 ? 1e-8 : -1e-8));
}

// The nearest triangle along o + d t for t in (0, tMax). `anyHit`: stop at the first one found (shadow rays).
// `backFaces`: single sided triangles met from behind count too (hit.back says so): for the GI probes, which must
// know when they look at the inside of something.
// `maxSteps`: the ray gives up after that many boxes and triangles and returns what it has (a pass takes as long as
// its slowest ray: a few rays through clutter held the GI pass at over a millisecond however few rays there were).
BvhHit BvhTraceEx(float3 o, float3 d, float tMax, bool anyHit, bool backFaces, uint maxSteps) {
	BvhHit hit;
	hit.t = -1.0;
	hit.instance = 0;
	hit.tri = 0;
	hit.normal = 0.0;
	hit.bary = 0.0;
	hit.steps = 0;
	hit.back = 0;
	float best = tMax;
	float3 inv = BvhSafeInverse(d);
	uint topStack[24];
	uint meshStack[32];
	int top = 0;
	topStack[0] = 0;
	hit.steps++;
	if (BvhBox(o, inv, g_TlasNodes[0].bmin, g_TlasNodes[0].bmax, best) > 1e29) {
		return hit;
	}
	[loop] while (top >= 0 && hit.steps < maxSteps) {
		BvhNode node = g_TlasNodes[topStack[top--]];
		if (node.b == 0) {
			// the nearer child first: what it finds shortens the ray for the other
			BvhNode l = g_TlasNodes[node.a], r = g_TlasNodes[node.a + 1];
			float tl = BvhBox(o, inv, l.bmin, l.bmax, best), tr = BvhBox(o, inv, r.bmin, r.bmax, best);
			hit.steps += 2;
			bool leftFirst = tl <= tr;
			uint first = leftFirst ? node.a : node.a + 1, second = leftFirst ? node.a + 1 : node.a;
			if (max(tl, tr) < 1e29 && top < 22) {
				topStack[++top] = second;
			}
			if (min(tl, tr) < 1e29 && top < 22) {
				topStack[++top] = first;
			}
			continue;
		}
		// an instance: the ray in the mesh's own space (the direction is not normalised again, so t means the same)
		TlasInstance inst = g_TlasInstances[node.a];
		float3 lo = float3(dot(inst.row0.xyz, o) + inst.row0.w, dot(inst.row1.xyz, o) + inst.row1.w, dot(inst.row2.xyz, o) + inst.row2.w);
		float3 ld = float3(dot(inst.row0.xyz, d), dot(inst.row1.xyz, d), dot(inst.row2.xyz, d));
		float3 linv = BvhSafeInverse(ld);
		int sp = 0;
		meshStack[0] = inst.root;
		hit.steps++;
		if (BvhBox(lo, linv, g_BvhNodes[inst.root].bmin, g_BvhNodes[inst.root].bmax, best) > 1e29) {
			continue;
		}
		[loop] while (sp >= 0 && hit.steps < maxSteps) {
			BvhNode n = g_BvhNodes[meshStack[sp--]];
			if (n.b == 0) {
				BvhNode l = g_BvhNodes[n.a], r = g_BvhNodes[n.a + 1];
				float tl = BvhBox(lo, linv, l.bmin, l.bmax, best), tr = BvhBox(lo, linv, r.bmin, r.bmax, best);
				hit.steps += 2;
				bool leftFirst = tl <= tr;
				uint first = leftFirst ? n.a : n.a + 1, second = leftFirst ? n.a + 1 : n.a;
				if (max(tl, tr) < 1e29 && sp < 30) {
					meshStack[++sp] = second;
				}
				if (min(tl, tr) < 1e29 && sp < 30) {
					meshStack[++sp] = first;
				}
				continue;
			}
			for (uint i = 0; i < n.b; ++i) {
				// Moeller and Trumbore
				BvhTri tri = g_BvhTris[n.a + i];
				hit.steps++;
				if (tri.subMesh < 32u && ((inst.skipSubMeshes >> tri.subMesh) & 1u) != 0u) {
					continue;
				}
				float3 p = cross(ld, tri.e2);
				float det = dot(tri.e1, p);
				if (abs(det) < 1e-12) {
					continue;
				}
				// A triangle seen from behind is not there, as in the rasterised picture (unless its material is double
				// sided). Left in, single sided things (ground planes, backdrops) blocked rays from below and behind.
				// (det > 0: the ray meets the front, the side Unity's winding faces. This is asked in the mesh's own space,
				// where a mirrored instance's triangles are what they always were: mirroring changes nothing here.)
				bool behind = det < 0.0 && (tri.subMesh >= 32u || ((inst.doubleSided >> tri.subMesh) & 1u) == 0u);
				if (behind && !backFaces) {
					continue;
				}
				float invDet = 1.0 / det;
				float3 s = lo - tri.v0;
				float u = dot(s, p) * invDet;
				float3 q = cross(s, tri.e1);
				float v = dot(ld, q) * invDet;
				float t = dot(tri.e2, q) * invDet;
				if (u >= 0.0 && v >= 0.0 && u + v <= 1.0 && t > 1e-4 && t < best) {
					best = t;
					hit.t = t;
					hit.instance = node.a;
					hit.tri = n.a + i;
					hit.bary = float2(u, v);
					hit.back = behind ? 1u : 0u;
				}
			}
			if (anyHit && hit.t > 0.0) {
				return hit;
			}
		}
	}
	if (hit.t > 0.0) {
		// the normal: the triangle's, taken to the world by the transpose of world -> mesh (its rows as columns)
		TlasInstance inst = g_TlasInstances[hit.instance];
		BvhTri tri = g_BvhTris[hit.tri];
		float3 n = cross(tri.e1, tri.e2);
		float3 world = normalize(inst.row0.xyz * n.x + inst.row1.xyz * n.y + inst.row2.xyz * n.z);
		hit.normal = dot(world, d) > 0.0 ? -world : world;
	}
	return hit;
}

BvhHit BvhTrace(float3 o, float3 d, float tMax, bool anyHit) {
	return BvhTraceEx(o, d, tMax, anyHit, false, 100000u);
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
