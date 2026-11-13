using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;
using UnityEngine.Profiling;
using UnityEngine.Rendering;
using UnityEngine.Rendering.HighDefinition;
using UnityEngine.SceneManagement;

namespace Aether {
	/// <summary>
	/// What the game looks like from Unity's side, published to the native runtime as JSON on request (the overlay's
	/// Scene tab, the MCP tools get_scene_info / get_material_info / get_resource_list). Read only: nothing in the scene
	/// is changed. These walks are not free (materials visits every renderer), so they only run when asked for.
	/// This is also the survey Phase 2's scene capture is built from.
	/// </summary>
	internal sealed class SceneReporter {
		private bool sceneDirty_ = true;
		private float sceneDirtyAt_;

		public void SceneChanged() {
			sceneDirty_ = true;
			sceneDirtyAt_ = Time.unscaledTime + 1.0f; // let the scene's objects wake up first
		}

		public bool TakeSceneDirty() {
			if (sceneDirty_ && Time.unscaledTime >= sceneDirtyAt_) {
				sceneDirty_ = false;
				return true;
			}
			return false;
		}

		public void Publish(uint requests) {
			if ((requests & Native.RequestScene) != 0) {
				Guarded("scene", Scene);
			}
			if ((requests & Native.RequestMaterials) != 0) {
				Guarded("materials", Materials);
			}
			if ((requests & Native.RequestSurvey) != 0) {
				Guarded("survey", RenderSurvey.Build);
			}
			if ((requests & Native.RequestRenderTextures) != 0) {
				Guarded("renderTextures", RenderTextures);
			}
		}

		private static void Guarded(string key, Func<string> build) {
			try {
				Native.SetManagedState(key, build());
			} catch (Exception e) {
				Native.Log(3, "Could not collect '" + key + "': " + e.GetType().Name + ": " + e.Message);
				Native.SetManagedState(key, new Json().BeginObject().Add("error", e.GetType().Name + ": " + e.Message).Add("stack", e.StackTrace).EndObject().ToString());
			}
		}

		// ---- the game, Unity, the render pipeline, the other mods

		public string GameInfo(string status, string sceneCapture) {
			var j = new Json().BeginObject();
			j.Add("aetherStatus", status);
			j.Add("sceneCapture", sceneCapture);
			j.Add("nativeLoad", Native.LoadStrategy);
			j.Add("hdrpDx12Fix", HdrpDx12Fix.Status);
			j.Add("gamePassesDx12", GameDx12Workarounds.Status);
			j.Add("loader", AetherCore.Host.LoaderName);
			j.Add("unityVersion", Application.unityVersion);
			j.Add("gameVersion", Application.version);
			j.Add("graphicsDeviceType", SystemInfo.graphicsDeviceType.ToString());
			j.Add("graphicsDeviceName", SystemInfo.graphicsDeviceName);
			j.Add("graphicsDeviceVersion", SystemInfo.graphicsDeviceVersion);
			j.Add("graphicsMemoryMB", SystemInfo.graphicsMemorySize);
			j.Add("renderingThreadingMode", SystemInfo.renderingThreadingMode.ToString());
			j.Add("graphicsShaderLevel", SystemInfo.graphicsShaderLevel);
			j.Add("screenWidth", Screen.width).Add("screenHeight", Screen.height);
			j.Add("fullScreenMode", Screen.fullScreenMode.ToString());
			j.Add("vSyncCount", QualitySettings.vSyncCount);
			j.Add("targetFrameRate", Application.targetFrameRate);
			j.Add("qualityLevel", QualitySettings.names.Length > 0 ? QualitySettings.names[QualitySettings.GetQualityLevel()] : "");
			j.Add("colorSpace", QualitySettings.activeColorSpace.ToString());
			j.Add("hdrDisplayActive", HDROutputSettings.main != null && HDROutputSettings.main.active);
			j.Add("streamingMipmapsActive", QualitySettings.streamingMipmapsActive);
			j.Add("streamingMipmapsMemoryBudgetMB", QualitySettings.streamingMipmapsMemoryBudget);

			RenderPipelineAsset asset = GraphicsSettings.currentRenderPipeline;
			j.BeginObject("renderPipeline");
			j.Add("asset", asset != null ? asset.name : null);
			j.Add("type", asset != null ? asset.GetType().FullName : "built-in");
			if (asset is HDRenderPipelineAsset hd) {
				HdrpSettings(j, hd);
			}
			j.EndObject();

			j.BeginArray("mods");
			try {
				foreach (object mod in AetherCore.Host.LoadedMods()) {
					j.BeginObject();
					foreach (string name in new[] { "Name", "Version", "Author", "Type" }) {
						PropertyInfo p = mod.GetType().GetProperty(name);
						if (p != null) {
							j.Add(char.ToLowerInvariant(name[0]) + name.Substring(1), Convert.ToString(p.GetValue(mod)));
						}
					}
					j.EndObject();
				}
			} catch (Exception) {
				// the list is a diagnosis aid only
			}
			j.EndArray();
			return j.EndObject().ToString();
		}

