using System;
using System.Reflection;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.HighDefinition;

namespace Aether {
	/// <summary>
	/// A read-only survey of what the game's renderers are made of: the property layout of every shader in the scene
	/// (with one material's values), the vertex layouts of the meshes, the lights in HDRP's units, the global
	/// environment. This is what Aether's scene capture and material translation are written against
	/// (docs/architecture/scene-capture.md). Published as "survey" on request (MCP get_material_info detail=survey).
	/// </summary>
	internal static class RenderSurvey {
		public static string Build() {
			var j = new Json().BeginObject();
			Renderer[] renderers = UnityEngine.Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None);

			// ---- shaders: property layout + the values of the first material seen
			var shaders = new Dictionary<Shader, Material>();
			var shared = new List<Material>();
			foreach (Renderer r in renderers) {
				shared.Clear();
				r.GetSharedMaterials(shared);
				foreach (Material m in shared) {
					if (m != null && m.shader != null && !shaders.ContainsKey(m.shader)) {
						shaders[m.shader] = m;
					}
				}
			}
			j.BeginArray("shaders");
			foreach (var kv in shaders) {
				Shader s = kv.Key;
				Material m = kv.Value;
				j.BeginObject().Add("shader", s.name).Add("sampleMaterial", m.name).Add("renderQueue", m.renderQueue);
				j.BeginArray("keywords");
				foreach (string k in m.shaderKeywords) {
					j.Add(null, k);
				}
				j.EndArray();
				j.BeginArray("passes");
				for (int p = 0; p < m.passCount && p < 24; ++p) {
					j.Add(null, m.GetPassName(p));
				}
				j.EndArray();
				j.BeginArray("properties");
				int n = s.GetPropertyCount();
				for (int i = 0; i < n && i < 160; ++i) {
					string name = s.GetPropertyName(i);
					ShaderPropertyType type = s.GetPropertyType(i);
					j.BeginObject().Add("name", name).Add("type", type.ToString());
					switch (type) {
					case ShaderPropertyType.Color: {
						Color c = m.GetColor(name);
						j.Add("value", c.r.ToString("0.###") + "," + c.g.ToString("0.###") + "," + c.b.ToString("0.###") + "," + c.a.ToString("0.###"));
						break;
					}
					case ShaderPropertyType.Vector: {
						Vector4 v = m.GetVector(name);
						j.Add("value", v.x.ToString("0.###") + "," + v.y.ToString("0.###") + "," + v.z.ToString("0.###") + "," + v.w.ToString("0.###"));
						break;
					}
					case ShaderPropertyType.Float:
					case ShaderPropertyType.Range:
						j.Add("value", m.GetFloat(name));
						break;
					case ShaderPropertyType.Int:
						j.Add("value", m.GetInteger(name));
						break;
					case ShaderPropertyType.Texture: {
						Texture t = m.GetTexture(name);
						if (t != null) {
							Vector2 scale = m.GetTextureScale(name), offset = m.GetTextureOffset(name);
							j.Add("value", t.name + " [" + t.GetType().Name + " " + t.dimension + " " + t.width + "x" + t.height + " " + t.graphicsFormat + " mips " + t.mipmapCount + "] st " + scale.x + "," + scale.y + "," + offset.x + "," + offset.y);
						} else {
							j.Add("value", (string)null);
						}
						break;
					}
					}
					j.EndObject();
				}
				j.EndArray();
				j.EndObject();
			}
			j.EndArray();

