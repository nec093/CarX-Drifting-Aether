using System;
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

			// ---- lights in HDRP's units
			j.BeginArray("lights");
			foreach (Light l in UnityEngine.Object.FindObjectsByType<Light>(FindObjectsSortMode.None)) {
				j.BeginObject().Add("name", l.name).Add("type", l.type.ToString()).Add("intensity", l.intensity).Add("range", l.range)
					.Add("spotAngle", l.spotAngle).Add("innerSpotAngle", l.innerSpotAngle).Add("colorTemperature", l.colorTemperature).Add("useColorTemperature", l.useColorTemperature)
					.Add("color", l.color.r.ToString("0.###") + "," + l.color.g.ToString("0.###") + "," + l.color.b.ToString("0.###")).Add("shadows", l.shadows.ToString());
				if (l.TryGetComponent(out HDAdditionalLightData hd)) {
					j.Add("hdType", hd.type.ToString()).Add("hdIntensity", hd.intensity).Add("hdUnit", hd.lightUnit.ToString()).Add("hdShadowResolution", hd.shadowResolution.level)
						.Add("hdAffectsVolumetric", hd.affectsVolumetric).Add("hdAngularDiameter", hd.angularDiameter);
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
			VolumeStack stack = VolumeManager.instance != null ? VolumeManager.instance.stack : null;
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