		private static void HdrpSettings(Json j, HDRenderPipelineAsset hd) {
			RenderPipelineSettings s = hd.currentPlatformRenderPipelineSettings;
			j.BeginObject("hdrp");
			j.Add("litShaderMode", s.supportedLitShaderMode.ToString());
			j.Add("colorBufferFormat", s.colorBufferFormat.ToString());
			j.Add("msaaSampleCount", s.msaaSampleCount.ToString());
			j.Add("supportSSR", s.supportSSR);
			j.Add("supportSSRTransparent", s.supportSSRTransparent);
			j.Add("supportSSAO", s.supportSSAO);
			j.Add("supportSSGI", s.supportSSGI);
			j.Add("supportVolumetrics", s.supportVolumetrics);
			j.Add("supportVolumetricClouds", s.supportVolumetricClouds);
			j.Add("supportRayTracing", s.supportRayTracing);
			j.Add("supportDecals", s.supportDecals);
			j.Add("supportDecalLayers", s.supportDecalLayers);
			j.Add("supportMotionVectors", s.supportMotionVectors);
			j.Add("supportTransparentBackface", s.supportTransparentBackface);
			j.Add("supportTransparentDepthPrepass", s.supportTransparentDepthPrepass);
			j.Add("supportSubsurfaceScattering", s.supportSubsurfaceScattering);
			j.Add("supportWater", s.supportWater);
			j.Add("supportDistortion", s.supportDistortion);
			j.Add("supportCustomPass", s.supportCustomPass);
			j.Add("dynamicResolutionEnabled", s.dynamicResolutionSettings.enabled);
			j.Add("dynamicResolutionUpsampleFilter", s.dynamicResolutionSettings.upsampleFilter.ToString());
			j.Add("maxDirectionalLightsOnScreen", s.lightLoopSettings.maxDirectionalLightsOnScreen);
			j.Add("maxPunctualLightsOnScreen", s.lightLoopSettings.maxPunctualLightsOnScreen);
			j.Add("maxAreaLightsOnScreen", s.lightLoopSettings.maxAreaLightsOnScreen);
			j.Add("maxDecalsOnScreen", s.lightLoopSettings.maxDecalsOnScreen);
			j.Add("reflectionCacheCompressed", s.lightLoopSettings.reflectionCacheCompressed);
			j.Add("cookieAtlasSize", s.lightLoopSettings.cookieAtlasSize.ToString());
			j.Add("shadowAtlasResolutionPunctual", s.hdShadowInitParams.punctualLightShadowAtlas.shadowAtlasResolution);
			j.Add("shadowAtlasResolutionArea", s.hdShadowInitParams.areaLightShadowAtlas.shadowAtlasResolution);
			j.Add("shadowAtlasResolutionCached", s.hdShadowInitParams.cachedPunctualLightShadowAtlas);
			j.Add("maxShadowRequests", s.hdShadowInitParams.maxShadowRequests);
			j.Add("directionalShadowsDepthBits", s.hdShadowInitParams.directionalShadowsDepthBits.ToString());
			// the switches HDRP's build strips compute shader variants by (unity/hdrp-game-settings.json mirrors them)
			j.Add("punctualShadowFilteringQuality", s.hdShadowInitParams.punctualShadowFilteringQuality.ToString());
			j.Add("directionalShadowFilteringQuality", s.hdShadowInitParams.directionalShadowFilteringQuality.ToString());
			j.Add("areaShadowFilteringQuality", s.hdShadowInitParams.areaShadowFilteringQuality.ToString());
			j.Add("supportScreenSpaceShadows", s.hdShadowInitParams.supportScreenSpaceShadows);
			j.Add("lightProbeSystem", s.lightProbeSystem.ToString());
			j.Add("probeVolumeSHBands", s.probeVolumeSHBands.ToString());
			j.Add("supportRuntimeAOVAPI", s.supportRuntimeAOVAPI);
			j.Add("supportHighQualityLineRendering", s.supportHighQualityLineRendering);
			j.Add("qualityLevelCount", QualitySettings.names.Length);
			j.Add("gradingLutSize", s.postProcessSettings.lutSize);
			j.Add("postProcessBufferFormat", s.postProcessSettings.bufferFormat.ToString());
			j.EndObject();
		}