			// ---- meshes: vertex layouts, index formats, static batching, skinning
			var layouts = new Dictionary<string, int>();
			var meshes = new HashSet<int>();
			int meshRenderers = 0, skinned = 0, staticBatched = 0, noMesh = 0, index32 = 0, multiStream = 0, nonTriangle = 0, negativeScale = 0, other = 0;
			long vertices = 0, indices = 0;
			var attributes = new List<VertexAttributeDescriptor>();
			var otherTypes = new Dictionary<string, int>();
			foreach (Renderer r in renderers) {
				Mesh mesh = null;
				if (r is MeshRenderer) {
					++meshRenderers;
					var filter = r.GetComponent<MeshFilter>();
					mesh = filter != null ? filter.sharedMesh : null;
					if (r.isPartOfStaticBatch) {
						++staticBatched;
					}
				} else if (r is SkinnedMeshRenderer sk) {
					++skinned;
					mesh = sk.sharedMesh;
				} else {
					++other;
					string t = r.GetType().Name;
					otherTypes.TryGetValue(t, out int c);
					otherTypes[t] = c + 1;
					continue;
				}
				if (mesh == null) {
					++noMesh;
					continue;
				}
				if (r.transform.localToWorldMatrix.determinant < 0) {
					++negativeScale;
				}
				if (!meshes.Add(mesh.GetInstanceID())) {
					continue;
				}
				vertices += mesh.vertexCount;
				if (mesh.indexFormat == IndexFormat.UInt32) {
					++index32;
				}
				if (mesh.vertexBufferCount > 1) {
					++multiStream;
				}
				for (int s = 0; s < mesh.subMeshCount; ++s) {
					SubMeshDescriptor d = mesh.GetSubMesh(s);
					indices += d.indexCount;
					if (d.topology != MeshTopology.Triangles) {
						++nonTriangle;
					}
				}
				attributes.Clear();
				mesh.GetVertexAttributes(attributes);
				var sb = new System.Text.StringBuilder();
				foreach (VertexAttributeDescriptor a in attributes) {
					sb.Append(a.attribute).Append(':').Append(a.format).Append('x').Append(a.dimension).Append("@s").Append(a.stream).Append(' ');
				}
				string key = sb.ToString().TrimEnd();
				layouts.TryGetValue(key, out int count);
				layouts[key] = count + 1;
			}
			j.BeginObject("meshes");
			j.Add("renderers", renderers.Length).Add("meshRenderers", meshRenderers).Add("skinnedMeshRenderers", skinned).Add("otherRenderers", other);
			j.Add("staticBatchedRenderers", staticBatched).Add("renderersWithoutMesh", noMesh).Add("negativeScaleRenderers", negativeScale);
			j.Add("uniqueMeshes", meshes.Count).Add("uniqueMeshVertices", vertices).Add("uniqueMeshIndices", indices);
			j.Add("index32Meshes", index32).Add("multiStreamMeshes", multiStream).Add("nonTriangleSubMeshes", nonTriangle);
			j.BeginObject("otherRendererTypes");
			foreach (var kv in otherTypes) {
				j.Add(kv.Key, kv.Value);
			}
			j.EndObject();
			j.BeginArray("vertexLayouts");
			foreach (var kv in layouts) {
				j.BeginObject().Add("meshes", kv.Value).Add("layout", kv.Key).EndObject();
			}
			j.EndArray();
			j.EndObject();

			// ---- what lies under a few screen points (physics ray from the screen camera): the renderer and its materials' values
			j.BeginArray("screenProbes");
			try {
				Camera probeCamera = Camera.main;
				if (probeCamera != null) {
					foreach (Vector2 point in new[] { new Vector2(0.13f, 0.12f), new Vector2(0.5f, 0.04f), new Vector2(0.3f, 0.4f) }) {
						Ray ray = probeCamera.ViewportPointToRay(new Vector3(point.x, point.y, 0));
						if (!Physics.Raycast(ray, out RaycastHit hit, 200.0f)) {
							continue;
						}
						Renderer hitRenderer = hit.collider.GetComponent<Renderer>();
						if (hitRenderer == null) {
							// (the collider is an object of its own: the visible, opaque renderer with the smallest bounds around the point)
							float best = float.MaxValue;
							foreach (MeshRenderer candidate in UnityEngine.Object.FindObjectsByType<MeshRenderer>(FindObjectsSortMode.None)) {
								Bounds bounds = candidate.bounds;
								bounds.Expand(new Vector3(0, 0.3f, 0));
								float size = bounds.size.x * bounds.size.z;
								if (candidate.enabled && candidate.isVisible && bounds.Contains(hit.point) && size < best && candidate.sharedMaterial != null && candidate.sharedMaterial.renderQueue < 2500) {
									best = size;
									hitRenderer = candidate;
								}
							}
						}
						j.BeginObject().Add("viewport", point.ToString("F2")).Add("collider", hit.collider.name).Add("distance", hit.distance).Add("renderer", hitRenderer != null ? hitRenderer.name : null);
						if (hitRenderer != null) {
							int index = 0;
							foreach (Material m in hitRenderer.sharedMaterials) {
								if (m == null || m.shader == null) {
									continue;
								}
								var parts = new List<string>();
								Shader s = m.shader;
								for (int pi = 0; pi < s.GetPropertyCount(); ++pi) {
									string name = s.GetPropertyName(pi);
									if (name.StartsWith("_Stencil") || name.StartsWith("_ZTest") || name.StartsWith("unity_") || name.StartsWith("_Cull") || name.Contains("Blend") || name.StartsWith("_Transparent") || name.StartsWith("_DoubleSided") || name.Contains("MipInfo")) {
										continue;
									}
									switch (s.GetPropertyType(pi)) {
									case UnityEngine.Rendering.ShaderPropertyType.Color: parts.Add(name + "=" + m.GetColor(name).ToString("F3")); break;
									case UnityEngine.Rendering.ShaderPropertyType.Vector: parts.Add(name + "=" + m.GetVector(name).ToString("F2")); break;
									case UnityEngine.Rendering.ShaderPropertyType.Texture: { Texture tex = m.GetTexture(name); if (tex != null) { parts.Add(name + "=" + tex.name + " " + tex.width + " " + tex.graphicsFormat + " st " + m.GetTextureScale(name).ToString("F2")); } break; }
									default: { float v = m.GetFloat(name); if (v != 0) { parts.Add(name + "=" + v.ToString("0.###")); } break; }
									}
								}
								j.Add("material" + index++, m.name + " [" + s.name + "] kw " + string.Join(" ", m.shaderKeywords) + " | " + string.Join("; ", parts));
							}
						}
						j.EndObject();
					}
				}
			} catch (Exception) {
			}
			j.EndArray();

			// ---- transparent materials one by one (they are few and each is a special case)
			j.BeginArray("transparentMaterials");
			{
				var seenMaterials = new HashSet<int>();
				var mats = new List<Material>();
				foreach (Renderer r in UnityEngine.Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None)) {
					r.GetSharedMaterials(mats);
					foreach (Material m in mats) {
						if (m == null || m.shader == null || m.renderQueue < 2500 || !seenMaterials.Add(m.GetInstanceID())) {
							continue;
						}
						j.BeginObject().Add("material", m.name).Add("shader", m.shader.name).Add("queue", m.renderQueue).Add("renderer", r.name).Add("rendererEnabled", r.enabled && r.gameObject.activeInHierarchy)
							.Add("bounds", r.bounds.center.ToString("F1") + " +- " + r.bounds.extents.ToString("F1")).Add("keywords", string.Join(" ", m.shaderKeywords));
						foreach (string name in new[] { "_BaseColor", "_Color", "_TintColor", "_EmissiveColor", "_UnlitColor" }) {
							if (m.HasProperty(name)) {
								j.Add(name, m.GetColor(name).ToString("F3"));
							}
						}
						foreach (string name in new[] { "_SurfaceType", "_BlendMode", "_SrcBlend", "_DstBlend", "_AlphaCutoffEnable", "_AlphaCutoff", "_Smoothness", "_Metallic", "_ZWrite", "_EnableBlendModePreserveSpecularLighting", "_TransparentSortPriority", "_SpecularOcclusionMode", "_EnergyConservingSpecularColor", "_ReceivesSSRTransparent" }) {
							if (m.HasProperty(name)) {
								j.Add(name, m.GetFloat(name));
							}
						}
						foreach (string name in m.GetTexturePropertyNames()) {
							Texture tex = m.GetTexture(name);
							if (tex != null && !name.StartsWith("unity_")) {
								j.Add(name, tex.name + " " + tex.width + "x" + tex.height + " " + tex.graphicsFormat);
							}
						}
						j.EndObject();
					}
				}
			}
			j.EndArray();