		// ---- the scene

		private static string PathOf(Transform t) {
			string path = t.name;
			int depth = 0;
			while (t.parent != null && depth++ < 6) {
				t = t.parent;
				path = t.name + "/" + path;
			}
			return path;
		}

		private string Scene() {
			var j = new Json().BeginObject();
			Scene active = SceneManager.GetActiveScene();
			j.Add("activeScene", active.name);
			j.BeginArray("loadedScenes");
			for (int i = 0; i < SceneManager.sceneCount; ++i) {
				Scene s = SceneManager.GetSceneAt(i);
				j.BeginObject().Add("name", s.name).Add("path", s.path).Add("isLoaded", s.isLoaded).Add("rootCount", s.isLoaded ? s.rootCount : 0).EndObject();
			}
			j.EndArray();

			Camera[] cameras = Camera.allCameras;
			j.BeginArray("cameras");
			foreach (Camera c in cameras) {
				j.BeginObject();
				j.Add("path", PathOf(c.transform));
				j.Add("tag", c.tag);
				j.Add("depth", c.depth);
				j.Add("targetTexture", c.targetTexture != null ? c.targetTexture.name + " " + c.targetTexture.width + "x" + c.targetTexture.height : null);
				j.Add("pixelWidth", c.pixelWidth).Add("pixelHeight", c.pixelHeight);
				j.Add("fieldOfView", c.fieldOfView).Add("near", c.nearClipPlane).Add("far", c.farClipPlane);
				j.Add("orthographic", c.orthographic);
				j.Add("clearFlags", c.clearFlags.ToString());
				j.Add("cullingMask", c.cullingMask);
				j.Add("allowHDR", c.allowHDR);
				j.Add("cameraType", c.cameraType.ToString());
				if (c.TryGetComponent(out HDAdditionalCameraData hd)) {
					j.Add("hdAntialiasing", hd.antialiasing.ToString());
					j.Add("hdClearColorMode", hd.clearColorMode.ToString());
					j.Add("hdCustomFrameSettings", hd.customRenderingSettings);
					j.Add("hdAllowDynamicResolution", hd.allowDynamicResolution);
					j.Add("hdAllowDlss", hd.allowDeepLearningSuperSampling);
					j.Add("hdVolumeLayerMask", hd.volumeLayerMask.value);
				}
				j.EndObject();
			}
			j.EndArray();

			// lights: counts by type and the strongest few
			Light[] lights = UnityEngine.Object.FindObjectsByType<Light>(FindObjectsSortMode.None);
			var byType = new Dictionary<string, int>();
			int shadowed = 0;
			foreach (Light l in lights) {
				string type = l.type.ToString();
				byType.TryGetValue(type, out int n);
				byType[type] = n + 1;
				if (l.shadows != LightShadows.None) {
					++shadowed;
				}
			}
			j.BeginObject("lights");
			j.Add("active", lights.Length).Add("castingShadows", shadowed);
			j.BeginObject("byType");
			foreach (var kv in byType) {
				j.Add(kv.Key, kv.Value);
			}
			j.EndObject();
			Array.Sort(lights, (a, b) => b.intensity.CompareTo(a.intensity));
			j.BeginArray("strongest");
			for (int i = 0; i < lights.Length && i < 48; ++i) {
				Light l = lights[i];
				j.BeginObject().Add("path", PathOf(l.transform)).Add("type", l.type.ToString()).Add("intensity", l.intensity).Add("range", l.range)
					.Add("shadows", l.shadows.ToString()).Add("color", "#" + ColorUtility.ToHtmlStringRGB(l.color)).Add("enabled", l.isActiveAndEnabled)
					.Add("baked", l.bakingOutput.isBaked).Add("bakeType", l.bakingOutput.lightmapBakeType.ToString());
				// (HDRP's own component: whether it is enabled (HDRP draws the light either way), its dimmer and fade distance)
				if (l.TryGetComponent(out HDAdditionalLightData hdLight)) {
					j.Add("hdEnabled", hdLight.isActiveAndEnabled).Add("hdDimmer", hdLight.lightDimmer).Add("hdFadeDistance", hdLight.fadeDistance);
				}
				j.EndObject();
			}
			j.EndArray();
			j.EndObject();

			Renderer[] renderers = UnityEngine.Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None);
			var rendererTypes = new Dictionary<string, int>();
			int visible = 0, shadowCasters = 0, staticBatched = 0;
			foreach (Renderer r in renderers) {
				string type = r.GetType().Name;
				rendererTypes.TryGetValue(type, out int n);
				rendererTypes[type] = n + 1;
				if (r.isVisible) ++visible;
				if (r.shadowCastingMode != ShadowCastingMode.Off) ++shadowCasters;
				if (r.isPartOfStaticBatch) ++staticBatched;
			}
			j.BeginObject("renderers");
			j.Add("active", renderers.Length).Add("visible", visible).Add("shadowCasters", shadowCasters).Add("staticBatched", staticBatched);
			j.BeginObject("byType");
			foreach (var kv in rendererTypes) {
				j.Add(kv.Key, kv.Value);
			}
			j.EndObject();
			j.EndObject();