			// ---- per renderer overrides (MaterialPropertyBlock): which of the shader's properties a renderer replaces
			j.BeginArray("propertyBlocks");
			{
				var block = new MaterialPropertyBlock();
				var seenBlocks = new Dictionary<string, int>();
				var mats = new List<Material>();
				foreach (Renderer r in UnityEngine.Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None)) {
					r.GetSharedMaterials(mats);
					for (int mi = 0; mi < mats.Count; ++mi) {
						Material m = mats[mi];
						if (m == null || m.shader == null) {
							continue;
						}
						// the slot's own block wins over the whole renderer's (HasPropertyBlock does not tell about the former)
						r.GetPropertyBlock(block, mi);
						bool perMaterial = !block.isEmpty;
						if (block.isEmpty) {
							r.GetPropertyBlock(block);
						}
						if (block.isEmpty) {
							continue;
						}
						var parts = new List<string>();
						Shader s = m.shader;
						for (int pi = 0; pi < s.GetPropertyCount(); ++pi) {
							int id = s.GetPropertyNameId(pi);
							string name = s.GetPropertyName(pi);
							if (block.HasTexture(id)) {
								Texture tex = block.GetTexture(id);
								parts.Add(name + "=tex:" + (tex != null ? tex.name + " " + tex.GetType().Name + " " + tex.width + "x" + tex.height : "null"));
							} else if (block.HasColor(id) || block.HasVector(id)) {
								parts.Add(name + "=" + block.GetVector(id).ToString("F3"));
							} else if (block.HasFloat(id)) {
								parts.Add(name + "=" + block.GetFloat(id).ToString("F3"));
							} else if (block.HasProperty(id)) {
								parts.Add(name + "=(other)");
							}
						}
						string key = s.name + " | " + m.name + " | " + string.Join("; ", parts);
						if (seenBlocks.TryGetValue(key, out int n)) {
							seenBlocks[key] = n + 1;
							continue;
						}
						seenBlocks[key] = 1;
						if (seenBlocks.Count <= 80) {
							j.BeginObject().Add("shader", s.name).Add("material", m.name).Add("renderer", r.name).Add("perMaterialIndex", perMaterial).Add("block", string.Join("; ", parts)).EndObject();
						}
					}
				}
			}
			j.EndArray();

			// ---- skinned renderers: how each mesh lays its vertices out, and whether Unity skins it on the GPU
			j.BeginArray("skinned");
			foreach (SkinnedMeshRenderer r in UnityEngine.Object.FindObjectsByType<SkinnedMeshRenderer>(FindObjectsSortMode.None)) {
				Mesh mesh = r.sharedMesh;
				j.BeginObject().Add("renderer", r.name).Add("mesh", mesh != null ? mesh.name : null).Add("bones", r.bones != null ? r.bones.Length : 0)
					.Add("visible", r.isVisible).Add("updateWhenOffscreen", r.updateWhenOffscreen);
				{
					// which space Unity's skinned vertices are in: where each candidate matrix puts the centre of the local
					// bounds, next to where Unity itself says the renderer is (bounds)
					Transform root = r.rootBone != null ? r.rootBone : r.transform;
					Vector3 c = r.localBounds.center;
					j.Add("cloth", r.GetComponent<Cloth>() != null).Add("rootBone", r.rootBone != null ? r.rootBone.name : null)
						.Add("rootScale", root.lossyScale.ToString("F3")).Add("ownScale", r.transform.lossyScale.ToString("F3"))
						.Add("boundsCenter", r.bounds.center.ToString("F2")).Add("localBoundsCenter", c.ToString("F2"))
						.Add("byRendererMatrix", r.localToWorldMatrix.MultiplyPoint3x4(c).ToString("F2"))
						.Add("byOwnTransform", r.transform.localToWorldMatrix.MultiplyPoint3x4(c).ToString("F2"))
						.Add("byRoot", root.localToWorldMatrix.MultiplyPoint3x4(c).ToString("F2"))
						.Add("byRootNoScale", Matrix4x4.TRS(root.position, root.rotation, Vector3.one).MultiplyPoint3x4(c).ToString("F2"))
						.Add("aetherMatrix", SceneCapture.SkinnedMatrix(r).MultiplyPoint3x4(c).ToString("F2"));
				}
				if (mesh != null) {
					var parts = new List<string>();
					var skinnedAttributes = new List<VertexAttributeDescriptor>();
					mesh.GetVertexAttributes(skinnedAttributes);
					foreach (VertexAttributeDescriptor a in skinnedAttributes) {
						parts.Add(a.attribute + ":s" + a.stream + ":" + a.format + "x" + a.dimension);
					}
					var strides = new List<string>();
					for (int s = 0; s < mesh.vertexBufferCount; ++s) {
						strides.Add(mesh.GetVertexBufferStride(s).ToString());
					}
					j.Add("vertices", mesh.vertexCount).Add("streams", mesh.vertexBufferCount).Add("strides", string.Join(",", strides)).Add("layout", string.Join(" ", parts))
						.Add("blendShapes", mesh.blendShapeCount).Add("indexFormat", mesh.indexFormat.ToString());
					try {
						GraphicsBuffer vb = r.GetVertexBuffer();
						j.Add("skinnedBuffer", vb != null ? "stride " + vb.stride + ", count " + vb.count + ", target " + vb.target : "none");
						if (vb != null) {
							vb.Dispose();
						}
					} catch (Exception e) {
						j.Add("skinnedBuffer", "error: " + e.GetType().Name);
					}
				}
				j.EndObject();
			}
			j.EndArray();

			// ---- packed material textures: what each channel really holds (mean / min / max of a 32 x 32 downsample,
			// read back from the GPU; linear values 0..255). The channel conventions are the game's own.
			j.BeginArray("packedTextures");
			{
				string[] names = { "_PackMap", "_MaskMap", "_TirePackMap", "_ColormaskMap", "_EmissionMasks", "_MaterialProperties", "_StitchesPack", "_Spectrum", "Texture2D_67B10B5E", "_TextureMaterial", "_BaseColorMap" };
				var seen = new HashSet<int>();
				int done = 0;
				foreach (Renderer r in UnityEngine.Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None)) {
					foreach (Material m in r.sharedMaterials) {
						if (m == null || done >= 60) {
							continue;
						}
						foreach (string name in names) {
							if (!m.HasProperty(name) || (name == "_BaseColorMap" && (m.shader == null || m.shader.name.IndexOf("Blob", StringComparison.OrdinalIgnoreCase) < 0))) {
								continue;
							}
							Texture tex = m.GetTexture(name);
							if (tex == null || !seen.Add(tex.GetInstanceID()) || done >= 60) {
								continue;
							}
							++done;
							j.BeginObject().Add("property", name).Add("shader", m.shader != null ? m.shader.name : null).Add("material", m.name).Add("texture", tex.name)
								.Add("format", tex.graphicsFormat.ToString()).Add("size", tex.width + "x" + tex.height);
							RenderTexture rt = null;
							Texture2D read = null;
							RenderTexture previous = RenderTexture.active;
							try {
								rt = RenderTexture.GetTemporary(32, 32, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
								Graphics.Blit(tex, rt);
								RenderTexture.active = rt;
								read = new Texture2D(32, 32, TextureFormat.RGBA32, false, true);
								read.ReadPixels(new Rect(0, 0, 32, 32), 0, 0);
								Color32[] pixels = read.GetPixels32();
								long[] sum = new long[4];
								int[] min = { 255, 255, 255, 255 }, max = { 0, 0, 0, 0 };
								foreach (Color32 c in pixels) {
									int[] v = { c.r, c.g, c.b, c.a };
									for (int k = 0; k < 4; ++k) {
										sum[k] += v[k];
										min[k] = Math.Min(min[k], v[k]);
										max[k] = Math.Max(max[k], v[k]);
									}
								}
								Color32 corner = pixels[0], centre = pixels[16 * 32 + 16];
								j.Add("corner", corner.r + "," + corner.g + "," + corner.b + "," + corner.a).Add("centre", centre.r + "," + centre.g + "," + centre.b + "," + centre.a);
								string[] channel = { "r", "g", "b", "a" };
								for (int k = 0; k < 4; ++k) {
									j.Add(channel[k], (sum[k] / pixels.Length) + " (" + min[k] + ".." + max[k] + ")");
								}
							} catch (Exception e) {
								j.Add("error", e.GetType().Name + ": " + e.Message);
							} finally {
								RenderTexture.active = previous;
								if (rt != null) {
									RenderTexture.ReleaseTemporary(rt);
								}
								if (read != null) {
									UnityEngine.Object.Destroy(read);
								}
							}
							j.EndObject();
						}
					}
				}
			}
			j.EndArray();

			// ---- how the renderers take part in lighting: rendering layers (HDRP light layers) and shadow casting
			{
				var layerCounts = new Dictionary<uint, int>();
				var castCounts = new Dictionary<string, int>();
				foreach (MeshRenderer r in UnityEngine.Object.FindObjectsByType<MeshRenderer>(FindObjectsSortMode.None)) {
					layerCounts.TryGetValue(r.renderingLayerMask, out int c);
					layerCounts[r.renderingLayerMask] = c + 1;
					string mode = r.shadowCastingMode + (r.receiveShadows ? "" : " (no receive)");
					castCounts.TryGetValue(mode, out int k);
					castCounts[mode] = k + 1;
				}
				j.BeginObject("rendererLighting");
				j.BeginObject("renderingLayerMasks");
				foreach (var kv in layerCounts) {
					j.Add("0x" + kv.Key.ToString("X"), kv.Value);
				}
				j.EndObject();
				j.BeginObject("shadowCasting");
				foreach (var kv in castCounts) {
					j.Add(kv.Key, kv.Value);
				}
				j.EndObject();
				j.EndObject();
			}
			// ---- lights in HDRP's units
			j.BeginArray("lights");
			foreach (Light l in UnityEngine.Object.FindObjectsByType<Light>(FindObjectsSortMode.None)) {
				j.BeginObject().Add("name", l.name).Add("type", l.type.ToString()).Add("intensity", l.intensity).Add("range", l.range)
					.Add("spotAngle", l.spotAngle).Add("innerSpotAngle", l.innerSpotAngle).Add("colorTemperature", l.colorTemperature).Add("useColorTemperature", l.useColorTemperature)
					.Add("color", l.color.r.ToString("0.###") + "," + l.color.g.ToString("0.###") + "," + l.color.b.ToString("0.###")).Add("shadows", l.shadows.ToString())
					.Add("enabled", l.isActiveAndEnabled).Add("cullingMask", l.cullingMask).Add("renderingLayerMask", l.renderingLayerMask).Add("shadowStrength", l.shadowStrength)
					.Add("forward", l.transform.forward.x.ToString("0.###") + "," + l.transform.forward.y.ToString("0.###") + "," + l.transform.forward.z.ToString("0.###"));
				if (l.TryGetComponent(out HDAdditionalLightData hd)) {
					j.Add("hdType", hd.type.ToString()).Add("hdIntensity", hd.intensity).Add("hdUnit", hd.lightUnit.ToString()).Add("hdShadowResolution", hd.shadowResolution.level)
						.Add("hdAffectsVolumetric", hd.affectsVolumetric).Add("hdAngularDiameter", hd.angularDiameter)
						.Add("hdLightDimmer", hd.lightDimmer).Add("hdShadowDimmer", hd.shadowDimmer).Add("hdAffectDiffuse", hd.affectDiffuse).Add("hdAffectSpecular", hd.affectSpecular)
						.Add("hdLightLayers", (int)hd.lightlayersMask).Add("hdInteractsWithSky", hd.interactsWithSky);
					j.Add("position", l.transform.position.ToString("F2")).Add("hdFadeDistance", hd.fadeDistance).Add("hdSpotLightShape", hd.spotLightShape.ToString()).Add("hdReflector", hd.enableSpotReflector)
						.Add("hdRangeAttenuation", hd.applyRangeAttenuation).Add("hdShapeRadius", hd.shapeRadius).Add("hdVolumetricDimmer", hd.volumetricDimmer).Add("hdEnabled", hd.enabled).Add("bakeType", l.bakingOutput.lightmapBakeType.ToString()).Add("isBaked", l.bakingOutput.isBaked).Add("mixedMode", l.bakingOutput.mixedLightingMode.ToString());
				}
				j.EndObject();
			}
			j.EndArray();

			// ---- environment: what is reachable without HDRP's internals
			j.BeginObject("environment");
			SphericalHarmonicsL2 sh = RenderSettings.ambientProbe;
			j.Add("ambientProbeL0", sh[0, 0].ToString("0.###") + "," + sh[1, 0].ToString("0.###") + "," + sh[2, 0].ToString("0.###"));
			j.Add("ambientIntensity", RenderSettings.ambientIntensity);
			j.Add("reflectionIntensity", RenderSettings.reflectionIntensity);
			j.Add("sun", RenderSettings.sun != null ? RenderSettings.sun.name : null);
			foreach (string name in new[] { "_SkyTexture", "_ExposureTexture", "_PrevExposureTexture", "_AirSingleScatteringTexture", "_VBufferLighting", "_CameraDepthTexture", "_ColorPyramidTexture", "_ShadowmapAtlas", "_ShadowmapCascadeAtlas", "_EnvCubemapTextures", "_AmbientProbeData" }) {
				Texture t = Shader.GetGlobalTexture(name);
				j.Add("global" + name, t != null ? t.name + " [" + t.GetType().Name + " " + t.dimension + " " + t.width + "x" + t.height + " " + t.graphicsFormat + "]" : null);
			}
			// (while the game's own pipeline runs: the exposure it uses right now, read back from its 1 x 1 texture)
			try {
				Texture exposureTexture = Shader.GetGlobalTexture("_ExposureTexture");
				if (exposureTexture != null) {
					RenderTexture previous = RenderTexture.active;
					RenderTexture rt = RenderTexture.GetTemporary(1, 1, 0, RenderTextureFormat.ARGBFloat, RenderTextureReadWrite.Linear);
					Graphics.Blit(exposureTexture, rt);
					RenderTexture.active = rt;
					var read = new Texture2D(1, 1, TextureFormat.RGBAFloat, false, true);
					read.ReadPixels(new Rect(0, 0, 1, 1), 0, 0);
					Color value = read.GetPixel(0, 0);
					RenderTexture.active = previous;
					RenderTexture.ReleaseTemporary(rt);
					UnityEngine.Object.Destroy(read);
					j.Add("hostExposureMultiplier", value.r).Add("hostExposureEV100", value.g);
				}
			} catch (Exception) {
			}
			// the volume stack of the game's screen camera (HDRP keeps one per camera); the manager's global stack is only
			// the fallback: it holds whatever camera was rendered last
			VolumeStack stack = null;
			string stackSource = "none";
			try {
				Camera screen = null;
				foreach (Camera c in Camera.allCameras) {
					if (c.cameraType == CameraType.Game && c.targetTexture == null && c.isActiveAndEnabled && (screen == null || c.depth > screen.depth)) {
						screen = c;
					}
				}
				if (screen != null && VolumeManager.instance.isInitialized) {
					// evaluated here for the camera, the way Aether's capture does it (no renderer of the game's is asked)
					stack = VolumeManager.instance.CreateStack();
					LayerMask layers = ~0;
					Transform anchor = screen.transform;
					if (screen.TryGetComponent(out HDAdditionalCameraData data)) {
						layers = data.volumeLayerMask;
						anchor = data.volumeAnchorOverride != null ? data.volumeAnchorOverride : anchor;
					}
					VolumeManager.instance.Update(stack, anchor, layers);
					stackSource = "volume manager, evaluated for " + screen.name + " (layers " + layers.value + ", anchor " + anchor.name + ")";
				}
			} catch (Exception) {
			}
			if (stack == null && VolumeManager.instance != null) {
				stack = VolumeManager.instance.stack;
				stackSource = "VolumeManager.instance.stack";
			}
			// the volumes themselves: where the game keeps its look
			j.BeginArray("volumes");
			foreach (Volume v in UnityEngine.Object.FindObjectsByType<Volume>(FindObjectsInactive.Include, FindObjectsSortMode.None)) {
				VolumeProfile profile = v.sharedProfile;
				var parts = new List<string>();
				if (profile != null) {
					foreach (VolumeComponent c in profile.components) {
						if (c != null) {
							parts.Add(c.GetType().Name + (c.active ? "" : " (off)"));
						}
					}
				}
				j.BeginObject().Add("name", v.name).Add("active", v.isActiveAndEnabled).Add("global", v.isGlobal).Add("priority", v.priority).Add("weight", v.weight)
					.Add("layer", v.gameObject.layer).Add("hideFlags", v.gameObject.hideFlags.ToString()).Add("profile", profile != null ? profile.name : null).Add("components", string.Join(", ", parts)).EndObject();
			}
			j.EndArray();
			j.Add("volumeStackSource", stackSource);
			// every volume component of that stack whose values differ from the component's defaults: what the game
			// really asks its renderer for (grading, ambient occlusion, reflections, shadows, fog, sky ...)
			if (stack != null && VolumeManager.instance != null) {
				j.BeginObject("volumeOverrides");
				foreach (Type type in VolumeManager.instance.baseComponentTypeArray) {
					VolumeComponent live = null, defaults = null;
					try {
						live = stack.GetComponent(type);
						defaults = (VolumeComponent)ScriptableObject.CreateInstance(type);
					} catch (Exception) {
					}
					if (live == null || defaults == null) {
						continue;
					}
					var changed = new List<string>();
					foreach (FieldInfo field in type.GetFields(BindingFlags.Public | BindingFlags.Instance)) {
						if (!typeof(VolumeParameter).IsAssignableFrom(field.FieldType)) {
							continue;
						}
						try {
							PropertyInfo value = field.FieldType.GetProperty("value");
							if (value == null) {
								continue;
							}
							object a = value.GetValue(field.GetValue(live)), b = value.GetValue(field.GetValue(defaults));
							string sa = a is UnityEngine.Object oa ? (oa != null ? oa.name : "null") : Convert.ToString(a, System.Globalization.CultureInfo.InvariantCulture);
							string sb = b is UnityEngine.Object ob ? (ob != null ? ob.name : "null") : Convert.ToString(b, System.Globalization.CultureInfo.InvariantCulture);
							if (sa != sb) {
								changed.Add(field.Name + "=" + sa);
							}
						} catch (Exception) {
						}
					}
					UnityEngine.Object.Destroy(defaults);
					if (changed.Count > 0) {
						j.Add(type.Name, string.Join("; ", changed));
					}
				}
				j.EndObject();
			}
			if (stack != null) {
				var sky = stack.GetComponent<VisualEnvironment>();
				var exposure = stack.GetComponent<Exposure>();
				var fog = stack.GetComponent<Fog>();
				var tonemap = stack.GetComponent<Tonemapping>();
				if (sky != null) {
					j.Add("skyType", sky.skyType.value).Add("cloudType", sky.cloudType.value).Add("skyAmbientMode", sky.skyAmbientMode.value.ToString());
				}
				if (exposure != null) {
					j.Add("exposureMode", exposure.mode.value.ToString()).Add("fixedExposure", exposure.fixedExposure.value).Add("exposureCompensation", exposure.compensation.value)
						.Add("exposureLimitMin", exposure.limitMin.value).Add("exposureLimitMax", exposure.limitMax.value);
				}
				if (fog != null) {
					j.Add("fogEnabled", fog.enabled.value).Add("fogMeanFreePath", fog.meanFreePath.value).Add("fogBaseHeight", fog.baseHeight.value).Add("fogMaxHeight", fog.maximumHeight.value)
						.Add("fogVolumetric", fog.enableVolumetricFog.value);
				}
				if (tonemap != null) {
					j.Add("tonemapMode", tonemap.mode.value.ToString());
				}
				// what else shapes the picture's brightness and contrast
				var indirect = stack.GetComponent<IndirectLightingController>();
				if (indirect != null) {
					j.Add("indirectDiffuseMultiplier", indirect.indirectDiffuseLightingMultiplier.value).Add("reflectionMultiplier", indirect.reflectionLightingMultiplier.value);
				}
				var ssao = stack.GetComponent<ScreenSpaceAmbientOcclusion>();
				if (ssao != null) {
					j.Add("ssaoIntensity", ssao.intensity.value).Add("ssaoRadius", ssao.radius.value).Add("ssaoDirectStrength", ssao.directLightingStrength.value);
				}
				var grade = stack.GetComponent<ColorAdjustments>();
				if (grade != null) {
					Color f = grade.colorFilter.value;
					j.Add("postExposure", grade.postExposure.value).Add("contrast", grade.contrast.value).Add("saturation", grade.saturation.value)
						.Add("colorFilter", f.r.ToString("0.###") + "," + f.g.ToString("0.###") + "," + f.b.ToString("0.###"));
				}
				var shadows = stack.GetComponent<HDShadowSettings>();
				if (shadows != null) {
					j.Add("shadowMaxDistance", shadows.maxShadowDistance.value).Add("shadowCascades", shadows.cascadeShadowSplitCount.value);
				}
				var bloom = stack.GetComponent<Bloom>();
				if (bloom != null) {
					j.Add("bloomIntensity", bloom.intensity.value).Add("bloomThreshold", bloom.threshold.value);
				}
				var hdri = stack.GetComponent<HDRISky>();
				if (hdri != null && hdri.hdriSky.value != null) {
					Texture t = hdri.hdriSky.value;
					j.Add("hdriSky", t.name + " [" + t.dimension + " " + t.width + "x" + t.height + " " + t.graphicsFormat + "]").Add("hdriExposure", hdri.exposure.value).Add("hdriMultiplier", hdri.multiplier.value)
						.Add("hdriRotation", hdri.rotation.value).Add("hdriDesiredLux", hdri.desiredLuxValue.value).Add("hdriIntensityMode", hdri.skyIntensityMode.value.ToString());
				}
			}
			j.EndObject();

			Camera cam = Camera.main;
			if (cam != null) {
				j.BeginObject("camera");
				j.Add("name", cam.name).Add("fov", cam.fieldOfView).Add("near", cam.nearClipPlane).Add("far", cam.farClipPlane).Add("aspect", cam.aspect);
				j.Add("worldToCamera", cam.worldToCameraMatrix.ToString("0.####").Replace("\n", " | "));
				j.Add("projection", cam.projectionMatrix.ToString("0.####").Replace("\n", " | "));
				j.Add("gpuProjection", GL.GetGPUProjectionMatrix(cam.projectionMatrix, false).ToString("0.####").Replace("\n", " | "));
				j.Add("gpuProjectionRT", GL.GetGPUProjectionMatrix(cam.projectionMatrix, true).ToString("0.####").Replace("\n", " | "));
				j.Add("position", cam.transform.position.ToString("0.###")).Add("forward", cam.transform.forward.ToString("0.###"));
				j.EndObject();
			}
			return j.EndObject().ToString();
		}
	}
}