			ReflectionProbe[] probes = UnityEngine.Object.FindObjectsByType<ReflectionProbe>(FindObjectsSortMode.None);
			j.BeginArray("reflectionProbes");
			for (int i = 0; i < probes.Length && i < 32; ++i) {
				ReflectionProbe p = probes[i];
				j.BeginObject().Add("path", PathOf(p.transform)).Add("mode", p.mode.ToString()).Add("refreshMode", p.refreshMode.ToString()).Add("resolution", p.resolution).EndObject();
			}
			j.EndArray();
			j.Add("reflectionProbeCount", probes.Length);

			// HDRP volumes and what they override: the list of HDRP features the game really uses (Phase 3)
			Volume[] volumes = UnityEngine.Object.FindObjectsByType<Volume>(FindObjectsSortMode.None);
			j.BeginArray("volumes");
			for (int i = 0; i < volumes.Length && i < 48; ++i) {
				Volume v = volumes[i];
				j.BeginObject().Add("path", PathOf(v.transform)).Add("isGlobal", v.isGlobal).Add("priority", v.priority).Add("weight", v.weight);
				j.BeginArray("components");
				VolumeProfile profile = v.sharedProfile;
				if (profile != null) {
					foreach (VolumeComponent c in profile.components) {
						if (c != null && c.active) {
							j.Add(null, c.GetType().Name);
						}
					}
				}
				j.EndArray().EndObject();
			}
			j.EndArray();
			j.Add("volumeCount", volumes.Length);

			Canvas[] canvases = UnityEngine.Object.FindObjectsByType<Canvas>(FindObjectsSortMode.None);
			j.BeginArray("canvases");
			for (int i = 0; i < canvases.Length && i < 32; ++i) {
				Canvas c = canvases[i];
				if (c.isRootCanvas) {
					j.BeginObject().Add("path", PathOf(c.transform)).Add("renderMode", c.renderMode.ToString()).Add("sortingOrder", c.sortingOrder).EndObject();
				}
			}
			j.EndArray();

			j.BeginObject("environment");
			j.Add("skybox", RenderSettings.skybox != null ? RenderSettings.skybox.shader.name : null);
			j.Add("fog", RenderSettings.fog);
			j.Add("ambientMode", RenderSettings.ambientMode.ToString());
			j.Add("sun", RenderSettings.sun != null ? PathOf(RenderSettings.sun.transform) : null);
			j.EndObject();
			return j.EndObject().ToString();
		}

		// ---- materials, grouped by shader

		private sealed class ShaderGroup {
			public string shader;
			public readonly HashSet<int> materials = new HashSet<int>();
			public int renderers;
			public int minQueue = int.MaxValue, maxQueue = int.MinValue;
			public readonly HashSet<string> keywords = new HashSet<string>();
			public readonly List<string> samples = new List<string>();
			public int transparent, alphaTest;
			public int textureProperties;
		}

		private string Materials() {
			Renderer[] renderers = UnityEngine.Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None);
			var groups = new Dictionary<string, ShaderGroup>();
			var shared = new List<Material>();
			var allMaterials = new HashSet<int>();
			foreach (Renderer r in renderers) {
				shared.Clear();
				r.GetSharedMaterials(shared);
				foreach (Material m in shared) {
					if (m == null || m.shader == null) {
						continue;
					}
					string name = m.shader.name;
					if (!groups.TryGetValue(name, out ShaderGroup g)) {
						g = new ShaderGroup { shader = name };
						g.textureProperties = CountTextureProperties(m.shader);
						groups[name] = g;
					}
					++g.renderers;
					if (!g.materials.Add(m.GetInstanceID())) {
						continue;
					}
					allMaterials.Add(m.GetInstanceID());
					g.minQueue = Math.Min(g.minQueue, m.renderQueue);
					g.maxQueue = Math.Max(g.maxQueue, m.renderQueue);
					if (m.renderQueue >= (int)RenderQueue.Transparent) {
						++g.transparent;
					} else if (m.renderQueue >= (int)RenderQueue.AlphaTest) {
						++g.alphaTest;
					}
					if (g.keywords.Count < 64) {
						foreach (string k in m.shaderKeywords) {
							g.keywords.Add(k);
						}
					}
					if (g.samples.Count < 5) {
						g.samples.Add(m.name);
					}
				}
			}
			var sorted = new List<ShaderGroup>(groups.Values);
			sorted.Sort((a, b) => b.renderers.CompareTo(a.renderers));

			var j = new Json().BeginObject();
			j.Add("scene", SceneManager.GetActiveScene().name);
			j.Add("rendererCount", renderers.Length);
			j.Add("materialCount", allMaterials.Count);
			j.Add("shaderCount", groups.Count);
			j.BeginArray("shaders");
			for (int i = 0; i < sorted.Count && i < 200; ++i) {
				ShaderGroup g = sorted[i];
				j.BeginObject();
				j.Add("shader", g.shader);
				j.Add("materials", g.materials.Count).Add("rendererSlots", g.renderers);
				j.Add("renderQueueMin", g.minQueue).Add("renderQueueMax", g.maxQueue);
				j.Add("transparentMaterials", g.transparent).Add("alphaTestMaterials", g.alphaTest);
				j.Add("textureProperties", g.textureProperties);
				j.BeginArray("keywords");
				foreach (string k in g.keywords) {
					j.Add(null, k);
				}
				j.EndArray();
				j.BeginArray("sampleMaterials");
				foreach (string s in g.samples) {
					j.Add(null, s);
				}
				j.EndArray();
				j.EndObject();
			}
			j.EndArray();
			return j.EndObject().ToString();
		}

		private static int CountTextureProperties(Shader shader) {
			int count = 0;
			int n = shader.GetPropertyCount();
			for (int i = 0; i < n; ++i) {
				if (shader.GetPropertyType(i) == ShaderPropertyType.Texture) {
					++count;
				}
			}
			return count;
		}

		// ---- Unity's render targets: the part of the VRAM that belongs to HDRP and the game

		private string RenderTextures() {
			RenderTexture[] all = Resources.FindObjectsOfTypeAll<RenderTexture>();
			var entries = new List<KeyValuePair<long, RenderTexture>>(all.Length);
			long total = 0;
			int created = 0;
			foreach (RenderTexture rt in all) {
				if (rt == null) {
					continue;
				}
				long bytes = rt.IsCreated() ? Profiler.GetRuntimeMemorySizeLong(rt) : 0;
				total += bytes;
				created += rt.IsCreated() ? 1 : 0;
				entries.Add(new KeyValuePair<long, RenderTexture>(bytes, rt));
			}
			entries.Sort((a, b) => b.Key.CompareTo(a.Key));
			var j = new Json().BeginObject();
			j.Add("count", all.Length).Add("created", created).Add("totalBytes", total);
			j.Add("note", "bytes are Unity's estimate (Profiler.GetRuntimeMemorySizeLong); the largest 100 are listed");
			j.BeginArray("renderTextures");
			for (int i = 0; i < entries.Count && i < 100; ++i) {
				RenderTexture rt = entries[i].Value;
				j.BeginObject();
				j.Add("name", rt.name);
				j.Add("bytes", entries[i].Key);
				j.Add("width", rt.width).Add("height", rt.height).Add("volumeDepth", rt.volumeDepth);
				j.Add("dimension", rt.dimension.ToString());
				j.Add("format", rt.graphicsFormat.ToString());
				j.Add("depthFormat", rt.depthStencilFormat.ToString());
				j.Add("msaa", rt.antiAliasing);
				j.Add("mipmaps", rt.useMipMap);
				j.Add("randomWrite", rt.enableRandomWrite);
				j.Add("created", rt.IsCreated());
				j.EndObject();
			}
			j.EndArray();
			return j.EndObject().ToString();
		}
	}
}
